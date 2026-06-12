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
FluvialErosionGpuEvaluator g_fluvialErosionGpuEvaluator = nullptr;

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
//
// Erosion model: particles carve material proportional to the local slope
// (stream-power-like, no clamp to the neighbour heights — a planar hillside
// erodes too), carry it as sediment, and drop it where the slope falls below
// the deposit angle. Per-cell deltas are applied as a soft-saturated *sum*,
// not an average: cells crossed by many particles deepen faster (the flow →
// incision → flow feedback that forms dendritic networks) while the
// saturation cap keeps overlapping particles from spiking the terrain in a
// single iteration.
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

    // Particle density: fraction of cells seeded per pass. Small Channel
    // Influence raises the density on finer pyramid levels so small tributaries
    // appear where the grid is fine enough to resolve them.
    const float fineFactor = static_cast<float>(n) / static_cast<float>(std::max(1, targetN));
    const float density = std::clamp(settings.erosionGranularity / 100.0f *
                                     (1.0f + std::clamp(settings.smallChannelInfluence, 0.0f, 1.0f) * fineFactor),
                                     0.0f, 1.0f);
    const int particlesPerIter = std::clamp(static_cast<int>(static_cast<float>(cellCount) * density), 500, 60000);

    // Carve rate per particle visit (fraction of the slope-proportional relief
    // removed) and the per-cell saturation cap for one iteration's applied
    // delta. The cap scales with cell size so coarse pyramid levels carve the
    // broad valleys and fine levels only refine; it is also what bounds how
    // deep the busiest (valley-floor) cells can sink per iteration, so it is
    // the main lever against over-deepened valleys.
    constexpr float kWearRate = 0.05f;
    constexpr float kDepositRate = 0.25f;
    const float deltaCap = 0.12f * cellSize;

    // Per-iteration height/wear deltas plus cumulative flow/deposit, all atomic
    // so the parallel particle pass can accumulate without races.
    std::vector<std::atomic<float>> dH(cellCount); // per-iteration height delta sum
    std::vector<std::atomic<float>> dW(cellCount); // per-iteration wear delta sum
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
            dW[i].store(0.0f, std::memory_order_relaxed);
        }

        std::for_each(std::execution::par, particleIndices.begin(), particleIndices.end(), [&](int p) {
            uint32_t rng = SeedFor(settings.seed, levelSeed, it, p);
            float px = 1.0f + Hash01(rng) * spawnRange;
            float pz = 1.0f + Hash01(rng) * spawnRange;
            float velX = 0.0f;
            float velZ = 0.0f;
            float sediment = 0.0f; // carved material carried by this particle (m)

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

                // Fade all terrain edits out near the map border. Every
                // particle path that drains off the map terminates there, so
                // without the fade the border cells receive the maximum carve
                // each iteration and turn into needle-like notches (the
                // outermost row is never splatted at all and stays up).
                const float edgeDist = std::min(std::min(px, pz),
                                                std::min(static_cast<float>(n - 1) - px, static_cast<float>(n - 1) - pz));
                const float edgeFade = std::clamp((edgeDist - 1.0f) / 3.0f, 0.0f, 1.0f);

                // Effective slope blends the terrain slope with the slope the
                // particle's momentum corresponds to (velLen * friction /
                // sedimentVelocity is the slope whose equilibrium speed equals
                // velLen). Momentum carries erosion onto the gentler lower
                // slopes, so channels run from the ridges all the way down
                // instead of fading out where the hillside flattens.
                const float velSlope = velLen * friction / sedimentVelocity;
                const float effSlope = std::max(slope, velSlope);
                // Erode only while actually moving downhill: a particle trapped
                // in a pit oscillates against the force and would otherwise
                // keep grinding the pit deeper (the pock-mark artifacts).
                const bool movingDownhill = (velX * fx + velZ * fz) > 0.0f;

                if (movingDownhill && effSlope >= tanWear && slope <= tanMax)
                {
                    // Stream-power-like carve: remove material in proportion to
                    // the effective slope (slope * cellSize is the rise over
                    // one step) and carry it as sediment. No clamp to the
                    // snapshot neighbours — a planar hillside incises too,
                    // which is what lets channels cut below the surrounding
                    // surface.
                    // Clamped to deltaCap so a single visit can never exceed the
                    // per-iteration cell cap (also keeps the GPU backend's
                    // fixed-point accumulators well inside int32 range).
                    const float erode = std::min(erodeStrength * effSlope * cellSize * kWearRate * edgeFade, deltaCap);
                    if (erode > 0.0f)
                    {
                        SplatAtomic(dH, n, px, pz, -erode);
                        SplatAtomic(dW, n, px, pz, erode);
                        sediment += erode;
                    }
                }
                else if (effSlope < tanDeposit && sediment > 0.0f)
                {
                    // Gentle ground with the momentum spent: drop part of the
                    // carried sediment. Channeling discards a share of it so
                    // beds and outlets stay incised instead of filling back in.
                    const float released = sediment * kDepositRate;
                    sediment -= released;
                    const float dep = released * (1.0f - channeling) * edgeFade;
                    if (dep > 0.0f)
                    {
                        SplatAtomic(dH, n, px, pz, dep);
                        SplatAtomic(depAcc, n, px, pz, dep);
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
            // Soft-saturated sum: cells crossed by many particles move further
            // than cells crossed by one (the feedback that grows dendritic
            // networks), but never by more than ~deltaCap in one iteration, so
            // overlapping particles cannot spike the terrain.
            const float sum = dH[i].load(std::memory_order_relaxed);
            if (sum != 0.0f)
            {
                heights[i] += sum / (1.0f + std::abs(sum) / deltaCap);
            }
            const float w = dW[i].load(std::memory_order_relaxed);
            if (w > 0.0f)
            {
                wear[i] += w / (1.0f + w / deltaCap);
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
    if (settings.backend == FluvialErosionBackend::GpuCompute && g_fluvialErosionGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_fluvialErosionGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    // Feature Size (m) sets how coarse the multi-grid pyramid starts: larger
    // features begin at a lower resolution so broad valleys form first.
    const float feature = std::clamp(settings.featureSize, 1.0f, 256.0f);
    const int coarsest = std::clamp(static_cast<int>(std::lround(grid.terrainSizeMeters / feature)), 16, grid.resolution);
    RunErosion(grid, settings, coarsest, RunFluvialLevel);
}

void SetFluvialErosionGpuEvaluator(FluvialErosionGpuEvaluator evaluator)
{
    g_fluvialErosionGpuEvaluator = evaluator;
}
} // namespace rock
