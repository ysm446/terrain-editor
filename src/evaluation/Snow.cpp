#include "Snow.h"

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
SnowGpuEvaluator g_snowGpuEvaluator = nullptr;

template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
}

inline size_t CellIndex(int x, int z, int n)
{
    return static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
}
} // namespace

// Snow node.
//
// This is a compact material-transport model rather than a direct slope mask.
// Snow is injected as thickness, then unstable cells move part of that snow to
// the steepest lower neighbour until the snow surface is below the motion
// slope limit. The output mask is coverage-like: mostly black or white, with a
// controlled grey feather near the exposed-ground edge.
void ApplySnow(HeightfieldGrid& grid, const SnowSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount)
    {
        return;
    }

    grid.mask.assign(cellCount, 0.0f);

    const float emission = std::max(0.0f, settings.emissionAmount);
    if (emission <= 0.0f)
    {
        return;
    }

    if (settings.backend == SnowBackend::GpuCompute && g_snowGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_snowGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    const float kPi = 3.14159265358979323846f;
    const int iterationCount = std::clamp(settings.iterationCount, 1, 256);
    const float emissionTime = std::clamp(settings.emissionTime, 0.0f, 1.0f);
    const int emissionIterations = emissionTime <= 0.0f
        ? 1
        : std::clamp(static_cast<int>(std::ceil(static_cast<float>(iterationCount) * emissionTime)), 1, iterationCount);
    const float emissionPerIteration = emission / static_cast<float>(emissionIterations);
    const int settlingPasses = std::clamp(settings.smoothingIterations, 1, 16);
    const float motionLimitRad = std::clamp(settings.motionSlopeLimitDeg, 0.0f, 89.9f) * (kPi / 180.0f);
    const float motionLimitTan = std::tan(motionLimitRad);
    const float transportRate = std::clamp(settings.transportRate, 0.0f, 1.0f);
    const float surfaceSmoothing = std::clamp(settings.surfaceSmoothing, 0.0f, 1.0f);
    const float maskThreshold = std::max(0.0f, settings.maskThresholdM);
    const float maskFeather = std::max(0.0f, settings.maskFeatherM);

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSize = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSize, terrainSize * 0.5f);
    const int maxStride = std::clamp(static_cast<int>(std::round(largestDetailM / cellSize)), 1, 64);
    int strideLevels = 0;
    for (int stride = maxStride; stride > 1; stride = std::max(1, stride / 2))
    {
        ++strideLevels;
    }

    const std::vector<float> baseHeights = grid.heights;
    std::vector<float> thickness(cellCount, 0.0f);
    std::vector<float> nextThickness(cellCount, 0.0f);
    std::vector<size_t> moveTargets(cellCount, 0);
    std::vector<float> moveAmounts(cellCount, 0.0f);
    std::vector<float> smoothScratch(cellCount, 0.0f);

    for (int iter = 0; iter < iterationCount; ++iter)
    {
        const float stepEmission = (iter < emissionIterations) ? emissionPerIteration : 0.0f;
        if (stepEmission > 0.0f)
        {
            ParallelForRows(n, [&](int z) {
                const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
                for (int x = 0; x < n; ++x)
                {
                    thickness[rowBase + static_cast<size_t>(x)] += stepEmission;
                }
            });
        }

        if (transportRate <= 0.0f)
        {
            continue;
        }

        for (int pass = 0; pass < settlingPasses; ++pass)
        {
            const int level = settlingPasses <= 1 ? strideLevels : (pass * strideLevels) / std::max(1, settlingPasses - 1);
            int stride = maxStride;
            for (int s = 0; s < level; ++s)
            {
                stride = std::max(1, stride / 2);
            }

            ParallelForRows(n, [&](int z) {
                for (int x = 0; x < n; ++x)
                {
                    const size_t idx = CellIndex(x, z, n);
                    moveTargets[idx] = idx;
                    moveAmounts[idx] = 0.0f;

                    const float snow = thickness[idx];
                    if (snow <= 0.0f)
                    {
                        continue;
                    }

                    const float surface = baseHeights[idx] + snow;
                    float bestSlope = motionLimitTan;
                    float bestDistance = cellSize;
                    size_t bestIdx = idx;

                    for (int dz = -1; dz <= 1; ++dz)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            if (dx == 0 && dz == 0)
                            {
                                continue;
                            }
                            const int nx = std::clamp(x + dx * stride, 0, n - 1);
                            const int nz = std::clamp(z + dz * stride, 0, n - 1);
                            if (nx == x && nz == z)
                            {
                                continue;
                            }

                            const size_t nidx = CellIndex(nx, nz, n);
                            const float distance = cellSize * static_cast<float>(stride) *
                                ((dx != 0 && dz != 0) ? 1.41421356237f : 1.0f);
                            const float neighbourSurface = baseHeights[nidx] + thickness[nidx];
                            const float slope = (surface - neighbourSurface) / std::max(distance, 1e-6f);
                            if (slope > bestSlope)
                            {
                                bestSlope = slope;
                                bestDistance = distance;
                                bestIdx = nidx;
                            }
                        }
                    }

                    if (bestIdx == idx)
                    {
                        continue;
                    }

                    const float stableDrop = motionLimitTan * bestDistance;
                    const float neighbourSurface = baseHeights[bestIdx] + thickness[bestIdx];
                    const float excess = std::max(0.0f, surface - neighbourSurface - stableDrop);
                    const float slopeFactor = std::clamp((bestSlope - motionLimitTan) / std::max(bestSlope, 1e-6f), 0.0f, 1.0f);
                    const float amount = std::min({snow, excess * 0.5f, snow * transportRate * slopeFactor});
                    if (amount > 0.0f)
                    {
                        moveTargets[idx] = bestIdx;
                        moveAmounts[idx] = amount;
                    }
                }
            });

            nextThickness = thickness;
            float movedSnow = 0.0f;
            for (size_t idx = 0; idx < cellCount; ++idx)
            {
                const float amount = moveAmounts[idx];
                if (amount <= 0.0f)
                {
                    continue;
                }
                const size_t target = moveTargets[idx];
                nextThickness[idx] -= amount;
                nextThickness[target] += amount;
                movedSnow += amount;
            }

            thickness.swap(nextThickness);
            if (movedSnow <= 1e-6f * static_cast<float>(cellCount))
            {
                break;
            }
        }
    }

    if (surfaceSmoothing > 0.0f)
    {
        const int smoothRadius = std::clamp(maxStride, 1, 32);
        const float sigma = std::max(1.0f, static_cast<float>(smoothRadius) * 0.5f);
        const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
        auto snowCoverage = [&](float snow) {
            if (maskFeather <= 0.0f)
            {
                return snow >= maskThreshold ? 1.0f : 0.0f;
            }
            const float lo = std::max(0.0f, maskThreshold - maskFeather);
            const float hi = maskThreshold + maskFeather;
            const float t = std::clamp((snow - lo) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        };

        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                float sum = 0.0f;
                float weightSum = 0.0f;
                for (int ox = -smoothRadius; ox <= smoothRadius; ++ox)
                {
                    const int sx = std::clamp(x + ox, 0, n - 1);
                    const size_t sidx = rowBase + static_cast<size_t>(sx);
                    const float w = std::exp(-static_cast<float>(ox * ox) * invTwoSigma2) * snowCoverage(thickness[sidx]);
                    sum += (baseHeights[sidx] + thickness[sidx]) * w;
                    weightSum += w;
                }
                const size_t idx = rowBase + static_cast<size_t>(x);
                smoothScratch[idx] = weightSum > 1e-6f ? sum / weightSum : (baseHeights[idx] + thickness[idx]);
            }
        });
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                float sum = 0.0f;
                float weightSum = 0.0f;
                for (int oz = -smoothRadius; oz <= smoothRadius; ++oz)
                {
                    const int sz = std::clamp(z + oz, 0, n - 1);
                    const size_t sidx = static_cast<size_t>(sz) * static_cast<size_t>(n) + static_cast<size_t>(x);
                    const float w = std::exp(-static_cast<float>(oz * oz) * invTwoSigma2) * snowCoverage(thickness[sidx]);
                    sum += smoothScratch[sidx] * w;
                    weightSum += w;
                }
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float blurredSurface = weightSum > 1e-6f ? sum / weightSum : (baseHeights[idx] + thickness[idx]);
                const float targetThickness = std::max(0.0f, blurredSurface - baseHeights[idx]);
                const float blend = surfaceSmoothing * snowCoverage(thickness[idx]);
                thickness[idx] = std::max(0.0f, thickness[idx] + (targetThickness - thickness[idx]) * blend);
            }
        });
    }

    auto coverageMask = [&](float snow) {
        if (maskFeather <= 0.0f)
        {
            return snow >= maskThreshold ? 1.0f : 0.0f;
        }
        const float lo = std::max(0.0f, maskThreshold - maskFeather);
        const float hi = maskThreshold + maskFeather;
        const float t = std::clamp((snow - lo) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t idx = rowBase + static_cast<size_t>(x);
            const float snow = std::max(0.0f, thickness[idx]);
            grid.heights[idx] = baseHeights[idx] + snow;
            grid.mask[idx] = coverageMask(snow);
        }
    });
}

void SetSnowGpuEvaluator(SnowGpuEvaluator evaluator)
{
    g_snowGpuEvaluator = evaluator;
}
} // namespace rock
