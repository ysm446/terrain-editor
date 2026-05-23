#pragma once

#include "GpuComputeContext.h"

#include "../node_graph.h"

#include <filesystem>
#include <string>

namespace terrain::gpu
{

struct MseComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetMseComputeContext(MseComputeContext context);
void ResetMseComputeResources();
const std::string& MseComputeStatus();

bool RunMseComputeGrid(rock::HeightfieldGrid& grid, const rock::MultiScaleErosionSettings& settings, std::string* error);
void ProcessPendingMseGpuRequests();

} // namespace terrain::gpu
