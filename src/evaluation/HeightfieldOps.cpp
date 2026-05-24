#include "HeightfieldOps.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <numeric>

namespace rock
{
namespace
{
void NormalizeField(std::vector<float>& field)
{
    float maxValue = 0.0f;
    for (float value : field)
    {
        maxValue = std::max(maxValue, value);
    }
    if (maxValue > 0.000001f)
    {
        for (float& value : field)
        {
            value = std::clamp(value / maxValue, 0.0f, 1.0f);
        }
    }
}

template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
}
} // namespace

void NormalizeHeightfieldFields(HeightfieldGrid& grid)
{
    NormalizeField(grid.mask);
    NormalizeField(grid.deposits);
    NormalizeField(grid.flows);
    NormalizeField(grid.age);
}

void SelectHeightfieldPreviewField(HeightfieldGrid& grid, HeightfieldPreviewField previewField)
{
    if (previewField == HeightfieldPreviewField::Deposits && !grid.deposits.empty())
    {
        grid.mask = grid.deposits;
    }
    else if (previewField == HeightfieldPreviewField::Flows && !grid.flows.empty())
    {
        grid.mask = grid.flows;
    }
    else if (previewField == HeightfieldPreviewField::Age && !grid.age.empty())
    {
        grid.mask = grid.age;
    }
    else if (previewField == HeightfieldPreviewField::UniqueMask && !grid.uniqueMask.empty())
    {
        grid.mask = grid.uniqueMask;
    }
}

void ApplyHeightmapBlur(HeightfieldGrid& grid, const HeightmapBlurSettings& settings)
{
    const int n = grid.resolution;
    if (n < 2 || grid.heights.size() < static_cast<size_t>(n * n) || settings.radius <= 0.0f || settings.strength <= 0.0f || settings.iterations <= 0)
    {
        return;
    }

    const float radius = std::clamp(settings.radius, 0.0f, 128.0f);
    const float strength = std::clamp(settings.strength, 0.0f, 1.0f);
    const int iterations = std::clamp(settings.iterations, 0, 64);
    const int kernelRadius = std::clamp(static_cast<int>(std::ceil(radius)), 1, std::max(1, n - 1));
    const float sigma = std::max(radius * 0.5f, 0.5f);

    std::vector<float> weights(static_cast<size_t>(kernelRadius) + 1u);
    float weightSum = 0.0f;
    for (int offset = 0; offset <= kernelRadius; ++offset)
    {
        const float x = static_cast<float>(offset) / sigma;
        const float weight = std::exp(-0.5f * x * x);
        weights[static_cast<size_t>(offset)] = weight;
        weightSum += offset == 0 ? weight : weight * 2.0f;
    }
    for (float& weight : weights)
    {
        weight /= weightSum;
    }

    std::vector<float> temp(grid.heights.size(), 0.0f);
    std::vector<float> blurred(grid.heights.size(), 0.0f);
    const auto indexAt = [n](int x, int z) {
        return static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
    };

    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        const std::vector<float>& source = grid.heights;
        ParallelForRows(n, [&](int z) {
            for (int x = 0; x < n; ++x)
            {
                float value = source[indexAt(x, z)] * weights[0];
                for (int offset = 1; offset <= kernelRadius; ++offset)
                {
                    const int left = std::clamp(x - offset, 0, n - 1);
                    const int right = std::clamp(x + offset, 0, n - 1);
                    const float weight = weights[static_cast<size_t>(offset)];
                    value += (source[indexAt(left, z)] + source[indexAt(right, z)]) * weight;
                }
                temp[indexAt(x, z)] = value;
            }
        });

        ParallelForRows(n, [&](int z) {
            for (int x = 0; x < n; ++x)
            {
                float value = temp[indexAt(x, z)] * weights[0];
                for (int offset = 1; offset <= kernelRadius; ++offset)
                {
                    const int up = std::clamp(z - offset, 0, n - 1);
                    const int down = std::clamp(z + offset, 0, n - 1);
                    const float weight = weights[static_cast<size_t>(offset)];
                    value += (temp[indexAt(x, up)] + temp[indexAt(x, down)]) * weight;
                }
                blurred[indexAt(x, z)] = value;
            }
        });

        ParallelForRows(n, [&](int z) {
            const size_t row = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t i = row + static_cast<size_t>(x);
                grid.heights[i] = std::lerp(source[i], blurred[i], strength);
            }
        });
    }
}

