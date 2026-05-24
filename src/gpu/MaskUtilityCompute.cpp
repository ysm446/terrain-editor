#include "MaskUtilityCompute.h"

#include "../D3D12Utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
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

MaskUtilityComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_maskPathPso;
ComPtr<ID3D12PipelineState> g_blurHorizontalPso;
ComPtr<ID3D12PipelineState> g_blurVerticalPso;
ComPtr<ID3D12PipelineState> g_heightmapFromMaskPso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Mask Utility GPU Compute not initialized";

struct Constants
{
    UINT resolution = 0;
    UINT sourceResolution = 0;
    UINT pointCount = 0;
    UINT segmentCount = 0;
    float terrainSizeMeters = 1.0f;
    float gamma = 1.0f;
    float invert = 0.0f;
    float radiusPixels = 0.0f;
    UINT iterations = 1;
    float strength = 1.0f;
    float heightMeters = 0.0f;
    float baseHeightMeters = 0.0f;
};
static_assert(sizeof(Constants) == 12 * sizeof(UINT), "Constants must be 12 DWORDs");

struct PointGpu
{
    float x, z, width, feather, intensity, pad0, pad1, pad2;
};

struct SegmentGpu
{
    float ax, az, abX, abZ, lenSq, widthA, widthB, featherA, featherB, intensityA, intensityB, pad0;
};

struct RequestResult
{
    bool success = false;
    rock::MaskGrid mask;
    rock::HeightfieldGrid heightfield;
    std::string error;
};

struct Request
{
    enum class Kind { MaskPath, MaskBlur, HeightmapFromMask };
    Kind kind = Kind::MaskPath;
    rock::MaskGrid mask;
    rock::PathSettings path;
    rock::MaskPathSettings maskPath;
    rock::MaskBlurSettings maskBlur;
    rock::HeightmapFromMaskSettings heightmapFromMask;
    int resolution = 0;
    float terrainSizeMeters = 1.0f;
    std::promise<RequestResult> promise;
};

std::vector<std::shared_ptr<Request>> g_pendingRequests;

template <typename T>
ComPtr<ID3D12Resource> CreateUploadBuffer(const std::vector<T>& values)
{
    if (values.empty())
    {
        return {};
    }
    const UINT64 size = static_cast<UINT64>(values.size()) * sizeof(T);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferResourceDesc(size);
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(g_context.gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)), "Create upload buffer failed");
    void* mapped = nullptr;
    ThrowIfFailed(buffer->Map(0, nullptr, &mapped), "Map upload buffer failed");
    std::memcpy(mapped, values.data(), static_cast<size_t>(size));
    buffer->Unmap(0, nullptr);
    return buffer;
}

ComPtr<ID3D12Resource> CreateDefaultBuffer(UINT64 size, D3D12_RESOURCE_STATES initialState, D3D12_RESOURCE_FLAGS flags)
{
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC desc = BufferResourceDesc(size, flags);
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(g_context.gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr, IID_PPV_ARGS(&buffer)), "Create default buffer failed");
    return buffer;
}

ComPtr<ID3D12Resource> CreateReadbackBuffer(UINT64 size)
{
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC desc = BufferResourceDesc(size);
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(g_context.gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)), "Create readback buffer failed");
    return buffer;
}

void AddTransition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

void AddUavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    commandList->ResourceBarrier(1, &barrier);
}

bool CompilePso(const char* entry, ID3D12PipelineState** outPso, std::string* error)
{
    const UINT compileFlags = DefaultShaderCompileFlags();
    ComPtr<ID3DBlob> csBlob;
    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entry, "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Mask Utility shader failed";
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    hr = g_context.gpu.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(outPso));
    if (FAILED(hr))
    {
        if (error) *error = "Create Mask Utility PSO failed";
        return false;
    }
    return true;
}

bool EnsurePipeline(std::string* error)
{
    if (g_ready)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4;
    srvRange.BaseShaderRegister = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 3;
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.Num32BitValues = 12;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = rootParams;
    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_context.gpu.device, rsDesc, g_rootSignature.ReleaseAndGetAddressOf(), errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Mask Utility root sig failed";
        return false;
    }

    if (!CompilePso("CSMaskPath", g_maskPathPso.ReleaseAndGetAddressOf(), error) ||
        !CompilePso("CSMaskBlurHorizontal", g_blurHorizontalPso.ReleaseAndGetAddressOf(), error) ||
        !CompilePso("CSMaskBlurVertical", g_blurVerticalPso.ReleaseAndGetAddressOf(), error) ||
        !CompilePso("CSHeightmapFromMask", g_heightmapFromMaskPso.ReleaseAndGetAddressOf(), error))
    {
        return false;
    }
    g_ready = true;
    g_status = "Mask Utility GPU Compute dispatch ready";
    return true;
}

