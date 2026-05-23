#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct RockComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetRockComputeContext(RockComputeContext context);
void ResetRockComputeResources();
const std::string& RockComputeStatus();

bool RunRockCompute(rock::HeightfieldGrid& grid,
                    const rock::RockSettings& settings,
                    std::string* error);
void ProcessPendingRockGpuRequests();

} // namespace terrain::gpu
