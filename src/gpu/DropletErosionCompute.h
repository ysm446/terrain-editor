#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct DropletErosionComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetDropletErosionComputeContext(DropletErosionComputeContext context);
void ResetDropletErosionComputeResources();
const std::string& DropletErosionComputeStatus();

bool RunDropletErosionCompute(rock::HeightfieldGrid& grid,
                              const rock::DropletErosionSettings& settings,
                              std::string* error);
void ProcessPendingDropletErosionGpuRequests();

} // namespace terrain::gpu
