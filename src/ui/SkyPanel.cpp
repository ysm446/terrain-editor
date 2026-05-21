#include "SkyPanel.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <imgui.h>

#include "PropertyWidgets.h"

namespace terrain::ui
{
namespace
{
constexpr std::array<int, 4> kShadowResolutionPresets = {512, 1024, 2048, 4096};
constexpr float kDegreesToRadians = 3.1415926535f / 180.0f;

void SaveAppSettings(const SkyPanelState& state)
{
    if (state.saveAppSettings)
    {
        state.saveAppSettings();
    }
}

bool DrawShadowResolutionPresetRow(const char* label, const char* id, int* value, int defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr)
{
    return DrawPresetIntRow(label, id, value, defaultValue, kShadowResolutionPresets, 1024, dirtyReason, recordUndo, tooltip);
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

    if (ImGui::CollapsingHeader("太陽と影", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("SunShadowSettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::SeparatorText("太陽");
            int sunModeInt = static_cast<int>(settings.preview.sunDirectionMode);
            if (DrawPropertyComboRow("Sun Mode", "DisplaySunMode", &sunModeInt, "Manual\0Date Time\0\0", "Manual は方位角と高度を直接指定します。Date Time は緯度、経度、月日、時刻、UTC Offset からそれらしい太陽位置を計算します。", static_cast<int>(rock::PreviewSettings{}.sunDirectionMode)))
            {
                settings.preview.sunDirectionMode = static_cast<rock::SunDirectionMode>(std::clamp(sunModeInt,
                    static_cast<int>(rock::SunDirectionMode::Manual),
                    static_cast<int>(rock::SunDirectionMode::DateTime)));
                SaveAppSettings(state);
            }
            if (settings.preview.sunDirectionMode == rock::SunDirectionMode::DateTime)
            {
                if (DrawPropertyFloatRow("Latitude", "SunLatitude", &settings.preview.sunLatitudeDegrees, -90.0f, 90.0f, rock::PreviewSettings{}.sunLatitudeDegrees, "Sun latitude changed", false, "太陽位置計算に使う緯度です。北緯を正、南緯を負で指定します。"))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Longitude", "SunLongitude", &settings.preview.sunLongitudeDegrees, -180.0f, 180.0f, rock::PreviewSettings{}.sunLongitudeDegrees, "Sun longitude changed", false, "太陽位置計算に使う経度です。東経を正、西経を負で指定します。"))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("UTC Offset", "SunUtcOffset", &settings.preview.sunUtcOffsetHours, -12.0f, 14.0f, rock::PreviewSettings{}.sunUtcOffsetHours, "Sun UTC offset changed", false, "日時の解釈に使う UTC からの時差です。夏時間やタイムゾーンDBは使わず、ここで指定した値をそのまま使います。", "%.1f"))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyIntRow("Month", "SunMonth", &settings.preview.sunMonth, 1, 12, rock::PreviewSettings{}.sunMonth, "Sun month changed", false, "太陽位置計算に使う月です。年は固定の非うるう年として扱います。"))
                {
                    settings.preview.sunDay = std::clamp(settings.preview.sunDay, 1, DaysInMonth(settings.preview.sunMonth));
                    SaveAppSettings(state);
                }
                const int maxDay = DaysInMonth(settings.preview.sunMonth);
                if (DrawPropertyIntRow("Day", "SunDay", &settings.preview.sunDay, 1, maxDay, std::clamp(rock::PreviewSettings{}.sunDay, 1, maxDay), "Sun day changed", false, "太陽位置計算に使う日です。月に応じて最大日数を制限します。"))
                {
                    settings.preview.sunDay = std::clamp(settings.preview.sunDay, 1, maxDay);
                    SaveAppSettings(state);
                }
                if (DrawTimeOfDayRow("Time", "SunTime", &settings.preview.sunTimeHours, rock::PreviewSettings{}.sunTimeHours, "Sun time changed", "ローカル時刻です。0:00 から 24:00 までをスライダーで指定します。"))
                {
                    SaveAppSettings(state);
                }

                const SkyPanelSunPosition computedSun = EffectiveSunPosition(settings.preview);
                DrawReadOnlyFloatRow("Computed Azimuth", computedSun.azimuth, "%.2f", "計算されたアプリ内方位角です。0° が南(Z+)、90° が東(X+)です。");
                DrawReadOnlyFloatRow("Computed Elevation", computedSun.elevation, "%.2f", "計算された太陽高度です。");
            }
            else
            {
                if (DrawPropertyFloatRow("Sun Azimuth (deg)", "DisplaySunAzimuth", &settings.preview.sunAzimuthDegrees, 0.0f, 360.0f, rock::PreviewSettings{}.sunAzimuthDegrees, "Sun azimuth changed", false, "太陽の水平角度です。0° が南(Z+)、90° が東(X+)です。地形の溝が読みやすい方向へ回せます。"))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Sun Elevation (deg)", "DisplaySunElevation", &settings.preview.sunElevationDegrees, -10.0f, 89.0f, rock::PreviewSettings{}.sunElevationDegrees, "Sun elevation changed", false, "太陽の高さです。低いほど影が長く、凹凸が強調されます。0° は地平線、負値は地平より下 (夜遷移の確認用)。"))
                {
                    SaveAppSettings(state);
                }
            }
            if (DrawPropertyFloatRow("Sun Intensity", "DisplaySunIntensity", &settings.preview.sunIntensity, 0.0f, 5.0f, rock::PreviewSettings{}.sunIntensity, "Sun intensity changed", false, "直射光の強さです。"))
            {
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Ambient", "DisplayAmbientStrength", &settings.preview.ambientStrength, 0.0f, 2.0f, rock::PreviewSettings{}.ambientStrength, "Ambient strength changed", false, "影側を持ち上げる環境光の強さです。"))
            {
                SaveAppSettings(state);
            }
            ImGui::SeparatorText("影");
            if (DrawPropertyFloatRow("Shadow Strength", "DisplayShadowStrength", &settings.preview.shadowStrength, 0.0f, 1.0f, rock::PreviewSettings{}.shadowStrength, "Shadow strength changed", false, "シャドウマップで落ちる影の濃さです。"))
            {
                SaveAppSettings(state);
            }
            if (DrawShadowResolutionPresetRow("Shadow Map Resolution", "DisplayShadowMapResolution", &settings.preview.shadowMapResolution, rock::PreviewSettings{}.shadowMapResolution, "Shadow map resolution changed", false, "太陽方向から見た深度マップの解像度です。高いほど影の輪郭が細かくなりますが描画負荷が増えます。"))
            {
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Shadow Bias", "DisplayShadowBias", &settings.preview.shadowBias, 0.0f, 0.05f, rock::PreviewSettings{}.shadowBias, "Shadow bias changed", false, "影のにじみや縞を抑えるための深度オフセットです。大きすぎると影が浮いて見えます。"))
            {
                SaveAppSettings(state);
            }

            ImGui::EndTable();
        }
    }

    if (!ImGui::CollapsingHeader("天球と雲", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("SkySettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        rock::SkySettings& sky = settings.sky;
            ImGui::SeparatorText("天球 (大気散乱)");
            DrawPropertyFloatRow("大気厚み (密度)", "SkyAtmosphereDensity", &sky.atmosphereDensity, 0.05f, 5.0f, rock::SkySettings{}.atmosphereDensity, "Sky atmosphere density changed", false, "Rayleigh 散乱係数 β_R と地平ヘイズの倍率。1.0 が地球標準、0.5 で薄い大気、2-3 で濃い大気。地形の遠景フォグもこの値から自動で決まります。");
            DrawPropertyFloatRow("ヘイズ (Mie 強度)", "SkyMieStrength", &sky.mieStrength, 0.0f, 8.0f, rock::SkySettings{}.mieStrength, "Sky mie strength changed", false, "Mie 散乱の強さ。0.2 前後が編集ビュー向けの標準です。大きいほど太陽方向の霞とグローが強くなりますが、地平の暖色帯も出やすくなります。0 で純 Rayleigh。");
            DrawPropertyFloatRow("Mie 偏向 (g)", "SkyMieG", &sky.mieEccentricity, -0.95f, 0.95f, rock::SkySettings{}.mieEccentricity, "Sky mie g changed", false, "Henyey-Greenstein g 値。0 で等方散乱、正で前方 (太陽方向) 散乱が強くなりグローが太陽周りに集中。0.7-0.85 が現実的。");
            DrawColorRgbRow("地面アルベド", "SkyGroundAlbedo", sky.groundAlbedo, rock::SkySettings{}.groundAlbedo);
            DrawPropertyFloatRow("太陽サイズ (deg)", "SkySunSize", &sky.sunSizeDegrees, 0.1f, 20.0f, rock::SkySettings{}.sunSizeDegrees, "Sky sun size changed", false, "太陽ディスクの直径(度)。実際の太陽は約 0.5 度ですが、視認性のためデフォルトはやや大きめです。");
            DrawPropertyFloatRow("太陽グロー", "SkySunGlow", &sky.sunGlowStrength, 0.0f, 2.0f, rock::SkySettings{}.sunGlowStrength, "Sky sun glow changed", false, "太陽周辺の柔らかい光の強さ。0 でグロー無し。");

            ImGui::SeparatorText("ボリューム雲");
            rock::CloudSettings& clouds = settings.clouds;
            DrawPropertyBoolRow("有効", "CloudEnabled", &clouds.enabled, "Clouds enabled toggled", "ボリューム雲のレイマーチ描画を有効化します。3D 密度テクスチャ (128³ R8 = 2MB) を生成し、雲帯 [Altitude Min, Max] とのレイ交差をフルスクリーンパスで毎フレーム積分します。", rock::CloudSettings{}.enabled, true);
            if (clouds.enabled)
            {
                DrawPropertyIntRow("Cloud Seed", "CloudSeed", &clouds.seed, 0, 999999, rock::CloudSettings{}.seed, "Cloud seed changed", false, "3D 密度ノイズのシード。変更すると雲のパターンが変わります(テクスチャを再生成)。");
                DrawPropertyFloatRow("Coverage", "CloudCoverage", &clouds.coverage, 0.0f, 1.0f, rock::CloudSettings{}.coverage, "Cloud coverage changed", false, "空に占める雲の割合。0 で雲無し、1 で空一面が雲。");
                DrawPropertyFloatRow("Density", "CloudDensity", &clouds.densityMultiplier, 0.0f, 4.0f, rock::CloudSettings{}.densityMultiplier, "Cloud density changed", false, "雲の濃さ倍率。大きいほど雲が不透明になります。");
                DrawPropertyFloatRow("Altitude Min (m)", "CloudAltMin", &clouds.altitudeMin, 0.0f, 8000.0f, rock::CloudSettings{}.altitudeMin, "Cloud altitude min changed", false, "雲帯の下限高度 (m)。地形をすっぽり包むには地形最高点より低い値、上に浮かべるなら高い値を指定。", "%.0f");
                DrawPropertyFloatRow("Altitude Max (m)", "CloudAltMax", &clouds.altitudeMax, 0.0f, 12000.0f, rock::CloudSettings{}.altitudeMax, "Cloud altitude max changed", false, "雲帯の上限高度 (m)。Max - Min が雲層の厚さです。", "%.0f");
                DrawPropertyFloatRow("Horizontal Scale (m)", "CloudHorizScale", &clouds.horizontalScale, 200.0f, 30000.0f, rock::CloudSettings{}.horizontalScale, "Cloud scale changed", false, "雲の水平スケール。大きいほど雲塊が大きく、小さいほど細かい雲になります。", "%.0f");
                DrawPropertyFloatRow("Field Radius (m)", "CloudFieldRadius", &clouds.fieldRadius, 200.0f, 50000.0f, rock::CloudSettings{}.fieldRadius, "Cloud field radius changed", false, "地形の中心を原点にした、雲が存在する円形フィールドの半径。大きくするとより遠くまで雲が広がります。地形と同程度にすると地形の周りだけに雲が出ます。", "%.0f");
                DrawPropertyFloatRow("Field Falloff (m)", "CloudFieldFalloff", &clouds.fieldFalloff, 50.0f, 20000.0f, rock::CloudSettings{}.fieldFalloff, "Cloud field falloff changed", false, "フィールド端のフェードアウト幅。大きいほど雲がじわっと消え、小さいと境界がくっきりします。", "%.0f");
                DrawPropertyFloatRow("Absorption", "CloudAbsorption", &clouds.absorption, 0.0f, 0.5f, rock::CloudSettings{}.absorption, "Cloud absorption changed", false, "Beer-Lambert の吸収係数。大きいほど雲がはっきり不透明になります。", "%.4f");
                DrawColorRgbRow("Cloud Color", "CloudColor", clouds.color, rock::CloudSettings{}.color);
                DrawPropertyBoolRow("雲を動かす", "CloudAnimate", &clouds.animate, "Cloud animation toggled", "ON のときだけ風向きと速度を使って雲を流します。OFF では速度の設定値を保持したまま静止表示します。", rock::CloudSettings{}.animate, true);
                DrawPropertyPercentRow("Loop Phase (%)", "CloudLoopPhase", &clouds.loopPhase, 0.0f, 1.0f, rock::CloudSettings{}.loopPhase, "Cloud loop phase changed", "雲タイル一周の中でどの位置を表示するかです。0% と 100% は同じ位置で、雲を動かすと Wind Direction に近いループ方向を Wind Speed と Horizontal Scale から計算した速度で進みます。");
                if (clouds.animate)
                {
                    DrawPropertyFloatRow("Wind Speed (m/s)", "CloudWindSpeed", &clouds.windSpeedMetersPerSec, 0.0f, 200.0f, rock::CloudSettings{}.windSpeedMetersPerSec, "Cloud wind speed changed", false, "雲が流れる速度 (m/s)。動かすとフレーム毎にビューポートが再描画され負荷が増えます。");
                    DrawPropertyFloatRow("Wind Direction (deg)", "CloudWindDir", &clouds.windDirectionDegrees, 0.0f, 360.0f, rock::CloudSettings{}.windDirectionDegrees, "Cloud wind direction changed", false, "雲が流れる向きです。度数で指定します。北=0、東=90。", "%.0f");
                }
                DrawPropertyIntRow("Quality (samples)", "CloudQuality", &clouds.qualitySamples, 8, 96, rock::CloudSettings{}.qualitySamples, "Cloud quality changed", false, "1 ピクセルあたりのレイマーチサンプル数。大きいほど雲のディテールが上がりますが負荷も増えます。32 が標準、低スペックなら 16、高品質なら 64。");
                DrawPropertyFloatRow("Shadow Strength", "CloudShadowStrength", &clouds.shadowStrength, 0.0f, 1.0f, rock::CloudSettings{}.shadowStrength, "Cloud shadow strength changed", false, "雲が地形に落とす影の強さ。0 で影無し、1 で完全に暗くなります。太陽方向に projection した雲の透過率を地形シェーダーで乗算します。");
                if (DrawShadowResolutionPresetRow("Shadow Resolution", "CloudShadowResolution", &clouds.shadowResolution, rock::CloudSettings{}.shadowResolution, "Cloud shadow resolution changed", false, "雲影テクスチャの解像度 (片辺ピクセル数)。1024 で約 1MB。大きいほど影の輪郭が細かくなりますが生成負荷が増えます。"))
                {
                    SaveAppSettings(state);
                }
                DrawPropertyIntRow("Shadow Samples", "CloudShadowSamples", &clouds.shadowSamples, 4, 64, rock::CloudSettings{}.shadowSamples, "Cloud shadow samples changed", false, "雲影テクスチャ生成時に太陽方向へ撃つレイのサンプル数。大きいほど厚い雲の影が正確になりますが生成時間も増えます。16 が標準。");
                DrawPropertyIntRow("Light Samples", "CloudLightSamples", &clouds.lightSamples, 0, 16, rock::CloudSettings{}.lightSamples, "Cloud light samples changed", false, "雲内自己遮蔽の太陽方向レイマーチ段数。0 で無効化(従来の上下ランプのみ)、6 が標準。大きいほど雲塊の陰影がはっきりしますが負荷も増えます。");
                DrawPropertyFloatRow("Light Step (m)", "CloudLightStep", &clouds.lightStepMeters, 1.0f, 1000.0f, rock::CloudSettings{}.lightStepMeters, "Cloud light step changed", false, "自己遮蔽レイマーチの 1 ステップあたりの距離 (m)。Light Samples × Light Step が太陽方向への投光距離になります。雲スケールに対して短すぎると深い雲の中まで届かず、長すぎるとサンプルが粗くなります。", "%.0f");
                DrawPropertyFloatRow("Phase Eccentricity", "CloudPhaseG", &clouds.phaseEccentricity, -0.99f, 0.99f, rock::CloudSettings{}.phaseEccentricity, "Cloud phase eccentricity changed", false, "Henyey-Greenstein 位相関数の g 値。0 で等方散乱、正値で前方散乱(逆光時に太陽周りが明るくなるシルバーライニング)、負値で後方散乱。0.4 前後が雲らしい見た目。");
            }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}


} // namespace terrain::ui
