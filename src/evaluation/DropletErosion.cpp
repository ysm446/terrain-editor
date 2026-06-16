#include "DropletErosion.h"

#include "ParticleErosionCommon.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

namespace rock
{
using namespace particle_erosion;

namespace
{
DropletErosionGpuEvaluator g_dropletErosionGpuEvaluator = nullptr;

// Physical width of the border taper over which carving/deposition fades to
// zero, so channels that drain off the map do not cut a needle at the edge.
constexpr float kEdgeFadeMeters = 24.0f;

// Add `delta` to the terrain, spread over a radial brush. Used for carving
// (negative delta, so droplets do not drill single-cell pits) and for
// oversaturation deposits (positive delta, so dumped sediment forms smooth
// banks instead of single-cell pimples — especially visible after the coarse
// pyramid levels are upsampled).
void ApplyBrush(std::vector<float>& heights, int n, float px, float pz, float delta, float radius)
{
    if (radius <= 1.0f)
    {
        SplatBilinear(heights, n, px, pz, delta);
        return;
    }
    const int r = static_cast<int>(std::ceil(radius));
    const int cx = static_cast<int>(std::floor(px + 0.5f));
    const int cz = static_cast<int>(std::floor(pz + 0.5f));
    float weightSum = 0.0f;
    for (int j = -r; j <= r; ++j)
    {
        for (int i = -r; i <= r; ++i)
        {
            const int qx = cx + i;
            const int qz = cz + j;
            if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
            const float d = std::sqrt(static_cast<float>(i * i + j * j));
            weightSum += std::max(0.0f, radius - d);
        }
    }
    if (weightSum <= 0.0f)
    {
        SplatBilinear(heights, n, px, pz, delta);
        return;
    }
    for (int j = -r; j <= r; ++j)
    {
        for (int i = -r; i <= r; ++i)
        {
            const int qx = cx + i;
            const int qz = cz + j;
            if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
            const float d = std::sqrt(static_cast<float>(i * i + j * j));
            const float w = std::max(0.0f, radius - d);
            if (w <= 0.0f) { continue; }
            heights[static_cast<size_t>(Index1D(qx, qz, n))] += delta * (w / weightSum);
        }
    }
}

// One resolution level of droplet (capacity-based) hydraulic erosion.
void RunDropletLevel(HeightfieldGrid& grid, const DropletErosionSettings& settings, int levelSeed, int targetN)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount || settings.dropletDensity <= 0.0f)
    {
        return;
    }

    grid.flows.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    std::vector<float>& heights = grid.heights;

