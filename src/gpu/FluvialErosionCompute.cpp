#include "FluvialErosionCompute.h"

#include "../D3D12Utils.h"
#include "../evaluation/ParticleErosionCommon.h"

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

FluvialErosionComputeContext g_context;
ComPtr<ID3D12RootSignature> g_rootSignature;
ComPtr<ID3D12PipelineState> g_clearLevelPso;
ComPtr<ID3D12PipelineState> g_clearIterPso;
ComPtr<ID3D12PipelineState> g_tracePso;
ComPtr<ID3D12PipelineState> g_applyPso;
ComPtr<ID3D12PipelineState> g_copyToSrcPso;
ComPtr<ID3D12PipelineState> g_upsamplePso;
bool g_ready = false;
std::mutex g_computeMutex;
std::mutex g_requestMutex;
std::string g_status = "Fluvial Erosion GPU Compute not initialized";

// Fixed-point counts per metre. Must match kFixedScale in
// shaders/fluvial_erosion_compute.hlsl.
constexpr float kFixedScale = 4096.0f;

// Mirrors the cbuffer in shaders/fluvial_erosion_compute.hlsl.
struct FluvialErosionShaderConstants
{
    UINT n;
    UINT srcN;
    UINT particleCount;
    UINT steps;

    INT seed;
    INT levelSeed;
    INT iter;
    float cellSize;

    float friction;
    float erodeStrength;
    float channeling;
    float sedimentVelocity;

    float flowVolume;
    float tanWear;
    float tanDeposit;
    float tanMax;

    float deltaCap;
    float pad0;
    float pad1;
    float pad2;
};
static_assert(sizeof(FluvialErosionShaderConstants) == 20 * sizeof(UINT), "FluvialErosionShaderConstants must be 20 DWORDs");

struct FluvialErosionGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct FluvialErosionGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::FluvialErosionSettings settings;
    std::promise<FluvialErosionGpuRequestResult> promise;
};

std::vector<std::shared_ptr<FluvialErosionGpuRequest>> g_pendingRequests;

bool EnsureFluvialErosionComputePipeline(std::string* error)
{
    if (g_ready && g_rootSignature
        && g_clearLevelPso && g_clearIterPso && g_tracePso
        && g_applyPso && g_copyToSrcPso && g_upsamplePso)
    {
        return true;
    }
    if (!g_context.gpu.device)
    {
        if (error) *error = "D3D12 device is not available";
        g_status = "Fluvial Erosion GPU Compute unavailable";
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
    rootParams[0].Constants.Num32BitValues = 20;
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
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create Fluvial Erosion root sig failed";
        g_status = "Fluvial Erosion GPU Compute root signature failed";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();
    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(g_context.shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Fluvial Erosion shader failed";
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
            if (error) *error = "Create Fluvial Erosion PSO failed";
            return false;
        }
        return true;
    };

    struct Entry { const char* name; ComPtr<ID3D12PipelineState>* pso; };
    Entry entries[] = {
        {"CSClearLevel", &g_clearLevelPso},
        {"CSClearIter", &g_clearIterPso},
        {"CSTrace", &g_tracePso},
        {"CSApply", &g_applyPso},
        {"CSCopyToSrc", &g_copyToSrcPso},
        {"CSUpsample", &g_upsamplePso},
    };
    for (const Entry& e : entries)
    {
        ComPtr<ID3DBlob> blob;
        if (!compileEntry(e.name, blob))
        {
            g_status = std::string("Fluvial Erosion ") + e.name + " compile failed";
            return false;
        }
        if (!buildPso(blob.Get(), *e.pso))
        {
            g_status = std::string("Fluvial Erosion ") + e.name + " PSO failed";
            return false;
        }
    }

    g_ready = true;
    g_status = "Fluvial Erosion GPU Compute dispatch ready";
    return true;
}

