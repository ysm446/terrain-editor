#pragma once

#include <string>
#include <string_view>

#include "../node_graph.h"

namespace rock
{
std::string ResolveAssetPath(std::string_view path);
HeightfieldGrid BuildHeightfieldFromHeightmap(const HeightmapLoadSettings& settings, int resolution, float terrainSizeMeters, std::string* message);
} // namespace rock
