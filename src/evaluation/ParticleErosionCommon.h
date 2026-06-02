#pragma once

// Shared helpers for the particle-based erosion nodes (Droplet Erosion and
// Fluvial Erosion). Both trace particles over a heightfield, mutating the
// terrain in place, and both reuse the same bilinear sampling, splatting, and
// coarse-to-fine multi-grid scaffolding. Only the per-particle kernel differs,
// so each node supplies its own RunLevel callable to RunErosion().

#include "HeightfieldOps.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rock
{
namespace particle_erosion
{
constexpr float kPi = 3.14159265358979323846f;
constexpr int kCoarsestPyramidLevel = 64;

inline int Index1D(int x, int z, int n)
{
    return z * n + x;
}

// Bilinear sample of an arbitrary cell field at a continuous cell-space
// position. Caller guarantees pos stays within [0, n-1].
inline float SampleField(const std::vector<float>& field, int n, float px, float pz)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, n - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(pz)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1);
    const int z1 = std::min(z0 + 1, n - 1);
    const float u = px - static_cast<float>(x0);
    const float v = pz - static_cast<float>(z0);
    const float nw = field[static_cast<size_t>(Index1D(x0, z0, n))];
    const float ne = field[static_cast<size_t>(Index1D(x1, z0, n))];
    const float sw = field[static_cast<size_t>(Index1D(x0, z1, n))];
    const float se = field[static_cast<size_t>(Index1D(x1, z1, n))];
    return std::lerp(std::lerp(nw, ne, u), std::lerp(sw, se, u), v);
}

struct HeightGradient
{
    float height = 0.0f;
    float gradX = 0.0f;
    float gradZ = 0.0f;
};

// Height and analytic gradient of the bilinear surface at a continuous
// position.
inline HeightGradient SampleHeightGradient(const std::vector<float>& heights, int n, float px, float pz)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, n - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(pz)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1);
    const int z1 = std::min(z0 + 1, n - 1);
    const float u = px - static_cast<float>(x0);
    const float v = pz - static_cast<float>(z0);
    const float nw = heights[static_cast<size_t>(Index1D(x0, z0, n))];
    const float ne = heights[static_cast<size_t>(Index1D(x1, z0, n))];
    const float sw = heights[static_cast<size_t>(Index1D(x0, z1, n))];
    const float se = heights[static_cast<size_t>(Index1D(x1, z1, n))];

    HeightGradient out;
    out.gradX = std::lerp(ne - nw, se - sw, v);
    out.gradZ = std::lerp(sw - nw, se - ne, u);
    out.height = std::lerp(std::lerp(nw, ne, u), std::lerp(sw, se, u), v);
    return out;
}

// Splat a height delta onto the four cells around a continuous position using
// bilinear weights (sharp deposition / single-cell height update).
inline void SplatBilinear(std::vector<float>& field, int n, float px, float pz, float amount)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, n - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(pz)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1);
    const int z1 = std::min(z0 + 1, n - 1);
    const float u = px - static_cast<float>(x0);
    const float v = pz - static_cast<float>(z0);
    field[static_cast<size_t>(Index1D(x0, z0, n))] += amount * (1.0f - u) * (1.0f - v);
    field[static_cast<size_t>(Index1D(x1, z0, n))] += amount * u * (1.0f - v);
    field[static_cast<size_t>(Index1D(x0, z1, n))] += amount * (1.0f - u) * v;
    field[static_cast<size_t>(Index1D(x1, z1, n))] += amount * u * v;
}

// Number of particles to trace at a given pyramid level: keeps the particle
// density (particles per cell) roughly constant across levels so the coarse
// passes stay cheap.
inline int ParticlesForLevel(int requested, int levelN, int targetN)
{
    if (levelN >= targetN) { return std::max(0, requested); }
    const double ratio = (static_cast<double>(levelN) * levelN) / (static_cast<double>(targetN) * targetN);
    return std::max(1000, static_cast<int>(static_cast<double>(requested) * ratio));
}

inline std::vector<float> ResampleHeightsBilinear(const std::vector<float>& source, int sourceN, int targetN)
{
    std::vector<float> result(static_cast<size_t>(targetN) * static_cast<size_t>(targetN), 0.0f);
    for (int z = 0; z < targetN; ++z)
    {
        const float v = targetN > 1 ? static_cast<float>(z) / static_cast<float>(targetN - 1) : 0.0f;
        const float sz = v * static_cast<float>(std::max(1, sourceN - 1));
        for (int x = 0; x < targetN; ++x)
        {
            const float u = targetN > 1 ? static_cast<float>(x) / static_cast<float>(targetN - 1) : 0.0f;
            const float sx = u * static_cast<float>(std::max(1, sourceN - 1));
            result[static_cast<size_t>(z * targetN + x)] = SampleField(source, sourceN, sx, sz);
        }
    }
    return result;
}

// Coarse-to-fine driver shared by both erosion nodes. `runLevel` runs one
// resolution level in place: it must assign grid.flows / grid.deposits, trace
// its particles, and normalize the auxiliary fields. `coarsestLevel` is the
// resolution the multi-grid pyramid starts from. Signature:
//   void runLevel(HeightfieldGrid& level, const Settings& s, int levelSeed, int targetN)
template <typename Settings, typename RunLevelFn>
inline void RunErosion(HeightfieldGrid& grid, const Settings& settings, int coarsestLevel, RunLevelFn runLevel)
{
    const int targetN = grid.resolution;
    const size_t targetCellCount = static_cast<size_t>(targetN) * static_cast<size_t>(targetN);
    if (targetN < 3 || grid.heights.size() < targetCellCount)
    {
        return;
    }

    if (!settings.useMultigrid)
    {
        runLevel(grid, settings, 0, targetN);
        return;
    }

    std::vector<int> levels;
    for (int r = std::clamp(coarsestLevel, 16, targetN); r < targetN; r *= 2)
    {
        levels.push_back(r);
    }
    levels.push_back(targetN);

    if (levels.size() <= 1)
    {
        runLevel(grid, settings, 0, targetN);
        return;
    }

    HeightfieldGrid working;
    working.terrainSizeMeters = grid.terrainSizeMeters;
    working.resolution = levels[0];
    working.heights = ResampleHeightsBilinear(grid.heights, targetN, levels[0]);

    for (size_t i = 0; i < levels.size(); ++i)
    {
        if (i > 0)
        {
            working.heights = ResampleHeightsBilinear(working.heights, levels[i - 1], levels[i]);
            working.resolution = levels[i];
        }
        runLevel(working, settings, static_cast<int>(i + 1), targetN);
    }

    grid.heights = std::move(working.heights);
    grid.flows = std::move(working.flows);
    grid.deposits = std::move(working.deposits);
    grid.mask.assign(targetCellCount, 0.0f);
    grid.uniqueMask.assign(targetCellCount, 0.0f);
    grid.age.assign(targetCellCount, 0.0f);
}

// Common post-pass for a single level: log-compress the long-tailed flow
// accumulation and normalize all auxiliary fields to [0, 1].
inline void FinalizeLevel(HeightfieldGrid& grid, size_t cellCount)
{
    for (float& f : grid.flows) { f = std::log(1.0f + std::max(0.0f, f)); }
    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);
    NormalizeHeightfieldFields(grid);
}
} // namespace particle_erosion
} // namespace rock
