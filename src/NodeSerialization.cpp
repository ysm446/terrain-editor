#include "NodeSerialization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <utility>

namespace terrain
{
namespace
{

constexpr std::array<int, 5> kResolutionPresets = {128, 256, 512, 1024, 2048};

template <size_t N>
int NearestPreset(int value, const std::array<int, N>& presets, int fallback)
{
    const auto nearest = std::ranges::min_element(presets, [value](int lhs, int rhs) {
        return std::abs(lhs - value) < std::abs(rhs - value);
    });
    return nearest != presets.end() ? *nearest : fallback;
}

int NearestResolutionPreset(int value)
{
    return NearestPreset(value, kResolutionPresets, 512);
}

bool IsSerializableNodeKind(rock::NodeKind kind)
{
    switch (kind)
    {
    case rock::NodeKind::HeightmapLoad:
    case rock::NodeKind::HeightmapBlur:
    case rock::NodeKind::Shape:
    case rock::NodeKind::MultiScaleErosion:
    case rock::NodeKind::MaskNoise:
    case rock::NodeKind::MaskBlend:
    case rock::NodeKind::MaskFluvial:
    case rock::NodeKind::Rock:
    case rock::NodeKind::Sediment:
    case rock::NodeKind::Snow:
    case rock::NodeKind::Colorize:
    case rock::NodeKind::MaskCurvature:
    case rock::NodeKind::MaskLevels:
    case rock::NodeKind::MaskSlope:
    case rock::NodeKind::MaskHeight:
    case rock::NodeKind::Crumbling:
    case rock::NodeKind::Scatter:
    case rock::NodeKind::Path:
        return true;
    default:
        return false;
    }
}

} // namespace
nlohmann::json MakeBasicHeightfieldSettingsJson(const rock::Node& node, const AssetPathForJson& assetPathForJson)
{
    return {
        {"heightmap", {
            {"path", assetPathForJson(node.heightmap.path)},
            {"scaleMeters", node.heightmap.scaleMeters},
            {"relativeVerticalScalePercent", node.heightmap.relativeVerticalScalePercent},
            {"verticalOffsetMeters", node.heightmap.verticalOffsetMeters},
        }},
        {"shape", {
            {"kind", static_cast<int>(node.shape.kind)},
            {"scaleMeters", node.shape.scaleMeters},
            {"relativeHeightPercent", node.shape.relativeHeightPercent},
        }},
        {"heightmapBlur", {
            {"radius", node.heightmapBlur.radius},
            {"strength", node.heightmapBlur.strength},
            {"iterations", node.heightmapBlur.iterations},
        }},
    };
}

nlohmann::json MakeMultiScaleErosionSettingsJson(const rock::Node& node)
{
    return {
        {"multiScaleErosion", {
            {"iterations", node.multiScaleErosion.iterations},
            {"enableStreamPower", node.multiScaleErosion.enableStreamPower},
            {"enableThermal", node.multiScaleErosion.enableThermal},
            {"enableDeposition", node.multiScaleErosion.enableDeposition},
            {"speStrength", node.multiScaleErosion.speStrength},
            {"streamExponent", node.multiScaleErosion.streamExponent},
            {"slopeExponent", node.multiScaleErosion.slopeExponent},
            {"maxStreamPower", node.multiScaleErosion.maxStreamPower},
            {"flowExponent", node.multiScaleErosion.flowExponent},
            {"speTimeStep", node.multiScaleErosion.speTimeStep},
            {"thermalAngleDegrees", node.multiScaleErosion.thermalAngleDegrees},
            {"thermalStrength", node.multiScaleErosion.thermalStrength},
            {"thermalNoisifyAngle", node.multiScaleErosion.thermalNoisifyAngle},
            {"thermalNoiseMin", node.multiScaleErosion.thermalNoiseMin},
            {"thermalNoiseMax", node.multiScaleErosion.thermalNoiseMax},
            {"thermalNoiseWavelength", node.multiScaleErosion.thermalNoiseWavelength},
            {"depositionStrength", node.multiScaleErosion.depositionStrength},
            {"rain", node.multiScaleErosion.rain},
            {"useMultigrid", node.multiScaleErosion.useMultigrid},
            {"backend", static_cast<int>(node.multiScaleErosion.backend)},
        }},
    };
}

nlohmann::json MakeMaskSettingsJson(const rock::Node& node)
{
    return {
        {"maskNoise", {
            {"seed", node.maskNoise.seed},
            {"octaves", node.maskNoise.octaves},
            {"frequency", node.maskNoise.frequency},
            {"lacunarity", node.maskNoise.lacunarity},
            {"persistence", node.maskNoise.persistence},
            {"backend", static_cast<int>(node.maskNoise.backend)},
        }},
        {"maskFluvial", {
            {"simulationMode", static_cast<int>(node.maskFluvial.simulationMode)},
            {"algorithm", static_cast<int>(node.maskFluvial.algorithm)},
            {"outputCurve", static_cast<int>(node.maskFluvial.outputCurve)},
            {"accumulationThreshold", node.maskFluvial.accumulationThreshold},
            {"gamma", node.maskFluvial.gamma},
            {"softness", node.maskFluvial.softness},
            {"power", node.maskFluvial.power},
            {"largestDetailLevelM", node.maskFluvial.largestDetailLevelM},
            {"mfdExponent", node.maskFluvial.mfdExponent},
            {"particleCount", node.maskFluvial.particleCount},
            {"particleLifetime", node.maskFluvial.particleLifetime},
            {"particleInertia", node.maskFluvial.particleInertia},
            {"particleStepLengthM", node.maskFluvial.particleStepLengthM},
            {"particleSeed", node.maskFluvial.particleSeed},
            {"backend", static_cast<int>(node.maskFluvial.backend)},
        }},
        {"maskCurvature", {
            {"mode", static_cast<int>(node.maskCurvature.mode)},
            {"largestDetailLevelM", node.maskCurvature.largestDetailLevelM},
            {"radius", node.maskCurvature.radius},
            {"sensitivityMeters", node.maskCurvature.sensitivityMeters},
            {"threshold", node.maskCurvature.threshold},
            {"gamma", node.maskCurvature.gamma},
        }},
        {"maskLevels", {
            {"blackPoint", node.maskLevels.blackPoint},
            {"whitePoint", node.maskLevels.whitePoint},
            {"gamma", node.maskLevels.gamma},
            {"invert", node.maskLevels.invert},
        }},
        {"maskSlope", {
            {"largestDetailLevelM", node.maskSlope.largestDetailLevelM},
            {"slopeMinDeg", node.maskSlope.slopeMinDeg},
            {"slopeMaxDeg", node.maskSlope.slopeMaxDeg},
            {"gamma", node.maskSlope.gamma},
            {"invert", node.maskSlope.invert},
        }},
        {"maskHeight", {
            {"useFullRange", node.maskHeight.useFullRange},
            {"heightMinMeters", node.maskHeight.heightMinMeters},
            {"heightMaxMeters", node.maskHeight.heightMaxMeters},
            {"featherMeters", node.maskHeight.featherMeters},
            {"gamma", node.maskHeight.gamma},
            {"invert", node.maskHeight.invert},
        }},
        {"maskBlend", {
            {"mode", static_cast<int>(node.maskBlend.mode)},
            {"intensity", node.maskBlend.intensity},
        }},
    };
}

nlohmann::json MakeSnowSettingsJson(const rock::Node& node)
{
    return {
        {"snow", {
            {"emissionAmount", node.snow.emissionAmount},
            {"slopeLimitMinDeg", node.snow.slopeLimitMinDeg},
            {"slopeLimitMaxDeg", node.snow.slopeLimitMaxDeg},
            {"maskMaxSnow", node.snow.maskMaxSnow},
            {"iterationCount", node.snow.iterationCount},
            {"emissionTime", node.snow.emissionTime},
            {"smoothingIterations", node.snow.smoothingIterations},
            {"motionSlopeLimitDeg", node.snow.motionSlopeLimitDeg},
            {"transportRate", node.snow.transportRate},
            {"surfaceSmoothing", node.snow.surfaceSmoothing},
            {"maskThresholdM", node.snow.maskThresholdM},
            {"maskFeatherM", node.snow.maskFeatherM},
            {"largestDetailLevelM", node.snow.largestDetailLevelM},
            {"fillRadius", node.snow.fillRadius},
            {"backend", static_cast<int>(node.snow.backend)},
        }},
    };
}

nlohmann::json MakeColorizeSettingsJson(const rock::Node& node)
{
    nlohmann::json stopsArr = nlohmann::json::array();
    for (const rock::ColorStop& s : node.colorize.stops)
    {
        stopsArr.push_back({{"position", s.position}, {"r", s.r}, {"g", s.g}, {"b", s.b}});
    }
    return {{"colorize", {
        {"backend", static_cast<int>(node.colorize.backend)},
        {"stops", stopsArr},
    }}};
}

nlohmann::json MakePathSettingsJson(const rock::Node& node)
{
    nlohmann::json pointsJson = nlohmann::json::array();
    for (const rock::PathPoint& point : node.path.points)
    {
        pointsJson.push_back({
            {"id", point.id},
            {"x", point.x},
            {"z", point.z},
            {"height", point.height},
            {"heightOffset", point.heightOffset},
            {"heightMode", static_cast<int>(point.heightMode)},
        });
    }

    nlohmann::json edgesJson = nlohmann::json::array();
    for (const rock::PathEdge& edge : node.path.edges)
    {
        edgesJson.push_back({
            {"id", edge.id},
            {"fromPoint", edge.fromPoint},
            {"toPoint", edge.toPoint},
            {"widthMeters", edge.widthMeters},
            {"featherMeters", edge.featherMeters},
            {"enabled", edge.enabled},
        });
    }

    return {{"path", {
        {"defaultWidthMeters", node.path.defaultWidthMeters},
        {"defaultFeatherMeters", node.path.defaultFeatherMeters},
        {"points", pointsJson},
        {"edges", edgesJson},
    }}};
}

nlohmann::json MakeRockSettingsJson(const rock::Node& node)
{
    return {
        {"rock", {
            {"style", static_cast<int>(node.rock.style)},
            {"orientationRule", static_cast<int>(node.rock.orientationRule)},
            {"layerCount", node.rock.layerCount},
            {"seed", node.rock.seed},
            {"density", node.rock.density},
            {"coverage", node.rock.coverage},
            {"rockSizeMinM", node.rock.rockSizeMinM},
            {"rockSizeMaxM", node.rock.rockSizeMaxM},
            {"rockHeight", node.rock.rockHeight},
            {"heightJitter", node.rock.heightJitter},
            {"rotationVariation", node.rock.rotationVariation},
            {"aspectVariation", node.rock.aspectVariation},
            {"edgeSharpness", node.rock.edgeSharpness},
            {"bumpiness", node.rock.bumpiness},
            {"facetSharpness", node.rock.facetSharpness},
            {"facetScale", node.rock.facetScale},
            {"groundDetailLevelM", node.rock.groundDetailLevelM},
            {"backend", static_cast<int>(node.rock.backend)},
        }},
    };
}

nlohmann::json MakeScatterSettingsJson(const rock::Node& node)
{
    return {
        {"scatter", {
            {"shapeType", static_cast<int>(node.scatter.shapeType)},
            {"orientationRule", static_cast<int>(node.scatter.orientationRule)},
            {"seed", node.scatter.seed},
            {"density", node.scatter.density},
            {"coverage", node.scatter.coverage},
            {"sizeMinM", node.scatter.sizeMinM},
            {"sizeMaxM", node.scatter.sizeMaxM},
            {"height", node.scatter.height},
            {"heightJitter", node.scatter.heightJitter},
            {"rotationVariation", node.scatter.rotationVariation},
            {"aspectVariation", node.scatter.aspectVariation},
            {"groundDetailLevelM", node.scatter.groundDetailLevelM},
            {"backend", static_cast<int>(node.scatter.backend)},
        }},
    };
}

nlohmann::json MakeCrumblingSettingsJson(const rock::Node& node)
{
    return {
        {"crumbling", {
            {"physicsCount", node.crumbling.physicsCount},
            {"debrisAmount", node.crumbling.debrisAmount},
            {"debrisSizeMinM", node.crumbling.debrisSizeMinM},
            {"debrisSizeMaxM", node.crumbling.debrisSizeMaxM},
            {"style", static_cast<int>(node.crumbling.style)},
            {"gravity", node.crumbling.gravity},
            {"spread", node.crumbling.spread},
            {"seed", node.crumbling.seed},
        }},
    };
}

nlohmann::json MakeSedimentSettingsJson(const rock::Node& node)
{
    return {
        {"sediment", {
            {"iterations", node.sediment.iterations},
            {"stabilizationIterations", node.sediment.stabilizationIterations},
            {"largestDetailLevelM", node.sediment.largestDetailLevelM},
            {"emissionAmountM", node.sediment.emissionAmountM},
            {"emissionTime", node.sediment.emissionTime},
            {"sedimentViscosity", node.sediment.sedimentViscosity},
            {"convertTerrainToSediment", node.sediment.convertTerrainToSediment},
            {"maskContrast", node.sediment.maskContrast},
            {"backend", static_cast<int>(node.sediment.backend)},
        }},
    };
}

nlohmann::json MakeNodeSettingsJson(const rock::Node& node, const AssetPathForJson& assetPathForJson)
{
    nlohmann::json nodeJson;
    nodeJson.update(MakeBasicHeightfieldSettingsJson(node, assetPathForJson));
    nodeJson.update(MakeMultiScaleErosionSettingsJson(node));
    nodeJson.update(MakeMaskSettingsJson(node));
    nodeJson.update(MakeCrumblingSettingsJson(node));
    nodeJson.update(MakeRockSettingsJson(node));
    nodeJson.update(MakeScatterSettingsJson(node));
    nodeJson.update(MakeSedimentSettingsJson(node));
    nodeJson.update(MakeSnowSettingsJson(node));
    nodeJson.update(MakeColorizeSettingsJson(node));
    nodeJson.update(MakePathSettingsJson(node));
    return nodeJson;
}

nlohmann::json MakeSerializedNodeJson(const rock::Node& node, const AssetPathForJson& assetPathForJson)
{
    nlohmann::json nodeJson = {
        {"id", node.id},
        {"kind", static_cast<int>(node.kind)},
        {"title", node.title},
        {"inputs", nlohmann::json::array()},
        {"outputs", nlohmann::json::array()},
    };
    nodeJson.update(MakeNodeSettingsJson(node, assetPathForJson));

    for (const rock::Pin& pin : node.inputs)
    {
        nodeJson["inputs"].push_back({
            {"id", pin.id},
            {"valueType", static_cast<int>(pin.valueType)},
            {"label", pin.label},
        });
    }
    for (const rock::Pin& pin : node.outputs)
    {
        nodeJson["outputs"].push_back({
            {"id", pin.id},
            {"valueType", static_cast<int>(pin.valueType)},
            {"label", pin.label},
        });
    }
    return nodeJson;
}

nlohmann::json MakeSerializedNodesJson(const std::vector<rock::Node>& nodes, const AssetPathForJson& assetPathForJson)
{
    nlohmann::json nodesJson = nlohmann::json::array();
    for (const rock::Node& node : nodes)
    {
        nodesJson.push_back(MakeSerializedNodeJson(node, assetPathForJson));
    }
    return nodesJson;
}

nlohmann::json MakeSerializedLinksJson(const std::vector<rock::Link>& links)
{
    nlohmann::json linksJson = nlohmann::json::array();
    for (const rock::Link& link : links)
    {
        linksJson.push_back({
            {"id", link.id},
            {"startPin", link.startPin},
            {"endPin", link.endPin},
        });
    }
    return linksJson;
}

std::optional<rock::NodeKind> ReadSerializedNodeKind(const nlohmann::json& nodeJson)
{
    const int kindInt = nodeJson.value("kind", 0);
    const rock::NodeKind kind = static_cast<rock::NodeKind>(kindInt);
    if (!IsSerializableNodeKind(kind))
    {
        return std::nullopt;
    }
    return kind;
}

std::optional<rock::PreviewStage> ReadSerializedPreviewStage(const nlohmann::json& root, rock::PreviewStage fallbackStage)
{
    const int stageInt = root.value("previewStage", static_cast<int>(fallbackStage));
    const rock::PreviewStage stage = static_cast<rock::PreviewStage>(stageInt);
    switch (stage)
    {
    case rock::PreviewStage::Graph:
    case rock::PreviewStage::HeightmapBlur:
    case rock::PreviewStage::Shape:
    case rock::PreviewStage::MultiScaleErosion:
    case rock::PreviewStage::MaskNoise:
    case rock::PreviewStage::MaskBlend:
    case rock::PreviewStage::MaskLevels:
    case rock::PreviewStage::MaskSlope:
    case rock::PreviewStage::MaskHeight:
    case rock::PreviewStage::Crumbling:
    case rock::PreviewStage::MaskCurvature:
    case rock::PreviewStage::MaskFluvial:
    case rock::PreviewStage::Rock:
    case rock::PreviewStage::Scatter:
    case rock::PreviewStage::Sediment:
    case rock::PreviewStage::Snow:
    case rock::PreviewStage::Colorize:
        return stage;
    default:
        return std::nullopt;
    }
}

void ReadBasicHeightfieldSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeHeightmapJson = nodeJson.value("heightmap", nlohmann::json::object());
    const nlohmann::json nodeShapeJson = nodeJson.value("shape", nlohmann::json::object());
    const nlohmann::json nodeBlurJson = nodeJson.value("heightmapBlur", nlohmann::json::object());
    node.heightmap.path = nodeHeightmapJson.value("path", node.heightmap.path);
    node.heightmap.scaleMeters = std::clamp(nodeHeightmapJson.value("scaleMeters", node.heightmap.scaleMeters), 1.0f, 1000000.0f);
    node.heightmap.relativeVerticalScalePercent = std::clamp(nodeHeightmapJson.value("relativeVerticalScalePercent", node.heightmap.relativeVerticalScalePercent), 0.0f, 10000.0f);
    node.heightmap.verticalOffsetMeters = std::clamp(nodeHeightmapJson.value("verticalOffsetMeters", node.heightmap.verticalOffsetMeters), -1000000.0f, 1000000.0f);
    node.heightmap.simulationResolution = NearestResolutionPreset(nodeHeightmapJson.value("simulationResolution", node.heightmap.simulationResolution));
    node.shape.kind = static_cast<rock::ShapeKind>(std::clamp(nodeShapeJson.value("kind", static_cast<int>(node.shape.kind)), 0, 1));
    node.shape.scaleMeters = std::clamp(nodeShapeJson.value("scaleMeters", node.shape.scaleMeters), 1.0f, 1000000.0f);
    node.shape.relativeHeightPercent = std::clamp(nodeShapeJson.value("relativeHeightPercent", node.shape.relativeHeightPercent), 0.0f, 10000.0f);
    node.shape.simulationResolution = NearestResolutionPreset(nodeShapeJson.value("simulationResolution", node.shape.simulationResolution));
    node.heightmapBlur.radius = std::clamp(nodeBlurJson.value("radius", node.heightmapBlur.radius), 0.0f, 128.0f);
    node.heightmapBlur.strength = std::clamp(nodeBlurJson.value("strength", node.heightmapBlur.strength), 0.0f, 1.0f);
    node.heightmapBlur.iterations = std::clamp(nodeBlurJson.value("iterations", node.heightmapBlur.iterations), 0, 64);
}

void ReadMultiScaleErosionSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeMultiScaleErosionJson = nodeJson.value("multiScaleErosion", nlohmann::json::object());

    node.multiScaleErosion.iterations = std::clamp(nodeMultiScaleErosionJson.value("iterations", node.multiScaleErosion.iterations), 0, 500);
    node.multiScaleErosion.enableStreamPower = nodeMultiScaleErosionJson.value("enableStreamPower", node.multiScaleErosion.enableStreamPower);
    node.multiScaleErosion.enableThermal = nodeMultiScaleErosionJson.value("enableThermal", node.multiScaleErosion.enableThermal);
    node.multiScaleErosion.enableDeposition = nodeMultiScaleErosionJson.value("enableDeposition", node.multiScaleErosion.enableDeposition);
    node.multiScaleErosion.speStrength = std::clamp(nodeMultiScaleErosionJson.value("speStrength", node.multiScaleErosion.speStrength), 0.0f, 0.01f);
    node.multiScaleErosion.streamExponent = std::clamp(nodeMultiScaleErosionJson.value("streamExponent", node.multiScaleErosion.streamExponent), 0.0f, 2.0f);
    node.multiScaleErosion.slopeExponent = std::clamp(nodeMultiScaleErosionJson.value("slopeExponent", node.multiScaleErosion.slopeExponent), 0.0f, 4.0f);
    node.multiScaleErosion.maxStreamPower = std::clamp(nodeMultiScaleErosionJson.value("maxStreamPower", node.multiScaleErosion.maxStreamPower), 1.0f, 1000000.0f);
    node.multiScaleErosion.flowExponent = std::clamp(nodeMultiScaleErosionJson.value("flowExponent", node.multiScaleErosion.flowExponent), 0.5f, 4.0f);
    node.multiScaleErosion.speTimeStep = std::clamp(nodeMultiScaleErosionJson.value("speTimeStep", node.multiScaleErosion.speTimeStep), 0.0f, 4.0f);
    node.multiScaleErosion.thermalAngleDegrees = std::clamp(nodeMultiScaleErosionJson.value("thermalAngleDegrees", node.multiScaleErosion.thermalAngleDegrees), 0.0f, 60.0f);
    node.multiScaleErosion.thermalStrength = std::clamp(nodeMultiScaleErosionJson.value("thermalStrength", node.multiScaleErosion.thermalStrength), 0.0f, 0.01f);
    node.multiScaleErosion.thermalNoisifyAngle = nodeMultiScaleErosionJson.value("thermalNoisifyAngle", node.multiScaleErosion.thermalNoisifyAngle);
    node.multiScaleErosion.thermalNoiseMin = std::clamp(nodeMultiScaleErosionJson.value("thermalNoiseMin", node.multiScaleErosion.thermalNoiseMin), 0.0f, 4.0f);
    node.multiScaleErosion.thermalNoiseMax = std::clamp(nodeMultiScaleErosionJson.value("thermalNoiseMax", node.multiScaleErosion.thermalNoiseMax), 0.0f, 4.0f);
    node.multiScaleErosion.thermalNoiseWavelength = std::clamp(nodeMultiScaleErosionJson.value("thermalNoiseWavelength", node.multiScaleErosion.thermalNoiseWavelength), 0.0f, 0.05f);
    node.multiScaleErosion.depositionStrength = std::clamp(nodeMultiScaleErosionJson.value("depositionStrength", node.multiScaleErosion.depositionStrength), 0.0f, 8.0f);
    node.multiScaleErosion.rain = std::clamp(nodeMultiScaleErosionJson.value("rain", node.multiScaleErosion.rain), 0.0f, 10.0f);
    node.multiScaleErosion.useMultigrid = nodeMultiScaleErosionJson.value("useMultigrid", node.multiScaleErosion.useMultigrid);
    {
        const int backendInt = std::clamp(nodeMultiScaleErosionJson.value("backend", static_cast<int>(node.multiScaleErosion.backend)),
                                           static_cast<int>(rock::MultiScaleErosionBackend::CpuReference),
                                           static_cast<int>(rock::MultiScaleErosionBackend::GpuCompute));
        node.multiScaleErosion.backend = static_cast<rock::MultiScaleErosionBackend>(backendInt);
    }
}

void ReadMaskSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeMaskNoiseJson = nodeJson.value("maskNoise", nlohmann::json::object());
    const nlohmann::json nodeMaskBlendJson = nodeJson.value("maskBlend", nlohmann::json::object());
    const nlohmann::json nodeMaskFluvialJson = nodeJson.value("maskFluvial", nlohmann::json::object());
    const nlohmann::json nodeMaskCurvatureJson = nodeJson.value("maskCurvature", nlohmann::json::object());
    const nlohmann::json nodeMaskLevelsJson = nodeJson.value("maskLevels", nlohmann::json::object());
    const nlohmann::json nodeMaskSlopeJson = nodeJson.value("maskSlope", nlohmann::json::object());
    const nlohmann::json nodeMaskHeightJson = nodeJson.value("maskHeight", nlohmann::json::object());

    node.maskNoise.seed = std::clamp(nodeMaskNoiseJson.value("seed", node.maskNoise.seed), 0, 999999);
    node.maskNoise.octaves = std::clamp(nodeMaskNoiseJson.value("octaves", node.maskNoise.octaves), 1, 12);
    node.maskNoise.frequency = std::clamp(nodeMaskNoiseJson.value("frequency", node.maskNoise.frequency), 0.0f, 256.0f);
    node.maskNoise.lacunarity = std::clamp(nodeMaskNoiseJson.value("lacunarity", node.maskNoise.lacunarity), 0.0f, 8.0f);
    node.maskNoise.persistence = std::clamp(nodeMaskNoiseJson.value("persistence", node.maskNoise.persistence), 0.0f, 1.0f);
    node.maskNoise.simulationResolution = NearestResolutionPreset(nodeMaskNoiseJson.value("simulationResolution", node.maskNoise.simulationResolution));
    {
        const int maskNoiseBackendInt = std::clamp(nodeMaskNoiseJson.value("backend", static_cast<int>(node.maskNoise.backend)),
                                                    static_cast<int>(rock::MaskNoiseBackend::CpuParallel),
                                                    static_cast<int>(rock::MaskNoiseBackend::GpuCompute));
        node.maskNoise.backend = static_cast<rock::MaskNoiseBackend>(maskNoiseBackendInt);
    }
    {
        const int modeInt = std::clamp(nodeMaskBlendJson.value("mode", static_cast<int>(node.maskBlend.mode)),
                                        static_cast<int>(rock::MaskBlendMode::Add),
                                        static_cast<int>(rock::MaskBlendMode::Max));
        node.maskBlend.mode = static_cast<rock::MaskBlendMode>(modeInt);
    }
    node.maskBlend.intensity = std::clamp(nodeMaskBlendJson.value("intensity", node.maskBlend.intensity), 0.0f, 1.0f);
    {
        const int modeInt = std::clamp(nodeMaskCurvatureJson.value("mode", static_cast<int>(node.maskCurvature.mode)),
                                        static_cast<int>(rock::MaskCurvatureMode::Ridges),
                                        static_cast<int>(rock::MaskCurvatureMode::Absolute));
        node.maskCurvature.mode = static_cast<rock::MaskCurvatureMode>(modeInt);
    }
    node.maskCurvature.radius = std::clamp(nodeMaskCurvatureJson.value("radius", node.maskCurvature.radius), 1, 64);
    node.maskCurvature.largestDetailLevelM = std::clamp(
        nodeMaskCurvatureJson.value("largestDetailLevelM", static_cast<float>(node.maskCurvature.radius)),
        1.0f,
        1024.0f);
    node.maskCurvature.sensitivityMeters = std::clamp(nodeMaskCurvatureJson.value("sensitivityMeters", node.maskCurvature.sensitivityMeters), 0.001f, 1000.0f);
    node.maskCurvature.threshold = std::clamp(nodeMaskCurvatureJson.value("threshold", node.maskCurvature.threshold), 0.0f, 0.99f);
    node.maskCurvature.gamma = std::clamp(nodeMaskCurvatureJson.value("gamma", node.maskCurvature.gamma), 0.05f, 8.0f);
    node.maskLevels.blackPoint = std::clamp(nodeMaskLevelsJson.value("blackPoint", node.maskLevels.blackPoint), 0.0f, 1.0f);
    node.maskLevels.whitePoint = std::clamp(nodeMaskLevelsJson.value("whitePoint", node.maskLevels.whitePoint), 0.0f, 1.0f);
    node.maskLevels.gamma = std::clamp(nodeMaskLevelsJson.value("gamma", node.maskLevels.gamma), 0.05f, 8.0f);
    node.maskLevels.invert = nodeMaskLevelsJson.value("invert", node.maskLevels.invert);
    node.maskSlope.largestDetailLevelM = std::clamp(nodeMaskSlopeJson.value("largestDetailLevelM", node.maskSlope.largestDetailLevelM), 0.0f, 1024.0f);
    node.maskSlope.slopeMinDeg = std::clamp(nodeMaskSlopeJson.value("slopeMinDeg", node.maskSlope.slopeMinDeg), 0.0f, 89.9f);
    node.maskSlope.slopeMaxDeg = std::clamp(nodeMaskSlopeJson.value("slopeMaxDeg", node.maskSlope.slopeMaxDeg), 0.0f, 89.9f);
    if (node.maskSlope.slopeMaxDeg < node.maskSlope.slopeMinDeg)
    {
        std::swap(node.maskSlope.slopeMinDeg, node.maskSlope.slopeMaxDeg);
    }
    node.maskSlope.gamma = std::clamp(nodeMaskSlopeJson.value("gamma", node.maskSlope.gamma), 0.05f, 8.0f);
    node.maskSlope.invert = nodeMaskSlopeJson.value("invert", node.maskSlope.invert);
    node.maskHeight.useFullRange = nodeMaskHeightJson.value("useFullRange", node.maskHeight.useFullRange);
    node.maskHeight.heightMinMeters = std::clamp(nodeMaskHeightJson.value("heightMinMeters", node.maskHeight.heightMinMeters), -100000.0f, 100000.0f);
    node.maskHeight.heightMaxMeters = std::clamp(nodeMaskHeightJson.value("heightMaxMeters", node.maskHeight.heightMaxMeters), -100000.0f, 100000.0f);
    if (node.maskHeight.heightMaxMeters < node.maskHeight.heightMinMeters)
    {
        std::swap(node.maskHeight.heightMinMeters, node.maskHeight.heightMaxMeters);
    }
    node.maskHeight.featherMeters = std::clamp(nodeMaskHeightJson.value("featherMeters", node.maskHeight.featherMeters), 0.0f, 100000.0f);
    node.maskHeight.gamma = std::clamp(nodeMaskHeightJson.value("gamma", node.maskHeight.gamma), 0.05f, 8.0f);
    node.maskHeight.invert = nodeMaskHeightJson.value("invert", node.maskHeight.invert);
    {
        const int modeInt = std::clamp(nodeMaskFluvialJson.value("simulationMode", static_cast<int>(node.maskFluvial.simulationMode)),
                                       static_cast<int>(rock::MaskFluvialSimulationMode::FlowAccumulation),
                                       static_cast<int>(rock::MaskFluvialSimulationMode::Particles));
        node.maskFluvial.simulationMode = static_cast<rock::MaskFluvialSimulationMode>(modeInt);
    }
    {
        (void)nodeMaskFluvialJson.value("algorithm", static_cast<int>(node.maskFluvial.algorithm));
        node.maskFluvial.algorithm = rock::FlowAccumulationAlgorithm::MFD;
    }
    {
        const int curveInt = std::clamp(nodeMaskFluvialJson.value("outputCurve", static_cast<int>(node.maskFluvial.outputCurve)),
                                         static_cast<int>(rock::MaskFluvialOutputCurve::Log),
                                         static_cast<int>(rock::MaskFluvialOutputCurve::Linear));
        node.maskFluvial.outputCurve = static_cast<rock::MaskFluvialOutputCurve>(curveInt);
    }
    node.maskFluvial.accumulationThreshold = std::clamp(nodeMaskFluvialJson.value("accumulationThreshold", node.maskFluvial.accumulationThreshold), 0.0f, 1.0f);
    node.maskFluvial.gamma = std::clamp(nodeMaskFluvialJson.value("gamma", node.maskFluvial.gamma), 0.05f, 8.0f);
    node.maskFluvial.softness = std::clamp(nodeMaskFluvialJson.value("softness", node.maskFluvial.softness), 0.001f, 4.0f);
    node.maskFluvial.power = std::clamp(nodeMaskFluvialJson.value("power", node.maskFluvial.power), 0.1f, 8.0f);
    (void)nodeMaskFluvialJson.value("pitFillIterations", node.maskFluvial.pitFillIterations);
    node.maskFluvial.pitFillIterations = rock::MaskFluvialSettings{}.pitFillIterations;
    node.maskFluvial.largestDetailLevelM = std::clamp(nodeMaskFluvialJson.value("largestDetailLevelM", node.maskFluvial.largestDetailLevelM), 1.0f, 1024.0f);
    node.maskFluvial.mfdExponent = std::clamp(nodeMaskFluvialJson.value("mfdExponent", node.maskFluvial.mfdExponent), 0.1f, 16.0f);
    (void)nodeMaskFluvialJson.value("inertia", node.maskFluvial.inertia);
    node.maskFluvial.inertia = rock::MaskFluvialSettings{}.inertia;
    node.maskFluvial.particleCount = std::clamp(nodeMaskFluvialJson.value("particleCount", node.maskFluvial.particleCount), 1, 200000);
    node.maskFluvial.particleLifetime = std::clamp(nodeMaskFluvialJson.value("particleLifetime", node.maskFluvial.particleLifetime), 1, 2048);
    node.maskFluvial.particleInertia = std::clamp(nodeMaskFluvialJson.value("particleInertia", node.maskFluvial.particleInertia), 0.0f, 0.98f);
    node.maskFluvial.particleStepLengthM = std::clamp(nodeMaskFluvialJson.value("particleStepLengthM", node.maskFluvial.particleStepLengthM), 0.01f, 1024.0f);
    node.maskFluvial.particleSeed = std::clamp(nodeMaskFluvialJson.value("particleSeed", node.maskFluvial.particleSeed), 0, 999999);
    {
        const int backendInt = std::clamp(nodeMaskFluvialJson.value("backend", static_cast<int>(node.maskFluvial.backend)),
                                           static_cast<int>(rock::MaskFluvialBackend::CpuReference),
                                           static_cast<int>(rock::MaskFluvialBackend::GpuCompute));
        node.maskFluvial.backend = static_cast<rock::MaskFluvialBackend>(backendInt);
    }
}

void ReadRockSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeRockJson = nodeJson.value("rock", nlohmann::json::object());

    if (nodeRockJson.contains("style"))
    {
        const int styleInt = nodeRockJson.value("style", static_cast<int>(node.rock.style));
        node.rock.style = static_cast<rock::RockStyle>(std::clamp(styleInt,
            static_cast<int>(rock::RockStyle::Classic),
            static_cast<int>(rock::RockStyle::Shard)));
    }
    else
    {
        node.rock.style = rock::RockStyle::Classic;
    }
    {
        const int orientationInt = nodeRockJson.value("orientationRule", static_cast<int>(node.rock.orientationRule));
        node.rock.orientationRule = static_cast<rock::RockOrientationRule>(std::clamp(orientationInt,
            static_cast<int>(rock::RockOrientationRule::Flat),
            static_cast<int>(rock::RockOrientationRule::SlopeOriented)));
    }
    node.rock.layerCount = std::clamp(nodeRockJson.value("layerCount", node.rock.layerCount), 1, 8);
    node.rock.seed = std::clamp(nodeRockJson.value("seed", node.rock.seed), 0, 999999);
    node.rock.density = std::clamp(nodeRockJson.value("density", node.rock.density), 0.5f, 1000.0f);
    node.rock.coverage = std::clamp(nodeRockJson.value("coverage", node.rock.coverage), 0.0f, 1.0f);
    const float density = node.rock.density;
    const float legacyRockFill = nodeRockJson.value("rockFill", -1.0f);
    const float legacyRockSize = nodeRockJson.value("rockSize", -1.0f);
    const float legacyMinRatio = nodeRockJson.value("rockSizeMin", -1.0f);
    const float legacyMaxRatio = nodeRockJson.value("rockSizeMax", -1.0f);
    if (legacyRockFill > 0.0f)
    {
        node.rock.rockSizeMinM = legacyRockFill * density;
        node.rock.rockSizeMaxM = node.rock.rockSizeMinM;
    }
    else if (legacyRockSize > 0.0f)
    {
        node.rock.rockSizeMinM = legacyRockSize * density;
        node.rock.rockSizeMaxM = node.rock.rockSizeMinM;
    }
    else if (legacyMinRatio > 0.0f || legacyMaxRatio > 0.0f)
    {
        const float minR = (legacyMinRatio > 0.0f) ? legacyMinRatio : 0.7f;
        const float maxR = (legacyMaxRatio > 0.0f) ? legacyMaxRatio : 1.2f;
        node.rock.rockSizeMinM = minR * density;
        node.rock.rockSizeMaxM = maxR * density;
    }
    else
    {
        node.rock.rockSizeMinM = nodeRockJson.value("rockSizeMinM", node.rock.rockSizeMinM);
        node.rock.rockSizeMaxM = nodeRockJson.value("rockSizeMaxM", node.rock.rockSizeMaxM);
    }
    node.rock.rockSizeMinM = std::clamp(node.rock.rockSizeMinM, 0.1f, 200.0f);
    node.rock.rockSizeMaxM = std::clamp(std::max(node.rock.rockSizeMaxM, node.rock.rockSizeMinM), 0.1f, 200.0f);
    node.rock.rockHeight = std::clamp(nodeRockJson.value("rockHeight", node.rock.rockHeight), 0.0f, 100.0f);
    node.rock.heightJitter = std::clamp(nodeRockJson.value("heightJitter", node.rock.heightJitter), 0.0f, 1.0f);
    node.rock.rotationVariation = std::clamp(nodeRockJson.value("rotationVariation", node.rock.rotationVariation), 0.0f, 1.0f);
    node.rock.aspectVariation = std::clamp(nodeRockJson.value("aspectVariation", node.rock.aspectVariation), 0.0f, 1.0f);
    node.rock.edgeSharpness = std::clamp(nodeRockJson.value("edgeSharpness", node.rock.edgeSharpness), 0.0f, 1.0f);
    node.rock.bumpiness = std::clamp(nodeRockJson.value("bumpiness", node.rock.bumpiness), 0.0f, 1.0f);
    node.rock.facetSharpness = std::clamp(nodeRockJson.value("facetSharpness", node.rock.facetSharpness), 0.0f, 1.0f);
    node.rock.facetScale = std::clamp(nodeRockJson.value("facetScale", node.rock.facetScale), 0.5f, 8.0f);
    node.rock.groundDetailLevelM = std::clamp(nodeRockJson.value("groundDetailLevelM", node.rock.groundDetailLevelM), 0.0f, 1024.0f);
    {
        const int backendInt = nodeRockJson.value("backend", static_cast<int>(node.rock.backend));
        node.rock.backend = static_cast<rock::RockBackend>(std::clamp(backendInt,
            static_cast<int>(rock::RockBackend::CpuReference),
            static_cast<int>(rock::RockBackend::GpuCompute)));
    }
}

void ReadScatterSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeScatterJson = nodeJson.value("scatter", nlohmann::json::object());
    {
        const int shapeInt = std::clamp(nodeScatterJson.value("shapeType", static_cast<int>(node.scatter.shapeType)),
            static_cast<int>(rock::ScatterShapeType::Hemisphere),
            static_cast<int>(rock::ScatterShapeType::Cone));
        node.scatter.shapeType = static_cast<rock::ScatterShapeType>(shapeInt);
    }
    node.scatter.seed = std::clamp(nodeScatterJson.value("seed", node.scatter.seed), 0, 999999);
    {
        const int orientationInt = nodeScatterJson.value("orientationRule", static_cast<int>(node.scatter.orientationRule));
        node.scatter.orientationRule = static_cast<rock::RockOrientationRule>(std::clamp(orientationInt,
            static_cast<int>(rock::RockOrientationRule::Flat),
            static_cast<int>(rock::RockOrientationRule::SlopeOriented)));
    }
    node.scatter.density = std::clamp(nodeScatterJson.value("density", node.scatter.density), 0.5f, 1000.0f);
    node.scatter.coverage = std::clamp(nodeScatterJson.value("coverage", node.scatter.coverage), 0.0f, 1.0f);
    node.scatter.sizeMinM = std::clamp(nodeScatterJson.value("sizeMinM", node.scatter.sizeMinM), 0.1f, 200.0f);
    node.scatter.sizeMaxM = std::clamp(std::max(nodeScatterJson.value("sizeMaxM", node.scatter.sizeMaxM), node.scatter.sizeMinM), 0.1f, 200.0f);
    node.scatter.height = std::clamp(nodeScatterJson.value("height", node.scatter.height), 0.0f, 100.0f);
    node.scatter.heightJitter = std::clamp(nodeScatterJson.value("heightJitter", node.scatter.heightJitter), 0.0f, 1.0f);
    node.scatter.rotationVariation = std::clamp(nodeScatterJson.value("rotationVariation", node.scatter.rotationVariation), 0.0f, 1.0f);
    node.scatter.aspectVariation = std::clamp(nodeScatterJson.value("aspectVariation", node.scatter.aspectVariation), 0.0f, 1.0f);
    node.scatter.groundDetailLevelM = std::clamp(nodeScatterJson.value("groundDetailLevelM", node.scatter.groundDetailLevelM), 0.0f, 1024.0f);
    {
        const int backendInt = nodeScatterJson.value("backend", static_cast<int>(node.scatter.backend));
        node.scatter.backend = static_cast<rock::ScatterBackend>(std::clamp(backendInt,
            static_cast<int>(rock::ScatterBackend::CpuReference),
            static_cast<int>(rock::ScatterBackend::GpuCompute)));
    }
}

void ReadCrumblingSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeCrumblingJson = nodeJson.value("crumbling", nlohmann::json::object());
    node.crumbling.physicsCount = std::clamp(nodeCrumblingJson.value("physicsCount", node.crumbling.physicsCount), 0, 512);
    node.crumbling.debrisAmount = std::clamp(nodeCrumblingJson.value("debrisAmount", node.crumbling.debrisAmount), 0.0f, 1.0f);
    node.crumbling.debrisSizeMinM = std::clamp(nodeCrumblingJson.value("debrisSizeMinM", node.crumbling.debrisSizeMinM), 0.1f, 1000.0f);
    node.crumbling.debrisSizeMaxM = std::clamp(nodeCrumblingJson.value("debrisSizeMaxM", node.crumbling.debrisSizeMaxM), 0.1f, 1000.0f);
    if (node.crumbling.debrisSizeMaxM < node.crumbling.debrisSizeMinM)
    {
        std::swap(node.crumbling.debrisSizeMinM, node.crumbling.debrisSizeMaxM);
    }
    {
        const int styleInt = std::clamp(nodeCrumblingJson.value("style", static_cast<int>(node.crumbling.style)),
            static_cast<int>(rock::RockStyle::Classic),
            static_cast<int>(rock::RockStyle::Shard));
        node.crumbling.style = static_cast<rock::RockStyle>(styleInt);
    }
    node.crumbling.gravity = std::clamp(nodeCrumblingJson.value("gravity", node.crumbling.gravity), 0.0f, 1.0f);
    node.crumbling.spread = std::clamp(nodeCrumblingJson.value("spread", node.crumbling.spread), 0.0f, 1.0f);
    node.crumbling.seed = std::clamp(nodeCrumblingJson.value("seed", node.crumbling.seed), 0, 999999);
}

void ReadSedimentSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeSedimentJson = nodeJson.value("sediment", nlohmann::json::object());

    node.sediment.iterations = std::clamp(nodeSedimentJson.value("iterations", node.sediment.iterations), 1, 1000);
    node.sediment.stabilizationIterations = std::clamp(nodeSedimentJson.value("stabilizationIterations", node.sediment.stabilizationIterations), 1, 32);
    node.sediment.largestDetailLevelM = std::clamp(nodeSedimentJson.value("largestDetailLevelM", node.sediment.largestDetailLevelM), 1.0f, 1024.0f);
    node.sediment.emissionAmountM = std::clamp(nodeSedimentJson.value("emissionAmountM", node.sediment.emissionAmountM), 0.0f, 1000.0f);
    node.sediment.emissionTime = std::clamp(nodeSedimentJson.value("emissionTime", node.sediment.emissionTime), 0.0f, 1.0f);
    node.sediment.sedimentViscosity = std::clamp(nodeSedimentJson.value("sedimentViscosity", node.sediment.sedimentViscosity), 0.0f, 1.0f);
    node.sediment.convertTerrainToSediment = nodeSedimentJson.value("convertTerrainToSediment", node.sediment.convertTerrainToSediment);
    node.sediment.maskContrast = std::clamp(nodeSedimentJson.value("maskContrast", node.sediment.maskContrast), 0.0f, 1.0f);
    {
        const int backendInt = nodeSedimentJson.value("backend", static_cast<int>(node.sediment.backend));
        node.sediment.backend = static_cast<rock::SedimentBackend>(std::clamp(backendInt,
            static_cast<int>(rock::SedimentBackend::CpuReference),
            static_cast<int>(rock::SedimentBackend::GpuCompute)));
    }
}

void ReadSnowSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeSnowJson = nodeJson.value("snow", nlohmann::json::object());

    node.snow.emissionAmount = std::clamp(nodeSnowJson.value("emissionAmount", node.snow.emissionAmount), 0.0f, 100.0f);
    node.snow.slopeLimitMinDeg = std::clamp(nodeSnowJson.value("slopeLimitMinDeg", node.snow.slopeLimitMinDeg), 0.0f, 89.9f);
    node.snow.slopeLimitMaxDeg = std::clamp(std::max(nodeSnowJson.value("slopeLimitMaxDeg", node.snow.slopeLimitMaxDeg), node.snow.slopeLimitMinDeg), 0.0f, 89.9f);
    node.snow.maskMaxSnow = std::clamp(nodeSnowJson.value("maskMaxSnow", node.snow.maskMaxSnow), 0.001f, 1000.0f);
    node.snow.iterationCount = std::clamp(nodeSnowJson.value("iterationCount", node.snow.iterationCount), 1, 256);
    node.snow.emissionTime = std::clamp(nodeSnowJson.value("emissionTime", node.snow.emissionTime), 0.0f, 1.0f);
    node.snow.smoothingIterations = std::clamp(nodeSnowJson.value("smoothingIterations", node.snow.smoothingIterations), 1, 16);
    node.snow.motionSlopeLimitDeg = std::clamp(nodeSnowJson.value("motionSlopeLimitDeg", node.snow.motionSlopeLimitDeg), 0.0f, 89.9f);
    node.snow.transportRate = std::clamp(nodeSnowJson.value("transportRate", node.snow.transportRate), 0.0f, 1.0f);
    node.snow.surfaceSmoothing = std::clamp(nodeSnowJson.value("surfaceSmoothing", node.snow.surfaceSmoothing), 0.0f, 1.0f);
    node.snow.maskThresholdM = std::clamp(nodeSnowJson.value("maskThresholdM", node.snow.maskThresholdM), 0.0f, 1000.0f);
    node.snow.maskFeatherM = std::clamp(nodeSnowJson.value("maskFeatherM", node.snow.maskFeatherM), 0.0f, 1000.0f);
    node.snow.largestDetailLevelM = std::clamp(nodeSnowJson.value("largestDetailLevelM", node.snow.largestDetailLevelM), 1.0f, 1024.0f);
    node.snow.fillRadius = std::clamp(nodeSnowJson.value("fillRadius", node.snow.fillRadius), 1, 8);
    {
        const int backendInt = std::clamp(nodeSnowJson.value("backend", static_cast<int>(node.snow.backend)),
                                           static_cast<int>(rock::SnowBackend::CpuReference),
                                           static_cast<int>(rock::SnowBackend::GpuCompute));
        node.snow.backend = static_cast<rock::SnowBackend>(backendInt);
    }
}

void ReadColorizeSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json colorizeJson = nodeJson.value("colorize", nlohmann::json::object());
    {
        const int backendInt = std::clamp(colorizeJson.value("backend", static_cast<int>(node.colorize.backend)),
                                          static_cast<int>(rock::ColorizeBackend::CpuParallel),
                                          static_cast<int>(rock::ColorizeBackend::GpuCompute));
        node.colorize.backend = static_cast<rock::ColorizeBackend>(backendInt);
    }
    if (!colorizeJson.contains("stops") || !colorizeJson["stops"].is_array())
    {
        return;
    }
    node.colorize.stops.clear();
    for (const auto& stopJson : colorizeJson["stops"])
    {
        rock::ColorStop s;
        s.position = std::clamp(stopJson.value("position", 0.0f), 0.0f, 1.0f);
        s.r = std::clamp(stopJson.value("r", 0.0f), 0.0f, 1.0f);
        s.g = std::clamp(stopJson.value("g", 0.0f), 0.0f, 1.0f);
        s.b = std::clamp(stopJson.value("b", 0.0f), 0.0f, 1.0f);
        node.colorize.stops.push_back(s);
    }
    // デフォルトに戻す (stops が空になった場合)
    if (node.colorize.stops.empty())
    {
        node.colorize.stops = {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    }
}

void ReadPathSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json pathJson = nodeJson.value("path", nlohmann::json::object());
    node.path.defaultWidthMeters = std::clamp(pathJson.value("defaultWidthMeters", node.path.defaultWidthMeters), 0.01f, 100000.0f);
    node.path.defaultFeatherMeters = std::clamp(pathJson.value("defaultFeatherMeters", node.path.defaultFeatherMeters), 0.0f, 100000.0f);

    node.path.points.clear();
    if (pathJson.contains("points") && pathJson["points"].is_array())
    {
        for (const nlohmann::json& pointJson : pathJson["points"])
        {
            rock::PathPoint point;
            point.id = pointJson.value("id", 0);
            point.x = std::clamp(pointJson.value("x", 0.0f), -1000000.0f, 1000000.0f);
            point.z = std::clamp(pointJson.value("z", 0.0f), -1000000.0f, 1000000.0f);
            point.height = std::clamp(pointJson.value("height", 0.0f), -1000000.0f, 1000000.0f);
            point.heightOffset = std::clamp(pointJson.value("heightOffset", 0.0f), -1000000.0f, 1000000.0f);
            const int modeInt = std::clamp(pointJson.value("heightMode", static_cast<int>(point.heightMode)),
                static_cast<int>(rock::PathPointHeightMode::ProjectToTerrain),
                static_cast<int>(rock::PathPointHeightMode::Absolute));
            point.heightMode = static_cast<rock::PathPointHeightMode>(modeInt);
            if (point.id > 0)
            {
                node.path.points.push_back(point);
            }
        }
    }

    node.path.edges.clear();
    if (pathJson.contains("edges") && pathJson["edges"].is_array())
    {
        for (const nlohmann::json& edgeJson : pathJson["edges"])
        {
            rock::PathEdge edge;
            edge.id = edgeJson.value("id", 0);
            edge.fromPoint = edgeJson.value("fromPoint", 0);
            edge.toPoint = edgeJson.value("toPoint", 0);
            edge.widthMeters = std::clamp(edgeJson.value("widthMeters", node.path.defaultWidthMeters), 0.01f, 100000.0f);
            edge.featherMeters = std::clamp(edgeJson.value("featherMeters", node.path.defaultFeatherMeters), 0.0f, 100000.0f);
            edge.enabled = edgeJson.value("enabled", true);
            if (edge.id > 0 && edge.fromPoint > 0 && edge.toPoint > 0 && edge.fromPoint != edge.toPoint)
            {
                node.path.edges.push_back(edge);
            }
        }
    }
}

void ReadNodeSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    ReadBasicHeightfieldSettingsJson(nodeJson, node);
    ReadMultiScaleErosionSettingsJson(nodeJson, node);
    ReadMaskSettingsJson(nodeJson, node);
    ReadCrumblingSettingsJson(nodeJson, node);
    ReadRockSettingsJson(nodeJson, node);
    ReadScatterSettingsJson(nodeJson, node);
    ReadSedimentSettingsJson(nodeJson, node);
    ReadSnowSettingsJson(nodeJson, node);
    ReadColorizeSettingsJson(nodeJson, node);
    ReadPathSettingsJson(nodeJson, node);
}


void ReadSerializedPinsJson(const nlohmann::json& pinsJson,
                            rock::GraphId nodeId,
                            rock::PinKind pinKind,
                            std::vector<rock::Pin>& pins)
{
    if (!pinsJson.is_array())
    {
        return;
    }

    for (const nlohmann::json& pinJson : pinsJson)
    {
        rock::Pin pin;
        pin.id = pinJson.value("id", 0);
        pin.nodeId = nodeId;
        pin.kind = pinKind;
        const int serializedValueType = pinJson.value("valueType", static_cast<int>(rock::ValueType::HeightField));
        if (serializedValueType == static_cast<int>(rock::ValueType::Mask))
            pin.valueType = rock::ValueType::Mask;
        else if (serializedValueType == static_cast<int>(rock::ValueType::ColorTexture))
            pin.valueType = rock::ValueType::ColorTexture;
        else if (serializedValueType == static_cast<int>(rock::ValueType::Path))
            pin.valueType = rock::ValueType::Path;
        else
            pin.valueType = rock::ValueType::HeightField;
        pin.label = pinJson.value("label", std::string(rock::ToString(pin.valueType)));
        // 旧プロジェクトでは入力 / 出力どちらの heightfield ピンも `HeightField`
        // と保存されていた可能性があるが、現在は両方とも `Heightmap` に統一
        // しているのでマイグレーションする。
        if (pin.valueType == rock::ValueType::HeightField && pin.label == "HeightField")
        {
            pin.label = "Heightmap";
        }
        pins.push_back(std::move(pin));
    }
}

std::optional<rock::Node> ReadSerializedNodeJson(const nlohmann::json& nodeJson)
{
    rock::Node node;
    node.id = nodeJson.value("id", 0);
    const std::optional<rock::NodeKind> nodeKind = ReadSerializedNodeKind(nodeJson);
    if (!nodeKind || node.id == 0)
    {
        return std::nullopt;
    }

    node.kind = *nodeKind;
    node.title = nodeJson.value("title", std::string(rock::ToString(node.kind)));
    if (node.kind == rock::NodeKind::HeightmapLoad && node.title == "Load Heightmap")
    {
        node.title = std::string(rock::ToString(node.kind));
    }
    ReadNodeSettingsJson(nodeJson, node);
    ReadSerializedPinsJson(nodeJson.value("inputs", nlohmann::json::array()), node.id, rock::PinKind::Input, node.inputs);
    ReadSerializedPinsJson(nodeJson.value("outputs", nlohmann::json::array()), node.id, rock::PinKind::Output, node.outputs);
    return node;
}

std::vector<rock::Node> ReadSerializedNodesJson(const nlohmann::json& root)
{
    const nlohmann::json nodesJson = root.value("nodes", nlohmann::json::array());
    if (!nodesJson.is_array() || nodesJson.empty())
    {
        return {};
    }

    std::vector<rock::Node> nodes;
    for (const nlohmann::json& nodeJson : nodesJson)
    {
        std::optional<rock::Node> node = ReadSerializedNodeJson(nodeJson);
        if (node)
        {
            nodes.push_back(std::move(*node));
        }
    }
    MigrateRockUniqueMaskPins(nodes);
    return nodes;
}

std::vector<rock::Link> ReadSerializedLinksJson(const nlohmann::json& root, const CanCreateLink& canCreateLink)
{
    std::vector<rock::Link> links;
    if (!root.contains("links") || !root["links"].is_array())
    {
        return links;
    }

    for (const nlohmann::json& linkJson : root["links"])
    {
        rock::Link link;
        link.id = linkJson.value("id", 0);
        link.startPin = linkJson.value("startPin", 0);
        link.endPin = linkJson.value("endPin", 0);
        if (link.id > 0 && canCreateLink(link.startPin, link.endPin))
        {
            links.push_back(link);
        }
    }
    return links;
}

void MigrateRockUniqueMaskPins(std::vector<rock::Node>& nodes)
{
    rock::GraphId nextId = 1;
    for (const rock::Node& node : nodes)
    {
        nextId = std::max(nextId, node.id + 1);
        for (const rock::Pin& pin : node.inputs)
        {
            nextId = std::max(nextId, pin.id + 1);
        }
        for (const rock::Pin& pin : node.outputs)
        {
            nextId = std::max(nextId, pin.id + 1);
        }
    }

    for (rock::Node& node : nodes)
    {
        if (node.kind != rock::NodeKind::Rock)
        {
            continue;
        }
        const bool hasUniqueMask = std::ranges::any_of(node.outputs, [](const rock::Pin& pin) {
            return pin.kind == rock::PinKind::Output &&
                   pin.valueType == rock::ValueType::Mask &&
                   pin.label == "Unique Mask";
        });
        if (hasUniqueMask)
        {
            continue;
        }

        rock::Pin pin;
        pin.id = nextId++;
        pin.nodeId = node.id;
        pin.kind = rock::PinKind::Output;
        pin.valueType = rock::ValueType::Mask;
        pin.label = "Unique Mask";
        node.outputs.push_back(std::move(pin));
    }
}


} // namespace terrain
