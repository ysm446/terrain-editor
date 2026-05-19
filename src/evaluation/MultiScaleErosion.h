#pragma once

#include "../node_graph.h"

namespace rock
{
void ApplyMultiScaleErosion(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings);
} // namespace rock