bool RunFluvialErosionComputeImmediate(rock::HeightfieldGrid& grid, const rock::FluvialErosionSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_computeMutex);
    if (!EnsureFluvialErosionComputePipeline(error))
    {
        return false;
    }
    if (!IsGpuComputeContextReady(g_context.gpu))
    {
        if (error) *error = "D3D12 queue or fence is not available";
        return false;
    }

    const int targetN = std::clamp(grid.resolution, 0, 4096);
    const size_t targetCellCount = static_cast<size_t>(targetN) * static_cast<size_t>(targetN);
    if (targetN < 3 || grid.heights.size() < targetCellCount)
    {
        if (error) *error = "Invalid heightfield for Fluvial Erosion GPU Compute";
        return false;
    }

    // Mirror ApplyFluvialErosion / RunErosion: build the pyramid level list.
    const float feature = std::clamp(settings.featureSize, 1.0f, 256.0f);
    const int coarsest = std::clamp(static_cast<int>(std::lround(grid.terrainSizeMeters / feature)), 16, targetN);
    std::vector<int> levels;
    if (settings.useMultigrid)
    {
        for (int r = std::clamp(coarsest, 16, targetN); r < targetN; r *= 2)
        {
            levels.push_back(r);
        }
    }
    levels.push_back(targetN);
    // Match RunErosion's levelSeed assignment: single-level runs use seed 0,
    // pyramid levels use 1-based indices.
    const bool singleLevel = levels.size() <= 1;

    const UINT64 cellBytesFloat = static_cast<UINT64>(targetCellCount) * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc = BufferResourceDesc(cellBytesFloat, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc = BufferResourceDesc(cellBytesFloat);

    ComPtr<ID3D12Resource> heightsBuf, srcHeightsBuf, wearBuf, deltaHBuf, deltaWBuf, flowBuf, depositBuf;
    ComPtr<ID3D12Resource> uploadHeights, readbackHeights, readbackFlow, readbackDeposit;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_context.gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &fieldGpuDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
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

    if (!createDefault(heightsBuf, "FE heights")) return false;
    if (!createDefault(srcHeightsBuf, "FE src heights")) return false;
    if (!createDefault(wearBuf, "FE wear")) return false;
    if (!createDefault(deltaHBuf, "FE delta H")) return false;
    if (!createDefault(deltaWBuf, "FE delta W")) return false;
    if (!createDefault(flowBuf, "FE flow")) return false;
    if (!createDefault(depositBuf, "FE deposit")) return false;
    if (!createUpload(uploadHeights, "FE upload heights")) return false;
    if (!createReadback(readbackHeights, "FE readback heights")) return false;
    if (!createReadback(readbackFlow, "FE readback flow")) return false;
    if (!createReadback(readbackDeposit, "FE readback deposit")) return false;

    // Level-0 heights: resampled on the CPU exactly like RunErosion does.
    std::vector<float> level0Heights;
    const int level0N = levels[0];
    if (level0N != targetN)
    {
        level0Heights = rock::particle_erosion::ResampleHeightsBilinear(grid.heights, targetN, level0N);
    }
    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map FE heights upload failed");
    const std::vector<float>& uploadSource = level0Heights.empty() ? grid.heights : level0Heights;
    std::memcpy(mapped, uploadSource.data(), static_cast<size_t>(level0N) * static_cast<size_t>(level0N) * sizeof(float));
    uploadHeights->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 7;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = ShaderVisibleCbvSrvUavDescriptorHeapDesc(kDescriptorCount);
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_context.gpu.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create FE descriptor heap failed"; return false; }

    const UINT descriptorSize = g_context.gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto createUav = [&](ID3D12Resource* res, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = static_cast<UINT>(targetCellCount);
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * descriptorSize;
        g_context.gpu.device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    createUav(heightsBuf.Get(), 0);
    createUav(srcHeightsBuf.Get(), 1);
    createUav(wearBuf.Get(), 2);
    createUav(deltaHBuf.Get(), 3);
    createUav(deltaWBuf.Get(), 4);
    createUav(flowBuf.Get(), 5);
    createUav(depositBuf.Get(), 6);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_context.gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create FE command allocator failed");
    ThrowIfFailed(g_context.gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create FE command list failed");

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

    transition(heightsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(heightsBuf.Get(), 0, uploadHeights.Get(), 0,
                                  static_cast<UINT64>(level0N) * static_cast<UINT64>(level0N) * sizeof(float));
    transition(heightsBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    // Settings shared by every level (mirrors RunFluvialLevel).
    const int iterations = std::clamp(settings.simulationIterations, 0, 200);
    const float ageGain = std::clamp(settings.geologicalAge, 0.0f, 20.0f) / 20.0f;
    constexpr float kPi = 3.14159265358979323846f;

    FluvialErosionShaderConstants constants{};
    constants.seed = settings.seed;
    constants.friction = std::clamp(settings.friction, 0.0f, 0.99f);
    constants.erodeStrength = std::clamp(settings.erosionStrength, 0.0f, 1.0f) * std::max(0.0f, ageGain);
    constants.channeling = std::clamp(settings.channeling, 0.0f, 1.0f);
    constants.sedimentVelocity = std::clamp(settings.sedimentVelocity, 0.01f, 2.0f);
    constants.flowVolume = std::clamp(settings.flowVolume, 0.0f, 1.0f);
    constants.tanWear = std::tan(std::clamp(settings.wearAngleDeg, 0.0f, 89.0f) * kPi / 180.0f);
    constants.tanDeposit = std::tan(std::clamp(settings.depositAngleDeg, 0.0f, 89.0f) * kPi / 180.0f);
    constants.tanMax = std::tan(std::clamp(settings.maxErosionAngleDeg, 0.0f, 89.0f) * kPi / 180.0f);

    auto setConstants = [&]() {
        commandList->SetComputeRoot32BitConstants(0, 20, &constants, 0);
    };
    auto dispatchCells = [&](ID3D12PipelineState* pso, int n) {
        commandList->SetPipelineState(pso);
        const UINT groups = (static_cast<UINT>(n) * static_cast<UINT>(n) + 255u) / 256u;
        commandList->Dispatch(groups, 1, 1);
        uavBarrier();
    };

    for (size_t levelIndex = 0; levelIndex < levels.size(); ++levelIndex)
    {
        const int n = levels[levelIndex];
        if (n < 3)
        {
            continue;
        }
        const float cellSize = grid.terrainSizeMeters / static_cast<float>(std::max(1, n - 1));
        const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);

        // Per-level particle density (mirrors RunFluvialLevel).
        const float fineFactor = static_cast<float>(n) / static_cast<float>(std::max(1, targetN));
        const float density = std::clamp(settings.erosionGranularity / 100.0f *
                                         (1.0f + std::clamp(settings.smallChannelInfluence, 0.0f, 1.0f) * fineFactor),
                                         0.0f, 1.0f);

        constants.n = static_cast<UINT>(n);
        constants.levelSeed = singleLevel ? 0 : static_cast<INT>(levelIndex + 1);
        constants.particleCount = static_cast<UINT>(std::clamp(static_cast<int>(static_cast<float>(cellCount) * density), 500, 60000));
        constants.steps = static_cast<UINT>(std::clamp(static_cast<int>(std::lround(settings.channelLength / std::max(cellSize, 1e-3f))), 1, 4096));
        constants.cellSize = cellSize;
        constants.deltaCap = 0.12f * cellSize;

        if (levelIndex > 0)
        {
            // Upsample the previous level's heights into this level's grid.
            constants.srcN = static_cast<UINT>(levels[levelIndex - 1]);
            setConstants();
            dispatchCells(g_copyToSrcPso.Get(), levels[levelIndex - 1]);
            dispatchCells(g_upsamplePso.Get(), n);
        }

        setConstants();
        dispatchCells(g_clearLevelPso.Get(), n);

        for (int it = 0; it < iterations; ++it)
        {
            constants.iter = it;
            setConstants();
            dispatchCells(g_clearIterPso.Get(), n);
            commandList->SetPipelineState(g_tracePso.Get());
            commandList->Dispatch((constants.particleCount + 63u) / 64u, 1, 1);
            uavBarrier();
            dispatchCells(g_applyPso.Get(), n);
        }
    }

    transition(heightsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, heightsBuf.Get(), 0, cellBytesFloat);
    transition(flowBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readbackFlow.Get(), 0, flowBuf.Get(), 0, cellBytesFloat);
    transition(depositBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readbackDeposit.Get(), 0, depositBuf.Get(), 0, cellBytesFloat);
    ThrowIfFailed(commandList->Close(), "Close FE command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_context.gpu.commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++(*g_context.gpu.fenceLastSignaledValue);
    ThrowIfFailed(g_context.gpu.commandQueue->Signal(g_context.gpu.fence, fenceValue), "Signal FE fence failed");
    g_context.gpu.waitForFenceValue(fenceValue);

    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(cellBytesFloat)};
    const D3D12_RANGE emptyWriteRange{0, 0};

    void* mappedHeights = nullptr;
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map FE readback heights failed");
    grid.heights.assign(targetCellCount, 0.0f);
    std::memcpy(grid.heights.data(), mappedHeights, cellBytesFloat);
    readbackHeights->Unmap(0, &emptyWriteRange);

    void* mappedFlow = nullptr;
    ThrowIfFailed(readbackFlow->Map(0, &readRange, &mappedFlow), "Map FE readback flow failed");
    const UINT* flowCounts = static_cast<const UINT*>(mappedFlow);
    grid.flows.assign(targetCellCount, 0.0f);
    for (size_t i = 0; i < targetCellCount; ++i)
    {
        grid.flows[i] = static_cast<float>(flowCounts[i]);
    }
    readbackFlow->Unmap(0, &emptyWriteRange);

    void* mappedDeposit = nullptr;
    ThrowIfFailed(readbackDeposit->Map(0, &readRange, &mappedDeposit), "Map FE readback deposit failed");
    const INT* depositFixed = static_cast<const INT*>(mappedDeposit);
    grid.deposits.assign(targetCellCount, 0.0f);
    for (size_t i = 0; i < targetCellCount; ++i)
    {
        grid.deposits[i] = static_cast<float>(depositFixed[i]) / kFixedScale;
    }
    readbackDeposit->Unmap(0, &emptyWriteRange);

    // Same post-pass as the CPU backend: log-compress flows, clear mask/age,
    // normalize the auxiliary fields.
    rock::particle_erosion::FinalizeLevel(grid, targetCellCount);

    g_status = "Fluvial Erosion GPU Compute evaluated";
    return true;
}

} // namespace

void SetFluvialErosionComputeContext(FluvialErosionComputeContext context)
{
    g_context = std::move(context);
}

void ResetFluvialErosionComputeResources()
{
    g_clearLevelPso.Reset();
    g_clearIterPso.Reset();
    g_tracePso.Reset();
    g_applyPso.Reset();
    g_copyToSrcPso.Reset();
    g_upsamplePso.Reset();
    g_rootSignature.Reset();
    g_ready = false;
}

const std::string& FluvialErosionComputeStatus()
{
    return g_status;
}

bool RunFluvialErosionCompute(rock::HeightfieldGrid& grid, const rock::FluvialErosionSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_context.gpu.mainThreadId)
    {
        return RunFluvialErosionComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<FluvialErosionGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<FluvialErosionGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        g_pendingRequests.push_back(request);
    }
    g_status = "Fluvial Erosion GPU Compute queued on main thread";

    FluvialErosionGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingFluvialErosionGpuRequests()
{
    if (std::this_thread::get_id() != g_context.gpu.mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<FluvialErosionGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_requestMutex);
        requests.swap(g_pendingRequests);
    }

    for (const std::shared_ptr<FluvialErosionGpuRequest>& request : requests)
    {
        FluvialErosionGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunFluvialErosionComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

} // namespace terrain::gpu
