#include "SkyPanel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include <imgui.h>

#include "Localization.h"
#include "PropertyWidgets.h"

namespace terrain::ui
{
namespace
{
constexpr float kDegreesToRadians = 3.1415926535f / 180.0f;

std::string StableImGuiLabel(const char* label, const char* stableId)
{
    std::string stableLabel = label != nullptr ? label : "";
    if (stableId != nullptr && stableId[0] != '\0')
    {
        stableLabel += "###";
        stableLabel += stableId;
    }
    return stableLabel;
}

void SaveAppSettings(const SkyPanelState& state)
{
    if (state.saveAppSettings)
    {
        state.saveAppSettings();
    }
}

int DaysInMonth(int month)
{
    static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int clampedMonth = std::clamp(month, 1, 12);
    return kDays[static_cast<size_t>(clampedMonth - 1)];
}

int DayOfYear(int month, int day)
{
    int total = 0;
    const int clampedMonth = std::clamp(month, 1, 12);
    for (int m = 1; m < clampedMonth; ++m)
    {
        total += DaysInMonth(m);
    }
    return total + std::clamp(day, 1, DaysInMonth(clampedMonth));
}

float NormalizeDegrees(float degrees)
{
    float normalized = std::fmod(degrees, 360.0f);
    if (normalized < 0.0f)
    {
        normalized += 360.0f;
    }
    return normalized;
}

SkyPanelSunPosition ComputeDateTimeSunPosition(const rock::PreviewSettings& preview)
{
    const float latitude = std::clamp(preview.sunLatitudeDegrees, -90.0f, 90.0f) * kDegreesToRadians;
    const float longitude = std::clamp(preview.sunLongitudeDegrees, -180.0f, 180.0f);
    const float utcOffset = std::clamp(preview.sunUtcOffsetHours, -12.0f, 14.0f);
    const int dayOfYear = DayOfYear(preview.sunMonth, preview.sunDay);
    const float localHours = std::clamp(preview.sunTimeHours, 0.0f, 24.0f);
    const float fractionalYear = (2.0f * 3.1415926535f / 365.0f) *
        (static_cast<float>(dayOfYear - 1) + (localHours - 12.0f) / 24.0f);

    const float equationOfTime = 229.18f * (
        0.000075f +
        0.001868f * std::cos(fractionalYear) -
        0.032077f * std::sin(fractionalYear) -
        0.014615f * std::cos(2.0f * fractionalYear) -
        0.040849f * std::sin(2.0f * fractionalYear));
    const float declination =
        0.006918f -
        0.399912f * std::cos(fractionalYear) +
        0.070257f * std::sin(fractionalYear) -
        0.006758f * std::cos(2.0f * fractionalYear) +
        0.000907f * std::sin(2.0f * fractionalYear) -
        0.002697f * std::cos(3.0f * fractionalYear) +
        0.00148f * std::sin(3.0f * fractionalYear);

    float trueSolarMinutes = localHours * 60.0f + equationOfTime + 4.0f * longitude - 60.0f * utcOffset;
    trueSolarMinutes = std::fmod(trueSolarMinutes, 1440.0f);
    if (trueSolarMinutes < 0.0f)
    {
        trueSolarMinutes += 1440.0f;
    }

    float hourAngleDegrees = trueSolarMinutes / 4.0f - 180.0f;
    if (hourAngleDegrees < -180.0f)
    {
        hourAngleDegrees += 360.0f;
    }
    const float hourAngle = hourAngleDegrees * kDegreesToRadians;

    const float cosZenith = std::clamp(
        std::sin(latitude) * std::sin(declination) +
        std::cos(latitude) * std::cos(declination) * std::cos(hourAngle),
        -1.0f,
        1.0f);
    const float zenith = std::acos(cosZenith);
    const float elevation = 90.0f - zenith / kDegreesToRadians;

    const float northClockwiseAzimuth = NormalizeDegrees(
        std::atan2(
            std::sin(hourAngle),
            std::cos(hourAngle) * std::sin(latitude) - std::tan(declination) * std::cos(latitude)) /
        kDegreesToRadians + 180.0f);

    return {
        NormalizeDegrees(180.0f - northClockwiseAzimuth),
        std::clamp(elevation, -90.0f, 90.0f),
    };
}

SkyPanelSunPosition EffectiveSunPosition(const rock::PreviewSettings& preview)
{
    if (preview.sunDirectionMode == rock::SunDirectionMode::DateTime)
    {
        return ComputeDateTimeSunPosition(preview);
    }
    return {
        NormalizeDegrees(preview.sunAzimuthDegrees),
        std::clamp(preview.sunElevationDegrees, -90.0f, 90.0f),
    };
}
} // namespace
void DrawSkySettingsPanel(SkyPanelState state)
{
    rock::GraphSettings& settings = state.settings;
    const float headerRightPadding = 10.0f;
    const float sectionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - headerRightPadding);
    ImGui::BeginChild("SkySettingsSection", ImVec2(sectionWidth, 0.0f), false);

