#include "RockCompute.h"

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

RockComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Rock GPU Compute not initialized";

// Mirrors the cbuffer in shaders/rock_compute.hlsl.
struct RockShaderConstants
{
    UINT resolution;
    int seed;
    float terrainSizeMeters;
    float density;

    float coverage;
    float rockSizeMinCells;
    float rockSizeMaxCells;
    float rockHeight;

    float heightJitter;
    float rotationVar;
    float aspectVar;
    float edgeSharpness;

    float bumpiness;
    float facetSharpness;
    float facetScale;
    int searchRadius;

    float maxReach;
    float domeExp;
    int needPolyhedral;
    int rockStyle;

    int orientationRule;
    int layerCount;
    int pad2;
    int pad3;
};
static_assert(sizeof(RockShaderConstants) == 24 * sizeof(UINT), "RockShaderConstants must be 24 DWORDs");

struct RockGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct RockGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::RockSettings settings;
    std::promise<RockGpuRequestResult> promise;
};

std::vector<std::shared_ptr<RockGpuRequest>> g_pendingRequests;

bool EnsureRockComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature && g_pso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "Rock GPU Compute unavailable";
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
    rootParams[0].Constants.Num32BitValues = 24;
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
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Rock root sig failed";
        g_status = "Rock GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();
    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    const HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                 "CSRock", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Rock shader failed";
        g_status = "Rock shader compile failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    const HRESULT psoHr = g_context.gpu.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_pso));
    if (FAILED(psoHr))
    {
        if (error) *error = "Create Rock PSO failed";
        g_status = "Rock PSO failed";
        return false;
    }

    g_ready = true;
    g_status = "Rock GPU Compute dispatch ready";
    return true;
}

