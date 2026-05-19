#include "node_graph.h"

#include "evaluation/Colorize.h"
#include "evaluation/HeightmapSource.h"
#include "evaluation/MaskNoise.h"
#include "evaluation/MaskOps.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
#include <format>
#include <iterator>
#include <limits>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>


namespace rock
{
namespace
{
MultiScaleErosionGpuEvaluator g_mseGpuEvaluator = nullptr;
SedimentGpuEvaluator g_sedimentGpuEvaluator = nullptr;
RockGpuEvaluator g_rockGpuEvaluator = nullptr;
ScatterGpuEvaluator g_scatterGpuEvaluator = nullptr;
MaskFluvialGpuEvaluator g_maskFluvialGpuEvaluator = nullptr;
SnowGpuEvaluator g_snowGpuEvaluator = nullptr;
std::atomic<GraphId> g_currentlyEvaluatingNodeId{0};

void HashCombine(uint64_t& seed, uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

uint64_t HashFloat(float value)
{
    return static_cast<uint64_t>(std::hash<float>{}(value));
}

uint64_t HashHeightmapSettings(const HeightmapLoadSettings& settings, int resolution, float terrainSizeMeters)
{
    uint64_t hash = 1469598103934665603ull;
    const std::string resolvedPath = ResolveAssetPath(settings.path);
    HashCombine(hash, static_cast<uint64_t>(std::hash<std::string>{}(resolvedPath)));
    HashCombine(hash, HashFloat(settings.scaleMeters));
    HashCombine(hash, HashFloat(settings.relativeVerticalScalePercent));
    HashCombine(hash, HashFloat(settings.verticalOffsetMeters));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    HashCombine(hash, HashFloat(terrainSizeMeters));
    return hash;
}

uint64_t HashShapeSettings(const ShapeSettings& settings, int resolution, float terrainSizeMeters)
{
    uint64_t hash = 1469598103934665603ull;
    HashCombine(hash, static_cast<uint64_t>(settings.kind));
    HashCombine(hash, HashFloat(settings.scaleMeters));
    HashCombine(hash, HashFloat(settings.relativeHeightPercent));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    HashCombine(hash, HashFloat(terrainSizeMeters));
    return hash;
}

uint64_t HashHeightmapBlurSettings(const HeightmapBlurSettings& settings, int resolution)
{
    uint64_t hash = 2166136261ull;
    HashCombine(hash, HashFloat(settings.radius));
    HashCombine(hash, HashFloat(settings.strength));
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashMaskNoiseSettings(const MaskNoiseSettings& settings, int resolution)
{
    uint64_t hash = 1099511628211ull;
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
    HashCombine(hash, static_cast<uint64_t>(settings.octaves));
    HashCombine(hash, HashFloat(settings.frequency));
    HashCombine(hash, HashFloat(settings.lacunarity));
    HashCombine(hash, HashFloat(settings.persistence));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    return hash;
}

uint64_t HashMaskBlendSettings(const MaskBlendSettings& settings)
{
    uint64_t hash = 16777619ull;
    HashCombine(hash, static_cast<uint64_t>(settings.mode));
    HashCombine(hash, HashFloat(settings.intensity));
    return hash;
}

uint64_t HashMaskCurvatureSettings(const MaskCurvatureSettings& settings, int resolution)
{
    uint64_t hash = 1099511628211ull;
    HashCombine(hash, static_cast<uint64_t>(settings.mode));
    HashCombine(hash, HashFloat(settings.largestDetailLevelM));
    HashCombine(hash, HashFloat(settings.sensitivityMeters));
    HashCombine(hash, HashFloat(settings.threshold));
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashMaskLevelsSettings(const MaskLevelsSettings& settings)
{
    uint64_t hash = 1469598103934665603ull;
    HashCombine(hash, HashFloat(settings.blackPoint));
    HashCombine(hash, HashFloat(settings.whitePoint));
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, static_cast<uint64_t>(settings.invert ? 1 : 0));
    return hash;
}

uint64_t HashMaskSlopeSettings(const MaskSlopeSettings& settings, int resolution)
{
    uint64_t hash = 7809847782465536322ull;
    HashCombine(hash, HashFloat(settings.largestDetailLevelM));
    HashCombine(hash, HashFloat(settings.slopeMinDeg));
    HashCombine(hash, HashFloat(settings.slopeMaxDeg));
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, static_cast<uint64_t>(settings.invert ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashMaskHeightSettings(const MaskHeightSettings& settings, int resolution)
{
    uint64_t hash = 10723151780598845931ull;
    HashCombine(hash, static_cast<uint64_t>(settings.useFullRange ? 1 : 0));
    HashCombine(hash, HashFloat(settings.heightMinMeters));
    HashCombine(hash, HashFloat(settings.heightMaxMeters));
    HashCombine(hash, HashFloat(settings.featherMeters));
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, static_cast<uint64_t>(settings.invert ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashCrumblingSettings(const CrumblingSettings& settings, int resolution)
{
    uint64_t hash = 1493897469210471337ull;
    HashCombine(hash, static_cast<uint64_t>(settings.physicsCount));
    HashCombine(hash, HashFloat(settings.debrisAmount));
    HashCombine(hash, HashFloat(settings.debrisSizeMinM));
    HashCombine(hash, HashFloat(settings.debrisSizeMaxM));
    HashCombine(hash, static_cast<uint64_t>(settings.style));
    HashCombine(hash, HashFloat(settings.gravity));
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashRockSettings(const RockSettings& settings, int resolution)
{
    uint64_t hash = 6364136223846793005ull;
    HashCombine(hash, static_cast<uint64_t>(settings.style));
    HashCombine(hash, static_cast<uint64_t>(settings.orientationRule));
    HashCombine(hash, static_cast<uint64_t>(settings.layerCount));
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
    HashCombine(hash, HashFloat(settings.density));
    HashCombine(hash, HashFloat(settings.coverage));
    HashCombine(hash, HashFloat(settings.rockSizeMinM));
    HashCombine(hash, HashFloat(settings.rockSizeMaxM));
    HashCombine(hash, HashFloat(settings.rockHeight));
    HashCombine(hash, HashFloat(settings.heightJitter));
    HashCombine(hash, HashFloat(settings.rotationVariation));
    HashCombine(hash, HashFloat(settings.aspectVariation));
    HashCombine(hash, HashFloat(settings.edgeSharpness));
    HashCombine(hash, HashFloat(settings.bumpiness));
    HashCombine(hash, HashFloat(settings.facetSharpness));
    HashCombine(hash, HashFloat(settings.facetScale));
    HashCombine(hash, HashFloat(settings.groundDetailLevelM));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashScatterSettings(const ScatterSettings& settings, int resolution)
{
    uint64_t hash = 1442695040888963407ull;
    HashCombine(hash, static_cast<uint64_t>(settings.shapeType));
    HashCombine(hash, static_cast<uint64_t>(settings.orientationRule));
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
    HashCombine(hash, HashFloat(settings.density));
    HashCombine(hash, HashFloat(settings.coverage));
    HashCombine(hash, HashFloat(settings.sizeMinM));
    HashCombine(hash, HashFloat(settings.sizeMaxM));
    HashCombine(hash, HashFloat(settings.height));
    HashCombine(hash, HashFloat(settings.heightJitter));
    HashCombine(hash, HashFloat(settings.rotationVariation));
    HashCombine(hash, HashFloat(settings.aspectVariation));
    HashCombine(hash, HashFloat(settings.groundDetailLevelM));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashSedimentSettings(const SedimentSettings& settings, int resolution)
{
    uint64_t hash = 1099511628211ull;
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, static_cast<uint64_t>(settings.stabilizationIterations));
    HashCombine(hash, HashFloat(settings.largestDetailLevelM));
    HashCombine(hash, HashFloat(settings.emissionAmountM));
    HashCombine(hash, HashFloat(settings.emissionTime));
    HashCombine(hash, HashFloat(settings.sedimentViscosity));
    HashCombine(hash, static_cast<uint64_t>(settings.convertTerrainToSediment ? 1 : 0));
    HashCombine(hash, HashFloat(settings.maskContrast));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashSnowSettings(const SnowSettings& settings, int resolution)
{
    uint64_t hash = 14695981039346656037ull;
    HashCombine(hash, HashFloat(settings.emissionAmount));
    HashCombine(hash, HashFloat(settings.slopeLimitMinDeg));
    HashCombine(hash, HashFloat(settings.slopeLimitMaxDeg));
    HashCombine(hash, HashFloat(settings.maskMaxSnow));
    HashCombine(hash, static_cast<uint64_t>(settings.smoothingIterations));
    HashCombine(hash, HashFloat(settings.largestDetailLevelM));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashColorizeSettings(const ColorizeSettings& settings)
{
    uint64_t hash = 7450123456789012345ull;
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    for (const ColorStop& stop : settings.stops)
    {
        HashCombine(hash, HashFloat(stop.position));
        HashCombine(hash, HashFloat(stop.r));
        HashCombine(hash, HashFloat(stop.g));
        HashCombine(hash, HashFloat(stop.b));
    }
    return hash;
}

uint64_t HashMaskFluvialSettings(const MaskFluvialSettings& settings, int resolution)
{
    uint64_t hash = 8589869056ull;
    HashCombine(hash, static_cast<uint64_t>(settings.simulationMode));
    HashCombine(hash, static_cast<uint64_t>(settings.outputCurve));
    HashCombine(hash, HashFloat(settings.accumulationThreshold));
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, HashFloat(settings.softness));
    HashCombine(hash, HashFloat(settings.power));
    HashCombine(hash, HashFloat(settings.largestDetailLevelM));
    HashCombine(hash, HashFloat(settings.mfdExponent));
    HashCombine(hash, static_cast<uint64_t>(settings.particleCount));
    HashCombine(hash, static_cast<uint64_t>(settings.particleLifetime));
    HashCombine(hash, HashFloat(settings.particleInertia));
    HashCombine(hash, HashFloat(settings.particleStepLengthM));
    HashCombine(hash, static_cast<uint64_t>(settings.particleSeed));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashMultiScaleErosionSettings(const MultiScaleErosionSettings& settings, int resolution)
{
    uint64_t hash = 11400714819323198485ull;
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, static_cast<uint64_t>(settings.enableStreamPower ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.enableThermal ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.enableDeposition ? 1 : 0));
    HashCombine(hash, HashFloat(settings.speStrength));
    HashCombine(hash, HashFloat(settings.streamExponent));
    HashCombine(hash, HashFloat(settings.slopeExponent));
    HashCombine(hash, HashFloat(settings.maxStreamPower));
    HashCombine(hash, HashFloat(settings.flowExponent));
    HashCombine(hash, HashFloat(settings.speTimeStep));
    HashCombine(hash, HashFloat(settings.thermalAngleDegrees));
    HashCombine(hash, HashFloat(settings.thermalStrength));
    HashCombine(hash, static_cast<uint64_t>(settings.thermalNoisifyAngle ? 1 : 0));
    HashCombine(hash, HashFloat(settings.thermalNoiseMin));
    HashCombine(hash, HashFloat(settings.thermalNoiseMax));
    HashCombine(hash, HashFloat(settings.thermalNoiseWavelength));
    HashCombine(hash, HashFloat(settings.depositionStrength));
    HashCombine(hash, HashFloat(settings.rain));
    HashCombine(hash, static_cast<uint64_t>(settings.useMultigrid ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}


float SampleHeightfieldValue(const std::vector<float>& values, int resolution, float u, float v)
{
    if (resolution < 2 || values.size() < static_cast<size_t>(resolution * resolution))
    {
        return 0.0f;
    }
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(resolution - 1);
    const float z = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(resolution - 1);
    const int x0 = static_cast<int>(std::floor(x));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = std::min(x0 + 1, resolution - 1);
    const int z1 = std::min(z0 + 1, resolution - 1);
    const float tx = x - static_cast<float>(x0);
    const float tz = z - static_cast<float>(z0);
    const auto at = [&](int px, int pz) {
        return values[static_cast<size_t>(pz * resolution + px)];
    };
    const float a = std::lerp(at(x0, z0), at(x1, z0), tx);
    const float b = std::lerp(at(x0, z1), at(x1, z1), tx);
    return std::lerp(a, b, tz);
}

float Hash01(int x, int y, int seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 374761393u;
    h += static_cast<uint32_t>(y) * 668265263u;
    h ^= static_cast<uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

HeightfieldGrid BuildHeightfieldFromShape(const ShapeSettings& settings, int resolution, float terrainSizeMeters, std::string* message)
{
    HeightfieldGrid grid;
    grid.resolution = std::clamp(resolution, 2, 2048);
    grid.terrainSizeMeters = std::max(1.0f, terrainSizeMeters);
    const float shapeSizeMeters = std::max(1.0f, settings.scaleMeters);
    const float heightMeters = shapeSizeMeters * std::max(0.0f, settings.relativeHeightPercent) / 100.0f;
    const float halfTerrain = grid.terrainSizeMeters * 0.5f;
    const float halfShape = shapeSizeMeters * 0.5f;
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.heights.reserve(cellCount);
    grid.mask.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    grid.flows.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);

    for (int z = 0; z < grid.resolution; ++z)
    {
        const float v = grid.resolution > 1 ? static_cast<float>(z) / static_cast<float>(grid.resolution - 1) : 0.0f;
        const float worldZ = std::lerp(-halfTerrain, halfTerrain, v);
        for (int x = 0; x < grid.resolution; ++x)
        {
            const float u = grid.resolution > 1 ? static_cast<float>(x) / static_cast<float>(grid.resolution - 1) : 0.0f;
            const float worldX = std::lerp(-halfTerrain, halfTerrain, u);
            float normalizedHeight = 0.0f;
            if (std::abs(worldX) <= halfShape && std::abs(worldZ) <= halfShape)
            {
                const float nx = worldX / halfShape;
                const float nz = worldZ / halfShape;
                if (settings.kind == ShapeKind::Hemisphere)
                {
                    const float radiusSq = nx * nx + nz * nz;
                    normalizedHeight = radiusSq < 1.0f ? std::sqrt(1.0f - radiusSq) : 0.0f;
                }
                else
                {
                    normalizedHeight = std::max(0.0f, 1.0f - std::max(std::abs(nx), std::abs(nz)));
                }
            }
            grid.heights.push_back(normalizedHeight * heightMeters);
        }
    }

    if (message != nullptr)
    {
        *message = std::format(
            "{} shape {} x {}, canvas {:.0f}m, scale {:.0f}m, height {:.0f}m",
            ToString(settings.kind),
            grid.resolution,
            grid.resolution,
            grid.terrainSizeMeters,
            shapeSizeMeters,
            heightMeters);
    }
    return grid;
}

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

// Run `fn(z)` for each row z in [0, n). Each iteration must be independent
// (writing only to its own row). Used by the heightfield ops that follow.
template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
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

    // Each row writes only to its own indices in temp / blurred (separable
    // Gaussian: horizontal pass then vertical pass), so both passes are safely
    // parallelized across z.
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

        for (size_t i = 0; i < grid.heights.size(); ++i)
        {
            grid.heights[i] = std::lerp(source[i], blurred[i], strength);
        }
    }
}

// グラデーションの stops を t (0..1) でサンプリングして RGB を返す。
// stops は position 順にソートされていること。stops が空の場合は黒を返す。
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

// Multi-Scale Erosion — CPU port of Schott et al. (SIGGRAPH 2024) shaders:
//   data/shaders/erosion.glsl      (Stream Power Erosion)
//   data/shaders/thermal.glsl      (Thermal / talus)
//   data/shaders/deposition.glsl   (Sediment deposition)
// Source repo: https://github.com/H-Schott/MultiScaleErosion (MIT)
//
// Each pass uses ping-pong buffers and reads upstream contributions through
// the previous iteration's stream/sediment field. SPE/Deposition use the same
// D8 weighted-flow direction (`flow_p` exponent on slope). Thermal is a 3x3
// stencil with wraparound boundary, matching the shader.
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

namespace mse
{
// Coarsest resolution in the multi-grid pyramid. Anything coarser than this
// has too few cells to be meaningful; anything finer is reached by repeated
// x2 bilinear upsampling.
constexpr int kCoarsestPyramidLevel = 64;

// Reference cell size for resolution-invariant scaling of per-iter strengths.
// The shader's `eps * cellArea` (and `rain * cellArea`) factors make the per-
// iter effect scale with cellSize², which causes the visual outcome to drift
// noticeably as the user changes Simulation Resolution. We anchor those
// `cellArea` factors to a fixed reference so the same parameter set produces
// roughly the same look across resolutions. 4 m matches the typical 512 grid
// over a 2048 m terrain — existing tunings stay accurate there, and other
// resolutions get the compensation.
constexpr float kRefCellSize = 4.0f;
constexpr float kRefCellArea = kRefCellSize * kRefCellSize;

constexpr std::array<std::pair<int, int>, 8> kNext8 = {{
    {0, 1}, {1, 1}, {1, 0}, {1, -1},
    {0, -1}, {-1, -1}, {-1, 0}, {-1, 1},
}};

inline int Index1D(int x, int z, int n)
{
    return z * n + x;
}

// 2D value-noise approximation used to perturb the thermal threshold angle.
// The reference shader samples 3D simplex noise on (x, y, height); we keep a
// cheaper 2D variant since the spatial variation is what matters visually.
inline float Hash2(float x, float y)
{
    const float s = std::sin(x * 12.9898f + y * 78.233f) * 43758.5453123f;
    return s - std::floor(s);
}

inline float ValueNoise2D(float x, float y)
{
    const float xi = std::floor(x);
    const float yi = std::floor(y);
    const float xf = x - xi;
    const float yf = y - yi;
    const float a = Hash2(xi, yi);
    const float b = Hash2(xi + 1.0f, yi);
    const float c = Hash2(xi, yi + 1.0f);
    const float d = Hash2(xi + 1.0f, yi + 1.0f);
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = yf * yf * (3.0f - 2.0f * yf);
    return std::lerp(std::lerp(a, b, u), std::lerp(c, d, v), v) * 2.0f - 1.0f;
}

// Each row of the (x, z) grid is independent in the SPE / Thermal / Deposition
// passes (outputs are written only to the row's own indices), so we parallelize
// across rows using the outer-namespace `ParallelForRows` helper. par (not
// par_unseq) — the bodies have heavy branching that wouldn't vectorize cleanly.

struct WeightedFlow
{
    std::array<float, 8> w{};
};

// Compute the weighted D8 outflow distribution for cell p. `pow(slope, flow_p)`
// over downhill neighbours, normalized; uphill neighbours get 0.
inline WeightedFlow GetFlowWeighted(const std::vector<float>& heights, int n, float cellSize, int x, int z, float flowP)
{
    WeightedFlow out{};
    float sum = 0.0f;
    const float h = heights[static_cast<size_t>(Index1D(x, z, n))];
    for (int i = 0; i < 8; ++i)
    {
        const int qx = x + kNext8[i].first;
        const int qz = z + kNext8[i].second;
        if (qx < 0 || qx >= n || qz < 0 || qz >= n) { out.w[static_cast<size_t>(i)] = 0.0f; continue; }
        const float hq = heights[static_cast<size_t>(Index1D(qx, qz, n))];
        const float dx = static_cast<float>(kNext8[i].first) * cellSize;
        const float dz = static_cast<float>(kNext8[i].second) * cellSize;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float slope = (h - hq) / d;
        if (slope > 0.0f)
        {
            const float w = std::pow(slope, flowP);
            out.w[static_cast<size_t>(i)] = w;
            sum += w;
        }
        else
        {
            out.w[static_cast<size_t>(i)] = 0.0f;
        }
    }
    if (sum > 1e-6f)
    {
        for (float& w : out.w) { w /= sum; }
    }
    return out;
}

// Steepest descent direction (i, j) at p, plus its slope magnitude.
inline void GetSteepestDescent(const std::vector<float>& heights, int n, float cellSize, int x, int z,
                                int& dx, int& dz, float& slope)
{
    dx = 0;
    dz = 0;
    slope = 0.0f;
    const float h = heights[static_cast<size_t>(Index1D(x, z, n))];
    for (int i = 0; i < 8; ++i)
    {
        const int qx = x + kNext8[i].first;
        const int qz = z + kNext8[i].second;
        if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
        const float hq = heights[static_cast<size_t>(Index1D(qx, qz, n))];
        const float ax = static_cast<float>(kNext8[i].first) * cellSize;
        const float az = static_cast<float>(kNext8[i].second) * cellSize;
        const float d = std::sqrt(ax * ax + az * az);
        const float s = (h - hq) / d;
        if (s > slope)
        {
            slope = s;
            dx = kNext8[i].first;
            dz = kNext8[i].second;
        }
    }
}

void StepStreamPower(std::vector<float>& heightsIn, std::vector<float>& heightsOut,
                     std::vector<float>& streamIn, std::vector<float>& streamOut,
                     int n, float cellSize, const MultiScaleErosionSettings& s)
{
    const float cellDiag = cellSize * std::sqrt(2.0f);
    const float baseStream = cellDiag;
    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const int id = Index1D(x, z, n);

            // Incoming flow from neighbours' weighted outflow towards p.
            float incoming = 0.0f;
            for (int i = 0; i < 8; ++i)
            {
                const int qx = x + kNext8[i].first;
                const int qz = z + kNext8[i].second;
                if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
                const WeightedFlow wf = GetFlowWeighted(heightsIn, n, cellSize, qx, qz, s.flowExponent);
                const float w = wf.w[static_cast<size_t>((i + 4) % 8)];
                if (w > 0.0f)
                {
                    incoming += w * streamIn[static_cast<size_t>(Index1D(qx, qz, n))];
                }
            }
            const float stream = baseStream + incoming;

            int dx = 0, dz = 0;
            float steepest = 0.0f;
            GetSteepestDescent(heightsIn, n, cellSize, x, z, dx, dz, steepest);
            const float receiverHeight = (dx == 0 && dz == 0)
                ? heightsIn[static_cast<size_t>(id)]
                : heightsIn[static_cast<size_t>(Index1D(x + dx, z + dz, n))];

            float spe = std::pow(stream, s.streamExponent) * std::clamp(std::pow(steepest, s.slopeExponent), 0.0f, 1.0f);
            spe = std::clamp(spe, 0.0f, s.maxStreamPower) * s.speStrength;

            const float oldHeight = heightsIn[static_cast<size_t>(id)];
            float newHeight = oldHeight - s.speTimeStep * spe;
            newHeight = std::max(newHeight, receiverHeight);
            heightsOut[static_cast<size_t>(id)] = newHeight;
            streamOut[static_cast<size_t>(id)] = stream;
        }
    });
}

void StepThermal(std::vector<float>& heightsIn, std::vector<float>& heightsOut,
                 int n, float cellSize, const MultiScaleErosionSettings& s)
{
    const float baseTan = std::tan(s.thermalAngleDegrees * 3.14159265358979323846f / 180.0f);
    // Resolution-invariant: anchor matter to refCellArea instead of the
    // current cellArea (see kRefCellSize comment above).
    const float matter = s.thermalStrength * kRefCellArea;
    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const int id = Index1D(x, z, n);
            const float h = heightsIn[static_cast<size_t>(id)];

            float tanAngle = baseTan;
            if (s.thermalNoisifyAngle)
            {
                const float t = ValueNoise2D(static_cast<float>(x) * s.thermalNoiseWavelength * static_cast<float>(n),
                                             static_cast<float>(z) * s.thermalNoiseWavelength * static_cast<float>(n)) * 0.5f + 0.5f;
                tanAngle = baseTan * std::lerp(s.thermalNoiseMin, s.thermalNoiseMax, t);
            }

            float receiveMul = 0.0f;
            float distributeMul = 0.0f;
            for (int j = -1; j <= 1; ++j)
            {
                for (int i = -1; i <= 1; ++i)
                {
                    if (i == 0 && j == 0) { continue; }
                    // Wraparound (matches thermal.glsl).
                    const int qx = ((x + i) % n + n) % n;
                    const int qz = ((z + j) % n + n) % n;
                    const float ax = static_cast<float>(i) * cellSize;
                    const float az = static_cast<float>(j) * cellSize;
                    const float d = std::sqrt(ax * ax + az * az);
                    const float hq = heightsIn[static_cast<size_t>(Index1D(qx, qz, n))];
                    if ((hq - h) / d > tanAngle) { receiveMul += 1.0f; }
                    if ((h - hq) / d > tanAngle) { distributeMul += 1.0f; }
                }
            }
            heightsOut[static_cast<size_t>(id)] = h + matter * (receiveMul - distributeMul);
        }
    });
}

void StepDeposition(std::vector<float>& heightsIn, std::vector<float>& heightsOut,
                    std::vector<float>& streamIn, std::vector<float>& streamOut,
                    std::vector<float>& sedIn, std::vector<float>& sedOut,
                    int n, float cellSize, const MultiScaleErosionSettings& s)
{
    // Match deposition.glsl, but anchor cellArea to refCellArea for resolution
    // invariance (see kRefCellSize comment near the top of the namespace).
    // Otherwise rain * cellArea (the per-cell rain volume contribution) would
    // scale with cellSize², shifting deposition behavior across resolutions.
    constexpr float cellArea = kRefCellArea * 0.00001f;
    ParallelForRows(n, [&](int z) {
        for (int x = 0; x < n; ++x)
        {
            const int id = Index1D(x, z, n);
            const float h = heightsIn[static_cast<size_t>(id)];
            float sed = sedIn[static_cast<size_t>(id)];

            int dx = 0, dz = 0;
            float steepest = 0.0f;
            GetSteepestDescent(heightsIn, n, cellSize, x, z, dx, dz, steepest);
            // Match deposition.glsl `if (!CheckPit(p)) sed = 0;`: only local minima
            // (no lower neighbour) retain sediment between iterations. Non-pit cells
            // flush their sediment downstream and start fresh from incoming + pickup.
            const bool isPit = (dx == 0 && dz == 0);
            if (!isPit) { sed = 0.0f; }

            // Add rain and incoming weighted flow / sediment.
            float incomingStream = 0.0f;
            float incomingSed = 0.0f;
            for (int i = 0; i < 8; ++i)
            {
                const int qx = x + kNext8[i].first;
                const int qz = z + kNext8[i].second;
                if (qx < 0 || qx >= n || qz < 0 || qz >= n) { continue; }
                const WeightedFlow wf = GetFlowWeighted(heightsIn, n, cellSize, qx, qz, s.flowExponent);
                const float w = wf.w[static_cast<size_t>((i + 4) % 8)];
                if (w > 0.0f)
                {
                    incomingStream += w * streamIn[static_cast<size_t>(Index1D(qx, qz, n))];
                    incomingSed += w * sedIn[static_cast<size_t>(Index1D(qx, qz, n))];
                }
            }
            const float stream = s.rain * cellArea + incomingStream;
            sed += incomingSed;

            const float speed = std::clamp(std::pow(steepest, 2.0f), 0.0f, 1.0f);
            const float streamPower = std::pow(std::max(stream, 1e-12f), 0.3f) * speed;

            float newHeight = h;
            if (s.depositionStrength * sed > streamPower)
            {
                const float deposit = std::min(sed, (s.depositionStrength * sed - streamPower) * 0.1f);
                newHeight += deposit;
                sed = std::max(0.0f, sed - deposit);
            }
            sed += 0.1f * streamPower;

            heightsOut[static_cast<size_t>(id)] = newHeight;
            streamOut[static_cast<size_t>(id)] = stream;
            sedOut[static_cast<size_t>(id)] = sed;
        }
    });
}
// Bilinear resample of a height array between arbitrary source/target
// resolutions. Reuses SampleHeightfieldValue's bilinear sampler so the same
// formula is used as elsewhere in the project.
inline std::vector<float> ResampleHeightsBilinear(const std::vector<float>& source, int sourceN, int targetN)
{
    std::vector<float> result(static_cast<size_t>(targetN) * static_cast<size_t>(targetN), 0.0f);
    for (int z = 0; z < targetN; ++z)
    {
        const float v = targetN > 1 ? static_cast<float>(z) / static_cast<float>(targetN - 1) : 0.0f;
        for (int x = 0; x < targetN; ++x)
        {
            const float u = targetN > 1 ? static_cast<float>(x) / static_cast<float>(targetN - 1) : 0.0f;
            result[static_cast<size_t>(z * targetN + x)] = SampleHeightfieldValue(source, sourceN, u, v);
        }
    }
    return result;
}

void ApplyMultiScaleErosionSingleLevel(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount || settings.iterations <= 0)
    {
        return;
    }

    // GPU compute path. Falls back to CPU if the evaluator hasn't been
    // registered (no D3D12 device) or returns failure (e.g. shader compile
    // error, runtime issue). The evaluator is responsible for filling
    // grid.heights / flows / deposits and for normalizing the auxiliary
    // fields just like the CPU branch below.
    if (settings.backend == MultiScaleErosionBackend::GpuCompute && g_mseGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_mseGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Fall through to CPU path on GPU failure.
    }

    const float cellSize = grid.terrainSizeMeters / static_cast<float>(std::max(1, n - 1));

    std::vector<float> heightsA = grid.heights;
    std::vector<float> heightsB(cellCount, 0.0f);
    std::vector<float> streamA(cellCount, 0.0f);
    std::vector<float> streamB(cellCount, 0.0f);
    std::vector<float> sedA(cellCount, 0.0f);
    std::vector<float> sedB(cellCount, 0.0f);

    const int iterations = std::clamp(settings.iterations, 0, 500);
    for (int it = 0; it < iterations; ++it)
    {
        if (settings.enableStreamPower)
        {
            StepStreamPower(heightsA, heightsB, streamA, streamB, n, cellSize, settings);
            std::swap(heightsA, heightsB);
            std::swap(streamA, streamB);
        }
        if (settings.enableThermal)
        {
            StepThermal(heightsA, heightsB, n, cellSize, settings);
            std::swap(heightsA, heightsB);
        }
        if (settings.enableDeposition)
        {
            StepDeposition(heightsA, heightsB, streamA, streamB, sedA, sedB, n, cellSize, settings);
            std::swap(heightsA, heightsB);
            std::swap(streamA, streamB);
            std::swap(sedA, sedB);
        }
    }

    grid.heights = std::move(heightsA);
    grid.flows = std::move(streamA);
    grid.deposits = std::move(sedA);
    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);
    NormalizeHeightfieldFields(grid);
}
} // namespace mse

void ApplyMultiScaleErosion(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings)
{
    const int targetN = grid.resolution;
    const size_t targetCellCount = static_cast<size_t>(targetN) * static_cast<size_t>(targetN);
    if (targetN < 3 || grid.heights.size() < targetCellCount || settings.iterations <= 0)
    {
        return;
    }

    if (!settings.useMultigrid)
    {
        mse::ApplyMultiScaleErosionSingleLevel(grid, settings);
        return;
    }

    // Build pyramid: start at kCoarsestPyramidLevel and double up to the target
    // resolution. The coarse pass establishes the drainage network quickly
    // (stream propagation cost is O(path_length / cellSize), which is small at
    // coarse cellSize). Each subsequent level only refines the existing
    // structure with bilinear upsample + more iterations.
    std::vector<int> levels;
    for (int r = mse::kCoarsestPyramidLevel; r < targetN; r *= 2)
    {
        levels.push_back(r);
    }
    levels.push_back(targetN);

    if (levels.size() <= 1)
    {
        // Target is already <= the coarsest level, no benefit from pyramid.
        mse::ApplyMultiScaleErosionSingleLevel(grid, settings);
        return;
    }

    HeightfieldGrid working;
    working.terrainSizeMeters = grid.terrainSizeMeters;
    working.resolution = levels[0];
    working.heights = mse::ResampleHeightsBilinear(grid.heights, targetN, levels[0]);

    for (size_t i = 0; i < levels.size(); ++i)
    {
        if (i > 0)
        {
            std::vector<float> upsampled = mse::ResampleHeightsBilinear(working.heights, levels[i - 1], levels[i]);
            working.heights = std::move(upsampled);
            working.resolution = levels[i];
        }
        mse::ApplyMultiScaleErosionSingleLevel(working, settings);
    }

    // Final level's output is already at target resolution.
    grid.heights = std::move(working.heights);
    grid.flows = std::move(working.flows);
    grid.deposits = std::move(working.deposits);
    grid.mask.assign(targetCellCount, 0.0f);
    grid.age.assign(targetCellCount, 0.0f);
}

float SampleGridBilinear(const std::vector<float>& values, int n, float x, float z)
{
    x = std::clamp(x, 0.0f, static_cast<float>(n - 1));
    z = std::clamp(z, 0.0f, static_cast<float>(n - 1));
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, n - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(z)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1);
    const int z1 = std::min(z0 + 1, n - 1);
    const float tx = x - static_cast<float>(x0);
    const float tz = z - static_cast<float>(z0);
    const float h00 = values[static_cast<size_t>(z0) * n + x0];
    const float h10 = values[static_cast<size_t>(z0) * n + x1];
    const float h01 = values[static_cast<size_t>(z1) * n + x0];
    const float h11 = values[static_cast<size_t>(z1) * n + x1];
    const float hx0 = h00 + (h10 - h00) * tx;
    const float hx1 = h01 + (h11 - h01) * tx;
    return hx0 + (hx1 - hx0) * tz;
}

void SplatGridBilinear(std::vector<float>& values, int n, float x, float z, float amount)
{
    if (x < 0.0f || z < 0.0f || x > static_cast<float>(n - 1) || z > static_cast<float>(n - 1))
    {
        return;
    }
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, n - 1);
    const int z0 = std::clamp(static_cast<int>(std::floor(z)), 0, n - 1);
    const int x1 = std::min(x0 + 1, n - 1);
    const int z1 = std::min(z0 + 1, n - 1);
    const float tx = x - static_cast<float>(x0);
    const float tz = z - static_cast<float>(z0);
    values[static_cast<size_t>(z0) * n + x0] += amount * (1.0f - tx) * (1.0f - tz);
    values[static_cast<size_t>(z0) * n + x1] += amount * tx * (1.0f - tz);
    values[static_cast<size_t>(z1) * n + x0] += amount * (1.0f - tx) * tz;
    values[static_cast<size_t>(z1) * n + x1] += amount * tx * tz;
}

void ConvertFluvialAccumulationToMask(HeightfieldGrid& grid, const MaskFluvialSettings& settings, const std::vector<float>& accum, float thresholdUnits)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    grid.mask.assign(cellCount, 0.0f);
    if (accum.empty())
    {
        return;
    }

