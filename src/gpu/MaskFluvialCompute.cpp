#include "MaskFluvialCompute.h"

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

MaskFluvialComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pitFillPso;
ComPtr<ID3D12PipelineState> g_commitHeightsPso;
ComPtr<ID3D12PipelineState> g_copyInputHeightsPso;
ComPtr<ID3D12PipelineState> g_blurHorizontalPso;
ComPtr<ID3D12PipelineState> g_blurVerticalPso;
ComPtr<ID3D12PipelineState> g_computeWeightsPso;
ComPtr<ID3D12PipelineState> g_accumInitPso;
ComPtr<ID3D12PipelineState> g_accumIterPso;
ComPtr<ID3D12PipelineState> g_maxReducePso;
ComPtr<ID3D12PipelineState> g_toMaskLogPso;
ComPtr<ID3D12PipelineState> g_toMaskLinearPso;
ComPtr<ID3D12PipelineState> g_toMaskThresholdPso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Mask Fluvial GPU Compute not initialized";

// Mirrors the cbuffer in shaders/mask_fluvial_compute.hlsl.
struct MaskFluvialShaderConstants
{
    UINT resolution;
    UINT algorithmIsMfd;
    float mfdExponent;
    UINT accumDirection;

    float thresholdCells;
    float gamma;
    float softness;
    float power;

    UINT outputCurve;
    float inertia;
    UINT detailBlurRadius;
    UINT pad0;
    UINT pad1;
    UINT pad2;
    UINT pad3;
    UINT pad4;
};
static_assert(sizeof(MaskFluvialShaderConstants) == 16 * sizeof(UINT), "MaskFluvialShaderConstants must be 16 DWORDs");

struct MaskFluvialGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct MaskFluvialGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::MaskFluvialSettings settings;
    std::promise<MaskFluvialGpuRequestResult> promise;
};

std::vector<std::shared_ptr<MaskFluvialGpuRequest>> g_pendingRequests;

bool EnsureMaskFluvialComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature
        && g_pitFillPso && g_commitHeightsPso && g_copyInputHeightsPso
        && g_blurHorizontalPso && g_blurVerticalPso
        && g_computeWeightsPso && g_accumInitPso && g_accumIterPso
        && g_maxReducePso && g_toMaskLogPso && g_toMaskLinearPso && g_toMaskThresholdPso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "Mask Fluvial GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 8;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 16;
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
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Mask Fluvial root sig failed";
        g_status = "Mask Fluvial GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();
    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Mask Fluvial shader failed";
            return false;
        }
        return true;
    };

    auto buildPso = [&](ID3DBlob* csBlob, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_rootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        const HRESULT psoHr = g_context.gpu.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(psoHr))
        {
            if (error) *error = "Create Mask Fluvial PSO failed";
            return false;
        }
        return true;
    };

    struct Entry { const char* name; ComPtr<ID3D12PipelineState>* pso; };
    Entry entries[] = {
        {"CSCopyInputHeights", &g_copyInputHeightsPso},
        {"CSBlurHorizontal", &g_blurHorizontalPso},
        {"CSBlurVertical", &g_blurVerticalPso},
        {"CSPitFillJacobi", &g_pitFillPso},
        {"CSCommitHeights", &g_commitHeightsPso},
        {"CSComputeWeights", &g_computeWeightsPso},
        {"CSAccumInit", &g_accumInitPso},
        {"CSAccumIter", &g_accumIterPso},
        {"CSMaxReduce", &g_maxReducePso},
        {"CSToMaskLog", &g_toMaskLogPso},
        {"CSToMaskLinear", &g_toMaskLinearPso},
        {"CSToMaskThreshold", &g_toMaskThresholdPso},
    };
    for (const Entry& e : entries)
    {
        ComPtr<ID3DBlob> blob;
        if (!compileEntry(e.name, blob))
        {
            g_status = std::string("Mask Fluvial ") + e.name + " compile failed";
            return false;
        }
        if (!buildPso(blob.Get(), *e.pso))
        {
            g_status = std::string("Mask Fluvial ") + e.name + " PSO failed";
            return false;
        }
    }

    g_ready = true;
    g_status = "Mask Fluvial GPU Compute dispatch ready";
    return true;
}

