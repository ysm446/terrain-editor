#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct FluvialErosionComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetFluvialErosionComputeContext(FluvialErosionComputeContext context);
void ResetFluvialErosionComputeResources();
const std::string& FluvialErosionComputeStatus();

bool RunFluvialErosionCompute(rock::HeightfieldGrid& grid,
                              const rock::FluvialErosionSettings& settings,
                              std::string* error);
void ProcessPendingFluvialErosionGpuRequests();

} // namespace terrain::gpu