    const float maxAccum = std::max(1e-6f, *std::max_element(accum.begin(), accum.end()));
    if (settings.outputCurve == MaskFluvialOutputCurve::Threshold)
    {
        const float thresholdLow = std::max(0.0f, thresholdUnits);
        const float softness = std::clamp(settings.softness, 0.001f, 4.0f);
        const float thresholdHigh = thresholdLow + std::max(maxAccum * softness, 1e-6f);
        const float power = std::clamp(settings.power, 0.1f, 8.0f);
        const float invRange = 1.0f / std::max(thresholdHigh - thresholdLow, 1e-6f);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                float t = std::clamp((accum[idx] - thresholdLow) * invRange, 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                grid.mask[idx] = std::pow(t, power);
            }
        });
        return;
    }

    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    const float adjustedMax = std::max(maxAccum - thresholdUnits, 1e-6f);
    if (settings.outputCurve == MaskFluvialOutputCurve::Log)
    {
        const float invLogMax = 1.0f / std::log1p(adjustedMax);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float v = std::max(0.0f, accum[idx] - thresholdUnits);
                grid.mask[idx] = std::pow(std::clamp(std::log1p(v) * invLogMax, 0.0f, 1.0f), gamma);
            }
        });
        return;
    }

    const float invMax = 1.0f / adjustedMax;
    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t idx = rowBase + static_cast<size_t>(x);
            const float v = std::max(0.0f, accum[idx] - thresholdUnits) * invMax;
            grid.mask[idx] = std::pow(std::clamp(v, 0.0f, 1.0f), gamma);
        }
    });
}

void ApplyMaskFluvialParticles(HeightfieldGrid& grid, const MaskFluvialSettings& settings, const std::vector<float>& heights, float cellSize)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    std::vector<float> hits(cellCount, 0.0f);

    const int particleCount = std::clamp(settings.particleCount, 1, 200000);
    const int lifetime = std::clamp(settings.particleLifetime, 1, 2048);
    const float inertia = std::clamp(settings.particleInertia, 0.0f, 0.98f);
    const float stepCells = std::clamp(settings.particleStepLengthM / std::max(cellSize, 1e-6f), 0.25f, 8.0f);

    std::mt19937 rng(static_cast<uint32_t>(settings.particleSeed));
    std::uniform_real_distribution<float> spawnDist(1.0f, static_cast<float>(std::max(1, n - 2)));
    std::uniform_real_distribution<float> unitDist(0.0f, 1.0f);

    for (int particle = 0; particle < particleCount; ++particle)
    {
        float x = spawnDist(rng);
        float z = spawnDist(rng);
        float vx = 0.0f;
        float vz = 0.0f;
        for (int age = 0; age < lifetime; ++age)
        {
            if (x < 1.0f || z < 1.0f || x > static_cast<float>(n - 2) || z > static_cast<float>(n - 2))
            {
                break;
            }

            const float gx = 0.5f * (SampleGridBilinear(heights, n, x + 1.0f, z) - SampleGridBilinear(heights, n, x - 1.0f, z));
            const float gz = 0.5f * (SampleGridBilinear(heights, n, x, z + 1.0f) - SampleGridBilinear(heights, n, x, z - 1.0f));
            float dx = -gx;
            float dz = -gz;
            float len = std::sqrt(dx * dx + dz * dz);
            if (len <= 1e-8f)
            {
                const float angle = unitDist(rng) * 6.28318530718f;
                dx = std::cos(angle);
                dz = std::sin(angle);
            }
            else
            {
                dx /= len;
                dz /= len;
                const float jitterAngle = (unitDist(rng) * 2.0f - 1.0f) * (1.0f - inertia) * 0.35f;
                const float cs = std::cos(jitterAngle);
                const float sn = std::sin(jitterAngle);
                const float jx = dx * cs - dz * sn;
                const float jz = dx * sn + dz * cs;
                dx = jx;
                dz = jz;
            }

            vx = vx * inertia + dx * (1.0f - inertia);
            vz = vz * inertia + dz * (1.0f - inertia);
            len = std::sqrt(vx * vx + vz * vz);
            if (len <= 1e-8f)
            {
                vx = dx;
                vz = dz;
            }
            else
            {
                vx /= len;
                vz /= len;
            }

            const float ageWeight = 1.0f - 0.35f * (static_cast<float>(age) / static_cast<float>(std::max(1, lifetime - 1)));
            SplatGridBilinear(hits, n, x, z, ageWeight);
            x += vx * stepCells;
            z += vz * stepCells;
        }
    }

    const float maxHit = std::max(1e-6f, *std::max_element(hits.begin(), hits.end()));
    const float threshold = std::clamp(settings.accumulationThreshold, 0.0f, 1.0f) * maxHit;
    ConvertFluvialAccumulationToMask(grid, settings, hits, threshold);
}

