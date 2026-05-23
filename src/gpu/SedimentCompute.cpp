#include "SedimentCompute.h"

#include "../D3D12Utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <d3dcompiler.h>
#include <dxgiformat.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace terrain::gpu
{
namespace
{
using terrain::d3d12::BufferResourceDesc;
using terrain::d3d12::CreateRootSignatureFromDesc;
using terrain::d3d12::DefaultShaderCompileFlags;
using terrain::d3d12::HeapProperties;
using terrain::d3d12::ShaderVisibleCbvSrvUavDescriptorHeapDesc;
using terrain::d3d12::ThrowIfFailed;

SedimentComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_setupPso;
ComPtr<ID3D12PipelineState> g_emitPso;
ComPtr<ID3D12PipelineState> g_sweep1Pso;
ComPtr<ID3D12PipelineState> g_sweep2Pso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Sediment GPU Compute not initialized";

// Mirrors the cbuffer in shaders/sediment_compute.hlsl. Re-bound for
// every dispatch while the passes share the same root signature.
struct SedimentShaderConstants
{
    UINT resolution;
    float talusH;
    float emissionPerIter;
    UINT convertTerrainToSediment;
};
static_assert(sizeof(SedimentShaderConstants) == 4 * sizeof(UINT), "SedimentShaderConstants must be 4 DWORDs");

struct SedimentGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct SedimentGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::SedimentSettings settings;
    std::promise<SedimentGpuRequestResult> promise;
};

std::vector<std::shared_ptr<SedimentGpuRequest>> g_pendingRequests;

bool EnsureSedimentComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature && g_setupPso && g_emitPso && g_sweep1Pso && g_sweep2Pso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "Sediment GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 4;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 4;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_context.gpu.device,
                                             rsDesc,
                                             g_rootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Sediment root sig failed";
        g_status = "Sediment GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();
    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Sediment shader failed";
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> setupBlob, emitBlob, sweep1Blob, sweep2Blob;
    if (!compileEntry("CSSetup", setupBlob)) { g_status = "Sediment Setup shader compile failed"; return false; }
    if (!compileEntry("CSEmit", emitBlob)) { g_status = "Sediment Emit shader compile failed"; return false; }
    if (!compileEntry("CSSlideSweep1", sweep1Blob)) { g_status = "Sediment Sweep1 shader compile failed"; return false; }
    if (!compileEntry("CSSlideSweep2", sweep2Blob)) { g_status = "Sediment Sweep2 shader compile failed"; return false; }

    auto buildPso = [&](ID3DBlob* csBlob, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_rootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        const HRESULT psoHr = g_context.gpu.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(psoHr))
        {
            if (error) *error = "Create Sediment PSO failed";
            return false;
        }
        return true;
    };
    if (!buildPso(setupBlob.Get(), g_setupPso)) { g_status = "Sediment Setup PSO failed"; return false; }
    if (!buildPso(emitBlob.Get(), g_emitPso)) { g_status = "Sediment Emit PSO failed"; return false; }
    if (!buildPso(sweep1Blob.Get(), g_sweep1Pso)) { g_status = "Sediment Sweep1 PSO failed"; return false; }
    if (!buildPso(sweep2Blob.Get(), g_sweep2Pso)) { g_status = "Sediment Sweep2 PSO failed"; return false; }

    g_ready = true;
    g_status = "Sediment GPU Compute dispatch ready";
    return true;
}

