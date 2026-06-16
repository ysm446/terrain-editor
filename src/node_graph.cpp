#include "node_graph.h"

#include "evaluation/Colorize.h"
#include "evaluation/DropletErosion.h"
#include "evaluation/FluvialErosion.h"
#include "evaluation/HeightfieldOps.h"
#include "evaluation/HeightmapSource.h"
#include "evaluation/MaskNoise.h"
#include "evaluation/MaskOps.h"
#include "evaluation/MultiScaleErosion.h"
#include "evaluation/Rock.h"
#include "evaluation/Sediment.h"
#include "evaluation/ShapeSource.h"
#include "evaluation/Snow.h"

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
constexpr std::array<PinDefinition, 1> kHeightSourcePins = {{
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
}};

constexpr std::array<PinDefinition, 2> kHeightFilterPins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
}};

constexpr std::array<PinDefinition, 4> kMultiScaleErosionPins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::Mask, "Flows"},
    {PinKind::Output, ValueType::Mask, "Deposits"},
}};

constexpr std::array<PinDefinition, 4> kFluvialErosionPins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::Mask, "Flows"},
    {PinKind::Output, ValueType::Mask, "Deposits"},
}};

constexpr std::array<PinDefinition, 2> kHeightToMaskPins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 2> kMaskPathPins = {{
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 2> kHeightmapFromMaskPins = {{
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
}};

constexpr std::array<PinDefinition, 5> kHeightMaskUniquePins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Unique Mask"},
}};

constexpr std::array<PinDefinition, 5> kCrumblingPins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Input, ValueType::Mask, "Emission Mask"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Unique Mask"},
}};

constexpr std::array<PinDefinition, 3> kSedimentPins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::Mask, "Sediment"},
}};

constexpr std::array<PinDefinition, 3> kSnowPins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::HeightField, "Heightmap"},
    {PinKind::Output, ValueType::Mask, "Snow"},
}};

