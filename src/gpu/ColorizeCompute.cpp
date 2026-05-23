#include "ColorizeCompute.h"

#include "../D3D12Utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
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

ColorizeComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Colorize GPU Compute not initialized";

struct ColorizeShaderConstants
{
    UINT resolution;
    UINT cellCount;
    UINT stopCount;
    UINT hasMask;
    UINT hasBaseColor;
};
static_assert(sizeof(ColorizeShaderConstants) == 5 * sizeof(UINT), "ColorizeShaderConstants must be 5 DWORDs");

struct ColorizeGpuRequestResult
{
    bool success = false;
    rock::ColorGrid grid;
    std::string error;
};

struct ColorizeGpuRequest
{
    rock::ColorizeSettings settings;
    rock::MaskGrid gradientMask;
    rock::MaskGrid mask;
    rock::ColorGrid baseColor;
    bool hasMask = false;
    bool hasBaseColor = false;
    std::promise<ColorizeGpuRequestResult> promise;
};

std::vector<std::shared_ptr<ColorizeGpuRequest>> g_pendingRequests;

ComPtr<ID3D12Resource> CreateUploadBuffer(const void* data, UINT64 byteSize, const char* message)
{
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferResourceDesc(std::max<UINT64>(byteSize, 1));
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(g_context.gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)), message);
    if (byteSize > 0)
    {
        void* mapped = nullptr;
        ThrowIfFailed(buffer->Map(0, nullptr, &mapped), "Map upload buffer failed");
        std::memcpy(mapped, data, static_cast<size_t>(byteSize));
        buffer->Unmap(0, nullptr);
    }
    return buffer;
}

bool EnsureColorizeComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature && g_pso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "Colorize GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 5;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_context.gpu.device,
                                             rsDesc,
                                             g_rootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Colorize root sig failed";
        g_status = "Colorize GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();
    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    const HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                 "CSColorize", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Colorize shader failed";
        g_status = "Colorize shader compile failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    hr = g_context.gpu.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_pso));
    if (FAILED(hr))
    {
        if (error) *error = "Create Colorize PSO failed";
        g_status = "Colorize PSO failed";
        return false;
    }

    g_ready = true;
    g_status = "Colorize GPU Compute dispatch ready";
    return true;
}

