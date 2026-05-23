#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct ColorizeComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetColorizeComputeContext(ColorizeComputeContext context);
void ResetColorizeComputeResources();
const std::string& ColorizeComputeStatus();

bool RunColorizeCompute(rock::ColorGrid& grid,
                        const rock::ColorizeSettings& settings,
                        const rock::MaskGrid& gradientMask,
                        const rock::MaskGrid* mask,
                        const rock::ColorGrid* baseColor,
                        std::string* error);
void ProcessPendingColorizeGpuRequests();

} // namespace terrain::gpu
