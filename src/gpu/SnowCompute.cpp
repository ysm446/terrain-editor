#include "SnowCompute.h"

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

SnowComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_copyInputHeightsPso;
ComPtr<ID3D12PipelineState> g_computeThicknessPso;
ComPtr<ID3D12PipelineState> g_envelopeSmoothingPso;
ComPtr<ID3D12PipelineState> g_surfaceSmoothingPso;
ComPtr<ID3D12PipelineState> g_applyPso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Snow GPU Compute not initialized";

// Mirrors the cbuffer in shaders/snow_compute.hlsl.
struct SnowShaderConstants
{
    UINT resolution;
    float terrainSizeMeters;
    float emissionAmount;
    float motionLimitTan;

    float transportRate;
    float maskThresholdM;
    UINT settleStride;
    UINT smoothDirection;

    float maskFeatherM;
    float surfaceSmoothing;
    UINT smoothRadius;
    float slopeDependentEmission;
};
static_assert(sizeof(SnowShaderConstants) == 12 * sizeof(UINT), "SnowShaderConstants must be 12 DWORDs");

// Snow / Soil 共有の GPU 再配分パラメータ。両ノードの設定をここへ写して
// 同じ compute pipeline を実行する。
struct SettleGpuParams
{
    float emissionAmount = 0.0f;
    int iterationCount = 1;
    float emissionTime = 0.0f;
    int settlingPasses = 1;
    float motionSlopeLimitDeg = 35.0f;
    float transportRate = 0.0f;
    float surfaceSmoothing = 0.0f;
    float maskThresholdM = 0.0f;
    float maskFeatherM = 0.0f;
    float largestDetailLevelM = 8.0f;
    float slopeDependentEmission = 0.0f;
};

SettleGpuParams MakeSettleParams(const rock::SnowSettings& settings)
{
    SettleGpuParams params;
    params.emissionAmount = settings.emissionAmount;
    params.iterationCount = settings.iterationCount;
    params.emissionTime = settings.emissionTime;
    params.settlingPasses = settings.smoothingIterations;
    params.motionSlopeLimitDeg = settings.motionSlopeLimitDeg;
    params.transportRate = settings.transportRate;
    params.surfaceSmoothing = settings.surfaceSmoothing;
    params.maskThresholdM = settings.maskThresholdM;
    params.maskFeatherM = settings.maskFeatherM;
    params.largestDetailLevelM = settings.largestDetailLevelM;
    params.slopeDependentEmission = 0.0f;
    return params;
}

SettleGpuParams MakeSettleParams(const rock::SoilSettings& settings)
{
    SettleGpuParams params;
    params.emissionAmount = settings.emissionAmount;
    params.iterationCount = settings.iterationCount;
    params.emissionTime = settings.emissionTime;
    params.settlingPasses = settings.settlingPasses;
    params.motionSlopeLimitDeg = settings.motionSlopeLimitDeg;
    params.transportRate = settings.transportRate;
    params.surfaceSmoothing = settings.surfaceSmoothing;
    params.maskThresholdM = settings.maskThresholdM;
    params.maskFeatherM = settings.maskFeatherM;
    params.largestDetailLevelM = settings.largestDetailLevelM;
    params.slopeDependentEmission = settings.slopeDependentEmission;
    return params;
}

struct SnowGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct SnowGpuRequest
{
    rock::HeightfieldGrid grid;
    SettleGpuParams params;
    std::promise<SnowGpuRequestResult> promise;
};

std::vector<std::shared_ptr<SnowGpuRequest>> g_pendingRequests;

bool EnsureSnowComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature
        && g_copyInputHeightsPso && g_computeThicknessPso
        && g_envelopeSmoothingPso && g_surfaceSmoothingPso && g_applyPso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "Snow GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 7;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 12;
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
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Snow root sig failed";
        g_status = "Snow GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();

    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Snow shader failed";
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
            if (error) *error = "Create Snow PSO failed";
            return false;
        }
        return true;
    };

    struct Entry
    {
        const char* name;
        ComPtr<ID3D12PipelineState>* pso;
    };
    Entry entries[] = {
        {"CSCopyInputHeights", &g_copyInputHeightsPso},
        {"CSComputeThickness", &g_computeThicknessPso},
        {"CSEnvelopeSmoothing", &g_envelopeSmoothingPso},
        {"CSSmoothSnowSurface", &g_surfaceSmoothingPso},
        {"CSApply", &g_applyPso},
    };
    for (const Entry& e : entries)
    {
        ComPtr<ID3DBlob> blob;
        if (!compileEntry(e.name, blob))
        {
            g_status = std::string("Snow ") + e.name + " compile failed";
            return false;
        }
        if (!buildPso(blob.Get(), *e.pso))
        {
            g_status = std::string("Snow ") + e.name + " PSO failed";
            return false;
        }
    }

    g_ready = true;
    g_status = "Snow GPU Compute dispatch ready";
    return true;
}

