#pragma once

#include <vector>

#include "../node_graph.h"

namespace rock
{
void NormalizeHeightfieldFields(HeightfieldGrid& grid);
void SelectHeightfieldPreviewField(HeightfieldGrid& grid, HeightfieldPreviewField previewField);
void ApplyHeightmapBlur(HeightfieldGrid& grid, const HeightmapBlurSettings& settings);
std::vector<float> BoxBlurHeights(const HeightfieldGrid& grid, int radius);
void ApplyMaskCurvature(HeightfieldGrid& grid, const MaskCurvatureSettings& settings);
void ApplyMaskSlope(HeightfieldGrid& grid, const MaskSlopeSettings& settings);
void ApplyMaskHeight(HeightfieldGrid& grid, const MaskHeightSettings& settings);
} // namespace rock
