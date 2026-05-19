#include "CameraPanel.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "PropertyWidgets.h"

namespace terrain::ui
{
namespace
{
constexpr float kFullFrameSensorHeightMm = 24.0f;

float CameraFocalLengthMmFromFovYDegrees(float fovYDegrees)
{
    const float clampedFovYDegrees = std::clamp(fovYDegrees, 15.0f, 90.0f);
    const float fovRadians = clampedFovYDegrees * 3.1415926535f / 180.0f;
    return kFullFrameSensorHeightMm / (2.0f * std::tan(fovRadians * 0.5f));
}

float CameraFovYDegreesFromFocalLengthMm(float focalLengthMm)
{
    const float defaultFocalLengthMm = CameraFocalLengthMmFromFovYDegrees(45.0f);
    const float clampedFocalLengthMm = std::max(0.1f, std::isfinite(focalLengthMm) ? focalLengthMm : defaultFocalLengthMm);
    const float fovRadians = 2.0f * std::atan(kFullFrameSensorHeightMm / (2.0f * clampedFocalLengthMm));
    return std::clamp(fovRadians * 180.0f / 3.1415926535f, 15.0f, 90.0f);
}
} // namespace

void DrawCameraPanel(CameraPanelState state)
{
    CameraPanelViewport& viewport = state.viewport;
    rock::PreviewSettings& preview = state.preview;
    const CameraPanelDefaults& defaults = state.defaults;

    if (ImGui::Button("Reset View"))
    {
        if (state.resetViewport)
        {
            state.resetViewport();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("カメラの向き、距離、パンを既定値に戻します。ショートカット: F");
    }

    ImGui::Spacing();
    if (ImGui::BeginTable("CameraRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        DrawCameraFloatRow("FOV", "FovDegrees", &viewport.fovDegrees, 15.0f, 90.0f, defaults.fovDegrees, "%.1f",
            "垂直画角です。小さいほど望遠、大きいほど広角になります。焦点距離 (mm) と連動します。");
        float focalLengthMm = CameraFocalLengthMmFromFovYDegrees(viewport.fovDegrees);
        if (DrawCameraFloatRow("焦点距離 (mm)", "FocalLengthMm", &focalLengthMm, 1.0f, 200.0f, CameraFocalLengthMmFromFovYDegrees(defaults.fovDegrees), "%.1f",
            "35mm フルサイズ相当のレンズ焦点距離です。画角と DOF のぼけ量の両方に反映されます。"))
        {
            viewport.fovDegrees = CameraFovYDegreesFromFocalLengthMm(focalLengthMm);
        }
        DrawCameraFloatRow("Distance", "OrbitDistance", &viewport.orbitDistance, 1.0f, defaults.maxOrbitDistance, defaults.orbitDistance, "%.1f",
            "注視点からカメラまでの距離です。マウスホイールのオービット距離と同じ値です。");
        DrawCameraFloatRow("Yaw", "ViewportYaw", &viewport.yaw, -3.14159f, 3.14159f, defaults.yaw, "%.3f",
            "カメラの水平回転です。地形を左右から見る向きを調整します。単位はラジアンです。");
        DrawCameraFloatRow("Pitch", "ViewportPitch", &viewport.pitch, -1.25f, 1.25f, defaults.pitch, "%.3f",
            "カメラの上下角です。高い視点や低い視点から地形を見る角度を調整します。単位はラジアンです。");

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Depth of Field");
    if (ImGui::BeginTable("CameraDofRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        DrawPropertyBoolRow("有効", "DofEnabled", &preview.depthOfFieldEnabled, "Depth of Field toggled",
            "ビューポート表示だけにかかる被写界深度です。地形データや OBJ エクスポートには影響しません。",
            rock::PreviewSettings{}.depthOfFieldEnabled, true);
        if (preview.depthOfFieldEnabled)
        {
            DrawPropertyFloatRow("F 値", "DofFStop", &preview.dofFStop, 0.7f, 32.0f, rock::PreviewSettings{}.dofFStop, "Depth of Field f-stop changed", false,
                "絞り値です。小さいほどぼけが強く、大きいほど深くピントが合います。", "%.1f");
            DrawPropertyFloatRow("フォーカス距離 (m)", "DofFocusDistance", &preview.dofFocusDistanceMeters, 0.1f, 20000.0f, rock::PreviewSettings{}.dofFocusDistanceMeters, "Depth of Field focus distance changed", false,
                "カメラからピント面までの距離です。Orbit Distance と近い値にすると注視点付近にピントが合います。", "%.1f", ImGuiSliderFlags_Logarithmic);
            DrawPropertyFloatRow("センサー高さ (mm)", "DofSensorHeight", &preview.dofSensorHeightMm, 4.0f, 80.0f, rock::PreviewSettings{}.dofSensorHeightMm, "Depth of Field sensor height changed", false,
                "Circle of Confusion の描画計算に使うセンサー高さです。フルサイズ横位置なら 24mm が標準です。", "%.1f");
            DrawPropertyFloatRow("最大ぼけ (px)", "DofMaxBlur", &preview.dofMaxBlurPixels, 0.0f, 64.0f, rock::PreviewSettings{}.dofMaxBlurPixels, "Depth of Field max blur changed", false,
                "表示上の最大ぼけ半径です。現実値ベースの操作感を保ちながら、重くなりすぎるぼけを抑えます。", "%.1f");
            DrawPropertyBoolRow("Miniature", "DofMiniatureEnabled", &preview.dofMiniatureEnabled, "Depth of Field miniature toggled",
                "地形をミニチュア撮影のように見せるため、DOF のぼけ範囲を視覚的に強調します。物理カメラ設定はそのまま残し、表示だけを調整します。",
                rock::PreviewSettings{}.dofMiniatureEnabled, true);
            if (preview.dofMiniatureEnabled)
            {
                DrawPropertyFloatRow("Miniature Scale", "DofMiniatureScale", &preview.dofMiniatureScale, 1.0f, 50.0f, rock::PreviewSettings{}.dofMiniatureScale, "Depth of Field miniature scale changed", false,
                    "DOF のぼけ範囲に掛ける倍率です。大きいほどミニチュア風の浅い焦点幅になります。", "%.1f", ImGuiSliderFlags_Logarithmic);
            }
            int apertureShape = std::clamp(preview.dofApertureShape, 0, 4);
            if (DrawPropertyComboRow("絞り形状", "DofApertureShape", &apertureShape, "丸\0三角形\0六角形\0八角形\0カスタム\0\0",
                "ぼけのサンプル形状です。多角形にすると絞り羽根由来の角の立ったボケになります。", rock::PreviewSettings{}.dofApertureShape))
            {
                preview.dofApertureShape = std::clamp(apertureShape, 0, 4);
                if (state.markGraphChanged)
                {
                    state.markGraphChanged("Depth of Field aperture shape changed");
                }
            }
            if (preview.dofApertureShape == 4)
            {
                DrawPropertyIntRow("絞り羽根", "DofApertureBlades", &preview.dofApertureBlades, 3, 12, rock::PreviewSettings{}.dofApertureBlades, "Depth of Field aperture blades changed", false,
                    "カスタム多角形ボケの羽根数です。");
            }
            DrawPropertyFloatRow("絞り回転 (deg)", "DofApertureRotation", &preview.dofApertureRotationDegrees, -180.0f, 180.0f, rock::PreviewSettings{}.dofApertureRotationDegrees, "Depth of Field aperture rotation changed", false,
                "多角形ボケの角度です。丸ボケでは見た目にほぼ影響しません。", "%.1f");
            DrawPropertyFloatRow("ハイライト強調", "DofHighlightBoost", &preview.dofHighlightBoost, 0.0f, 4.0f, rock::PreviewSettings{}.dofHighlightBoost, "Depth of Field highlight boost changed", false,
                "明るいサンプルを少し強め、点光源や明るい輪郭のボケを目立たせます。", "%.2f");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Right-handed / Y-up");
    ImGui::TextDisabled("Grid: %d x %d, %.0f m cells",
        state.gridCellCount,
        state.gridCellCount,
        state.gridCellSizeMeters);
}
} // namespace terrain::ui
