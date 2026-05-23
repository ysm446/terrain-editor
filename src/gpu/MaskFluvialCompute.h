#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct MaskFluvialComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetMaskFluvialComputeContext(MaskFluvialComputeContext context);
void ResetMaskFluvialComputeResources();
const std::string& MaskFluvialComputeStatus();

bool RunMaskFluvialCompute(rock::HeightfieldGrid& grid,
                           const rock::MaskFluvialSettings& settings,
                           std::string* error);
void ProcessPendingMaskFluvialGpuRequests();

} // namespace terrain::gpu
