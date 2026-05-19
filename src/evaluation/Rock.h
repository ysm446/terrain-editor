#pragma once

#include "../node_graph.h"

namespace rock
{
void ApplyRock(HeightfieldGrid& grid, const RockSettings& settings, const MaskGrid* placementMask = nullptr);
} // namespace rock
