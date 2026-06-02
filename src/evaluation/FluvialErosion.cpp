#include "FluvialErosion.h"

#include "ParticleErosionCommon.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <execution>
#include <numeric>
#include <vector>

namespace rock
{
using namespace particle_erosion;

namespace
{
// Cheap, stateless per-particle PRNG (so scatter is deterministic and
// independent of thread scheduling). Advances `s` and returns a float in [0,1).
inline float Hash01(uint32_t& s)
{
    s += 0x9e3779b9u;
    uint32_t z = s;
    z = (z ^ (z >> 16)) * 0x21f0aaadu;
    z = (z ^ (z >> 15)) * 0x735a2d97u;
    z = z ^ (z >> 15);
    return static_cast<float>(z >> 8) * (1.0f / 16777216.0f);
}

inline uint32_t SeedFor(int seed, int levelSeed, int iter, int particle)
{
    uint32_t h = static_cast<uint32_t>(seed) * 2654435761u;
    h = (h ^ static_cast<uint32_t>(levelSeed + 1)) * 2246822519u;
    h = (h ^ static_cast<uint32_t>(iter + 1)) * 3266489917u;
    h = (h ^ static_cast<uint32_t>(particle)) * 668265263u;
    return h ^ (h >> 15);
}

// Bilinear atomic splat of `amount` onto the four cells around a continuous
// position. Used so particles can accumulate concurrently without data races.
inline void SplatAtomic(std::vector<std::atomic<float>>& field, int n, float px, float pz, float amount)
{
    const int x0 = std::clamp(static_cast<int>(std::floor(px)), 0, n - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(pz)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1);
    const int z1 = std::min(z0 + 1, n - 1);
    const float u = px - static_cast<float>(x0);
    const float v = pz - static_cast<float>(z0);
    field[static_cast<size_t>(Index1D(x0, z0, n))].fetch_add(amount * (1.0f - u) * (1.0f - v), std::memory_order_relaxed);
    field[static_cast<size_t>(Index1D(x1, z0, n))].fetch_add(amount * u * (1.0f - v), std::memory_order_relaxed);
    field[static_cast<size_t>(Index1D(x0, z1, n))].fetch_add(amount * (1.0f - u) * v, std::memory_order_relaxed);
    field[static_cast<size_t>(Index1D(x1, z1, n))].fetch_add(amount * u * v, std::memory_order_relaxed);
}

// One resolution level of KTT-style force-field particle transport. Each
// iteration freezes the height (and wear) field, traces all particles against
// that snapshot in parallel — matching KTT's GPU model where every particle
// runs against one Update_Forces snapshot — accumulates height/wear/flow/
// deposit deltas atomically, then applies them. The next iteration sees the
// carved channels, so the network deepens and branches over the passes.
//
// The gradient is divided by cell size so `slope` is a true rise/run ratio and
// can be compared against tan(angle) thresholds; without this the angle gates
// never fire on real terrain and the node appears inert.
void RunFluvialLevel(HeightfieldGrid& grid, const FluvialErosionSettings& settings, int levelSeed, int targetN)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount)
    {
        return;
    }

    grid.flows.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    std::vector<float>& heights = grid.heights;

    const float cellSize = grid.terrainSizeMeters / static_cast<float>(std::max(1, n - 1));
    const int iterations = std::clamp(settings.simulationIterations, 0, 200);
    if (iterations <= 0) { FinalizeLevel(grid, cellCount); return; }

    const float ageGain = std::clamp(settings.geologicalAge, 0.0f, 20.0f) / 20.0f;
    const int steps = std::clamp(static_cast<int>(std::lround(settings.channelLength / std::max(cellSize, 1e-3f))), 1, 4096);
    const float friction = std::clamp(settings.friction, 0.0f, 0.99f);
    const float erodeStrength = std::clamp(settings.erosionStrength, 0.0f, 1.0f) * std::max(0.0f, ageGain);
    const float channeling = std::clamp(settings.channeling, 0.0f, 1.0f);
    const float sedimentVelocity = std::clamp(settings.sedimentVelocity, 0.01f, 2.0f);
    const float flowVolume = std::clamp(settings.flowVolume, 0.0f, 1.0f);
    const float tanWear = std::tan(std::clamp(settings.wearAngleDeg, 0.0f, 89.0f) * kPi / 180.0f);
    const float tanDeposit = std::tan(std::clamp(settings.depositAngleDeg, 0.0f, 89.0f) * kPi / 180.0f);
    const float tanMax = std::tan(std::clamp(settings.maxErosionAngleDeg, 0.0f, 89.0f) * kPi / 180.0f);
    const float tanLow = std::max(tanWear, tanDeposit);

    // Particle density: fraction of cells seeded per pass. Small Channel
    // Influence raises the density on finer pyramid levels so small tributaries
    // appear where the grid is fine enough to resolve them.
    const float fineFactor = static_cast<float>(n) / static_cast<float>(std::max(1, targetN));
    const float density = std::clamp(settings.erosionGranularity / 100.0f *
                                     (1.0f + std::clamp(settings.smallChannelInfluence, 0.0f, 1.0f) * fineFactor),
                                     0.0f, 1.0f);
    const int particlesPerIter = std::clamp(static_cast<int>(static_cast<float>(cellCount) * density), 500, 60000);

