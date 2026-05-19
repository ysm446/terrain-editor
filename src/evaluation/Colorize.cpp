#include "Colorize.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <execution>
#include <numeric>
#include <string>
#include <vector>

namespace rock
{
namespace
{
ColorizeGpuEvaluator g_colorizeGpuEvaluator = nullptr;

template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int z) { fn(z); });
}

std::array<float, 3> SampleColorGradient(const std::vector<ColorStop>& stops, float t)
{
    if (stops.empty())
    {
        return {0.0f, 0.0f, 0.0f};
    }
    if (t <= stops.front().position)
    {
        return {stops.front().r, stops.front().g, stops.front().b};
    }
    if (t >= stops.back().position)
    {
        return {stops.back().r, stops.back().g, stops.back().b};
    }
    for (size_t i = 0; i + 1 < stops.size(); ++i)
    {
        if (t <= stops[i + 1].position)
        {
            const float span = stops[i + 1].position - stops[i].position;
            const float alpha = span > 0.0f ? (t - stops[i].position) / span : 0.0f;
            return {
                stops[i].r + alpha * (stops[i + 1].r - stops[i].r),
                stops[i].g + alpha * (stops[i + 1].g - stops[i].g),
                stops[i].b + alpha * (stops[i + 1].b - stops[i].b),
            };
        }
    }
    return {stops.back().r, stops.back().g, stops.back().b};
}
} // namespace

ColorGrid GenerateColorize(
    const ColorizeSettings& settings,
    const MaskGrid& gradientMask,
    const MaskGrid* mask,
    const ColorGrid* baseColor)
{
    ColorGrid grid;
    if (gradientMask.resolution <= 0)
    {
        return grid;
    }
    grid.resolution = gradientMask.resolution;
    const int res = grid.resolution;
    const size_t n = static_cast<size_t>(res) * static_cast<size_t>(res);
    grid.pixels.resize(n * 4);

    const bool hasMask = (mask != nullptr && mask->resolution == res);
    const bool hasBaseColor = baseColor != nullptr &&
        baseColor->resolution > 0 &&
        baseColor->pixels.size() >= static_cast<size_t>(baseColor->resolution) * static_cast<size_t>(baseColor->resolution) * 4u;
    const std::vector<ColorStop>& stops = settings.stops;
    if (settings.backend == ColorizeBackend::GpuCompute && g_colorizeGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_colorizeGpuEvaluator(grid, settings, gradientMask, hasMask ? mask : nullptr, hasBaseColor ? baseColor : nullptr, &ignoredError))
        {
            return grid;
        }
        grid.pixels.assign(n * 4, 0);
    }

    ParallelForRows(res, [&](int z) {
        for (int x = 0; x < res; ++x)
        {
            const size_t i = static_cast<size_t>(z) * static_cast<size_t>(res) + static_cast<size_t>(x);
            const float t = std::clamp(gradientMask.values[i], 0.0f, 1.0f);
            const auto [r, g, b] = SampleColorGradient(stops, t);
            const float a = hasMask ? std::clamp(mask->values[i], 0.0f, 1.0f) : 1.0f;
            float outR = r;
            float outG = g;
            float outB = b;
            float outA = 1.0f;
            if (hasBaseColor)
            {
                const float u = res > 1 ? static_cast<float>(x) / static_cast<float>(res - 1) : 0.0f;
                const float v = res > 1 ? static_cast<float>(z) / static_cast<float>(res - 1) : 0.0f;
                const int baseX = std::clamp(static_cast<int>(std::round(u * static_cast<float>(baseColor->resolution - 1))), 0, baseColor->resolution - 1);
                const int baseZ = std::clamp(static_cast<int>(std::round(v * static_cast<float>(baseColor->resolution - 1))), 0, baseColor->resolution - 1);
                const size_t bi = (static_cast<size_t>(baseZ) * static_cast<size_t>(baseColor->resolution) + static_cast<size_t>(baseX)) * 4u;
                const float br = static_cast<float>(baseColor->pixels[bi + 0u]) / 255.0f;
                const float bg = static_cast<float>(baseColor->pixels[bi + 1u]) / 255.0f;
                const float bb = static_cast<float>(baseColor->pixels[bi + 2u]) / 255.0f;
                outR = std::lerp(br, r, a);
                outG = std::lerp(bg, g, a);
                outB = std::lerp(bb, b, a);
            }
            grid.pixels[i * 4 + 0] = static_cast<uint8_t>(std::clamp(outR * 255.0f, 0.0f, 255.0f));
            grid.pixels[i * 4 + 1] = static_cast<uint8_t>(std::clamp(outG * 255.0f, 0.0f, 255.0f));
            grid.pixels[i * 4 + 2] = static_cast<uint8_t>(std::clamp(outB * 255.0f, 0.0f, 255.0f));
            grid.pixels[i * 4 + 3] = static_cast<uint8_t>(std::clamp(outA * 255.0f, 0.0f, 255.0f));
        }
    });
    return grid;
}

void SetColorizeGpuEvaluator(ColorizeGpuEvaluator evaluator)
{
    g_colorizeGpuEvaluator = evaluator;
}
} // namespace rock
