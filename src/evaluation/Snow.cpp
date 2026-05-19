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
} // namespace
// Snow node.
//
// 「雪を降らせる」フィルタ。ベースとなる emissionAmount [m] の雪を
// terrain 全体に均一に積もらせるが、ローカルな斜面角度に応じて積雪量を
// 減衰させる。
//
//   slope <= slopeLimitMin    → emissionAmount まるごと積もる (満雪)
//   slope >= slopeLimitMax    → 雪はまったく積もらない (剥き出しの岩)
//   その間                    → smoothstep で滑らかに遷移
//
// 仕上げに「snow envelope smoothing」を smoothingIterations 回かける:
//   surface = heights + thickness
//   blurred = separable gaussian blur of surface (Largest Detail Level radius)
//   surface = max(surface, blurred)   ← 周囲より低いセルだけ持ち上がる
//   thickness = surface - heights
// これにより周囲が高いセル (= 溝の底) は雪が増えて埋まり、周囲より高い
// セル (= 出っ張り) は変わらない。スロープ遷移域の per-cell な厚み揺らぎ
// が消え、雪が物理的に「積もって流れて埋める」自然な見た目になる。
//
// 出力:
//   grid.heights += smoothedThickness  (元地形 + 雪の厚み)
//   grid.mask     = smoothedThickness / maskMaxSnow を [0,1] にクランプ
void ApplySnow(HeightfieldGrid& grid, const SnowSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount)
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

    grid.mask.assign(cellCount, 0.0f);

    const float emission = std::max(0.0f, settings.emissionAmount);
    const float kPi = 3.14159265358979323846f;
    const float minRad = std::clamp(settings.slopeLimitMinDeg, 0.0f, 89.9f) * (kPi / 180.0f);
    const float maxRad = std::clamp(std::max(settings.slopeLimitMaxDeg, settings.slopeLimitMinDeg), 0.0f, 89.9f) * (kPi / 180.0f);
    const float minTan = std::tan(minRad);
    const float maxTan = std::tan(maxRad);
    const float invRange = 1.0f / std::max(maxTan - minTan, 1e-6f);
    const float maskMax = std::max(1e-4f, settings.maskMaxSnow);
    const int smoothIters = std::clamp(settings.smoothingIterations, 0, 16);

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSize = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSize, terrainSize * 0.5f);
    const int fillRadius = std::clamp(static_cast<int>(std::round(largestDetailM / cellSize)), 1, 64);
    const float invTwoCell = 1.0f / (2.0f * cellSize);

    // Phase 1: 元高さから slope を計算し、初期 thickness を求める。
    const std::vector<float> baseHeights = grid.heights;
    std::vector<float> thickness(cellCount, 0.0f);
    ParallelForRows(n, [&](int z) {
        const int zm = std::max(0, z - 1);
        const int zp = std::min(n - 1, z + 1);
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        const size_t rowAbove = static_cast<size_t>(zm) * static_cast<size_t>(n);
        const size_t rowBelow = static_cast<size_t>(zp) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const int xm = std::max(0, x - 1);
            const int xp = std::min(n - 1, x + 1);
            const float h_xm = baseHeights[rowBase + static_cast<size_t>(xm)];
            const float h_xp = baseHeights[rowBase + static_cast<size_t>(xp)];
            const float h_zm = baseHeights[rowAbove + static_cast<size_t>(x)];
            const float h_zp = baseHeights[rowBelow + static_cast<size_t>(x)];
            const float dhdx = (h_xp - h_xm) * invTwoCell;
            const float dhdz = (h_zp - h_zm) * invTwoCell;
            const float slopeTan = std::sqrt(dhdx * dhdx + dhdz * dhdz);

            const float t = std::clamp((slopeTan - minTan) * invRange, 0.0f, 1.0f);
            const float smoothT = t * t * (3.0f - 2.0f * t);
            const float snowFraction = 1.0f - smoothT;
            thickness[rowBase + static_cast<size_t>(x)] = emission * snowFraction;
        }
    });

    // Phase 2: snow envelope smoothing。
    //   surface = baseHeights + thickness を分離ガウスブラーでならし、
    //   max(surface, blurred) で出っ張りを保ちつつ溝を埋める。
    if (smoothIters > 0)
    {
        std::vector<float> surfA(cellCount);
        std::vector<float> surfB(cellCount);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                surfA[idx] = baseHeights[idx] + thickness[idx];
            }
        });
        for (int iter = 0; iter < smoothIters; ++iter)
        {
            const float sigma = std::max(1.0f, static_cast<float>(fillRadius) * 0.5f);
            const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
            ParallelForRows(n, [&](int z) {
                const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
                for (int x = 0; x < n; ++x)
                {
                    float sum = 0.0f;
                    float weightSum = 0.0f;
                    for (int ox = -fillRadius; ox <= fillRadius; ++ox)
                    {
                        const int sx = std::clamp(x + ox, 0, n - 1);
                        const float w = std::exp(-static_cast<float>(ox * ox) * invTwoSigma2);
                        sum += surfA[rowBase + static_cast<size_t>(sx)] * w;
                        weightSum += w;
                    }
                    surfB[rowBase + static_cast<size_t>(x)] = sum / std::max(weightSum, 1e-6f);
                }
            });
            ParallelForRows(n, [&](int z) {
                const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
                for (int x = 0; x < n; ++x)
                {
                    float sum = 0.0f;
                    float weightSum = 0.0f;
                    for (int oz = -fillRadius; oz <= fillRadius; ++oz)
                    {
                        const int sz = std::clamp(z + oz, 0, n - 1);
                        const float w = std::exp(-static_cast<float>(oz * oz) * invTwoSigma2);
                        sum += surfB[static_cast<size_t>(sz) * static_cast<size_t>(n) + static_cast<size_t>(x)] * w;
                        weightSum += w;
                    }
                    const float s11 = surfA[rowBase + static_cast<size_t>(x)];
                    const float blurred = sum / std::max(weightSum, 1e-6f);
                    surfA[rowBase + static_cast<size_t>(x)] = std::max(s11, blurred);
                }
            });
        }
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                thickness[idx] = std::max(0.0f, surfA[idx] - baseHeights[idx]);
            }
        });
    }

    // Phase 3: write smoothed snow thickness back to height and mask.
    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t idx = rowBase + static_cast<size_t>(x);
            grid.heights[idx] = baseHeights[idx] + thickness[idx];
            grid.mask[idx] = std::clamp(thickness[idx] / maskMax, 0.0f, 1.0f);
        }
    });
}

void SetSnowGpuEvaluator(SnowGpuEvaluator evaluator)
{
    g_snowGpuEvaluator = evaluator;
}
} // namespace rock