bool RunSnowComputeImmediate(rock::HeightfieldGrid& grid, const SettleGpuParams& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureSnowComputePipeline(error))
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
        if (error) *error = "Invalid heightfield for Snow GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize = cellCount * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc = BufferResourceDesc(fieldByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc = BufferResourceDesc(fieldByteSize);

    ComPtr<ID3D12Resource> inputHeightsBuf, baseHeightsBuf, thicknessBuf, surfABuf, surfBBuf, outHeightsBuf, outMaskBuf;
    ComPtr<ID3D12Resource> uploadHeights, readbackHeights, readbackMask;

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

    if (!createDefault(inputHeightsBuf, fieldGpuDesc, "Snow input heights")) return false;
    if (!createDefault(baseHeightsBuf, fieldGpuDesc, "Snow base heights")) return false;
    if (!createDefault(thicknessBuf, fieldGpuDesc, "Snow thickness")) return false;
    if (!createDefault(surfABuf, fieldGpuDesc, "Snow surfA")) return false;
    if (!createDefault(surfBBuf, fieldGpuDesc, "Snow surfB")) return false;
    if (!createDefault(outHeightsBuf, fieldGpuDesc, "Snow out heights")) return false;
    if (!createDefault(outMaskBuf, fieldGpuDesc, "Snow out mask")) return false;
    if (!createUpload(uploadHeights, "Snow upload heights")) return false;
    if (!createReadback(readbackHeights, "Snow readback heights")) return false;
    if (!createReadback(readbackMask, "Snow readback mask")) return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map Snow heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 7;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(kDescriptorCount);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Snow descriptor heap failed"; return false; }
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
    createUav(inputHeightsBuf.Get(), static_cast<UINT>(cellCount), 0);
    createUav(baseHeightsBuf.Get(), static_cast<UINT>(cellCount), 1);
    createUav(thicknessBuf.Get(), static_cast<UINT>(cellCount), 2);
    createUav(surfABuf.Get(), static_cast<UINT>(cellCount), 3);
    createUav(surfBBuf.Get(), static_cast<UINT>(cellCount), 4);
    createUav(outHeightsBuf.Get(), static_cast<UINT>(cellCount), 5);
    createUav(outMaskBuf.Get(), static_cast<UINT>(cellCount), 6);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Snow command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Snow command list failed");

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &b);
    };
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &b);
    };

    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(inputHeightsBuf.Get(), 0, uploadHeights.Get(), 0, fieldByteSize);
    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    SnowShaderConstants k{};
    k.resolution = resolution;
    k.terrainSizeMeters = std::max(grid.terrainSizeMeters, 1.0f);
    const float totalEmission = std::max(0.0f, settings.emissionAmount);
    k.emissionAmount = totalEmission;
    const float kPi = 3.14159265358979323846f;
    k.motionLimitTan = std::tan(std::clamp(settings.motionSlopeLimitDeg, 0.0f, 89.9f) * (kPi / 180.0f));
    k.transportRate = std::clamp(settings.transportRate, 0.0f, 1.0f);
    k.maskThresholdM = std::max(0.0f, settings.maskThresholdM);
    k.settleStride = 1u;
    k.smoothDirection = 0u;
    k.maskFeatherM = std::max(0.0f, settings.maskFeatherM);
    k.surfaceSmoothing = std::clamp(settings.surfaceSmoothing, 0.0f, 1.0f);
    const float cellSizeMeters = k.terrainSizeMeters / static_cast<float>(std::max(1u, resolution - 1u));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSizeMeters, k.terrainSizeMeters * 0.5f);
    const int maxStride = std::clamp(static_cast<int>(std::round(largestDetailM / cellSizeMeters)), 1, 64);
    k.smoothRadius = static_cast<UINT>(std::clamp(maxStride, 1, 32));
    k.slopeDependentEmission = std::clamp(settings.slopeDependentEmission, 0.0f, 1.0f);
    auto setConstants = [&]() {
        commandList->SetComputeRoot32BitConstants(0, 12, &k, 0);
    };
    setConstants();

    const UINT groupCount = (resolution + 7u) / 8u;

    commandList->SetPipelineState(g_copyInputHeightsPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    const int iterationCount = std::clamp(settings.iterationCount, 1, 256);
    const float emissionTime = std::clamp(settings.emissionTime, 0.0f, 1.0f);
    const int emissionIterations = emissionTime <= 0.0f
        ? 1
        : std::clamp(static_cast<int>(std::ceil(static_cast<float>(iterationCount) * emissionTime)), 1, iterationCount);
    const float emissionPerIteration = totalEmission / static_cast<float>(emissionIterations);
    const int settlingPasses = std::clamp(settings.settlingPasses, 1, 16);
    int strideLevels = 0;
    for (int stride = maxStride; stride > 1; stride = std::max(1, stride / 2))
    {
        ++strideLevels;
    }
    for (int iter = 0; iter < iterationCount; ++iter)
    {
        const float stepEmission = (iter < emissionIterations) ? emissionPerIteration : 0.0f;
        k.emissionAmount = stepEmission;
        k.settleStride = 1u;
        setConstants();

        commandList->SetPipelineState(g_computeThicknessPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();

        commandList->SetPipelineState(g_envelopeSmoothingPso.Get());
        for (int pass = 0; pass < settlingPasses && k.transportRate > 0.0f; ++pass)
        {
            const int level = settlingPasses <= 1 ? strideLevels : (pass * strideLevels) / std::max(1, settlingPasses - 1);
            int stride = maxStride;
            for (int s = 0; s < level; ++s)
            {
                stride = std::max(1, stride / 2);
            }
            k.settleStride = static_cast<UINT>(std::max(1, stride));
            setConstants();
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();

            k.settleStride = 0u;
            setConstants();
            commandList->SetPipelineState(g_computeThicknessPso.Get());
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();
            commandList->SetPipelineState(g_envelopeSmoothingPso.Get());
        }
    }
    k.emissionAmount = totalEmission;
    k.settleStride = 1u;
    setConstants();

    if (k.surfaceSmoothing > 0.0f)
    {
        commandList->SetPipelineState(g_surfaceSmoothingPso.Get());
        k.smoothDirection = 0u;
        setConstants();
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();

        k.smoothDirection = 1u;
        setConstants();
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }

    commandList->SetPipelineState(g_applyPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    D3D12_RESOURCE_BARRIER toCopySrc[2]{};
    ID3D12Resource* copyResources[2] = {outHeightsBuf.Get(), outMaskBuf.Get()};
    for (int i = 0; i < 2; ++i)
    {
        toCopySrc[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySrc[i].Transition.pResource = copyResources[i];
        toCopySrc[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopySrc[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySrc[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(2, toCopySrc);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, outHeightsBuf.Get(), 0, fieldByteSize);
    commandList->CopyBufferRegion(readbackMask.Get(), 0, outMaskBuf.Get(), 0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close Snow command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal Snow fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedMask = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map Snow readback heights failed");
    ThrowIfFailed(readbackMask->Map(0, &readRange, &mappedMask), "Map Snow readback mask failed");
    std::memcpy(grid.heights.data(), mappedHeights, fieldByteSize);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.mask.data(), mappedMask, fieldByteSize);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackMask->Unmap(0, &emptyWriteRange);

    g_status = "Snow GPU Compute evaluated";
    return true;
}

} // namespace

void SetSnowComputeContext(SnowComputeContext context)
{
    g_context = std::move(context);
}

void ResetSnowComputeResources()
{
    g_copyInputHeightsPso.Reset();
    g_computeThicknessPso.Reset();
    g_envelopeSmoothingPso.Reset();
    g_surfaceSmoothingPso.Reset();
    g_applyPso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& SnowComputeStatus()
{
    return g_status;
}

namespace
{
bool RunSettleCompute(rock::HeightfieldGrid& grid, const SettleGpuParams& params, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunSnowComputeImmediate(grid, params, error);
    }

    auto request = std::make_shared<SnowGpuRequest>();
    request->grid = grid;
    request->params = params;
    std::future<SnowGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "Snow GPU Compute queued on main thread";

    SnowGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}
} // namespace

bool RunSnowCompute(rock::HeightfieldGrid& grid, const rock::SnowSettings& settings, std::string* error)
{
    return RunSettleCompute(grid, MakeSettleParams(settings), error);
}

bool RunSoilCompute(rock::HeightfieldGrid& grid, const rock::SoilSettings& settings, std::string* error)
{
    return RunSettleCompute(grid, MakeSettleParams(settings), error);
}

void ProcessPendingSnowGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<SnowGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<SnowGpuRequest>& request : requests)
    {
        SnowGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunSnowComputeImmediate(result.grid, request->params, &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