bool RunSedimentComputeImmediate(rock::HeightfieldGrid& grid, const rock::SedimentSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureSedimentComputePipeline(error))
    {
        return false;
    }
    if (!IsGpuComputeContextReady(g_context.gpu))
    {
        if (error) *error = "D3D12 queue or fence is not available";
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    if (resolution < 2 || grid.heights.size() < cellCount)
    {
        if (error) *error = "Invalid heightfield for Sediment GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize = cellCount * sizeof(float);
    const UINT64 outgoingByteSize = cellCount * 4ull * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc = BufferResourceDesc(fieldByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC outgoingGpuDesc = BufferResourceDesc(outgoingByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc = BufferResourceDesc(fieldByteSize);

    ComPtr<ID3D12Resource> bedrockBuf, sedimentBuf, outgoingBuf, inputHeightsBuf;
    ComPtr<ID3D12Resource> uploadHeights;
    ComPtr<ID3D12Resource> readbackSediment, readbackBedrock;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(bedrockBuf, fieldGpuDesc, "Sediment bedrock buffer")) return false;
    if (!createDefault(sedimentBuf, fieldGpuDesc, "Sediment sediment buffer")) return false;
    if (!createDefault(outgoingBuf, outgoingGpuDesc, "Sediment outgoing buffer")) return false;
    if (!createDefault(inputHeightsBuf, fieldGpuDesc, "Sediment input-heights buffer")) return false;
    if (!createUpload(uploadHeights, "Sediment upload heights")) return false;
    if (!createReadback(readbackSediment, "Sediment readback sediment")) return false;
    if (!createReadback(readbackBedrock, "Sediment readback bedrock")) return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map Sediment heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 4;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(kDescriptorCount);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Sediment descriptor heap failed"; return false; }

    const UINT descriptorSize = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto createUav = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        g_context.gpu.device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    createUav(bedrockBuf.Get(), static_cast<UINT>(cellCount), 0);
    createUav(sedimentBuf.Get(), static_cast<UINT>(cellCount), 1);
    createUav(outgoingBuf.Get(), static_cast<UINT>(cellCount * 4u), 2);
    createUav(inputHeightsBuf.Get(), static_cast<UINT>(cellCount), 3);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Sediment command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Sediment command list failed");

    D3D12_RESOURCE_BARRIER toCopyDest{};
    toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopyDest.Transition.pResource = inputHeightsBuf.Get();
    toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopyDest);
    commandList->CopyBufferRegion(inputHeightsBuf.Get(), 0, uploadHeights.Get(), 0, fieldByteSize);
    D3D12_RESOURCE_BARRIER toUav = toCopyDest;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &toUav);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    const float terrainSizeMeters = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSizeMeters = terrainSizeMeters / static_cast<float>(std::max<UINT>(1, resolution - 1));
    const float viscosity = std::clamp(settings.sedimentViscosity, 0.0f, 1.0f);
    const float talusAngleDeg = viscosity * viscosity * 80.0f;
    const float talusH = std::tan(talusAngleDeg * 3.14159265358979323846f / 180.0f) * cellSizeMeters;

    const float largestM = std::clamp(settings.largestDetailLevelM, cellSizeMeters, terrainSizeMeters * 0.5f);
    const int macroPasses = std::max(1, static_cast<int>(std::ceil(largestM / cellSizeMeters)));
    const int iterations = std::max(1, settings.iterations);
    const int stabIter = std::max(1, settings.stabilizationIterations);
    const float emissionAmount = std::max(0.0f, settings.emissionAmountM);
    const float emissionTime = std::clamp(settings.emissionTime, 0.0f, 1.0f);
    const int emissionEnd = std::max(1, static_cast<int>(std::ceil(static_cast<float>(iterations) * emissionTime)));
    const float emissionPerIter = emissionAmount / static_cast<float>(emissionEnd);
    const UINT groupCount = (resolution + 7u) / 8u;

    auto setConstants = [&](float talusHValue, float emitValue, UINT convertFlag) {
        SedimentShaderConstants constants{};
        constants.resolution = resolution;
        constants.talusH = talusHValue;
        constants.emissionPerIter = emitValue;
        constants.convertTerrainToSediment = convertFlag;
        commandList->SetComputeRoot32BitConstants(0, 4, &constants, 0);
    };
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &barrier);
    };

    setConstants(talusH, 0.0f, settings.convertTerrainToSediment ? 1u : 0u);
    commandList->SetPipelineState(g_setupPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    for (int iter = 0; iter < iterations; ++iter)
    {
        if (iter < emissionEnd && emissionPerIter > 0.0f)
        {
            setConstants(talusH, emissionPerIter, 0u);
            commandList->SetPipelineState(g_emitPso.Get());
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();
        }

        const int passes = macroPasses * stabIter;
        for (int p = 0; p < passes; ++p)
        {
            setConstants(talusH, 0.0f, 0u);
            commandList->SetPipelineState(g_sweep1Pso.Get());
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();
            commandList->SetPipelineState(g_sweep2Pso.Get());
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();
        }
    }

    D3D12_RESOURCE_BARRIER toCopySrc[2]{};
    ID3D12Resource* copyResources[2] = {sedimentBuf.Get(), bedrockBuf.Get()};
    for (int i = 0; i < 2; ++i)
    {
        toCopySrc[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySrc[i].Transition.pResource = copyResources[i];
        toCopySrc[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopySrc[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySrc[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(2, toCopySrc);
    commandList->CopyBufferRegion(readbackSediment.Get(), 0, sedimentBuf.Get(), 0, fieldByteSize);
    commandList->CopyBufferRegion(readbackBedrock.Get(), 0, bedrockBuf.Get(), 0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close Sediment command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal Sediment fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    void* mappedSed = nullptr;
    void* mappedRock = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackSediment->Map(0, &readRange, &mappedSed), "Map Sediment readback (sediment) failed");
    ThrowIfFailed(readbackBedrock->Map(0, &readRange, &mappedRock), "Map Sediment readback (bedrock) failed");
    const float* sedimentValues = static_cast<const float*>(mappedSed);
    const float* bedrockValues = static_cast<const float*>(mappedRock);

    grid.heights.assign(static_cast<size_t>(cellCount), 0.0f);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::vector<float> sortedSediment(sedimentValues, sedimentValues + cellCount);
    const size_t pIndex = std::min(static_cast<size_t>(cellCount - 1), (static_cast<size_t>(cellCount) * 95u) / 100u);
    std::nth_element(sortedSediment.begin(), sortedSediment.begin() + pIndex, sortedSediment.end());
    const float maskNorm = std::max(sortedSediment[pIndex], 1e-4f);
    const float halfBand = std::max((1.0f - std::clamp(settings.maskContrast, 0.0f, 1.0f)) * 0.5f, 0.005f);
    const float maskLo = 0.5f - halfBand;
    const float maskHi = 0.5f + halfBand;
    for (size_t i = 0; i < cellCount; ++i)
    {
        grid.heights[i] = bedrockValues[i] + sedimentValues[i];
        const float t = std::clamp((sedimentValues[i] / maskNorm - maskLo) / (maskHi - maskLo), 0.0f, 1.0f);
        grid.mask[i] = t * t * (3.0f - 2.0f * t);
    }
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackSediment->Unmap(0, &emptyWriteRange);
    readbackBedrock->Unmap(0, &emptyWriteRange);

    g_status = "Sediment GPU Compute evaluated";
    return true;
}

} // namespace

void SetSedimentComputeContext(SedimentComputeContext context)
{
    g_context = std::move(context);
}

void ResetSedimentComputeResources()
{
    g_setupPso.Reset();
    g_emitPso.Reset();
    g_sweep1Pso.Reset();
    g_sweep2Pso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& SedimentComputeStatus()
{
    return g_status;
}

bool RunSedimentCompute(rock::HeightfieldGrid& grid, const rock::SedimentSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunSedimentComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<SedimentGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<SedimentGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "Sediment GPU Compute queued on main thread";

    SedimentGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingSedimentGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<SedimentGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<SedimentGpuRequest>& request : requests)
    {
        SedimentGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunSedimentComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
