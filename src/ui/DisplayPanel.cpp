#include "DisplayPanel.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

#include <imgui.h>

#include "PropertyWidgets.h"

namespace terrain::ui
{
namespace
{
constexpr std::array<int, 4> kTerrainSizePresets = {512, 1024, 2048, 4096};
constexpr std::array<int, 5> kResolutionPresets = {128, 256, 512, 1024, 2048};
constexpr std::array<int, 3> kFrameRateLimitPresets = {0, 60, 30};

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
        return "上限なし";
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
    if (!ImGui::CollapsingHeader("設定", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("PreviewDisplaySettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::SeparatorText("解像度");
        const float defaultDistanceBeforeTerrainSizeEdit = DefaultViewportOrbitDistance(state);
        if (DrawTerrainSizePresetRow("Terrain Size (m)", "GlobalTerrainSizeMeters", &settings.preview.terrainSizeMeters, rock::PreviewSettings{}.terrainSizeMeters, "Terrain size changed", false, "ノードグラフ全体の地形キャンバスの縦横サイズです。Import Heightmap の Scale はこの中で画像が占める実サイズとして扱い、大きければクロップ、小さければ外側を高さ 0 にします。"))
        {
            if (!FloatDiffersFromDefault(state.orbitDistance, defaultDistanceBeforeTerrainSizeEdit))
            {
                state.orbitDistance = DefaultViewportOrbitDistance(state);
            }
            MarkGraphChanged(state, "Terrain size changed");
            EvaluateGraph(state);
        }
        if (DrawResolutionPresetRow("Simulation Resolution", "GlobalSimulationResolution", &settings.preview.simulationResolution, rock::PreviewSettings{}.simulationResolution, "Simulation resolution changed", false, "ノードグラフ全体の地形・マスク評価解像度です。高いほど細かく計算できますが、評価時間とメモリ使用量が増えます。"))
        {
            MarkGraphChanged(state, "Simulation resolution changed");
            EvaluateGraph(state);
        }
        if (DrawResolutionPresetRow("Viewport Mesh Resolution", "DisplayPreviewResolution", &settings.preview.resolution, rock::PreviewSettings{}.resolution, "Preview mesh resolution changed", false, "3D プレビュー用メッシュの細かさです。Simulation Resolution は変えず、表示の分割数だけを変更します。"))
        {
            EvaluateGraph(state);
            SaveAppSettings(state);
        }
        if (DrawPropertyIntRow("LOD", "DisplayPreviewLod", &settings.preview.lod, 0, 4, rock::PreviewSettings{}.lod, "Preview LOD changed", false))
        {
            EvaluateGraph(state);
            SaveAppSettings(state);
        }

        ImGui::SeparatorText("プレビュー画面");
        if (DrawPropertyBoolRow("Mesh Preview", "DisplayMeshPreview", &state.meshPreview, "Mesh preview visibility changed", nullptr, true, true))
        {
            SaveAppSettings(state);
        }
        if (DrawPropertyBoolRow("FPS", "DisplayFps", &state.showFps, "FPS visibility changed", nullptr, true, true))
        {
            SaveAppSettings(state);
        }
        if (DrawFrameRateLimitRow("FPS Limit", "DisplayFrameRateLimit", &settings.preview.frameRateLimitFps, "3D ビューポートを含むアプリ全体の描画更新上限です。上限なしではアプリ側の待ちを入れません。"))
        {
            SaveAppSettings(state);
        }
        {
            int backendInt = static_cast<int>(settings.preview.meshBackend);
            if (DrawPropertyComboRow("Mesh Backend", "DisplayMeshBackend", &backendInt, "CPU Mesh\0GPU Displacement\0\0", "プレビュー 3D ビューポートのレンダリング経路。CPU Mesh は CPU 側でメッシュを生成・アップロード(従来動作)、GPU Displacement は静的 UV グリッド + ハイトテクスチャを頂点シェーダーで displace します。GPU 側はテクスチャアップロード(~数 ms)だけで済むため、パラメータ変更時の応答性が上がります(現状はサーフェス描画のみ。シャドウ・ワイヤフレームは CPU パスを併走させます)。", static_cast<int>(rock::PreviewSettings{}.meshBackend)))
            {
                settings.preview.meshBackend = static_cast<rock::MeshPreviewBackend>(std::clamp(backendInt,
                    static_cast<int>(rock::MeshPreviewBackend::CpuMesh),
                    static_cast<int>(rock::MeshPreviewBackend::GpuDisplacement)));
                SaveAppSettings(state);
            }
        }
        if (settings.preview.meshBackend == rock::MeshPreviewBackend::GpuDisplacement)
        {
            if (DrawPropertyBoolRow("Tessellation", "DisplayViewportTessellation", &settings.preview.viewportTessellation, "Viewport tessellation changed", "GPU Displacement のビューポート描画だけをハードウェアテセレーションで細分化します。ノード評価やエクスポート用メッシュには影響しません。", rock::PreviewSettings{}.viewportTessellation, true))
            {
                SaveAppSettings(state);
            }
            if (settings.preview.viewportTessellation)
            {
                if (DrawPropertyFloatRow("Tess Min", "DisplayTessMin", &settings.preview.tessellationMinFactor, 1.0f, 16.0f, rock::PreviewSettings{}.tessellationMinFactor, "Tessellation min changed", false, "遠景で使う最小テセレーション係数です。"))
                {
                    settings.preview.tessellationMinFactor = std::clamp(settings.preview.tessellationMinFactor, 1.0f, 64.0f);
                    settings.preview.tessellationMaxFactor = std::max(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Tess Max", "DisplayTessMax", &settings.preview.tessellationMaxFactor, 1.0f, 32.0f, rock::PreviewSettings{}.tessellationMaxFactor, "Tessellation max changed", false, "近景で使う最大テセレーション係数です。高いほど滑らかになりますが描画負荷が増えます。"))
                {
                    settings.preview.tessellationMaxFactor = std::clamp(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor, 64.0f);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Tess Near (m)", "DisplayTessNear", &settings.preview.tessellationNearDistance, 1.0f, 20000.0f, rock::PreviewSettings{}.tessellationNearDistance, "Tessellation near changed", false, "この距離までは最大テセレーション係数を使います。", "%.0f"))
                {
                    settings.preview.tessellationNearDistance = std::clamp(settings.preview.tessellationNearDistance, 1.0f, 100000.0f);
                    settings.preview.tessellationFarDistance = std::max(settings.preview.tessellationFarDistance, settings.preview.tessellationNearDistance + 1.0f);
                    SaveAppSettings(state);
                }
                if (DrawPropertyFloatRow("Tess Far (m)", "DisplayTessFar", &settings.preview.tessellationFarDistance, 1.0f, 50000.0f, rock::PreviewSettings{}.tessellationFarDistance, "Tessellation far changed", false, "この距離以遠では最小テセレーション係数へ落とします。", "%.0f"))
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
            if (DrawPropertyComboRow("地形境界", "DisplayTerrainBoundaryMode", &boundaryModeInt, "なし\0断面ポリゴン\0ライン\0\0",
                "地形外周の表示です。断面ポリゴンは側面と底面を描画し、ラインは四隅から高さ 0 への縦線と下端の正方形だけを表示します。",
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
            if (DrawPropertyIntRow("Grid Cells", "DisplayGridCells", &settings.preview.gridCellCount, 1, 200, rock::PreviewSettings{}.gridCellCount, "Grid cell count changed", false, "グリッド全体の1辺あたりのマス数です。10なら10 x 10です。"))
            {
                settings.preview.gridCellCount = std::clamp(settings.preview.gridCellCount, 1, 200);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("Grid Cell Size (m)", "DisplayGridCellSize", &settings.preview.gridCellSizeMeters, 1.0f, 10000.0f, rock::PreviewSettings{}.gridCellSizeMeters, "Grid cell size changed", false, "グリッド1マスの長さです。"))
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
        if (DrawPropertyComboRow("表示モード", "ViewportDisplayMode", &displayModeInt, "シンプル\0PBR\0天球\0\0", "シンプル: フラットで軽い表示。PBR: 単色背景でリアル寄りのライティング。天球: 天球背景とリアル寄りのライティングです。", ToDisplayModeIndex(ViewportDisplayMode::Simple)))
        {
            ApplyViewportDisplayMode(settings, DisplayModeFromIndex(std::clamp(displayModeInt, 0, 2)));
            SaveAppSettings(state);
        }
        const ViewportDisplayMode displayMode = CurrentViewportDisplayMode(settings);
        ImGui::SeparatorText("地表");
        if (displayMode != ViewportDisplayMode::Sky)
        {
            if (DrawColorRgbRow("ビューポート背景色", "ViewportBackgroundColor", settings.preview.viewportBackground, rock::PreviewSettings{}.viewportBackground))
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

        ImGui::SeparatorText("アンビエントオクルージョン");
        if (DrawPropertyBoolRow("AO", "DisplayAOEnabled", &settings.preview.aoEnabled, "AO toggled", "ハイトフィールドから水平線仰角を計算した静的アンビエントオクルージョンです。谷や凹部のアンビエントライトを遮蔽します。ハイトフィールドが変わると自動で再計算されます。", rock::PreviewSettings{}.aoEnabled, true))
        {
            SaveAppSettings(state);
        }
        if (settings.preview.aoEnabled)
        {
            if (DrawPropertyFloatRow("AO 強度", "DisplayAOStrength", &settings.preview.aoStrength, 0.0f, 1.0f, rock::PreviewSettings{}.aoStrength, "AO strength changed", false, "AO がアンビエント項を暗化する強度です。0 で無効、1 で最大。"))
            {
                settings.preview.aoStrength = std::clamp(settings.preview.aoStrength, 0.0f, 1.0f);
                SaveAppSettings(state);
            }
            if (DrawPropertyFloatRow("AO 半径 (m)", "DisplayAORadius", &settings.preview.aoRadius, 10.0f, 1000.0f, rock::PreviewSettings{}.aoRadius, "AO radius changed", false, "遮蔽をサンプリングする最大半径です。小さいと岩の窪みや細部のみ暗く、大きいと谷や盆地スケールまで暗くなります。変更時は AO を再計算します。", "%.0f"))
            {
                settings.preview.aoRadius = std::clamp(settings.preview.aoRadius, 10.0f, 5000.0f);
                SaveAppSettings(state);
            }
        }

        ImGui::SeparatorText("マスクテクスチャー");
        {
            int maskShadingInt = static_cast<int>(settings.preview.maskShading);
            if (DrawPropertyComboRow("マスクシェーディング", "DisplayMaskShading", &maskShadingInt, "グレースケール\0グレー×オレンジ\0グレースケール + 斜線\0\0", "マスクプレビューの表示方式です。グレースケール: mask=0→黒, mask=1→白の純粋な白黒ランプ (既定)。グレー×オレンジ: ライティング付きのグレー×オレンジ調シェーディング。グレースケール + 斜線: GeoGen 風の対角ハッチング — mask が 1.0 付近では密な白斜線、0.0 付近では疎な白斜線、中間は素直なランプ。3D ビューと 2D マップ両方に反映されます。", static_cast<int>(rock::PreviewSettings{}.maskShading)))
            {
                settings.preview.maskShading = static_cast<rock::MaskShadingMode>(std::clamp(maskShadingInt,
                    static_cast<int>(rock::MaskShadingMode::Grayscale),
                    static_cast<int>(rock::MaskShadingMode::GrayscaleHatched)));
                SaveAppSettings(state);
            }
            if (DrawPropertyBoolRow("近い地形でマスク表示", "DisplayMaskUseNearestHeightmap", &settings.preview.maskPreviewUseNearestHeightmap, "Mask preview nearest heightmap toggled", "Mask Noise / Mask Blend / Mask Levels など、ハイトマップ参照を直接持たないマスクノードをプレビューするとき、入力側をたどって見つかった一番近い Heightmap を表示用の地形に使います。見つからない場合は従来どおり平面表示します。", rock::PreviewSettings{}.maskPreviewUseNearestHeightmap, true))
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
