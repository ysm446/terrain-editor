#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct ScatterComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetScatterComputeContext(ScatterComputeContext context);
void ResetScatterComputeResources();
const std::string& ScatterComputeStatus();

bool RunScatterCompute(rock::HeightfieldGrid& grid,
                       const rock::ScatterSettings& settings,
                       std::string* error);
void ProcessPendingScatterGpuRequests();

} // namespace terrain::gpu