// Mask Fluvial: MFD flow accumulation -> river-stream mask.
// Heights pass through. Fills grid.mask with a normalized 0..1 mask
// where the upstream-cell count exceeds accumulationThreshold.
void ApplyMaskFluvial(HeightfieldGrid& grid, const MaskFluvialSettings& settings)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 3 || grid.heights.size() < cellCount)
    {
        return;
    }

    if (settings.simulationMode == MaskFluvialSimulationMode::FlowAccumulation &&
        settings.backend == MaskFluvialBackend::GpuCompute && g_maskFluvialGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_maskFluvialGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    // 1. Build analysis heights. Largest Detail Level low-passes small
    // wrinkles before flow routing, without modifying the output heightfield.
    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSize = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSize, terrainSize * 0.5f);
    const int detailRadius = std::clamp(static_cast<int>(std::round(largestDetailM / cellSize)), 1, 64);
    std::vector<float> analysisHeights = grid.heights;
    if (detailRadius > 1)
    {
        std::vector<float> temp(cellCount, 0.0f);
        const float sigma = std::max(1.0f, static_cast<float>(detailRadius) * 0.5f);
        const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                float sum = 0.0f;
                float weightSum = 0.0f;
                for (int ox = -detailRadius; ox <= detailRadius; ++ox)
                {
                    const int sx = std::clamp(x + ox, 0, n - 1);
                    const float w = std::exp(-static_cast<float>(ox * ox) * invTwoSigma2);
                    sum += analysisHeights[rowBase + static_cast<size_t>(sx)] * w;
                    weightSum += w;
                }
                temp[rowBase + static_cast<size_t>(x)] = sum / std::max(weightSum, 1e-6f);
            }
        });
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                float sum = 0.0f;
                float weightSum = 0.0f;
                for (int oz = -detailRadius; oz <= detailRadius; ++oz)
                {
                    const int sz = std::clamp(z + oz, 0, n - 1);
                    const float w = std::exp(-static_cast<float>(oz * oz) * invTwoSigma2);
                    sum += temp[static_cast<size_t>(sz) * static_cast<size_t>(n) + static_cast<size_t>(x)] * w;
                    weightSum += w;
                }
                analysisHeights[rowBase + static_cast<size_t>(x)] = sum / std::max(weightSum, 1e-6f);
            }
        });
    }

    // 2. Iterative pit fill (Jacobi, double-buffered). Any interior cell
    // whose 8 neighbours are all >= itself gets raised to (min_neighbour +
    // epsilon). Boundary cells act as outlets. We use Jacobi (read from
    // `filled`, write to `next`, swap) instead of Gauss-Seidel so the
    // sweep parallelises cleanly across rows. Jacobi propagates fills one
    // cell per iteration just like GS, so iteration count is the practical
    // tunable for "how deep a pit can be filled".
    std::vector<float> filled = std::move(analysisHeights);
    std::vector<float> next = filled;
    const int pitIters = MaskFluvialSettings{}.pitFillIterations;
    constexpr float kPitEpsilon = 1e-4f;
    for (int iter = 0; iter < pitIters; ++iter)
    {
        ParallelForRows(n, [&](int z) {
            if (z == 0 || z >= n - 1)
            {
                return;
            }
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            const size_t rowAbove = static_cast<size_t>(z - 1) * static_cast<size_t>(n);
            const size_t rowBelow = static_cast<size_t>(z + 1) * static_cast<size_t>(n);
            for (int x = 1; x < n - 1; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float h = filled[idx];
                const float n00 = filled[rowAbove + static_cast<size_t>(x - 1)];
                const float n01 = filled[rowAbove + static_cast<size_t>(x)];
                const float n02 = filled[rowAbove + static_cast<size_t>(x + 1)];
                const float n10 = filled[rowBase + static_cast<size_t>(x - 1)];
                const float n12 = filled[rowBase + static_cast<size_t>(x + 1)];
                const float n20 = filled[rowBelow + static_cast<size_t>(x - 1)];
                const float n21 = filled[rowBelow + static_cast<size_t>(x)];
                const float n22 = filled[rowBelow + static_cast<size_t>(x + 1)];
                const float minNeighbor = std::min({n00, n01, n02, n10, n12, n20, n21, n22});
                next[idx] = (h <= minNeighbor) ? (minNeighbor + kPitEpsilon) : h;
            }
        });
        std::swap(filled, next);
    }

    if (settings.simulationMode == MaskFluvialSimulationMode::Particles)
    {
        ApplyMaskFluvialParticles(grid, settings, filled, cellSize);
        return;
    }

    // 3. Sort cell indices by height descending. Accumulation must process
    // each cell after every higher cell upstream of it has already pushed
    // its flow downhill, so a topological sort by elevation works.
    std::vector<int> indices(cellCount);
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(std::execution::par, indices.begin(), indices.end(),
              [&filled](int a, int b) { return filled[a] > filled[b]; });

    // 4. Flow accumulation. Each cell starts with weight 1 and pushes its
    // accumulator to several MFD downhill neighbours.
    std::vector<float> accum(cellCount, 1.0f);
    static const int kDx[8]    = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int kDz[8]    = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const float kDist[8] = {
        1.41421356f, 1.0f, 1.41421356f,
        1.0f,              1.0f,
        1.41421356f, 1.0f, 1.41421356f,
    };

    const float p = std::clamp(settings.mfdExponent, 0.1f, 16.0f);
    for (int idx : indices)
    {
        const int x = idx % n;
        const int z = idx / n;
        const float h = filled[static_cast<size_t>(idx)];
        float weights[8] = {0};
        float weightSum = 0.0f;
        for (int k = 0; k < 8; ++k)
        {
            const int nx = x + kDx[k];
            const int nz = z + kDz[k];
            if (nx < 0 || nx >= n || nz < 0 || nz >= n) continue;
            const float nh = filled[static_cast<size_t>(nz) * n + nx];
            const float slope = (h - nh) / kDist[k];
            if (slope > 0.0f)
            {
                weights[k] = std::pow(slope, p);
                weightSum += weights[k];
            }
        }
        if (weightSum > 0.0f)
        {
            const float inv = 1.0f / weightSum;
            const float a = accum[static_cast<size_t>(idx)];
            for (int k = 0; k < 8; ++k)
            {
                if (weights[k] > 0.0f)
                {
                    const int nx = x + kDx[k];
                    const int nz = z + kDz[k];
                    accum[static_cast<size_t>(nz) * n + nx] += a * weights[k] * inv;
                }
            }
        }
    }

    // 5. Convert accumulation to mask. Threshold is interpreted as a
    // fraction of grid cells so it stays meaningful across resolutions.
    // The per-cell math is heavy (std::log / std::pow), so the row sweep
    // here parallelises cleanly and is a big win at higher resolutions.
    const float thresholdCells = std::clamp(settings.accumulationThreshold, 0.0f, 1.0f) * static_cast<float>(cellCount);
    grid.mask.assign(cellCount, 0.0f);

    if (settings.outputCurve == MaskFluvialOutputCurve::Threshold)
    {
        const float thresholdLow = std::max(1.0f, thresholdCells);
        const float softness = std::clamp(settings.softness, 0.001f, 4.0f);
        const float thresholdHigh = thresholdLow * (1.0f + 4.0f * softness);
        const float power = std::clamp(settings.power, 0.1f, 8.0f);
        const float invRange = 1.0f / std::max(thresholdHigh - thresholdLow, 1e-3f);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                float t = (accum[idx] - thresholdLow) * invRange;
                t = std::clamp(t, 0.0f, 1.0f);
                const float smooth = t * t * (3.0f - 2.0f * t);
                grid.mask[idx] = std::pow(smooth, power);
            }
        });
        return;
    }

    // Log / Linear: parallel max reduction (each row computes its local
    // adjusted-max, then a quick serial fold across rows), followed by a
    // parallel mask-conversion sweep. We don't materialise the adjusted
    // vector — recomputing accum[i] - threshold inline is cheaper than the
    // extra allocation + pass.
    std::vector<float> rowMax(static_cast<size_t>(n), 0.0f);
    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        float local = 0.0f;
        for (int x = 0; x < n; ++x)
        {
            const float v = accum[rowBase + static_cast<size_t>(x)] - thresholdCells;
            if (v > local) local = v;
        }
        rowMax[static_cast<size_t>(z)] = local;
    });
    float maxAdjusted = 0.0f;
    for (float v : rowMax) if (v > maxAdjusted) maxAdjusted = v;

    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    if (settings.outputCurve == MaskFluvialOutputCurve::Log)
    {
        const float invLogMax = 1.0f / std::max(std::log(1.0f + maxAdjusted), 1e-3f);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float a = std::max(0.0f, accum[idx] - thresholdCells);
                const float t = std::log(1.0f + a) * invLogMax;
                grid.mask[idx] = std::pow(std::clamp(t, 0.0f, 1.0f), gamma);
            }
        });
    }
    else  // Linear
    {
        const float invMax = 1.0f / std::max(maxAdjusted, 1e-3f);
        ParallelForRows(n, [&](int z) {
            const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = rowBase + static_cast<size_t>(x);
                const float a = std::max(0.0f, accum[idx] - thresholdCells);
                const float t = a * invMax;
                grid.mask[idx] = std::pow(std::clamp(t, 0.0f, 1.0f), gamma);
            }
        });
    }
}

namespace rock_node
{
inline uint32_t Hash2(int32_t x, int32_t y, int32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x27d4eb2du + static_cast<uint32_t>(y) * 0x9e3779b9u + static_cast<uint32_t>(seed) * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x21f0aaadu;
    h ^= h >> 15;
    h *= 0x735a2d97u;
    h ^= h >> 15;
    return h;
}

inline float HashFloat01(int32_t x, int32_t y, int32_t seed)
{
    return static_cast<float>(Hash2(x, y, seed) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

inline int32_t DeriveSeed(int32_t seed, uint32_t multiplier, uint32_t addend)
{
    const uint32_t value = static_cast<uint32_t>(seed) * multiplier + addend;
    return static_cast<int32_t>(value);
}

// One Voronoi pass: jittered grid where each integer cell holds a single
// site at its centre + a per-cell offset in [-0.45, 0.45]. Returns the two
// nearest distances (F1, F2) and the integer coordinates of the F1 cell —
// callers reuse those coordinates to fetch per-cell randomisation.
inline void VoronoiF1F2(float x, float z, int32_t seed,
                        float& f1, float& f2,
                        int32_t& f1cx, int32_t& f1cz)
{
    const int32_t cx = static_cast<int32_t>(std::floor(x));
    const int32_t cz = static_cast<int32_t>(std::floor(z));
    f1 = std::numeric_limits<float>::infinity();
    f2 = std::numeric_limits<float>::infinity();
    f1cx = cx;
    f1cz = cz;
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int32_t gx = cx + dx;
            const int32_t gz = cz + dz;
            const float jx = HashFloat01(gx, gz, seed) * 0.9f - 0.45f;
            const float jz = HashFloat01(gx, gz, seed + 73) * 0.9f - 0.45f;
            const float sx = static_cast<float>(gx) + 0.5f + jx;
            const float sz = static_cast<float>(gz) + 0.5f + jz;
            const float dxs = sx - x;
            const float dzs = sz - z;
            const float d = std::sqrt(dxs * dxs + dzs * dzs);
            if (d < f1)
            {
                f2 = f1;
                f1 = d;
                f1cx = gx;
                f1cz = gz;
            }
            else if (d < f2)
            {
                f2 = d;
            }
        }
    }
}

inline float Smoothstep01(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace rock_node

// Rock: tiles the terrain with a jittered Voronoi grid, raising each cell
// into a dome with sub-cell roughness and an optional crack at the cell
// boundary. Heights are added (peaks rise above the input terrain), and a
// 0..1 mask of "where the rock dome is significant" is written to grid.mask.
void ApplyRock(HeightfieldGrid& grid, const RockSettings& settings, const MaskGrid* placementMask = nullptr)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount || settings.density <= 0.0f)
    {
        return;
    }

    const bool hasPlacementMask = placementMask != nullptr &&
        placementMask->resolution > 0 &&
        !placementMask->values.empty();
    const bool usesSmoothedGround = settings.groundDetailLevelM > 0.0f;
    if (!hasPlacementMask && !usesSmoothedGround &&
        settings.backend == RockBackend::GpuCompute && g_rockGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_rockGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);

    const float density = std::max(settings.density, 0.1f);
    const float coverage = std::clamp(settings.coverage, 0.0f, 1.0f);
    // Sizes are specified in metres; convert to cell-pitch units (1 cell = density m).
    const float rockSizeMinM = std::clamp(settings.rockSizeMinM, 0.1f, 200.0f);
    const float rockSizeMaxM = std::clamp(std::max(settings.rockSizeMaxM, rockSizeMinM), 0.1f, 200.0f);
    const float rockSizeMinCells = rockSizeMinM / density;
    const float rockSizeMaxCells = rockSizeMaxM / density;
    const float rockHeight = std::max(settings.rockHeight, 0.0f);
    const float heightJitter = std::clamp(settings.heightJitter, 0.0f, 1.0f);
    const float rotationVar = std::clamp(settings.rotationVariation, 0.0f, 1.0f);
    const float aspectVar = std::clamp(settings.aspectVariation, 0.0f, 1.0f);
    const float edgeSharpness = std::clamp(settings.edgeSharpness, 0.0f, 1.0f);
    const float bumpiness = std::clamp(settings.bumpiness, 0.0f, 1.0f);
    const float facetSharpness = std::clamp(settings.facetSharpness, 0.0f, 1.0f);
    const float facetScale = std::clamp(settings.facetScale, 0.5f, 8.0f);
    const int layerCount = std::clamp(settings.layerCount, 1, 8);
    const int orientationRule = std::clamp(static_cast<int>(settings.orientationRule), 0, 2);
    const int rockStyle = std::clamp(static_cast<int>(settings.style), 0, 2);
    const bool polygonalStyle = rockStyle != static_cast<int>(RockStyle::Classic);
    const bool shardStyle = rockStyle == static_cast<int>(RockStyle::Shard);
    const float effectiveEdgeSharpness = polygonalStyle ? std::max(edgeSharpness, 0.65f) : edgeSharpness;
    const float effectiveFacetSharpness = polygonalStyle ? std::max(facetSharpness, 0.7f) : facetSharpness;
    const float styleAspectBoost = shardStyle ? 0.65f : 0.0f;
    const int32_t seed = settings.seed;
    const int32_t subSeedI = rock_node::DeriveSeed(seed, 7919u, 31337u);
    const int32_t facetSeedI = rock_node::DeriveSeed(seed, 2347u, 8675309u);
    const int32_t rotSeed = rock_node::DeriveSeed(seed, 4519u, 91173u);
    const int32_t sizeSeed = rock_node::DeriveSeed(seed, 1583u, 22441u);
    const int32_t aspectSeed = rock_node::DeriveSeed(seed, 2381u, 33797u);
    const int32_t aspectAxisSeed = rock_node::DeriveSeed(seed, 4093u, 51817u);
    const int32_t subOffsetSeedX = rock_node::DeriveSeed(seed, 643u, 5081u);
    const int32_t subOffsetSeedZ = rock_node::DeriveSeed(seed, 757u, 6151u);
    const int32_t polyCountSeed = rock_node::DeriveSeed(seed, 1009u, 13513u);
    const int32_t polyAngleSeed = rock_node::DeriveSeed(seed, 137u, 60013u);
    const int32_t polyRadiusSeed = rock_node::DeriveSeed(seed, 251u, 70003u);
    const int32_t apexSeedX = rock_node::DeriveSeed(seed, 1181u, 42043u);
    const int32_t apexSeedZ = rock_node::DeriveSeed(seed, 1871u, 52189u);
    const bool needPolyhedral = effectiveEdgeSharpness > 0.0f;

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float halfSize = terrainSize * 0.5f;
    const float invStep = (n > 1) ? 1.0f / static_cast<float>(n - 1) : 0.0f;
    const float cellSizeMeters = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float invTwoCellMeters = 1.0f / (2.0f * cellSizeMeters);
    const float groundDetailM = std::clamp(settings.groundDetailLevelM, 0.0f, terrainSize * 0.5f);
    const int groundRadius = groundDetailM > 0.0f
        ? std::clamp(static_cast<int>(std::round(groundDetailM / cellSizeMeters)), 1, 128)
        : 0;
    const std::vector<float> groundHeights = groundRadius > 1 ? BoxBlurHeights(grid, groundRadius) : std::vector<float>();
    const std::vector<float> inputHeights = (orientationRule != static_cast<int>(RockOrientationRule::Flat))
        ? grid.heights
        : std::vector<float>();
    const auto samplePlacementMask = [&](float u, float v) {
        if (!hasPlacementMask)
        {
            return 1.0f;
        }
        return std::clamp(SampleMaskBilinear(*placementMask, u, v), 0.0f, 1.0f);
    };

    // Search radius covers the worst case: largest rock × max aspect stretch.
    // aspect uses pow(2, aspectVar) to give a symmetric multiplicative range
    // around 1.0 (e.g. aspectVar = 0.5 → aspect ∈ [√½, √2]).
    const float maxDomeRadius = rockSizeMaxCells * 0.5f;
    const float maxAspect = std::pow(2.0f, aspectVar + styleAspectBoost);
    const float maxReach = maxDomeRadius * maxAspect;
    const int searchRadius = std::max(1, static_cast<int>(std::ceil(maxReach - 0.05f)));
    // Apex sharpness: higher facetSharpness pinches the apex, but at full
    // edgeSharpness the dome is already a flat-faceted polyhedron so we
    // keep the falloff linear (exp = 1).
    const float domeExp = polygonalStyle ? 1.0f : (1.0f + facetSharpness * 1.5f * (1.0f - edgeSharpness));

