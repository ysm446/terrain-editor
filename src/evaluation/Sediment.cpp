#include "Sediment.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace rock
{
namespace
{
SedimentGpuEvaluator g_sedimentGpuEvaluator = nullptr;

template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
}
} // namespace
namespace sediment_shared
{
// Apply contrast curve to a normalised sediment value. `contrast` ∈ [0, 1]:
//   0 → wide smoothstep over the full [0, 1] range (gentle S-curve, near linear).
//   1 → near-binary step at t = 0.5.
// Implemented as smoothstep(lo, hi, t) where the band [lo, hi] shrinks
// from [0, 1] to [≈0.5, ≈0.5] as contrast goes 0 → 1.
inline float ApplyMaskContrast(float t, float contrast)
{
    const float halfBand = std::max((1.0f - std::clamp(contrast, 0.0f, 1.0f)) * 0.5f, 0.005f);
    const float lo = 0.5f - halfBand;
    const float hi = 0.5f + halfBand;
    const float x = std::clamp((t - lo) / (hi - lo), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
} // namespace sediment_shared

namespace sediment_geogen
{
// One thermal-slide pass at unit stride (4-connected neighbours).
// Sediment flows from each cell to neighbours whose total height is
// lower by more than the talus drop `talusH`. Per-neighbour flow uses
// the (n+1) divisor: with n active lower neighbours, each receives
// `drops[k] / (n+1)` and the cell loses `totalDrop / (n+1)`. This is
// the unique amount that makes every post-flow slope equal exactly
// `talusH` in one step (no overshoot, no oscillation), regardless of
// how many neighbours are active or how unevenly the drops are
// distributed. Race-free: first sweep snapshots outgoing shares to a
// scratch buffer, second sweep applies (own-out − sum of neighbours'
// shares aimed back at this cell). Memory: 4 × n² floats for
// `outgoing`, allocated once by the caller and reused.
inline void ThermalSlideUnitStride(
    std::vector<float>& sediment,
    const std::vector<float>& bedrock,
    std::vector<float>& outgoing,
    int n,
    float talusH)
{
    static constexpr int dxs[4] = {+1, -1, 0, 0};
    static constexpr int dzs[4] = {0, 0, +1, -1};
    static constexpr int oppositeK[4] = {1, 0, 3, 2};

    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const size_t i = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
            const float h = bedrock[i] + sediment[i];

            float drops[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            float totalDrop = 0.0f;
            int activeCount = 0;
            for (int k = 0; k < 4; ++k)
            {
                const int nx = x + dxs[k];
                const int nz = z + dzs[k];
                if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
                const size_t j = static_cast<size_t>(nz) * static_cast<size_t>(n) + static_cast<size_t>(nx);
                const float diff = h - bedrock[j] - sediment[j];
                if (diff > talusH)
                {
                    drops[k] = diff - talusH;
                    totalDrop += drops[k];
                    ++activeCount;
                }
            }

            const size_t base = i * 4u;
            outgoing[base + 0] = 0.0f;
            outgoing[base + 1] = 0.0f;
            outgoing[base + 2] = 0.0f;
            outgoing[base + 3] = 0.0f;
            if (activeCount == 0 || totalDrop <= 0.0f) continue;

            // (n+1)-divisor flow. Ideal per-neighbour: drops[k] / (n+1).
            // Cap by available sediment and scale all shares uniformly.
            const float divisor = static_cast<float>(activeCount + 1);
            const float idealOut = totalDrop / divisor;
            const float actualOut = std::min(sediment[i], idealOut);
            const float scale = (idealOut > 0.0f) ? (actualOut / idealOut) : 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                if (drops[k] > 0.0f) outgoing[base + static_cast<size_t>(k)] = (drops[k] / divisor) * scale;
            }
        }
    });

    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const size_t i = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
            const size_t base = i * 4u;
            const float totalOut = outgoing[base + 0] + outgoing[base + 1] + outgoing[base + 2] + outgoing[base + 3];
            float incoming = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                const int nx = x + dxs[k];
                const int nz = z + dzs[k];
                if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
                const size_t j = static_cast<size_t>(nz) * static_cast<size_t>(n) + static_cast<size_t>(nx);
                incoming += outgoing[j * 4u + static_cast<size_t>(oppositeK[k])];
            }
            sediment[i] = std::max(0.0f, sediment[i] - totalOut + incoming);
        }
    });
}
} // namespace sediment_geogen