std::vector<float> BoxBlurHeights(const HeightfieldGrid& grid, int radius)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n <= 0 || grid.heights.size() < cellCount || radius <= 0)
    {
        return grid.heights;
    }

    radius = std::clamp(radius, 1, 64);
    std::vector<float> horizontal(cellCount, 0.0f);
    std::vector<float> blurred(cellCount, 0.0f);

    ParallelForRows(n, [&](int z) {
        std::vector<float> prefix(static_cast<size_t>(n) + 1u, 0.0f);
        const size_t row = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            prefix[static_cast<size_t>(x) + 1u] = prefix[static_cast<size_t>(x)] + grid.heights[row + static_cast<size_t>(x)];
        }
        for (int x = 0; x < n; ++x)
        {
            const int left = std::max(0, x - radius);
            const int right = std::min(n - 1, x + radius);
            const float sum = prefix[static_cast<size_t>(right) + 1u] - prefix[static_cast<size_t>(left)];
            horizontal[row + static_cast<size_t>(x)] = sum / static_cast<float>(right - left + 1);
        }
    });

    ParallelForRows(n, [&](int x) {
        std::vector<float> prefix(static_cast<size_t>(n) + 1u, 0.0f);
        for (int z = 0; z < n; ++z)
        {
            prefix[static_cast<size_t>(z) + 1u] = prefix[static_cast<size_t>(z)] +
                horizontal[static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x)];
        }
        for (int z = 0; z < n; ++z)
        {
            const int top = std::max(0, z - radius);
            const int bottom = std::min(n - 1, z + radius);
            const float sum = prefix[static_cast<size_t>(bottom) + 1u] - prefix[static_cast<size_t>(top)];
            blurred[static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x)] =
                sum / static_cast<float>(bottom - top + 1);
        }
    });

    return blurred;
}

void ApplyMaskCurvature(HeightfieldGrid& grid, const MaskCurvatureSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n <= 0 || grid.heights.size() < cellCount)
    {
        return;
    }

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSize = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSize, terrainSize * 0.5f);
    const int radius = std::clamp(static_cast<int>(std::round(largestDetailM / cellSize)), 1, 64);
    const float sensitivity = std::max(settings.sensitivityMeters, 0.0001f);
    const float threshold = std::clamp(settings.threshold, 0.0f, 0.99f);
    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    const std::vector<float> blurred = BoxBlurHeights(grid, radius);

    grid.mask.assign(cellCount, 0.0f);
    ParallelForRows(n, [&](int z) {
        const size_t row = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t i = row + static_cast<size_t>(x);
            const float delta = grid.heights[i] - blurred[i];
            float curvature = 0.0f;
            switch (settings.mode)
            {
            case MaskCurvatureMode::Ridges:
                curvature = delta;
                break;
            case MaskCurvatureMode::Valleys:
                curvature = -delta;
                break;
            case MaskCurvatureMode::Absolute:
            default:
                curvature = std::abs(delta);
                break;
            }

            float value = std::clamp(curvature / sensitivity, 0.0f, 1.0f);
            value = std::clamp((value - threshold) / std::max(1.0f - threshold, 0.0001f), 0.0f, 1.0f);
            grid.mask[i] = std::pow(value, gamma);
        }
    });
}