bool RunColorizeComputeImmediate(rock::ColorGrid& grid, const rock::ColorizeSettings& settings, const rock::MaskGrid& gradientMask, const rock::MaskGrid* mask, const rock::ColorGrid* baseColor, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureColorizeComputePipeline(error))
    {
        return false;
    }
    if (!IsGpuComputeContextReady(g_context.gpu))
    {
        if (error) *error = "D3D12 queue or fence is not available";
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(gradientMask.resolution, 0, 4096));
    if (resolution < 2 || gradientMask.values.size() < static_cast<size_t>(resolution) * static_cast<size_t>(resolution))
    {
        if (error) *error = "Invalid Gradient Mask for Colorize GPU Compute";
        return false;
    }
    const bool hasMask = mask != nullptr &&
        mask->resolution == static_cast<int>(resolution) &&
        mask->values.size() >= static_cast<size_t>(resolution) * static_cast<size_t>(resolution);
    const bool hasBaseColor = baseColor != nullptr &&
        baseColor->resolution > 0 &&
        baseColor->pixels.size() >= static_cast<size_t>(baseColor->resolution) * static_cast<size_t>(baseColor->resolution) * 4u;
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    const UINT64 maskByteSize = cellCount * sizeof(float);
    std::vector<rock::ColorStop> sourceStops = settings.stops;
    if (sourceStops.empty())
    {
        sourceStops = {
            {0.0f, 0.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
        };
    }
    const UINT stopCount = static_cast<UINT>(std::min<size_t>(sourceStops.size(), 256));
    struct GpuStop { float position; float r; float g; float b; };
    std::vector<GpuStop> stops;
    stops.reserve(stopCount);
    for (UINT i = 0; i < stopCount; ++i)
    {
        const rock::ColorStop& s = sourceStops[i];
        stops.push_back({std::clamp(s.position, 0.0f, 1.0f), s.r, s.g, s.b});
    }

    std::vector<uint32_t> basePixels(static_cast<size_t>(cellCount), 0xff000000u);
    if (hasBaseColor)
    {
        for (UINT y = 0; y < resolution; ++y)
        {
            const float v = resolution > 1 ? static_cast<float>(y) / static_cast<float>(resolution - 1u) : 0.0f;
            const int baseY = std::clamp(static_cast<int>(std::round(v * static_cast<float>(baseColor->resolution - 1))), 0, baseColor->resolution - 1);
            for (UINT x = 0; x < resolution; ++x)
            {
                const float u = resolution > 1 ? static_cast<float>(x) / static_cast<float>(resolution - 1u) : 0.0f;
                const int baseX = std::clamp(static_cast<int>(std::round(u * static_cast<float>(baseColor->resolution - 1))), 0, baseColor->resolution - 1);
                const size_t src = (static_cast<size_t>(baseY) * static_cast<size_t>(baseColor->resolution) + static_cast<size_t>(baseX)) * 4u;
                const uint32_t r = baseColor->pixels[src + 0u];
                const uint32_t g = baseColor->pixels[src + 1u];
                const uint32_t b = baseColor->pixels[src + 2u];
                const uint32_t a = baseColor->pixels[src + 3u];
                basePixels[static_cast<size_t>(y) * static_cast<size_t>(resolution) + static_cast<size_t>(x)] =
                    (r & 255u) | ((g & 255u) << 8) | ((b & 255u) << 16) | ((a & 255u) << 24);
            }
        }
    }

    const UINT64 stopByteSize = std::max<UINT64>(static_cast<UINT64>(stops.size()) * sizeof(GpuStop), sizeof(GpuStop));
    const UINT64 outputByteSize = cellCount * sizeof(uint32_t);
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC outputGpuDesc = BufferResourceDesc(outputByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC outputCpuDesc = BufferResourceDesc(outputByteSize);

    ComPtr<ID3D12Resource> gradientUpload = CreateUploadBuffer(gradientMask.values.data(), maskByteSize, "Create Colorize gradient upload failed");
    ComPtr<ID3D12Resource> maskUpload = CreateUploadBuffer(hasMask ? mask->values.data() : gradientMask.values.data(), maskByteSize, "Create Colorize mask upload failed");
    ComPtr<ID3D12Resource> stopsUpload = CreateUploadBuffer(stops.data(), stopByteSize, "Create Colorize stops upload failed");
    ComPtr<ID3D12Resource> baseUpload = CreateUploadBuffer(basePixels.data(), outputByteSize, "Create Colorize base upload failed");
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> readback;
    HRESULT hr = g_context.gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outputGpuDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output));
    if (FAILED(hr)) { if (error) *error = "Create Colorize output buffer failed"; return false; }
    hr = g_context.gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &outputCpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) { if (error) *error = "Create Colorize readback buffer failed"; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(5);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Colorize descriptor heap failed"; return false; }
    const UINT descriptorSize = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpuHandle = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize;
        return handle;
    };
    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize;
        return handle;
    };

    auto createFloatSrv = [&](ID3D12Resource* resource, UINT64 elementCount, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.NumElements = static_cast<UINT>(elementCount);
        srvDesc.Buffer.StructureByteStride = sizeof(float);
        g_context.gpu.device->CreateShaderResourceView(resource, &srvDesc, dst);
    };
    createFloatSrv(gradientUpload.Get(), cellCount, cpuHandle(0));
    createFloatSrv(maskUpload.Get(), cellCount, cpuHandle(1));
    D3D12_SHADER_RESOURCE_VIEW_DESC stopsSrv{};
    stopsSrv.Format = DXGI_FORMAT_UNKNOWN;
    stopsSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    stopsSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    stopsSrv.Buffer.NumElements = stopCount;
    stopsSrv.Buffer.StructureByteStride = sizeof(GpuStop);
    g_context.gpu.device->CreateShaderResourceView(stopsUpload.Get(), &stopsSrv, cpuHandle(2));
    D3D12_SHADER_RESOURCE_VIEW_DESC baseSrv{};
    baseSrv.Format = DXGI_FORMAT_UNKNOWN;
    baseSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    baseSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    baseSrv.Buffer.NumElements = static_cast<UINT>(cellCount);
    baseSrv.Buffer.StructureByteStride = sizeof(uint32_t);
    g_context.gpu.device->CreateShaderResourceView(baseUpload.Get(), &baseSrv, cpuHandle(3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav{};
    outputUav.Format = DXGI_FORMAT_UNKNOWN;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    outputUav.Buffer.NumElements = static_cast<UINT>(cellCount);
    outputUav.Buffer.StructureByteStride = sizeof(uint32_t);
    g_context.gpu.device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav, cpuHandle(4));

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Colorize command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Colorize command list failed");

    ColorizeShaderConstants constants{};
    constants.resolution = resolution;
    constants.cellCount = static_cast<UINT>(cellCount);
    constants.stopCount = stopCount;
    constants.hasMask = hasMask ? 1u : 0u;
    constants.hasBaseColor = hasBaseColor ? 1u : 0u;

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetPipelineState(g_pso.Get());
    commandList->SetComputeRoot32BitConstants(0, 5, &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(0));
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(4));
    const UINT groupCount = (resolution + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = output.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopy);
    commandList->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outputByteSize);
    ThrowIfFailed(commandList->Close(), "Close Colorize command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal Colorize fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(outputByteSize)};
    ThrowIfFailed(readback->Map(0, &readRange, &mapped), "Map Colorize readback failed");
    const uint32_t* packed = static_cast<const uint32_t*>(mapped);
    grid.resolution = static_cast<int>(resolution);
    grid.pixels.resize(static_cast<size_t>(cellCount) * 4u);
    for (UINT64 i = 0; i < cellCount; ++i)
    {
        const uint32_t p = packed[i];
        grid.pixels[i * 4u + 0u] = static_cast<uint8_t>(p & 0xffu);
        grid.pixels[i * 4u + 1u] = static_cast<uint8_t>((p >> 8) & 0xffu);
        grid.pixels[i * 4u + 2u] = static_cast<uint8_t>((p >> 16) & 0xffu);
        grid.pixels[i * 4u + 3u] = static_cast<uint8_t>((p >> 24) & 0xffu);
    }
    const D3D12_RANGE emptyWriteRange{0, 0};
    readback->Unmap(0, &emptyWriteRange);

    g_status = "Colorize GPU Compute evaluated";
    return true;
}

} // namespace

