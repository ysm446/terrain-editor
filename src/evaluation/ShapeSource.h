#pragma once

#include <string>

#include "../node_graph.h"

namespace rock
{
HeightfieldGrid BuildHeightfieldFromShape(const ShapeSettings& settings, int resolution, float terrainSizeMeters, std::string* message);
} // namespace rock