    ParallelForRows(n, [&](int z) {
        const float worldZ = -halfSize + static_cast<float>(z) * invStep * terrainSize;
        const float cellZ = worldZ / density;
        for (int x = 0; x < n; ++x)
        {
            const float worldX = -halfSize + static_cast<float>(x) * invStep * terrainSize;
            const float cellX = worldX / density;
            const int32_t baseCx = static_cast<int32_t>(std::floor(cellX));
            const int32_t baseCz = static_cast<int32_t>(std::floor(cellZ));

            // Track the largest rock contribution at this pixel.
            float bestRockH = 0.0f;
            float bestDome = 0.0f;
            float bestUnique = 0.0f;
            float gradX = 0.0f;
            float gradZ = 0.0f;
            float slopeLen = 0.0f;
            float normalUp = 1.0f;
            if (!inputHeights.empty())
            {
                const int xm = std::max(0, x - 1);
                const int xp = std::min(n - 1, x + 1);
                const int zm = std::max(0, z - 1);
                const int zp = std::min(n - 1, z + 1);
                const size_t idxL = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(xm);
                const size_t idxR = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(xp);
                const size_t idxD = static_cast<size_t>(zm) * static_cast<size_t>(n) + static_cast<size_t>(x);
                const size_t idxU = static_cast<size_t>(zp) * static_cast<size_t>(n) + static_cast<size_t>(x);
                gradX = (inputHeights[idxR] - inputHeights[idxL]) * invTwoCellMeters;
                gradZ = (inputHeights[idxU] - inputHeights[idxD]) * invTwoCellMeters;
                slopeLen = std::sqrt(gradX * gradX + gradZ * gradZ);
                normalUp = 1.0f / std::sqrt(1.0f + slopeLen * slopeLen);
            }

            for (int layer = 0; layer < layerCount; ++layer)
            {
                const int32_t layerSeed = rock_node::DeriveSeed(seed + layer * 1009, 1667u, 104729u);
                for (int dz = -searchRadius; dz <= searchRadius; ++dz)
                {
                    for (int dx = -searchRadius; dx <= searchRadius; ++dx)
                    {
                    const int32_t gx = baseCx + dx;
                    const int32_t gz = baseCz + dz;
                    const float jx = rock_node::HashFloat01(gx, gz, layerSeed) * 0.9f - 0.45f;
                    const float jz = rock_node::HashFloat01(gx, gz, layerSeed + 73) * 0.9f - 0.45f;
                    const float sx = static_cast<float>(gx) + 0.5f + jx;
                    const float sz = static_cast<float>(gz) + 0.5f + jz;
                    const float siteWorldX = sx * density;
                    const float siteWorldZ = sz * density;
                    const float siteU = (siteWorldX + halfSize) / terrainSize;
                    const float siteV = (siteWorldZ + halfSize) / terrainSize;
                    const float siteMask = samplePlacementMask(siteU, siteV);
                    if (siteMask <= 0.0f)
                    {
                        continue;
                    }
                    const float ddx = cellX - sx;
                    const float ddz = cellZ - sz;
                    const float d_iso = std::sqrt(ddx * ddx + ddz * ddz);

                    if (d_iso >= maxReach)
                    {
                        continue;
                    }

                    // Per-seed coverage gate: this cell may not be a rock at all.
                    const float cellRandom = rock_node::HashFloat01(gx, gz, layerSeed + 17);
                    if (cellRandom > coverage * siteMask)
                    {
                        continue;
                    }

                    // Per-rock random size in [rockSizeMinCells, rockSizeMaxCells].
                    const float sizeRand = rock_node::HashFloat01(gx, gz, sizeSeed + layerSeed);
                    const float rockSizeCells = rockSizeMinCells + sizeRand * (rockSizeMaxCells - rockSizeMinCells);
                    const float domeRadius_per = rockSizeCells * 0.5f;

                    // Per-rock rotation. rotationVar = 1 → full 2π, 0 → no rotation.
                    const float rotRand = rock_node::HashFloat01(gx, gz, rotSeed + layerSeed);
                    const float randomTheta = (rotRand - 0.5f) * 2.0f * 3.14159265358979323846f * rotationVar;
                    const float slopeTheta = (slopeLen > 1e-4f) ? std::atan2(gradZ, gradX) : 0.0f;
                    const float theta = (orientationRule == static_cast<int>(RockOrientationRule::SlopeOriented) && slopeLen > 1e-4f)
                        ? (slopeTheta + randomTheta)
                        : randomTheta;
                    const float cosT = std::cos(theta);
                    const float sinT = std::sin(theta);

                    // Per-rock area-preserving aspect. aspect ∈ [pow(2,-aspectVar), pow(2,aspectVar)].
                    // aspectAxis ∈ {0, 1} chooses which axis (in the rock's local frame) is the long one.
                    const float aspectRand = rock_node::HashFloat01(gx, gz, aspectSeed + layerSeed);
                    const float aspectExp = aspectVar * (2.0f * aspectRand - 1.0f) + styleAspectBoost;
                    const float aspect = std::pow(2.0f, aspectExp);
                    const float axisRand = rock_node::HashFloat01(gx, gz, aspectAxisSeed + layerSeed);
                    const float aspect_x = (axisRand < 0.5f) ? aspect : (1.0f / aspect);
                    const float aspect_z = 1.0f / aspect_x;

                    // Local rock-frame coordinates: rotate into the rock's
                    // own frame, then divide by the per-axis aspect to get
                    // an elliptic distance metric.
                    const float rx_unrot = ddx * cosT + ddz * sinT;
                    const float rz_unrot = -ddx * sinT + ddz * cosT;
                    const float rx = rx_unrot / aspect_x;
                    const float rz = rz_unrot / aspect_z;
                    const float slopeAlong = (orientationRule != static_cast<int>(RockOrientationRule::Flat))
                        ? (gradX * ddx + gradZ * ddz)
                        : 0.0f;
                    const float d_local = std::sqrt(rx * rx + rz * rz + slopeAlong * slopeAlong);
                    if (d_local >= domeRadius_per)
                    {
                        continue;
                    }

                    // Per-rock height variation centred on rockHeight.
                    const float heightRand = rock_node::HashFloat01(gx, gz, layerSeed + 53);
                    const float orientationHeightScale = (orientationRule == static_cast<int>(RockOrientationRule::FollowGround)) ? normalUp : 1.0f;
                    const float cellHeight = rockHeight * orientationHeightScale * (1.0f - heightJitter + heightJitter * 2.0f * heightRand);

                    // Radial component: smooth circular dome (falls off linearly
                    // from centre to elliptic boundary).
                    const float radialT = std::clamp(1.0f - d_local / domeRadius_per, 0.0f, 1.0f);

                    // Polyhedral component: signed-distance field of an irregular
                    // 4–7 sided convex polygon inscribed in the elliptic dome.
                    // When edgeSharpness > 0 we hard-clip the rock to the polygon
                    // — pixels outside the polygon get no contribution at all,
                    // so there's no halo of soft radial dome leaking past the
                    // polygon edges. The blend by edgeSharpness only affects
                    // the *interior* dome height (radial vs flat-facet shape).
                    float polyhedralT = 0.0f;
                    float topPlaneMask = 0.0f;
                    if (needPolyhedral)
                    {
                        int facetCount = std::min(7, 4 + static_cast<int>(rock_node::HashFloat01(gx, gz, polyCountSeed + layerSeed) * 4.0f)); // 4..7
                        if (polygonalStyle)
                        {
                            const float styleCount = rock_node::HashFloat01(gx, gz, polyCountSeed + layerSeed + 97);
                            facetCount = shardStyle
                                ? std::min(6, 4 + static_cast<int>(styleCount * 3.0f))
                                : std::min(8, 5 + static_cast<int>(styleCount * 4.0f));
                        }
                        const float facetCountF = static_cast<float>(facetCount);
                        const float kPi = 3.14159265358979323846f;
                        // Polygon vertices touch the elliptic boundary; edges sit
                        // at the inradius. Per-edge jitter shrinks edges further
                        // in for irregular convex shapes.
                        const float baseInradius = domeRadius_per * std::cos(kPi / facetCountF);
                        const float edgeAngularSpan = (2.0f * kPi) / facetCountF;
                        const float apexRange = shardStyle ? 0.42f : 0.28f;
                        const float apexX = polygonalStyle ? ((rock_node::HashFloat01(gx, gz, apexSeedX + layerSeed) - 0.5f) * 2.0f * baseInradius * apexRange) : 0.0f;
                        const float apexZ = polygonalStyle ? ((rock_node::HashFloat01(gx, gz, apexSeedZ + layerSeed) - 0.5f) * 2.0f * baseInradius * apexRange) : 0.0f;
                        float polyDist = polygonalStyle ? 1.0f : std::numeric_limits<float>::max();
                        for (int i = 0; i < facetCount; ++i)
                        {
                            const float baseAngle = static_cast<float>(i) * edgeAngularSpan;
                            const float aJit = (rock_node::HashFloat01(gx, gz, polyAngleSeed + layerSeed + i * 17) - 0.5f) * (edgeAngularSpan * 0.5f);
                            const float theta_i = baseAngle + aJit;
                            const float n_x = std::cos(theta_i);
                            const float n_z = std::sin(theta_i);
                            const float rJit = rock_node::HashFloat01(gx, gz, polyRadiusSeed + layerSeed + i * 23);
                            const float radiusJitter = polygonalStyle ? 0.18f : 0.3f;
                            const float r_i = baseInradius * (1.0f - rJit * radiusJitter);
                            const float interiorDist = r_i - (rx * n_x + rz * n_z);
                            if (polygonalStyle)
                            {
                                const float apexDist = r_i - (apexX * n_x + apexZ * n_z);
                                const float normalizedDist = interiorDist / std::max(apexDist, 1e-4f);
                                if (normalizedDist < polyDist) polyDist = normalizedDist;
                            }
                            else if (interiorDist < polyDist)
                            {
                                polyDist = interiorDist;
                            }
                        }
                        // Hard polygon clip — outside polygon, this rock contributes nothing.
                        if (polyDist <= 0.0f) continue;
                        if (polygonalStyle)
                        {
                            const float topCut = shardStyle ? 0.92f : 0.64f;
                            polyhedralT = std::clamp(polyDist / topCut, 0.0f, 1.0f);
                            if (!shardStyle)
                            {
                                const float topT = (polyDist - topCut) / std::max(1.0f - topCut, 1e-4f);
                                topPlaneMask = rock_node::Smoothstep01(topT);
                            }
                        }
                        else
                        {
                            polyhedralT = std::clamp(polyDist / std::max(baseInradius, 1e-4f), 0.0f, 1.0f);
                        }
                    }

                    // Blend interior dome height by edgeSharpness.
                    // Outside polygon was already excluded by the hard clip above
                    // (when needPolyhedral); so radialT here is always meaningful too.
                    const float t = (1.0f - effectiveEdgeSharpness) * radialT + effectiveEdgeSharpness * polyhedralT;
                    if (t <= 0.0f) continue;
                    const float dome = std::pow(t, domeExp);

                    // Per-rock facet field, sampled in the rock's local
                    // (rotated, unsquashed) frame. Per-rock random offset
                    // gives every rock a unique facet pattern.
                    const float subOffX = rock_node::HashFloat01(gx, gz, subOffsetSeedX + layerSeed) * 1024.0f;
                    const float subOffZ = rock_node::HashFloat01(gx, gz, subOffsetSeedZ + layerSeed) * 1024.0f;
                    float sub_f1 = 0.0f, sub_f2 = 0.0f;
                    int32_t sub_cx = 0, sub_cz = 0;
                    rock_node::VoronoiF1F2(subOffX + rx * facetScale, subOffZ + rz * facetScale,
                                           subSeedI, sub_f1, sub_f2, sub_cx, sub_cz);
                    const float smoothBump = rock_node::Smoothstep01(1.0f - sub_f1 / 0.5f) - 0.5f;
                    const float facetH = rock_node::HashFloat01(sub_cx, sub_cz, facetSeedI) - 0.5f;
                    const float edgeT = std::clamp((sub_f2 - sub_f1) * 4.0f, 0.0f, 1.0f);
                    const float facetTerm = facetH * edgeT - (1.0f - edgeT) * 0.25f;
                    const float surfaceMod = ((1.0f - effectiveFacetSharpness) * smoothBump + effectiveFacetSharpness * facetTerm) * (1.0f - topPlaneMask);

                    const float rockH = cellHeight * dome * (1.0f + bumpiness * surfaceMod);
                    if (rockH > bestRockH)
                    {
                        bestRockH = rockH;
                        bestDome = dome;
                        bestUnique = rock_node::HashFloat01(gx, gz, layerSeed + 131);
                    }
                    }
                }
            }

            if (bestRockH <= 0.0f)
            {
                continue;
            }

            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
            const float pixelMask = samplePlacementMask(
                static_cast<float>(x) * invStep,
                static_cast<float>(z) * invStep);
            if (pixelMask <= 0.0f)
            {
                continue;
            }

            const float originalH = grid.heights[idx];
            const float groundH = groundHeights.empty() ? originalH : groundHeights[idx];
            const float rockTargetH = groundH + bestRockH * pixelMask;
            grid.heights[idx] = std::max(originalH, rockTargetH);
            grid.mask[idx] = bestDome * pixelMask;
            grid.uniqueMask[idx] = bestUnique;
        }
    });
}

void ApplyScatter(HeightfieldGrid& grid, const ScatterSettings& settings, const MaskGrid* placementMask = nullptr)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount || settings.density <= 0.0f)
    {
        return;
    }

    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);

    const bool hasPlacementMask = placementMask != nullptr &&
        placementMask->resolution > 0 &&
        !placementMask->values.empty();
    const bool usesSmoothedGround = settings.groundDetailLevelM > 0.0f;
    if (!hasPlacementMask && !usesSmoothedGround &&
        settings.backend == ScatterBackend::GpuCompute && g_scatterGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_scatterGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    const float density = std::max(settings.density, 0.1f);
    const float coverage = std::clamp(settings.coverage, 0.0f, 1.0f);
    const float sizeMinM = std::clamp(settings.sizeMinM, 0.1f, 200.0f);
    const float sizeMaxM = std::clamp(std::max(settings.sizeMaxM, sizeMinM), 0.1f, 200.0f);
    const float sizeMinCells = sizeMinM / density;
    const float sizeMaxCells = sizeMaxM / density;
    const float height = std::max(settings.height, 0.0f);
    const float heightJitter = std::clamp(settings.heightJitter, 0.0f, 1.0f);
    const float rotationVar = std::clamp(settings.rotationVariation, 0.0f, 1.0f);
    const float aspectVar = std::clamp(settings.aspectVariation, 0.0f, 1.0f);
    const int shapeType = std::clamp(static_cast<int>(settings.shapeType),
        static_cast<int>(ScatterShapeType::Hemisphere),
        static_cast<int>(ScatterShapeType::Cone));
    const int orientationRule = std::clamp(static_cast<int>(settings.orientationRule),
        static_cast<int>(RockOrientationRule::Flat),
        static_cast<int>(RockOrientationRule::SlopeOriented));

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float halfSize = terrainSize * 0.5f;
    const float invStep = (n > 1) ? 1.0f / static_cast<float>(n - 1) : 0.0f;
    const float cellSizeMeters = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float invTwoCellMeters = 1.0f / (2.0f * cellSizeMeters);
    const float groundDetailM = std::clamp(settings.groundDetailLevelM, 0.0f, terrainSize * 0.5f);
    const int groundRadius = groundDetailM > 0.0f
        ? std::clamp(static_cast<int>(std::round(groundDetailM / cellSizeMeters)), 1, 128)
        : 0;
    const std::vector<float> groundHeights = groundRadius > 1 ? BoxBlurHeights(grid, groundRadius) : std::vector<float>();
    const std::vector<float> inputHeights = (orientationRule != static_cast<int>(RockOrientationRule::Flat))
        ? grid.heights
        : std::vector<float>();
    const int32_t seed = settings.seed;
    const int32_t sizeSeed = rock_node::DeriveSeed(seed, 1583u, 22441u);
    const int32_t heightSeed = rock_node::DeriveSeed(seed, 2017u, 39019u);
    const int32_t rotSeed = rock_node::DeriveSeed(seed, 4519u, 91173u);
    const int32_t aspectSeed = rock_node::DeriveSeed(seed, 2381u, 33797u);
    const int32_t aspectAxisSeed = rock_node::DeriveSeed(seed, 4093u, 51817u);
    const int32_t uniqueSeed = rock_node::DeriveSeed(seed, 1877u, 73009u);

    const auto samplePlacementMask = [&](float u, float v) {
        if (!hasPlacementMask)
        {
            return 1.0f;
        }
        return std::clamp(SampleMaskBilinear(*placementMask, u, v), 0.0f, 1.0f);
    };

    const float maxRadiusCells = sizeMaxCells * 0.5f;
    const float maxAspect = std::pow(2.0f, aspectVar);
    const float maxReach = maxRadiusCells * maxAspect;
    const int searchRadius = std::max(1, static_cast<int>(std::ceil(maxReach - 0.05f)));
    constexpr float kPi = 3.14159265358979323846f;

    ParallelForRows(n, [&](int z) {
        const float worldZ = -halfSize + static_cast<float>(z) * invStep * terrainSize;
        const float cellZ = worldZ / density;
        for (int x = 0; x < n; ++x)
        {
            const float worldX = -halfSize + static_cast<float>(x) * invStep * terrainSize;
            const float cellX = worldX / density;
            const int32_t baseCx = static_cast<int32_t>(std::floor(cellX));
            const int32_t baseCz = static_cast<int32_t>(std::floor(cellZ));

            float bestShape = 0.0f;
            float bestHeight = 0.0f;
            float bestUnique = 0.0f;
            float gradX = 0.0f;
            float gradZ = 0.0f;
            float slopeLen = 0.0f;
            float normalUp = 1.0f;
            if (!inputHeights.empty())
            {
                const int xm = std::max(0, x - 1);
                const int xp = std::min(n - 1, x + 1);
                const int zm = std::max(0, z - 1);
                const int zp = std::min(n - 1, z + 1);
                const size_t idxL = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(xm);
                const size_t idxR = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(xp);
                const size_t idxD = static_cast<size_t>(zm) * static_cast<size_t>(n) + static_cast<size_t>(x);
                const size_t idxU = static_cast<size_t>(zp) * static_cast<size_t>(n) + static_cast<size_t>(x);
                gradX = (inputHeights[idxR] - inputHeights[idxL]) * invTwoCellMeters;
                gradZ = (inputHeights[idxU] - inputHeights[idxD]) * invTwoCellMeters;
                slopeLen = std::sqrt(gradX * gradX + gradZ * gradZ);
                normalUp = 1.0f / std::sqrt(1.0f + slopeLen * slopeLen);
            }

            for (int dz = -searchRadius; dz <= searchRadius; ++dz)
            {
                for (int dx = -searchRadius; dx <= searchRadius; ++dx)
                {
                    const int32_t gx = baseCx + dx;
                    const int32_t gz = baseCz + dz;
                    const float jx = rock_node::HashFloat01(gx, gz, seed) * 0.9f - 0.45f;
                    const float jz = rock_node::HashFloat01(gx, gz, seed + 73) * 0.9f - 0.45f;
                    const float sx = static_cast<float>(gx) + 0.5f + jx;
                    const float sz = static_cast<float>(gz) + 0.5f + jz;
                    const float siteWorldX = sx * density;
                    const float siteWorldZ = sz * density;
                    const float siteU = (siteWorldX + halfSize) / terrainSize;
                    const float siteV = (siteWorldZ + halfSize) / terrainSize;
                    const float siteMask = samplePlacementMask(siteU, siteV);
                    if (siteMask <= 0.0f)
                    {
                        continue;
                    }
                    if (rock_node::HashFloat01(gx, gz, seed + 17) > coverage * siteMask)
                    {
                        continue;
                    }

                    const float ddx = cellX - sx;
                    const float ddz = cellZ - sz;
                    if (std::sqrt(ddx * ddx + ddz * ddz) >= maxReach)
                    {
                        continue;
                    }

                    const float sizeRand = rock_node::HashFloat01(gx, gz, sizeSeed);
                    const float sizeCells = sizeMinCells + sizeRand * (sizeMaxCells - sizeMinCells);
                    const float radiusCells = std::max(sizeCells * 0.5f, 1e-4f);
                    const float randomTheta = (rock_node::HashFloat01(gx, gz, rotSeed) - 0.5f) * 2.0f * kPi * rotationVar;
                    const float slopeTheta = (slopeLen > 1e-4f) ? std::atan2(gradZ, gradX) : 0.0f;
                    const float theta = (orientationRule == static_cast<int>(RockOrientationRule::SlopeOriented) && slopeLen > 1e-4f)
                        ? (slopeTheta + randomTheta)
                        : randomTheta;
                    const float cosT = std::cos(theta);
                    const float sinT = std::sin(theta);
                    const float aspectRand = rock_node::HashFloat01(gx, gz, aspectSeed);
                    const float aspectExp = aspectVar * (2.0f * aspectRand - 1.0f);
                    const float aspect = std::pow(2.0f, aspectExp);
                    const bool longX = rock_node::HashFloat01(gx, gz, aspectAxisSeed) < 0.5f;
                    const float aspectX = longX ? aspect : (1.0f / aspect);
                    const float aspectZ = 1.0f / aspectX;
                    const float rxUnrot = ddx * cosT + ddz * sinT;
                    const float rzUnrot = -ddx * sinT + ddz * cosT;
                    const float rx = rxUnrot / aspectX;
                    const float rz = rzUnrot / aspectZ;
                    const float slopeAlong = (orientationRule != static_cast<int>(RockOrientationRule::Flat))
                        ? (gradX * ddx + gradZ * ddz)
                        : 0.0f;
                    const float normalizedDistance = std::sqrt(rx * rx + rz * rz + slopeAlong * slopeAlong) / radiusCells;
                    if (normalizedDistance >= 1.0f)
                    {
                        continue;
                    }

                    const float shape = shapeType == static_cast<int>(ScatterShapeType::Cone)
                        ? std::clamp(1.0f - normalizedDistance, 0.0f, 1.0f)
                        : std::sqrt(std::max(0.0f, 1.0f - normalizedDistance * normalizedDistance));
                    const float heightRand = rock_node::HashFloat01(gx, gz, heightSeed);
                    const float orientationHeightScale = (orientationRule == static_cast<int>(RockOrientationRule::FollowGround)) ? normalUp : 1.0f;
                    const float cellHeight = height * orientationHeightScale * (1.0f - heightJitter + heightJitter * 2.0f * heightRand);
                    const float contribution = cellHeight * shape;
                    if (shape > bestShape)
                    {
                        bestShape = shape;
                        bestHeight = contribution;
                        bestUnique = rock_node::HashFloat01(gx, gz, uniqueSeed);
                    }
                }
            }

            if (bestShape <= 0.0f)
            {
                continue;
            }

            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
            const float pixelMask = samplePlacementMask(
                static_cast<float>(x) * invStep,
                static_cast<float>(z) * invStep);
            if (pixelMask <= 0.0f)
            {
                continue;
            }

            const float originalH = grid.heights[idx];
            const float groundH = groundHeights.empty() ? originalH : groundHeights[idx];
            if (bestHeight > 0.0f)
            {
                grid.heights[idx] = std::max(originalH, groundH + bestHeight * pixelMask);
            }
            grid.mask[idx] = bestShape * pixelMask;
            grid.uniqueMask[idx] = bestUnique;
        }
    });
}

struct CrumblingParticle
{
    float x = 0.0f;
    float z = 0.0f;
    float sizeCells = 1.0f;
    float height = 0.0f;
    float rotation = 0.0f;
    float aspect = 1.0f;
    float unique = 0.0f;
};

