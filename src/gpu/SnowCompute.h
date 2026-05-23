#pragma once

#include "GpuComputeContext.h"

#include "../node_graph.h"

#include <filesystem>
#include <string>

namespace terrain::gpu
{

struct SnowComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetSnowComputeContext(SnowComputeContext context);
void ResetSnowComputeResources();
const std::string& SnowComputeStatus();

bool RunSnowCompute(rock::HeightfieldGrid& grid, const rock::SnowSettings& settings, std::string* error);
void ProcessPendingSnowGpuRequests();

} // namespace terrain::gpu
