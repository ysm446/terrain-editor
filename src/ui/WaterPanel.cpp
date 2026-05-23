#include "WaterPanel.h"

#include <algorithm>

#include <imgui.h>

#include "PropertyWidgets.h"

namespace terrain::ui
{
namespace
{
void SaveAppSettings(const WaterPanelState& state)
{
    if (state.saveAppSettings)
    {
        state.saveAppSettings();
    }
}
} // namespace

void DrawWaterSettingsPanel(WaterPanelState state)
{
    rock::PreviewSettings& preview = state.settings.preview;
    const float headerRightPadding = 10.0f;
    const float sectionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - headerRightPadding);
    ImGui::BeginChild("WaterSettingsSection", ImVec2(sectionWidth, 0.0f), false);
    if (ImGui::BeginTable("WaterSettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::SeparatorText("水面");
        if (DrawPropertyBoolRow("Water Surface", "DisplayWaterEnabled", &preview.waterEnabled, "Water surface toggled",
            "表示専用の水平な水面です。ノード評価や OBJ エクスポートには影響しません。", rock::PreviewSettings{}.waterEnabled, true))
        {
            SaveAppSettings(state);
        }
        if (preview.waterEnabled)
        {
            if (DrawPropertyFloatRow("Water Level (m)", "DisplayWaterLevel", &preview.waterLevelMeters, 0.0f, 4000.0f, rock::PreviewSettings{}.waterLevelMeters, "Water level changed", false,
                "水面の高さです。0m 以上の水平な水位として表示されます。", "%.1f"))
            {
                preview.waterLevelMeters = std::clamp(preview.waterLevelMeters, 0.0f, 10000.0f);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Opacity", "DisplayWaterOpacity", &preview.waterOpacity, 0.0f, 1.0f, rock::PreviewSettings{}.waterOpacity, "Water opacity changed", false,
                "水の濁りや吸収の強さです。0 では遠くまで透き通り、1 では短い距離で不透明になります。反射とスペキュラーは別に残ります。", "%.2f"))
            {
                preview.waterOpacity = std::clamp(preview.waterOpacity, 0.0f, 1.0f);
                SaveAppSettings(state);
            }
            if (DrawColorRgbRow("Water Color", "DisplayWaterColor", preview.waterColor, rock::PreviewSettings{}.waterColor))
            {
                SaveAppSettings(state);
            }
            if (DrawPropertyBoolRow("Animation", "DisplayWaterAnimEnabled", &preview.waterAnimationEnabled, "Water animation toggled",
                "波のアニメーションのオン / オフです。オフにすると波形が静止します。", rock::PreviewSettings{}.waterAnimationEnabled, true))
            {
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Waves Scale (m)", "DisplayWaterWavesScale", &preview.waterWavesScale, 1.0f, 500.0f, rock::PreviewSettings{}.waterWavesScale, "Water waves scale changed", false,
                "水面法線の主波長です。20〜80m 程度では地形スケールに馴染みやすく、小さいほど細かいさざ波、大きいほど緩い波になります。", "%.1f"))
            {
                preview.waterWavesScale = std::clamp(preview.waterWavesScale, 1.0f, 500.0f);
                SaveAppSettings(state);
            }

            ImGui::SeparatorText("反射");
            if (DrawPropertyFloatRow("Reflection Strength", "DisplayWaterReflectionStrength", &preview.waterReflectionStrength, 0.0f, 2.0f, rock::PreviewSettings{}.waterReflectionStrength, "Water reflection strength changed", false,
                "水面への地形・空の映り込みの強さです。0 で反射なし、1 が標準、2 で強めになります。SSR による実際の地形色の映り込みにも影響します。", "%.2f"))
            {
                preview.waterReflectionStrength = std::clamp(preview.waterReflectionStrength, 0.0f, 3.0f);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Refractive Index", "DisplayWaterRefractiveIndex", &preview.waterRefractiveIndex, 1.0f, 2.0f, rock::PreviewSettings{}.waterRefractiveIndex, "Water refractive index changed", false,
                "フレネル反射用の屈折率です。1.0 = 反射なし、1.33 = 水相当、2.0 = ガラス相当。", "%.2f"))
            {
                preview.waterRefractiveIndex = std::clamp(preview.waterRefractiveIndex, 1.0f, 4.0f);
                SaveAppSettings(state);
            }

            ImGui::SeparatorText("屈折 / 断面");
            if (DrawPropertyFloatRow("Refraction Strength", "DisplayWaterRefractionStrength", &preview.waterRefractionStrength, 0.0f, 2.0f, rock::PreviewSettings{}.waterRefractionStrength, "Water refraction strength changed", false,
                "水面越しの背景をどれだけ歪ませるかです。断面の屈折効果にも影響します。0 で屈折なし、1 が標準、2 で強めになります。", "%.2f"))
            {
                preview.waterRefractionStrength = std::clamp(preview.waterRefractionStrength, 0.0f, 2.0f);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Refraction Blur", "DisplayWaterRefractionBlur", &preview.waterRefractionBlur, 0.0f, 2.0f, rock::PreviewSettings{}.waterRefractionBlur, "Water refraction blur changed", false,
                "断面側壁の背景のにじみ量です。0 でシャープ、1 が標準、2 で強めにぼかします。薄い水では背後の地形色が透けて見え、厚みがあるほど水色へ戻ります。", "%.2f"))
            {
                preview.waterRefractionBlur = std::clamp(preview.waterRefractionBlur, 0.0f, 2.0f);
                SaveAppSettings(state);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}
} // namespace terrain::ui