void ApplyCrumbling(HeightfieldGrid& grid, const CrumblingSettings& settings, const MaskGrid* emissionMask)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount)
    {
        return;
    }

    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSize = terrainSize / static_cast<float>(std::max(1, n - 1));
    const int physicsCount = std::clamp(settings.physicsCount, 0, 512);
    const float amount = std::clamp(settings.debrisAmount, 0.0f, 1.0f);
    if (amount <= 0.0f)
    {
        return;
    }
    const float minSizeM = std::clamp(settings.debrisSizeMinM, 0.1f, 1000.0f);
    const float maxSizeM = std::clamp(std::max(settings.debrisSizeMaxM, minSizeM), 0.1f, 1000.0f);
    const float minSizeCells = std::max(0.5f, minSizeM / cellSize);
    const float maxSizeCells = std::max(minSizeCells, maxSizeM / cellSize);
    const float gravity = std::clamp(settings.gravity, 0.0f, 1.0f);
    const int style = std::clamp(static_cast<int>(settings.style),
        static_cast<int>(RockStyle::Classic),
        static_cast<int>(RockStyle::Shard));
    const bool polygonalStyle = style != static_cast<int>(RockStyle::Classic);
    const bool shardStyle = style == static_cast<int>(RockStyle::Shard);
    const int targetParticles = std::clamp(static_cast<int>(std::round(256.0f + amount * 12000.0f)), 1, 12000);
    const int maxAttempts = targetParticles * 10;
    const float invTwoCell = 1.0f / (2.0f * cellSize);
    const float kPi = 3.14159265358979323846f;

    const auto hash01 = [](uint32_t& state) {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>((state >> 8) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
    };
    const auto sampleMask = [&](float gx, float gz) {
        if (emissionMask == nullptr || emissionMask->resolution <= 1 || emissionMask->values.empty())
        {
            return 1.0f;
        }
        const float u = std::clamp(gx / static_cast<float>(n - 1), 0.0f, 1.0f);
        const float v = std::clamp(gz / static_cast<float>(n - 1), 0.0f, 1.0f);
        return std::clamp(SampleHeightfieldValue(emissionMask->values, emissionMask->resolution, u, v), 0.0f, 1.0f);
    };
    const auto sampleHeight = [&](float gx, float gz) {
        const float u = std::clamp(gx / static_cast<float>(n - 1), 0.0f, 1.0f);
        const float v = std::clamp(gz / static_cast<float>(n - 1), 0.0f, 1.0f);
        return SampleHeightfieldValue(grid.heights, n, u, v);
    };
    const auto gradientAt = [&](float gx, float gz, float& gradX, float& gradZ) {
        const float hL = sampleHeight(gx - 1.0f, gz);
        const float hR = sampleHeight(gx + 1.0f, gz);
        const float hD = sampleHeight(gx, gz - 1.0f);
        const float hU = sampleHeight(gx, gz + 1.0f);
        gradX = (hR - hL) * invTwoCell;
        gradZ = (hU - hD) * invTwoCell;
    };

    std::vector<CrumblingParticle> particles;
    particles.reserve(static_cast<size_t>(targetParticles));
    uint32_t state = static_cast<uint32_t>(settings.seed) * 747796405u + 2891336453u;
    for (int attempt = 0; attempt < maxAttempts && static_cast<int>(particles.size()) < targetParticles; ++attempt)
    {
        const float x0 = hash01(state) * static_cast<float>(n - 1);
        const float z0 = hash01(state) * static_cast<float>(n - 1);
        const float source = sampleMask(x0, z0);
        if (hash01(state) > source * amount)
        {
            continue;
        }

        const float sizeT = hash01(state);
        const float sizeCells = minSizeCells + sizeT * (maxSizeCells - minSizeCells);
        const float sizeMeters = sizeCells * cellSize;
        float x = x0;
        float z = z0;
        float dirX = hash01(state) * 2.0f - 1.0f;
        float dirZ = hash01(state) * 2.0f - 1.0f;
        float dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
        if (dirLen > 1e-4f)
        {
            dirX /= dirLen;
            dirZ /= dirLen;
        }
        const float stepCells = std::clamp(sizeCells * (0.18f + gravity * 0.34f), 0.25f, 8.0f);
        for (int step = 0; step < physicsCount; ++step)
        {
            float gradX = 0.0f;
            float gradZ = 0.0f;
            gradientAt(x, z, gradX, gradZ);
            float downX = -gradX;
            float downZ = -gradZ;
            const float downLen = std::sqrt(downX * downX + downZ * downZ);
            if (downLen < 1e-5f)
            {
                break;
            }
            downX /= downLen;
            downZ /= downLen;

            const float wander = (hash01(state) - 0.5f) * (1.0f - gravity) * 0.75f;
            const float cosW = std::cos(wander);
            const float sinW = std::sin(wander);
            const float wx = downX * cosW - downZ * sinW;
            const float wz = downX * sinW + downZ * cosW;
            dirX = std::lerp(dirX, wx, 0.35f + gravity * 0.55f);
            dirZ = std::lerp(dirZ, wz, 0.35f + gravity * 0.55f);
            dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
            if (dirLen < 1e-5f)
            {
                break;
            }
            dirX /= dirLen;
            dirZ /= dirLen;

            const float nextX = x + dirX * stepCells;
            const float nextZ = z + dirZ * stepCells;
            if (nextX <= 0.0f || nextX >= static_cast<float>(n - 1) ||
                nextZ <= 0.0f || nextZ >= static_cast<float>(n - 1))
            {
                break;
            }
            const float h0 = sampleHeight(x, z);
            const float h1 = sampleHeight(nextX, nextZ);
            x = nextX;
            z = nextZ;
            if (h0 - h1 < cellSize * 0.003f && step > physicsCount / 4)
            {
                break;
            }
        }

        CrumblingParticle p;
        p.x = x;
        p.z = z;
        p.sizeCells = sizeCells;
        p.height = sizeMeters * (0.10f + 0.18f * amount) * (0.65f + 0.7f * hash01(state));
        p.rotation = std::atan2(dirZ, dirX) + (hash01(state) - 0.5f) * kPi * (shardStyle ? 0.35f : 1.0f);
        const float aspectBoost = shardStyle ? 1.2f : (polygonalStyle ? 0.45f : 0.15f);
        p.aspect = std::pow(2.0f, aspectBoost * hash01(state));
        p.unique = hash01(state);
        particles.push_back(p);
    }

    std::vector<float> debrisHeight(cellCount, 0.0f);
    for (const CrumblingParticle& p : particles)
    {
        const float radius = std::max(0.5f, p.sizeCells * 0.5f);
        const float reach = radius * std::max(p.aspect, 1.0f / p.aspect) * 1.1f;
        const int minX = std::clamp(static_cast<int>(std::floor(p.x - reach)), 0, n - 1);
        const int maxX = std::clamp(static_cast<int>(std::ceil(p.x + reach)), 0, n - 1);
        const int minZ = std::clamp(static_cast<int>(std::floor(p.z - reach)), 0, n - 1);
        const int maxZ = std::clamp(static_cast<int>(std::ceil(p.z + reach)), 0, n - 1);
        const float cosT = std::cos(p.rotation);
        const float sinT = std::sin(p.rotation);
        const int facets = shardStyle ? 4 : 6;
        const float inradius = radius * std::cos(kPi / static_cast<float>(facets));
        for (int z = minZ; z <= maxZ; ++z)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float dx = static_cast<float>(x) - p.x;
                const float dz = static_cast<float>(z) - p.z;
                const float rx = (dx * cosT + dz * sinT) / p.aspect;
                const float rz = (-dx * sinT + dz * cosT) * p.aspect;
                const float dist = std::sqrt(rx * rx + rz * rz);
                if (dist >= radius)
                {
                    continue;
                }
                float t = std::clamp(1.0f - dist / radius, 0.0f, 1.0f);
                if (polygonalStyle)
                {
                    float polyDist = std::numeric_limits<float>::max();
                    for (int i = 0; i < facets; ++i)
                    {
                        const float a = (static_cast<float>(i) + p.unique) * (2.0f * kPi / static_cast<float>(facets));
                        const float interior = inradius - (rx * std::cos(a) + rz * std::sin(a));
                        polyDist = std::min(polyDist, interior);
                    }
                    if (polyDist <= 0.0f)
                    {
                        continue;
                    }
                    t = std::clamp(polyDist / std::max(inradius, 1e-4f), 0.0f, 1.0f);
                }
                const float dome = shardStyle ? t : rock_node::Smoothstep01(t);
                const float h = p.height * dome;
                const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
                if (h > debrisHeight[idx])
                {
                    debrisHeight[idx] = h;
                    grid.mask[idx] = std::max(grid.mask[idx], dome);
                    grid.uniqueMask[idx] = p.unique;
                }
            }
        }
    }
    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t idx = rowBase + static_cast<size_t>(x);
            grid.heights[idx] += debrisHeight[idx];
        }
    });
}

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

// Sediment node — Particle backend.
//
// Hydraulic-erosion-style stochastic particle sim. Each particle has a
// position, direction, velocity, water and carried-sediment amount.
// At every step it samples the height gradient with bilinear weights,
// blends the descent direction with its previous direction (inertia),
// moves one cell, and erodes or deposits based on its current carrying
// capacity. The result is dendritic deposition lines along particle
// paths — closely matches the GeoGen reference.
//
// CPU parallelism: each thread owns a disjoint slice of particles AND
// a private delta map. Threads never write to a shared buffer, so the
// only synchronisation is the final reduction. This trades a small
// loss of inter-particle interaction (a particle in thread A doesn't
// see thread B's deposits as it moves) for race-free O(1)-locked
// scalability — the standard choice for procedural hydraulic erosion.
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

// Phase 1 mesh build: pre-allocate every vertex / triangle / edge slot so
// the hot loops can write at known indices and run in parallel. Top
// surface uses gradient-based per-vertex normals computed straight from
// the heightfield (no per-triangle accumulation, no race), walls and the
// bottom face have constant normals, and edges are emitted in a
// structured pattern so the unordered_set dedup is gone.
MeshData BuildMeshFromHeightfield(const HeightfieldGrid& grid, int meshResolution)
{
    MeshData mesh;
    const int gridResolution = grid.resolution;
    if (gridResolution < 2 || grid.heights.size() < static_cast<size_t>(gridResolution * gridResolution))
    {
        return mesh;
    }
    meshResolution = std::clamp(meshResolution, 2, 2048);
    const int M = meshResolution;
    const int M1 = M - 1;
    const float halfSize = grid.terrainSizeMeters * 0.5f;
    const float worldDX = grid.terrainSizeMeters / static_cast<float>(M1);
    const float baseY = 0.0f;
    const float invM1 = 1.0f / static_cast<float>(M1);

    // Vertex layout (all sizes exact, no push_back):
    //   [0, M*M)                         top surface
    //   [topEnd, topEnd + 16*M1)          walls — 4 sides × M1 segments × 4 verts
    //   [wallEnd, wallEnd + M*M)          bottom surface
    const size_t topVerts = static_cast<size_t>(M) * static_cast<size_t>(M);
    const size_t wallVerts = static_cast<size_t>(16) * static_cast<size_t>(M1);
    const size_t bottomVerts = topVerts;
    const size_t topVertsStart = 0;
    const size_t wallVertsStart = topVertsStart + topVerts;
    const size_t bottomVertsStart = wallVertsStart + wallVerts;
    mesh.vertices.resize(topVerts + wallVerts + bottomVerts);

    // Triangle layout:
    //   [0, 2*M1*M1)                     top surface
    //   [topTriEnd, topTriEnd + 8*M1)    walls — 2 tris × 4 sides × M1 segments
    //   [wallTriEnd, wallTriEnd + 2*M1*M1) bottom surface
    const size_t topTris = static_cast<size_t>(M1) * static_cast<size_t>(M1) * 2;
    const size_t wallTris = static_cast<size_t>(M1) * static_cast<size_t>(8);
    const size_t bottomTris = topTris;
    const size_t topTrisStart = 0;
    const size_t wallTrisStart = topTrisStart + topTris;
    const size_t bottomTrisStart = wallTrisStart + wallTris;
    mesh.triangles.resize(topTris + wallTris + bottomTris);

    // Edge layout (structured emission, no dedup):
    //   top: M*M1 horizontal + M1*M vertical + M1*M1 diagonals
    //   walls: 5 unique edges per segment × 4 sides × M1 segments
    //   bottom: same counts as top
    const size_t topEdges = static_cast<size_t>(M) * static_cast<size_t>(M1) * 2u + static_cast<size_t>(M1) * static_cast<size_t>(M1);
    const size_t wallEdges = static_cast<size_t>(5) * static_cast<size_t>(4) * static_cast<size_t>(M1);
    const size_t bottomEdges = topEdges;
    const size_t topEdgesStart = 0;
    const size_t wallEdgesStart = topEdgesStart + topEdges;
    const size_t bottomEdgesStart = wallEdgesStart + wallEdges;
    mesh.edges.resize(topEdges + wallEdges + bottomEdges);

    const auto topIdx = [M](int x, int z) -> uint32_t {
        return static_cast<uint32_t>(z * M + x);
    };
    const auto bottomIdx = [M, bottomVertsStart](int x, int z) -> uint32_t {
        return static_cast<uint32_t>(bottomVertsStart + static_cast<size_t>(z * M + x));
    };

    // ---- Top surface vertices (parallel) ----
    // Gradient normal is computed from a 4-tap central difference of the
    // heightfield. SampleHeightfieldValue clamps u/v to [0, 1] so the
    // boundary samples degrade to a one-sided difference automatically.
    ParallelForRows(M, [&](int z) {
        const float v = static_cast<float>(z) * invM1;
        const float worldZ = std::lerp(halfSize, -halfSize, v);
        const float vMinus = static_cast<float>(z - 1) * invM1;
        const float vPlus = static_cast<float>(z + 1) * invM1;
        for (int x = 0; x < M; ++x)
        {
            const float u = static_cast<float>(x) * invM1;
            const float worldX = std::lerp(-halfSize, halfSize, u);
            const float uMinus = static_cast<float>(x - 1) * invM1;
            const float uPlus = static_cast<float>(x + 1) * invM1;

            const float h = SampleHeightfieldValue(grid.heights, gridResolution, u, v);
            const float hxm = SampleHeightfieldValue(grid.heights, gridResolution, uMinus, v);
            const float hxp = SampleHeightfieldValue(grid.heights, gridResolution, uPlus, v);
            const float hzm = SampleHeightfieldValue(grid.heights, gridResolution, u, vMinus);
            const float hzp = SampleHeightfieldValue(grid.heights, gridResolution, u, vPlus);

            // World z increases as v decreases (worldZ = lerp(halfSize, -halfSize, v))
            // so dhdz against world z is (hzm - hzp) / (2 * dx).
            const float dhdx = (hxp - hxm) / (2.0f * worldDX);
            const float dhdz = (hzm - hzp) / (2.0f * worldDX);
            const float nxRaw = -dhdx;
            const float nyRaw = 1.0f;
            const float nzRaw = -dhdz;
            const float lenSq = nxRaw * nxRaw + nyRaw * nyRaw + nzRaw * nzRaw;
            const float invLen = (lenSq > 1e-12f) ? (1.0f / std::sqrt(lenSq)) : 1.0f;

            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(M) + static_cast<size_t>(x);
            mesh.vertices[idx] = {
                worldX,
                h,
                worldZ,
                nxRaw * invLen,
                nyRaw * invLen,
                nzRaw * invLen,
                SampleHeightfieldValue(grid.mask, gridResolution, u, v),
            };
        }
    });

    // ---- Top surface triangles (parallel, fixed winding) ----
    ParallelForRows(M1, [&](int z) {
        const size_t rowBase = topTrisStart + static_cast<size_t>(z) * static_cast<size_t>(M1) * 2;
        for (int x = 0; x < M1; ++x)
        {
            const uint32_t a = topIdx(x, z);
            const uint32_t b = topIdx(x + 1, z);
            const uint32_t c = topIdx(x + 1, z + 1);
            const uint32_t d = topIdx(x, z + 1);
            const size_t triIdx = rowBase + static_cast<size_t>(x) * 2;
            mesh.triangles[triIdx + 0] = {a, b, c};
            mesh.triangles[triIdx + 1] = {a, c, d};
        }
    });

    // ---- Top surface edges (structured, parallel) ----
    // Per row (z, z+1) emit: horizontal at z, plus the row's vertical and
    // diagonals between z and z+1. The last row z=M-1 has only horizontal.
    ParallelForRows(M, [&](int z) {
        const bool hasNextRow = z < M1;
        const size_t rowEdgesBefore = static_cast<size_t>(z) * (static_cast<size_t>(M1) * 3u);
        const size_t lastRowOffset = hasNextRow ? rowEdgesBefore : (static_cast<size_t>(M1) * static_cast<size_t>(M1) * 3u);
        size_t cursor = topEdgesStart + lastRowOffset;
        // Horizontal edges along row z.
        for (int x = 0; x < M1; ++x)
        {
            mesh.edges[cursor++] = {topIdx(x, z), topIdx(x + 1, z)};
        }
        if (!hasNextRow) return;
        // Vertical and diagonal edges between row z and z+1.
        for (int x = 0; x < M1; ++x)
        {
            mesh.edges[cursor++] = {topIdx(x, z), topIdx(x, z + 1)};
            mesh.edges[cursor++] = {topIdx(x, z), topIdx(x + 1, z + 1)};
        }
        // Final right-edge vertical for x = M1.
        mesh.edges[cursor++] = {topIdx(M1, z), topIdx(M1, z + 1)};
    });

    // ---- Walls (parallel per side) ----
    // Each segment owns 4 vertices (TopA, TopB, BottomA, BottomB) and 2
    // triangles. Normals are constant per side, so no gradient sampling
    // needed. Emit 5 unique edges per segment too.
    // 壁の `mask` には sentinel 値 (>1.0 で PSEdge の負センチネルとも非衝突)
    // を入れ、シェーダー側のマスクプレビューで一律グレーに塗り潰す。上端の
    // マスクをそのまま継承すると、上端 1 セルのマスクが縦に引き伸ばされて
    // 見えてしまうため。
    constexpr float kWallMaskSentinel = 2.0f;
    auto emitWallSegment = [&](size_t segIndex, uint32_t topAIdx, uint32_t topBIdx,
                               float nx, float nz) {
        const size_t vBase = wallVertsStart + segIndex * 4;
        const size_t triBase = wallTrisStart + segIndex * 2;
        const size_t edgeBase = wallEdgesStart + segIndex * 5;

        const MeshVertex& va = mesh.vertices[topAIdx];
        const MeshVertex& vb = mesh.vertices[topBIdx];
        mesh.vertices[vBase + 0] = {va.x, va.y,  va.z, nx, 0.0f, nz, kWallMaskSentinel};  // TopA
        mesh.vertices[vBase + 1] = {vb.x, vb.y,  vb.z, nx, 0.0f, nz, kWallMaskSentinel};  // TopB
        mesh.vertices[vBase + 2] = {va.x, baseY, va.z, nx, 0.0f, nz, kWallMaskSentinel};  // BottomA
        mesh.vertices[vBase + 3] = {vb.x, baseY, vb.z, nx, 0.0f, nz, kWallMaskSentinel};  // BottomB

        const uint32_t v0 = static_cast<uint32_t>(vBase + 0);
        const uint32_t v1 = static_cast<uint32_t>(vBase + 1);
        const uint32_t v2 = static_cast<uint32_t>(vBase + 2);
        const uint32_t v3 = static_cast<uint32_t>(vBase + 3);
        // Same winding the original used (CCW from outside).
        mesh.triangles[triBase + 0] = {v0, v2, v3};
        mesh.triangles[triBase + 1] = {v0, v3, v1};

        mesh.edges[edgeBase + 0] = {v0, v1};  // top edge of segment
        mesh.edges[edgeBase + 1] = {v2, v3};  // bottom edge
        mesh.edges[edgeBase + 2] = {v0, v2};  // left vertical
        mesh.edges[edgeBase + 3] = {v1, v3};  // right vertical
        mesh.edges[edgeBase + 4] = {v0, v3};  // diagonal
    };

    // Side 0 = front (+Z, world +halfSize), Side 1 = back (-Z),
    // Side 2 = left (-X), Side 3 = right (+X).
    ParallelForRows(M1, [&](int s) {
        emitWallSegment(0u * static_cast<size_t>(M1) + static_cast<size_t>(s),
                         topIdx(s, 0),       topIdx(s + 1, 0),       0.0f,  1.0f);
        emitWallSegment(1u * static_cast<size_t>(M1) + static_cast<size_t>(s),
                         topIdx(s + 1, M1),  topIdx(s, M1),          0.0f, -1.0f);
        emitWallSegment(2u * static_cast<size_t>(M1) + static_cast<size_t>(s),
                         topIdx(0, s + 1),   topIdx(0, s),          -1.0f,  0.0f);
        emitWallSegment(3u * static_cast<size_t>(M1) + static_cast<size_t>(s),
                         topIdx(M1, s),      topIdx(M1, s + 1),      1.0f,  0.0f);
    });

    // ---- Bottom surface vertices (parallel, constant down-facing normal) ----
    ParallelForRows(M, [&](int z) {
        const float v = static_cast<float>(z) * invM1;
        const float worldZ = std::lerp(halfSize, -halfSize, v);
        for (int x = 0; x < M; ++x)
        {
            const float u = static_cast<float>(x) * invM1;
            const float worldX = std::lerp(-halfSize, halfSize, u);
            const size_t idx = bottomVertsStart + static_cast<size_t>(z) * static_cast<size_t>(M) + static_cast<size_t>(x);
            mesh.vertices[idx] = {worldX, baseY, worldZ, 0.0f, -1.0f, 0.0f, 0.0f};
        }
    });

    // ---- Bottom surface triangles (parallel, reverse winding so normal faces down) ----
    ParallelForRows(M1, [&](int z) {
        const size_t rowBase = bottomTrisStart + static_cast<size_t>(z) * static_cast<size_t>(M1) * 2;
        for (int x = 0; x < M1; ++x)
        {
            const uint32_t a = bottomIdx(x, z);
            const uint32_t b = bottomIdx(x + 1, z);
            const uint32_t c = bottomIdx(x + 1, z + 1);
            const uint32_t d = bottomIdx(x, z + 1);
            const size_t triIdx = rowBase + static_cast<size_t>(x) * 2;
            mesh.triangles[triIdx + 0] = {a, c, b};
            mesh.triangles[triIdx + 1] = {a, d, c};
        }
    });

    // ---- Bottom surface edges (mirror top layout) ----
    ParallelForRows(M, [&](int z) {
        const bool hasNextRow = z < M1;
        const size_t rowEdgesBefore = static_cast<size_t>(z) * (static_cast<size_t>(M1) * 3u);
        const size_t lastRowOffset = hasNextRow ? rowEdgesBefore : (static_cast<size_t>(M1) * static_cast<size_t>(M1) * 3u);
        size_t cursor = bottomEdgesStart + lastRowOffset;
        for (int x = 0; x < M1; ++x)
        {
            mesh.edges[cursor++] = {bottomIdx(x, z), bottomIdx(x + 1, z)};
        }
        if (!hasNextRow) return;
        for (int x = 0; x < M1; ++x)
        {
            mesh.edges[cursor++] = {bottomIdx(x, z), bottomIdx(x, z + 1)};
            mesh.edges[cursor++] = {bottomIdx(x, z), bottomIdx(x + 1, z + 1)};
        }
        mesh.edges[cursor++] = {bottomIdx(M1, z), bottomIdx(M1, z + 1)};
    });

    return mesh;
}

