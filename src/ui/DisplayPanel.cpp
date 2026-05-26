#include "DisplayPanel.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <string>

#include <imgui.h>

#include "Localization.h"
#include "PropertyWidgets.h"

namespace terrain::ui
{
namespace
{
constexpr std::array<int, 4> kTerrainSizePresets = {512, 1024, 2048, 4096};
constexpr std::array<int, 6> kResolutionPresets = {128, 256, 512, 1024, 2048, 4096};
constexpr std::array<int, 3> kFrameRateLimitPresets = {0, 60, 30};

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

enum class ViewportDisplayMode
{
    Simple,
    Pbr,
    Sky,
};

template <size_t N>
int NearestPreset(int value, const std::array<int, N>& presets, int fallback)
{
    const auto nearest = std::ranges::min_element(presets, [value](int lhs, int rhs) {
        return std::abs(lhs - value) < std::abs(rhs - value);
    });
    return nearest != presets.end() ? *nearest : fallback;
}

int NearestTerrainSizePreset(float value)
{
    return NearestPreset(static_cast<int>(std::round(value)), kTerrainSizePresets, 1024);
}

bool DrawResolutionPresetRow(const char* label, const char* id, int* value, int defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr)
{
    return DrawPresetIntRow(label, id, value, defaultValue, kResolutionPresets, 512, dirtyReason, recordUndo, tooltip);
}

bool DrawTerrainSizePresetRow(const char* label, const char* id, float* value, float defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr)
{
    int intValue = NearestTerrainSizePreset(*value);
    const int intDefault = NearestTerrainSizePreset(defaultValue);
    const bool changed = DrawPresetIntRow(label, id, &intValue, intDefault, kTerrainSizePresets, 1024, dirtyReason, recordUndo, tooltip);
    *value = static_cast<float>(intValue);
    return changed;
}

const char* FrameRateLimitLabel(int limitFps)
{
    switch (limitFps)
    {
    case 60:
        return "60 FPS";
    case 30:
        return "30 FPS";
    default:
        return Tr("Unlimited", "上限なし");
    }
}

bool DrawFrameRateLimitRow(const char* label, const char* id, int* value, const char* tooltip)
{
    int currentIndex = 0;
    for (int i = 0; i < static_cast<int>(kFrameRateLimitPresets.size()); ++i)
    {
        if (*value == kFrameRateLimitPresets[static_cast<size_t>(i)])
        {
            currentIndex = i;
            break;
        }
    }

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("%s", tooltip);
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::PushID(id);
    bool changed = false;
    if (ImGui::BeginCombo("##FrameRateLimit", FrameRateLimitLabel(kFrameRateLimitPresets[static_cast<size_t>(currentIndex)])))
    {
        for (int i = 0; i < static_cast<int>(kFrameRateLimitPresets.size()); ++i)
        {
            const int preset = kFrameRateLimitPresets[static_cast<size_t>(i)];
            const bool selected = (i == currentIndex);
            if (ImGui::Selectable(FrameRateLimitLabel(preset), selected))
            {
                *value = preset;
                changed = true;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return changed;
}

void SaveAppSettings(const DisplayPanelState& state)
{
    if (state.saveAppSettings)
    {
        state.saveAppSettings();
    }
}

void EvaluateGraph(const DisplayPanelState& state)
{
    if (state.evaluateGraph)
    {
        state.evaluateGraph();
    }
}

void MarkGraphChanged(const DisplayPanelState& state, const char* reason)
{
    if (state.markGraphChanged)
    {
        state.markGraphChanged(reason);
    }
}

float DefaultViewportOrbitDistance(const DisplayPanelState& state)
{
    return state.defaultViewportOrbitDistance ? state.defaultViewportOrbitDistance() : state.orbitDistance;
}

ViewportDisplayMode CurrentViewportDisplayMode(const rock::GraphSettings& settings)
{
    if (settings.sky.mode == rock::SkyMode::Atmospheric)
    {
        return ViewportDisplayMode::Sky;
    }
    if (settings.preview.lightingMode >= 1)
    {
        return ViewportDisplayMode::Pbr;
    }
    return ViewportDisplayMode::Simple;
}

int ToDisplayModeIndex(ViewportDisplayMode mode)
{
    switch (mode)
    {
    case ViewportDisplayMode::Pbr:
        return 1;
    case ViewportDisplayMode::Sky:
        return 2;
    case ViewportDisplayMode::Simple:
    default:
        return 0;
    }
}

ViewportDisplayMode DisplayModeFromIndex(int index)
{
    switch (index)
    {
    case 1:
        return ViewportDisplayMode::Pbr;
    case 2:
        return ViewportDisplayMode::Sky;
    default:
        return ViewportDisplayMode::Simple;
    }
}

void ApplyViewportDisplayMode(rock::GraphSettings& settings, ViewportDisplayMode mode)
{
    switch (mode)
    {
    case ViewportDisplayMode::Simple:
        settings.preview.lightingMode = 0;
        settings.sky.mode = rock::SkyMode::SolidColor;
        break;
    case ViewportDisplayMode::Pbr:
        settings.preview.lightingMode = 1;
        settings.sky.mode = rock::SkyMode::SolidColor;
        break;
    case ViewportDisplayMode::Sky:
        settings.preview.lightingMode = 1;
        settings.sky.mode = rock::SkyMode::Atmospheric;
        break;
    }
}
} // namespace
void DrawDisplaySettingsPanel(DisplayPanelState state)
{
    rock::GraphSettings& settings = state.settings;
    const float headerRightPadding = 10.0f;
    const float sectionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - headerRightPadding);
    ImGui::BeginChild("PreviewDisplaySection", ImVec2(sectionWidth, 0.0f), false);
    const std::string settingsHeaderLabel = StableImGuiLabel(Tr("Settings", "設定"), "DisplaySettingsHeader");
    if (!ImGui::CollapsingHeader(settingsHeaderLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("PreviewDisplaySettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::SeparatorText(Tr("Resolution", "解像度"));
        const float defaultDistanceBeforeTerrainSizeEdit = DefaultViewportOrbitDistance(state);
        if (DrawTerrainSizePresetRow("Terrain Size (m)", "GlobalTerrainSizeMeters", &settings.preview.terrainSizeMeters, rock::PreviewSettings{}.terrainSizeMeters, "Terrain size changed", false, Tr("The width and depth of the terrain canvas used by the whole node graph. Import Heightmap Scale is interpreted as the image's real size within this area.", "ノードグラフ全体の地形キャンバスの縦横サイズです。Import Heightmap の Scale はこの中で画像が占める実サイズとして扱い、大きければクロップ、小さければ外側を高さ 0 にします。")))
        {
            if (!FloatDiffersFromDefault(state.orbitDistance, defaultDistanceBeforeTerrainSizeEdit))
            {
                state.orbitDistance = DefaultViewportOrbitDistance(state);
            }
            MarkGraphChanged(state, "Terrain size changed");
            EvaluateGraph(state);
        }
        if (DrawResolutionPresetRow("Simulation Resolution", "GlobalSimulationResolution", &settings.preview.simulationResolution, rock::PreviewSettings{}.simulationResolution, "Simulation resolution changed", false, Tr("Evaluation resolution for terrain and masks across the node graph. Higher values preserve more detail but increase evaluation time and memory use.", "ノードグラフ全体の地形・マスク評価解像度です。高いほど細かく計算できますが、評価時間とメモリ使用量が増えます。")))
        {
            MarkGraphChanged(state, "Simulation resolution changed");
            EvaluateGraph(state);
        }
        if (DrawResolutionPresetRow("Viewport Mesh Resolution", "DisplayPreviewResolution", &settings.preview.resolution, rock::PreviewSettings{}.resolution, "Preview mesh resolution changed", false, Tr("Mesh density for the 3D preview. This changes only the displayed subdivisions, not the Simulation Resolution.", "3D プレビュー用メッシュの細かさです。Simulation Resolution は変えず、表示の分割数だけを変更します。")))
        {
            EvaluateGraph(state);
            SaveAppSettings(state);
        }
        if (DrawPropertyIntRow("LOD", "DisplayPreviewLod", &settings.preview.lod, 0, 4, rock::PreviewSettings{}.lod, "Preview LOD changed", false))
        {
            EvaluateGraph(state);
            SaveAppSettings(state);
        }

        ImGui::SeparatorText(Tr("Preview", "プレビュー画面"));
        if (DrawPropertyBoolRow("Mesh Preview", "DisplayMeshPreview", &state.meshPreview, "Mesh preview visibility changed", nullptr, true, true))
        {
            SaveAppSettings(state);
        }
        if (DrawPropertyBoolRow("FPS", "DisplayFps", &state.showFps, "FPS visibility changed", nullptr, true, true))
        {
            SaveAppSettings(state);
        }
        if (DrawFrameRateLimitRow("FPS Limit", "DisplayFrameRateLimit", &settings.preview.frameRateLimitFps, Tr("Frame-rate cap for the whole app, including the 3D viewport. Unlimited does not add an app-side wait.", "3D ビューポートを含むアプリ全体の描画更新上限です。上限なしではアプリ側の待ちを入れません。")))
        {
            SaveAppSettings(state);
        }
        {
            int backendInt = static_cast<int>(settings.preview.meshBackend);
            if (DrawPropertyComboRow("Mesh Backend", "DisplayMeshBackend", &backendInt, "CPU Mesh\0GPU Displacement\0\0", Tr("Rendering backend for the 3D preview. CPU Mesh builds and uploads mesh data on the CPU. GPU Displacement uses a static UV grid and a height texture in the vertex shader for faster parameter updates.", "プレビュー 3D ビューポートのレンダリング経路。CPU Mesh は CPU 側でメッシュを生成・アップロード(従来動作)、GPU Displacement は静的 UV グリッド + ハイトテクスチャを頂点シェーダーで displace します。GPU 側はテクスチャアップロード(~数 ms)だけで済むため、パラメータ変更時の応答性が上がります(現状はサーフェス描画のみ。シャドウ・ワイヤフレームは CPU パスを併走させます)。"), static_cast<int>(rock::PreviewSettings{}.meshBackend)))
            {
                settings.preview.meshBackend = static_cast<rock::MeshPreviewBackend>(std::clamp(backendInt,
                    static_cast<int>(rock::MeshPreviewBackend::CpuMesh),
                    static_cast<int>(rock::MeshPreviewBackend::GpuDisplacement)));
                SaveAppSettings(state);
            }
        }
        if (settings.preview.meshBackend == rock::MeshPreviewBackend::GpuDisplacement)
        {
            if (DrawPropertyBoolRow("Tessellation", "DisplayViewportTessellation", &settings.preview.viewportTessellation, "Viewport tessellation changed", Tr("Subdivides only the GPU Displacement viewport rendering with hardware tessellation. Node evaluation and exported meshes are not affected.", "GPU Displacement のビューポート描画だけをハードウェアテセレーションで細分化します。ノード評価やエクスポート用メッシュには影響しません。"), rock::PreviewSettings{}.viewportTessellation, true))
            {
                SaveAppSettings(state);
            }
            if (settings.preview.viewportTessellation)
            {
                if (DrawPropertyFloatRow("Tess Min", "DisplayTessMin", &settings.preview.tessellationMinFactor, 1.0f, 16.0f, rock::PreviewSettings{}.tessellationMinFactor, "Tessellation min changed", false, Tr("Minimum tessellation factor used in the distance.", "遠景で使う最小テセレーション係数です。")))
                {
                    settings.preview.tessellationMinFactor = std::clamp(settings.preview.tessellationMinFactor, 1.0f, 64.0f);
                    settings.preview.tessellationMaxFactor = std::max(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Tess Max", "DisplayTessMax", &settings.preview.tessellationMaxFactor, 1.0f, 32.0f, rock::PreviewSettings{}.tessellationMaxFactor, "Tessellation max changed", false, Tr("Maximum tessellation factor used nearby. Higher values look smoother but cost more to render.", "近景で使う最大テセレーション係数です。高いほど滑らかになりますが描画負荷が増えます。")))
                {
                    settings.preview.tessellationMaxFactor = std::clamp(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor, 64.0f);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Tess Near (m)", "DisplayTessNear", &settings.preview.tessellationNearDistance, 1.0f, 20000.0f, rock::PreviewSettings{}.tessellationNearDistance, "Tessellation near changed", false, Tr("Uses the maximum tessellation factor up to this distance.", "この距離までは最大テセレーション係数を使います。"), "%.0f"))
                {
                    settings.preview.tessellationNearDistance = std::clamp(settings.preview.tessellationNearDistance, 1.0f, 100000.0f);
                    settings.preview.tessellationFarDistance = std::max(settings.preview.tessellationFarDistance, settings.preview.tessellationNearDistance + 1.0f);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Tess Far (m)", "DisplayTessFar", &settings.preview.tessellationFarDistance, 1.0f, 50000.0f, rock::PreviewSettings{}.tessellationFarDistance, "Tessellation far changed", false, Tr("Falls back to the minimum tessellation factor beyond this distance.", "この距離以遠では最小テセレーション係数へ落とします。"), "%.0f"))
                {
                    settings.preview.tessellationFarDistance = std::clamp(settings.preview.tessellationFarDistance, settings.preview.tessellationNearDistance + 1.0f, 200000.0f);
                    SaveAppSettings(state);
                }
            }
        }

        if (DrawPropertyBoolRow("Surface", "DisplaySurface", &settings.preview.showSurface, "Surface visibility changed", nullptr, rock::PreviewSettings{}.showSurface, true))
        {
            SaveAppSettings(state);
        }
        {
            int boundaryModeInt = static_cast<int>(settings.preview.terrainBoundaryMode);
            if (DrawPropertyComboRow(Tr("Terrain Boundary", "地形境界"), "DisplayTerrainBoundaryMode", &boundaryModeInt, Tr("None\0Section Polygon\0Lines\0\0", "なし\0断面ポリゴン\0ライン\0\0"),
                Tr("Controls the terrain outer edge display. Section Polygon draws sides and a bottom face. Lines draw only vertical corner lines and a bottom square.", "地形外周の表示です。断面ポリゴンは側面と底面を描画し、ラインは四隅から高さ 0 への縦線と下端の正方形だけを表示します。"),
                static_cast<int>(rock::PreviewSettings{}.terrainBoundaryMode)))
            {
                settings.preview.terrainBoundaryMode = static_cast<rock::TerrainBoundaryMode>(std::clamp(boundaryModeInt,
                    static_cast<int>(rock::TerrainBoundaryMode::None),
                    static_cast<int>(rock::TerrainBoundaryMode::Lines)));
                SaveAppSettings(state);
            }
        }
        if (DrawPropertyBoolRow("Grid", "DisplayGrid", &settings.preview.showGrid, "Grid visibility changed", nullptr, rock::PreviewSettings{}.showGrid, true))
        {
            SaveAppSettings(state);
        }
        if (settings.preview.showGrid)
        {
            if (DrawPropertyIntRow("Grid Cells", "DisplayGridCells", &settings.preview.gridCellCount, 1, 200, rock::PreviewSettings{}.gridCellCount, "Grid cell count changed", false, Tr("Number of cells per side for the whole grid. 10 means a 10 x 10 grid.", "グリッド全体の1辺あたりのマス数です。10なら10 x 10です。")))
            {
                settings.preview.gridCellCount = std::clamp(settings.preview.gridCellCount, 1, 200);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Grid Cell Size (m)", "DisplayGridCellSize", &settings.preview.gridCellSizeMeters, 1.0f, 10000.0f, rock::PreviewSettings{}.gridCellSizeMeters, "Grid cell size changed", false, Tr("Length of one grid cell.", "グリッド1マスの長さです。")))
            {
                settings.preview.gridCellSizeMeters = std::clamp(settings.preview.gridCellSizeMeters, 1.0f, 10000.0f);
                SaveAppSettings(state);
            }
            if (DrawColorRgbRow("Grid Color", "DisplayGridColor", settings.preview.gridColor, rock::PreviewSettings{}.gridColor))
            {
                SaveAppSettings(state);
            }
        }
        int displayModeInt = ToDisplayModeIndex(CurrentViewportDisplayMode(settings));
        if (DrawPropertyComboRow(Tr("Display Mode", "表示モード"), "ViewportDisplayMode", &displayModeInt, Tr("Simple\0PBR\0Sky\0\0", "シンプル\0PBR\0天球\0\0"), Tr("Simple: flat and lightweight. PBR: realistic lighting with a solid background. Sky: sky background with realistic lighting.", "シンプル: フラットで軽い表示。PBR: 単色背景でリアル寄りのライティング。天球: 天球背景とリアル寄りのライティングです。"), ToDisplayModeIndex(ViewportDisplayMode::Simple)))
        {
            ApplyViewportDisplayMode(settings, DisplayModeFromIndex(std::clamp(displayModeInt, 0, 2)));
            SaveAppSettings(state);
        }
        const ViewportDisplayMode displayMode = CurrentViewportDisplayMode(settings);
        ImGui::SeparatorText(Tr("Surface", "地表"));
        if (displayMode != ViewportDisplayMode::Sky)
        {
            if (DrawColorRgbRow(Tr("Viewport Background", "ビューポート背景色"), "ViewportBackgroundColor", settings.preview.viewportBackground, rock::PreviewSettings{}.viewportBackground))
            {
                SaveAppSettings(state);
            }
        }
        if (displayMode != ViewportDisplayMode::Simple)
        {
            if (DrawColorRgbRow("Albedo", "DisplayPbrAlbedo", settings.preview.pbrAlbedo, rock::PreviewSettings{}.pbrAlbedo))
            {
                SaveAppSettings(state);
            }
        }

        ImGui::SeparatorText("HDR");
        if (DrawPropertyBoolRow("HDR Viewport", "DisplayHdrViewportEnabled", &settings.preview.hdrViewportEnabled, "HDR viewport toggled",
            Tr("Uses a 16-bit float HDR viewport buffer with tonemapping and exposure controls. Turn this off to use the pre-HDR 8-bit viewport path.", "16-bit float の HDR ビューポートバッファを使い、トーンマップと露出調整を有効にします。オフにすると HDR 以前の 8-bit ビューポート経路を使います。"),
            rock::PreviewSettings{}.hdrViewportEnabled, true))
        {
            SaveAppSettings(state);
        }

        if (settings.preview.hdrViewportEnabled)
        {
            ImGui::SeparatorText(Tr("Exposure", "露出"));
            {
                int exposureModeInt = static_cast<int>(settings.preview.exposureMode);
                if (DrawPropertyComboRow(Tr("Mode", "モード"), "DisplayExposureMode", &exposureModeInt, Tr("Manual\0Auto\0\0", "Manual\0Auto\0\0"),
                    Tr("Manual uses the Exposure EV value directly. Auto samples the HDR viewport and adjusts exposure into the selected EV range.", "Manual は Exposure EV をそのまま使います。Auto は HDR ビューポートをサンプリングし、指定した EV 範囲内で露出を自動調整します。"),
                    static_cast<int>(rock::PreviewSettings{}.exposureMode)))
                {
                    settings.preview.exposureMode = static_cast<rock::ExposureMode>(std::clamp(exposureModeInt,
                        static_cast<int>(rock::ExposureMode::Manual),
                        static_cast<int>(rock::ExposureMode::Auto)));
                    SaveAppSettings(state);
                }
            }
            if (settings.preview.exposureMode == rock::ExposureMode::Manual)
            {
                if (DrawPropertyFloatRow("Exposure EV", "DisplayExposureEv", &settings.preview.exposureEv, -8.0f, 8.0f, rock::PreviewSettings{}.exposureEv, "Exposure changed", false,
                    Tr("Manual exposure compensation in stops. Positive values brighten the viewport; negative values darken it.", "手動露出補正です。正の値で明るく、負の値で暗くします。"), "%.2f"))
                {
                    SaveAppSettings(state);
                }
            }
            else
            {
                if (DrawPropertyFloatRow("Bias EV", "DisplayAutoExposureBiasEv", &settings.preview.autoExposureBiasEv, -4.0f, 4.0f, rock::PreviewSettings{}.autoExposureBiasEv, "Auto exposure bias changed", false,
                    Tr("Exposure compensation applied after auto metering.", "自動測光後に加える露出補正です。"), "%.2f"))
                {
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Min EV", "DisplayAutoExposureMinEv", &settings.preview.autoExposureMinEv, -8.0f, 8.0f, rock::PreviewSettings{}.autoExposureMinEv, "Auto exposure min changed", false,
                    Tr("Darkest exposure allowed for auto exposure.", "自動露出で許可する最も暗い露出です。"), "%.2f"))
                {
                    settings.preview.autoExposureMinEv = std::clamp(settings.preview.autoExposureMinEv, -8.0f, 8.0f);
                    settings.preview.autoExposureMaxEv = std::max(settings.preview.autoExposureMaxEv, settings.preview.autoExposureMinEv);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Max EV", "DisplayAutoExposureMaxEv", &settings.preview.autoExposureMaxEv, -8.0f, 8.0f, rock::PreviewSettings{}.autoExposureMaxEv, "Auto exposure max changed", false,
                    Tr("Brightest exposure allowed for auto exposure.", "自動露出で許可する最も明るい露出です。"), "%.2f"))
                {
                    settings.preview.autoExposureMaxEv = std::clamp(settings.preview.autoExposureMaxEv, settings.preview.autoExposureMinEv, 8.0f);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Speed", "DisplayAutoExposureSpeed", &settings.preview.autoExposureSpeed, 0.05f, 8.0f, rock::PreviewSettings{}.autoExposureSpeed, "Auto exposure speed changed", false,
                    Tr("How quickly auto exposure adapts. Lower values make transitions slower.", "自動露出が追従する速さです。小さいほど露出変化がゆっくりになります。"), "%.2f"))
                {
                    settings.preview.autoExposureSpeed = std::clamp(settings.preview.autoExposureSpeed, 0.05f, 8.0f);
                    SaveAppSettings(state);
                }
            }
        }

        ImGui::SeparatorText(Tr("Ambient Occlusion", "アンビエントオクルージョン"));
        if (DrawPropertyBoolRow("AO", "DisplayAOEnabled", &settings.preview.aoEnabled, "AO toggled", Tr("Static ambient occlusion computed from the heightfield horizon angle. It darkens valleys and concave areas and is recalculated when the heightfield changes.", "ハイトフィールドから水平線仰角を計算した静的アンビエントオクルージョンです。谷や凹部のアンビエントライトを遮蔽します。ハイトフィールドが変わると自動で再計算されます。"), rock::PreviewSettings{}.aoEnabled, true))
        {
            SaveAppSettings(state);
        }
        if (settings.preview.aoEnabled)
        {
            if (DrawPropertyFloatRow(Tr("AO Strength", "AO 強度"), "DisplayAOStrength", &settings.preview.aoStrength, 0.0f, 1.0f, rock::PreviewSettings{}.aoStrength, "AO strength changed", false, Tr("How strongly AO darkens the ambient term. 0 disables it; 1 is maximum.", "AO がアンビエント項を暗化する強度です。0 で無効、1 で最大。")))
            {
                settings.preview.aoStrength = std::clamp(settings.preview.aoStrength, 0.0f, 1.0f);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow(Tr("AO Radius (m)", "AO 半径 (m)"), "DisplayAORadius", &settings.preview.aoRadius, 10.0f, 1000.0f, rock::PreviewSettings{}.aoRadius, "AO radius changed", false, Tr("Maximum radius sampled for occlusion. Small values affect only local pits and detail; large values reach valley and basin scales.", "遮蔽をサンプリングする最大半径です。小さいと岩の窪みや細部のみ暗く、大きいと谷や盆地スケールまで暗くなります。変更時は AO を再計算します。"), "%.0f"))
            {
                settings.preview.aoRadius = std::clamp(settings.preview.aoRadius, 10.0f, 5000.0f);
                SaveAppSettings(state);
            }
        }

        ImGui::SeparatorText(Tr("Mask Texture", "マスクテクスチャー"));
        {
            int maskShadingInt = static_cast<int>(settings.preview.maskShading);
            if (DrawPropertyComboRow(Tr("Mask Shading", "マスクシェーディング"), "DisplayMaskShading", &maskShadingInt, Tr("Grayscale\0Gray + Orange\0Grayscale + Hatching\0\0", "グレースケール\0グレー×オレンジ\0グレースケール + 斜線\0\0"), Tr("Display style for mask previews. Grayscale is a pure black-to-white ramp. Gray + Orange adds lighting. Grayscale + Hatching adds a GeoGen-style diagonal hatch overlay.", "マスクプレビューの表示方式です。グレースケール: mask=0→黒, mask=1→白の純粋な白黒ランプ (既定)。グレー×オレンジ: ライティング付きのグレー×オレンジ調シェーディング。グレースケール + 斜線: GeoGen 風の対角ハッチング — mask が 1.0 付近では密な白斜線、0.0 付近では疎な白斜線、中間は素直なランプ。3D ビューと 2D マップ両方に反映されます。"), static_cast<int>(rock::PreviewSettings{}.maskShading)))
            {
                settings.preview.maskShading = static_cast<rock::MaskShadingMode>(std::clamp(maskShadingInt,
                    static_cast<int>(rock::MaskShadingMode::Grayscale),
                    static_cast<int>(rock::MaskShadingMode::GrayscaleHatched)));
                SaveAppSettings(state);
            }
            if (DrawPropertyBoolRow(Tr("Use Nearest Heightmap", "近い地形でマスク表示"), "DisplayMaskUseNearestHeightmap", &settings.preview.maskPreviewUseNearestHeightmap, "Mask preview nearest heightmap toggled", Tr("When previewing mask nodes without a direct heightmap reference, use the nearest upstream Heightmap as the display terrain. Falls back to a plane when none is found.", "Mask Noise / Mask Blend / Mask Levels など、ハイトマップ参照を直接持たないマスクノードをプレビューするとき、入力側をたどって見つかった一番近い Heightmap を表示用の地形に使います。見つからない場合は従来どおり平面表示します。"), rock::PreviewSettings{}.maskPreviewUseNearestHeightmap, true))
            {
                EvaluateGraph(state);
                SaveAppSettings(state);
            }
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();
}


} // namespace terrain::ui
