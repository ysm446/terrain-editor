#include "DropletErosion.h"

#include "ParticleErosionCommon.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace rock
{
using namespace particle_erosion;

namespace
{
// Lower the terrain by `amount`, spread over a radial brush so droplet carving
// stays smooth instead of drilling single-cell pits.
void ErodeBrush(std::vector<float>& heights, int n, float px, float pz, float amount, float radius)
{
    if (radius <= 1.0f)
    {
        SplatBilinear(heights, n, px, pz, -amount);
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
        SplatBilinear(heights, n, px, pz, -amount);
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
            heights[static_cast<size_t>(Index1D(qx, qz, n))] -= amount * (w / weightSum);
        }
    }
}

// One resolution level of droplet (capacity-based) hydraulic erosion.
void RunDropletLevel(HeightfieldGrid& grid, const DropletErosionSettings& settings, int levelSeed, int targetN)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount || settings.particleCount <= 0)
    {
        return;
    }

    grid.flows.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    std::vector<float>& heights = grid.heights;

    const int particles = ParticlesForLevel(settings.particleCount, n, targetN);
    const int lifetime = std::clamp(settings.maxLifetime, 1, 2048);
    const float inertia = std::clamp(settings.inertia, 0.0f, 0.99f);
    const float capacityFactor = std::max(0.01f, settings.sedimentCapacity);
    const float minSlope = std::max(0.0001f, settings.minSlope);
    const float erodeRate = std::clamp(settings.erosionStrength, 0.0f, 1.0f);
    const float depositRate = std::clamp(settings.depositionStrength, 0.0f, 1.0f);
    const float evaporation = std::clamp(settings.evaporation, 0.0f, 1.0f);
    const float gravity = std::max(0.0f, settings.gravity);
    const float radius = std::clamp(settings.erosionRadius, 0.5f, 8.0f);

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

            const float capacity = std::max(-dH, minSlope) * speed * water * capacityFactor;

            if (sediment > capacity || dH > 0.0f)
            {
                const float deposit = (dH > 0.0f)
                    ? std::min(dH, sediment)
                    : (sediment - capacity) * depositRate;
                sediment -= deposit;
                SplatBilinear(heights, n, px, pz, deposit);
                SplatBilinear(grid.deposits, n, px, pz, deposit);
            }
            else
            {
                const float erode = std::min((capacity - sediment) * erodeRate, -dH);
                ErodeBrush(heights, n, px, pz, erode, radius);
                sediment += erode;
            }

            speed = std::sqrt(std::max(0.0f, speed * speed + (-dH) * gravity));
            water *= (1.0f - evaporation);
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
    RunErosion(grid, settings, kCoarsestPyramidLevel, RunDropletLevel);
}
} // namespace rock