template <typename Settings>
int EffectiveMeshResolution(const Settings& settings, int maxResolution = 512)
{
    const int divisor = 1 << std::clamp(settings.lod, 0, 4);
    return std::clamp(settings.resolution / divisor, 16, maxResolution);
}

// Lightweight mesh builder for Mask Noise / Mask Blend previews. The
// heightfield is flat (y = 0), so we skip the wall and bottom geometry
// that BuildMeshFromHeightfield needs for terrain. All surface normals
// are (0, 1, 0) and the grid topology is regular, so triangles and edges
// can be written by index in parallel rows.
MeshData BuildFlatMaskMesh(const HeightfieldGrid& grid, int meshResolution)
{
    MeshData mesh;
    if (grid.resolution < 2 || grid.mask.empty())
    {
        return mesh;
    }
    meshResolution = std::clamp(meshResolution, 2, 2048);

    const int M = meshResolution;
    const int gridResolution = grid.resolution;
    const float halfSize = grid.terrainSizeMeters * 0.5f;
    const size_t vertexCount = static_cast<size_t>(M) * static_cast<size_t>(M);
    const size_t triangleCount = static_cast<size_t>(M - 1) * static_cast<size_t>(M - 1) * 2u;
    const size_t horizEdges = static_cast<size_t>(M) * static_cast<size_t>(M - 1);
    const size_t vertEdges = static_cast<size_t>(M - 1) * static_cast<size_t>(M);
    const size_t diagEdges = static_cast<size_t>(M - 1) * static_cast<size_t>(M - 1);

    mesh.vertices.resize(vertexCount);
    mesh.triangles.resize(triangleCount);
    mesh.edges.resize(horizEdges + vertEdges + diagEdges);

    const float invDenom = 1.0f / static_cast<float>(M - 1);

    ParallelForRows(M, [&](int z) {
        const float v = static_cast<float>(z) * invDenom;
        const float zPos = std::lerp(halfSize, -halfSize, v);
        const size_t rowStart = static_cast<size_t>(z) * static_cast<size_t>(M);
        for (int x = 0; x < M; ++x)
        {
            const float u = static_cast<float>(x) * invDenom;
            mesh.vertices[rowStart + static_cast<size_t>(x)] = MeshVertex{
                std::lerp(-halfSize, halfSize, u),
                0.0f,
                zPos,
                0.0f,
                1.0f,
                0.0f,
                SampleHeightfieldValue(grid.mask, gridResolution, u, v),
            };
        }
    });

    ParallelForRows(M - 1, [&](int z) {
        const size_t rowStart = static_cast<size_t>(z) * static_cast<size_t>(M - 1) * 2u;
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        const uint32_t nextRowOffset = rowOffset + static_cast<uint32_t>(M);
        for (int x = 0; x < M - 1; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            const uint32_t b = a + 1u;
            const uint32_t c = nextRowOffset + static_cast<uint32_t>(x) + 1u;
            const uint32_t d = nextRowOffset + static_cast<uint32_t>(x);
            mesh.triangles[rowStart + static_cast<size_t>(x) * 2u + 0u] = {a, b, c};
            mesh.triangles[rowStart + static_cast<size_t>(x) * 2u + 1u] = {a, c, d};
        }
    });

    // Horizontal edges: M rows × (M-1) edges per row.
    ParallelForRows(M, [&](int z) {
        const size_t rowStart = static_cast<size_t>(z) * static_cast<size_t>(M - 1);
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        for (int x = 0; x < M - 1; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            mesh.edges[rowStart + static_cast<size_t>(x)] = {a, a + 1u};
        }
    });

    // Vertical edges: (M-1) rows × M edges per row.
    const size_t vertOffset = horizEdges;
    ParallelForRows(M - 1, [&](int z) {
        const size_t rowStart = vertOffset + static_cast<size_t>(z) * static_cast<size_t>(M);
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        for (int x = 0; x < M; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            mesh.edges[rowStart + static_cast<size_t>(x)] = {a, a + static_cast<uint32_t>(M)};
        }
    });

    // Diagonal edges (one per quad): (M-1) × (M-1).
    const size_t diagOffset = horizEdges + vertEdges;
    ParallelForRows(M - 1, [&](int z) {
        const size_t rowStart = diagOffset + static_cast<size_t>(z) * static_cast<size_t>(M - 1);
        const uint32_t rowOffset = static_cast<uint32_t>(z) * static_cast<uint32_t>(M);
        for (int x = 0; x < M - 1; ++x)
        {
            const uint32_t a = rowOffset + static_cast<uint32_t>(x);
            const uint32_t b = a + static_cast<uint32_t>(M) + 1u;
            mesh.edges[rowStart + static_cast<size_t>(x)] = {a, b};
        }
    });

    return mesh;
}

void ApplyHeightfieldOperation(HeightfieldGrid& grid, const HeightfieldPipeline::HeightfieldOperation& operation)
{
    switch (operation.kind)
    {
    case HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur:
        ApplyHeightmapBlur(grid, operation.heightmapBlur);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion:
        ApplyMultiScaleErosion(grid, operation.multiScaleErosion);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskCurvature:
        ApplyMaskCurvature(grid, operation.maskCurvature);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskSlope:
        ApplyMaskSlope(grid, operation.maskSlope);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskHeight:
        ApplyMaskHeight(grid, operation.maskHeight);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::Crumbling:
        ApplyCrumbling(grid, operation.crumbling, nullptr);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial:
        ApplyMaskFluvial(grid, operation.maskFluvial);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::Rock:
        ApplyRock(grid, operation.rock);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::Scatter:
        ApplyScatter(grid, operation.scatter);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::Sediment:
        ApplySediment(grid, operation.sediment);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::Snow:
        ApplySnow(grid, operation.snow);
        break;
    }
}

uint64_t HashHeightfieldOperation(const HeightfieldPipeline::HeightfieldOperation& operation, int resolution)
{
    switch (operation.kind)
    {
    case HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur:
        return HashHeightmapBlurSettings(operation.heightmapBlur, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion:
        return HashMultiScaleErosionSettings(operation.multiScaleErosion, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskCurvature:
        return HashMaskCurvatureSettings(operation.maskCurvature, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskSlope:
        return HashMaskSlopeSettings(operation.maskSlope, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskHeight:
        return HashMaskHeightSettings(operation.maskHeight, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::Crumbling:
        return HashCrumblingSettings(operation.crumbling, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial:
        return HashMaskFluvialSettings(operation.maskFluvial, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::Rock:
        return HashRockSettings(operation.rock, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::Scatter:
        return HashScatterSettings(operation.scatter, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::Sediment:
        return HashSedimentSettings(operation.sediment, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::Snow:
        return HashSnowSettings(operation.snow, resolution);
    }
    return 0;
}

HeightfieldPipeline::HeightfieldOperation MakeHeightfieldOperation(const Node& node)
{
    HeightfieldPipeline::HeightfieldOperation operation;
    operation.nodeId = node.id;
    switch (node.kind)
    {
    case NodeKind::HeightmapBlur:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::HeightmapBlur;
        operation.heightmapBlur = node.heightmapBlur;
        break;
    case NodeKind::MultiScaleErosion:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::MultiScaleErosion;
        operation.multiScaleErosion = node.multiScaleErosion;
        break;
    case NodeKind::MaskCurvature:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::MaskCurvature;
        operation.maskCurvature = node.maskCurvature;
        break;
    case NodeKind::MaskSlope:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::MaskSlope;
        operation.maskSlope = node.maskSlope;
        break;
    case NodeKind::MaskHeight:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::MaskHeight;
        operation.maskHeight = node.maskHeight;
        break;
    case NodeKind::Crumbling:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::Crumbling;
        operation.crumbling = node.crumbling;
        break;
    case NodeKind::MaskFluvial:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::MaskFluvial;
        operation.maskFluvial = node.maskFluvial;
        break;
    case NodeKind::Rock:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::Rock;
        operation.rock = node.rock;
        break;
    case NodeKind::Scatter:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::Scatter;
        operation.scatter = node.scatter;
        break;
    case NodeKind::Sediment:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::Sediment;
        operation.sediment = node.sediment;
        break;
    case NodeKind::Snow:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::Snow;
        operation.snow = node.snow;
        break;
    default:
        operation.nodeId = 0;
        break;
    }
    return operation;
}

bool IsHeightfieldOperationNode(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::HeightmapBlur:
    case NodeKind::MultiScaleErosion:
    case NodeKind::MaskCurvature:
    case NodeKind::MaskSlope:
    case NodeKind::MaskHeight:
    case NodeKind::Crumbling:
    case NodeKind::MaskFluvial:
    case NodeKind::Rock:
    case NodeKind::Scatter:
    case NodeKind::Sediment:
    case NodeKind::Snow:
        return true;
    default:
        return false;
    }
}

MeshData BuildMeshFromHeightPipeline(const HeightfieldPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap, HeightfieldGrid* previewGrid = nullptr)
{
    const int simulationResolution = std::clamp(pipeline.simulationResolution, 2, 2048);
    const float terrainSizeMeters = std::max(1.0f, pipeline.terrainSizeMeters);
    HeightfieldGrid grid = pipeline.useShape
        ? BuildHeightfieldFromShape(pipeline.shape, simulationResolution, terrainSizeMeters, message)
        : BuildHeightfieldFromHeightmap(pipeline.heightmap, simulationResolution, terrainSizeMeters, message);
    if (grid.resolution <= 0)
    {
        return {};
    }
    for (const HeightfieldPipeline::HeightfieldOperation& operation : pipeline.heightfieldOperations)
    {
        ApplyHeightfieldOperation(grid, operation);
    }
    if (message != nullptr && !pipeline.heightfieldOperations.empty())
    {
        *message += std::format(" + {} heightfield op{}", pipeline.heightfieldOperations.size(), pipeline.heightfieldOperations.size() == 1 ? "" : "s");
    }
    SelectHeightfieldPreviewField(grid, previewField);
    if (previewGrid != nullptr)
    {
        *previewGrid = grid;
    }
    return BuildMeshFromHeightfield(grid, resolution);
}
} // namespace

HeightfieldGrid NodeGraph::EvaluateHeightPipelineCached(const HeightfieldPipeline& pipeline, std::string* message, HeightfieldPreviewField previewField, uint64_t* outputHash)
{
    if (outputHash != nullptr) { *outputHash = 0; }

    const GraphId sourceNodeId = pipeline.useShape ? pipeline.shapeNodeId : pipeline.heightmapNodeId;
    if (sourceNodeId == 0)
    {
        return {};
    }

    const int simulationResolution = std::clamp(pipeline.simulationResolution, 2, 2048);
    const float terrainSizeMeters = std::max(1.0f, pipeline.terrainSizeMeters);
    uint64_t inputHash = 0;
    const uint64_t sourceHash = pipeline.useShape
        ? HashShapeSettings(pipeline.shape, simulationResolution, terrainSizeMeters)
        : HashHeightmapSettings(pipeline.heightmap, simulationResolution, terrainSizeMeters);
    HeightfieldNodeCache& sourceCache = heightfieldCache_[sourceNodeId];
    if (!sourceCache.valid ||
        sourceCache.resolution != simulationResolution ||
        sourceCache.inputHash != inputHash ||
        sourceCache.parameterHash != sourceHash)
    {
        g_currentlyEvaluatingNodeId.store(sourceNodeId, std::memory_order_relaxed);
        std::string sourceMessage;
        sourceCache.grid = pipeline.useShape
            ? BuildHeightfieldFromShape(pipeline.shape, simulationResolution, terrainSizeMeters, &sourceMessage)
            : BuildHeightfieldFromHeightmap(pipeline.heightmap, simulationResolution, terrainSizeMeters, &sourceMessage);
        sourceCache.message = sourceMessage;
        sourceCache.valid = true;
        sourceCache.resolution = simulationResolution;
        sourceCache.inputHash = inputHash;
        sourceCache.parameterHash = sourceHash;
        sourceCache.outputHash = sourceHash;
    }

    HeightfieldGrid grid = sourceCache.grid;
    inputHash = sourceCache.outputHash;
    if (message != nullptr)
    {
        *message = sourceCache.message;
    }
    if (grid.resolution <= 0)
    {
        return {};
    }

    for (const HeightfieldPipeline::HeightfieldOperation& operation : pipeline.heightfieldOperations)
    {
        if (operation.nodeId == 0)
        {
            ApplyHeightfieldOperation(grid, operation);
            continue;
        }

        uint64_t parameterHash = HashHeightfieldOperation(operation, simulationResolution);
        MaskGrid inputMask;
        bool hasInputMask = false;
        if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::Crumbling ||
            operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::Rock ||
            operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::Scatter)
        {
            const size_t maskInputIndex = operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::Crumbling ? 1u : 1u;
            if (const Node* operationNode = FindNode(operation.nodeId);
                operationNode != nullptr && operationNode->inputs.size() > maskInputIndex)
            {
                const UpstreamConnection upstream = FindUpstreamConnectionForPin(operationNode->inputs[maskInputIndex].id);
                const auto isMaskProducer = [](NodeKind kind) {
                    return IsMaskOnlyNodeKind(kind) ||
                        kind == NodeKind::MaskCurvature ||
                        kind == NodeKind::MaskSlope ||
                        kind == NodeKind::MaskHeight ||
                        kind == NodeKind::MaskFluvial ||
                        kind == NodeKind::Rock ||
                        kind == NodeKind::Scatter ||
                        kind == NodeKind::Crumbling ||
                        kind == NodeKind::Sediment ||
                        kind == NodeKind::Snow ||
                        kind == NodeKind::MultiScaleErosion;
                };
                if (upstream.node != nullptr && isMaskProducer(upstream.node->kind))
                {
                    uint64_t maskHash = 0;
                    inputMask = EvaluateMaskGridForNodeCached(*upstream.node, 0, &maskHash, upstream.outputPin ? std::string_view(upstream.outputPin->label) : std::string_view{});
                    hasInputMask = inputMask.resolution > 0;
                    HashCombine(parameterHash, maskHash);
                }
            }
        }
        HeightfieldNodeCache& operationCache = heightfieldCache_[operation.nodeId];
        if (!operationCache.valid ||
            operationCache.resolution != simulationResolution ||
            operationCache.inputHash != inputHash ||
            operationCache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(operation.nodeId, std::memory_order_relaxed);
            HeightfieldGrid operationGrid = grid;
            if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::Crumbling)
            {
                ApplyCrumbling(operationGrid, operation.crumbling, hasInputMask ? &inputMask : nullptr);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::Rock)
            {
                ApplyRock(operationGrid, operation.rock, hasInputMask ? &inputMask : nullptr);
            }
            else if (operation.kind == HeightfieldPipeline::HeightfieldOperation::Kind::Scatter)
            {
                ApplyScatter(operationGrid, operation.scatter, hasInputMask ? &inputMask : nullptr);
            }
            else
            {
                ApplyHeightfieldOperation(operationGrid, operation);
            }
            operationCache.grid = std::move(operationGrid);
            operationCache.message.clear();
            operationCache.valid = true;
            operationCache.resolution = simulationResolution;
            operationCache.inputHash = inputHash;
            operationCache.parameterHash = parameterHash;
            operationCache.outputHash = inputHash;
            HashCombine(operationCache.outputHash, parameterHash);
            HashCombine(operationCache.outputHash, static_cast<uint64_t>(operation.nodeId));
        }

        grid = operationCache.grid;
        inputHash = operationCache.outputHash;
    }

    if (message != nullptr && !pipeline.heightfieldOperations.empty())
    {
        *message += std::format(" + {} heightfield op{}", pipeline.heightfieldOperations.size(), pipeline.heightfieldOperations.size() == 1 ? "" : "s");
    }
    SelectHeightfieldPreviewField(grid, previewField);
    if (outputHash != nullptr) { *outputHash = inputHash; }
    return grid;
}

MeshData NodeGraph::BuildMeshFromHeightPipelineCached(const HeightfieldPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField, HeightfieldGrid* previewGrid)
{
    const GraphId sourceNodeId = pipeline.useShape ? pipeline.shapeNodeId : pipeline.heightmapNodeId;
    if (sourceNodeId == 0)
    {
        return BuildMeshFromHeightPipeline(pipeline, resolution, message, previewField, previewGrid);
    }

    uint64_t heightHash = 0;
    HeightfieldGrid grid = EvaluateHeightPipelineCached(pipeline, message, previewField, &heightHash);
    if (grid.resolution <= 0)
    {
        return {};
    }
    if (previewGrid != nullptr)
    {
        *previewGrid = grid;
    }

    uint64_t meshInputHash = heightHash;
    HashCombine(meshInputHash, static_cast<uint64_t>(previewField));
    MeshNodeCache& meshCache = meshCache_[sourceNodeId];
    if (!meshCache.valid ||
        meshCache.resolution != resolution ||
        meshCache.inputHash != meshInputHash ||
        meshCache.previewField != previewField)
    {
        meshCache.mesh = BuildMeshFromHeightfield(grid, resolution);
        meshCache.valid = true;
        meshCache.resolution = resolution;
        meshCache.inputHash = meshInputHash;
        meshCache.previewField = previewField;
    }
    return meshCache.mesh;
}

NodeGraph NodeGraph::CreateDefaultTerrainGraph()
{
    NodeGraph graph;
    graph.Evaluate();
    return graph;
}

const std::vector<Node>& NodeGraph::Nodes() const
{
    return nodes_;
}

const std::vector<Link>& NodeGraph::Links() const
{
    return links_;
}

GraphSettings& NodeGraph::Settings()
{
    return settings_;
}

const GraphSettings& NodeGraph::Settings() const
{
    return settings_;
}

const EvaluationSummary& NodeGraph::Evaluation() const
{
    return evaluation_;
}

const Pin* NodeGraph::FindPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        const auto input = std::ranges::find_if(node.inputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (input != node.inputs.end())
        {
            return &*input;
        }

        const auto output = std::ranges::find_if(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (output != node.outputs.end())
        {
            return &*output;
        }
    }

    return nullptr;
}

const Node* NodeGraph::FindNode(GraphId nodeId) const
{
    const auto it = std::ranges::find_if(nodes_, [nodeId](const Node& node) { return node.id == nodeId; });
    return it == nodes_.end() ? nullptr : &*it;
}

bool NodeGraph::IsInputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Input;
}

bool NodeGraph::IsOutputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Output;
}

bool NodeGraph::PinHasLink(GraphId pinId) const
{
    return std::ranges::any_of(links_, [pinId](const Link& link) {
        return link.startPin == pinId || link.endPin == pinId;
    });
}

bool NodeGraph::CanCreateLink(GraphId startPin, GraphId endPin) const
{
    if (startPin == 0 || endPin == 0 || startPin == endPin)
    {
        return false;
    }

    const Pin* start = FindPin(startPin);
    const Pin* end = FindPin(endPin);
    if (start == nullptr || end == nullptr || start->nodeId == end->nodeId || start->valueType != end->valueType)
    {
        return false;
    }

    return (start->kind == PinKind::Output && end->kind == PinKind::Input) ||
           (start->kind == PinKind::Input && end->kind == PinKind::Output);
}

bool NodeGraph::CreateLink(GraphId startPin, GraphId endPin)
{
    if (!CanCreateLink(startPin, endPin))
    {
        return false;
    }

    if (IsInputPin(startPin))
    {
        std::swap(startPin, endPin);
    }

    std::erase_if(links_, [endPin](const Link& link) {
        return link.endPin == endPin;
    });
    links_.push_back({AllocateGraphId(), startPin, endPin});
    MarkDirty("Link changed");
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId)
{
    const auto oldSize = links_.size();
    std::erase_if(links_, [linkId](const Link& link) { return link.id == linkId; });
    if (links_.size() == oldSize)
    {
        return false;
    }

    MarkDirty("Link deleted");
    return true;
}

bool NodeGraph::DeleteNode(GraphId nodeId)
{
    const Node* node = FindNode(nodeId);
    if (node == nullptr)
    {
        return false;
    }

    std::vector<GraphId> pinIds;
    pinIds.reserve(node->inputs.size() + node->outputs.size());
    for (const Pin& pin : node->inputs)
    {
        pinIds.push_back(pin.id);
    }
    for (const Pin& pin : node->outputs)
    {
        pinIds.push_back(pin.id);
    }

    std::erase_if(links_, [&](const Link& link) {
        return std::ranges::find(pinIds, link.startPin) != pinIds.end() ||
               std::ranges::find(pinIds, link.endPin) != pinIds.end();
    });
    std::erase_if(nodes_, [nodeId](const Node& candidate) {
        return candidate.id == nodeId;
    });
    if (evaluation_.previewNodeId == nodeId)
    {
        evaluation_.previewNodeId = 0;
        evaluation_.previewPinId = 0;
        evaluation_.previewShowsMask = false;
    }
    MarkDirty("Node deleted");
    return true;
}

GraphId NodeGraph::CreateNode(NodeKind kind)
{
    const GraphId nodeId = AddNode(kind, std::string(ToString(kind)));
    switch (kind)
    {
    case NodeKind::HeightmapLoad:
    case NodeKind::Shape:
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    case NodeKind::HeightmapBlur:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        break;
    case NodeKind::MultiScaleErosion:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Flows");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Deposits");
        break;
    case NodeKind::MaskFluvial:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::MaskCurvature:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::MaskSlope:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::MaskHeight:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::Crumbling:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Emission Mask");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Unique Mask");
        break;
    case NodeKind::Rock:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Mask");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Unique Mask");
        break;
    case NodeKind::Scatter:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Mask");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Unique Mask");
        break;
    case NodeKind::Sediment:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Sediment");
        break;
    case NodeKind::Snow:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Snow");
        break;
    case NodeKind::MaskNoise:
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::MaskBlend:
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Foreground");
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Background");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::MaskLevels:
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Mask");
        AddPin(nodeId, PinKind::Output, ValueType::Mask, "Mask");
        break;
    case NodeKind::Colorize:
        AddPin(nodeId, PinKind::Input, ValueType::HeightField, "Heightmap");
        AddPin(nodeId, PinKind::Input, ValueType::ColorTexture, "Base Color");
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Mask");
        AddPin(nodeId, PinKind::Input, ValueType::Mask, "Gradient Mask");
        AddPin(nodeId, PinKind::Output, ValueType::ColorTexture, "Color Texture");
        break;
    default:
        break;
    }
    MarkDirty("Node added");
    return nodeId;
}

