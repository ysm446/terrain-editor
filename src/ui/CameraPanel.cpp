#include "CameraPanel.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

#include "Localization.h"
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

    if (ImGui::Button(Tr("Reset View", "ビューをリセット")))
    {
        if (state.resetViewport)
        {
            state.resetViewport();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("%s", Tr("Reset camera rotation, distance, and pan to their defaults. Shortcut: A",
            "カメラの向き、距離、パンを既定値に戻します。ショートカット: A"));
    }

    ImGui::Spacing();
    if (ImGui::BeginTable("CameraRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        DrawCameraFloatRow("FOV", "FovDegrees", &viewport.fovDegrees, 15.0f, 90.0f, defaults.fovDegrees, "%.1f",
            Tr("Vertical field of view. Smaller values are telephoto; larger values are wide-angle. Linked with Focal Length (mm).",
                "垂直画角です。小さいほど望遠、大きいほど広角になります。焦点距離 (mm) と連動します。"));
        float focalLengthMm = CameraFocalLengthMmFromFovYDegrees(viewport.fovDegrees);
        if (DrawCameraFloatRow(Tr("Focal Length (mm)", "焦点距離 (mm)"), "FocalLengthMm", &focalLengthMm, 1.0f, 200.0f, CameraFocalLengthMmFromFovYDegrees(defaults.fovDegrees), "%.1f",
            Tr("Lens focal length in 35mm full-frame terms. It affects both the view angle and DOF blur amount.",
                "35mm フルサイズ相当のレンズ焦点距離です。画角と DOF のぼけ量の両方に反映されます。")))
        {
            viewport.fovDegrees = CameraFovYDegreesFromFocalLengthMm(focalLengthMm);
        }
        DrawCameraFloatRow("Distance", "OrbitDistance", &viewport.orbitDistance, 1.0f, defaults.maxOrbitDistance, defaults.orbitDistance, "%.1f",
            Tr("Distance from the focus point to the camera. This is the same orbit distance controlled by the mouse wheel.",
                "注視点からカメラまでの距離です。マウスホイールのオービット距離と同じ値です。"));
        DrawCameraFloatRow("Yaw", "ViewportYaw", &viewport.yaw, -3.14159f, 3.14159f, defaults.yaw, "%.3f",
            Tr("Horizontal camera rotation. Adjusts the direction used to view the terrain from the sides. Unit: radians.",
                "カメラの水平回転です。地形を左右から見る向きを調整します。単位はラジアンです。"));
        DrawCameraFloatRow("Pitch", "ViewportPitch", &viewport.pitch, -1.25f, 1.25f, defaults.pitch, "%.3f",
            Tr("Vertical camera angle. Adjusts whether the terrain is viewed from a higher or lower point. Unit: radians.",
                "カメラの上下角です。高い視点や低い視点から地形を見る角度を調整します。単位はラジアンです。"));
        if (DrawPropertyBoolRow(Tr("Orbit Camera", "カメラを回す"), "CameraAutoOrbitEnabled", &viewport.autoOrbitEnabled, "Camera auto orbit toggled",
            Tr("Automatically rotates the camera around the Y axis for demos. Shortcut: O",
                "デモ用に、カメラを Y 軸まわりへ自動回転させます。ショートカット: O"), false, true))
        {
            if (state.saveAppSettings)
            {
                state.saveAppSettings();
            }
        }
        if (DrawCameraFloatRow(Tr("Angular Speed (deg/s)", "角速度 (deg/s)"), "CameraAutoOrbitSpeed", &viewport.autoOrbitSpeedDegreesPerSecond, -180.0f, 180.0f, 15.0f, "%.1f",
            Tr("Angular speed for automatic camera orbit. Positive values rotate one way; negative values rotate the other way.",
                "カメラ自動回転の角速度です。正の値で右回り、負の値で逆回りに回転します。")))
        {
            viewport.autoOrbitSpeedDegreesPerSecond = std::clamp(viewport.autoOrbitSpeedDegreesPerSecond, -360.0f, 360.0f);
            if (state.saveAppSettings)
            {
                state.saveAppSettings();
            }
        }

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("HDR");
    if (ImGui::BeginTable("CameraHdrRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        DrawPropertyBoolRow("HDR Viewport", "CameraHdrViewportEnabled", &preview.hdrViewportEnabled, "HDR viewport toggled",
            Tr("Uses a 16-bit float HDR viewport buffer with tonemapping and exposure controls. Turn this off to use the pre-HDR 8-bit viewport path.",
                "16-bit float の HDR ビューポートバッファを使い、トーンマップと露出調整を有効にします。オフにすると HDR 以前の 8-bit ビューポート経路を使います。"),
            rock::PreviewSettings{}.hdrViewportEnabled, true);

        ImGui::BeginDisabled(!preview.hdrViewportEnabled);
        {
            int exposureModeInt = static_cast<int>(preview.exposureMode);
            if (DrawPropertyComboRow(Tr("Exposure Mode", "露出モード"), "CameraExposureMode", &exposureModeInt, Tr("Manual\0Auto\0\0", "Manual\0Auto\0\0"),
                Tr("Manual uses the Exposure EV value directly. Auto samples the HDR viewport and adjusts exposure into the selected EV range.",
                    "Manual は Exposure EV をそのまま使います。Auto は HDR ビューポートをサンプリングし、指定した EV 範囲内で露出を自動調整します。"),
                static_cast<int>(rock::PreviewSettings{}.exposureMode)))
            {
                preview.exposureMode = static_cast<rock::ExposureMode>(std::clamp(exposureModeInt,
                    static_cast<int>(rock::ExposureMode::Manual),
                    static_cast<int>(rock::ExposureMode::Auto)));
            }
            if (preview.exposureMode == rock::ExposureMode::Manual)
            {
                DrawPropertyFloatRow("Exposure EV", "CameraExposureEv", &preview.exposureEv, -8.0f, 8.0f, rock::PreviewSettings{}.exposureEv, "Exposure changed", false,
                    Tr("Manual exposure compensation in stops. Positive values brighten the viewport; negative values darken it.",
                        "手動露出補正です。正の値で明るく、負の値で暗くします。"), "%.2f");
            }
            else
            {
                DrawPropertyFloatRow("Bias EV", "CameraAutoExposureBiasEv", &preview.autoExposureBiasEv, -4.0f, 4.0f, rock::PreviewSettings{}.autoExposureBiasEv, "Auto exposure bias changed", false,
                    Tr("Exposure compensation applied after auto metering.", "自動測光後に加える露出補正です。"), "%.2f");
                if (DrawPropertyFloatRow("Min EV", "CameraAutoExposureMinEv", &preview.autoExposureMinEv, -8.0f, 8.0f, rock::PreviewSettings{}.autoExposureMinEv, "Auto exposure min changed", false,
                    Tr("Darkest exposure allowed for auto exposure.", "自動露出で許可する最も暗い露出です。"), "%.2f"))
                {
                    preview.autoExposureMinEv = std::clamp(preview.autoExposureMinEv, -8.0f, 8.0f);
                    preview.autoExposureMaxEv = std::max(preview.autoExposureMaxEv, preview.autoExposureMinEv);
                }
                if (DrawPropertyFloatRow("Max EV", "CameraAutoExposureMaxEv", &preview.autoExposureMaxEv, -8.0f, 8.0f, rock::PreviewSettings{}.autoExposureMaxEv, "Auto exposure max changed", false,
                    Tr("Brightest exposure allowed for auto exposure.", "自動露出で許可する最も明るい露出です。"), "%.2f"))
                {
                    preview.autoExposureMaxEv = std::clamp(preview.autoExposureMaxEv, preview.autoExposureMinEv, 8.0f);
                }
                if (DrawPropertyFloatRow("Speed", "CameraAutoExposureSpeed", &preview.autoExposureSpeed, 0.05f, 8.0f, rock::PreviewSettings{}.autoExposureSpeed, "Auto exposure speed changed", false,
                    Tr("How quickly auto exposure adapts. Lower values make transitions slower.",
                        "自動露出が追従する速さです。小さいほど露出変化がゆっくりになります。"), "%.2f"))
                {
                    preview.autoExposureSpeed = std::clamp(preview.autoExposureSpeed, 0.05f, 8.0f);
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Color");
    if (ImGui::BeginTable("CameraColorRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        DrawPropertyFloatRow(Tr("Color Temperature (K)", "色温度 (K)"), "CameraColorTemperatureKelvin", &preview.colorTemperatureKelvin,
            2000.0f, 12000.0f, rock::PreviewSettings{}.colorTemperatureKelvin, "Color temperature changed", false,
            Tr("Viewport white balance. Lower values warm the image; higher values cool it.",
                "ビューポートのホワイトバランスです。低い値で暖色、高い値で寒色へ寄せます。"), "%.0f");
        preview.colorTemperatureKelvin = std::clamp(preview.colorTemperatureKelvin, 2000.0f, 12000.0f);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Depth of Field");
    if (ImGui::BeginTable("CameraDofRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        DrawPropertyBoolRow(Tr("Enabled", "有効"), "DofEnabled", &preview.depthOfFieldEnabled, "Depth of Field toggled",
            Tr("Depth of Field applied only to the viewport display. It does not affect terrain data or OBJ export. Shortcut: D",
                "ビューポート表示だけにかかる被写界深度です。地形データや OBJ エクスポートには影響しません。ショートカット: D"),
            rock::PreviewSettings{}.depthOfFieldEnabled, true);
        ImGui::BeginDisabled(!preview.depthOfFieldEnabled);
        DrawPropertyFloatRow(Tr("F-Stop", "F 値"), "DofFStop", &preview.dofFStop, 0.7f, 32.0f, rock::PreviewSettings{}.dofFStop, "Depth of Field f-stop changed", false,
            Tr("Aperture value. Smaller values create stronger blur; larger values keep more depth in focus.",
                "絞り値です。小さいほどぼけが強く、大きいほど深くピントが合います。"), "%.1f");
        DrawPropertyFloatRow(Tr("Focus Distance (m)", "フォーカス距離 (m)"), "DofFocusDistance", &preview.dofFocusDistanceMeters, 0.1f, 20000.0f, rock::PreviewSettings{}.dofFocusDistanceMeters, "Depth of Field focus distance changed", false,
            Tr("Distance from the camera to the focus plane. Values near Orbit Distance focus around the point of interest.",
                "カメラからピント面までの距離です。Orbit Distance と近い値にすると注視点付近にピントが合います。"), "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(Tr("Focus Picker", "フォーカス指定"));
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button(state.focusPickActive ? Tr("Picking...", "選択中...") : Tr("Pick Focus", "フォーカスを選択"), ImVec2(132.0f, 0.0f)))
        {
            if (state.requestFocusPick)
            {
                state.requestFocusPick();
            }
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("%s", Tr("Click terrain in the viewport to set the focus distance.\nShortcut: F+Click",
                "ビューポートの地形をクリックしてフォーカス距離を設定します。\nショートカット: F+クリック"));
        }
        DrawPropertyFloatRow(Tr("Sensor Height (mm)", "センサー高さ (mm)"), "DofSensorHeight", &preview.dofSensorHeightMm, 4.0f, 80.0f, rock::PreviewSettings{}.dofSensorHeightMm, "Depth of Field sensor height changed", false,
            Tr("Sensor height used for Circle of Confusion rendering. 24 mm is standard for full-frame landscape orientation.",
                "Circle of Confusion の描画計算に使うセンサー高さです。フルサイズ横位置なら 24mm が標準です。"), "%.1f");
        DrawPropertyFloatRow(Tr("Max Blur (px)", "最大ぼけ (px)"), "DofMaxBlur", &preview.dofMaxBlurPixels, 0.0f, 64.0f, rock::PreviewSettings{}.dofMaxBlurPixels, "Depth of Field max blur changed", false,
            Tr("Maximum blur radius on screen. Keeps physically based controls usable while preventing overly heavy blur.",
                "表示上の最大ぼけ半径です。現実値ベースの操作感を保ちながら、重くなりすぎるぼけを抑えます。"), "%.1f");
        DrawPropertyBoolRow("Miniature", "DofMiniatureEnabled", &preview.dofMiniatureEnabled, "Depth of Field miniature toggled",
            Tr("Visually exaggerates the DOF blur range to make terrain look like miniature photography. Physical camera settings remain unchanged.",
                "地形をミニチュア撮影のように見せるため、DOF のぼけ範囲を視覚的に強調します。物理カメラ設定はそのまま残し、表示だけを調整します。"),
            rock::PreviewSettings{}.dofMiniatureEnabled, true);
        ImGui::BeginDisabled(!preview.dofMiniatureEnabled);
        DrawPropertyFloatRow("Miniature Scale", "DofMiniatureScale", &preview.dofMiniatureScale, 1.0f, 50.0f, rock::PreviewSettings{}.dofMiniatureScale, "Depth of Field miniature scale changed", false,
            Tr("Multiplier applied to the DOF blur range. Larger values create a shallower miniature-style focus band.",
                "DOF のぼけ範囲に掛ける倍率です。大きいほどミニチュア風の浅い焦点幅になります。"), "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
        int apertureShape = std::clamp(preview.dofApertureShape, 0, 4);
        if (DrawPropertyComboRow(Tr("Aperture Shape", "絞り形状"), "DofApertureShape", &apertureShape, Tr("Circle\0Triangle\0Hexagon\0Octagon\0Custom\0\0", "丸\0三角形\0六角形\0八角形\0カスタム\0\0"),
            Tr("Sample shape for blur. Polygon shapes create angular bokeh based on aperture blades.",
                "ぼけのサンプル形状です。多角形にすると絞り羽根由来の角の立ったボケになります。"), rock::PreviewSettings{}.dofApertureShape))
        {
            preview.dofApertureShape = std::clamp(apertureShape, 0, 4);
            if (state.markGraphChanged)
            {
                state.markGraphChanged("Depth of Field aperture shape changed");
            }
        }
        ImGui::BeginDisabled(preview.dofApertureShape != 4);
        DrawPropertyIntRow(Tr("Aperture Blades", "絞り羽根"), "DofApertureBlades", &preview.dofApertureBlades, 3, 12, rock::PreviewSettings{}.dofApertureBlades, "Depth of Field aperture blades changed", false,
            Tr("Number of blades for custom polygon bokeh.",
                "カスタム多角形ボケの羽根数です。"));
        ImGui::EndDisabled();
        DrawPropertyFloatRow(Tr("Aperture Rotation (deg)", "絞り回転 (deg)"), "DofApertureRotation", &preview.dofApertureRotationDegrees, -180.0f, 180.0f, rock::PreviewSettings{}.dofApertureRotationDegrees, "Depth of Field aperture rotation changed", false,
            Tr("Angle of polygon bokeh. It has almost no visible effect for circular bokeh.",
                "多角形ボケの角度です。丸ボケでは見た目にほぼ影響しません。"), "%.1f");
        DrawPropertyFloatRow(Tr("Highlight Boost", "ハイライト強調"), "DofHighlightBoost", &preview.dofHighlightBoost, 0.0f, 4.0f, rock::PreviewSettings{}.dofHighlightBoost, "Depth of Field highlight boost changed", false,
            Tr("Slightly boosts bright samples so point lights and bright edges stand out in the blur.",
                "明るいサンプルを少し強め、点光源や明るい輪郭のボケを目立たせます。"), "%.2f");
        ImGui::EndDisabled();
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