void ApplyMaskSlope(HeightfieldGrid& grid, const MaskSlopeSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount)
    {
        return;
    }

    float minDeg = std::clamp(settings.slopeMinDeg, 0.0f, 89.9f);
    float maxDeg = std::clamp(settings.slopeMaxDeg, 0.0f, 89.9f);
    if (maxDeg < minDeg)
    {
        std::swap(minDeg, maxDeg);
    }

    const float invRange = 1.0f / std::max(maxDeg - minDeg, 0.0001f);
    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    const float exponent = 1.0f / gamma;
    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSize = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float invTwoCell = 1.0f / (2.0f * cellSize);
    const float radToDeg = 57.29577951308232f;
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, 0.0f, terrainSize * 0.5f);
    const int blurRadius = largestDetailM > 0.0f
        ? std::clamp(static_cast<int>(std::round(largestDetailM / cellSize)), 1, 64)
        : 0;
    const std::vector<float> blurred = blurRadius > 0 ? BoxBlurHeights(grid, blurRadius) : std::vector<float>{};
    const std::vector<float>& heights = blurRadius > 0 ? blurred : grid.heights;

    grid.mask.assign(cellCount, 0.0f);
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
            const float hXm = heights[rowBase + static_cast<size_t>(xm)];
            const float hXp = heights[rowBase + static_cast<size_t>(xp)];
            const float hZm = heights[rowAbove + static_cast<size_t>(x)];
            const float hZp = heights[rowBelow + static_cast<size_t>(x)];
            const float dhdx = (hXp - hXm) * invTwoCell;
            const float dhdz = (hZp - hZm) * invTwoCell;
            const float slopeTan = std::sqrt(dhdx * dhdx + dhdz * dhdz);
            const float slopeDeg = std::atan(slopeTan) * radToDeg;

            float value = std::clamp((slopeDeg - minDeg) * invRange, 0.0f, 1.0f);
            value = value * value * (3.0f - 2.0f * value);
            value = std::pow(value, exponent);
            if (settings.invert)
            {
                value = 1.0f - value;
            }
            grid.mask[rowBase + static_cast<size_t>(x)] = value;
        }
    });
}

void ApplyMaskHeight(HeightfieldGrid& grid, const MaskHeightSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 1 || grid.heights.size() < cellCount)
    {
        return;
    }

    float minMeters = settings.heightMinMeters;
    float maxMeters = settings.heightMaxMeters;
    if (maxMeters < minMeters)
    {
        std::swap(minMeters, maxMeters);
    }

    const auto smoothstep = [](float edge0, float edge1, float x) {
        const float range = std::max(edge1 - edge0, 0.0001f);
        const float t = std::clamp((x - edge0) / range, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    const float feather = std::max(settings.featherMeters, 0.0f);
    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    const float exponent = 1.0f / gamma;
    float fullRangeMin = 0.0f;
    float fullRangeInv = 0.0f;
    if (settings.useFullRange)
    {
        const auto [minIt, maxIt] = std::minmax_element(grid.heights.begin(), grid.heights.begin() + static_cast<std::ptrdiff_t>(cellCount));
        fullRangeMin = *minIt;
        const float range = *maxIt - *minIt;
        fullRangeInv = (range > 0.0001f) ? (1.0f / range) : 0.0f;
    }

    grid.mask.assign(cellCount, 0.0f);
    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t index = rowBase + static_cast<size_t>(x);
            const float h = grid.heights[index];
            float value = 0.0f;
            if (settings.useFullRange)
            {
                value = std::clamp((h - fullRangeMin) * fullRangeInv, 0.0f, 1.0f);
            }
            else if (feather <= 0.0f)
            {
                value = (h >= minMeters && h <= maxMeters) ? 1.0f : 0.0f;
            }
            else
            {
                const float lower = smoothstep(minMeters - feather, minMeters, h);
                const float upper = 1.0f - smoothstep(maxMeters, maxMeters + feather, h);
                value = std::clamp(std::min(lower, upper), 0.0f, 1.0f);
            }
            value = std::pow(value, exponent);
            if (settings.invert)
            {
                value = 1.0f - value;
            }
            grid.mask[index] = value;
        }
    });
}
} // namespace rock
