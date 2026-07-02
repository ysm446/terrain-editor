#include "Snow.h"

#include "GranularSettle.h"

#include <algorithm>
#include <string>

namespace rock
{
namespace
{
SnowGpuEvaluator g_snowGpuEvaluator = nullptr;
} // namespace

// Snow node.
//
// This is a compact material-transport model rather than a direct slope mask.
// Snow is injected as thickness, then unstable cells move part of that snow to
// the steepest lower neighbour until the snow surface is below the motion
// slope limit. The output mask is coverage-like: mostly black or white, with a
// controlled grey feather near the exposed-ground edge.
// 再配分コアは Soil ノードと共有 (GranularSettle)。
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

    GranularSettleParams params;
    params.emissionAmount = settings.emissionAmount;
    params.iterationCount = settings.iterationCount;
    params.emissionTime = settings.emissionTime;
    params.settlingPasses = settings.smoothingIterations;
    params.motionSlopeLimitDeg = settings.motionSlopeLimitDeg;
    params.transportRate = settings.transportRate;
    params.surfaceSmoothing = settings.surfaceSmoothing;
    params.maskThresholdM = settings.maskThresholdM;
    params.maskFeatherM = settings.maskFeatherM;
    params.largestDetailLevelM = settings.largestDetailLevelM;
    params.slopeDependentEmission = 0.0f;
    ApplyGranularSettle(grid, params);
}

void SetSnowGpuEvaluator(SnowGpuEvaluator evaluator)
{
    g_snowGpuEvaluator = evaluator;
}
} // namespace rock
