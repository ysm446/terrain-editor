#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct MaskNoiseComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetMaskNoiseComputeContext(MaskNoiseComputeContext context);
void ResetMaskNoiseComputeResources();
const std::string& MaskNoiseComputeStatus();

bool RunMaskNoiseCompute(rock::MaskGrid& grid,
                         const rock::MaskNoiseSettings& settings,
                         std::string* error);
void ProcessPendingMaskNoiseGpuRequests();

} // namespace terrain::gpu
