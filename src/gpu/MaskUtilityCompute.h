#pragma once

#include <filesystem>
#include <string>

#include "GpuComputeContext.h"
#include "../node_graph.h"

namespace terrain::gpu
{

struct MaskUtilityComputeContext
{
    GpuComputeContext gpu;
    std::filesystem::path shaderPath;
};

void SetMaskUtilityComputeContext(MaskUtilityComputeContext context);
void ResetMaskUtilityComputeResources();
const std::string& MaskUtilityComputeStatus();

bool RunMaskPathCompute(rock::MaskGrid& grid,
                        const rock::PathSettings& path,
                        const rock::MaskPathSettings& settings,
                        float terrainSizeMeters,
                        std::string* error);
bool RunMaskBlurCompute(rock::MaskGrid& grid,
                        const rock::MaskBlurSettings& settings,
                        float terrainSizeMeters,
                        std::string* error);
bool RunHeightmapFromMaskCompute(rock::HeightfieldGrid& grid,
                                 const rock::MaskGrid& mask,
                                 const rock::HeightmapFromMaskSettings& settings,
                                 int resolution,
                                 float terrainSizeMeters,
                                 std::string* error);
void ProcessPendingMaskUtilityGpuRequests();

} // namespace terrain::gpu