bool RunMaskFluvialComputeImmediate(rock::HeightfieldGrid& grid, const rock::MaskFluvialSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureMaskFluvialComputePipeline(error))
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
    if (resolution < 3 || grid.heights.size() < cellCount)
    {
        if (error) *error = "Invalid heightfield for Mask Fluvial GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize = cellCount * sizeof(float);
    const UINT64 weightsByteSize = cellCount * 8ull * sizeof(float);
    const UINT64 maxScratchBytes = sizeof(UINT);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc = BufferResourceDesc(fieldByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC weightsGpuDesc = BufferResourceDesc(weightsByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC scratchGpuDesc = BufferResourceDesc(maxScratchBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc = BufferResourceDesc(fieldByteSize);
    const D3D12_RESOURCE_DESC scratchCpuDesc = BufferResourceDesc(maxScratchBytes);

    ComPtr<ID3D12Resource> heightsBuf, heightsScratchBuf, weightsBuf, accumABuf, accumBBuf, outMaskBuf, maxScratchBuf, inputHeightsBuf;
    ComPtr<ID3D12Resource> uploadHeights, uploadMaxScratch, readbackMask;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(heightsBuf, fieldGpuDesc, "MF heights")) return false;
    if (!createDefault(heightsScratchBuf, fieldGpuDesc, "MF heights scratch")) return false;
    if (!createDefault(weightsBuf, weightsGpuDesc, "MF weights")) return false;
    if (!createDefault(accumABuf, fieldGpuDesc, "MF accum A")) return false;
    if (!createDefault(accumBBuf, fieldGpuDesc, "MF accum B")) return false;
    if (!createDefault(outMaskBuf, fieldGpuDesc, "MF out mask")) return false;
    if (!createDefault(maxScratchBuf, scratchGpuDesc, "MF max scratch")) return false;
    if (!createDefault(inputHeightsBuf, fieldGpuDesc, "MF input heights")) return false;
    if (!createUpload(uploadHeights, fieldCpuDesc, "MF upload heights")) return false;
    if (!createUpload(uploadMaxScratch, scratchCpuDesc, "MF upload max")) return false;
    if (!createReadback(readbackMask, fieldCpuDesc, "MF readback mask")) return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map MF heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    ThrowIfFailed(uploadMaxScratch->Map(0, &emptyReadRange, &mapped), "Map MF max scratch upload failed");
    *static_cast<UINT*>(mapped) = 0u;
    uploadMaxScratch->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 8;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(kDescriptorCount);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create MF descriptor heap failed"; return false; }

    const UINT descriptorSize = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto createUavFloat = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        g_context.gpu.device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    auto createUavUint = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(UINT);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        g_context.gpu.device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    createUavFloat(heightsBuf.Get(), static_cast<UINT>(cellCount), 0);
    createUavFloat(heightsScratchBuf.Get(), static_cast<UINT>(cellCount), 1);
    createUavFloat(weightsBuf.Get(), static_cast<UINT>(cellCount * 8u), 2);
    createUavFloat(accumABuf.Get(), static_cast<UINT>(cellCount), 3);
    createUavFloat(accumBBuf.Get(), static_cast<UINT>(cellCount), 4);
    createUavFloat(outMaskBuf.Get(), static_cast<UINT>(cellCount), 5);
    createUavUint(maxScratchBuf.Get(), 1u, 6);
    createUavFloat(inputHeightsBuf.Get(), static_cast<UINT>(cellCount), 7);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create MF command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create MF command list failed");

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = res;
        barrier.Transition.StateBefore = from;
        barrier.Transition.StateAfter = to;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &barrier);
    };
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &barrier);
    };

    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(inputHeightsBuf.Get(), 0, uploadHeights.Get(), 0, fieldByteSize);
    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    transition(maxScratchBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(maxScratchBuf.Get(), 0, uploadMaxScratch.Get(), 0, maxScratchBytes);
    transition(maxScratchBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    const UINT groupCount = (resolution + 7u) / 8u;
    const float thresholdCells = std::clamp(settings.accumulationThreshold, 0.0f, 1.0f) * static_cast<float>(cellCount);

    MaskFluvialShaderConstants constants{};
    constants.resolution = resolution;
    constants.algorithmIsMfd = 1u;
    constants.mfdExponent = std::clamp(settings.mfdExponent, 0.1f, 16.0f);
    constants.accumDirection = 0u;
    constants.thresholdCells = thresholdCells;
    constants.gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    constants.softness = std::clamp(settings.softness, 0.001f, 4.0f);
    constants.power = std::clamp(settings.power, 0.1f, 8.0f);
    constants.outputCurve = static_cast<UINT>(settings.outputCurve);
    constants.inertia = 0.0f;
    const float terrainSizeMeters = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSizeMeters = terrainSizeMeters / static_cast<float>(std::max(1u, resolution - 1u));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSizeMeters, terrainSizeMeters * 0.5f);
    constants.detailBlurRadius = static_cast<UINT>(std::clamp(static_cast<int>(std::round(largestDetailM / cellSizeMeters)), 1, 64));
    auto setConstants = [&]() {
        commandList->SetComputeRoot32BitConstants(0, 16, &constants, 0);
    };

    setConstants();
    commandList->SetPipelineState(g_copyInputHeightsPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    if (constants.detailBlurRadius > 1u)
    {
        commandList->SetPipelineState(g_blurHorizontalPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
        commandList->SetPipelineState(g_blurVerticalPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }

    const int pitIters = rock::MaskFluvialSettings{}.pitFillIterations;
    for (int i = 0; i < pitIters; ++i)
    {
        commandList->SetPipelineState(g_pitFillPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
        commandList->SetPipelineState(g_commitHeightsPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }

    commandList->SetPipelineState(g_computeWeightsPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    commandList->SetPipelineState(g_accumInitPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    const int accumIters = static_cast<int>(resolution) * 2;
    for (int i = 0; i < accumIters; ++i)
    {
        constants.accumDirection = static_cast<UINT>(i & 1);
        setConstants();
        commandList->SetPipelineState(g_accumIterPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }
    constants.accumDirection = 0u;
    setConstants();

    if (settings.outputCurve != rock::MaskFluvialOutputCurve::Threshold)
    {
        commandList->SetPipelineState(g_maxReducePso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }

    ID3D12PipelineState* maskPso = g_toMaskLogPso.Get();
    if (settings.outputCurve == rock::MaskFluvialOutputCurve::Threshold) maskPso = g_toMaskThresholdPso.Get();
    else if (settings.outputCurve == rock::MaskFluvialOutputCurve::Linear) maskPso = g_toMaskLinearPso.Get();
    commandList->SetPipelineState(maskPso);
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    transition(outMaskBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readbackMask.Get(), 0, outMaskBuf.Get(), 0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close MF command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal MF fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    void* mappedMask = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackMask->Map(0, &readRange, &mappedMask), "Map MF readback mask failed");
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.mask.data(), mappedMask, fieldByteSize);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackMask->Unmap(0, &emptyWriteRange);

    g_status = "Mask Fluvial GPU Compute evaluated";
    return true;
}

} // namespace

void SetMaskFluvialComputeContext(MaskFluvialComputeContext context)
{
    g_context = std::move(context);
}

void ResetMaskFluvialComputeResources()
{
    g_pitFillPso.Reset();
    g_commitHeightsPso.Reset();
    g_copyInputHeightsPso.Reset();
    g_blurHorizontalPso.Reset();
    g_blurVerticalPso.Reset();
    g_computeWeightsPso.Reset();
    g_accumInitPso.Reset();
    g_accumIterPso.Reset();
    g_maxReducePso.Reset();
    g_toMaskLogPso.Reset();
    g_toMaskLinearPso.Reset();
    g_toMaskThresholdPso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& MaskFluvialComputeStatus()
{
    return g_status;
}

bool RunMaskFluvialCompute(rock::HeightfieldGrid& grid, const rock::MaskFluvialSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunMaskFluvialComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<MaskFluvialGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<MaskFluvialGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "Mask Fluvial GPU Compute queued on main thread";

    MaskFluvialGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingMaskFluvialGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<MaskFluvialGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<MaskFluvialGpuRequest>& request : requests)
    {
        MaskFluvialGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunMaskFluvialComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