void CreateSrv(ID3D12Resource* resource, UINT elements, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.NumElements = elements;
    desc.Buffer.StructureByteStride = stride;
    g_context.gpu.device->CreateShaderResourceView(resource, &desc, handle);
}

void CreateUav(ID3D12Resource* resource, UINT elements, UINT stride, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements = elements;
    desc.Buffer.StructureByteStride = stride;
    g_context.gpu.device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void ExecuteAndWait(ComPtr<ID3D12GraphicsCommandList>& commandList)
{
    ThrowIfFailed(commandList->Close(), "Close Mask Utility command list failed");
    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal Mask Utility fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);
}

} // namespace

void SetMaskUtilityComputeContext(MaskUtilityComputeContext context)
{
    g_context = std::move(context);
}

void ResetMaskUtilityComputeResources()
{
    g_heightmapFromMaskPso.Reset();
    g_blurVerticalPso.Reset();
    g_blurHorizontalPso.Reset();
    g_maskPathPso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& MaskUtilityComputeStatus()
{
    return g_status;
}

bool RunMaskPathCompute(rock::MaskGrid& grid, const rock::PathSettings& path, const rock::MaskPathSettings& settings, float terrainSizeMeters, std::string* error)
{
    const bool hasSmoothSegments = std::ranges::any_of(path.edges, [](const rock::PathEdge& edge) {
        return edge.enabled && edge.segmentType != rock::PathSegmentType::Line;
    });
    if (hasSmoothSegments)
    {
        if (error) *error = "Mask Path GPU currently supports straight path segments only";
        return false;
    }

    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        auto request = std::make_shared<Request>();
        request->kind = Request::Kind::MaskPath;
        request->mask = grid;
        request->path = path;
        request->maskPath = settings;
        request->terrainSizeMeters = terrainSizeMeters;
        std::future<RequestResult> future = request->promise.get_future();
        {
            std::lock_guard<std::mutex> lock(g_requestMutex);
            g_pendingRequests.push_back(request);
        }
        g_status = "Mask Path GPU Compute queued on main thread";
        RequestResult result = future.get();
        if (!result.success)
        {
            if (error) *error = result.error;
            return false;
        }
        grid = std::move(result.mask);
        return true;
    }

    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsurePipeline(error) || !IsGpuComputeContextReady(g_context.gpu)) { return false; }
    const UINT n = static_cast<UINT>(std::clamp(grid.resolution, 2, 2048));
    const UINT64 cellCount = static_cast<UINT64>(n) * n;

    std::vector<PointGpu> points;
    points.reserve(path.points.size());
    for (const rock::PathPoint& p : path.points)
    {
        points.push_back({p.x, p.z, std::clamp(p.widthMeters, 0.01f, 100000.0f), std::clamp(p.featherMeters, 0.0f, 100000.0f), std::clamp(p.intensity, 0.0f, 1.0f), 0, 0, 0});
    }
    if (points.empty())
    {
        grid.values.assign(static_cast<size_t>(cellCount), 0.0f);
        return true;
    }
    std::vector<SegmentGpu> segments;
    for (const rock::PathEdge& edge : path.edges)
    {
        if (!edge.enabled) { continue; }
        const auto findPoint = [&](rock::GraphId id) -> const rock::PathPoint* {
            const auto it = std::ranges::find_if(path.points, [id](const rock::PathPoint& p) { return p.id == id; });
            return it != path.points.end() ? &*it : nullptr;
        };
        const rock::PathPoint* a = findPoint(edge.fromPoint);
        const rock::PathPoint* b = findPoint(edge.toPoint);
        if (!a || !b) { continue; }
        const float abX = b->x - a->x;
        const float abZ = b->z - a->z;
        const float lenSq = abX * abX + abZ * abZ;
        if (lenSq <= 0.000001f) { continue; }
        segments.push_back({a->x, a->z, abX, abZ, lenSq,
            std::clamp(a->widthMeters, 0.01f, 100000.0f), std::clamp(b->widthMeters, 0.01f, 100000.0f),
            std::clamp(a->featherMeters, 0.0f, 100000.0f), std::clamp(b->featherMeters, 0.0f, 100000.0f),
            std::clamp(a->intensity, 0.0f, 1.0f), std::clamp(b->intensity, 0.0f, 1.0f), 0});
    }

    const UINT64 outSize = cellCount * sizeof(float);
    auto pointUpload = CreateUploadBuffer(points);
    auto segmentUpload = CreateUploadBuffer(segments);
    auto output = CreateDefaultBuffer(outSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = CreateReadbackBuffer(outSize);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(7);
    ComPtr<ID3D12DescriptorHeap> heap;
    ThrowIfFailed(g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Create Mask Path descriptor heap failed");
    const UINT inc = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    CreateSrv(nullptr, 1, sizeof(float), h); h.ptr += inc;
    CreateSrv(pointUpload.Get(), static_cast<UINT>(points.size()), sizeof(PointGpu), h); h.ptr += inc;
    CreateSrv(segments.empty() ? nullptr : segmentUpload.Get(), static_cast<UINT>(std::max<size_t>(segments.size(), 1)), sizeof(SegmentGpu), h); h.ptr += inc;
    CreateSrv(nullptr, 1, sizeof(float), h); h.ptr += inc;
    CreateUav(output.Get(), static_cast<UINT>(cellCount), sizeof(float), h);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Mask Utility allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Mask Utility command list failed");
    Constants c{};
    c.resolution = n; c.pointCount = static_cast<UINT>(points.size()); c.segmentCount = static_cast<UINT>(segments.size());
    c.terrainSizeMeters = std::max(1.0f, terrainSizeMeters); c.gamma = std::clamp(settings.gamma, 0.05f, 8.0f); c.invert = settings.invert ? 1.0f : 0.0f;
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetPipelineState(g_maskPathPso.Get());
    commandList->SetComputeRoot32BitConstants(0, 12, &c, 0);
    commandList->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_GPU_DESCRIPTOR_HANDLE uavTable = heap->GetGPUDescriptorHandleForHeapStart();
    uavTable.ptr += static_cast<UINT64>(inc) * 4;
    commandList->SetComputeRootDescriptorTable(2, uavTable);
    const UINT groups = (n + 7u) / 8u;
    commandList->Dispatch(groups, groups, 1);
    AddTransition(commandList.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outSize);
    ExecuteAndWait(commandList);
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(outSize)};
    ThrowIfFailed(readback->Map(0, &range, &mapped), "Map Mask Path readback failed");
    const float* values = static_cast<const float*>(mapped);
    grid.values.assign(values, values + cellCount);
    readback->Unmap(0, nullptr);
    g_status = "Mask Path GPU Compute evaluated";
    return true;
}

