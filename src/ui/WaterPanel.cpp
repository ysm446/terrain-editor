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
            if (DrawPropertyFloatRow("Water Level (m)", "DisplayWaterLevel", &preview.waterLevelMeters, -2000.0f, 4000.0f, rock::PreviewSettings{}.waterLevelMeters, "Water level changed", false,
                "水面の高さです。地形の窪地に対して水平な水位として表示されます。", "%.1f"))
            {
                preview.waterLevelMeters = std::clamp(preview.waterLevelMeters, -10000.0f, 10000.0f);
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
            if (DrawPropertyFloatRow("Waves Scale (m)", "DisplayWaterWavesScale", &preview.waterWavesScale, 1.0f, 500.0f, rock::PreviewSettings{}.waterWavesScale, "Water waves scale changed", false,
                "水面法線の主波長です。20〜80m 程度では地形スケールに馴染みやすく、小さいほど細かいさざ波、大きいほど緩い波になります。", "%.1f"))
            {
                preview.waterWavesScale = std::clamp(preview.waterWavesScale, 1.0f, 500.0f);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Refractive Index", "DisplayWaterRefractiveIndex", &preview.waterRefractiveIndex, 1.0f, 2.0f, rock::PreviewSettings{}.waterRefractiveIndex, "Water refractive index changed", false,
                "水の屈折率です。フレネル反射の強さを決定します。1.0 = 反射なし、1.33 = 水相当、2.0 = ガラス相当。", "%.2f"))
            {
                preview.waterRefractiveIndex = std::clamp(preview.waterRefractiveIndex, 1.0f, 4.0f);
                SaveAppSettings(state);
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}
} // namespace terrain::ui
