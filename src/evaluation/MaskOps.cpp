#include "MaskOps.h"

#include <algorithm>
#include <cmath>
#include <execution>
#include <numeric>
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
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int z) { fn(z); });
}
} // namespace

float SampleMaskBilinear(const MaskGrid& grid, float u, float v)
{
    if (grid.resolution <= 0)
    {
        return 0.0f;
    }
    const float fx = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(grid.resolution - 1);
    const float fy = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(grid.resolution - 1);
    const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, grid.resolution - 1);
    const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, grid.resolution - 1);
    const int x1 = std::min(x0 + 1, grid.resolution - 1);
    const int y1 = std::min(y0 + 1, grid.resolution - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float a = grid.values[static_cast<size_t>(y0) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x0)];
    const float b = grid.values[static_cast<size_t>(y0) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x1)];
    const float c = grid.values[static_cast<size_t>(y1) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x0)];
    const float d = grid.values[static_cast<size_t>(y1) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x1)];
    return std::lerp(std::lerp(a, b, tx), std::lerp(c, d, tx), ty);
}

MaskGrid ResampleMaskGrid(const MaskGrid& source, int targetResolution)
{
    if (source.resolution == targetResolution || source.resolution <= 0)
    {
        return source;
    }
    MaskGrid resampled;
    resampled.resolution = targetResolution;
    resampled.values.assign(static_cast<size_t>(targetResolution) * static_cast<size_t>(targetResolution), 0.0f);
    const float invDenom = targetResolution > 1 ? 1.0f / static_cast<float>(targetResolution - 1) : 0.0f;
    for (int z = 0; z < targetResolution; ++z)
    {
        const float v = static_cast<float>(z) * invDenom;
        for (int x = 0; x < targetResolution; ++x)
        {
            const float u = static_cast<float>(x) * invDenom;
            resampled.values[static_cast<size_t>(z) * static_cast<size_t>(targetResolution) + static_cast<size_t>(x)] =
                SampleMaskBilinear(source, u, v);
        }
    }
    return resampled;
}

MaskGrid BlendMaskGrids(const MaskGrid& a, const MaskGrid& b, MaskBlendMode mode, float intensity)
{
    if (a.resolution <= 0 && b.resolution <= 0) { return {}; }
    if (a.resolution <= 0) { return b; }
    if (b.resolution <= 0) { return a; }

    const int n = std::max(a.resolution, b.resolution);
    const MaskGrid& gridA = a.resolution == n ? a : ResampleMaskGrid(a, n);
    const MaskGrid& gridB = b.resolution == n ? b : ResampleMaskGrid(b, n);

    MaskGrid result;
    result.resolution = n;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    result.values.resize(cellCount);
    const float t = std::clamp(intensity, 0.0f, 1.0f);
    for (size_t i = 0; i < cellCount; ++i)
    {
        const float va = gridA.values[i];
        const float vb = gridB.values[i];
        float blended = va;
        switch (mode)
        {
        case MaskBlendMode::Add:
            blended = va + vb;
            break;
        case MaskBlendMode::Multiply:
            blended = va * vb;
            break;
        case MaskBlendMode::Min:
            blended = std::min(va, vb);
            break;
        case MaskBlendMode::Max:
            blended = std::max(va, vb);
            break;
        }
        result.values[i] = std::clamp(std::lerp(va, blended, t), 0.0f, 1.0f);
    }
    return result;
}

MaskGrid ApplyMaskLevels(const MaskGrid& source, const MaskLevelsSettings& settings)
{
    if (source.resolution <= 0 || source.values.empty())
    {
        return {};
    }

    const int n = source.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    MaskGrid result;
    result.resolution = n;
    result.values.assign(cellCount, 0.0f);

    float black = std::clamp(settings.blackPoint, 0.0f, 1.0f);
    float white = std::clamp(settings.whitePoint, 0.0f, 1.0f);
    if (white < black)
    {
        std::swap(black, white);
    }
    const float invRange = 1.0f / std::max(white - black, 0.0001f);
    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    const float exponent = 1.0f / gamma;
    const size_t count = std::min(cellCount, source.values.size());

    ParallelForRows(n, [&](int z) {
        const size_t row = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t i = row + static_cast<size_t>(x);
            if (i >= count)
            {
                continue;
            }
            float value = std::clamp((source.values[i] - black) * invRange, 0.0f, 1.0f);
            value = std::pow(value, exponent);
            if (settings.invert)
            {
                value = 1.0f - value;
            }
            result.values[i] = value;
        }
    });

    return result;
}
} // namespace rock
