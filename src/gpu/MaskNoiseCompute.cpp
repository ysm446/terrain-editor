#include "MaskNoiseCompute.h"

#include "../D3D12Utils.h"

#include <algorithm>
#include <future>
#include <memory>
#include <mutex>
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

MaskNoiseComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_pso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Mask Noise GPU Compute not initialized";

struct MaskNoiseShaderConstants
{
    UINT  resolution;
    UINT  octaves;
    INT   seed;
    float frequency;
    float lacunarity;
    float persistence;
};
static_assert(sizeof(MaskNoiseShaderConstants) == 6 * sizeof(UINT), "MaskNoiseShaderConstants must be 6 DWORDs");

struct MaskNoiseGpuRequestResult
{
    bool success = false;
    rock::MaskGrid grid;
    std::string error;
};

struct MaskNoiseGpuRequest
{
    rock::MaskNoiseSettings settings;
    int resolution = 0;
    std::promise<MaskNoiseGpuRequestResult> promise;
};

std::vector<std::shared_ptr<MaskNoiseGpuRequest>> g_pendingRequests;

bool EnsureMaskNoiseComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature && g_pso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "Mask Noise GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 6;
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
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Mask Noise root sig failed";
        g_status = "Mask Noise GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();
    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                           "CSGenerate", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Mask Noise shader failed";
        g_status = "Mask Noise shader compile failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    HRESULT psoHr = g_context.gpu.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_pso));
    if (FAILED(psoHr))
    {
        if (error) *error = "Create Mask Noise PSO failed";
        g_status = "Mask Noise PSO failed";
        return false;
    }

    g_ready = true;
    g_status = "Mask Noise GPU Compute dispatch ready";
    return true;
}

bool RunMaskNoiseComputeImmediate(rock::MaskGrid& grid, const rock::MaskNoiseSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureMaskNoiseComputePipeline(error))
    {
        return false;
    }
    if (!IsGpuComputeContextReady(g_context.gpu))
    {
        if (error) *error = "D3D12 queue or fence is not available";
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    if (resolution < 2)
    {
        if (error) *error = "Invalid resolution for Mask Noise GPU Compute";
        return false;
    }
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    const UINT64 bufferSize = cellCount * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC gpuDesc = BufferResourceDesc(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC cpuDesc = BufferResourceDesc(bufferSize);

    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> readback;
    HRESULT hr = g_context.gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output));
    if (FAILED(hr)) { if (error) *error = "Create Mask Noise output buffer failed"; return false; }
    hr = g_context.gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) { if (error) *error = "Create Mask Noise readback buffer failed"; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(1);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Mask Noise descriptor heap failed"; return false; }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = static_cast<UINT>(cellCount);
    uavDesc.Buffer.StructureByteStride = sizeof(float);
    g_context.gpu.device->CreateUnorderedAccessView(output.Get(), nullptr, &uavDesc, descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Mask Noise command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Mask Noise command list failed");

    MaskNoiseShaderConstants constants{};
    constants.resolution = resolution;
    constants.octaves = static_cast<UINT>(std::clamp(settings.octaves, 1, 12));
    constants.seed = settings.seed;
    constants.frequency = std::max(settings.frequency, 0.0f);
    constants.lacunarity = std::max(settings.lacunarity, 0.0f);
    constants.persistence = std::clamp(settings.persistence, 0.0f, 1.0f);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetPipelineState(g_pso.Get());
    commandList->SetComputeRoot32BitConstants(0, 6, &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    const UINT groupCount = (resolution + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = output.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopy);
    commandList->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, bufferSize);
    ThrowIfFailed(commandList->Close(), "Close Mask Noise command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal Mask Noise fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bufferSize)};
    ThrowIfFailed(readback->Map(0, &readRange, &mapped), "Map Mask Noise readback failed");
    const float* values = static_cast<const float*>(mapped);
    grid.values.assign(values, values + cellCount);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readback->Unmap(0, &emptyWriteRange);

    g_status = "Mask Noise GPU Compute evaluated";
    return true;
}

} // namespace

void SetMaskNoiseComputeContext(MaskNoiseComputeContext context)
{
    g_context = std::move(context);
}

void ResetMaskNoiseComputeResources()
{
    g_pso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& MaskNoiseComputeStatus()
{
    return g_status;
}

bool RunMaskNoiseCompute(rock::MaskGrid& grid, const rock::MaskNoiseSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunMaskNoiseComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<MaskNoiseGpuRequest>();
    request->settings = settings;
    request->resolution = grid.resolution;
    std::future<MaskNoiseGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "Mask Noise GPU Compute queued on main thread";

    MaskNoiseGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingMaskNoiseGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<MaskNoiseGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<MaskNoiseGpuRequest>& request : requests)
    {
        MaskNoiseGpuRequestResult result;
        result.grid.resolution = request->resolution;
        result.grid.values.assign(static_cast<size_t>(request->resolution) * static_cast<size_t>(request->resolution), 0.0f);
        result.success = RunMaskNoiseComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
