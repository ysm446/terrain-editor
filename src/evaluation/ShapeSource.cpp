#include "ShapeSource.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <format>
#include <numeric>
#include <utility>
#include <vector>

namespace rock
{
namespace
{
template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
}
} // namespace

HeightfieldGrid BuildHeightfieldFromShape(const ShapeSettings& settings, int resolution, float terrainSizeMeters, std::string* message)
{
    HeightfieldGrid grid;
    grid.resolution = std::clamp(resolution, 2, 2048);
    grid.terrainSizeMeters = std::max(1.0f, terrainSizeMeters);
    const float shapeSizeMeters = std::max(1.0f, settings.scaleMeters);
    const float heightMeters = shapeSizeMeters * std::max(0.0f, settings.relativeHeightPercent) / 100.0f;
    const float halfTerrain = grid.terrainSizeMeters * 0.5f;
    const float halfShape = shapeSizeMeters * 0.5f;
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.heights.assign(cellCount, 0.0f);
    grid.mask.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    grid.flows.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);

    ParallelForRows(grid.resolution, [&](int z) {
        const float v = grid.resolution > 1 ? static_cast<float>(z) / static_cast<float>(grid.resolution - 1) : 0.0f;
        const float worldZ = std::lerp(-halfTerrain, halfTerrain, v);
        const size_t row = static_cast<size_t>(z) * static_cast<size_t>(grid.resolution);
        for (int x = 0; x < grid.resolution; ++x)
        {
            const float u = grid.resolution > 1 ? static_cast<float>(x) / static_cast<float>(grid.resolution - 1) : 0.0f;
            const float worldX = std::lerp(-halfTerrain, halfTerrain, u);
            float normalizedHeight = 0.0f;
            if (std::abs(worldX) <= halfShape && std::abs(worldZ) <= halfShape)
            {
                const float nx = worldX / halfShape;
                const float nz = worldZ / halfShape;
                if (settings.kind == ShapeKind::Hemisphere)
                {
                    const float radiusSq = nx * nx + nz * nz;
                    normalizedHeight = radiusSq < 1.0f ? std::sqrt(1.0f - radiusSq) : 0.0f;
                }
                else if (settings.kind == ShapeKind::Pyramid)
                {
                    normalizedHeight = std::max(0.0f, 1.0f - std::max(std::abs(nx), std::abs(nz)));
                }
                else if (settings.kind == ShapeKind::Box)
                {
                    normalizedHeight = 1.0f;
                }
            }
            grid.heights[row + static_cast<size_t>(x)] = normalizedHeight * heightMeters;
        }
    });

    if (message != nullptr)
    {
        *message = std::format(
            "{} shape {} x {}, canvas {:.0f}m, scale {:.0f}m, height {:.0f}m",
            ToString(settings.kind),
            grid.resolution,
            grid.resolution,
            grid.terrainSizeMeters,
            shapeSizeMeters,
            heightMeters);
    }
    return grid;
}
} // namespace rock