void SetColorizeComputeContext(ColorizeComputeContext context)
{
    g_context = std::move(context);
}

void ResetColorizeComputeResources()
{
    g_pso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& ColorizeComputeStatus()
{
    return g_status;
}

bool RunColorizeCompute(rock::ColorGrid& grid, const rock::ColorizeSettings& settings, const rock::MaskGrid& gradientMask, const rock::MaskGrid* mask, const rock::ColorGrid* baseColor, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunColorizeComputeImmediate(grid, settings, gradientMask, mask, baseColor, error);
    }

    auto request = std::make_shared<ColorizeGpuRequest>();
    request->settings = settings;
    request->gradientMask = gradientMask;
    request->hasMask = mask != nullptr;
    if (mask != nullptr)
    {
        request->mask = *mask;
    }
    request->hasBaseColor = baseColor != nullptr;
    if (baseColor != nullptr)
    {
        request->baseColor = *baseColor;
    }
    std::future<ColorizeGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "Colorize GPU Compute queued on main thread";

    ColorizeGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingColorizeGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<ColorizeGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<ColorizeGpuRequest>& request : requests)
    {
        ColorizeGpuRequestResult result;
        result.success = RunColorizeComputeImmediate(
            result.grid,
            request->settings,
            request->gradientMask,
            request->hasMask ? &request->mask : nullptr,
            request->hasBaseColor ? &request->baseColor : nullptr,
            &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
