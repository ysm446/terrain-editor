#include "MseCompute.h"

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

MseComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_streamPowerPso;
ComPtr<ID3D12PipelineState> g_thermalPso;
ComPtr<ID3D12PipelineState> g_depositionPso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "MSE GPU Compute not initialized";

struct MseShaderConstants
{
    UINT resolution;
    float terrainSizeMeters;
    float cellSizeMeters;
    float cellDiag;
    float refCellArea;
    float speStrength;
    float streamExponent;
    float slopeExponent;
    float maxStreamPower;
    float flowExponent;
    float speTimeStep;
    float thermalTanAngle;
    float thermalStrength;
    UINT thermalNoisifyAngle;
    float thermalNoiseMin;
    float thermalNoiseMax;
    float thermalNoiseWavelength;
    float depositionStrength;
    float rain;
    float pad0;
    float pad1;
};
static_assert(sizeof(MseShaderConstants) == 21 * sizeof(UINT), "MseShaderConstants must be 21 DWORDs");

struct MseGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct MseGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::MultiScaleErosionSettings settings;
    std::promise<MseGpuRequestResult> promise;
};

std::vector<std::shared_ptr<MseGpuRequest>> g_pendingRequests;

bool EnsureMseComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature && g_streamPowerPso && g_thermalPso && g_depositionPso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "MSE GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 6;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 21;
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
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create MSE root sig failed";
        g_status = "MSE GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();

    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile MSE shader failed";
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> spBlob;
    ComPtr<ID3DBlob> thBlob;
    ComPtr<ID3DBlob> depBlob;
    if (!compileEntry("CSStreamPower", spBlob)) { g_status = "MSE SPE shader compile failed"; return false; }
    if (!compileEntry("CSThermal", thBlob)) { g_status = "MSE thermal shader compile failed"; return false; }
    if (!compileEntry("CSDeposition", depBlob)) { g_status = "MSE deposition shader compile failed"; return false; }

    auto buildPso = [&](ID3DBlob* csBlob, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_rootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        const HRESULT psoHr = g_context.gpu.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(psoHr))
        {
            if (error) *error = "Create MSE PSO failed";
            return false;
        }
        return true;
    };

    if (!buildPso(spBlob.Get(), g_streamPowerPso)) { g_status = "MSE SPE PSO failed"; return false; }
    if (!buildPso(thBlob.Get(), g_thermalPso)) { g_status = "MSE thermal PSO failed"; return false; }
    if (!buildPso(depBlob.Get(), g_depositionPso)) { g_status = "MSE deposition PSO failed"; return false; }

    g_ready = true;
    g_status = "MSE GPU Compute dispatch ready";
    return true;
}

