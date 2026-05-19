#pragma once

#include "../node_graph.h"

namespace rock
{
ColorGrid GenerateColorize(
    const ColorizeSettings& settings,
    const MaskGrid& gradientMask,
    const MaskGrid* mask,
    const ColorGrid* baseColor);
} // namespace rock