bool RunMaskBlurCompute(rock::MaskGrid& grid, const rock::MaskBlurSettings& settings, float terrainSizeMeters, std::string* error)
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        auto request = std::make_shared<Request>();
        request->kind = Request::Kind::MaskBlur;
        request->mask = grid;
        request->maskBlur = settings;
        request->terrainSizeMeters = terrainSizeMeters;
        std::future<RequestResult> future = request->promise.get_future();
        {
            std::lock_guard<std::mutex> lock(g_requestMutex);
            g_pendingRequests.push_back(request);
        }
        g_status = "Mask Blur GPU Compute queued on main thread";
        RequestResult result = future.get();
        if (!result.success)
        {
            if (error) *error = result.error;
            return false;
        }
        grid = std::move(result.mask);
        return true;
    }

    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsurePipeline(error) || !IsGpuComputeContextReady(g_context.gpu)) { return false; }
    const UINT n = static_cast<UINT>(std::clamp(grid.resolution, 2, 2048));
    const UINT64 cellCount = static_cast<UINT64>(n) * n;
    if (grid.values.size() < cellCount) { return false; }
    const UINT64 size = cellCount * sizeof(float);
    const float cellSize = n > 1 ? std::max(1.0f, terrainSizeMeters) / static_cast<float>(n - 1) : std::max(1.0f, terrainSizeMeters);
    const int radius = std::clamp(static_cast<int>(std::ceil(std::max(0.0f, settings.radiusMeters) / std::max(cellSize, 0.0001f))), 0, static_cast<int>(n - 1));
    if (radius <= 0 || settings.strength <= 0.0f) { return true; }
    const int iterations = std::clamp(settings.iterations, 1, 16);

    auto upload = CreateUploadBuffer(grid.values);
    auto currentA = CreateDefaultBuffer(size, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto currentB = CreateDefaultBuffer(size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto scratch = CreateDefaultBuffer(size, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = CreateReadbackBuffer(size);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(7);
    ComPtr<ID3D12DescriptorHeap> heap;
    ThrowIfFailed(g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Create Mask Blur descriptor heap failed");
    const UINT inc = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    CreateSrv(currentA.Get(), static_cast<UINT>(cellCount), sizeof(float), h); h.ptr += inc;
    CreateSrv(nullptr, 1, sizeof(float), h); h.ptr += inc;
    CreateSrv(nullptr, 1, sizeof(float), h); h.ptr += inc;
    CreateSrv(currentB.Get(), static_cast<UINT>(cellCount), sizeof(float), h); h.ptr += inc;
    CreateUav(scratch.Get(), static_cast<UINT>(cellCount), sizeof(float), h); h.ptr += inc;
    CreateUav(currentB.Get(), static_cast<UINT>(cellCount), sizeof(float), h); h.ptr += inc;
    CreateUav(currentA.Get(), static_cast<UINT>(cellCount), sizeof(float), h);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Mask Utility allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Mask Utility command list failed");
    commandList->CopyBufferRegion(currentA.Get(), 0, upload.Get(), 0, size);
    AddTransition(commandList.Get(), currentA.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    const UINT groups = (n + 7u) / 8u;
    for (int i = 0; i < iterations; ++i)
    {
        Constants c{}; c.resolution = n; c.radiusPixels = static_cast<float>(radius); c.iterations = static_cast<UINT>(i & 1); c.strength = std::clamp(settings.strength, 0.0f, 1.0f);
        commandList->SetComputeRoot32BitConstants(0, 12, &c, 0);
        commandList->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
        D3D12_GPU_DESCRIPTOR_HANDLE uavTable = heap->GetGPUDescriptorHandleForHeapStart(); uavTable.ptr += static_cast<UINT64>(inc) * 4;
        commandList->SetComputeRootDescriptorTable(2, uavTable);
        commandList->SetPipelineState(g_blurHorizontalPso.Get());
        commandList->Dispatch(groups, groups, 1);
        AddUavBarrier(commandList.Get(), scratch.Get());
        commandList->SetPipelineState(g_blurVerticalPso.Get());
        commandList->Dispatch(groups, groups, 1);
        AddUavBarrier(commandList.Get(), (i & 1) ? currentA.Get() : currentB.Get());
    }
    ID3D12Resource* finalResource = (iterations & 1) ? currentB.Get() : currentA.Get();
    AddTransition(commandList.Get(), finalResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readback.Get(), 0, finalResource, 0, size);
    ExecuteAndWait(commandList);
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(size)};
    ThrowIfFailed(readback->Map(0, &range, &mapped), "Map Mask Blur readback failed");
    const float* values = static_cast<const float*>(mapped);
    grid.values.assign(values, values + cellCount);
    readback->Unmap(0, nullptr);
    g_status = "Mask Blur GPU Compute evaluated";
    return true;
}

bool RunHeightmapFromMaskCompute(rock::HeightfieldGrid& grid, const rock::MaskGrid& mask, const rock::HeightmapFromMaskSettings& settings, int resolution, float terrainSizeMeters, std::string* error)
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        auto request = std::make_shared<Request>();
        request->kind = Request::Kind::HeightmapFromMask;
        request->mask = mask;
        request->heightmapFromMask = settings;
        request->resolution = resolution;
        request->terrainSizeMeters = terrainSizeMeters;
        std::future<RequestResult> future = request->promise.get_future();
        {
            std::lock_guard<std::mutex> lock(g_requestMutex);
            g_pendingRequests.push_back(request);
        }
        g_status = "Heightmap From Mask GPU Compute queued on main thread";
        RequestResult result = future.get();
        if (!result.success)
        {
            if (error) *error = result.error;
            return false;
        }
        grid = std::move(result.heightfield);
        return true;
    }

    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsurePipeline(error) || !IsGpuComputeContextReady(g_context.gpu)) { return false; }
    const UINT n = static_cast<UINT>(std::clamp(resolution, 2, 2048));
    const UINT sourceN = static_cast<UINT>(std::clamp(mask.resolution, 2, 2048));
    const UINT64 cellCount = static_cast<UINT64>(n) * n;
    const UINT64 sourceCount = static_cast<UINT64>(sourceN) * sourceN;
    if (mask.values.size() < sourceCount) { return false; }
    const UINT64 outSize = cellCount * sizeof(float);
    auto upload = CreateUploadBuffer(mask.values);
    auto heights = CreateDefaultBuffer(outSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto outMask = CreateDefaultBuffer(outSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto heightsReadback = CreateReadbackBuffer(outSize);
    auto maskReadback = CreateReadbackBuffer(outSize);
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(7);
    ComPtr<ID3D12DescriptorHeap> heap;
    ThrowIfFailed(g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), "Create Heightmap From Mask descriptor heap failed");
    const UINT inc = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    CreateSrv(upload.Get(), static_cast<UINT>(sourceCount), sizeof(float), h); h.ptr += inc;
    CreateSrv(nullptr, 1, sizeof(float), h); h.ptr += inc;
    CreateSrv(nullptr, 1, sizeof(float), h); h.ptr += inc;
    CreateSrv(nullptr, 1, sizeof(float), h); h.ptr += inc;
    CreateUav(heights.Get(), static_cast<UINT>(cellCount), sizeof(float), h); h.ptr += inc;
    CreateUav(outMask.Get(), static_cast<UINT>(cellCount), sizeof(float), h);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Mask Utility allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Mask Utility command list failed");
    Constants c{}; c.resolution = n; c.sourceResolution = sourceN; c.gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    c.invert = settings.invert ? 1.0f : 0.0f; c.heightMeters = settings.heightMeters; c.baseHeightMeters = settings.baseHeightMeters;
    ID3D12DescriptorHeap* heaps[] = {heap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetPipelineState(g_heightmapFromMaskPso.Get());
    commandList->SetComputeRoot32BitConstants(0, 12, &c, 0);
    commandList->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
    D3D12_GPU_DESCRIPTOR_HANDLE uavTable = heap->GetGPUDescriptorHandleForHeapStart(); uavTable.ptr += static_cast<UINT64>(inc) * 4;
    commandList->SetComputeRootDescriptorTable(2, uavTable);
    const UINT groups = (n + 7u) / 8u;
    commandList->Dispatch(groups, groups, 1);
    AddTransition(commandList.Get(), heights.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    AddTransition(commandList.Get(), outMask.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(heightsReadback.Get(), 0, heights.Get(), 0, outSize);
    commandList->CopyBufferRegion(maskReadback.Get(), 0, outMask.Get(), 0, outSize);
    ExecuteAndWait(commandList);
    void* mapped = nullptr;
    const D3D12_RANGE range{0, static_cast<SIZE_T>(outSize)};
    ThrowIfFailed(heightsReadback->Map(0, &range, &mapped), "Map Heightmap From Mask heights readback failed");
    const float* values = static_cast<const float*>(mapped);
    grid.resolution = n; grid.terrainSizeMeters = std::max(1.0f, terrainSizeMeters);
    grid.heights.assign(values, values + cellCount);
    heightsReadback->Unmap(0, nullptr);
    ThrowIfFailed(maskReadback->Map(0, &range, &mapped), "Map Heightmap From Mask mask readback failed");
    values = static_cast<const float*>(mapped);
    grid.mask.assign(values, values + cellCount);
    maskReadback->Unmap(0, nullptr);
    grid.uniqueMask.assign(static_cast<size_t>(cellCount), 0.0f);
    grid.deposits.assign(static_cast<size_t>(cellCount), 0.0f);
    grid.flows.assign(static_cast<size_t>(cellCount), 0.0f);
    grid.age.assign(static_cast<size_t>(cellCount), 0.0f);
    g_status = "Heightmap From Mask GPU Compute evaluated";
    return true;
}

void ProcessPendingMaskUtilityGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<Request>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<Request>& request : requests)
    {
        RequestResult result;
        switch (request->kind)
        {
        case Request::Kind::MaskPath:
            result.mask = std::move(request->mask);
            result.success = RunMaskPathCompute(result.mask, request->path, request->maskPath, request->terrainSizeMeters, &result.error);
            break;
        case Request::Kind::MaskBlur:
            result.mask = std::move(request->mask);
            result.success = RunMaskBlurCompute(result.mask, request->maskBlur, request->terrainSizeMeters, &result.error);
            break;
        case Request::Kind::HeightmapFromMask:
            result.success = RunHeightmapFromMaskCompute(result.heightfield, request->mask, request->heightmapFromMask, request->resolution, request->terrainSizeMeters, &result.error);
            break;
        }
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
