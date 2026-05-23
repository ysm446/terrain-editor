#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct SedimentComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetSedimentComputeContext(SedimentComputeContext context);
void ResetSedimentComputeResources();
const std::string& SedimentComputeStatus();

bool RunSedimentCompute(rock::HeightfieldGrid& grid,
                        const rock::SedimentSettings& settings,
                        std::string* error);
void ProcessPendingSedimentGpuRequests();

} // namespace terrain::gpu