void NodeGraph::ReplaceNodes(std::vector<Node> nodes)
{
    nodes_ = std::move(nodes);
    RebuildNextGraphId();
    for (Node& node : nodes_)
    {
        if (node.kind == NodeKind::Colorize)
        {
            const bool hasBaseColorInput = std::ranges::any_of(node.inputs, [](const Pin& pin) {
                return pin.valueType == ValueType::ColorTexture && pin.label == "Base Color";
            });
            if (!hasBaseColorInput)
            {
                Pin baseColorPin{AllocateGraphId(), node.id, PinKind::Input, ValueType::ColorTexture, "Base Color"};
                const auto heightInputIt = std::ranges::find_if(node.inputs, [](const Pin& pin) {
                    return pin.valueType == ValueType::HeightField && pin.label == "Heightmap";
                });
                if (heightInputIt != node.inputs.end())
                {
                    node.inputs.insert(std::next(heightInputIt), std::move(baseColorPin));
                }
                else
                {
                    node.inputs.push_back(std::move(baseColorPin));
                }
            }
        }
        else if (node.kind == NodeKind::MaskBlend)
        {
            if (node.inputs.size() >= 1 && node.inputs[0].label == "A")
            {
                node.inputs[0].label = "Foreground";
            }
            if (node.inputs.size() >= 2 && node.inputs[1].label == "B")
            {
                node.inputs[1].label = "Background";
            }
        }
        else if (node.kind == NodeKind::Rock || node.kind == NodeKind::Scatter)
        {
            const bool hasMaskInput = std::ranges::any_of(node.inputs, [](const Pin& pin) {
                return pin.kind == PinKind::Input && pin.valueType == ValueType::Mask && pin.label == "Mask";
            });
            if (!hasMaskInput)
            {
                Pin maskPin{AllocateGraphId(), node.id, PinKind::Input, ValueType::Mask, "Mask"};
                const auto heightInputIt = std::ranges::find_if(node.inputs, [](const Pin& pin) {
                    return pin.valueType == ValueType::HeightField && pin.label == "Heightmap";
                });
                if (heightInputIt != node.inputs.end())
                {
                    node.inputs.insert(std::next(heightInputIt), std::move(maskPin));
                }
                else
                {
                    node.inputs.push_back(std::move(maskPin));
                }
            }
        }
    }
    MarkDirty("Project nodes loaded");
}

void NodeGraph::ReplaceLinks(std::vector<Link> links)
{
    links_ = std::move(links);
    RebuildNextGraphId();
    MarkDirty("Project links loaded");
}

bool NodeGraph::SetPreviewStage(PreviewStage stage)
{
    if (evaluation_.previewStage == stage)
    {
        return false;
    }

    evaluation_.previewStage = stage;
    evaluation_.dirty = true;
    evaluation_.status = std::format("Preview stage changed to {}", ToString(stage));
    return true;
}

bool NodeGraph::SetPreviewNode(GraphId nodeId)
{
    const Node* node = FindNode(nodeId);
    if (node == nullptr)
    {
        return false;
    }

    const PreviewStage stage = PreviewStageFor(node->kind);
    if (evaluation_.previewNodeId == nodeId && evaluation_.previewStage == stage)
    {
        return false;
    }

    // If the node has no HeightField output but does have a Mask output,
    // selecting the node body should default to the mask view — otherwise
    // the user sees terrain when the node produces a mask, which is
    // confusing. (Pin clicks still override via SetPreviewPin.)
    bool hasHeightOutput = false;
    bool hasMaskOutput = false;
    for (const Pin& pin : node->outputs)
    {
        if (pin.valueType == ValueType::HeightField) hasHeightOutput = true;
        else if (pin.valueType == ValueType::Mask) hasMaskOutput = true;
    }
    const bool defaultToMask = !hasHeightOutput && hasMaskOutput;

    evaluation_.previewNodeId = nodeId;
    evaluation_.previewPinId = 0;
    evaluation_.previewShowsMask = defaultToMask;
    evaluation_.previewField = defaultToMask ? HeightfieldPreviewField::Mask : HeightfieldPreviewField::Heightmap;
    evaluation_.previewStage = stage;
    evaluation_.dirty = true;
    evaluation_.status = std::format("Preview node changed to {}", node->title);
    return true;
}

bool NodeGraph::SetPreviewPin(GraphId pinId)
{
    const Pin* pin = FindPin(pinId);
    if (pin == nullptr || pin->kind != PinKind::Output)
    {
        return false;
    }
    const Node* node = FindNode(pin->nodeId);
    if (node == nullptr)
    {
        return false;
    }

    const bool showsMask = pin->valueType == ValueType::Mask;
    HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap;
    if (showsMask)
    {
        if (pin->label == "Deposits")
        {
            previewField = HeightfieldPreviewField::Deposits;
        }
        else if (pin->label == "Flows")
        {
            previewField = HeightfieldPreviewField::Flows;
        }
        else if (pin->label == "Age")
        {
            previewField = HeightfieldPreviewField::Age;
        }
        else if (pin->label == "Unique Mask")
        {
            previewField = HeightfieldPreviewField::UniqueMask;
        }
        else
        {
            previewField = HeightfieldPreviewField::Mask;
        }
    }
    const PreviewStage stage = PreviewStageFor(node->kind);
    if (evaluation_.previewNodeId == node->id &&
        evaluation_.previewPinId == pinId &&
        evaluation_.previewStage == stage &&
        evaluation_.previewShowsMask == showsMask &&
        evaluation_.previewField == previewField)
    {
        return false;
    }

    const bool pipelineChanged = evaluation_.previewNodeId != node->id ||
                                 evaluation_.previewStage != stage ||
                                 evaluation_.previewField != previewField ||
                                 evaluation_.previewShowsMask != showsMask;
    evaluation_.previewNodeId = node->id;
    evaluation_.previewPinId = pinId;
    evaluation_.previewShowsMask = showsMask;
    evaluation_.previewField = previewField;
    evaluation_.previewStage = stage;
    evaluation_.dirty = evaluation_.dirty || pipelineChanged;
    evaluation_.status = std::format("Preview output changed to {}", pin->label);
    return true;
}

PreviewStage NodeGraph::Preview() const
{
    return evaluation_.previewStage;
}

HeightfieldPipeline NodeGraph::PipelineFor(PreviewStage stage) const
{
    switch (stage)
    {
    case PreviewStage::HeightmapBlur:
        return PipelineTo(NodeKind::HeightmapBlur);
    case PreviewStage::Shape:
        return PipelineTo(NodeKind::Shape);
    case PreviewStage::MultiScaleErosion:
        return PipelineTo(NodeKind::MultiScaleErosion);
    case PreviewStage::MaskCurvature:
        return PipelineTo(NodeKind::MaskCurvature);
    case PreviewStage::MaskSlope:
        return PipelineTo(NodeKind::MaskSlope);
    case PreviewStage::MaskHeight:
        return PipelineTo(NodeKind::MaskHeight);
    case PreviewStage::Crumbling:
        return PipelineTo(NodeKind::Crumbling);
    case PreviewStage::MaskFluvial:
        return PipelineTo(NodeKind::MaskFluvial);
    case PreviewStage::Rock:
        return PipelineTo(NodeKind::Rock);
    case PreviewStage::Scatter:
        return PipelineTo(NodeKind::Scatter);
    case PreviewStage::Sediment:
        return PipelineTo(NodeKind::Sediment);
    case PreviewStage::Snow:
        return PipelineTo(NodeKind::Snow);
    case PreviewStage::Graph:
    default:
        if (const Node* node = FindFirstNode(NodeKind::Snow)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::Sediment)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::Crumbling)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::Scatter)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::Rock)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskHeight)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskSlope)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskCurvature)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskFluvial)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MultiScaleErosion)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::HeightmapBlur)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::Shape)) { return PipelineToNode(*node); }
        return PipelineTo(NodeKind::HeightmapLoad);
    }
}

HeightfieldPipeline NodeGraph::PreviewPipeline() const
{
    if (const Node* previewNode = FindNode(evaluation_.previewNodeId))
    {
        return PipelineToNode(*previewNode);
    }
    return PipelineFor(evaluation_.previewStage);
}

void NodeGraph::MarkDirty(std::string_view reason)
{
    evaluation_.dirty = true;
    evaluation_.status = std::string(reason);
}

void NodeGraph::SetEvaluationPending(std::string_view status)
{
    evaluation_.dirty = true;
    evaluation_.status = std::string(status);
}

void NodeGraph::ApplyEvaluationResultFrom(const NodeGraph& evaluatedGraph)
{
    heightfieldCache_ = evaluatedGraph.heightfieldCache_;
    maskCache_ = evaluatedGraph.maskCache_;
    colorCache_ = evaluatedGraph.colorCache_;
    meshCache_ = evaluatedGraph.meshCache_;
    evaluation_ = evaluatedGraph.evaluation_;
}

const Node* NodeGraph::FindFirstNode(NodeKind kind) const
{
    const auto it = std::ranges::find_if(nodes_, [kind](const Node& node) {
        return node.kind == kind;
    });
    return it != nodes_.end() ? &*it : nullptr;
}

Node* NodeGraph::FindMutableNode(GraphId nodeId)
{
    const auto it = std::ranges::find_if(nodes_, [nodeId](const Node& node) {
        return node.id == nodeId;
    });
    return it != nodes_.end() ? &*it : nullptr;
}

const Node* NodeGraph::FindNodeByOutputPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        if (std::ranges::any_of(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; }))
        {
            return &node;
        }
    }
    return nullptr;
}

const Node* NodeGraph::FindUpstreamNode(const Node& node) const
{
    if (node.inputs.empty())
    {
        return nullptr;
    }

    const GraphId inputPin = node.inputs.front().id;
    const auto linkIt = std::find_if(links_.rbegin(), links_.rend(), [inputPin](const Link& link) {
        return link.endPin == inputPin;
    });
    if (linkIt == links_.rend())
    {
        return nullptr;
    }

    return FindNodeByOutputPin(linkIt->startPin);
}

const Node* NodeGraph::FindUpstreamForPin(GraphId pinId) const
{
    return FindUpstreamConnectionForPin(pinId).node;
}

NodeGraph::UpstreamConnection NodeGraph::FindUpstreamConnectionForPin(GraphId pinId) const
{
    const auto linkIt = std::find_if(links_.rbegin(), links_.rend(), [pinId](const Link& link) {
        return link.endPin == pinId;
    });
    if (linkIt == links_.rend())
    {
        return {};
    }
    return {FindNodeByOutputPin(linkIt->startPin), FindPin(linkIt->startPin)};
}

// Recursive descent through Mask Noise / Mask Blend nodes. Mask Blend follows
// both inputs (FindUpstreamNode only walks the first input), so we use
// FindUpstreamForPin per pin and merge with BlendMaskGrids. Each node's output
// is cached by (input hash, parameter hash) so unrelated edits do not re-run
// upstream noise generation.
MaskGrid NodeGraph::EvaluateMaskGridForNodeCached(const Node& node, int depth, uint64_t* outputHash, std::string_view outputLabel)
{
    if (depth > 16)
    {
        if (outputHash != nullptr) { *outputHash = 0; }
        return {};
    }
    if (node.kind == NodeKind::MaskNoise)
    {
        MaskNoiseSettings settings = node.maskNoise;
        settings.simulationResolution = std::clamp(settings_.preview.simulationResolution, 2, 2048);
        const uint64_t parameterHash = HashMaskNoiseSettings(settings, settings.simulationResolution);
        MaskNodeCache& cache = maskCache_[node.id];
        if (!cache.valid || cache.inputHash != 0 || cache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
            cache.grid = GenerateMaskNoise(settings);
            cache.valid = true;
            cache.inputHash = 0;
            cache.parameterHash = parameterHash;
            cache.outputHash = parameterHash;
            HashCombine(cache.outputHash, static_cast<uint64_t>(node.id));
        }
        if (outputHash != nullptr) { *outputHash = cache.outputHash; }
        return cache.grid;
    }
    if (node.kind == NodeKind::MaskCurvature ||
        node.kind == NodeKind::MaskSlope ||
        node.kind == NodeKind::MaskHeight ||
        node.kind == NodeKind::Crumbling ||
        node.kind == NodeKind::MaskFluvial ||
        node.kind == NodeKind::Rock ||
        node.kind == NodeKind::Scatter ||
        node.kind == NodeKind::Sediment ||
        node.kind == NodeKind::Snow ||
        node.kind == NodeKind::MultiScaleErosion)
    {
        // Heightfield-derived mask nodes read a heightfield, so they can't be
        // evaluated through the mask-graph path on their own. Build the
        // heightfield pipeline up to this node, run it through the heightfield
        // cache, and lift the resulting grid.mask out as a MaskGrid. Caching
        // is fully delegated to the heightfield cache.
        HeightfieldPipeline pipeline = PipelineToNode(node);
        uint64_t hash = 0;
        HeightfieldPreviewField previewField = HeightfieldPreviewField::Mask;
        if (outputLabel == "Deposits")
        {
            previewField = HeightfieldPreviewField::Deposits;
        }
        else if (outputLabel == "Flows")
        {
            previewField = HeightfieldPreviewField::Flows;
        }
        else if (outputLabel == "Age")
        {
            previewField = HeightfieldPreviewField::Age;
        }
        else if (outputLabel == "Unique Mask")
        {
            previewField = HeightfieldPreviewField::UniqueMask;
        }
        HeightfieldGrid grid = EvaluateHeightPipelineCached(pipeline, nullptr, previewField, &hash);
        MaskGrid mask;
        mask.resolution = grid.resolution;
        mask.values = std::move(grid.mask);
        if (!outputLabel.empty())
        {
            HashCombine(hash, std::hash<std::string_view>{}(outputLabel));
        }
        if (outputHash != nullptr) { *outputHash = hash; }
        return mask;
    }
    if (node.kind == NodeKind::MaskBlend)
    {
        // Accept any node that produces a Mask: the two pure mask-graph
        // kinds (Mask Noise / Mask Blend) plus Mask Fluvial, which evaluates
        // through the heightfield cache but is presented here as a MaskGrid.
        const auto isMaskProducer = [](NodeKind kind) {
            return IsMaskOnlyNodeKind(kind) ||
                kind == NodeKind::MaskCurvature ||
                kind == NodeKind::MaskSlope ||
                kind == NodeKind::MaskHeight ||
                kind == NodeKind::Crumbling ||
                kind == NodeKind::MaskFluvial ||
                kind == NodeKind::Rock ||
                kind == NodeKind::Scatter ||
                kind == NodeKind::Sediment ||
                kind == NodeKind::Snow ||
                kind == NodeKind::MultiScaleErosion;
        };
        uint64_t aHash = 0;
        uint64_t bHash = 0;
        MaskGrid a;
        MaskGrid b;
        if (node.inputs.size() >= 1)
        {
            const UpstreamConnection upstream = FindUpstreamConnectionForPin(node.inputs[0].id);
            if (upstream.node != nullptr)
            {
                if (isMaskProducer(upstream.node->kind))
                {
                    a = EvaluateMaskGridForNodeCached(*upstream.node, depth + 1, &aHash, upstream.outputPin ? std::string_view(upstream.outputPin->label) : std::string_view{});
                }
            }
        }
        if (node.inputs.size() >= 2)
        {
            const UpstreamConnection upstream = FindUpstreamConnectionForPin(node.inputs[1].id);
            if (upstream.node != nullptr)
            {
                if (isMaskProducer(upstream.node->kind))
                {
                    b = EvaluateMaskGridForNodeCached(*upstream.node, depth + 1, &bHash, upstream.outputPin ? std::string_view(upstream.outputPin->label) : std::string_view{});
                }
            }
        }
        uint64_t inputHash = 0;
        HashCombine(inputHash, aHash);
        HashCombine(inputHash, bHash);
        const uint64_t parameterHash = HashMaskBlendSettings(node.maskBlend);
        MaskNodeCache& cache = maskCache_[node.id];
        if (!cache.valid || cache.inputHash != inputHash || cache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
            cache.grid = BlendMaskGrids(a, b, node.maskBlend.mode, node.maskBlend.intensity);
            cache.valid = true;
            cache.inputHash = inputHash;
            cache.parameterHash = parameterHash;
            cache.outputHash = inputHash;
            HashCombine(cache.outputHash, parameterHash);
            HashCombine(cache.outputHash, static_cast<uint64_t>(node.id));
        }
        if (outputHash != nullptr) { *outputHash = cache.outputHash; }
        return cache.grid;
    }
    if (node.kind == NodeKind::MaskLevels)
    {
        uint64_t inputHash = 0;
        MaskGrid input;
        if (!node.inputs.empty())
        {
            const auto isMaskProducer = [](NodeKind kind) {
                return IsMaskOnlyNodeKind(kind) ||
                    kind == NodeKind::MaskCurvature ||
                    kind == NodeKind::MaskSlope ||
                    kind == NodeKind::MaskHeight ||
                    kind == NodeKind::Crumbling ||
                    kind == NodeKind::MaskFluvial ||
                    kind == NodeKind::Rock ||
                    kind == NodeKind::Scatter ||
                    kind == NodeKind::Sediment ||
                    kind == NodeKind::Snow ||
                    kind == NodeKind::MultiScaleErosion;
            };
            const UpstreamConnection upstreamConnection = FindUpstreamConnectionForPin(node.inputs[0].id);
            if (upstreamConnection.node != nullptr && isMaskProducer(upstreamConnection.node->kind))
            {
                input = EvaluateMaskGridForNodeCached(*upstreamConnection.node, depth + 1, &inputHash, upstreamConnection.outputPin ? std::string_view(upstreamConnection.outputPin->label) : std::string_view{});
            }
        }

        const uint64_t parameterHash = HashMaskLevelsSettings(node.maskLevels);
        MaskNodeCache& cache = maskCache_[node.id];
        if (!cache.valid || cache.inputHash != inputHash || cache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
            cache.grid = ApplyMaskLevels(input, node.maskLevels);
            cache.valid = true;
            cache.inputHash = inputHash;
            cache.parameterHash = parameterHash;
            cache.outputHash = inputHash;
            HashCombine(cache.outputHash, parameterHash);
            HashCombine(cache.outputHash, static_cast<uint64_t>(node.id));
        }
        if (outputHash != nullptr) { *outputHash = cache.outputHash; }
        return cache.grid;
    }
    if (outputHash != nullptr) { *outputHash = 0; }
    return {};
}