bool RunMseComputeGridImmediate(rock::HeightfieldGrid& grid, const rock::MultiScaleErosionSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureMseComputePipeline(error))
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
        if (error) *error = "Invalid heightfield for MSE GPU Compute";
        return false;
    }

    const UINT64 bufferSize = cellCount * sizeof(float);
    const std::vector<float> zeroData(static_cast<size_t>(cellCount), 0.0f);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC gpuDesc = BufferResourceDesc(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC cpuDesc = BufferResourceDesc(bufferSize);

    ComPtr<ID3D12Resource> heightA, heightB, streamA, streamB, sedA, sedB;
    ComPtr<ID3D12Resource> uploadHeights, uploadZero;
    ComPtr<ID3D12Resource> readbackHeights, readbackStream, readbackSed;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(heightA, "MSE heightA")) return false;
    if (!createDefault(heightB, "MSE heightB")) return false;
    if (!createDefault(streamA, "MSE streamA")) return false;
    if (!createDefault(streamB, "MSE streamB")) return false;
    if (!createDefault(sedA, "MSE sedA")) return false;
    if (!createDefault(sedB, "MSE sedB")) return false;
    if (!createUpload(uploadHeights, "MSE upload heights")) return false;
    if (!createUpload(uploadZero, "MSE upload zero")) return false;
    if (!createReadback(readbackHeights, "MSE readback heights")) return false;
    if (!createReadback(readbackStream, "MSE readback stream")) return false;
    if (!createReadback(readbackSed, "MSE readback sed")) return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map MSE height upload failed");
    std::memcpy(mapped, grid.heights.data(), bufferSize);
    uploadHeights->Unmap(0, nullptr);
    ThrowIfFailed(uploadZero->Map(0, &emptyReadRange, &mapped), "Map MSE zero upload failed");
    std::memcpy(mapped, zeroData.data(), bufferSize);
    uploadZero->Unmap(0, nullptr);

    constexpr UINT kStateCount = 8;
    constexpr UINT kDescriptorsPerState = 6;
    constexpr UINT kHeapDescriptors = kStateCount * kDescriptorsPerState;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(kHeapDescriptors);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create MSE descriptor heap failed"; return false; }
    const UINT descriptorSize = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = static_cast<UINT>(cellCount);
    uavDesc.Buffer.StructureByteStride = sizeof(float);

    auto resolveBuffer = [&](int which, ID3D12Resource* a, ID3D12Resource* b) {
        return which == 0 ? a : b;
    };
    D3D12_CPU_DESCRIPTOR_HANDLE descBase = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT state = 0; state < kStateCount; ++state)
    {
        const int hCur = static_cast<int>((state >> 0) & 1u);
        const int sCur = static_cast<int>((state >> 1) & 1u);
        const int dCur = static_cast<int>((state >> 2) & 1u);
        ID3D12Resource* uavs[6] = {
            resolveBuffer(hCur, heightA.Get(), heightB.Get()),
            resolveBuffer(1 - hCur, heightA.Get(), heightB.Get()),
            resolveBuffer(sCur, streamA.Get(), streamB.Get()),
            resolveBuffer(1 - sCur, streamA.Get(), streamB.Get()),
            resolveBuffer(dCur, sedA.Get(), sedB.Get()),
            resolveBuffer(1 - dCur, sedA.Get(), sedB.Get()),
        };
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descBase;
        handle.ptr += static_cast<SIZE_T>(state) * static_cast<SIZE_T>(kDescriptorsPerState) * descriptorSize;
        for (UINT i = 0; i < 6; ++i)
        {
            g_context.gpu.device->CreateUnorderedAccessView(uavs[i], nullptr, &uavDesc, handle);
            handle.ptr += descriptorSize;
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create MSE command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create MSE command list failed");

    commandList->CopyBufferRegion(heightA.Get(), 0, uploadHeights.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(heightB.Get(), 0, uploadHeights.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(streamA.Get(), 0, uploadZero.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(streamB.Get(), 0, uploadZero.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(sedA.Get(), 0, uploadZero.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(sedB.Get(), 0, uploadZero.Get(), 0, bufferSize);

    D3D12_RESOURCE_BARRIER toUav[6]{};
    ID3D12Resource* uavResources[6] = {heightA.Get(), heightB.Get(), streamA.Get(), streamB.Get(), sedA.Get(), sedB.Get()};
    for (int i = 0; i < 6; ++i)
    {
        toUav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav[i].Transition.pResource = uavResources[i];
        toUav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toUav[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(6, toUav);

    const float cellSizeMeters = grid.terrainSizeMeters / static_cast<float>(std::max<UINT>(1, resolution - 1));
    MseShaderConstants constants{};
    constants.resolution = resolution;
    constants.terrainSizeMeters = grid.terrainSizeMeters;
    constants.cellSizeMeters = cellSizeMeters;
    constants.cellDiag = cellSizeMeters * std::sqrt(2.0f);
    constants.refCellArea = 16.0f;
    constants.speStrength = settings.speStrength;
    constants.streamExponent = settings.streamExponent;
    constants.slopeExponent = settings.slopeExponent;
    constants.maxStreamPower = settings.maxStreamPower;
    constants.flowExponent = settings.flowExponent;
    constants.speTimeStep = settings.speTimeStep;
    constants.thermalTanAngle = std::tan(settings.thermalAngleDegrees * 3.14159265358979323846f / 180.0f);
    constants.thermalStrength = settings.thermalStrength;
    constants.thermalNoisifyAngle = settings.thermalNoisifyAngle ? 1u : 0u;
    constants.thermalNoiseMin = settings.thermalNoiseMin;
    constants.thermalNoiseMax = settings.thermalNoiseMax;
    constants.thermalNoiseWavelength = settings.thermalNoiseWavelength;
    constants.depositionStrength = settings.depositionStrength;
    constants.rain = settings.rain;

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());

    const UINT groupCount = (resolution + 7u) / 8u;
    const int iterations = std::clamp(settings.iterations, 1, 500);

    auto stateIndex = [](int hCur, int sCur, int dCur) -> UINT {
        return static_cast<UINT>(hCur | (sCur << 1) | (dCur << 2));
    };
    auto bindStateAndDispatch = [&](int hCur, int sCur, int dCur, ID3D12PipelineState* pso) {
        commandList->SetPipelineState(pso);
        commandList->SetComputeRoot32BitConstants(0, 21, &constants, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE table = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += static_cast<UINT64>(stateIndex(hCur, sCur, dCur)) * kDescriptorsPerState * descriptorSize;
        commandList->SetComputeRootDescriptorTable(1, table);
        commandList->Dispatch(groupCount, groupCount, 1);

        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &uavBarrier);
    };

    int hCur = 0;
    int sCur = 0;
    int dCur = 0;
    for (int it = 0; it < iterations; ++it)
    {
        if (settings.enableStreamPower)
        {
            bindStateAndDispatch(hCur, sCur, dCur, g_streamPowerPso.Get());
            hCur ^= 1;
            sCur ^= 1;
        }
        if (settings.enableThermal)
        {
            bindStateAndDispatch(hCur, sCur, dCur, g_thermalPso.Get());
            hCur ^= 1;
        }
        if (settings.enableDeposition)
        {
            bindStateAndDispatch(hCur, sCur, dCur, g_depositionPso.Get());
            hCur ^= 1;
            sCur ^= 1;
            dCur ^= 1;
        }
    }

    ID3D12Resource* finalHeight = (hCur == 0) ? heightA.Get() : heightB.Get();
    ID3D12Resource* finalStream = (sCur == 0) ? streamA.Get() : streamB.Get();
    ID3D12Resource* finalSed = (dCur == 0) ? sedA.Get() : sedB.Get();

    D3D12_RESOURCE_BARRIER toCopy[3]{};
    ID3D12Resource* copyResources[3] = {finalHeight, finalStream, finalSed};
    for (int i = 0; i < 3; ++i)
    {
        toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy[i].Transition.pResource = copyResources[i];
        toCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(3, toCopy);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, finalHeight, 0, bufferSize);
    commandList->CopyBufferRegion(readbackStream.Get(), 0, finalStream, 0, bufferSize);
    commandList->CopyBufferRegion(readbackSed.Get(), 0, finalSed, 0, bufferSize);
    ThrowIfFailed(commandList->Close(), "Close MSE command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal MSE fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedStream = nullptr;
    void* mappedSed = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bufferSize)};
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map MSE height readback failed");
    ThrowIfFailed(readbackStream->Map(0, &readRange, &mappedStream), "Map MSE stream readback failed");
    ThrowIfFailed(readbackSed->Map(0, &readRange, &mappedSed), "Map MSE sed readback failed");
    const float* heightValues = static_cast<const float*>(mappedHeights);
    const float* streamValues = static_cast<const float*>(mappedStream);
    const float* sedValues = static_cast<const float*>(mappedSed);
    grid.heights.assign(heightValues, heightValues + cellCount);
    grid.flows.assign(streamValues, streamValues + cellCount);
    grid.deposits.assign(sedValues, sedValues + cellCount);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    grid.age.assign(static_cast<size_t>(cellCount), 0.0f);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackStream->Unmap(0, &emptyWriteRange);
    readbackSed->Unmap(0, &emptyWriteRange);

    auto normalize = [](std::vector<float>& field) {
        float maxValue = 0.0f;
        for (float v : field) { maxValue = std::max(maxValue, v); }
        if (maxValue > 1e-6f)
        {
            for (float& v : field) { v = std::clamp(v / maxValue, 0.0f, 1.0f); }
        }
    };
    normalize(grid.flows);
    normalize(grid.deposits);

    g_status = "MSE GPU Compute evaluated heightfield";
    return true;
}

} // namespace

void SetMseComputeContext(MseComputeContext context)
{
    g_context = std::move(context);
}

void ResetMseComputeResources()
{
    g_streamPowerPso.Reset();
    g_thermalPso.Reset();
    g_depositionPso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& MseComputeStatus()
{
    return g_status;
}

bool RunMseComputeGrid(rock::HeightfieldGrid& grid, const rock::MultiScaleErosionSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunMseComputeGridImmediate(grid, settings, error);
    }

    auto request = std::make_shared<MseGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<MseGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "MSE GPU Compute queued on main thread";

    MseGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingMseGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<MseGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<MseGpuRequest>& request : requests)
    {
        MseGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunMseComputeGridImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