constexpr std::array<PinDefinition, 1> kMaskSourcePins = {{
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 3> kMaskBlendPins = {{
    {PinKind::Input, ValueType::Mask, "Foreground"},
    {PinKind::Input, ValueType::Mask, "Background"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 2> kMaskFilterPins = {{
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 5> kColorizePins = {{
    {PinKind::Input, ValueType::HeightField, "Heightmap"},
    {PinKind::Input, ValueType::ColorTexture, "Base Color"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Input, ValueType::Mask, "Gradient Mask"},
    {PinKind::Output, ValueType::ColorTexture, "Color Texture"},
}};

constexpr std::array<PinDefinition, 1> kPathPins = {{
    {PinKind::Output, ValueType::Path, "Path"},
}};

constexpr std::array<NodeDefinition, 23> kNodeDefinitions = {{
    {NodeKind::HeightmapLoad, "Import Heightmap", NodeCategory::Heightfield, PreviewStage::Graph, false, false, kHeightSourcePins},
    {NodeKind::HeightmapBlur, "Heightmap Blur", NodeCategory::Heightfield, PreviewStage::HeightmapBlur, false, false, kHeightFilterPins},
    {NodeKind::Shape, "Shape", NodeCategory::Heightfield, PreviewStage::Shape, false, false, kHeightSourcePins},
    {NodeKind::MultiScaleErosion, "Multi-Scale Erosion", NodeCategory::Heightfield, PreviewStage::MultiScaleErosion, false, false, kMultiScaleErosionPins},
    {NodeKind::FluvialErosion, "Fluvial Erosion", NodeCategory::Heightfield, PreviewStage::FluvialErosion, false, false, kFluvialErosionPins},
    {NodeKind::DropletErosion, "Droplet Erosion", NodeCategory::Heightfield, PreviewStage::DropletErosion, false, false, kFluvialErosionPins},
    {NodeKind::MaskNoise, "Mask Noise", NodeCategory::Mask, PreviewStage::MaskNoise, true, false, kMaskSourcePins},
    {NodeKind::MaskBlend, "Mask Blend", NodeCategory::Mask, PreviewStage::MaskBlend, true, false, kMaskBlendPins},
    {NodeKind::MaskFluvial, "Mask Fluvial", NodeCategory::Mask, PreviewStage::MaskFluvial, false, false, kHeightToMaskPins},
    {NodeKind::Rock, "Rock", NodeCategory::Heightfield, PreviewStage::Rock, false, false, kHeightMaskUniquePins},
    {NodeKind::Sediment, "Sediment", NodeCategory::Heightfield, PreviewStage::Sediment, false, false, kSedimentPins},
    {NodeKind::Snow, "Snow", NodeCategory::Heightfield, PreviewStage::Snow, false, false, kSnowPins},
    {NodeKind::Colorize, "Colorize", NodeCategory::Color, PreviewStage::Colorize, false, true, kColorizePins},
    {NodeKind::MaskCurvature, "Mask Curvature", NodeCategory::Mask, PreviewStage::MaskCurvature, false, false, kHeightToMaskPins},
    {NodeKind::MaskLevels, "Mask Levels", NodeCategory::Mask, PreviewStage::MaskLevels, true, false, kMaskFilterPins},
    {NodeKind::MaskSlope, "Mask Slope", NodeCategory::Mask, PreviewStage::MaskSlope, false, false, kHeightToMaskPins},
    {NodeKind::MaskHeight, "Mask Height", NodeCategory::Mask, PreviewStage::MaskHeight, false, false, kHeightToMaskPins},
    {NodeKind::Crumbling, "Crumbling", NodeCategory::Heightfield, PreviewStage::Crumbling, false, false, kCrumblingPins},
    {NodeKind::Scatter, "Scatter", NodeCategory::Heightfield, PreviewStage::Scatter, false, false, kHeightMaskUniquePins},
    {NodeKind::Path, "Path", NodeCategory::Path, PreviewStage::Graph, false, false, kPathPins},
    {NodeKind::MaskPath, "Mask Path", NodeCategory::Mask, PreviewStage::MaskPath, true, false, kMaskPathPins},
    {NodeKind::HeightmapFromMask, "Heightmap From Mask", NodeCategory::Heightfield, PreviewStage::HeightmapFromMask, false, false, kHeightmapFromMaskPins},
    {NodeKind::MaskBlur, "Mask Blur", NodeCategory::Mask, PreviewStage::MaskBlur, true, false, kMaskFilterPins},
}};

ScatterGpuEvaluator g_scatterGpuEvaluator = nullptr;
MaskFluvialGpuEvaluator g_maskFluvialGpuEvaluator = nullptr;
MaskPathGpuEvaluator g_maskPathGpuEvaluator = nullptr;
HeightmapFromMaskGpuEvaluator g_heightmapFromMaskGpuEvaluator = nullptr;
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

uint64_t HashMaskBlurSettings(const MaskBlurSettings& settings, float terrainSizeMeters)
{
    uint64_t hash = 1469598103934665603ull;
    HashCombine(hash, HashFloat(settings.radiusMeters));
    HashCombine(hash, static_cast<uint64_t>(settings.iterations));
    HashCombine(hash, HashFloat(settings.strength));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, HashFloat(terrainSizeMeters));
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

uint64_t HashMaskPathSettings(const MaskPathSettings& settings, int resolution, float terrainSizeMeters)
{
    uint64_t hash = 1469598103934665603ull;
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, static_cast<uint64_t>(settings.invert ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    HashCombine(hash, HashFloat(terrainSizeMeters));
    return hash;
}

uint64_t HashHeightmapFromMaskSettings(const HeightmapFromMaskSettings& settings, int resolution, float terrainSizeMeters)
{
    uint64_t hash = 1469598103934665603ull;
    HashCombine(hash, HashFloat(settings.heightMeters));
    HashCombine(hash, HashFloat(settings.baseHeightMeters));
    HashCombine(hash, HashFloat(settings.gamma));
    HashCombine(hash, static_cast<uint64_t>(settings.invert ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.backend));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    HashCombine(hash, HashFloat(terrainSizeMeters));
    return hash;
}

uint64_t HashPathSettings(const PathSettings& path)
{
    uint64_t hash = 1099511628211ull;
    HashCombine(hash, HashFloat(path.defaultWidthMeters));
    HashCombine(hash, HashFloat(path.defaultFeatherMeters));
    HashCombine(hash, HashFloat(path.defaultHeightOffset));
    HashCombine(hash, static_cast<uint64_t>(path.defaultHeightMode));
    HashCombine(hash, static_cast<uint64_t>(path.defaultSegmentType));
    for (const PathPoint& point : path.points)
    {
        HashCombine(hash, static_cast<uint64_t>(point.id));
        HashCombine(hash, HashFloat(point.x));
        HashCombine(hash, HashFloat(point.z));
        HashCombine(hash, HashFloat(point.height));
        HashCombine(hash, HashFloat(point.heightOffset));
        HashCombine(hash, HashFloat(point.widthMeters));
        HashCombine(hash, HashFloat(point.featherMeters));
        HashCombine(hash, HashFloat(point.intensity));
        HashCombine(hash, static_cast<uint64_t>(point.heightMode));
    }
    for (const PathEdge& edge : path.edges)
    {
        HashCombine(hash, static_cast<uint64_t>(edge.id));
        HashCombine(hash, static_cast<uint64_t>(edge.fromPoint));
        HashCombine(hash, static_cast<uint64_t>(edge.toPoint));
        HashCombine(hash, static_cast<uint64_t>(edge.enabled ? 1 : 0));
        HashCombine(hash, static_cast<uint64_t>(edge.segmentType));
    }
    return hash;
}

float MaskPathDistanceValue(float distance, float widthMeters, float featherMeters)
{
    const float halfWidth = std::max(0.0f, widthMeters) * 0.5f;
    const float feather = std::max(0.0f, featherMeters);
    if (distance <= halfWidth)
    {
        return 1.0f;
    }
    if (feather <= 0.0001f || distance >= halfWidth + feather)
    {
        return 0.0f;
    }
    const float t = std::clamp((distance - halfWidth) / feather, 0.0f, 1.0f);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return 1.0f - smooth;
}

float PathPointMaskWidth(const PathPoint& point)
{
    return std::clamp(point.widthMeters, 0.01f, 100000.0f);
}

float PathPointMaskFeather(const PathPoint& point)
{
    return std::clamp(point.featherMeters, 0.0f, 100000.0f);
}

float PathPointMaskIntensity(const PathPoint& point)
{
    return std::clamp(point.intensity, 0.0f, 1.0f);
}

const PathPoint* FindPathPoint(const PathSettings& path, GraphId pointId)
{
    const auto it = std::ranges::find_if(path.points, [pointId](const PathPoint& point) {
        return point.id == pointId;
    });
    return it != path.points.end() ? &*it : nullptr;
}

const PathPoint* FindConnectedPathPoint(const PathSettings& path, GraphId pointId, GraphId excludePointId)
{
    for (const PathEdge& edge : path.edges)
    {
        if (!edge.enabled)
        {
            continue;
        }
        if (edge.fromPoint == pointId && edge.toPoint != excludePointId)
        {
            return FindPathPoint(path, edge.toPoint);
        }
        if (edge.toPoint == pointId && edge.fromPoint != excludePointId)
        {
            return FindPathPoint(path, edge.fromPoint);
        }
    }
    return nullptr;
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
    HashCombine(hash, HashFloat(settings.spread));
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
    HashCombine(hash, static_cast<uint64_t>(settings.iterationCount));
    HashCombine(hash, HashFloat(settings.emissionTime));
    HashCombine(hash, static_cast<uint64_t>(settings.smoothingIterations));
    HashCombine(hash, HashFloat(settings.motionSlopeLimitDeg));
    HashCombine(hash, HashFloat(settings.transportRate));
    HashCombine(hash, HashFloat(settings.surfaceSmoothing));
    HashCombine(hash, HashFloat(settings.maskThresholdM));
    HashCombine(hash, HashFloat(settings.maskFeatherM));
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

uint64_t HashDropletErosionSettings(const DropletErosionSettings& settings, int resolution)
{
    uint64_t hash = 11400714819323198485ull;
    HashCombine(hash, HashFloat(settings.dropletDensity));
    HashCombine(hash, HashFloat(settings.maxTravelDistance));
    HashCombine(hash, HashFloat(settings.erosionStrength));
    HashCombine(hash, HashFloat(settings.depositionStrength));
    HashCombine(hash, HashFloat(settings.inertia));
    HashCombine(hash, HashFloat(settings.minSlope));
    HashCombine(hash, static_cast<uint64_t>(settings.useMultigrid ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
    HashCombine(hash, HashFloat(settings.sedimentCapacity));
    HashCombine(hash, HashFloat(settings.evaporation));
    HashCombine(hash, HashFloat(settings.gravity));
    HashCombine(hash, HashFloat(settings.erosionRadiusMeters));
    HashCombine(hash, static_cast<uint64_t>(resolution));
    return hash;
}

uint64_t HashFluvialErosionSettings(const FluvialErosionSettings& settings, int resolution)
{
    uint64_t hash = 11400714819323198485ull;
    HashCombine(hash, HashFloat(settings.featureSize));
    HashCombine(hash, HashFloat(settings.geologicalAge));
    HashCombine(hash, static_cast<uint64_t>(settings.simulationIterations));
    HashCombine(hash, HashFloat(settings.channelLength));
    HashCombine(hash, HashFloat(settings.erosionStrength));
    HashCombine(hash, HashFloat(settings.channeling));
    HashCombine(hash, HashFloat(settings.friction));
    HashCombine(hash, HashFloat(settings.wearAngleDeg));
    HashCombine(hash, HashFloat(settings.depositAngleDeg));
    HashCombine(hash, HashFloat(settings.maxErosionAngleDeg));
    HashCombine(hash, HashFloat(settings.erosionGranularity));
    HashCombine(hash, HashFloat(settings.flowVolume));
    HashCombine(hash, HashFloat(settings.smallChannelInfluence));
    HashCombine(hash, HashFloat(settings.sedimentVelocity));
    HashCombine(hash, static_cast<uint64_t>(settings.useMultigrid ? 1 : 0));
    HashCombine(hash, static_cast<uint64_t>(settings.seed));
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

// Run `fn(z)` for each row z in [0, n). Each iteration must be independent
// (writing only to its own row). Used by the remaining row-parallel evaluators.
template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
}

MaskGrid GenerateMaskPath(const PathSettings& path, const MaskPathSettings& settings, int resolution, float terrainSizeMeters)
{
    struct PointSample
    {
        float x = 0.0f;
        float z = 0.0f;
        float width = 0.0f;
        float feather = 0.0f;
        float intensity = 1.0f;
    };

    struct SegmentSample
    {
        float ax = 0.0f;
        float az = 0.0f;
        float abX = 0.0f;
        float abZ = 0.0f;
        float lenSq = 0.0f;
        float widthA = 0.0f;
        float widthB = 0.0f;
        float featherA = 0.0f;
        float featherB = 0.0f;
        float intensityA = 1.0f;
        float intensityB = 1.0f;
    };

    const auto addLinearSegment = [](std::vector<SegmentSample>& samples,
                                     float ax,
                                     float az,
                                     float bx,
                                     float bz,
                                     float widthA,
                                     float widthB,
                                     float featherA,
                                     float featherB,
                                     float intensityA,
                                     float intensityB)
    {
        const float abX = bx - ax;
        const float abZ = bz - az;
        const float lenSq = abX * abX + abZ * abZ;
        if (lenSq <= 0.000001f)
        {
            return;
        }
        samples.push_back({ax, az, abX, abZ, lenSq, widthA, widthB, featherA, featherB, intensityA, intensityB});
    };

    const auto catmull = [](float p0, float p1, float p2, float p3, float t)
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    };

    MaskGrid grid;
    grid.resolution = std::clamp(resolution, 2, 2048);
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.values.assign(cellCount, 0.0f);

    if (settings.backend == MaskUtilityBackend::GpuCompute && g_maskPathGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_maskPathGpuEvaluator(grid, path, settings, terrainSizeMeters, &ignoredError))
        {
            return grid;
        }
        grid.values.assign(cellCount, 0.0f);
    }

    std::vector<PointSample> pointSamples;
    pointSamples.reserve(path.points.size());
    for (const PathPoint& point : path.points)
    {
        pointSamples.push_back({point.x, point.z, PathPointMaskWidth(point), PathPointMaskFeather(point), PathPointMaskIntensity(point)});
    }

    std::vector<SegmentSample> segmentSamples;
    segmentSamples.reserve(path.edges.size());
    for (const PathEdge& edge : path.edges)
    {
        if (!edge.enabled)
        {
            continue;
        }
        const PathPoint* a = FindPathPoint(path, edge.fromPoint);
        const PathPoint* b = FindPathPoint(path, edge.toPoint);
        if (a == nullptr || b == nullptr)
        {
            continue;
        }
        if (edge.segmentType == PathSegmentType::CatmullRom)
        {
            const PathPoint* p0 = FindConnectedPathPoint(path, a->id, b->id);
            const PathPoint* p3 = FindConnectedPathPoint(path, b->id, a->id);
            if (p0 == nullptr)
            {
                p0 = a;
            }
            if (p3 == nullptr)
            {
                p3 = b;
            }

            constexpr int kSmoothSteps = 16;
            float prevX = a->x;
            float prevZ = a->z;
            for (int step = 1; step <= kSmoothSteps; ++step)
            {
                const float t0 = static_cast<float>(step - 1) / static_cast<float>(kSmoothSteps);
                const float t1 = static_cast<float>(step) / static_cast<float>(kSmoothSteps);
                const float nextX = catmull(p0->x, a->x, b->x, p3->x, t1);
                const float nextZ = catmull(p0->z, a->z, b->z, p3->z, t1);
                addLinearSegment(segmentSamples, prevX, prevZ, nextX, nextZ,
                    std::lerp(PathPointMaskWidth(*a), PathPointMaskWidth(*b), t0),
                    std::lerp(PathPointMaskWidth(*a), PathPointMaskWidth(*b), t1),
                    std::lerp(PathPointMaskFeather(*a), PathPointMaskFeather(*b), t0),
                    std::lerp(PathPointMaskFeather(*a), PathPointMaskFeather(*b), t1),
                    std::lerp(PathPointMaskIntensity(*a), PathPointMaskIntensity(*b), t0),
                    std::lerp(PathPointMaskIntensity(*a), PathPointMaskIntensity(*b), t1));
                prevX = nextX;
                prevZ = nextZ;
            }
            continue;
        }

        addLinearSegment(segmentSamples,
            a->x,
            a->z,
            b->x,
            b->z,
            PathPointMaskWidth(*a),
            PathPointMaskWidth(*b),
            PathPointMaskFeather(*a),
            PathPointMaskFeather(*b),
            PathPointMaskIntensity(*a),
            PathPointMaskIntensity(*b));
    }

    const float terrainSize = std::max(1.0f, terrainSizeMeters);
    const float halfSize = terrainSize * 0.5f;
    const float invStep = 1.0f / static_cast<float>(std::max(1, grid.resolution - 1));
    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    ParallelForRows(grid.resolution, [&](int z)
    {
        const float worldZ = halfSize - static_cast<float>(z) * invStep * terrainSize;
        for (int x = 0; x < grid.resolution; ++x)
        {
            const float worldX = -halfSize + static_cast<float>(x) * invStep * terrainSize;
            float value = 0.0f;

            for (const PointSample& point : pointSamples)
            {
                const float dx = worldX - point.x;
                const float dz = worldZ - point.z;
                const float distance = std::sqrt(dx * dx + dz * dz);
                value = std::max(value, MaskPathDistanceValue(distance, point.width, point.feather) * point.intensity);
            }

            for (const SegmentSample& segment : segmentSamples)
            {
                const float t = std::clamp(((worldX - segment.ax) * segment.abX + (worldZ - segment.az) * segment.abZ) / segment.lenSq, 0.0f, 1.0f);
                const float closestX = segment.ax + segment.abX * t;
                const float closestZ = segment.az + segment.abZ * t;
                const float dx = worldX - closestX;
                const float dz = worldZ - closestZ;
                const float distance = std::sqrt(dx * dx + dz * dz);
                const float width = std::lerp(segment.widthA, segment.widthB, t);
                const float feather = std::lerp(segment.featherA, segment.featherB, t);
                const float intensity = std::lerp(segment.intensityA, segment.intensityB, t);
                value = std::max(value, MaskPathDistanceValue(distance, width, feather) * intensity);
            }

            value = std::pow(std::clamp(value, 0.0f, 1.0f), gamma);
            if (settings.invert)
            {
                value = 1.0f - value;
            }
            grid.values[static_cast<size_t>(z) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x)] = value;
        }
    });
    return grid;
}

float SampleGridBilinear(const std::vector<float>& values, int n, float x, float z);

HeightfieldGrid GenerateHeightmapFromMask(const MaskGrid& mask, const HeightmapFromMaskSettings& settings, int resolution, float terrainSizeMeters)
{
    HeightfieldGrid grid;
    grid.resolution = std::clamp(resolution, 2, 2048);
    grid.terrainSizeMeters = std::max(1.0f, terrainSizeMeters);
    const size_t cellCount = static_cast<size_t>(grid.resolution) * static_cast<size_t>(grid.resolution);
    grid.heights.assign(cellCount, settings.baseHeightMeters);
    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);
    grid.deposits.assign(cellCount, 0.0f);
    grid.flows.assign(cellCount, 0.0f);
    grid.age.assign(cellCount, 0.0f);

    const size_t requiredSourceCells = static_cast<size_t>(mask.resolution) * static_cast<size_t>(mask.resolution);
    if (mask.resolution <= 0 || mask.values.size() < requiredSourceCells)
    {
        return grid;
    }

    if (settings.backend == MaskUtilityBackend::GpuCompute && g_heightmapFromMaskGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_heightmapFromMaskGpuEvaluator(grid, mask, settings, resolution, terrainSizeMeters, &ignoredError))
        {
            return grid;
        }
        grid.heights.assign(cellCount, settings.baseHeightMeters);
        grid.mask.assign(cellCount, 0.0f);
    }

    const int sourceResolution = mask.resolution;
    const float gamma = std::clamp(settings.gamma, 0.05f, 8.0f);
    const float invTarget = 1.0f / static_cast<float>(std::max(1, grid.resolution - 1));
    const float sourceMax = static_cast<float>(std::max(0, sourceResolution - 1));
    ParallelForRows(grid.resolution, [&](int z)
    {
        const float sourceZ = static_cast<float>(z) * invTarget * sourceMax;
        for (int x = 0; x < grid.resolution; ++x)
        {
            const float sourceX = static_cast<float>(x) * invTarget * sourceMax;
            float value = SampleGridBilinear(mask.values, sourceResolution, sourceX, sourceZ);
            value = std::clamp(value, 0.0f, 1.0f);
            if (settings.invert)
            {
                value = 1.0f - value;
            }
            value = std::pow(value, gamma);
            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(grid.resolution) + static_cast<size_t>(x);
            grid.mask[idx] = value;
            grid.heights[idx] = settings.baseHeightMeters + value * settings.heightMeters;
        }
    });
    return grid;
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

    const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
    const int threadCount = std::clamp(static_cast<int>(hardwareThreads), 1, std::min(particleCount, 8));
    std::vector<std::vector<float>> threadHits(static_cast<size_t>(threadCount), std::vector<float>(cellCount, 0.0f));

    const auto particleSeed = [baseSeed = static_cast<uint32_t>(settings.particleSeed)](int particle) {
        uint32_t h = static_cast<uint32_t>(particle) * 0x9e3779b9u + baseSeed * 0x85ebca6bu + 0x27d4eb2du;
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        return h;
    };

    auto simulateRange = [&](int threadIndex, int firstParticle, int lastParticle) {
        std::vector<float>& localHits = threadHits[static_cast<size_t>(threadIndex)];
        std::uniform_real_distribution<float> spawnDist(1.0f, static_cast<float>(std::max(1, n - 2)));
        std::uniform_real_distribution<float> unitDist(0.0f, 1.0f);

        for (int particle = firstParticle; particle < lastParticle; ++particle)
        {
            std::mt19937 rng(particleSeed(particle));
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
                SplatGridBilinear(localHits, n, x, z, ageWeight);
                x += vx * stepCells;
                z += vz * stepCells;
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(threadCount));
    for (int threadIndex = 0; threadIndex < threadCount; ++threadIndex)
    {
        const int firstParticle = (particleCount * threadIndex) / threadCount;
        const int lastParticle = (particleCount * (threadIndex + 1)) / threadCount;
        workers.emplace_back(simulateRange, threadIndex, firstParticle, lastParticle);
    }
    for (std::thread& worker : workers)
    {
        worker.join();
    }

    ParallelForRows(n, [&](int z) {
        const size_t rowBase = static_cast<size_t>(z) * static_cast<size_t>(n);
        for (int x = 0; x < n; ++x)
        {
            const size_t idx = rowBase + static_cast<size_t>(x);
            float value = 0.0f;
            for (const std::vector<float>& localHits : threadHits)
            {
                value += localHits[idx];
            }
            hits[idx] = value;
        }
    });

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
    const float spread = std::clamp(settings.spread, 0.0f, 1.0f);
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

            const float gravityWander = (hash01(state) - 0.5f) * (1.0f - gravity) * 0.75f;
            const float spreadWander = spread > 0.0f
                ? (hash01(state) - 0.5f) * spread * (0.45f + 0.65f * (1.0f - gravity))
                : 0.0f;
            const float wander = gravityWander + spreadWander;
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

            const float sideStep = spread > 0.0f ? (hash01(state) - 0.5f) * spread * stepCells * 0.45f : 0.0f;
            const float nextX = x + dirX * stepCells - dirZ * sideStep;
            const float nextZ = z + dirZ * stepCells + dirX * sideStep;
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

    if (spread > 0.0f && particles.size() > 1)
    {
        const int separationPasses = 1 + static_cast<int>(std::round(spread * 3.0f));
        const float binSize = std::max(1.0f, maxSizeCells);
        const int binsPerAxis = std::max(1, static_cast<int>(std::ceil(static_cast<float>(n) / binSize)));
        const auto binIndex = [binsPerAxis](int bx, int bz) {
            return bz * binsPerAxis + bx;
        };
        std::vector<std::vector<int>> bins(static_cast<size_t>(binsPerAxis) * static_cast<size_t>(binsPerAxis));
        for (int pass = 0; pass < separationPasses; ++pass)
        {
            for (std::vector<int>& bin : bins)
            {
                bin.clear();
            }
            for (int i = 0; i < static_cast<int>(particles.size()); ++i)
            {
                const int bx = std::clamp(static_cast<int>(particles[static_cast<size_t>(i)].x / binSize), 0, binsPerAxis - 1);
                const int bz = std::clamp(static_cast<int>(particles[static_cast<size_t>(i)].z / binSize), 0, binsPerAxis - 1);
                bins[static_cast<size_t>(binIndex(bx, bz))].push_back(i);
            }
            for (int i = 0; i < static_cast<int>(particles.size()); ++i)
            {
                CrumblingParticle& a = particles[static_cast<size_t>(i)];
                const int bx = std::clamp(static_cast<int>(a.x / binSize), 0, binsPerAxis - 1);
                const int bz = std::clamp(static_cast<int>(a.z / binSize), 0, binsPerAxis - 1);
                for (int dz = -1; dz <= 1; ++dz)
                {
                    const int nbz = bz + dz;
                    if (nbz < 0 || nbz >= binsPerAxis)
                    {
                        continue;
                    }
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nbx = bx + dx;
                        if (nbx < 0 || nbx >= binsPerAxis)
                        {
                            continue;
                        }
                        const std::vector<int>& bin = bins[static_cast<size_t>(binIndex(nbx, nbz))];
                        for (const int j : bin)
                        {
                            if (j <= i)
                            {
                                continue;
                            }
                            CrumblingParticle& b = particles[static_cast<size_t>(j)];
                            float vx = b.x - a.x;
                            float vz = b.z - a.z;
                            float distSq = vx * vx + vz * vz;
                            if (distSq < 1e-6f)
                            {
                                const float angle = (a.unique - b.unique) * 2.0f * kPi;
                                vx = std::cos(angle);
                                vz = std::sin(angle);
                                distSq = 1.0f;
                            }
                            const float aRadius = std::max(0.5f, a.sizeCells * 0.5f);
                            const float bRadius = std::max(0.5f, b.sizeCells * 0.5f);
                            const float minDist = (aRadius + bRadius) * (0.35f + spread * 0.45f);
                            if (distSq >= minDist * minDist)
                            {
                                continue;
                            }
                            const float dist = std::sqrt(distSq);
                            const float push = (minDist - dist) * (0.18f + spread * 0.32f);
                            const float nx = vx / dist;
                            const float nz = vz / dist;
                            a.x = std::clamp(a.x - nx * push, 0.0f, static_cast<float>(n - 1));
                            a.z = std::clamp(a.z - nz * push, 0.0f, static_cast<float>(n - 1));
                            b.x = std::clamp(b.x + nx * push, 0.0f, static_cast<float>(n - 1));
                            b.z = std::clamp(b.z + nz * push, 0.0f, static_cast<float>(n - 1));
                        }
                    }
                }
            }
        }
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
    case HeightfieldPipeline::HeightfieldOperation::Kind::FluvialErosion:
        ApplyFluvialErosion(grid, operation.fluvialErosion);
        break;
    case HeightfieldPipeline::HeightfieldOperation::Kind::DropletErosion:
        ApplyDropletErosion(grid, operation.dropletErosion);
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
    case HeightfieldPipeline::HeightfieldOperation::Kind::FluvialErosion:
        return HashFluvialErosionSettings(operation.fluvialErosion, resolution);
    case HeightfieldPipeline::HeightfieldOperation::Kind::DropletErosion:
        return HashDropletErosionSettings(operation.dropletErosion, resolution);
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
    case NodeKind::FluvialErosion:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::FluvialErosion;
        operation.fluvialErosion = node.fluvialErosion;
        break;
    case NodeKind::DropletErosion:
        operation.kind = HeightfieldPipeline::HeightfieldOperation::Kind::DropletErosion;
        operation.dropletErosion = node.dropletErosion;
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
    case NodeKind::FluvialErosion:
    case NodeKind::DropletErosion:
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

    const GraphId sourceNodeId = pipeline.useMaskSource
        ? pipeline.maskSourceNodeId
        : (pipeline.useShape ? pipeline.shapeNodeId : pipeline.heightmapNodeId);
    if (sourceNodeId == 0)
    {
        return {};
    }

    const int simulationResolution = std::clamp(pipeline.simulationResolution, 2, 2048);
    const float terrainSizeMeters = std::max(1.0f, pipeline.terrainSizeMeters);
    uint64_t inputHash = 0;
    MaskGrid sourceMask;
    if (pipeline.useMaskSource)
    {
        if (const Node* sourceNode = FindNode(sourceNodeId);
            sourceNode != nullptr && !sourceNode->inputs.empty())
        {
            const UpstreamConnection upstream = FindUpstreamConnectionForPin(sourceNode->inputs[0].id);
            if (upstream.node != nullptr)
            {
                sourceMask = EvaluateMaskGridForNodeCached(*upstream.node, 0, &inputHash, upstream.outputPin ? std::string_view(upstream.outputPin->label) : std::string_view{});
            }
        }
    }
    const uint64_t sourceHash = pipeline.useMaskSource
        ? HashHeightmapFromMaskSettings(pipeline.heightmapFromMask, simulationResolution, terrainSizeMeters)
        : (pipeline.useShape
            ? HashShapeSettings(pipeline.shape, simulationResolution, terrainSizeMeters)
            : HashHeightmapSettings(pipeline.heightmap, simulationResolution, terrainSizeMeters));
    HeightfieldNodeCache& sourceCache = heightfieldCache_[sourceNodeId];
    if (!sourceCache.valid ||
        sourceCache.resolution != simulationResolution ||
        sourceCache.inputHash != inputHash ||
        sourceCache.parameterHash != sourceHash)
    {
        g_currentlyEvaluatingNodeId.store(sourceNodeId, std::memory_order_relaxed);
        std::string sourceMessage;
        if (pipeline.useMaskSource)
        {
            sourceCache.grid = GenerateHeightmapFromMask(sourceMask, pipeline.heightmapFromMask, simulationResolution, terrainSizeMeters);
            sourceMessage = "Heightmap From Mask";
        }
        else
        {
            sourceCache.grid = pipeline.useShape
                ? BuildHeightfieldFromShape(pipeline.shape, simulationResolution, terrainSizeMeters, &sourceMessage)
                : BuildHeightfieldFromHeightmap(pipeline.heightmap, simulationResolution, terrainSizeMeters, &sourceMessage);
        }
        sourceCache.message = sourceMessage;
        sourceCache.valid = true;
        sourceCache.resolution = simulationResolution;
        sourceCache.inputHash = inputHash;
        sourceCache.parameterHash = sourceHash;
        sourceCache.outputHash = inputHash;
        HashCombine(sourceCache.outputHash, sourceHash);
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
                        kind == NodeKind::MultiScaleErosion ||
                        kind == NodeKind::FluvialErosion ||
                        kind == NodeKind::DropletErosion;
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
    const GraphId sourceNodeId = pipeline.useMaskSource
        ? pipeline.maskSourceNodeId
        : (pipeline.useShape ? pipeline.shapeNodeId : pipeline.heightmapNodeId);
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
    if (const NodeDefinition* definition = FindNodeDefinition(kind))
    {
        for (const PinDefinition& pin : definition->pins)
        {
            AddPin(nodeId, pin.kind, pin.valueType, std::string(pin.label));
        }
    }
    MarkDirty("Node added");
    return nodeId;
}

GraphId NodeGraph::AllocatePathElementId()
{
    return AllocateGraphId();
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
    // confusing.
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
    case PreviewStage::FluvialErosion:
        return PipelineTo(NodeKind::FluvialErosion);
    case PreviewStage::DropletErosion:
        return PipelineTo(NodeKind::DropletErosion);
    case PreviewStage::MaskCurvature:
        return PipelineTo(NodeKind::MaskCurvature);
    case PreviewStage::MaskSlope:
        return PipelineTo(NodeKind::MaskSlope);
    case PreviewStage::MaskHeight:
        return PipelineTo(NodeKind::MaskHeight);
    case PreviewStage::HeightmapFromMask:
        return PipelineTo(NodeKind::HeightmapFromMask);
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
        if (const Node* node = FindFirstNode(NodeKind::HeightmapFromMask)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskHeight)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskSlope)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskCurvature)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::MaskFluvial)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::DropletErosion)) { return PipelineToNode(*node); }
        if (const Node* node = FindFirstNode(NodeKind::FluvialErosion)) { return PipelineToNode(*node); }
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
        node.kind == NodeKind::MultiScaleErosion ||
        node.kind == NodeKind::FluvialErosion ||
        node.kind == NodeKind::DropletErosion)
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
    if (node.kind == NodeKind::MaskPath)
    {
        uint64_t inputHash = 0;
        const Node* pathNode = nullptr;
        if (!node.inputs.empty())
        {
            const UpstreamConnection upstream = FindUpstreamConnectionForPin(node.inputs[0].id);
            if (upstream.node != nullptr && upstream.node->kind == NodeKind::Path)
            {
                pathNode = upstream.node;
                inputHash = HashPathSettings(upstream.node->path);
                HashCombine(inputHash, static_cast<uint64_t>(upstream.node->id));
            }
        }

        const int resolution = std::clamp(settings_.preview.simulationResolution, 2, 2048);
        const float terrainSize = std::max(1.0f, settings_.preview.terrainSizeMeters);
        const uint64_t parameterHash = HashMaskPathSettings(node.maskPath, resolution, terrainSize);
        MaskNodeCache& cache = maskCache_[node.id];
        if (!cache.valid || cache.inputHash != inputHash || cache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
            cache.grid = pathNode != nullptr
                ? GenerateMaskPath(pathNode->path, node.maskPath, resolution, terrainSize)
                : MaskGrid{};
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
                kind == NodeKind::MultiScaleErosion ||
                kind == NodeKind::FluvialErosion ||
                kind == NodeKind::DropletErosion;
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
                    kind == NodeKind::MultiScaleErosion ||
                    kind == NodeKind::FluvialErosion ||
                    kind == NodeKind::DropletErosion;
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
    if (node.kind == NodeKind::MaskBlur)
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
                    kind == NodeKind::MultiScaleErosion ||
                    kind == NodeKind::FluvialErosion ||
                    kind == NodeKind::DropletErosion;
            };
            const UpstreamConnection upstreamConnection = FindUpstreamConnectionForPin(node.inputs[0].id);
            if (upstreamConnection.node != nullptr && isMaskProducer(upstreamConnection.node->kind))
            {
                input = EvaluateMaskGridForNodeCached(*upstreamConnection.node, depth + 1, &inputHash, upstreamConnection.outputPin ? std::string_view(upstreamConnection.outputPin->label) : std::string_view{});
            }
        }

        const float terrainSize = std::max(1.0f, settings_.preview.terrainSizeMeters);
        const uint64_t parameterHash = HashMaskBlurSettings(node.maskBlur, terrainSize);
        MaskNodeCache& cache = maskCache_[node.id];
        if (!cache.valid || cache.inputHash != inputHash || cache.parameterHash != parameterHash)
        {
            g_currentlyEvaluatingNodeId.store(node.id, std::memory_order_relaxed);
            cache.grid = ApplyMaskBlur(input, node.maskBlur, terrainSize);
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
        else if (node->kind == NodeKind::HeightmapFromMask)
        {
            pipeline.hasSource = true;
            pipeline.useMaskSource = true;
            pipeline.maskSourceNodeId = node->id;
            pipeline.heightmapFromMask = node->heightmapFromMask;
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
                upstreamNode->kind == NodeKind::HeightmapFromMask ||
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

    // Track which node's kernel is running so the UI can paint a "Processing"
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
        for (const PathPoint& point : node.path.points)
        {
            maxId = std::max(maxId, point.id);
        }
        for (const PathEdge& edge : node.path.edges)
        {
            maxId = std::max(maxId, edge.id);
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
    case ShapeKind::Box:
        return "Box";
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
    if (const NodeDefinition* definition = FindNodeDefinition(kind))
    {
        return definition->title;
    }
    return "Unknown";
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
    case PreviewStage::FluvialErosion:
        return "Fluvial Erosion";
    case PreviewStage::DropletErosion:
        return "Droplet Erosion";
    case PreviewStage::MaskNoise:
        return "Mask Noise";
    case PreviewStage::MaskBlend:
        return "Mask Blend";
    case PreviewStage::MaskLevels:
        return "Mask Levels";
    case PreviewStage::MaskBlur:
        return "Mask Blur";
    case PreviewStage::MaskSlope:
        return "Mask Slope";
    case PreviewStage::MaskHeight:
        return "Mask Height";
    case PreviewStage::MaskPath:
        return "Mask Path";
    case PreviewStage::HeightmapFromMask:
        return "Heightmap From Mask";
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
    case ValueType::Path:
        return "Path";
    default:
        return "Unknown";
    }
}

PreviewStage PreviewStageFor(NodeKind kind)
{
    if (const NodeDefinition* definition = FindNodeDefinition(kind))
    {
        return definition->previewStage;
    }
    return PreviewStage::Graph;
}

const NodeDefinition* FindNodeDefinition(NodeKind kind)
{
    const auto it = std::ranges::find_if(kNodeDefinitions, [kind](const NodeDefinition& definition) {
        return definition.kind == kind;
    });
    return it != kNodeDefinitions.end() ? &*it : nullptr;
}

const NodeDefinition& GetNodeDefinition(NodeKind kind)
{
    if (const NodeDefinition* definition = FindNodeDefinition(kind))
    {
        return *definition;
    }
    return kNodeDefinitions.front();
}

NodeCategory CategoryFor(NodeKind kind)
{
    if (const NodeDefinition* definition = FindNodeDefinition(kind))
    {
        return definition->category;
    }
    return NodeCategory::Heightfield;
}

bool IsKnownNodeKind(NodeKind kind)
{
    return FindNodeDefinition(kind) != nullptr;
}

bool IsMaskOnlyNodeKind(NodeKind kind)
{
    if (const NodeDefinition* definition = FindNodeDefinition(kind))
    {
        return definition->maskOnly;
    }
    return false;
}

bool IsColorOnlyNodeKind(NodeKind kind)
{
    if (const NodeDefinition* definition = FindNodeDefinition(kind))
    {
        return definition->colorOnly;
    }
    return false;
}

void SetScatterGpuEvaluator(ScatterGpuEvaluator evaluator)
{
    g_scatterGpuEvaluator = evaluator;
}

void SetMaskFluvialGpuEvaluator(MaskFluvialGpuEvaluator evaluator)
{
    g_maskFluvialGpuEvaluator = evaluator;
}

void SetMaskPathGpuEvaluator(MaskPathGpuEvaluator evaluator)
{
    g_maskPathGpuEvaluator = evaluator;
}

void SetHeightmapFromMaskGpuEvaluator(HeightmapFromMaskGpuEvaluator evaluator)
{
    g_heightmapFromMaskGpuEvaluator = evaluator;
}

std::atomic<GraphId>& CurrentlyEvaluatingNodeId()
{
    return g_currentlyEvaluatingNodeId;
}

} // namespace rock
