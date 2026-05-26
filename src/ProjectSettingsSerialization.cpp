#include "ProjectSettingsSerialization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

namespace terrain
{
namespace
{

constexpr std::array<int, 6> kResolutionPresets = {128, 256, 512, 1024, 2048, 4096};
constexpr std::array<int, 4> kTerrainSizePresets = {512, 1024, 2048, 4096};

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

int NearestTerrainSizePreset(float value)
{
    return NearestPreset(static_cast<int>(std::round(value)), kTerrainSizePresets, 1024);
}

int DaysInMonth(int month)
{
    static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int index = std::clamp(month, 1, 12) - 1;
    return kDays[static_cast<size_t>(index)];
}

} // namespace

nlohmann::json MakeProjectSettingsJson(const rock::GraphSettings& graphSettings)
{
    const rock::PreviewSettings& preview = graphSettings.preview;
    const rock::SkySettings& sky = graphSettings.sky;
    const rock::CloudSettings& clouds = graphSettings.clouds;
    const int displayMode = sky.mode == rock::SkyMode::Atmospheric
        ? 2
        : (preview.lightingMode >= 1 ? 1 : 0);

    return {
        {"display", {
            {"mode", displayMode},
            {"cloudsEnabled", clouds.enabled},
        }},
        {"preview", {
            {"terrainSizeMeters", preview.terrainSizeMeters},
            {"simulationResolution", preview.simulationResolution},
            {"lightingMode", preview.lightingMode},
            {"hdrViewportEnabled", preview.hdrViewportEnabled},
            {"exposureMode", static_cast<int>(preview.exposureMode)},
            {"exposureEv", preview.exposureEv},
            {"autoExposureBiasEv", preview.autoExposureBiasEv},
            {"autoExposureMinEv", preview.autoExposureMinEv},
            {"autoExposureMaxEv", preview.autoExposureMaxEv},
            {"autoExposureSpeed", preview.autoExposureSpeed},
            {"colorTemperatureKelvin", preview.colorTemperatureKelvin},
            {"terrainBoundaryMode", static_cast<int>(preview.terrainBoundaryMode)},
            {"waterEnabled", preview.waterEnabled},
            {"waterLevelMeters", preview.waterLevelMeters},
            {"waterOpacity", preview.waterOpacity},
            {"waterColor", {
                preview.waterColor[0],
                preview.waterColor[1],
                preview.waterColor[2],
            }},
            {"waterWavesScale", preview.waterWavesScale},
            {"waterRefractiveIndex", preview.waterRefractiveIndex},
            {"waterFresnelPower", preview.waterFresnelPower},
            {"waterRefractionStrength", preview.waterRefractionStrength},
            {"waterAnimationEnabled", preview.waterAnimationEnabled},
            {"waterReflectionStrength", preview.waterReflectionStrength},
            {"waterSsrEnabled", preview.waterSsrEnabled},
            {"depthOfFieldEnabled", preview.depthOfFieldEnabled},
            {"dofFStop", preview.dofFStop},
            {"dofFocusDistanceMeters", preview.dofFocusDistanceMeters},
            {"dofSensorHeightMm", preview.dofSensorHeightMm},
            {"dofMaxBlurPixels", preview.dofMaxBlurPixels},
            {"dofApertureShape", preview.dofApertureShape},
            {"dofApertureBlades", preview.dofApertureBlades},
            {"dofApertureRotationDegrees", preview.dofApertureRotationDegrees},
            {"dofHighlightBoost", preview.dofHighlightBoost},
            {"dofMiniatureEnabled", preview.dofMiniatureEnabled},
            {"dofMiniatureScale", preview.dofMiniatureScale},
            {"sunAzimuthDegrees", preview.sunAzimuthDegrees},
            {"sunElevationDegrees", preview.sunElevationDegrees},
            {"sunIntensity", preview.sunIntensity},
            {"ambientStrength", preview.ambientStrength},
            {"shadowStrength", preview.shadowStrength},
            {"shadowBias", preview.shadowBias},
            {"sunDirectionMode", static_cast<int>(preview.sunDirectionMode)},
            {"sunLatitudeDegrees", preview.sunLatitudeDegrees},
            {"sunLongitudeDegrees", preview.sunLongitudeDegrees},
            {"sunUtcOffsetHours", preview.sunUtcOffsetHours},
            {"sunMonth", preview.sunMonth},
            {"sunDay", preview.sunDay},
            {"sunTimeHours", preview.sunTimeHours},
            {"sunTimeAnimate", preview.sunTimeAnimate},
            {"sunTimeDayLengthSeconds", preview.sunTimeDayLengthSeconds},
            {"sunTimeSkipNight", preview.sunTimeSkipNight},
            {"showGrid", preview.showGrid},
            {"gridCellCount", preview.gridCellCount},
            {"gridCellSizeMeters", preview.gridCellSizeMeters},
            {"gridColor", {
                preview.gridColor[0],
                preview.gridColor[1],
                preview.gridColor[2],
            }},
            {"maskPreviewUseNearestHeightmap", preview.maskPreviewUseNearestHeightmap},
            {"aoEnabled", preview.aoEnabled},
            {"aoStrength", preview.aoStrength},
            {"aoRadius", preview.aoRadius},
        }},
        {"sky", {
            {"mode", static_cast<int>(sky.mode)},
            {"atmosphereDensity", sky.atmosphereDensity},
            {"mieStrength", sky.mieStrength},
            {"mieEccentricity", sky.mieEccentricity},
            {"groundAlbedo", {sky.groundAlbedo[0], sky.groundAlbedo[1], sky.groundAlbedo[2]}},
            {"sunSizeDegrees", sky.sunSizeDegrees},
            {"sunGlowStrength", sky.sunGlowStrength},
        }},
        {"clouds", {
            {"enabled", clouds.enabled},
            {"seed", clouds.seed},
            {"coverage", clouds.coverage},
            {"densityMultiplier", clouds.densityMultiplier},
            {"altitudeMin", clouds.altitudeMin},
            {"altitudeMax", clouds.altitudeMax},
            {"horizontalScale", clouds.horizontalScale},
            {"absorption", clouds.absorption},
            {"color", {clouds.color[0], clouds.color[1], clouds.color[2]}},
            {"animate", clouds.animate},
            {"loopPhase", clouds.loopPhase},
            {"windDirectionDegrees", clouds.windDirectionDegrees},
            {"windSpeedMetersPerSec", clouds.windSpeedMetersPerSec},
            {"shadowStrength", clouds.shadowStrength},
            {"fieldRadius", clouds.fieldRadius},
            {"fieldFalloff", clouds.fieldFalloff},
            {"selfShadowEnabled", clouds.selfShadowEnabled},
            {"lightStepMeters", clouds.lightStepMeters},
            {"phaseEccentricity", clouds.phaseEccentricity},
            {"shadowAmbientStrength", clouds.shadowAmbientStrength},
        }},
    };
}

void ReadColor3Json(const nlohmann::json& ownerJson, const char* key, std::array<float, 3>& target, float maxValue)
{
    if (ownerJson.contains(key) && ownerJson[key].is_array() && ownerJson[key].size() == 3)
    {
        target[0] = std::clamp(ownerJson[key][0].get<float>(), 0.0f, maxValue);
        target[1] = std::clamp(ownerJson[key][1].get<float>(), 0.0f, maxValue);
        target[2] = std::clamp(ownerJson[key][2].get<float>(), 0.0f, maxValue);
    }
}

void ReadSkySettingsJson(const nlohmann::json& settingsJson, rock::SkySettings& sky)
{
    const nlohmann::json skyJson = settingsJson.value("sky", nlohmann::json::object());
    sky = rock::SkySettings{};
    if (skyJson.empty())
    {
        return;
    }

    const int skyModeInt = std::clamp(skyJson.value("mode", static_cast<int>(sky.mode)),
                                      static_cast<int>(rock::SkyMode::SolidColor),
                                      static_cast<int>(rock::SkyMode::Atmospheric));
    sky.mode = static_cast<rock::SkyMode>(skyModeInt);
    sky.atmosphereDensity = std::clamp(skyJson.value("atmosphereDensity", sky.atmosphereDensity), 0.05f, 8.0f);
    sky.mieStrength = std::clamp(skyJson.value("mieStrength", sky.mieStrength), 0.0f, 8.0f);
    sky.mieEccentricity = std::clamp(skyJson.value("mieEccentricity", sky.mieEccentricity), -0.99f, 0.99f);
    ReadColor3Json(skyJson, "groundAlbedo", sky.groundAlbedo, 8.0f);
    sky.sunSizeDegrees = std::clamp(skyJson.value("sunSizeDegrees", sky.sunSizeDegrees), 0.1f, 30.0f);
    sky.sunGlowStrength = std::clamp(skyJson.value("sunGlowStrength", sky.sunGlowStrength), 0.0f, 4.0f);
}

void ReadCloudSettingsJson(const nlohmann::json& settingsJson, rock::CloudSettings& clouds)
{
    const nlohmann::json cloudsJson = settingsJson.value("clouds", nlohmann::json::object());
    const int qualitySamples = clouds.qualitySamples;
    const int shadowResolution = clouds.shadowResolution;
    const int shadowSamples = clouds.shadowSamples;
    const int lightSamples = clouds.lightSamples;
    clouds = rock::CloudSettings{};
    clouds.qualitySamples = qualitySamples;
    clouds.shadowResolution = shadowResolution;
    clouds.shadowSamples = shadowSamples;
    clouds.lightSamples = lightSamples;
    if (cloudsJson.empty())
    {
        return;
    }

    clouds.enabled = cloudsJson.value("enabled", clouds.enabled);
    clouds.seed = std::clamp(cloudsJson.value("seed", clouds.seed), 0, 999999);
    clouds.coverage = std::clamp(cloudsJson.value("coverage", clouds.coverage), 0.0f, 1.0f);
    clouds.densityMultiplier = std::clamp(cloudsJson.value("densityMultiplier", clouds.densityMultiplier), 0.0f, 8.0f);
    clouds.altitudeMin = std::clamp(cloudsJson.value("altitudeMin", clouds.altitudeMin), -30000.0f, 30000.0f);
    clouds.altitudeMax = std::clamp(cloudsJson.value("altitudeMax", clouds.altitudeMax), -30000.0f, 30000.0f);
    clouds.horizontalScale = std::clamp(cloudsJson.value("horizontalScale", clouds.horizontalScale), 50.0f, 100000.0f);
    clouds.absorption = std::clamp(cloudsJson.value("absorption", clouds.absorption), 0.0f, 2.0f);
    ReadColor3Json(cloudsJson, "color", clouds.color, 8.0f);
    const bool legacyAnimatedClouds = !cloudsJson.contains("animate") &&
        cloudsJson.value("windSpeedMetersPerSec", clouds.windSpeedMetersPerSec) > 0.0f;
    clouds.animate = cloudsJson.value("animate", legacyAnimatedClouds ? true : clouds.animate);
    clouds.loopPhase = std::clamp(cloudsJson.value("loopPhase", clouds.loopPhase), 0.0f, 1.0f);
    clouds.windDirectionDegrees = std::clamp(cloudsJson.value("windDirectionDegrees", clouds.windDirectionDegrees), 0.0f, 360.0f);
    clouds.windSpeedMetersPerSec = std::clamp(cloudsJson.value("windSpeedMetersPerSec", clouds.windSpeedMetersPerSec), 0.0f, 500.0f);
    clouds.shadowStrength = std::clamp(cloudsJson.value("shadowStrength", clouds.shadowStrength), 0.0f, 1.0f);
    clouds.fieldRadius = std::clamp(cloudsJson.value("fieldRadius", clouds.fieldRadius), 100.0f, 200000.0f);
    clouds.fieldFalloff = std::clamp(cloudsJson.value("fieldFalloff", clouds.fieldFalloff), 1.0f, 50000.0f);
    clouds.selfShadowEnabled = cloudsJson.value("selfShadowEnabled", clouds.lightSamples > 0);
    clouds.lightStepMeters = std::clamp(cloudsJson.value("lightStepMeters", clouds.lightStepMeters), 1.0f, 2000.0f);
    clouds.phaseEccentricity = std::clamp(cloudsJson.value("phaseEccentricity", clouds.phaseEccentricity), -0.99f, 0.99f);
    clouds.shadowAmbientStrength = std::clamp(cloudsJson.value("shadowAmbientStrength", clouds.shadowAmbientStrength), 0.0f, 2.0f);
}

void ReadPreviewSettingsJson(const nlohmann::json& settingsJson, rock::PreviewSettings& preview, const rock::SkySettings& sky, bool& hadSimulationResolution)
{
    const nlohmann::json previewJson = settingsJson.value("preview", nlohmann::json::object());
    if (previewJson.empty())
    {
        if (sky.mode == rock::SkyMode::Atmospheric)
        {
            preview.lightingMode = 1;
        }
        return;
    }

    preview.terrainSizeMeters = static_cast<float>(NearestTerrainSizePreset(previewJson.value("terrainSizeMeters", preview.terrainSizeMeters)));
    preview.simulationResolution = NearestResolutionPreset(previewJson.value("simulationResolution", preview.simulationResolution));
    hadSimulationResolution = previewJson.contains("simulationResolution");
    preview.lightingMode = std::clamp(previewJson.value("lightingMode", preview.lightingMode), 0, 1);
    preview.hdrViewportEnabled = previewJson.value("hdrViewportEnabled", preview.hdrViewportEnabled);
    {
        const int exposureModeInt = std::clamp(previewJson.value("exposureMode", static_cast<int>(preview.exposureMode)),
            static_cast<int>(rock::ExposureMode::Manual),
            static_cast<int>(rock::ExposureMode::Auto));
        preview.exposureMode = static_cast<rock::ExposureMode>(exposureModeInt);
    }
    preview.exposureEv = std::clamp(previewJson.value("exposureEv", preview.exposureEv), -8.0f, 8.0f);
    preview.autoExposureBiasEv = std::clamp(previewJson.value("autoExposureBiasEv", preview.autoExposureBiasEv), -4.0f, 4.0f);
    preview.autoExposureMinEv = std::clamp(previewJson.value("autoExposureMinEv", preview.autoExposureMinEv), -8.0f, 8.0f);
    preview.autoExposureMaxEv = std::clamp(previewJson.value("autoExposureMaxEv", preview.autoExposureMaxEv), preview.autoExposureMinEv, 8.0f);
    preview.autoExposureSpeed = std::clamp(previewJson.value("autoExposureSpeed", preview.autoExposureSpeed), 0.05f, 8.0f);
    preview.colorTemperatureKelvin = std::clamp(previewJson.value("colorTemperatureKelvin", preview.colorTemperatureKelvin), 2000.0f, 12000.0f);
    const int boundaryInt = std::clamp(previewJson.value("terrainBoundaryMode", static_cast<int>(preview.terrainBoundaryMode)),
                                       static_cast<int>(rock::TerrainBoundaryMode::None),
                                       static_cast<int>(rock::TerrainBoundaryMode::Lines));
    preview.terrainBoundaryMode = static_cast<rock::TerrainBoundaryMode>(boundaryInt);
    preview.waterEnabled = previewJson.value("waterEnabled", preview.waterEnabled);
    preview.waterLevelMeters = std::clamp(previewJson.value("waterLevelMeters", preview.waterLevelMeters), 0.0f, 10000.0f);
    preview.waterOpacity = std::clamp(previewJson.value("waterOpacity", preview.waterOpacity), 0.0f, 1.0f);
    ReadColor3Json(previewJson, "waterColor", preview.waterColor, 1.0f);
    preview.waterWavesScale = std::clamp(previewJson.value("waterWavesScale", preview.waterWavesScale), 1.0f, 500.0f);
    preview.waterRefractiveIndex = std::clamp(previewJson.value("waterRefractiveIndex", preview.waterRefractiveIndex), 1.0f, 4.0f);
    preview.waterFresnelPower = std::clamp(previewJson.value("waterFresnelPower", preview.waterFresnelPower), 1.0f, 8.0f);
    preview.waterRefractionStrength = std::clamp(previewJson.value("waterRefractionStrength", preview.waterRefractionStrength), 0.0f, 2.0f);
    preview.waterAnimationEnabled = previewJson.value("waterAnimationEnabled", preview.waterAnimationEnabled);
    preview.waterReflectionStrength = std::clamp(previewJson.value("waterReflectionStrength", preview.waterReflectionStrength), 0.0f, 3.0f);
    preview.waterSsrEnabled = previewJson.value("waterSsrEnabled", preview.waterSsrEnabled);
    preview.depthOfFieldEnabled = previewJson.value("depthOfFieldEnabled", preview.depthOfFieldEnabled);
    preview.dofFStop = std::clamp(previewJson.value("dofFStop", preview.dofFStop), 0.7f, 32.0f);
    preview.dofFocusDistanceMeters = std::clamp(previewJson.value("dofFocusDistanceMeters", preview.dofFocusDistanceMeters), 0.1f, 20000.0f);
    preview.dofSensorHeightMm = std::clamp(previewJson.value("dofSensorHeightMm", preview.dofSensorHeightMm), 4.0f, 80.0f);
    preview.dofMaxBlurPixels = std::clamp(previewJson.value("dofMaxBlurPixels", preview.dofMaxBlurPixels), 0.0f, 64.0f);
    preview.dofApertureShape = std::clamp(previewJson.value("dofApertureShape", preview.dofApertureShape), 0, 4);
    preview.dofApertureBlades = std::clamp(previewJson.value("dofApertureBlades", preview.dofApertureBlades), 3, 12);
    preview.dofApertureRotationDegrees = std::clamp(previewJson.value("dofApertureRotationDegrees", preview.dofApertureRotationDegrees), -180.0f, 180.0f);
    preview.dofHighlightBoost = std::clamp(previewJson.value("dofHighlightBoost", preview.dofHighlightBoost), 0.0f, 4.0f);
    preview.dofMiniatureEnabled = previewJson.value("dofMiniatureEnabled", preview.dofMiniatureEnabled);
    preview.dofMiniatureScale = std::clamp(previewJson.value("dofMiniatureScale", preview.dofMiniatureScale), 1.0f, 50.0f);
    preview.sunAzimuthDegrees = std::clamp(previewJson.value("sunAzimuthDegrees", preview.sunAzimuthDegrees), 0.0f, 360.0f);
    preview.sunElevationDegrees = std::clamp(previewJson.value("sunElevationDegrees", preview.sunElevationDegrees), -10.0f, 89.0f);
    preview.sunIntensity = std::clamp(previewJson.value("sunIntensity", preview.sunIntensity), 0.0f, 5.0f);
    preview.ambientStrength = std::clamp(previewJson.value("ambientStrength", preview.ambientStrength), 0.0f, 2.0f);
    preview.shadowStrength = std::clamp(previewJson.value("shadowStrength", preview.shadowStrength), 0.0f, 1.0f);
    preview.shadowBias = std::clamp(previewJson.value("shadowBias", preview.shadowBias), 0.0f, 0.05f);
    {
        const int sunModeInt = std::clamp(previewJson.value("sunDirectionMode", static_cast<int>(preview.sunDirectionMode)),
            static_cast<int>(rock::SunDirectionMode::Manual),
            static_cast<int>(rock::SunDirectionMode::DateTime));
        preview.sunDirectionMode = static_cast<rock::SunDirectionMode>(sunModeInt);
    }
    preview.sunLatitudeDegrees = std::clamp(previewJson.value("sunLatitudeDegrees", preview.sunLatitudeDegrees), -90.0f, 90.0f);
    preview.sunLongitudeDegrees = std::clamp(previewJson.value("sunLongitudeDegrees", preview.sunLongitudeDegrees), -180.0f, 180.0f);
    preview.sunUtcOffsetHours = std::clamp(previewJson.value("sunUtcOffsetHours", preview.sunUtcOffsetHours), -12.0f, 14.0f);
    preview.sunMonth = std::clamp(previewJson.value("sunMonth", preview.sunMonth), 1, 12);
    preview.sunDay = std::clamp(previewJson.value("sunDay", preview.sunDay), 1, DaysInMonth(preview.sunMonth));
    preview.sunTimeHours = std::clamp(previewJson.value("sunTimeHours", preview.sunTimeHours), 0.0f, 24.0f);
    preview.sunTimeAnimate = previewJson.value("sunTimeAnimate", preview.sunTimeAnimate);
    preview.sunTimeDayLengthSeconds = std::clamp(previewJson.value("sunTimeDayLengthSeconds", preview.sunTimeDayLengthSeconds), 5.0f, 3600.0f);
    preview.sunTimeSkipNight = previewJson.value("sunTimeSkipNight", preview.sunTimeSkipNight);
    preview.showGrid = previewJson.value("showGrid", preview.showGrid);
    preview.gridCellCount = std::clamp(previewJson.value("gridCellCount", preview.gridCellCount), 1, 200);
    preview.gridCellSizeMeters = std::clamp(previewJson.value("gridCellSizeMeters", preview.gridCellSizeMeters), 1.0f, 10000.0f);
    ReadColor3Json(previewJson, "gridColor", preview.gridColor, 1.0f);
    preview.maskPreviewUseNearestHeightmap = previewJson.value("maskPreviewUseNearestHeightmap", preview.maskPreviewUseNearestHeightmap);
    preview.aoEnabled = previewJson.value("aoEnabled", preview.aoEnabled);
    preview.aoStrength = std::clamp(previewJson.value("aoStrength", preview.aoStrength), 0.0f, 1.0f);
    preview.aoRadius = std::clamp(previewJson.value("aoRadius", preview.aoRadius), 10.0f, 5000.0f);
}

void ReadDisplaySettingsJson(const nlohmann::json& settingsJson,
                             rock::PreviewSettings& preview,
                             rock::SkySettings& sky,
                             rock::CloudSettings& clouds)
{
    const nlohmann::json displayJson = settingsJson.value("display", nlohmann::json::object());
    if (displayJson.empty())
    {
        return;
    }

    clouds.enabled = displayJson.value("cloudsEnabled", clouds.enabled);
    const int displayMode = std::clamp(displayJson.value("mode", -1), -1, 2);
    if (displayMode == 0)
    {
        preview.lightingMode = 0;
        sky.mode = rock::SkyMode::SolidColor;
    }
    else if (displayMode == 1)
    {
        preview.lightingMode = 1;
        sky.mode = rock::SkyMode::SolidColor;
    }
    else if (displayMode == 2)
    {
        preview.lightingMode = 1;
        sky.mode = rock::SkyMode::Atmospheric;
    }
}

bool ReadProjectSettingsJson(const nlohmann::json& root, rock::GraphSettings& graphSettings)
{
    const nlohmann::json settingsJson = root.value("settings", nlohmann::json::object());
    rock::PreviewSettings& preview = graphSettings.preview;
    rock::SkySettings& sky = graphSettings.sky;
    rock::CloudSettings& clouds = graphSettings.clouds;

    bool hadSimulationResolution = false;
    ReadSkySettingsJson(settingsJson, sky);
    ReadCloudSettingsJson(settingsJson, clouds);
    ReadPreviewSettingsJson(settingsJson, preview, sky, hadSimulationResolution);
    ReadDisplaySettingsJson(settingsJson, preview, sky, clouds);
    return hadSimulationResolution;
}

} // namespace terrain