// Gradient Mask と Mask を評価して ColorGrid を生成する。
// Base Color がある場合は Mask を合成強度として上書き合成する。
// Heightmap はここでは評価しない (3D プレビュー用に Evaluate() で別途処理)。
ColorGrid NodeGraph::EvaluateColorGridForNodeCached(const Node& node, int depth, uint64_t* outputHash)
{
    if (depth > 16)
    {
        if (outputHash != nullptr) { *outputHash = 0; }
        return {};
    }
    if (node.kind != NodeKind::Colorize)
    {
        if (outputHash != nullptr) { *outputHash = 0; }
        return {};
    }

    const auto findInput = [&](std::string_view label, ValueType valueType) -> const Pin* {
        const auto it = std::ranges::find_if(node.inputs, [&](const Pin& pin) {
            return pin.valueType == valueType && pin.label == label;
        });
        return it != node.inputs.end() ? &*it : nullptr;
    };

    const Pin* gradientInput = findInput("Gradient Mask", ValueType::Mask);
    const Pin* maskInput = findInput("Mask", ValueType::Mask);
    const Pin* baseInput = findInput("Base Color", ValueType::ColorTexture);

    uint64_t gradientHash = 0;
    MaskGrid gradientMask;
    if (gradientInput != nullptr)
    {
        const UpstreamConnection upstream = FindUpstreamConnectionForPin(gradientInput->id);
        if (upstream.node != nullptr)
        {
            gradientMask = EvaluateMaskGridForNodeCached(*upstream.node, depth + 1, &gradientHash, upstream.outputPin ? std::string_view(upstream.outputPin->label) : std::string_view{});
        }
    }

    uint64_t maskHash = 0;
    MaskGrid maskGrid;
    bool hasMask = false;
    if (maskInput != nullptr)
    {
        const UpstreamConnection upstream = FindUpstreamConnectionForPin(maskInput->id);
        if (upstream.node != nullptr)
        {
            maskGrid = EvaluateMaskGridForNodeCached(*upstream.node, depth + 1, &maskHash, upstream.outputPin ? std::string_view(upstream.outputPin->label) : std::string_view{});
            hasMask = (maskGrid.resolution > 0);
        }
    }

    uint64_t baseHash = 0;
    ColorGrid baseGrid;
    bool hasBaseColor = false;
    if (baseInput != nullptr)
    {
        const Node* upstream = FindUpstreamForPin(baseInput->id);
        if (upstream != nullptr && IsColorOnlyNodeKind(upstream->kind))
        {
            baseGrid = EvaluateColorGridForNodeCached(*upstream, depth + 1, &baseHash);
            hasBaseColor = baseGrid.resolution > 0;
        }
    }

    uint64_t inputHash = 0;
    HashCombine(inputHash, gradientHash);
    HashCombine(inputHash, maskHash);
    HashCombine(inputHash, baseHash);
    const uint64_t parameterHash = HashColorizeSettings(node.colorize);

    ColorNodeCache& cache = colorCache_[node.id];
    if (!cache.valid || cache.inputHash != inputHash || cache.parameterHash != parameterHash)
    {
        g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
        cache.grid = GenerateColorize(node.colorize, gradientMask, hasMask ? &maskGrid : nullptr, hasBaseColor ? &baseGrid : nullptr);
        cache.valid = true;
        cache.inputHash = inputHash;
        cache.parameterHash = parameterHash;
        cache.outputHash = inputHash;
        HashCombine(cache.outputHash, parameterHash);
        HashCombine(cache.outputHash, static_cast<uint64_t>(node.id));
    }
    if (outputHash != nullptr) { *outputHash = cache.outputHash; }
    return cache.grid;
}

HeightfieldGrid NodeGraph::EvaluateMaskAsHeightfield(const Node& node, std::string* message)
{
    const MaskGrid mask = EvaluateMaskGridForNodeCached(node, 0, nullptr);
    HeightfieldGrid grid;
    if (mask.resolution <= 0)
    {
        if (message != nullptr)
        {
            *message = "Mask preview (no input)";
        }
        // Provide a default flat grid so the viewport still has something to draw.
        grid.resolution = 64;
        grid.terrainSizeMeters = std::max(1.0f, settings_.preview.terrainSizeMeters);
        const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
        grid.heights.assign(cellCount, 0.0f);
        grid.mask.assign(cellCount, 0.0f);
        return grid;
    }
    grid.resolution = mask.resolution;
    grid.terrainSizeMeters = std::max(1.0f, settings_.preview.terrainSizeMeters);
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.heights.assign(cellCount, 0.0f);
    grid.mask = mask.values;
    if (message != nullptr)
    {
        *message = std::format("Mask preview {} x {}", grid.resolution, grid.resolution);
    }
    return grid;
}

HeightfieldPipeline NodeGraph::PipelineTo(NodeKind targetKind) const
{
    const Node* node = FindFirstNode(targetKind);
    return node != nullptr ? PipelineToNode(*node) : HeightfieldPipeline{};
}

HeightfieldPipeline NodeGraph::PipelineToNode(const Node& targetNode) const
{
    HeightfieldPipeline pipeline;
    pipeline.simulationResolution = std::clamp(settings_.preview.simulationResolution, 2, 2048);
    pipeline.terrainSizeMeters = std::max(1.0f, settings_.preview.terrainSizeMeters);
    const Node* node = &targetNode;
    int guard = 0;
    while (node != nullptr && guard++ < 16)
    {
        if (IsHeightfieldOperationNode(node->kind))
        {
            pipeline.heightfieldOperations.push_back(MakeHeightfieldOperation(*node));
        }
        else if (node->kind == NodeKind::HeightmapLoad)
        {
            pipeline.hasSource = true;
            pipeline.heightmapNodeId = node->id;
            pipeline.heightmap = node->heightmap;
            break;
        }
        else if (node->kind == NodeKind::Shape)
        {
            pipeline.hasSource = true;
            pipeline.useShape = true;
            pipeline.shapeNodeId = node->id;
            pipeline.shape = node->shape;
            break;
        }

        node = FindUpstreamNode(*node);
    }
    std::ranges::reverse(pipeline.heightfieldOperations);
    return pipeline;
}

const Node* NodeGraph::FindNearestHeightfieldForMaskPreview(const Node& maskNode) const
{
    std::vector<const Node*> pending;
    std::vector<GraphId> visited;
    pending.push_back(&maskNode);
    visited.push_back(maskNode.id);

    for (size_t index = 0; index < pending.size() && index < 64; ++index)
    {
        const Node* current = pending[index];
        if (current == nullptr)
        {
            continue;
        }

        for (const Pin& input : current->inputs)
        {
            const UpstreamConnection upstream = FindUpstreamConnectionForPin(input.id);
            const Node* upstreamNode = upstream.node;
            if (upstreamNode == nullptr)
            {
                continue;
            }

            if (upstreamNode->kind == NodeKind::HeightmapLoad ||
                upstreamNode->kind == NodeKind::Shape ||
                IsHeightfieldOperationNode(upstreamNode->kind))
            {
                const HeightfieldPipeline pipeline = PipelineToNode(*upstreamNode);
                if (pipeline.hasSource)
                {
                    return upstreamNode;
                }
            }

            if (IsMaskOnlyNodeKind(upstreamNode->kind) &&
                std::ranges::find(visited, upstreamNode->id) == visited.end())
            {
                pending.push_back(upstreamNode);
                visited.push_back(upstreamNode->id);
            }
        }
    }

    return nullptr;
}

void NodeGraph::Evaluate(int previewMeshResolution)
{
    if (previewMeshResolution <= 0)
    {
        previewMeshResolution = EffectiveMeshResolution(settings_.preview, 2048);
    }

    // Track which node's kernel is running so the UI can paint a "計算中"
    // badge that walks the upstream chain. Cleared on exit so the badge
    // disappears when no kernel is active. Cache hits don't store —
    // they're instantaneous and the flicker would just be noise.
    struct ProgressGuard
    {
        ~ProgressGuard() { g_currentlyEvaluatingNodeId.store(0, std::memory_order_relaxed); }
    } progressGuard;
    g_currentlyEvaluatingNodeId.store(0, std::memory_order_relaxed);

    // Colorize preview: evaluate color grid and build geometry from Heightmap
    // input (or flat plane if no Heightmap). The 3D viewport samples the color
    // grid as a texture instead of baking it into vertex colors.
    evaluation_.previewIsColor = false;
    evaluation_.previewColorGrid = {};
    const Node* previewNode = FindNode(evaluation_.previewNodeId);
    if (previewNode != nullptr && previewNode->kind == NodeKind::Colorize)
    {
        evaluation_.previewMessage.clear();
        ColorGrid colorGrid = EvaluateColorGridForNodeCached(*previewNode, 0, nullptr);
        evaluation_.previewColorGrid = colorGrid;

        // Geometry: use Heightmap input (inputs[0]) if connected, else flat plane.
        const Node* hmNode = previewNode->inputs.empty()
            ? nullptr
            : FindUpstreamForPin(previewNode->inputs[0].id);
        HeightfieldGrid heightGrid;
        if (hmNode != nullptr)
        {
            HeightfieldPipeline pipeline = PipelineToNode(*hmNode);
            evaluation_.previewMesh = BuildMeshFromHeightPipelineCached(
                pipeline, previewMeshResolution, &evaluation_.previewMessage,
                HeightfieldPreviewField::Heightmap, &heightGrid);
        }
        else
        {
            heightGrid.resolution = 64;
            heightGrid.terrainSizeMeters = std::max(1.0f, settings_.preview.terrainSizeMeters);
            const size_t cellCount = 64 * 64;
            heightGrid.heights.assign(cellCount, 0.0f);
            heightGrid.mask.assign(cellCount, 0.0f);
            evaluation_.previewMesh = BuildFlatMaskMesh(heightGrid, previewMeshResolution);
        }
        evaluation_.previewHeightfield = heightGrid;

        evaluation_.previewIsColor = true;
        evaluation_.previewShowsMask = false;
        evaluation_.previewField = HeightfieldPreviewField::Heightmap;
        ++evaluation_.version;
        evaluation_.dirty = false;
        evaluation_.status = std::format(
            "Colorize preview [{}] -> {} verts / {} tris",
            evaluation_.previewMessage,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size());
        return;
    }

    // Mask-only preview: Mask Noise / Mask Blend live in their own pipeline
    // (no upstream heightfield), so render them on a flat plane with the mask
    // channel populated from the mask graph.
    if (previewNode != nullptr && IsMaskOnlyNodeKind(previewNode->kind))
    {
        evaluation_.previewMessage.clear();
        evaluation_.previewHeightfield = EvaluateMaskAsHeightfield(*previewNode, &evaluation_.previewMessage);
        evaluation_.previewShowsMask = true;
        evaluation_.previewField = HeightfieldPreviewField::Mask;
        if (settings_.preview.maskPreviewUseNearestHeightmap)
        {
            if (const Node* heightNode = FindNearestHeightfieldForMaskPreview(*previewNode))
            {
                HeightfieldGrid terrainGrid;
                std::string terrainMessage;
                const HeightfieldPipeline pipeline = PipelineToNode(*heightNode);
                terrainGrid = EvaluateHeightPipelineCached(pipeline, &terrainMessage, HeightfieldPreviewField::Heightmap, nullptr);
                if (terrainGrid.resolution > 0)
                {
                    MaskGrid maskGrid;
                    maskGrid.resolution = evaluation_.previewHeightfield.resolution;
                    maskGrid.values = evaluation_.previewHeightfield.mask;
                    if (maskGrid.resolution != terrainGrid.resolution)
                    {
                        maskGrid = ResampleMaskGrid(maskGrid, terrainGrid.resolution);
                    }
                    terrainGrid.mask = std::move(maskGrid.values);
                    evaluation_.previewHeightfield = std::move(terrainGrid);
                    evaluation_.previewMesh = BuildMeshFromHeightfield(evaluation_.previewHeightfield, previewMeshResolution);
                    evaluation_.previewMessage = evaluation_.previewMessage.empty()
                        ? std::format("Mask preview on nearest heightmap ({})", terrainMessage)
                        : std::format("{} on nearest heightmap ({})", evaluation_.previewMessage, terrainMessage);
                }
                else
                {
                    evaluation_.previewMesh = evaluation_.previewHeightfield.resolution > 0
                        ? BuildFlatMaskMesh(evaluation_.previewHeightfield, previewMeshResolution)
                        : MeshData{};
                }
            }
            else
            {
                evaluation_.previewMesh = evaluation_.previewHeightfield.resolution > 0
                    ? BuildFlatMaskMesh(evaluation_.previewHeightfield, previewMeshResolution)
                    : MeshData{};
            }
        }
        else
        {
            evaluation_.previewMesh = evaluation_.previewHeightfield.resolution > 0
                ? BuildFlatMaskMesh(evaluation_.previewHeightfield, previewMeshResolution)
                : MeshData{};
        }
        ++evaluation_.version;
        evaluation_.dirty = false;
        evaluation_.status = std::format(
            "Mask preview [{}] -> {} verts / {} tris",
            evaluation_.previewMessage,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size());
        return;
    }

    const HeightfieldPipeline previewPipeline = PreviewPipeline();
    evaluation_.previewShowsMask = evaluation_.previewShowsMask && previewPipeline.hasSource;
    if (!evaluation_.previewShowsMask)
    {
        evaluation_.previewField = HeightfieldPreviewField::Heightmap;
    }
    evaluation_.previewMessage.clear();
    if (!previewPipeline.hasSource)
    {
        evaluation_.previewHeightfield = {};
        evaluation_.previewMesh = {};
        evaluation_.previewMessage = "No source node";
    }
    else
    {
        evaluation_.previewMesh = BuildMeshFromHeightPipelineCached(previewPipeline, previewMeshResolution, &evaluation_.previewMessage, evaluation_.previewField, &evaluation_.previewHeightfield);
    }
    ++evaluation_.version;
    evaluation_.dirty = false;
    if (!previewPipeline.hasSource)
    {
        evaluation_.status = "No source node";
    }
    else
    {
        evaluation_.status = std::format(
            "Heightmap preview [{}] -> {} verts / {} tris{}",
            evaluation_.previewMessage,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size(),
            evaluation_.previewMesh.vertices.empty() ? " / no mesh" : "");
    }
}

GraphId NodeGraph::AddNode(NodeKind kind, std::string title)
{
    const GraphId id = AllocateGraphId();
    nodes_.push_back({id, kind, std::move(title), {}, {}});
    return id;
}

GraphId NodeGraph::AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label)
{
    Node* node = nullptr;
    for (Node& candidate : nodes_)
    {
        if (candidate.id == nodeId)
        {
            node = &candidate;
            break;
        }
    }

    if (node == nullptr)
    {
        return 0;
    }

    const GraphId id = AllocateGraphId();
    Pin pin{id, nodeId, kind, valueType, std::move(label)};
    if (kind == PinKind::Input)
    {
        node->inputs.push_back(std::move(pin));
    }
    else
    {
        node->outputs.push_back(std::move(pin));
    }
    return id;
}

void NodeGraph::AddInitialLink(GraphId startPin, GraphId endPin)
{
    links_.push_back({AllocateGraphId(), startPin, endPin});
}

GraphId NodeGraph::AllocateGraphId()
{
    return nextGraphId_++;
}

void NodeGraph::RebuildNextGraphId()
{
    GraphId maxId = 0;
    for (const Node& node : nodes_)
    {
        maxId = std::max(maxId, node.id);
        for (const Pin& pin : node.inputs)
        {
            maxId = std::max(maxId, pin.id);
        }
        for (const Pin& pin : node.outputs)
        {
            maxId = std::max(maxId, pin.id);
        }
    }
    for (const Link& link : links_)
    {
        maxId = std::max(maxId, link.id);
    }
    nextGraphId_ = maxId + 1;
}

std::string_view ToString(ShapeKind kind)
{
    switch (kind)
    {
    case ShapeKind::Hemisphere:
        return "Hemisphere";
    case ShapeKind::Pyramid:
        return "Pyramid";
    default:
        return "Unknown";
    }
}

std::string_view ToString(MaskBlendMode mode)
{
    switch (mode)
    {
    case MaskBlendMode::Add:
        return "Add";
    case MaskBlendMode::Multiply:
        return "Multiply";
    case MaskBlendMode::Min:
        return "Min";
    case MaskBlendMode::Max:
        return "Max";
    default:
        return "Unknown";
    }
}

std::string_view ToString(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::HeightmapLoad:
        return "Import Heightmap";
    case NodeKind::HeightmapBlur:
        return "Heightmap Blur";
    case NodeKind::Shape:
        return "Shape";
    case NodeKind::MultiScaleErosion:
        return "Multi-Scale Erosion";
    case NodeKind::MaskNoise:
        return "Mask Noise";
    case NodeKind::MaskBlend:
        return "Mask Blend";
    case NodeKind::MaskLevels:
        return "Mask Levels";
    case NodeKind::MaskSlope:
        return "Mask Slope";
    case NodeKind::MaskHeight:
        return "Mask Height";
    case NodeKind::Crumbling:
        return "Crumbling";
    case NodeKind::MaskCurvature:
        return "Mask Curvature";
    case NodeKind::MaskFluvial:
        return "Mask Fluvial";
    case NodeKind::Rock:
        return "Rock";
    case NodeKind::Scatter:
        return "Scatter";
    case NodeKind::Sediment:
        return "Sediment";
    case NodeKind::Snow:
        return "Snow";
    case NodeKind::Colorize:
        return "Colorize";
    default:
        return "Unknown";
    }
}

std::string_view ToString(PreviewStage stage)
{
    switch (stage)
    {
    case PreviewStage::Graph:
        return "Graph";
    case PreviewStage::HeightmapBlur:
        return "Heightmap Blur";
    case PreviewStage::Shape:
        return "Shape";
    case PreviewStage::MultiScaleErosion:
        return "Multi-Scale Erosion";
    case PreviewStage::MaskNoise:
        return "Mask Noise";
    case PreviewStage::MaskBlend:
        return "Mask Blend";
    case PreviewStage::MaskLevels:
        return "Mask Levels";
    case PreviewStage::MaskSlope:
        return "Mask Slope";
    case PreviewStage::MaskHeight:
        return "Mask Height";
    case PreviewStage::Crumbling:
        return "Crumbling";
    case PreviewStage::MaskCurvature:
        return "Mask Curvature";
    case PreviewStage::MaskFluvial:
        return "Mask Fluvial";
    case PreviewStage::Rock:
        return "Rock";
    case PreviewStage::Scatter:
        return "Scatter";
    case PreviewStage::Sediment:
        return "Sediment";
    case PreviewStage::Snow:
        return "Snow";
    case PreviewStage::Colorize:
        return "Colorize";
    default:
        return "Unknown";
    }
}

std::string_view ToString(ValueType type)
{
    switch (type)
    {
    case ValueType::Mesh:
        return "Mesh";
    case ValueType::HeightField:
        return "Heightmap";
    case ValueType::Mask:
        return "Mask";
    case ValueType::ColorTexture:
        return "Color Texture";
    default:
        return "Unknown";
    }
}

PreviewStage PreviewStageFor(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::HeightmapBlur:
        return PreviewStage::HeightmapBlur;
    case NodeKind::MultiScaleErosion:
        return PreviewStage::MultiScaleErosion;
    case NodeKind::HeightmapLoad:
        return PreviewStage::Graph;
    case NodeKind::Shape:
        return PreviewStage::Shape;
    case NodeKind::MaskNoise:
        return PreviewStage::MaskNoise;
    case NodeKind::MaskBlend:
        return PreviewStage::MaskBlend;
    case NodeKind::MaskLevels:
        return PreviewStage::MaskLevels;
    case NodeKind::MaskSlope:
        return PreviewStage::MaskSlope;
    case NodeKind::MaskHeight:
        return PreviewStage::MaskHeight;
    case NodeKind::Crumbling:
        return PreviewStage::Crumbling;
    case NodeKind::MaskCurvature:
        return PreviewStage::MaskCurvature;
    case NodeKind::MaskFluvial:
        return PreviewStage::MaskFluvial;
    case NodeKind::Rock:
        return PreviewStage::Rock;
    case NodeKind::Scatter:
        return PreviewStage::Scatter;
    case NodeKind::Sediment:
        return PreviewStage::Sediment;
    case NodeKind::Snow:
        return PreviewStage::Snow;
    case NodeKind::Colorize:
        return PreviewStage::Colorize;
    default:
        return PreviewStage::Graph;
    }
}

bool IsMaskOnlyNodeKind(NodeKind kind)
{
    return kind == NodeKind::MaskNoise || kind == NodeKind::MaskBlend || kind == NodeKind::MaskLevels;
}

bool IsColorOnlyNodeKind(NodeKind kind)
{
    return kind == NodeKind::Colorize;
}

void SetMultiScaleErosionGpuEvaluator(MultiScaleErosionGpuEvaluator evaluator)
{
    g_mseGpuEvaluator = evaluator;
}

void SetSedimentGpuEvaluator(SedimentGpuEvaluator evaluator)
{
    g_sedimentGpuEvaluator = evaluator;
}

void SetRockGpuEvaluator(RockGpuEvaluator evaluator)
{
    g_rockGpuEvaluator = evaluator;
}

void SetScatterGpuEvaluator(ScatterGpuEvaluator evaluator)
{
    g_scatterGpuEvaluator = evaluator;
}

void SetMaskFluvialGpuEvaluator(MaskFluvialGpuEvaluator evaluator)
{
    g_maskFluvialGpuEvaluator = evaluator;
}

void SetSnowGpuEvaluator(SnowGpuEvaluator evaluator)
{
    g_snowGpuEvaluator = evaluator;
}

std::atomic<GraphId>& CurrentlyEvaluatingNodeId()
{
    return g_currentlyEvaluatingNodeId;
}

} // namespace rock