    const float cellSize = grid.terrainSizeMeters / static_cast<float>(std::max(1, n - 1));
    // Droplet count is a per-cell density, so the same density (and result) holds
    // at any resolution; ParticlesForLevel then scales it by area per level.
    const int targetParticleCount = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(std::max(0.0f, settings.dropletDensity)) *
                                     static_cast<double>(targetN) * static_cast<double>(targetN))),
        1, 2000000);
    const int particles = ParticlesForLevel(targetParticleCount, n, targetN);
    // Lifetime comes from a travel distance in metres, so a droplet reaches the
    // same physical point regardless of how many cells that distance spans.
    const int lifetime = std::clamp(
        static_cast<int>(std::lround(settings.maxTravelDistance / std::max(cellSize, 1e-3f))), 1, 8192);
    const float inertia = std::clamp(settings.inertia, 0.0f, 0.99f);
    const float capacityFactor = std::max(0.01f, settings.sedimentCapacity);
    const float minSlope = std::max(0.0001f, settings.minSlope);
    const float erodeRate = std::clamp(settings.erosionStrength, 0.0f, 1.0f);
    const float depositRate = std::clamp(settings.depositionStrength, 0.0f, 1.0f);
    // Per-metre evaporation compounded over one cell-sized step. A finer grid
    // takes more steps to cross the same distance, so without this the water (and
    // thus capacity) would decay faster and channels would die mid-slope.
    const float evapPerMeter = std::clamp(settings.evaporation, 0.0f, 1.0f);
    const float stepEvapFactor = std::pow(std::max(0.0f, 1.0f - evapPerMeter), cellSize);
    const float gravity = std::max(0.0f, settings.gravity);
    // Brush radius given in metres; convert to cells. Upper-clamped so the O(r^2)
    // brush stays bounded on very fine grids.
    const float radius = std::clamp(settings.erosionRadiusMeters / std::max(cellSize, 1e-3f), 0.5f, 16.0f);
    // Border taper width, in metres converted to cells. A major channel that
    // exits the map otherwise incises at full depth right up to the faded edge
    // and leaves a needle-like notch; a fixed physical taper makes that fade out
    // gradually at every resolution (a cell-based taper is too thin on fine grids).
    const float edgeFadeCells = std::clamp(kEdgeFadeMeters / std::max(cellSize, 1e-3f), 2.0f, static_cast<float>(n) * 0.25f);

    std::mt19937 rng(static_cast<uint32_t>(settings.seed) ^ static_cast<uint32_t>(levelSeed * 2654435761u));
    std::uniform_real_distribution<float> pos(1.0f, static_cast<float>(n - 2));

    for (int p = 0; p < particles; ++p)
    {
        float px = pos(rng);
        float pz = pos(rng);
        float dirX = 0.0f;
        float dirZ = 0.0f;
        float speed = 1.0f;
        float water = 1.0f;
        float sediment = 0.0f;

        for (int step = 0; step < lifetime; ++step)
        {
            const HeightGradient hg = SampleHeightGradient(heights, n, px, pz);

            dirX = dirX * inertia - hg.gradX * (1.0f - inertia);
            dirZ = dirZ * inertia - hg.gradZ * (1.0f - inertia);
            const float dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
            if (dirLen < 1e-6f) { break; }
            dirX /= dirLen;
            dirZ /= dirLen;

            const float npx = px + dirX;
            const float npz = pz + dirZ;
            if (npx < 1.0f || npx > static_cast<float>(n - 2) || npz < 1.0f || npz > static_cast<float>(n - 2))
            {
                break;
            }

            const float newHeight = SampleHeightGradient(heights, n, npx, npz).height;
            const float dH = newHeight - hg.height;

            grid.flows[static_cast<size_t>(Index1D(static_cast<int>(px), static_cast<int>(pz), n))] += water;

            // Fade carving/deposition out near the map border. Every droplet
            // that drains off the map terminates at the edge, so without this the
            // outlet cells get carved each pass and turn into deep notches — the
            // mesh skirt then hangs far below the front edge (visible as vertical
            // "curtains", worst at high resolution where more droplets converge).
            const float edgeDist = std::min(std::min(px, pz),
                                            std::min(static_cast<float>(n - 1) - px, static_cast<float>(n - 1) - pz));
            const float edgeFade = std::clamp((edgeDist - 1.0f) / edgeFadeCells, 0.0f, 1.0f);

            // Capacity scales with the true slope (rise/run), not the raw
            // per-cell height drop, so coarse pyramid levels do not carry and
            // dump 8x more sediment than fine ones for the same terrain.
            const float slope = -dH / cellSize;
            const float capacity = std::max(slope, minSlope) * speed * water * capacityFactor;

            if (dH > 0.0f)
            {
                // Moved uphill: fill the pit at the current position so the
                // droplet can continue. Kept single-cell on purpose — the fill
                // must land in the pit itself.
                const float deposit = std::min(dH, sediment);
                sediment -= deposit;
                SplatBilinear(heights, n, px, pz, deposit);
                SplatBilinear(grid.deposits, n, px, pz, deposit);
            }
            else if (sediment > capacity)
            {
                // Oversaturated: drop the surplus with the same radial brush
                // as carving, so banks stay smooth instead of pimpling.
                const float deposit = (sediment - capacity) * depositRate * edgeFade;
                sediment -= deposit;
                ApplyBrush(heights, n, px, pz, deposit, radius);
                ApplyBrush(grid.deposits, n, px, pz, deposit, radius);
            }
            else
            {
                const float erode = std::min((capacity - sediment) * erodeRate, -dH) * edgeFade;
                ApplyBrush(heights, n, px, pz, -erode, radius);
                sediment += erode;
            }

            speed = std::sqrt(std::max(0.0f, speed * speed + (-dH) * gravity));
            water *= stepEvapFactor;
            if (water < 1e-4f) { break; }

            px = npx;
            pz = npz;
        }
    }

    FinalizeLevel(grid, cellCount);
}
} // namespace

void ApplyDropletErosion(HeightfieldGrid& grid, const DropletErosionSettings& settings)
{
    if (settings.backend == DropletErosionBackend::GpuCompute && g_dropletErosionGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_dropletErosionGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    RunErosion(grid, settings, kCoarsestPyramidLevel, RunDropletLevel);
}

void SetDropletErosionGpuEvaluator(DropletErosionGpuEvaluator evaluator)
{
    g_dropletErosionGpuEvaluator = evaluator;
}
} // namespace rock
