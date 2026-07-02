#include "Soil.h"

#include "GranularSettle.h"

#include <algorithm>
#include <string>
#include <vector>

namespace rock
{
namespace
{
SoilGpuEvaluator g_soilGpuEvaluator = nullptr;

// Thickness マスク: 堆積厚 (= 出力高さ - 入力高さ) を max で 0..1 正規化する。
// GPU / CPU どちらの経路でも高さ差から復元できるので、バックエンドに依存しない。
void WriteThicknessMask(HeightfieldGrid& grid, const std::vector<float>& baseHeights)
{
    const size_t cellCount = grid.heights.size();
    float maxThickness = 0.0f;
    for (size_t idx = 0; idx < cellCount; ++idx)
    {
        maxThickness = std::max(maxThickness, grid.heights[idx] - baseHeights[idx]);
    }
    if (maxThickness <= 0.0f)
    {
        grid.mask.assign(cellCount, 0.0f);
        return;
    }
    grid.mask.resize(cellCount);
    const float invMax = 1.0f / maxThickness;
    for (size_t idx = 0; idx < cellCount; ++idx)
    {
        grid.mask[idx] = std::clamp((grid.heights[idx] - baseHeights[idx]) * invMax, 0.0f, 1.0f);
    }
}
} // namespace

// Soil node.
//
// 被覆型の表土ノード。Snow と同じ注入 -> 再配分コア (GranularSettle) を
// 土向け既定値で使い、緩斜面・上面に表土をかぶせて急斜面・崖では剥がして
// 岩盤を露出させる。Slope-Dependent Emission で基盤の急な面ほど注入を
// 減らせるため、尾根・崖の岩盤露出がより早く出る。
void ApplySoil(HeightfieldGrid& grid, const SoilSettings& settings)
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

    if (settings.backend == SoilBackend::GpuCompute && g_soilGpuEvaluator != nullptr)
    {
        const std::vector<float> baseHeights = grid.heights;
        std::string ignoredError;
        if (g_soilGpuEvaluator(grid, settings, &ignoredError))
        {
            if (settings.maskMode == SoilMaskMode::Thickness)
            {
                WriteThicknessMask(grid, baseHeights);
            }
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    GranularSettleParams params;
    params.emissionAmount = settings.emissionAmount;
    params.iterationCount = settings.iterationCount;
    params.emissionTime = settings.emissionTime;
    params.settlingPasses = settings.settlingPasses;
    params.motionSlopeLimitDeg = settings.motionSlopeLimitDeg;
    params.transportRate = settings.transportRate;
    params.surfaceSmoothing = settings.surfaceSmoothing;
    params.maskThresholdM = settings.maskThresholdM;
    params.maskFeatherM = settings.maskFeatherM;
    params.largestDetailLevelM = settings.largestDetailLevelM;
    params.slopeDependentEmission = settings.slopeDependentEmission;

    std::vector<float> thickness;
    ApplyGranularSettle(grid, params, settings.maskMode == SoilMaskMode::Thickness ? &thickness : nullptr);

    if (settings.maskMode == SoilMaskMode::Thickness)
    {
        float maxThickness = 0.0f;
        for (const float value : thickness)
        {
            maxThickness = std::max(maxThickness, value);
        }
        if (maxThickness <= 0.0f)
        {
            grid.mask.assign(cellCount, 0.0f);
            return;
        }
        const float invMax = 1.0f / maxThickness;
        for (size_t idx = 0; idx < cellCount; ++idx)
        {
            grid.mask[idx] = std::clamp(thickness[idx] * invMax, 0.0f, 1.0f);
        }
    }
}

void SetSoilGpuEvaluator(SoilGpuEvaluator evaluator)
{
    g_soilGpuEvaluator = evaluator;
}
} // namespace rock