    const std::string sunHeaderLabel = StableImGuiLabel(Tr("Sun and Shadows", "太陽と影"), "SkySunAndShadowsHeader");
    if (ImGui::CollapsingHeader(sunHeaderLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("SunShadowSettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::SeparatorText(Tr("Sun", "太陽"));
            int sunModeInt = static_cast<int>(settings.preview.sunDirectionMode);
            if (DrawPropertyComboRow("Sun Mode", "DisplaySunMode", &sunModeInt, Tr("Manual\0Date Time\0\0", "Manual\0Date Time\0\0"),
                Tr("Manual directly sets azimuth and elevation. Date Time estimates the sun position from latitude, longitude, date, time, and UTC Offset.",
                    "Manual は方位角と高度を直接指定します。Date Time は緯度、経度、月日、時刻、UTC Offset からそれらしい太陽位置を計算します。"),
                static_cast<int>(rock::PreviewSettings{}.sunDirectionMode)))
            {
                settings.preview.sunDirectionMode = static_cast<rock::SunDirectionMode>(std::clamp(sunModeInt,
                    static_cast<int>(rock::SunDirectionMode::Manual),
                    static_cast<int>(rock::SunDirectionMode::DateTime)));
                SaveAppSettings(state);
            }
            if (settings.preview.sunDirectionMode == rock::SunDirectionMode::DateTime)
            {
                if (DrawPropertyFloatRow("Latitude", "SunLatitude", &settings.preview.sunLatitudeDegrees, -90.0f, 90.0f, rock::PreviewSettings{}.sunLatitudeDegrees, "Sun latitude changed", false,
                    Tr("Latitude used for sun position calculation. North is positive; south is negative.",
                        "太陽位置計算に使う緯度です。北緯を正、南緯を負で指定します。")))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Longitude", "SunLongitude", &settings.preview.sunLongitudeDegrees, -180.0f, 180.0f, rock::PreviewSettings{}.sunLongitudeDegrees, "Sun longitude changed", false,
                    Tr("Longitude used for sun position calculation. East is positive; west is negative.",
                        "太陽位置計算に使う経度です。東経を正、西経を負で指定します。")))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("UTC Offset", "SunUtcOffset", &settings.preview.sunUtcOffsetHours, -12.0f, 14.0f, rock::PreviewSettings{}.sunUtcOffsetHours, "Sun UTC offset changed", false,
                    Tr("Offset from UTC used to interpret the date and time. Daylight saving time and timezone databases are not used; this value is applied directly.",
                        "日時の解釈に使う UTC からの時差です。夏時間やタイムゾーンDBは使わず、ここで指定した値をそのまま使います。"), "%.1f"))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyIntRow("Month", "SunMonth", &settings.preview.sunMonth, 1, 12, rock::PreviewSettings{}.sunMonth, "Sun month changed", false,
                    Tr("Month used for sun position calculation. The year is treated as a fixed non-leap year.",
                        "太陽位置計算に使う月です。年は固定の非うるう年として扱います。")))
                {
                    settings.preview.sunDay = std::clamp(settings.preview.sunDay, 1, DaysInMonth(settings.preview.sunMonth));
                    SaveAppSettings(state);
                }
                const int maxDay = DaysInMonth(settings.preview.sunMonth);
                if (DrawPropertyIntRow("Day", "SunDay", &settings.preview.sunDay, 1, maxDay, std::clamp(rock::PreviewSettings{}.sunDay, 1, maxDay), "Sun day changed", false,
                    Tr("Day used for sun position calculation. The maximum day is limited by the selected month.",
                        "太陽位置計算に使う日です。月に応じて最大日数を制限します。")))
                {
                    settings.preview.sunDay = std::clamp(settings.preview.sunDay, 1, maxDay);
                    SaveAppSettings(state);
                }
                if (DrawTimeOfDayRow("Time", "SunTime", &settings.preview.sunTimeHours, rock::PreviewSettings{}.sunTimeHours, "Sun time changed",
                    Tr("Local time. The slider covers 0:00 to 24:00.",
                        "ローカル時刻です。0:00 から 24:00 までをスライダーで指定します。")))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyBoolRow(Tr("Animate Time", "時間を進める"), "SunTimeAnimate", &settings.preview.sunTimeAnimate, "Sun time animation toggled",
                    Tr("Automatically advances Date Time while the app is running.",
                        "アプリ実行中に Date Time の時刻を自動で進めます。"),
                    rock::PreviewSettings{}.sunTimeAnimate, true))
                {
                    SaveAppSettings(state);
                }
                ImGui::BeginDisabled(!settings.preview.sunTimeAnimate);
                if (DrawPropertyFloatRow(Tr("Day Length (sec)", "1日の長さ (sec)"), "SunTimeDayLength", &settings.preview.sunTimeDayLengthSeconds, 5.0f, 600.0f, rock::PreviewSettings{}.sunTimeDayLengthSeconds, "Sun time day length changed", false,
                    Tr("Real seconds for one full 24-hour Date Time cycle. Smaller values move the sun faster.",
                        "Date Time の 24 時間を何秒で一周させるかです。小さいほど太陽が速く動きます。"), "%.1f", ImGuiSliderFlags_Logarithmic, 5.0f, 3600.0f))
                {
                    settings.preview.sunTimeDayLengthSeconds = std::clamp(settings.preview.sunTimeDayLengthSeconds, 5.0f, 3600.0f);
                    SaveAppSettings(state);
                }
                if (DrawPropertyBoolRow(Tr("Skip Night", "夜をスキップ"), "SunTimeSkipNight", &settings.preview.sunTimeSkipNight, "Sun time skip night toggled",
                    Tr("When the sun drops below -5 degrees, jump forward to the next time it rises back to -5 degrees.",
                        "太陽高度が -5 度を下回ったら、次に -5 度へ戻る時刻まで早送りします。"),
                    rock::PreviewSettings{}.sunTimeSkipNight, true))
                {
                    SaveAppSettings(state);
                }
                ImGui::EndDisabled();

                const SkyPanelSunPosition computedSun = EffectiveSunPosition(settings.preview);
                DrawReadOnlyFloatRow("Computed Azimuth", computedSun.azimuth, "%.2f",
                    Tr("Calculated app-space azimuth. 0 deg is south (Z+), and 90 deg is east (X+).",
                        "計算されたアプリ内方位角です。0° が南(Z+)、90° が東(X+)です。"));
                DrawReadOnlyFloatRow("Computed Elevation", computedSun.elevation, "%.2f",
                    Tr("Calculated sun elevation.",
                        "計算された太陽高度です。"));
            }
            else
            {
                if (DrawPropertyFloatRow("Sun Azimuth (deg)", "DisplaySunAzimuth", &settings.preview.sunAzimuthDegrees, 0.0f, 360.0f, rock::PreviewSettings{}.sunAzimuthDegrees, "Sun azimuth changed", false,
                    Tr("Horizontal sun angle. 0 deg is south (Z+), and 90 deg is east (X+). Rotate it to make terrain grooves easier to read.",
                        "太陽の水平角度です。0° が南(Z+)、90° が東(X+)です。地形の溝が読みやすい方向へ回せます。")))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Sun Elevation (deg)", "DisplaySunElevation", &settings.preview.sunElevationDegrees, -10.0f, 89.0f, rock::PreviewSettings{}.sunElevationDegrees, "Sun elevation changed", false,
                    Tr("Sun height. Lower values cast longer shadows and emphasize relief. 0 deg is the horizon; negative values place the sun below the horizon for checking night transitions.",
                        "太陽の高さです。低いほど影が長く、凹凸が強調されます。0° は地平線、負値は地平より下 (夜遷移の確認用)。")))
                {
                    SaveAppSettings(state);
                }
            }
            if (DrawPropertyFloatRow("Sun Intensity", "DisplaySunIntensity", &settings.preview.sunIntensity, 0.0f, 5.0f, rock::PreviewSettings{}.sunIntensity, "Sun intensity changed", false,
                Tr("Strength of direct sunlight.",
                    "直射光の強さです。")))
            {
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Ambient", "DisplayAmbientStrength", &settings.preview.ambientStrength, 0.0f, 2.0f, rock::PreviewSettings{}.ambientStrength, "Ambient strength changed", false,
                Tr("Ambient light strength that lifts the shadow side.",
                    "影側を持ち上げる環境光の強さです。")))
            {
                SaveAppSettings(state);
            }
            ImGui::SeparatorText(Tr("Shadows", "影"));
            if (DrawPropertyFloatRow("Shadow Strength", "DisplayShadowStrength", &settings.preview.shadowStrength, 0.0f, 1.0f, rock::PreviewSettings{}.shadowStrength, "Shadow strength changed", false,
                Tr("Darkness of shadows cast by the shadow map.",
                    "シャドウマップで落ちる影の濃さです。")))
            {
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Shadow Bias", "DisplayShadowBias", &settings.preview.shadowBias, 0.0f, 0.05f, rock::PreviewSettings{}.shadowBias, "Shadow bias changed", false,
                Tr("Depth offset used to reduce shadow bleeding and striping. Too much bias makes shadows look detached.",
                    "影のにじみや縞を抑えるための深度オフセットです。大きすぎると影が浮いて見えます。")))
            {
                SaveAppSettings(state);
            }

            ImGui::EndTable();
        }
    }

    const std::string skyHeaderLabel = StableImGuiLabel(Tr("Sky", "天球"), "SkyAtmosphereHeader");
    if (!ImGui::CollapsingHeader(skyHeaderLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("SkySettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        rock::SkySettings& sky = settings.sky;
            ImGui::SeparatorText(Tr("Atmospheric Scattering", "大気散乱"));
            DrawPropertyFloatRow(Tr("Atmosphere Density", "大気厚み (密度)"), "SkyAtmosphereDensity", &sky.atmosphereDensity, 0.05f, 5.0f, rock::SkySettings{}.atmosphereDensity, "Sky atmosphere density changed", false,
                Tr("Multiplier for the Rayleigh scattering coefficient beta_R and horizon haze. 1.0 is Earth-like, 0.5 is thin air, and 2-3 is dense air. Distant terrain fog is also derived from this value.",
                    "Rayleigh 散乱係数 β_R と地平ヘイズの倍率。1.0 が地球標準、0.5 で薄い大気、2-3 で濃い大気。地形の遠景フォグもこの値から自動で決まります。"));
            DrawPropertyFloatRow(Tr("Haze (Mie Strength)", "ヘイズ (Mie 強度)"), "SkyMieStrength", &sky.mieStrength, 0.0f, 8.0f, rock::SkySettings{}.mieStrength, "Sky mie strength changed", false,
                Tr("Strength of Mie scattering. Around 0.2 is a good editing-view default. Higher values strengthen haze and glow toward the sun, but can also make warm horizon bands more visible. 0 is pure Rayleigh.",
                    "Mie 散乱の強さ。0.2 前後が編集ビュー向けの標準です。大きいほど太陽方向の霞とグローが強くなりますが、地平の暖色帯も出やすくなります。0 で純 Rayleigh。"));
            DrawPropertyFloatRow(Tr("Mie Anisotropy (g)", "Mie 偏向 (g)"), "SkyMieG", &sky.mieEccentricity, -0.95f, 0.95f, rock::SkySettings{}.mieEccentricity, "Sky mie g changed", false,
                Tr("Henyey-Greenstein g value. 0 is isotropic scattering; positive values strengthen forward scattering toward the sun and concentrate the glow around it. 0.7-0.85 is realistic.",
                    "Henyey-Greenstein g 値。0 で等方散乱、正で前方 (太陽方向) 散乱が強くなりグローが太陽周りに集中。0.7-0.85 が現実的。"));
            DrawColorRgbRow(Tr("Ground Albedo", "地面アルベド"), "SkyGroundAlbedo", sky.groundAlbedo, rock::SkySettings{}.groundAlbedo);
            DrawPropertyFloatRow(Tr("Sun Size (deg)", "太陽サイズ (deg)"), "SkySunSize", &sky.sunSizeDegrees, 0.1f, 20.0f, rock::SkySettings{}.sunSizeDegrees, "Sky sun size changed", false,
                Tr("Diameter of the sun disk in degrees. The real sun is about 0.5 degrees, but the default is slightly larger for visibility.",
                    "太陽ディスクの直径(度)。実際の太陽は約 0.5 度ですが、視認性のためデフォルトはやや大きめです。"));
            DrawPropertyFloatRow(Tr("Sun Glow", "太陽グロー"), "SkySunGlow", &sky.sunGlowStrength, 0.0f, 2.0f, rock::SkySettings{}.sunGlowStrength, "Sky sun glow changed", false,
                Tr("Strength of the soft glow around the sun. 0 disables the glow.",
                    "太陽周辺の柔らかい光の強さ。0 でグロー無し。"));

        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void DrawCloudSettingsPanel(SkyPanelState state)
{
    rock::GraphSettings& settings = state.settings;
    const float headerRightPadding = 10.0f;
    const float sectionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - headerRightPadding);
    ImGui::BeginChild("CloudSettingsSection", ImVec2(sectionWidth, 0.0f), false);

    if (ImGui::BeginTable("CloudSettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::SeparatorText(Tr("Volumetric Clouds", "ボリューム雲"));
            rock::CloudSettings& clouds = settings.clouds;
            DrawPropertyBoolRow(Tr("Enabled", "有効"), "CloudEnabled", &clouds.enabled, "Clouds enabled toggled",
                Tr("Enables raymarched volumetric clouds. A 3D density texture (128^3 R8 = 2 MB) is generated, and each frame a fullscreen pass integrates ray intersections with the cloud layer at the specified altitude and thickness.",
                    "ボリューム雲のレイマーチ描画を有効化します。3D 密度テクスチャ (128³ R8 = 2MB) を生成し、指定した高度と厚みの雲帯とのレイ交差をフルスクリーンパスで毎フレーム積分します。"),
                rock::CloudSettings{}.enabled, true);
            ImGui::BeginDisabled(!clouds.enabled);
                DrawPropertyIntRow("Cloud Seed", "CloudSeed", &clouds.seed, 0, 999999, rock::CloudSettings{}.seed, "Cloud seed changed", false,
                    Tr("Seed for the 3D density noise. Changing it regenerates the texture and changes the cloud pattern.",
                        "3D 密度ノイズのシード。変更すると雲のパターンが変わります(テクスチャを再生成)。"));
                DrawPropertyFloatRow("Coverage", "CloudCoverage", &clouds.coverage, 0.0f, 1.0f, rock::CloudSettings{}.coverage, "Cloud coverage changed", false,
                    Tr("Fraction of the sky covered by clouds. 0 means no clouds; 1 fills the sky.",
                        "空に占める雲の割合。0 で雲無し、1 で空一面が雲。"));
                DrawPropertyFloatRow("Density", "CloudDensity", &clouds.densityMultiplier, 0.0f, 4.0f, rock::CloudSettings{}.densityMultiplier, "Cloud density changed", false,
                    Tr("Cloud density multiplier. Larger values make clouds more opaque.",
                        "雲の濃さ倍率。大きいほど雲が不透明になります。"));
                clouds.altitudeMin = std::clamp(clouds.altitudeMin, -30000.0f, 30000.0f);
                clouds.altitudeMax = std::clamp(clouds.altitudeMax, clouds.altitudeMin + 1.0f, 30000.0f);
                const float thicknessBeforeAltitude = std::max(1.0f, clouds.altitudeMax - clouds.altitudeMin);
                const float altitudeBeforeEdit = clouds.altitudeMin;
                const bool altitudeEditEnded = DrawPropertyFloatRow("Altitude (m)", "CloudAltitude", &clouds.altitudeMin, -5000.0f, 12000.0f, rock::CloudSettings{}.altitudeMin, "Cloud altitude changed", false,
                    Tr("Lower altitude of the cloud layer. Moves the entire layer up or down while preserving thickness. Negative values allow the layer to start below the terrain reference height.",
                        "雲帯の下限高度です。厚みを保ったまま雲全体を上下に移動します。負の値にすると地形の基準高度より下から雲帯を始められます。"), "%.0f", 0, -30000.0f, 30000.0f);
                if (clouds.altitudeMin != altitudeBeforeEdit)
                {
                    clouds.altitudeMin = std::clamp(clouds.altitudeMin, -30000.0f, 30000.0f - thicknessBeforeAltitude);
                    clouds.altitudeMax = clouds.altitudeMin + thicknessBeforeAltitude;
                }
                if (altitudeEditEnded)
                {
                    SaveAppSettings(state);
                }

                float cloudThickness = std::max(1.0f, clouds.altitudeMax - clouds.altitudeMin);
                const float thicknessBeforeEdit = cloudThickness;
                const bool thicknessEditEnded = DrawPropertyFloatRow("Thickness (m)", "CloudThickness", &cloudThickness, 1.0f, 8000.0f, rock::CloudSettings{}.altitudeMax - rock::CloudSettings{}.altitudeMin, "Cloud thickness changed", false,
                    Tr("Thickness of the cloud layer. The upper altitude is calculated as Altitude + Thickness.",
                        "雲帯の厚みです。上限高度は Altitude + Thickness として計算します。"), "%.0f");
                if (cloudThickness != thicknessBeforeEdit)
                {
                    cloudThickness = std::clamp(cloudThickness, 1.0f, 30000.0f - clouds.altitudeMin);
                    clouds.altitudeMax = clouds.altitudeMin + cloudThickness;
                }
                if (thicknessEditEnded)
                {
                    SaveAppSettings(state);
                }
                DrawPropertyFloatRow("Horizontal Scale (m)", "CloudHorizScale", &clouds.horizontalScale, 200.0f, 30000.0f, rock::CloudSettings{}.horizontalScale, "Cloud scale changed", false,
                    Tr("Horizontal cloud scale. Larger values create bigger cloud masses; smaller values create finer clouds.",
                        "雲の水平スケール。大きいほど雲塊が大きく、小さいほど細かい雲になります。"), "%.0f");
                DrawPropertyFloatRow("Field Radius (m)", "CloudFieldRadius", &clouds.fieldRadius, 200.0f, 50000.0f, rock::CloudSettings{}.fieldRadius, "Cloud field radius changed", false,
                    Tr("Radius of the circular cloud field centered on the terrain. Larger values spread clouds farther away; values near the terrain size keep clouds around the terrain.",
                        "地形の中心を原点にした、雲が存在する円形フィールドの半径。大きくするとより遠くまで雲が広がります。地形と同程度にすると地形の周りだけに雲が出ます。"), "%.0f");
                DrawPropertyFloatRow("Field Falloff (m)", "CloudFieldFalloff", &clouds.fieldFalloff, 50.0f, 20000.0f, rock::CloudSettings{}.fieldFalloff, "Cloud field falloff changed", false,
                    Tr("Fade-out width at the field edge. Larger values make clouds fade gradually; smaller values make the boundary sharper.",
                        "フィールド端のフェードアウト幅。大きいほど雲がじわっと消え、小さいと境界がくっきりします。"), "%.0f");
                DrawPropertyFloatRow("Absorption", "CloudAbsorption", &clouds.absorption, 0.0f, 0.5f, rock::CloudSettings{}.absorption, "Cloud absorption changed", false,
                    Tr("Beer-Lambert absorption coefficient. Larger values make clouds more distinctly opaque.",
                        "Beer-Lambert の吸収係数。大きいほど雲がはっきり不透明になります。"), "%.4f");
                DrawColorRgbRow("Cloud Color", "CloudColor", clouds.color, rock::CloudSettings{}.color);
                DrawPropertyBoolRow(Tr("Animate Clouds", "雲を動かす"), "CloudAnimate", &clouds.animate, "Cloud animation toggled",
                    Tr("When ON, wind direction and speed move the clouds. When OFF, speed settings are kept but the display stays still.",
                        "ON のときだけ風向きと速度を使って雲を流します。OFF では速度の設定値を保持したまま静止表示します。"), rock::CloudSettings{}.animate, true);
                DrawPropertyPercentRow("Loop Phase (%)", "CloudLoopPhase", &clouds.loopPhase, 0.0f, 1.0f, rock::CloudSettings{}.loopPhase, "Cloud loop phase changed",
                    Tr("Position within one full cloud tile loop. 0% and 100% are the same position. When clouds are animated, the phase advances using Wind Speed and Horizontal Scale along the loop direction nearest Wind Direction.",
                        "雲タイル一周の中でどの位置を表示するかです。0% と 100% は同じ位置で、雲を動かすと Wind Direction に近いループ方向を Wind Speed と Horizontal Scale から計算した速度で進みます。"));
                if (clouds.animate)
                {
                    DrawPropertyFloatRow("Wind Speed (m/s)", "CloudWindSpeed", &clouds.windSpeedMetersPerSec, 0.0f, 200.0f, rock::CloudSettings{}.windSpeedMetersPerSec, "Cloud wind speed changed", false,
                        Tr("Cloud movement speed in meters per second. Animation redraws the viewport each frame and increases load.",
                            "雲が流れる速度 (m/s)。動かすとフレーム毎にビューポートが再描画され負荷が増えます。"));
                    DrawPropertyFloatRow("Wind Direction (deg)", "CloudWindDir", &clouds.windDirectionDegrees, 0.0f, 360.0f, rock::CloudSettings{}.windDirectionDegrees, "Cloud wind direction changed", false,
                        Tr("Direction the clouds move, in degrees. North = 0, east = 90.",
                            "雲が流れる向きです。度数で指定します。北=0、東=90。"), "%.0f");
                }
                DrawPropertyFloatRow("Shadow Strength", "CloudShadowStrength", &clouds.shadowStrength, 0.0f, 1.0f, rock::CloudSettings{}.shadowStrength, "Cloud shadow strength changed", false,
                    Tr("Strength of cloud shadows cast onto terrain. 0 disables them; 1 is fully dark. The terrain shader multiplies by cloud transmittance projected along the sun direction.",
                        "雲が地形に落とす影の強さ。0 で影無し、1 で完全に暗くなります。太陽方向に projection した雲の透過率を地形シェーダーで乗算します。"));
                DrawPropertyBoolRow("Cloud-to-Cloud Shadows", "CloudSelfShadowEnabled", &clouds.selfShadowEnabled, "Cloud self shadow toggled",
                    Tr("Enables self-shadowing by sampling density from each cloud sample toward the sun, letting front clouds darken clouds behind or below them.",
                        "雲の各サンプルから太陽方向へ密度を読み、手前の雲が奥や下側の雲を暗くする自己遮蔽を有効化します。"), rock::CloudSettings{}.selfShadowEnabled, true);
                ImGui::BeginDisabled(!clouds.selfShadowEnabled);
                    DrawPropertyFloatRow("Light Step (m)", "CloudLightStep", &clouds.lightStepMeters, 1.0f, 1000.0f, rock::CloudSettings{}.lightStepMeters, "Cloud light step changed", false,
                        Tr("Distance per self-shadow raymarch step. Light Samples x Light Step gives the light travel distance toward the sun. Too short relative to cloud scale will not reach deep into clouds; too long makes sampling coarse.",
                            "自己遮蔽レイマーチの 1 ステップあたりの距離 (m)。Light Samples × Light Step が太陽方向への投光距離になります。雲スケールに対して短すぎると深い雲の中まで届かず、長すぎるとサンプルが粗くなります。"), "%.0f");
                    DrawPropertyFloatRow("Phase Eccentricity", "CloudPhaseG", &clouds.phaseEccentricity, -0.99f, 0.99f, rock::CloudSettings{}.phaseEccentricity, "Cloud phase eccentricity changed", false,
                        Tr("Henyey-Greenstein phase function g value. 0 is isotropic scattering, positive values create forward scattering and silver lining around the sun in backlight, and negative values create backscattering. Around 0.4 looks cloud-like.",
                            "Henyey-Greenstein 位相関数の g 値。0 で等方散乱、正値で前方散乱(逆光時に太陽周りが明るくなるシルバーライニング)、負値で後方散乱。0.4 前後が雲らしい見た目。"));
                ImGui::EndDisabled();
                DrawPropertyFloatRow("Shadow Ambient", "CloudShadowAmbient", &clouds.shadowAmbientStrength, 0.0f, 2.0f, rock::CloudSettings{}.shadowAmbientStrength, "Cloud shadow ambient changed", false,
                    Tr("Sky-colored ambient light added to the shadow side of clouds. Larger values lift self-shadowed areas toward blue.",
                        "雲の影側に足す空色の環境光です。大きいほど自己遮蔽された部分が青く持ち上がります。"));
            ImGui::EndDisabled();
        ImGui::EndTable();
    }
    ImGui::EndChild();
}


} // namespace terrain::ui