bool RunRockComputeImmediate(rock::HeightfieldGrid& grid, const rock::RockSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureRockComputePipeline(error))
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
    if (resolution < 2 || grid.heights.size() < cellCount || settings.density <= 0.0f)
    {
        if (error) *error = "Invalid heightfield for Rock GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize = cellCount * sizeof(float);
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc = BufferResourceDesc(fieldByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc = BufferResourceDesc(fieldByteSize);

    ComPtr<ID3D12Resource> inputHeightsBuf, outputHeightsBuf, outputMaskBuf, outputUniqueMaskBuf;
    ComPtr<ID3D12Resource> uploadHeights;
    ComPtr<ID3D12Resource> readbackHeights, readbackMask, readbackUniqueMask;

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

    if (!createDefault(inputHeightsBuf, fieldGpuDesc, "Rock input-heights buffer")) return false;
    if (!createDefault(outputHeightsBuf, fieldGpuDesc, "Rock output-heights buffer")) return false;
    if (!createDefault(outputMaskBuf, fieldGpuDesc, "Rock output-mask buffer")) return false;
    if (!createDefault(outputUniqueMaskBuf, fieldGpuDesc, "Rock output-unique-mask buffer")) return false;
    if (!createUpload(uploadHeights, "Rock upload heights")) return false;
    if (!createReadback(readbackHeights, "Rock readback heights")) return false;
    if (!createReadback(readbackMask, "Rock readback mask")) return false;
    if (!createReadback(readbackUniqueMask, "Rock readback unique mask")) return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map Rock heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 4;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(kDescriptorCount);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Rock descriptor heap failed"; return false; }

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
    createUav(outputHeightsBuf.Get(), static_cast<UINT>(cellCount), 1);
    createUav(outputMaskBuf.Get(), static_cast<UINT>(cellCount), 2);
    createUav(outputUniqueMaskBuf.Get(), static_cast<UINT>(cellCount), 3);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Rock command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Rock command list failed");

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

    const float density = std::max(settings.density, 0.1f);
    const float rockSizeMinM = std::clamp(settings.rockSizeMinM, 0.1f, 200.0f);
    const float rockSizeMaxM = std::clamp(std::max(settings.rockSizeMaxM, rockSizeMinM), 0.1f, 200.0f);
    const float rockSizeMinCells = rockSizeMinM / density;
    const float rockSizeMaxCells = rockSizeMaxM / density;
    const float aspectVar = std::clamp(settings.aspectVariation, 0.0f, 1.0f);
    const float maxDomeRadius = rockSizeMaxCells * 0.5f;
    const float edgeSharpness = std::clamp(settings.edgeSharpness, 0.0f, 1.0f);
    const float facetSharpness = std::clamp(settings.facetSharpness, 0.0f, 1.0f);
    const int rockStyle = std::clamp(static_cast<int>(settings.style), 0, 2);
    const bool polygonalStyle = rockStyle != static_cast<int>(rock::RockStyle::Classic);
    const bool shardStyle = rockStyle == static_cast<int>(rock::RockStyle::Shard);
    const float styleAspectBoost = shardStyle ? 0.65f : 0.0f;
    const float maxAspect = std::pow(2.0f, aspectVar + styleAspectBoost);
    const float maxReach = maxDomeRadius * maxAspect;
    const int searchRadius = std::max(1, static_cast<int>(std::ceil(maxReach - 0.05f)));
    const float effectiveEdgeSharpness = polygonalStyle ? std::max(edgeSharpness, 0.65f) : edgeSharpness;
    const float domeExp = polygonalStyle ? 1.0f : (1.0f + facetSharpness * 1.5f * (1.0f - edgeSharpness));
    const int orientationRule = std::clamp(static_cast<int>(settings.orientationRule), 0, 2);
    const int layerCount = std::clamp(settings.layerCount, 1, 8);

    RockShaderConstants constants{};
    constants.resolution = resolution;
    constants.seed = settings.seed;
    constants.terrainSizeMeters = std::max(grid.terrainSizeMeters, 1.0f);
    constants.density = density;
    constants.coverage = std::clamp(settings.coverage, 0.0f, 1.0f);
    constants.rockSizeMinCells = rockSizeMinCells;
    constants.rockSizeMaxCells = rockSizeMaxCells;
    constants.rockHeight = std::max(settings.rockHeight, 0.0f);
    constants.heightJitter = std::clamp(settings.heightJitter, 0.0f, 1.0f);
    constants.rotationVar = std::clamp(settings.rotationVariation, 0.0f, 1.0f);
    constants.aspectVar = aspectVar;
    constants.edgeSharpness = edgeSharpness;
    constants.bumpiness = std::clamp(settings.bumpiness, 0.0f, 1.0f);
    constants.facetSharpness = facetSharpness;
    constants.facetScale = std::clamp(settings.facetScale, 0.5f, 8.0f);
    constants.searchRadius = searchRadius;
    constants.maxReach = maxReach;
    constants.domeExp = domeExp;
    constants.needPolyhedral = (effectiveEdgeSharpness > 0.0f) ? 1 : 0;
    constants.rockStyle = rockStyle;
    constants.orientationRule = orientationRule;
    constants.layerCount = layerCount;
    constants.pad2 = 0;
    constants.pad3 = 0;
    commandList->SetComputeRoot32BitConstants(0, 24, &constants, 0);

    commandList->SetPipelineState(g_pso.Get());
    const UINT groupCount = (resolution + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &uavBar);

    D3D12_RESOURCE_BARRIER toCopySrc[3]{};
    ID3D12Resource* copyResources[3] = {outputHeightsBuf.Get(), outputMaskBuf.Get(), outputUniqueMaskBuf.Get()};
    for (int i = 0; i < 3; ++i)
    {
        toCopySrc[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySrc[i].Transition.pResource = copyResources[i];
        toCopySrc[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopySrc[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySrc[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(3, toCopySrc);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, outputHeightsBuf.Get(), 0, fieldByteSize);
    commandList->CopyBufferRegion(readbackMask.Get(), 0, outputMaskBuf.Get(), 0, fieldByteSize);
    commandList->CopyBufferRegion(readbackUniqueMask.Get(), 0, outputUniqueMaskBuf.Get(), 0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close Rock command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal Rock fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedMask = nullptr;
    void* mappedUniqueMask = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map Rock readback (heights) failed");
    ThrowIfFailed(readbackMask->Map(0, &readRange, &mappedMask), "Map Rock readback (mask) failed");
    ThrowIfFailed(readbackUniqueMask->Map(0, &readRange, &mappedUniqueMask), "Map Rock readback (unique mask) failed");
    std::memcpy(grid.heights.data(), mappedHeights, fieldByteSize);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.mask.data(), mappedMask, fieldByteSize);
    grid.uniqueMask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.uniqueMask.data(), mappedUniqueMask, fieldByteSize);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackMask->Unmap(0, &emptyWriteRange);
    readbackUniqueMask->Unmap(0, &emptyWriteRange);

    g_status = "Rock GPU Compute evaluated";
    return true;
}

} // namespace

void SetRockComputeContext(RockComputeContext context)
{
    g_context = std::move(context);
}

void ResetRockComputeResources()
{
    g_pso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& RockComputeStatus()
{
    return g_status;
}

bool RunRockCompute(rock::HeightfieldGrid& grid, const rock::RockSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunRockComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<RockGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<RockGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "Rock GPU Compute queued on main thread";

    RockGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingRockGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<RockGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<RockGpuRequest>& request : requests)
    {
        RockGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunRockComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
