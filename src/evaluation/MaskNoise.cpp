#include "MaskNoise.h"

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
MaskNoiseGpuEvaluator g_maskNoiseGpuEvaluator = nullptr;

template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), [&](int z) { fn(z); });
}

// 2D Perlin / fBM noise used by the Mask Noise node. Unlike the directional
// noise in the erosion-noise namespace (which warps gradients to follow
// existing terrain), this is a plain isotropic Perlin field driving fBM.
namespace mask_noise
{
inline uint32_t Hash3(int32_t x, int32_t y, int32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x27d4eb2du;
    h ^= static_cast<uint32_t>(y) * 0x165667b1u;
    h ^= static_cast<uint32_t>(seed) * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

inline std::array<float, 2> Gradient2(int32_t x, int32_t y, int32_t seed)
{
    const uint32_t h = Hash3(x, y, seed);
    constexpr float twoPi = 6.28318530717958647692f;
    const float angle = (static_cast<float>(h & 0xFFFFu) / 65535.0f) * twoPi;
    return {std::cos(angle), std::sin(angle)};
}

inline float Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

inline float Perlin2D(float x, float y, int32_t seed)
{
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int32_t x0 = static_cast<int32_t>(fx);
    const int32_t y0 = static_cast<int32_t>(fy);
    const int32_t x1 = x0 + 1;
    const int32_t y1 = y0 + 1;
    const float dx = x - fx;
    const float dy = y - fy;

    const auto g00 = Gradient2(x0, y0, seed);
    const auto g10 = Gradient2(x1, y0, seed);
    const auto g01 = Gradient2(x0, y1, seed);
    const auto g11 = Gradient2(x1, y1, seed);

    const float v00 = g00[0] * dx + g00[1] * dy;
    const float v10 = g10[0] * (dx - 1.0f) + g10[1] * dy;
    const float v01 = g01[0] * dx + g01[1] * (dy - 1.0f);
    const float v11 = g11[0] * (dx - 1.0f) + g11[1] * (dy - 1.0f);

    const float u = Fade(dx);
    const float v = Fade(dy);
    return std::lerp(std::lerp(v00, v10, u), std::lerp(v01, v11, u), v);
}

inline float Fbm2D(float x, float y, int octaves, float lacunarity, float persistence, int32_t seed)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float maxAmplitude = 0.0f;
    float freq = 1.0f;
    for (int i = 0; i < octaves; ++i)
    {
        total += Perlin2D(x * freq, y * freq, seed + i * 1013) * amplitude;
        maxAmplitude += amplitude;
        amplitude *= persistence;
        freq *= lacunarity;
    }
    return maxAmplitude > 0.0f ? total / maxAmplitude : 0.0f;
}
} // namespace mask_noise
} // namespace

MaskGrid GenerateMaskNoise(const MaskNoiseSettings& settings)
{
    MaskGrid grid;
    grid.resolution = std::clamp(settings.simulationResolution, 2, 2048);
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.values.assign(cellCount, 0.0f);

    // GPU compute path. Falls back to the CPU branch below if the evaluator
    // hasn't been registered (no D3D12 device) or returns failure.
    if (settings.backend == MaskNoiseBackend::GpuCompute && g_maskNoiseGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_maskNoiseGpuEvaluator(grid, settings, &ignoredError))
        {
            return grid;
        }
        // Fall through on GPU failure; clear any partial writes first.
        grid.values.assign(cellCount, 0.0f);
    }

    const int octaves = std::clamp(settings.octaves, 1, 12);
    const float lacunarity = std::max(settings.lacunarity, 0.0f);
    const float persistence = std::clamp(settings.persistence, 0.0f, 1.0f);
    const float frequency = std::max(settings.frequency, 0.0f);
    const int32_t seed = settings.seed;
    const float invDenom = grid.resolution > 1 ? 1.0f / static_cast<float>(grid.resolution - 1) : 0.0f;

    const int n = grid.resolution;
    ParallelForRows(n, [&](int z) {
        const float v = static_cast<float>(z) * invDenom;
        for (int x = 0; x < n; ++x)
        {
            const float u = static_cast<float>(x) * invDenom;
            // Perlin / fBM output is roughly in [-1, 1] after normalisation, so
            // remap to [0, 1] for use as a Mask channel.
            const float ns = mask_noise::Fbm2D(u * frequency, v * frequency, octaves, lacunarity, persistence, seed);
            grid.values[static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x)] =
                std::clamp(ns * 0.5f + 0.5f, 0.0f, 1.0f);
        }
    });
    return grid;
}

void SetMaskNoiseGpuEvaluator(MaskNoiseGpuEvaluator evaluator)
{
    g_maskNoiseGpuEvaluator = evaluator;
}
} // namespace rock