    // Per-iteration height/wear deltas plus cumulative flow/deposit, all atomic
    // so the parallel particle pass can accumulate without races.
    std::vector<std::atomic<float>> dH(cellCount);       // per-iteration height delta sum (weighted)
    std::vector<std::atomic<float>> dHWeight(cellCount); // per-iteration contribution weight per cell
    std::vector<std::atomic<float>> dW(cellCount);       // per-iteration wear delta sum (weighted)
    std::vector<std::atomic<float>> flowAcc(cellCount);
    std::vector<std::atomic<float>> depAcc(cellCount);
    for (size_t i = 0; i < cellCount; ++i)
    {
        flowAcc[i].store(0.0f, std::memory_order_relaxed);
        depAcc[i].store(0.0f, std::memory_order_relaxed);
    }

    std::vector<float> wear(cellCount, 0.0f);
    std::vector<float> hSnap;
    std::vector<float> wSnap;

    std::vector<int> particleIndices(static_cast<size_t>(particlesPerIter));
    std::iota(particleIndices.begin(), particleIndices.end(), 0);

    const float spawnRange = static_cast<float>(n - 3); // start positions in [1, n-2]

    for (int it = 0; it < iterations; ++it)
    {
        hSnap = heights;
        wSnap = wear;
        for (size_t i = 0; i < cellCount; ++i)
        {
            dH[i].store(0.0f, std::memory_order_relaxed);
            dHWeight[i].store(0.0f, std::memory_order_relaxed);
            dW[i].store(0.0f, std::memory_order_relaxed);
        }

        std::for_each(std::execution::par, particleIndices.begin(), particleIndices.end(), [&](int p) {
            uint32_t rng = SeedFor(settings.seed, levelSeed, it, p);
            float px = 1.0f + Hash01(rng) * spawnRange;
            float pz = 1.0f + Hash01(rng) * spawnRange;
            float velX = 0.0f;
            float velZ = 0.0f;

            for (int step = 0; step < steps; ++step)
            {
                HeightGradient hg = SampleHeightGradient(hSnap, n, px, pz);
                float gx = hg.gradX;
                float gz = hg.gradZ;
                if (flowVolume > 0.0f)
                {
                    const HeightGradient wg = SampleHeightGradient(wSnap, n, px, pz);
                    gx -= flowVolume * wg.gradX;
                    gz -= flowVolume * wg.gradZ;
                }
                // Divide by cell size so the force is a true slope (rise/run).
                const float fx = -gx / cellSize;
                const float fz = -gz / cellSize;
                const float slope = std::sqrt(fx * fx + fz * fz);

                velX = velX * (1.0f - friction) + fx * sedimentVelocity;
                velZ = velZ * (1.0f - friction) + fz * sedimentVelocity;
                const float velLen = std::sqrt(velX * velX + velZ * velZ);
                if (velLen < 1e-6f) { break; }
                const float sx = velX / velLen;
                const float sz = velZ / velLen;

                flowAcc[static_cast<size_t>(Index1D(static_cast<int>(px), static_cast<int>(pz), n))]
                    .fetch_add(1.0f, std::memory_order_relaxed);

                if (slope >= tanLow && slope <= tanMax)
                {
                    const float h1 = hg.height;
                    const float h2 = SampleField(hSnap, n, px + sx, pz + sz);
                    const float h3 = SampleField(hSnap, n, px - sx, pz - sz);
                    const float lo = std::min(h2, h3);
                    const float hi = std::max(h2, h3);
                    // Pull toward the ahead/behind average, but never past the
                    // neighbour range. This incises channels and also erodes any
                    // stray spike back down (KTT clamps the height the same way).
                    const float newH1 = std::clamp(std::lerp(h1, 0.5f * (h2 + h3), erodeStrength), lo, hi);
                    float delta = newH1 - h1;
                    if (delta > 0.0f) { delta -= channeling * delta; } // cancel part of deposition
                    SplatAtomic(dH, n, px, pz, delta);
                    SplatAtomic(dHWeight, n, px, pz, 1.0f);
                    if (delta < 0.0f)
                    {
                        SplatAtomic(dW, n, px, pz, -delta);
                    }
                    else
                    {
                        SplatAtomic(depAcc, n, px, pz, delta);
                    }
                }

                const float npx = px + sx;
                const float npz = pz + sz;
                if (npx < 1.0f || npx > static_cast<float>(n - 2) || npz < 1.0f || npz > static_cast<float>(n - 2))
                {
                    break;
                }
                px = npx;
                pz = npz;
            }
        });

        for (size_t i = 0; i < cellCount; ++i)
        {
            const float w = dHWeight[i].load(std::memory_order_relaxed);
            if (w > 1e-6f)
            {
                // Apply the average per-cell delta this iteration. Summing every
                // overlapping particle's desired move would multiply the change
                // by the particle count and spike the terrain upward.
                const float inv = 1.0f / w;
                heights[i] += dH[i].load(std::memory_order_relaxed) * inv;
                wear[i] += dW[i].load(std::memory_order_relaxed) * inv;
            }
        }
    }

    for (size_t i = 0; i < cellCount; ++i)
    {
        grid.flows[i] = flowAcc[i].load(std::memory_order_relaxed);
        grid.deposits[i] = depAcc[i].load(std::memory_order_relaxed);
    }
    FinalizeLevel(grid, cellCount);
}
} // namespace

void ApplyFluvialErosion(HeightfieldGrid& grid, const FluvialErosionSettings& settings)
{
    // Feature Size (m) sets how coarse the multi-grid pyramid starts: larger
    // features begin at a lower resolution so broad valleys form first.
    const float feature = std::clamp(settings.featureSize, 1.0f, 256.0f);
    const int coarsest = std::clamp(static_cast<int>(std::lround(grid.terrainSizeMeters / feature)), 16, grid.resolution);
    RunErosion(grid, settings, coarsest, RunFluvialLevel);
}
} // namespace rock