void ApplySediment(HeightfieldGrid& grid, const SedimentSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount) return;

    // GPU compute path. Falls back to the CPU body below if the
    // evaluator hasn't been registered (no D3D12 device) or returns
    // failure (e.g. shader compile error). The evaluator fills
    // grid.heights and grid.mask in the same way the CPU branch does.
    if (settings.backend == SedimentBackend::GpuCompute && g_sedimentGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_sedimentGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Fall through to CPU on failure.
    }

    const float terrainSizeM = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSizeM = terrainSizeM / std::max(1.0f, static_cast<float>(n - 1));

    // Bedrock = static base, sediment = movable layer. "Convert terrain
    // to sediment" treats the input height itself as sediment over a flat
    // bedrock = 0, so the entire mountain can be reshaped by gravity.
    // Otherwise the input is fixed bedrock and we start with no sediment
    // (only what `Emission amount` adds is movable).
    std::vector<float> bedrock(cellCount);
    std::vector<float> sediment(cellCount);
    if (settings.convertTerrainToSediment)
    {
        ParallelForRows(n, [&](int z) {
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
                bedrock[idx] = 0.0f;
                sediment[idx] = grid.heights[idx];
            }
        });
    }
    else
    {
        ParallelForRows(n, [&](int z) {
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
                bedrock[idx] = grid.heights[idx];
                sediment[idx] = 0.0f;
            }
        });
    }

    // Talus angle from viscosity, with a quadratic curve so low viscosity
    // produces near-flat lakes (sediment levels out in basins like a
    // fluid). 0% → 0°, 20% → 3.2°, 50% → 20°, 100% → 80°. The default
    // 20% gives nearly horizontal accumulation surfaces in valleys
    // (matching GeoGen's behaviour where deposited areas read as flat
    // pools), while high values still allow steep talus piles.
    const float viscosity = std::clamp(settings.sedimentViscosity, 0.0f, 1.0f);
    const float talusAngleDeg = viscosity * viscosity * 80.0f;
    const float talusTan = std::tan(talusAngleDeg * 3.14159265358979323846f / 180.0f);

    // Talus drop threshold for the unit-stride slide. Information moves
    // 1 cell per pass, so we need many passes for sediment to relax over
    // long distances. `Largest Detail Level` says how far (in metres)
    // we want sediment to be able to travel before stopping at the talus
    // angle — convert to a per-iteration "macro-pass" multiplier so the
    // total work scales with the desired settling extent.
    const float talusH = talusTan * cellSizeM;

    const float largestM = std::clamp(settings.largestDetailLevelM, cellSizeM, terrainSizeM * 0.5f);
    const int macroPasses = std::max(1, static_cast<int>(std::ceil(largestM / cellSizeM)));

    // Emission timing: total `emissionAmountM` is split across the first
    // `emissionEnd` outer iterations. emissionTime=0 → all up-front
    // (loose layer settles freely from the start); emissionTime=1 →
    // spread evenly across every iteration (each thin layer settles
    // into the channels carved by the previous one — sharper detail).
    const int iterations = std::max(1, settings.iterations);
    const int stabIter = std::max(1, settings.stabilizationIterations);
    const float emissionAmount = std::max(0.0f, settings.emissionAmountM);
    const float emissionTime = std::clamp(settings.emissionTime, 0.0f, 1.0f);
    const int emissionEnd = std::max(1,
        static_cast<int>(std::ceil(static_cast<float>(iterations) * emissionTime)));
    const float emissionPerIter = emissionAmount / static_cast<float>(emissionEnd);

    std::vector<float> outgoing(cellCount * 4u, 0.0f);

    for (int iter = 0; iter < iterations; ++iter)
    {
        if (iter < emissionEnd && emissionPerIter > 0.0f)
        {
            ParallelForRows(n, [&](int z) {
                for (int x = 0; x < n; ++x)
                {
                    sediment[static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x)] += emissionPerIter;
                }
            });
        }

        // Each iteration runs `macroPasses × stabIter` unit-stride slide
        // passes. macroPasses scales with `Largest Detail Level` so
        // sediment can relax over the desired distance per iteration;
        // stabIter is the user-controlled inner refinement count.
        const int passes = macroPasses * stabIter;
        for (int p = 0; p < passes; ++p)
        {
            sediment_geogen::ThermalSlideUnitStride(sediment, bedrock, outgoing, n, talusH);
        }
    }

    // Mask normalisation: a single deep basin can carry 10-100× the
    // sediment thickness of typical deposit areas, so dividing by the
    // raw max would compress 99% of the map into the dim end of the
    // scale (only the deepest spike reads bright). Normalise by the
    // 95th percentile instead — the brightest 5% saturate to white and
    // the remaining 95% spread across the full [0, 1] range, matching
    // what the eye sees in the 3D view.
    std::vector<float> sortedSediment(sediment.begin(), sediment.begin() + cellCount);
    const size_t pIndex = std::min(cellCount - 1, (cellCount * 95u) / 100u);
    std::nth_element(sortedSediment.begin(), sortedSediment.begin() + pIndex, sortedSediment.end());
    const float maskNorm = std::max(sortedSediment[pIndex], 1e-4f);
    const float contrast = std::clamp(settings.maskContrast, 0.0f, 1.0f);

    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
            grid.heights[idx] = bedrock[idx] + sediment[idx];
            grid.mask[idx] = sediment_shared::ApplyMaskContrast(sediment[idx] / maskNorm, contrast);
        }
    });
}

void SetSedimentGpuEvaluator(SedimentGpuEvaluator evaluator)
{
    g_sedimentGpuEvaluator = evaluator;
}
} // namespace rock
