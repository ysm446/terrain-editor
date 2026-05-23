#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <d3d12.h>

#include "../node_graph.h"

namespace terrain::gpu
{

struct MaskNoiseComputeContext
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64* fenceLastSignaledValue = nullptr;
    std::thread::id mainThreadId{};
    std::filesystem::path shaderPath;
    std::function<void(UINT64)> waitForFenceValue;
};

void SetMaskNoiseComputeContext(MaskNoiseComputeContext context);
void ResetMaskNoiseComputeResources();
const std::string& MaskNoiseComputeStatus();

bool RunMaskNoiseCompute(rock::MaskGrid& grid,
                         const rock::MaskNoiseSettings& settings,
                         std::string* error);
void ProcessPendingMaskNoiseGpuRequests();

} // namespace terrain::gpu
