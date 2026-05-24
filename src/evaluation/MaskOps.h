#pragma once

#include "../node_graph.h"

namespace rock
{
float SampleMaskBilinear(const MaskGrid& grid, float u, float v);
MaskGrid ResampleMaskGrid(const MaskGrid& source, int targetResolution);
MaskGrid BlendMaskGrids(const MaskGrid& a, const MaskGrid& b, MaskBlendMode mode, float intensity);
MaskGrid ApplyMaskLevels(const MaskGrid& source, const MaskLevelsSettings& settings);
MaskGrid ApplyMaskBlur(const MaskGrid& source, const MaskBlurSettings& settings, float terrainSizeMeters);
} // namespace rock
