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
// Soil ノードも同じ再配分 compute shader (snow_compute.hlsl) を共有する。
// 傾斜依存注入 (Slope-Dependent Emission) は shader 側の定数で有効化される。
bool RunSoilCompute(rock::HeightfieldGrid& grid, const rock::SoilSettings& settings, std::string* error);
void ProcessPendingSnowGpuRequests();

} // namespace terrain::gpu
