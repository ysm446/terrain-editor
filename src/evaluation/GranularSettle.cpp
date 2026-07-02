#include "GranularSettle.h"

#include <algorithm>
#include <cmath>
#include <execution>
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

inline size_t CellIndex(int x, int z, int n)
{
    return static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
}
} // namespace

// Snow / Soil 共有の物質再配分コア。物質を厚みとして注入し、不安定なセルは
// 最も急な低い近傍へ厚みの一部を移して、物質面が安息角を下回るまで安定させる。
// mask は coverage 型 (Threshold / Feather でほぼ白黒 + 境界グレー)。
void ApplyGranularSettle(HeightfieldGrid& grid, const GranularSettleParams& params, std::vector<float>* outThickness)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount)
    {
        return;
    }

    grid.mask.assign(cellCount, 0.0f);
    if (outThickness != nullptr)
    {
        outThickness->assign(cellCount, 0.0f);
    }

    const float emission = std::max(0.0f, params.emissionAmount);
    if (emission <= 0.0f)
    {
        return;
    }

    const float kPi = 3.14159265358979323846f;
    const int iterationCount = std::clamp(params.iterationCount, 1, 256);
    const float emissionTime = std::clamp(params.emissionTime, 0.0f, 1.0f);
    const int emissionIterations = emissionTime <= 0.0f
        ? 1
        : std::clamp(static_cast<int>(std::ceil(static_cast<float>(iterationCount) * emissionTime)), 1, iterationCount);
    const float emissionPerIteration = emission / static_cast<float>(emissionIterations);
    const int settlingPasses = std::clamp(params.settlingPasses, 1, 16);
    const float motionLimitRad = std::clamp(params.motionSlopeLimitDeg, 0.0f, 89.9f) * (kPi / 180.0f);
    const float motionLimitTan = std::tan(motionLimitRad);
    const float transportRate = std::clamp(params.transportRate, 0.0f, 1.0f);
    const float surfaceSmoothing = std::clamp(params.surfaceSmoothing, 0.0f, 1.0f);
    const float maskThreshold = std::max(0.0f, params.maskThresholdM);
    const float maskFeather = std::max(0.0f, params.maskFeatherM);
    const float slopeDependentEmission = std::clamp(params.slopeDependentEmission, 0.0f, 1.0f);

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSize = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float largestDetailM = std::clamp(params.largestDetailLevelM, cellSize, terrainSize * 0.5f);
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

    // 傾斜依存注入: 基盤の傾斜が安息角に近いほど注入を減らし、緩斜面・上面に
    // 物質を集中させる。scale = lerp(1, max(0, 1 - tan(slope)/tan(limit)), amount)。
    std::vector<float> emissionScale;
    if (slopeDependentEmission > 0.0f)
    {
        emissionScale.assign(cellCount, 1.0f);
        const float invLimitTan = 1.0f / std::max(motionLimitTan, 1e-4f);
        ParallelForRows(n, [&](int z) {
            for (int x = 0; x < n; ++x)
            {
                const int xm = std::max(x - 1, 0);
                const int xp = std::min(x + 1, n - 1);
                const int zm = std::max(z - 1, 0);
                const int zp = std::min(z + 1, n - 1);
                const float dx = (baseHeights[CellIndex(xp, z, n)] - baseHeights[CellIndex(xm, z, n)]) /
                    (cellSize * static_cast<float>(xp - xm));
                const float dz = (baseHeights[CellIndex(x, zp, n)] - baseHeights[CellIndex(x, zm, n)]) /
                    (cellSize * static_cast<float>(zp - zm));
                const float slopeTan = std::sqrt(dx * dx + dz * dz);
                const float flatFactor = std::max(0.0f, 1.0f - slopeTan * invLimitTan);
                emissionScale[CellIndex(x, z, n)] = 1.0f + (flatFactor - 1.0f) * slopeDependentEmission;
            }
        });
    }

    for (int iter = 0; iter < iterationCount; ++iter)
    {
        const float stepEmission = (iter < emissionIterations) ? emissionPerIteration : 0.0f;
        if (stepEmission > 0.0f)
        {
            ParallelForRows(n, [&](int z) {
                const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
                for (int x = 0; x < n; ++x)
                {
                    const size_t idx = rowBase + static_cast<size_t>(x);
                    const float scale = emissionScale.empty() ? 1.0f : emissionScale[idx];
                    thickness[idx] += stepEmission * scale;
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

                    const float material = thickness[idx];
                    if (material <= 0.0f)
                    {
                        continue;
                    }

                    const float surface = baseHeights[idx] + material;
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
                    const float amount = std::min({material, excess * 0.5f, material * transportRate * slopeFactor});
                    if (amount > 0.0f)
                    {
                        moveTargets[idx] = bestIdx;
                        moveAmounts[idx] = amount;
                    }
                }
            });

            nextThickness = thickness;
            float movedMaterial = 0.0f;
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
                movedMaterial += amount;
            }

            thickness.swap(nextThickness);
            if (movedMaterial <= 1e-6f * static_cast<float>(cellCount))
            {
                break;
            }
        }
    }

    auto coverage = [&](float material) {
        if (maskFeather <= 0.0f)
        {
            return material >= maskThreshold ? 1.0f : 0.0f;
        }
        const float lo = std::max(0.0f, maskThreshold - maskFeather);
        const float hi = maskThreshold + maskFeather;
        const float t = std::clamp((material - lo) / std::max(hi - lo, 1e-6f), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };

    if (surfaceSmoothing > 0.0f)
    {
        const int smoothRadius = std::clamp(maxStride, 1, 32);
        const float sigma = std::max(1.0f, static_cast<float>(smoothRadius) * 0.5f);
        const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);

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
                    const float w = std::exp(-static_cast<float>(ox * ox) * invTwoSigma2) * coverage(thickness[sidx]);
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
                    const float w = std::exp(-static_cast<float>(oz * oz) * invTwoSigma2) * coverage(thickness[sidx]);
                    sum += smoothScratch[sidx] * w;
                    weightSum += w;
                }
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float blurredSurface = weightSum > 1e-6f ? sum / weightSum : (baseHeights[idx] + thickness[idx]);
                const float targetThickness = std::max(0.0f, blurredSurface - baseHeights[idx]);
                const float blend = surfaceSmoothing * coverage(thickness[idx]);
                thickness[idx] = std::max(0.0f, thickness[idx] + (targetThickness - thickness[idx]) * blend);
            }
        });
    }

    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t idx = rowBase + static_cast<size_t>(x);
            const float material = std::max(0.0f, thickness[idx]);
            grid.heights[idx] = baseHeights[idx] + material;
            grid.mask[idx] = coverage(material);
            if (outThickness != nullptr)
            {
                (*outThickness)[idx] = material;
            }
        }
    });
}
} // namespace rock
