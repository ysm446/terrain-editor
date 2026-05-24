#include "NodeProperties.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <utility>

#include <imgui.h>

#include "PropertyWidgets.h"

namespace terrain::ui
{
namespace
{
NodePropertyCallbacks g_callbacks;
ScreenColorPick g_screenPick;

void EvaluateGraph()
{
    if (g_callbacks.evaluateGraph)
    {
        g_callbacks.evaluateGraph();
    }
}

void MarkGraphChanged(const char* reason)
{
    if (g_callbacks.markGraphChanged)
    {
        g_callbacks.markGraphChanged(reason);
    }
}

bool IsCtrlDown()
{
    return g_callbacks.isCtrlDown && g_callbacks.isCtrlDown();
}

rock::GraphId SelectedPathPointId(rock::GraphId nodeId)
{
    return g_callbacks.selectedPathPointId ? g_callbacks.selectedPathPointId(nodeId) : 0;
}

rock::PathPoint* FindMutablePathPoint(rock::PathSettings& path, rock::GraphId pointId)
{
    const auto it = std::ranges::find_if(path.points, [pointId](const rock::PathPoint& point) {
        return point.id == pointId;
    });
    return it != path.points.end() ? &*it : nullptr;
}

const char* PathHeightModeItems()
{
    return "Project To Terrain\0Terrain Offset\0Absolute\0";
}

int PathHeightModeToIndex(rock::PathPointHeightMode mode)
{
    switch (mode)
    {
    case rock::PathPointHeightMode::TerrainOffset:
        return 1;
    case rock::PathPointHeightMode::Absolute:
        return 2;
    case rock::PathPointHeightMode::ProjectToTerrain:
    default:
        return 0;
    }
}

rock::PathPointHeightMode PathHeightModeFromIndex(int index)
{
    switch (index)
    {
    case 1:
        return rock::PathPointHeightMode::TerrainOffset;
    case 2:
        return rock::PathPointHeightMode::Absolute;
    case 0:
    default:
        return rock::PathPointHeightMode::ProjectToTerrain;
    }
}

bool DrawPathPointPositionRow(rock::PathPoint& point)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel("Position (Vector3)", "X, Y, Z の順にポイント座標を編集します。Y を編集すると高さモードは Absolute になります。", false);
    ImGui::TableSetColumnIndex(1);

    float position[3] = {point.x, point.height, point.z};
    ImGui::PushID("PathSelectedPointPosition");
    ImGui::SetNextItemWidth(std::min(260.0f, ImGui::GetContentRegionAvail().x));
    const bool changed = ImGui::InputFloat3("##value", position, "%.3f");
    ImGui::PopID();
    if (!changed)
    {
        return false;
    }

    point.x = std::clamp(position[0], -1000000.0f, 1000000.0f);
    point.height = std::clamp(position[1], -1000000.0f, 1000000.0f);
    point.z = std::clamp(position[2], -1000000.0f, 1000000.0f);
    point.heightMode = rock::PathPointHeightMode::Absolute;
    return true;
}

bool DrawPathHeightOffsetRow(rock::PathPoint& point)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel("Height Offset (m)", "Height Mode が Terrain Offset のとき、地形高さから上下にずらす量です。", FloatDiffersFromDefault(point.heightOffset, 0.0f));
    ImGui::TableSetColumnIndex(1);

    bool changed = false;
    ImGui::PushID("PathSelectedPointHeightOffset");
    ImGui::SetNextItemWidth(std::min(120.0f, ImGui::GetContentRegionAvail().x));
    if (DrawEnterCommitFloatInput("##number", &point.heightOffset, "%.3f"))
    {
        point.heightOffset = std::clamp(point.heightOffset, -1000000.0f, 1000000.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (DrawResetToDefaultButton("reset", !FloatDiffersFromDefault(point.heightOffset, 0.0f), "0.000"))
    {
        point.heightOffset = 0.0f;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

bool DrawPathPointCompactFloatRow(
    const char* label,
    const char* id,
    float* value,
    float minValue,
    float maxValue,
    float defaultValue,
    const char* tooltip,
    const char* format = "%.3f")
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, FloatDiffersFromDefault(*value, defaultValue));
    ImGui::TableSetColumnIndex(1);

    bool changed = false;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(std::min(120.0f, ImGui::GetContentRegionAvail().x));
    if (DrawEnterCommitFloatInput("##number", value, format))
    {
        *value = std::clamp(*value, minValue, maxValue);
        changed = true;
    }
    ImGui::SameLine();
    const bool isDefaultValue = !FloatDiffersFromDefault(*value, defaultValue);
    const std::string defaultValueText = FormatDefaultFloat(defaultValue, format);
    if (DrawResetToDefaultButton("reset", isDefaultValue, defaultValueText.c_str()))
    {
        *value = std::clamp(defaultValue, minValue, maxValue);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}
} // namespace

void SetNodePropertyCallbacks(NodePropertyCallbacks callbacks)
{
    g_callbacks = std::move(callbacks);
}

ScreenColorPick& ColorizeScreenPick()
{
    return g_screenPick;
}

void DrawNodePropertiesPanel(rock::NodeGraph& graph, rock::GraphId selectedNodeId)
{
    const rock::Node* selectedNode = graph.FindNode(selectedNodeId);
    if (selectedNode == nullptr)
    {
        ImGui::TextDisabled("ノードを選択してください");
        ImGui::Spacing();
        ImGui::TextWrapped("選択したノードの設定だけをここに表示します。");
        return;
    }

    ImGui::TextUnformatted(selectedNode->title.c_str());
    ImGui::TextDisabled("%s", rock::ToString(selectedNode->kind).data());
    ImGui::Separator();

    rock::Node* editableNode = graph.FindMutableNode(selectedNode->id);
    if (editableNode == nullptr)
    {
        return;
    }

    switch (selectedNode->kind)
    {
    case rock::NodeKind::HeightmapLoad:
        DrawHeightmapLoadProperties(*editableNode);
        return;
    case rock::NodeKind::Shape:
        DrawShapeProperties(*editableNode);
        return;
    case rock::NodeKind::HeightmapBlur:
        DrawHeightmapBlurProperties(*editableNode);
        return;
    case rock::NodeKind::MultiScaleErosion:
        DrawMultiScaleErosionProperties(*editableNode);
        return;
    case rock::NodeKind::MaskNoise:
        DrawMaskNoiseProperties(*editableNode);
        return;
    case rock::NodeKind::MaskBlend:
        DrawMaskBlendProperties(*editableNode);
        return;
    case rock::NodeKind::MaskLevels:
        DrawMaskLevelsProperties(*editableNode);
        return;
    case rock::NodeKind::MaskHeight:
        DrawMaskHeightProperties(*editableNode);
        return;
    case rock::NodeKind::MaskSlope:
        DrawMaskSlopeProperties(*editableNode);
        return;
    case rock::NodeKind::MaskCurvature:
        DrawMaskCurvatureProperties(*editableNode);
        return;
    case rock::NodeKind::MaskFluvial:
        DrawMaskFluvialProperties(*editableNode);
        return;
    case rock::NodeKind::Crumbling:
        DrawCrumblingProperties(*editableNode);
        return;
    case rock::NodeKind::Rock:
        DrawRockProperties(*editableNode);
        return;
    case rock::NodeKind::Scatter:
        DrawScatterProperties(*editableNode);
        return;
    case rock::NodeKind::Sediment:
        DrawSedimentProperties(*editableNode);
        return;
    case rock::NodeKind::Snow:
        DrawSnowProperties(*editableNode);
        return;
    case rock::NodeKind::Colorize:
        DrawColorizeProperties(*editableNode);
        return;
    case rock::NodeKind::Path:
        DrawPathProperties(*editableNode);
        return;
    case rock::NodeKind::MaskPath:
        DrawMaskPathProperties(*editableNode);
        return;
    default:
        return;
    }
}

bool DrawHeightmapLoadProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("HeightmapPropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 184.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    editableNode.heightmap.scaleMeters = std::clamp(editableNode.heightmap.scaleMeters, 1.0f, 8096.0f);
    editableNode.heightmap.relativeVerticalScalePercent = std::clamp(editableNode.heightmap.relativeVerticalScalePercent, 0.0f, 100.0f);
    editableNode.heightmap.verticalOffsetMeters = std::clamp(editableNode.heightmap.verticalOffsetMeters, -4096.0f, 4096.0f);

    if (DrawPropertyPathRow("File", "HeightmapFile", &editableNode.heightmap.path, "Heightmap file changed", "読み込むハイトマップ画像です。明るいピクセルほど高い地形として扱います。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Scale (m)", "HeightmapScaleMeters", &editableNode.heightmap.scaleMeters, 1.0f, 8096.0f, rock::HeightmapLoadSettings{}.scaleMeters, "Heightmap scale changed", true, "読み込むハイトマップ画像がグローバル Terrain Size 内で占める幅と奥行きです。Terrain Size より大きい場合は中央でクロップし、小さい場合は外側を高さ 0 にします。", "%.2f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Relative Vertical (%)", "HeightmapRelativeVerticalScale", &editableNode.heightmap.relativeVerticalScalePercent, 0.0f, 100.0f, rock::HeightmapLoadSettings{}.relativeVerticalScalePercent, "Heightmap vertical scale changed", true, "高さ方向の相対倍率です。実際の高さ範囲は Scale (m) x この値 / 100 になります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Offset (m)", "HeightmapVerticalOffset", &editableNode.heightmap.verticalOffsetMeters, -4096.0f, 4096.0f, rock::HeightmapLoadSettings{}.verticalOffsetMeters, "Heightmap vertical offset changed", true, "地形全体を上下に移動する高さオフセットです。"))
    {
        EvaluateGraph();
    }
    ImGui::EndTable();
    return true;
}

bool DrawShapeProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("ShapePropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 184.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::ShapeSettings& shape = editableNode.shape;
    shape.scaleMeters = std::clamp(shape.scaleMeters, 1.0f, 8096.0f);
    shape.relativeHeightPercent = std::clamp(shape.relativeHeightPercent, 0.0f, 100.0f);

    int shapeKind = static_cast<int>(shape.kind);
    if (DrawPropertyComboRow("Shape Type", "ShapeType", &shapeKind, "Hemisphere\0Pyramid\0", "デバッグ用の基本ハイトフィールド形状です。", static_cast<int>(rock::ShapeSettings{}.kind)))
    {
        shape.kind = static_cast<rock::ShapeKind>(std::clamp(shapeKind, 0, 1));
        MarkGraphChanged("Shape type changed");
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Scale (m)", "ShapeScaleMeters", &shape.scaleMeters, 1.0f, 8096.0f, rock::ShapeSettings{}.scaleMeters, "Shape scale changed", true, "グローバル Terrain Size 内でシェープが占める幅と奥行きです。Terrain Size より小さい場合は中央に配置され、外側は高さ 0 になります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Relative Height (%)", "ShapeRelativeHeight", &shape.relativeHeightPercent, 0.0f, 100.0f, rock::ShapeSettings{}.relativeHeightPercent, "Shape height changed", true, "最大高さです。実際の高さは Scale (m) x この値 / 100 になります。"))
    {
        EvaluateGraph();
    }
    ImGui::EndTable();
    return true;
}

bool DrawHeightmapBlurProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("HeightmapBlurRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 184.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::HeightmapBlurSettings& blur = editableNode.heightmapBlur;
    blur.radius = std::clamp(blur.radius, 0.0f, 128.0f);
    blur.strength = std::clamp(blur.strength, 0.0f, 1.0f);
    blur.iterations = std::clamp(blur.iterations, 0, 64);

    if (DrawPropertyFloatRow("Radius (cells)", "HeightmapBlurRadius", &blur.radius, 0.0f, 128.0f, rock::HeightmapBlurSettings{}.radius, "Heightmap blur radius changed", true, "ぼかしに使うセル半径です。大きいほど広い範囲の起伏をならします。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Strength (%)", "HeightmapBlurStrength", &blur.strength, 0.0f, 1.0f, rock::HeightmapBlurSettings{}.strength, "Heightmap blur strength changed", "元の高さとぼかし後の高さを混ぜる量です。低いほど元の形を残します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Iterations", "HeightmapBlurIterations", &blur.iterations, 0, 64, rock::HeightmapBlurSettings{}.iterations, "Heightmap blur iterations changed", true, "ぼかし処理を繰り返す回数です。増やすほど滑らかになりますが計算時間も増えます。"))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}

bool DrawMaskNoiseProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskNoiseRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskNoiseSettings& mn = editableNode.maskNoise;
    mn.seed = std::clamp(mn.seed, 0, 999999);
    mn.octaves = std::clamp(mn.octaves, 1, 12);
    mn.frequency = std::clamp(mn.frequency, 0.0f, 256.0f);
    mn.lacunarity = std::clamp(mn.lacunarity, 0.0f, 8.0f);
    mn.persistence = std::clamp(mn.persistence, 0.0f, 1.0f);

    {
        int backendInt = static_cast<int>(mn.backend);
        if (DrawPropertyComboRow("Backend", "MaskNoiseBackend", &backendInt, "CPU\0GPU\0\0", "CPU 並列実装と GPU (D3D12 compute) を切り替えます。GPU は解像度が高いほど速くなります (1024² 以上で顕著)。\nGPU が初期化に失敗したり実行時エラーになると自動的に CPU 版にフォールバックします。", static_cast<int>(rock::MaskNoiseSettings{}.backend)))
        {
            mn.backend = static_cast<rock::MaskNoiseBackend>(std::clamp(backendInt,
                static_cast<int>(rock::MaskNoiseBackend::CpuParallel),
                static_cast<int>(rock::MaskNoiseBackend::GpuCompute)));
            EvaluateGraph();
        }
    }
    if (DrawPropertyIntRow("Seed", "MaskNoiseSeed", &mn.seed, 0, 999999, rock::MaskNoiseSettings{}.seed, "Mask noise seed changed", true, "ハッシュのオフセットです。同じパラメータでも異なるパターンを得るために使います。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Octaves", "MaskNoiseOctaves", &mn.octaves, 1, 12, rock::MaskNoiseSettings{}.octaves, "Mask noise octaves changed", true, "重ねる Perlin ノイズのオクターブ数です。多いほど細かい階層が増えますが計算時間も増えます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Frequency", "MaskNoiseFrequency", &mn.frequency, 0.0f, 256.0f, rock::MaskNoiseSettings{}.frequency, "Mask noise frequency changed", true, "地形範囲に対する基本周波数です。大きいほど細かい模様になります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Lacunarity", "MaskNoiseLacunarity", &mn.lacunarity, 0.0f, 8.0f, rock::MaskNoiseSettings{}.lacunarity, "Mask noise lacunarity changed", true, "オクターブごとに周波数を何倍にするかです。標準は 2.0。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Persistence", "MaskNoisePersistence", &mn.persistence, 0.0f, 1.0f, rock::MaskNoiseSettings{}.persistence, "Mask noise persistence changed", true, "オクターブごとに振幅を何倍にするかです。標準は 0.5。大きいほど高オクターブが目立ちます。"))
    {
        EvaluateGraph();
    }
    ImGui::EndTable();
    return true;
}

bool DrawMaskBlendProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskBlendRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskBlendSettings& mb = editableNode.maskBlend;
    mb.intensity = std::clamp(mb.intensity, 0.0f, 1.0f);

    int modeInt = static_cast<int>(mb.mode);
    if (DrawPropertyComboRow("Blend Mode", "MaskBlendMode", &modeInt, "Add\0Multiply\0Min\0Max\0\0", "A と B を合成する方式です。Add は加算、Multiply は乗算、Min / Max はチャンネルごとの最小値・最大値です。", static_cast<int>(rock::MaskBlendSettings{}.mode)))
    {
        mb.mode = static_cast<rock::MaskBlendMode>(std::clamp(modeInt,
            static_cast<int>(rock::MaskBlendMode::Add),
            static_cast<int>(rock::MaskBlendMode::Max)));
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Blend Intensity (%)", "MaskBlendIntensity", &mb.intensity, 0.0f, 1.0f, rock::MaskBlendSettings{}.intensity, "Mask blend intensity changed", "A をベースに、A と Blend(A, B) の間を補間する強さです。0 で A のみ、1 で完全に合成結果を使います。"))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}

bool DrawMaskLevelsProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskLevelsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskLevelsSettings& ml = editableNode.maskLevels;
    ml.blackPoint = std::clamp(ml.blackPoint, 0.0f, 1.0f);
    ml.whitePoint = std::clamp(ml.whitePoint, 0.0f, 1.0f);
    ml.gamma = std::clamp(ml.gamma, 0.05f, 8.0f);

    if (DrawPropertyPercentRow("Black Point (%)", "MaskLevelsBlackPoint", &ml.blackPoint, 0.0f, 1.0f, rock::MaskLevelsSettings{}.blackPoint, "Mask levels black point changed", "この値以下を黒にします。上げるほど弱いマスクを切り落とします。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("White Point (%)", "MaskLevelsWhitePoint", &ml.whitePoint, 0.0f, 1.0f, rock::MaskLevelsSettings{}.whitePoint, "Mask levels white point changed", "この値以上を白にします。下げるほどマスクが早く飽和します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Gamma", "MaskLevelsGamma", &ml.gamma, 0.05f, 8.0f, rock::MaskLevelsSettings{}.gamma, "Mask levels gamma changed", true, "中間調のカーブです。1 未満で暗部を持ち上げ、1 より大きいと強い部分だけを残します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Invert", "MaskLevelsInvert", &ml.invert, "Mask levels invert toggled", "出力マスクを反転します。", rock::MaskLevelsSettings{}.invert))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}

bool DrawMaskHeightProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskHeightRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskHeightSettings& mh = editableNode.maskHeight;
    mh.heightMinMeters = std::clamp(mh.heightMinMeters, -100000.0f, 100000.0f);
    mh.heightMaxMeters = std::clamp(mh.heightMaxMeters, -100000.0f, 100000.0f);
    if (mh.heightMaxMeters < mh.heightMinMeters)
    {
        std::swap(mh.heightMinMeters, mh.heightMaxMeters);
    }
    mh.featherMeters = std::clamp(mh.featherMeters, 0.0f, 100000.0f);
    mh.gamma = std::clamp(mh.gamma, 0.05f, 8.0f);

    if (DrawPropertyBoolRow("Use Full Range", "MaskHeightUseFullRange", &mh.useFullRange, "Mask height full range toggled", "入力 Heightmap の最低標高を 0、最高標高を 1 として、標高全体をグラデーションの mask にします。", rock::MaskHeightSettings{}.useFullRange))
    {
        EvaluateGraph();
    }
    if (!mh.useFullRange)
    {
        if (DrawPropertyFloatInputRow("Height Min (m)", "MaskHeightMinMeters", &mh.heightMinMeters, -100000.0f, 100000.0f, rock::MaskHeightSettings{}.heightMinMeters, "Mask height min changed", true, "この標高より低い部分を黒にします。地形の実スケールに合わせたメートル単位です。", "%.2f"))
        {
            if (mh.heightMaxMeters < mh.heightMinMeters) mh.heightMaxMeters = mh.heightMinMeters;
            EvaluateGraph();
        }
        if (DrawPropertyFloatInputRow("Height Max (m)", "MaskHeightMaxMeters", &mh.heightMaxMeters, -100000.0f, 100000.0f, rock::MaskHeightSettings{}.heightMaxMeters, "Mask height max changed", true, "この標高より高い部分を黒にします。Min との差が抽出する標高帯になります。", "%.2f"))
        {
            if (mh.heightMaxMeters < mh.heightMinMeters) mh.heightMinMeters = mh.heightMaxMeters;
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Feather (m)", "MaskHeightFeatherMeters", &mh.featherMeters, 0.0f, 1000.0f, rock::MaskHeightSettings{}.featherMeters, "Mask height feather changed", true, "標高帯の境界をメートル単位でぼかします。スライダーは 0..1000m、数値入力ではより大きい値も指定できます。", "%.2f", 0, 0.0f, 100000.0f))
        {
            EvaluateGraph();
        }
    }
    if (DrawPropertyFloatRow("Gamma", "MaskHeightGamma", &mh.gamma, 0.05f, 8.0f, rock::MaskHeightSettings{}.gamma, "Mask height gamma changed", true, "出力 mask のカーブです。1 未満で境界の弱い値を明るく、1 より大きいと中心の強い値を強調します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Invert", "MaskHeightInvert", &mh.invert, "Mask height invert toggled", "出力マスクを反転します。指定標高帯の外側を使うときに便利です。", rock::MaskHeightSettings{}.invert))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}


bool DrawMaskSlopeProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskSlopeRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskSlopeSettings& ms = editableNode.maskSlope;
    ms.largestDetailLevelM = std::clamp(ms.largestDetailLevelM, 0.0f, 1024.0f);
    ms.slopeMinDeg = std::clamp(ms.slopeMinDeg, 0.0f, 89.9f);
    ms.slopeMaxDeg = std::clamp(ms.slopeMaxDeg, 0.0f, 89.9f);
    if (ms.slopeMaxDeg < ms.slopeMinDeg)
    {
        std::swap(ms.slopeMinDeg, ms.slopeMaxDeg);
    }
    ms.gamma = std::clamp(ms.gamma, 0.05f, 8.0f);

    {
        constexpr std::array<float, 10> kSlopeDetailLevels = {0.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f};
        int detailIndex = 0;
        float bestDistance = FLT_MAX;
        for (int i = 0; i < static_cast<int>(kSlopeDetailLevels.size()); ++i)
        {
            const float distance = std::abs(ms.largestDetailLevelM - kSlopeDetailLevels[static_cast<size_t>(i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                detailIndex = i;
            }
        }
        if (DrawPropertyComboRow("Largest Detail Level (m)", "MaskSlopeLargestDetailLevel", &detailIndex, "Max\0" "2 m\0" "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "128 m\0" "256 m\0" "512 m\0" "\0", "傾斜を調べる前の解析用ハイトをならす最大スケールです。Max は入力地形そのまま、数値を上げるほど小さな凹凸を無視して大きな斜面を優先します。入力地形そのものは変更しません。", 0))
        {
            detailIndex = std::clamp(detailIndex, 0, static_cast<int>(kSlopeDetailLevels.size()) - 1);
            ms.largestDetailLevelM = kSlopeDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }

    if (DrawPropertyFloatRow("Slope Min (deg)", "MaskSlopeMinDeg", &ms.slopeMinDeg, 0.0f, 89.9f, rock::MaskSlopeSettings{}.slopeMinDeg, "Mask slope min changed", true, "この角度以下を黒にします。上げるほど急な斜面だけを残します。", "%.1f"))
    {
        if (ms.slopeMaxDeg < ms.slopeMinDeg) ms.slopeMaxDeg = ms.slopeMinDeg;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Slope Max (deg)", "MaskSlopeMaxDeg", &ms.slopeMaxDeg, 0.0f, 89.9f, rock::MaskSlopeSettings{}.slopeMaxDeg, "Mask slope max changed", true, "この角度以上を白にします。Min との差を広げるほど境界が滑らかになります。", "%.1f"))
    {
        if (ms.slopeMaxDeg < ms.slopeMinDeg) ms.slopeMinDeg = ms.slopeMaxDeg;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Gamma", "MaskSlopeGamma", &ms.gamma, 0.05f, 8.0f, rock::MaskSlopeSettings{}.gamma, "Mask slope gamma changed", true, "出力 mask のカーブです。1 未満で弱い斜面を明るく、1 より大きいと急斜面だけを強調します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Invert", "MaskSlopeInvert", &ms.invert, "Mask slope invert toggled", "出力マスクを反転します。平地マスクを作るときに使います。", rock::MaskSlopeSettings{}.invert))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}

bool DrawMaskCurvatureProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskCurvatureRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskCurvatureSettings& mc = editableNode.maskCurvature;
    mc.largestDetailLevelM = std::clamp(mc.largestDetailLevelM, 1.0f, 1024.0f);
    mc.radius = std::clamp(mc.radius, 1, 64);
    mc.sensitivityMeters = std::clamp(mc.sensitivityMeters, 0.001f, 1000.0f);
    mc.threshold = std::clamp(mc.threshold, 0.0f, 0.99f);
    mc.gamma = std::clamp(mc.gamma, 0.05f, 8.0f);

    int modeInt = static_cast<int>(mc.mode);
    if (DrawPropertyComboRow("Mode", "MaskCurvatureMode", &modeInt, "Ridges\0Valleys\0Absolute\0\0", "Ridges は周囲より高い凸部、Valleys は周囲より低い凹部、Absolute は両方を検出します。", static_cast<int>(rock::MaskCurvatureSettings{}.mode)))
    {
        mc.mode = static_cast<rock::MaskCurvatureMode>(std::clamp(modeInt,
            static_cast<int>(rock::MaskCurvatureMode::Ridges),
            static_cast<int>(rock::MaskCurvatureMode::Absolute)));
        EvaluateGraph();
    }
    {
        constexpr std::array<float, 9> kCurvatureDetailLevels = {2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f};
        int detailIndex = 2;
        float bestDistance = FLT_MAX;
        for (int i = 0; i < static_cast<int>(kCurvatureDetailLevels.size()); ++i)
        {
            const float distance = std::abs(mc.largestDetailLevelM - kCurvatureDetailLevels[static_cast<size_t>(i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                detailIndex = i;
            }
        }
        if (DrawPropertyComboRow("Largest Detail Level (m)", "MaskCurvatureLargestDetailLevel", &detailIndex, "2 m\0" "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "128 m\0" "256 m\0" "512 m\0" "\0", "曲率を調べる前の解析用ハイトをならす最大スケールです。2m は細かい凹凸を拾いやすく、512m は小さな揺れを無視して大きな尾根や谷を優先します。入力地形そのものは変更しません。", 2))
        {
            detailIndex = std::clamp(detailIndex, 0, static_cast<int>(kCurvatureDetailLevels.size()) - 1);
            mc.largestDetailLevelM = kCurvatureDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }
    if (DrawPropertyFloatRow("Sensitivity (m)", "MaskCurvatureSensitivity", &mc.sensitivityMeters, 0.001f, 1000.0f, rock::MaskCurvatureSettings{}.sensitivityMeters, "Mask curvature sensitivity changed", true, "この高さ差で mask=1 になります。小さいほど弱い曲率も明るくなります。", "%.3f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Threshold (%)", "MaskCurvatureThreshold", &mc.threshold, 0.0f, 0.99f, rock::MaskCurvatureSettings{}.threshold, "Mask curvature threshold changed", "正規化後の下限です。上げるほど弱い曲率を落として、強い尾根や谷だけを残します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Gamma", "MaskCurvatureGamma", &mc.gamma, 0.05f, 8.0f, rock::MaskCurvatureSettings{}.gamma, "Mask curvature gamma changed", true, "出力 mask のカーブです。1 未満で微細な曲率を明るく、1 より大きいと強い曲率だけを強調します。"))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}


bool DrawMaskFluvialProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskFluvialRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskFluvialSettings& mf = editableNode.maskFluvial;
    mf.accumulationThreshold = std::clamp(mf.accumulationThreshold, 0.0f, 1.0f);
    mf.gamma = std::clamp(mf.gamma, 0.05f, 8.0f);
    mf.softness = std::clamp(mf.softness, 0.001f, 4.0f);
    mf.power = std::clamp(mf.power, 0.1f, 8.0f);
    mf.pitFillIterations = rock::MaskFluvialSettings{}.pitFillIterations;
    mf.inertia = rock::MaskFluvialSettings{}.inertia;
    mf.largestDetailLevelM = std::clamp(mf.largestDetailLevelM, 1.0f, 1024.0f);
    mf.mfdExponent = std::clamp(mf.mfdExponent, 0.1f, 16.0f);
    mf.particleCount = std::clamp(mf.particleCount, 1, 200000);
    mf.particleLifetime = std::clamp(mf.particleLifetime, 1, 2048);
    mf.particleInertia = std::clamp(mf.particleInertia, 0.0f, 0.98f);
    mf.particleStepLengthM = std::clamp(mf.particleStepLengthM, 0.01f, 1024.0f);
    mf.particleSeed = std::clamp(mf.particleSeed, 0, 999999);

    {
        int backendInt = static_cast<int>(mf.backend);
        if (DrawPropertyComboRow("Backend", "MaskFluvialBackend", &backendInt, "CPU\0GPU\0\0", "実行バックエンド。CPU は sort + 降順トポロジカル走査の厳密実装。GPU は Jacobi 反復ゲザー (~2*resolution iter) の近似実装で、視覚的には同等だが数値は完全一致せず (累積順序が異なるため)。GPU は 1024² で 5-10 倍程度高速。シェーダーコンパイル/ディスパッチ失敗時は CPU に自動フォールバック。", static_cast<int>(rock::MaskFluvialSettings{}.backend)))
        {
            mf.backend = static_cast<rock::MaskFluvialBackend>(std::clamp(backendInt,
                static_cast<int>(rock::MaskFluvialBackend::CpuReference),
                static_cast<int>(rock::MaskFluvialBackend::GpuCompute)));
            EvaluateGraph();
        }
    }

    {
        int modeInt = static_cast<int>(mf.simulationMode);
        if (DrawPropertyComboRow("Simulation Mode", "MaskFluvialSimulationMode", &modeInt, "Flow Accumulation\0Particles\0\0", "Flow Accumulation は従来の MFD 流量累積です。Particles は粒子を地形勾配に沿って流し、通過密度を Mask にします。粒子モードは現状 CPU 評価です。", static_cast<int>(rock::MaskFluvialSettings{}.simulationMode)))
        {
            mf.simulationMode = static_cast<rock::MaskFluvialSimulationMode>(std::clamp(modeInt,
                static_cast<int>(rock::MaskFluvialSimulationMode::FlowAccumulation),
                static_cast<int>(rock::MaskFluvialSimulationMode::Particles)));
            EvaluateGraph();
        }
    }

    mf.algorithm = rock::FlowAccumulationAlgorithm::MFD;
    int curveInt = static_cast<int>(mf.outputCurve);
    if (DrawPropertyComboRow("Output Curve", "MaskFluvialCurve", &curveInt, "Log\0Threshold\0Linear\0\0", "累積値をマスクへ写すカーブです。Log は連続的な樹枝状ドレナージマップ(既定、参考画像の見た目)、Threshold は閾値ベースの二値川筋抽出、Linear は非対数の連続マップ(主流偏重)。", static_cast<int>(rock::MaskFluvialSettings{}.outputCurve)))
    {
        mf.outputCurve = static_cast<rock::MaskFluvialOutputCurve>(std::clamp(curveInt,
            static_cast<int>(rock::MaskFluvialOutputCurve::Log),
            static_cast<int>(rock::MaskFluvialOutputCurve::Linear)));
        EvaluateGraph();
    }
    const char* thresholdTooltip = (mf.outputCurve == rock::MaskFluvialOutputCurve::Threshold)
        ? "全セル数に対する割合で、これ以上の上流寄与があるセルが川として現れます。下げるほど支流が増え、上げるほど太い本流のみ残ります。Threshold モード時の主要パラメータです。"
        : "ノイズフロアです。これ未満の上流寄与しか持たないセルはマスク 0 にクリップされます。0 で全セルを描画(参考画像の見た目)。";
    if (DrawPropertyPercentRow("Threshold (%)", "MaskFluvialThreshold", &mf.accumulationThreshold, 0.0f, 1.0f, rock::MaskFluvialSettings{}.accumulationThreshold, "Mask fluvial threshold changed", thresholdTooltip))
    {
        EvaluateGraph();
    }
    if (mf.outputCurve != rock::MaskFluvialOutputCurve::Threshold)
    {
        if (DrawPropertyFloatRow("Gamma", "MaskFluvialGamma", &mf.gamma, 0.05f, 8.0f, rock::MaskFluvialSettings{}.gamma, "Mask fluvial gamma changed", true, "Log/Linear カーブの最後にかける pow 指数です。小さいほど細い支流が明るくなり、大きいほど主流のみが残ってコントラストが上がります。"))
        {
            EvaluateGraph();
        }
    }
    if (mf.outputCurve == rock::MaskFluvialOutputCurve::Threshold)
    {
        if (DrawPropertyFloatRow("Softness", "MaskFluvialSoftness", &mf.softness, 0.001f, 4.0f, rock::MaskFluvialSettings{}.softness, "Mask fluvial softness changed", true, "閾値前後の smoothstep 幅です。小さいほどシャープな川筋、大きいほど湿地帯のような広がり。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Edge Power", "MaskFluvialPower", &mf.power, 0.1f, 8.0f, rock::MaskFluvialSettings{}.power, "Mask fluvial power changed", true, "pow(mask, power) で川縁をテーパーします。1 を超えると細く、1 未満で太く見えます。"))
        {
            EvaluateGraph();
        }
    }
    {
        constexpr std::array<float, 8> kFluvialDetailLevels = {4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f};
        int detailIndex = 1;
        float bestDistance = FLT_MAX;
        for (int i = 0; i < static_cast<int>(kFluvialDetailLevels.size()); ++i)
        {
            const float distance = std::abs(mf.largestDetailLevelM - kFluvialDetailLevels[static_cast<size_t>(i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                detailIndex = i;
            }
        }
        if (DrawPropertyComboRow("Largest Detail Level (m)", "MaskFluvialLargestDetailLevel", &detailIndex, "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "128 m\0" "256 m\0" "512 m\0" "\0", "流向を計算する前の解析用ハイトをならす最大スケールです。4m は細かい支流や小さな窪みを拾いやすく、512m は小さな凹凸を無視して大きな谷筋を優先します。入力地形そのものは変更しません。", 1))
        {
            detailIndex = std::clamp(detailIndex, 0, static_cast<int>(kFluvialDetailLevels.size()) - 1);
            mf.largestDetailLevelM = kFluvialDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }
    if (DrawPropertyFloatRow("Flow Concentration", "MaskFluvialMfdExponent", &mf.mfdExponent, 0.1f, 16.0f, rock::MaskFluvialSettings{}.mfdExponent, "Mask fluvial flow concentration changed", true, "MFD の下流分配の集中度です。大きいほど主流に集まり、小さいほど流域・湿地帯のように面で広がります。"))
    {
        EvaluateGraph();
    }

    if (mf.simulationMode == rock::MaskFluvialSimulationMode::Particles)
    {
        if (DrawPropertyIntRow("Particle Count", "MaskFluvialParticleCount", &mf.particleCount, 1, 200000, rock::MaskFluvialSettings{}.particleCount, "Mask fluvial particle count changed", true, "流す粒子数です。多いほど密度が安定しますが計算時間も増えます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyIntRow("Lifetime", "MaskFluvialParticleLifetime", &mf.particleLifetime, 1, 2048, rock::MaskFluvialSettings{}.particleLifetime, "Mask fluvial particle lifetime changed", true, "粒子が最大何ステップ流れるかです。大きいほど長い流路になります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyPercentRow("Inertia (%)", "MaskFluvialParticleInertia", &mf.particleInertia, 0.0f, 0.98f, rock::MaskFluvialSettings{}.particleInertia, "Mask fluvial particle inertia changed", "進行方向を保持する強さです。高いほど直進し、低いほど局所勾配や細かい揺らぎへ反応します。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Step Length (m)", "MaskFluvialParticleStepLength", &mf.particleStepLengthM, 0.01f, 1024.0f, rock::MaskFluvialSettings{}.particleStepLengthM, "Mask fluvial particle step changed", true, "粒子が 1 ステップで進む距離です。大きいほど粗く長い線、小さいほど細かく地形を追う線になります。", "%.2f"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyIntRow("Seed", "MaskFluvialParticleSeed", &mf.particleSeed, 0, 999999, rock::MaskFluvialSettings{}.particleSeed, "Mask fluvial particle seed changed", true, "粒子の初期配置と揺らぎのシードです。"))
        {
            EvaluateGraph();
        }
    }

    ImGui::EndTable();
    return true;
}

bool DrawMaskPathProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MaskPathRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MaskPathSettings& pm = editableNode.maskPath;
    pm.gamma = std::clamp(pm.gamma, 0.05f, 8.0f);

    if (DrawPropertyFloatRow("Gamma", "MaskPathGamma", &pm.gamma, 0.05f, 8.0f, rock::MaskPathSettings{}.gamma, "Mask path gamma changed", true, "Output mask curve. Lower values brighten the feather; higher values emphasize the center."))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Invert", "MaskPathInvert", &pm.invert, "Mask path invert toggled", "Invert the output mask.", rock::MaskPathSettings{}.invert))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    ImGui::TextWrapped("Mask Path uses each Path point's Width and Feather. Width is the full mask width; Feather fades outward from that width.");
    return true;
}


bool DrawRockProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("RockRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::RockSettings& rk = editableNode.rock;
    rk.style = static_cast<rock::RockStyle>(std::clamp(static_cast<int>(rk.style),
        static_cast<int>(rock::RockStyle::Classic),
        static_cast<int>(rock::RockStyle::Shard)));
    rk.orientationRule = static_cast<rock::RockOrientationRule>(std::clamp(static_cast<int>(rk.orientationRule),
        static_cast<int>(rock::RockOrientationRule::Flat),
        static_cast<int>(rock::RockOrientationRule::SlopeOriented)));
    rk.layerCount = std::clamp(rk.layerCount, 1, 8);
    rk.density = std::clamp(rk.density, 0.5f, 1000.0f);
    rk.coverage = std::clamp(rk.coverage, 0.0f, 1.0f);
    rk.rockSizeMinM = std::clamp(rk.rockSizeMinM, 0.1f, 200.0f);
    rk.rockSizeMaxM = std::clamp(std::max(rk.rockSizeMaxM, rk.rockSizeMinM), 0.1f, 200.0f);
    rk.rockHeight = std::clamp(rk.rockHeight, 0.0f, 100.0f);
    rk.heightJitter = std::clamp(rk.heightJitter, 0.0f, 1.0f);
    rk.rotationVariation = std::clamp(rk.rotationVariation, 0.0f, 1.0f);
    rk.aspectVariation = std::clamp(rk.aspectVariation, 0.0f, 1.0f);
    rk.edgeSharpness = std::clamp(rk.edgeSharpness, 0.0f, 1.0f);
    rk.bumpiness = std::clamp(rk.bumpiness, 0.0f, 1.0f);
    rk.facetSharpness = std::clamp(rk.facetSharpness, 0.0f, 1.0f);
    rk.facetScale = std::clamp(rk.facetScale, 0.5f, 8.0f);
    rk.seed = std::clamp(rk.seed, 0, 999999);

    {
        int backendInt = static_cast<int>(rk.backend);
        if (DrawPropertyComboRow("Backend", "RockBackend", &backendInt, "CPU\0GPU\0\0", "実行バックエンド。GPU (D3D12 compute) は CPU 比で 10-30 倍高速。シェーダーコンパイル/ディスパッチ失敗時は CPU に自動フォールバック。CPU は決定論的でデバッグ向き。", static_cast<int>(rock::RockSettings{}.backend)))
        {
            rk.backend = static_cast<rock::RockBackend>(std::clamp(backendInt,
                static_cast<int>(rock::RockBackend::CpuReference),
                static_cast<int>(rock::RockBackend::GpuCompute)));
            EvaluateGraph();
        }
    }
    {
        int styleInt = static_cast<int>(rk.style);
        if (DrawPropertyComboRow("Rock Style", "RockStyle", &styleInt, "Classic\0Polygonal\0Shard\0\0", "岩の基本シェープです。Classic は従来の丸みを残した多角形ドーム、Polygonal はオフセンター頂点を持つ低ポリゴン岩、Shard はより細長い破片状の岩を生成します。", static_cast<int>(rock::RockSettings{}.style)))
        {
            rk.style = static_cast<rock::RockStyle>(std::clamp(styleInt,
                static_cast<int>(rock::RockStyle::Classic),
                static_cast<int>(rock::RockStyle::Shard)));
            EvaluateGraph();
        }
    }
    {
        int orientationInt = static_cast<int>(rk.orientationRule);
        if (DrawPropertyComboRow("Orientation Rule", "RockOrientationRule", &orientationInt, "Flat\0Follow Ground\0Slope Oriented\0\0", "岩の向きと斜面への沿わせ方です。Flat は従来通り水平基準で加算、Follow Ground は斜面距離と法線の上向き成分を使って地形に沿わせ、Slope Oriented は岩の回転を斜面方向へ寄せます。", static_cast<int>(rock::RockSettings{}.orientationRule)))
        {
            rk.orientationRule = static_cast<rock::RockOrientationRule>(std::clamp(orientationInt,
                static_cast<int>(rock::RockOrientationRule::Flat),
                static_cast<int>(rock::RockOrientationRule::SlopeOriented)));
            EvaluateGraph();
        }
    }
    if (DrawPropertyIntRow("Layer Count", "RockLayerCount", &rk.layerCount, 1, 8, rock::RockSettings{}.layerCount, "Rock layer count changed", true, "重ねる岩散布レイヤー数です。増やすほど別シードのグリッドを重ねて密度と不規則さが増えますが、評価コストもほぼ比例して上がります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Seed", "RockSeed", &rk.seed, 0, 999999, rock::RockSettings{}.seed, "Rock seed changed", true, "ハッシュのオフセットです。同じパラメータでも異なる岩配置を得るために使います。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Density (m)", "RockDensity", &rk.density, 0.5f, 200.0f, rock::RockSettings{}.density, "Rock density changed", true, "岩中心のばらまき間隔 (m)。岩同士の中心間距離を決めます。岩サイズとは独立。", "%.2f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Coverage (%)", "RockCoverage", &rk.coverage, 0.0f, 1.0f, rock::RockSettings{}.coverage, "Rock coverage changed", "scatter 点が岩になる確率です。1.0 で全点、下げると元の地形が見える隙間が増えます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Rock Size Min (m)", "RockSizeMinM", &rk.rockSizeMinM, 0.1f, 200.0f, rock::RockSettings{}.rockSizeMinM, "Rock size min changed", true, "岩の最小直径 (m)。各岩は [Min, Max] の範囲からランダムに選ばれます。Density より大きいと岩が重なり、小さいと隙間ができます。", "%.2f"))
    {
        if (rk.rockSizeMaxM < rk.rockSizeMinM) rk.rockSizeMaxM = rk.rockSizeMinM;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Rock Size Max (m)", "RockSizeMaxM", &rk.rockSizeMaxM, 0.1f, 200.0f, rock::RockSettings{}.rockSizeMaxM, "Rock size max changed", true, "岩の最大直径 (m)。Min < Max で自動補正。重なる岩同士は max 合成され、接合線で自然な折れ線が出ます。", "%.2f"))
    {
        if (rk.rockSizeMaxM < rk.rockSizeMinM) rk.rockSizeMinM = rk.rockSizeMaxM;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Rock Height (m)", "RockHeight", &rk.rockHeight, 0.0f, 50.0f, rock::RockSettings{}.rockHeight, "Rock height changed", true, "岩塊の最大盛り上がり (m)。地形の起伏スケールに対して大きすぎると岩肌が浮き上がりすぎるので、地形の標高変化の数% 程度が目安。", "%.2f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Height Jitter (%)", "RockHeightJitter", &rk.heightJitter, 0.0f, 1.0f, rock::RockSettings{}.heightJitter, "Rock height jitter changed", "岩ごとの高さ振れ幅です。0 で全部同じ高さ、1 で 0 倍〜2 倍の範囲でランダム。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Rotation Variation (%)", "RockRotationVariation", &rk.rotationVariation, 0.0f, 1.0f, rock::RockSettings{}.rotationVariation, "Rock rotation variation changed", "各岩のランダム回転量です。0 で全岩が同じ向き、1 で完全ランダム回転。表面の面の向きが岩ごとに変わるので散らばり感が出ます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Aspect Variation (%)", "RockAspectVariation", &rk.aspectVariation, 0.0f, 1.0f, rock::RockSettings{}.aspectVariation, "Rock aspect variation changed", "各岩の細長さの振れ幅です。0 で円形、1 で最大 2:1 まで細長い岩が混ざります。回転と組み合わせて GeoGen のような不揃いな配置になります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Edge Sharpness (%)", "RockEdgeSharpness", &rk.edgeSharpness, 0.0f, 1.0f, rock::RockSettings{}.edgeSharpness, "Rock edge sharpness changed", "岩のシルエット形状です。0 で滑らかな円形ドーム、1 で 4–7 角形のダイヤモンドカット風シルエット。岩ごとに辺数・角度・半径がランダムに揺らぎます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Bumpiness (%)", "RockBumpiness", &rk.bumpiness, 0.0f, 1.0f, rock::RockSettings{}.bumpiness, "Rock bumpiness changed", "表面ディテールの振幅です。0 で滑らかなドーム、上げるほど岩肌の凹凸が強くなります。Facet Sharpness と組み合わせて多面体感を調整します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Facet Sharpness (%)", "RockFacetSharpness", &rk.facetSharpness, 0.0f, 1.0f, rock::RockSettings{}.facetSharpness, "Rock facet sharpness changed", "表面ディテールの形状です。0 で滑らかな丸み、1 で多面体状の平らな面 + 鋭いエッジ。岩肌に角を立てたいときに上げます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Facet Scale", "RockFacetScale", &rk.facetScale, 0.5f, 8.0f, rock::RockSettings{}.facetScale, "Rock facet scale changed", true, "1 つの岩に乗る面の細かさです。大きいほど面が小さく細かくなり、小さいほど大きな面が少数現れます。", "%.2f"))
    {
        EvaluateGraph();
    }

    {
        constexpr std::array<float, 9> kGroundDetailLevels = {0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f};
        int detailIndex = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (int i = 0; i < static_cast<int>(kGroundDetailLevels.size()); ++i)
        {
            const float distance = std::abs(rk.groundDetailLevelM - kGroundDetailLevels[static_cast<size_t>(i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                detailIndex = i;
            }
        }
        if (DrawPropertyComboRow("Ground Detail Level", "RockGroundDetailLevel", &detailIndex, "Max\0" "1 m\0" "2 m\0" "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "128 m\0" "\0", "岩を置く底面に使う地形ディテールです。Max は入力地形そのまま、数値を上げるほど小さな凹凸をならした下地に岩を乗せます。", 0))
        {
            rk.groundDetailLevelM = kGroundDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }

    ImGui::EndTable();
    return true;
}

bool DrawScatterProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("ScatterRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::ScatterSettings& sc = editableNode.scatter;
    sc.shapeType = static_cast<rock::ScatterShapeType>(std::clamp(static_cast<int>(sc.shapeType),
        static_cast<int>(rock::ScatterShapeType::Hemisphere),
        static_cast<int>(rock::ScatterShapeType::Cone)));
    sc.orientationRule = static_cast<rock::RockOrientationRule>(std::clamp(static_cast<int>(sc.orientationRule),
        static_cast<int>(rock::RockOrientationRule::Flat),
        static_cast<int>(rock::RockOrientationRule::SlopeOriented)));
    sc.seed = std::clamp(sc.seed, 0, 999999);
    sc.density = std::clamp(sc.density, 0.5f, 1000.0f);
    sc.coverage = std::clamp(sc.coverage, 0.0f, 1.0f);
    sc.sizeMinM = std::clamp(sc.sizeMinM, 0.1f, 200.0f);
    sc.sizeMaxM = std::clamp(std::max(sc.sizeMaxM, sc.sizeMinM), 0.1f, 200.0f);
    sc.height = std::clamp(sc.height, 0.0f, 100.0f);
    sc.heightJitter = std::clamp(sc.heightJitter, 0.0f, 1.0f);
    sc.rotationVariation = std::clamp(sc.rotationVariation, 0.0f, 1.0f);
    sc.aspectVariation = std::clamp(sc.aspectVariation, 0.0f, 1.0f);
    sc.backend = static_cast<rock::ScatterBackend>(std::clamp(static_cast<int>(sc.backend),
        static_cast<int>(rock::ScatterBackend::CpuReference),
        static_cast<int>(rock::ScatterBackend::GpuCompute)));

    {
        int backendInt = static_cast<int>(sc.backend);
        if (DrawPropertyComboRow("Backend", "ScatterBackend", &backendInt, "CPU\0GPU\0\0", "実行バックエンド。GPU (D3D12 compute) は Mask 入力なし、Ground Detail Level が Max の場合に使われます。それ以外や失敗時は CPU にフォールバックします。", static_cast<int>(rock::ScatterSettings{}.backend)))
        {
            sc.backend = static_cast<rock::ScatterBackend>(std::clamp(backendInt,
                static_cast<int>(rock::ScatterBackend::CpuReference),
                static_cast<int>(rock::ScatterBackend::GpuCompute)));
            EvaluateGraph();
        }
    }
    {
        int shapeInt = static_cast<int>(sc.shapeType);
        if (DrawPropertyComboRow("Shape Type", "ScatterShapeType", &shapeInt, "Hemisphere\0Cone\0\0", "散布するプロキシ形状です。Hemisphere は丸い植物や低木の分布、Cone は尖った草や小さな樹形の確認に使えます。", static_cast<int>(rock::ScatterSettings{}.shapeType)))
        {
            sc.shapeType = static_cast<rock::ScatterShapeType>(std::clamp(shapeInt,
                static_cast<int>(rock::ScatterShapeType::Hemisphere),
                static_cast<int>(rock::ScatterShapeType::Cone)));
            EvaluateGraph();
        }
    }
    {
        int orientationInt = static_cast<int>(sc.orientationRule);
        if (DrawPropertyComboRow("Orientation Rule", "ScatterOrientationRule", &orientationInt, "Flat\0Follow Ground\0Slope Oriented\0\0", "散布形状の向きと斜面への沿わせ方です。Flat は従来通り水平基準、Follow Ground は斜面距離と法線の上向き成分を使って地形に沿わせ、Slope Oriented は細長い個体の回転を斜面方向へ寄せます。", static_cast<int>(rock::ScatterSettings{}.orientationRule)))
        {
            sc.orientationRule = static_cast<rock::RockOrientationRule>(std::clamp(orientationInt,
                static_cast<int>(rock::RockOrientationRule::Flat),
                static_cast<int>(rock::RockOrientationRule::SlopeOriented)));
            EvaluateGraph();
        }
    }
    if (DrawPropertyIntRow("Seed", "ScatterSeed", &sc.seed, 0, 999999, rock::ScatterSettings{}.seed, "Scatter seed changed", true, "散布位置と個体差のシードです。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Density (m)", "ScatterDensity", &sc.density, 0.5f, 200.0f, rock::ScatterSettings{}.density, "Scatter density changed", true, "scatter 中心の間隔です。植生分布では株間や群落の粒度として扱えます。", "%.2f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Coverage (%)", "ScatterCoverage", &sc.coverage, 0.0f, 1.0f, rock::ScatterSettings{}.coverage, "Scatter coverage changed", "scatter 点が実際に配置される確率です。入力 Mask がある場合は Mask の値でさらに制限されます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Size Min (m)", "ScatterSizeMinM", &sc.sizeMinM, 0.1f, 200.0f, rock::ScatterSettings{}.sizeMinM, "Scatter size min changed", true, "散布形状の最小直径です。", "%.2f"))
    {
        if (sc.sizeMaxM < sc.sizeMinM) sc.sizeMaxM = sc.sizeMinM;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Size Max (m)", "ScatterSizeMaxM", &sc.sizeMaxM, 0.1f, 200.0f, rock::ScatterSettings{}.sizeMaxM, "Scatter size max changed", true, "散布形状の最大直径です。Min < Max で範囲内からランダムに選ばれます。", "%.2f"))
    {
        if (sc.sizeMaxM < sc.sizeMinM) sc.sizeMinM = sc.sizeMaxM;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Height (m)", "ScatterHeight", &sc.height, 0.0f, 50.0f, rock::ScatterSettings{}.height, "Scatter height changed", true, "形状を地形へ盛り上げる高さです。0 のままでも Mask と Unique Mask は出力されるので、植生分布用マスクとして使えます。", "%.2f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Height Jitter (%)", "ScatterHeightJitter", &sc.heightJitter, 0.0f, 1.0f, rock::ScatterSettings{}.heightJitter, "Scatter height jitter changed", "個体ごとの高さ振れ幅です。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Rotation Variation (%)", "ScatterRotationVariation", &sc.rotationVariation, 0.0f, 1.0f, rock::ScatterSettings{}.rotationVariation, "Scatter rotation variation changed", "細長い個体の向きのばらつきです。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Aspect Variation (%)", "ScatterAspectVariation", &sc.aspectVariation, 0.0f, 1.0f, rock::ScatterSettings{}.aspectVariation, "Scatter aspect variation changed", "個体の細長さの振れ幅です。0 で円形、上げるほど楕円形が混ざります。"))
    {
        EvaluateGraph();
    }

    {
        constexpr std::array<float, 9> kGroundDetailLevels = {0.0f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f};
        int detailIndex = 0;
        float bestDistance = std::numeric_limits<float>::max();
        for (int i = 0; i < static_cast<int>(kGroundDetailLevels.size()); ++i)
        {
            const float distance = std::abs(sc.groundDetailLevelM - kGroundDetailLevels[static_cast<size_t>(i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                detailIndex = i;
            }
        }
        if (DrawPropertyComboRow("Ground Detail Level", "ScatterGroundDetailLevel", &detailIndex, "Max\0" "1 m\0" "2 m\0" "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "128 m\0" "\0", "散布形状を置く底面に使う地形ディテールです。Max は入力地形そのまま、数値を上げるほど小さな凹凸をならした下地に配置します。", 0))
        {
            sc.groundDetailLevelM = kGroundDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }

    ImGui::EndTable();
    return true;
}

bool DrawCrumblingProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("CrumblingRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::CrumblingSettings& cr = editableNode.crumbling;
    cr.physicsCount = std::clamp(cr.physicsCount, 0, 512);
    cr.debrisAmount = std::clamp(cr.debrisAmount, 0.0f, 1.0f);
    cr.debrisSizeMinM = std::clamp(cr.debrisSizeMinM, 0.1f, 1000.0f);
    cr.debrisSizeMaxM = std::clamp(std::max(cr.debrisSizeMaxM, cr.debrisSizeMinM), 0.1f, 1000.0f);
    cr.style = static_cast<rock::RockStyle>(std::clamp(static_cast<int>(cr.style),
        static_cast<int>(rock::RockStyle::Classic),
        static_cast<int>(rock::RockStyle::Shard)));
    cr.gravity = std::clamp(cr.gravity, 0.0f, 1.0f);
    cr.spread = std::clamp(cr.spread, 0.0f, 1.0f);
    cr.seed = std::clamp(cr.seed, 0, 999999);

    if (DrawPropertyIntRow("Physics Count", "CrumblingPhysicsCount", &cr.physicsCount, 0, 512, rock::CrumblingSettings{}.physicsCount, "Crumbling physics count changed", true, "崩落粒子を下方向へ進めるステップ数です。大きいほど岩屑が斜面下部へ長く流れて、広くばらけます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Debris Amount (%)", "CrumblingDebrisAmount", &cr.debrisAmount, 0.0f, 1.0f, rock::CrumblingSettings{}.debrisAmount, "Crumbling debris amount changed", "Emission Mask から発生する岩屑の量です。上げるほど粒子数と盛り上がりが増えます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Debris Min Size (m)", "CrumblingDebrisSizeMin", &cr.debrisSizeMinM, 0.1f, 200.0f, rock::CrumblingSettings{}.debrisSizeMinM, "Crumbling debris min size changed", true, "崩落岩片の最小直径です。小さいほど細かい砂礫、大きいほど転石寄りになります。", "%.2f", 0, 0.1f, 1000.0f))
    {
        if (cr.debrisSizeMaxM < cr.debrisSizeMinM) cr.debrisSizeMaxM = cr.debrisSizeMinM;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Debris Max Size (m)", "CrumblingDebrisSizeMax", &cr.debrisSizeMaxM, 0.1f, 200.0f, rock::CrumblingSettings{}.debrisSizeMaxM, "Crumbling debris max size changed", true, "崩落岩片の最大直径です。Min < Max で範囲内からランダムに大きさを選びます。", "%.2f", 0, 0.1f, 1000.0f))
    {
        if (cr.debrisSizeMaxM < cr.debrisSizeMinM) cr.debrisSizeMinM = cr.debrisSizeMaxM;
        EvaluateGraph();
    }
    {
        int styleInt = static_cast<int>(cr.style);
        if (DrawPropertyComboRow("Rock Style", "CrumblingRockStyle", &styleInt, "Classic\0Polygonal\0Shard\0\0", "岩片の基本シェープです。Classic は丸み、Polygonal は低ポリゴン状、Shard は斜面に流れた破片状の形になります。", static_cast<int>(rock::CrumblingSettings{}.style)))
        {
            cr.style = static_cast<rock::RockStyle>(std::clamp(styleInt,
                static_cast<int>(rock::RockStyle::Classic),
                static_cast<int>(rock::RockStyle::Shard)));
            EvaluateGraph();
        }
    }
    if (DrawPropertyPercentRow("Gravity (%)", "CrumblingGravity", &cr.gravity, 0.0f, 1.0f, rock::CrumblingSettings{}.gravity, "Crumbling gravity changed", "低い方へ流れる強さです。高いほど直線的に下り、低いほど地形の細部やランダムな散り方が残ります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Spread (%)", "CrumblingSpread", &cr.spread, 0.0f, 1.0f, rock::CrumblingSettings{}.spread, "Crumbling spread changed", "進行方向から横へ逸れる強さです。上げるほど岩屑が筋状に重なりにくく、停止後の岩片も軽く押し分けられます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Seed", "CrumblingSeed", &cr.seed, 0, 999999, rock::CrumblingSettings{}.seed, "Crumbling seed changed", true, "岩屑の発生位置とばらつきのシードです。"))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}

bool DrawSedimentProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("SedimentRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::SedimentSettings& sd = editableNode.sediment;
    sd.iterations = std::clamp(sd.iterations, 1, 1000);
    sd.stabilizationIterations = std::clamp(sd.stabilizationIterations, 1, 32);
    sd.largestDetailLevelM = std::clamp(sd.largestDetailLevelM, 1.0f, 1024.0f);
    sd.emissionAmountM = std::clamp(sd.emissionAmountM, 0.0f, 1000.0f);
    sd.emissionTime = std::clamp(sd.emissionTime, 0.0f, 1.0f);
    sd.sedimentViscosity = std::clamp(sd.sedimentViscosity, 0.0f, 1.0f);
    sd.maskContrast = std::clamp(sd.maskContrast, 0.0f, 1.0f);

    {
        int backendInt = static_cast<int>(sd.backend);
        if (DrawPropertyComboRow("Backend", "SedimentBackend", &backendInt, "CPU\0GPU\0\0", "実行バックエンド。GPU (D3D12 compute) は CPU 比で 10-30 倍高速 (1024² で 100ms 程度)。シェーダーコンパイル/ディスパッチ失敗時は CPU に自動フォールバック。CPU は決定論的でデバッグ向き。", static_cast<int>(rock::SedimentSettings{}.backend)))
        {
            sd.backend = static_cast<rock::SedimentBackend>(std::clamp(backendInt,
                static_cast<int>(rock::SedimentBackend::CpuReference),
                static_cast<int>(rock::SedimentBackend::GpuCompute)));
            EvaluateGraph();
        }
    }
    if (DrawPropertyPercentRow("Emission Time (%)", "SedimentEmissionTime", &sd.emissionTime, 0.0f, 1.0f, rock::SedimentSettings{}.emissionTime, "Sediment emission time changed", "Emission Amount を最初の何割の Iteration にかけて徐々に追加するか。0% は最初に全量を一度に積む (緩い層が自由に流れて落ち着く)、100% は毎 Iteration に均等追加 (前 Iteration が彫った河道に新層が流れ込みディテールがシャープになる)。"))
    {
        EvaluateGraph();
    }
    {
        constexpr std::array<float, 10> kSedimentDetailLevels = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f};
        int detailIndex = 3;
        float bestDistance = FLT_MAX;
        for (int i = 0; i < static_cast<int>(kSedimentDetailLevels.size()); ++i)
        {
            const float distance = std::abs(sd.largestDetailLevelM - kSedimentDetailLevels[static_cast<size_t>(i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                detailIndex = i;
            }
        }
        if (DrawPropertyComboRow("Largest Detail Level (m)", "SedimentLargestDetailLevel", &detailIndex, "1 m\0" "2 m\0" "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "128 m\0" "256 m\0" "512 m\0" "\0", "1 iteration あたりの沈降距離スケールです。大きいほど広い盆地まで早く落ち着きますが、スライドパス数が増えて重くなります。小さいほど細部優先です。", 3))
        {
            detailIndex = std::clamp(detailIndex, 0, static_cast<int>(kSedimentDetailLevels.size()) - 1);
            sd.largestDetailLevelM = kSedimentDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }
    if (DrawPropertyIntRow("Iterations Count", "SedimentIterations", &sd.iterations, 1, 500, rock::SedimentSettings{}.iterations, "Sediment iterations changed", true, "外側の緩和反復回数。各反復で全スケールを粗→細で 1 周します。多いほど安定状態に近づきます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Stabilization Iterations", "SedimentStabilization", &sd.stabilizationIterations, 1, 16, rock::SedimentSettings{}.stabilizationIterations, "Sediment stabilization changed", true, "1 反復・1 スケール内で何回の安息角スライドを連続実行するか。多いほど各スケールがそのスケール内で完全に静定します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Sediment Viscosity (%)", "SedimentViscosity", &sd.sedimentViscosity, 0.0f, 1.0f, rock::SedimentSettings{}.sedimentViscosity, "Sediment viscosity changed", "堆積物の流動性 / 安息角を制御 (二乗カーブ)。0% = 0° (完全流体、谷底で水平面に均される)、20% (既定) ≈ 3° (ほぼ平らな堆積、GeoGen 相当)、50% = 20°、100% = 80° (粘り強く急斜面でも崩れない)。低いほど谷で水平な池状に、高いほど中腹に急な堆積として積もります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Emission Amount (m)", "SedimentEmissionAmount", &sd.emissionAmountM, 0.0f, 100.0f, rock::SedimentSettings{}.emissionAmountM, "Sediment emission amount changed", true, "全セルに上乗せする堆積物の総厚 (m)。Convert Terrain to Sediment が ON のときは元地形に対する追加分です。", "%.2f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Convert Terrain to Sediment", "SedimentConvertTerrain", &sd.convertTerrainToSediment, "Sediment convert terrain changed", "ON: 入力ハイトフィールド全体を可動堆積物として扱います (基盤 = 平坦)。山頂が崩れて谷を埋め、典型的な GeoGen 風の樹枝状デポジット模様になります。OFF: 入力は固定基盤、Emission Amount で追加した分だけが流れます。", rock::SedimentSettings{}.convertTerrainToSediment, true))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Mask Contrast (%)", "SedimentMaskContrast", &sd.maskContrast, 0.0f, 1.0f, rock::SedimentSettings{}.maskContrast, "Sediment mask contrast changed", "Mask 出力のコントラスト。0 で線形 (滑らかなグラデーション)、1 でほぼバイナリ。dendritic を強調するなら 0.5 以上。"))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}

bool DrawSnowProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("SnowRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::SnowSettings& sn = editableNode.snow;
    sn.emissionAmount = std::clamp(sn.emissionAmount, 0.0f, 100.0f);
    sn.slopeLimitMinDeg = std::clamp(sn.slopeLimitMinDeg, 0.0f, 89.9f);
    sn.slopeLimitMaxDeg = std::clamp(std::max(sn.slopeLimitMaxDeg, sn.slopeLimitMinDeg), 0.0f, 89.9f);
    sn.maskMaxSnow = std::clamp(sn.maskMaxSnow, 0.001f, 1000.0f);
    sn.iterationCount = std::clamp(sn.iterationCount, 1, 256);
    sn.emissionTime = std::clamp(sn.emissionTime, 0.0f, 1.0f);
    sn.smoothingIterations = std::clamp(sn.smoothingIterations, 1, 16);
    sn.motionSlopeLimitDeg = std::clamp(sn.motionSlopeLimitDeg, 0.0f, 89.9f);
    sn.transportRate = std::clamp(sn.transportRate, 0.0f, 1.0f);
    sn.surfaceSmoothing = std::clamp(sn.surfaceSmoothing, 0.0f, 1.0f);
    sn.maskThresholdM = std::clamp(sn.maskThresholdM, 0.0f, 1000.0f);
    sn.maskFeatherM = std::clamp(sn.maskFeatherM, 0.0f, 1000.0f);
    sn.largestDetailLevelM = std::clamp(sn.largestDetailLevelM, 1.0f, 1024.0f);
    sn.fillRadius = std::clamp(sn.fillRadius, 1, 8);

    {
        int backendInt = static_cast<int>(sn.backend);
        if (DrawPropertyComboRow("Backend", "SnowBackend", &backendInt, "CPU\0GPU\0\0", "Snow の再配分モデルは現在 CPU 参照実装で評価します。GPU は今後の移植用に保存されますが、現時点では同じ CPU 経路にフォールバックします。", static_cast<int>(rock::SnowSettings{}.backend)))
        {
            sn.backend = static_cast<rock::SnowBackend>(std::clamp(backendInt,
                static_cast<int>(rock::SnowBackend::CpuReference),
                static_cast<int>(rock::SnowBackend::GpuCompute)));
            EvaluateGraph();
        }
    }
    if (DrawPropertyFloatRow("Emission Amount (m)", "SnowEmissionAmount", &sn.emissionAmount, 0.0f, 50.0f, rock::SnowSettings{}.emissionAmount, "Snow emission amount changed", true, "平地 (slope <= Slope Limit Min) に積もる雪の最大厚み (m)。地形の高さが全体的にこの値だけ持ち上がる感覚です。", "%.2f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Iterations Count", "SnowIterations", &sn.iterationCount, 1, 256, rock::SnowSettings{}.iterationCount, "Snow iterations changed", true, "雪を何ステップで積もらせて安定化するかです。大きいほど少しずつ積もり、Emission Amount が小さいときの急な谷埋めを抑えやすくなります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Emission Time (%)", "SnowEmissionTime", &sn.emissionTime, 0.0f, 1.0f, rock::SnowSettings{}.emissionTime, "Snow emission time changed", "Iterations Count のうち、どの割合まで雪を降らせ続けるかです。0% は最初に全量を置いてから安定化し、100% は最後まで少しずつ降らせます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Snow Motion Slope Limit (deg)", "SnowMotionSlopeLimit", &sn.motionSlopeLimitDeg, 0.0f, 89.9f, rock::SnowSettings{}.motionSlopeLimitDeg, "Snow motion slope limit changed", true, "この角度以下の雪面では雪が流れず、これより急な雪面では低い隣接セルへ雪が移動します。GeoGen の snow motion slope limit 相当です。", "%.1f"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Transport Rate (%)", "SnowTransportRate", &sn.transportRate, 0.0f, 1.0f, rock::SnowSettings{}.transportRate, "Snow transport rate changed", "不安定な雪のうち、1 回の安定化パスで下へ動かす割合です。高いほど急斜面から雪が早く逃げ、谷や棚に集まりやすくなります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyPercentRow("Snow Surface Smoothing (%)", "SnowSurfaceSmoothing", &sn.surfaceSmoothing, 0.0f, 1.0f, rock::SnowSettings{}.surfaceSmoothing, "Snow surface smoothing changed", "積もった雪面だけをならす強さです。半径は Largest Detail Level (m) を使うため、追加のスケール設定はありません。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Mask Threshold (m)", "SnowMaskThreshold", &sn.maskThresholdM, 0.0f, 1.0f, rock::SnowSettings{}.maskThresholdM, "Snow mask threshold changed", true, "この雪厚以上を積雪域として白に近づけます。中間グレーを減らし、積もっている場所と地面が出ている場所を分けるためのしきい値です。", "%.3f", 0, 0.0f, 1000.0f))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Mask Feather (m)", "SnowMaskFeather", &sn.maskFeatherM, 0.0f, 0.5f, rock::SnowSettings{}.maskFeatherM, "Snow mask feather changed", true, "積雪境界だけを少しグレーにする幅です。0 にするとほぼ二値のマスクになります。", "%.3f", 0, 0.0f, 1000.0f))
    {
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Settling Passes", "SnowSmoothingIterations", &sn.smoothingIterations, 1, 16, rock::SnowSettings{}.smoothingIterations, "Snow settling passes changed", true, "各 simulation step の中で雪を低い場所へ再配分する回数です。大きいほど急斜面から雪が逃げ、谷底や棚へまとまりやすくなります。"))
    {
        EvaluateGraph();
    }
    {
        constexpr std::array<float, 8> kSnowDetailLevels = {4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f};
        int detailIndex = 1;
        float bestDistance = FLT_MAX;
        for (int i = 0; i < static_cast<int>(kSnowDetailLevels.size()); ++i)
        {
            const float distance = std::abs(sn.largestDetailLevelM - kSnowDetailLevels[static_cast<size_t>(i)]);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                detailIndex = i;
            }
        }
        if (DrawPropertyComboRow("Largest Detail Level (m)", "SnowLargestDetailLevel", &detailIndex, "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "128 m\0" "256 m\0" "512 m\0" "\0", "GeoGen Snow の Largest detail level 相当です。雪面をならして隙間を埋める最大スケールをメートル単位で選びます。4m は細い隙間まで追いやすく、512m は大きなスケールの積雪面を作ります。", 1))
        {
            detailIndex = std::clamp(detailIndex, 0, static_cast<int>(kSnowDetailLevels.size()) - 1);
            sn.largestDetailLevelM = kSnowDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }

    ImGui::EndTable();
    return true;
}


bool DrawMultiScaleErosionProperties(rock::Node& editableNode)
{
    if (!ImGui::BeginTable("MultiScaleErosionRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return false;
    }

    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    rock::MultiScaleErosionSettings& mse = editableNode.multiScaleErosion;
    mse.iterations = std::clamp(mse.iterations, 0, 500);
    mse.speStrength = std::clamp(mse.speStrength, 0.0f, 0.01f);
    mse.streamExponent = std::clamp(mse.streamExponent, 0.0f, 2.0f);
    mse.slopeExponent = std::clamp(mse.slopeExponent, 0.0f, 4.0f);
    mse.maxStreamPower = std::clamp(mse.maxStreamPower, 1.0f, 1000000.0f);
    mse.flowExponent = std::clamp(mse.flowExponent, 0.5f, 4.0f);
    mse.speTimeStep = std::clamp(mse.speTimeStep, 0.0f, 4.0f);
    mse.thermalAngleDegrees = std::clamp(mse.thermalAngleDegrees, 0.0f, 60.0f);
    mse.thermalStrength = std::clamp(mse.thermalStrength, 0.0f, 0.01f);
    mse.thermalNoiseMin = std::clamp(mse.thermalNoiseMin, 0.0f, 4.0f);
    mse.thermalNoiseMax = std::clamp(mse.thermalNoiseMax, 0.0f, 4.0f);
    mse.thermalNoiseWavelength = std::clamp(mse.thermalNoiseWavelength, 0.0f, 0.05f);
    mse.depositionStrength = std::clamp(mse.depositionStrength, 0.0f, 8.0f);
    mse.rain = std::clamp(mse.rain, 0.0f, 10.0f);

    {
        int backendInt = static_cast<int>(mse.backend);
        if (DrawPropertyComboRow("Backend", "MseBackend", &backendInt, "CPU\0GPU\0\0", "CPU 並列実装と GPU (D3D12 compute) を切り替えます。GPU は反復回数が多いほど速くなりますが、結果が CPU と微小にずれることがあります (浮動小数の累積順序)。\nGPU が初期化に失敗したり実行時エラーになると自動的に CPU 版にフォールバックします。", static_cast<int>(rock::MultiScaleErosionSettings{}.backend)))
        {
            mse.backend = static_cast<rock::MultiScaleErosionBackend>(std::clamp(backendInt,
                static_cast<int>(rock::MultiScaleErosionBackend::CpuReference),
                static_cast<int>(rock::MultiScaleErosionBackend::GpuCompute)));
            EvaluateGraph();
        }
    }
    if (DrawPropertyIntRow("Iterations", "MseIterations", &mse.iterations, 0, 500, rock::MultiScaleErosionSettings{}.iterations, "Multi-scale erosion iterations changed", true, "SPE → Thermal → Deposition の 3 パスを繰り返す回数です。Multigrid 有効時は各レベルで個別に反復します (粗→細の各段で同じ回数)。多いほど浸食が進みますが計算時間も増えます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Use Multigrid", "MseUseMultigrid", &mse.useMultigrid, "Multi-scale erosion multigrid toggled", "粗い解像度から目標解像度へ x2 アップサンプルしながら段階的に浸食を適用するピラミッド処理を有効にします。解像度を変えても結果が安定しやすくなります (Schott et al. 論文の本来の構成)。OFF にすると入力解像度で 1 段階のみの単純処理になります。", rock::MultiScaleErosionSettings{}.useMultigrid))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Enable Stream Power", "MseEnableSpe", &mse.enableStreamPower, "Multi-scale erosion SPE toggled", "河川浸食 (Stream Power Erosion) パスの ON/OFF。", rock::MultiScaleErosionSettings{}.enableStreamPower))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Enable Thermal", "MseEnableThermal", &mse.enableThermal, "Multi-scale erosion thermal toggled", "タラス崩壊 (角度しきい値による斜面安定化) パスの ON/OFF。", rock::MultiScaleErosionSettings{}.enableThermal))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Enable Deposition", "MseEnableDeposition", &mse.enableDeposition, "Multi-scale erosion deposition toggled", "土砂堆積パスの ON/OFF。流量と搬送能の差から谷底や合流部に土砂を残します。", rock::MultiScaleErosionSettings{}.enableDeposition))
    {
        EvaluateGraph();
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("Stream Power");
    ImGui::TableSetColumnIndex(1);
    ImGui::SeparatorText("Stream Power");
    if (DrawPropertyFloatRow("SPE Strength", "MseSpeStrength", &mse.speStrength, 0.0f, 0.01f, rock::MultiScaleErosionSettings{}.speStrength, "Multi-scale erosion SPE strength changed", true, "SPE シェーダーの k 係数。1 反復あたりの削り量倍率です。", "%.5f", ImGuiSliderFlags_Logarithmic))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Stream Exponent", "MseStreamExp", &mse.streamExponent, 0.0f, 2.0f, rock::MultiScaleErosionSettings{}.streamExponent, "Multi-scale erosion stream exponent changed", true, "SPE の p_sa。流量に対する非線形性です。大きいほど流量集中部で削れが強くなります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Slope Exponent", "MseSlopeExp", &mse.slopeExponent, 0.0f, 4.0f, rock::MultiScaleErosionSettings{}.slopeExponent, "Multi-scale erosion slope exponent changed", true, "SPE の p_sl。勾配に対する非線形性です。大きいほど急斜面でのみ削ります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Max Stream Power", "MseMaxSpe", &mse.maxStreamPower, 1.0f, 1000000.0f, rock::MultiScaleErosionSettings{}.maxStreamPower, "Multi-scale erosion max SPE changed", true, "SPE シェーダーの max_spe 上限。極端な削れの暴走を防ぎます。", "%.0f", ImGuiSliderFlags_Logarithmic))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Flow Exponent", "MseFlowExp", &mse.flowExponent, 0.5f, 4.0f, rock::MultiScaleErosionSettings{}.flowExponent, "Multi-scale erosion flow exponent changed", true, "D8 重み付きフローの集中度 (flow_p)。大きいほど最急方向に流量が集まります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Time Step", "MseTimeStep", &mse.speTimeStep, 0.0f, 4.0f, rock::MultiScaleErosionSettings{}.speTimeStep, "Multi-scale erosion time step changed", true, "SPE の dt。1 反復あたりの時間刻みです。大きいほど速いが不安定になります。"))
    {
        EvaluateGraph();
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("Thermal");
    ImGui::TableSetColumnIndex(1);
    ImGui::SeparatorText("Thermal");
    if (DrawPropertyFloatRow("Threshold Angle (deg)", "MseThermalAngle", &mse.thermalAngleDegrees, 0.0f, 60.0f, rock::MultiScaleErosionSettings{}.thermalAngleDegrees, "Multi-scale erosion thermal angle changed", true, "タラス崩壊の安息角。これを超える勾配は崩落します。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Thermal Strength", "MseThermalStrength", &mse.thermalStrength, 0.0f, 0.01f, rock::MultiScaleErosionSettings{}.thermalStrength, "Multi-scale erosion thermal strength changed", true, "タラスシェーダーの ε。1 反復あたりに移動する土砂量です。", "%.6f", ImGuiSliderFlags_Logarithmic))
    {
        EvaluateGraph();
    }
    if (DrawPropertyBoolRow("Noisify Angle", "MseThermalNoisify", &mse.thermalNoisifyAngle, "Multi-scale erosion noisify angle toggled", "安息角を空間ノイズで揺らし、岩質の不均一を表現します。", rock::MultiScaleErosionSettings{}.thermalNoisifyAngle))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Noise Min", "MseThermalNoiseMin", &mse.thermalNoiseMin, 0.0f, 4.0f, rock::MultiScaleErosionSettings{}.thermalNoiseMin, "Multi-scale erosion thermal noise min changed", true, "tan(角度) 倍率の下限。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Noise Max", "MseThermalNoiseMax", &mse.thermalNoiseMax, 0.0f, 4.0f, rock::MultiScaleErosionSettings{}.thermalNoiseMax, "Multi-scale erosion thermal noise max changed", true, "tan(角度) 倍率の上限。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Noise Wavelength", "MseThermalNoiseWavelength", &mse.thermalNoiseWavelength, 0.0f, 0.05f, rock::MultiScaleErosionSettings{}.thermalNoiseWavelength, "Multi-scale erosion thermal noise wavelength changed", true, "角度ノイズの空間周波数。小さいほど広い範囲で同じ角度になります。", "%.4f"))
    {
        EvaluateGraph();
    }
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextDisabled("Deposition");
    ImGui::TableSetColumnIndex(1);
    ImGui::SeparatorText("Deposition");
    if (DrawPropertyFloatRow("Deposition Strength", "MseDepositionStrength", &mse.depositionStrength, 0.0f, 8.0f, rock::MultiScaleErosionSettings{}.depositionStrength, "Multi-scale erosion deposition strength changed", true, "搬送能を超えた分の堆積率。大きいほど土砂が早く落ちます。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Rain", "MseRain", &mse.rain, 0.0f, 10.0f, rock::MultiScaleErosionSettings{}.rain, "Multi-scale erosion rain changed", true, "セルあたりに降る水量。大きいほど流量が増え、堆積も活発になります。"))
    {
        EvaluateGraph();
    }

    ImGui::EndTable();
    return true;
}


// グラデーションバーを ImDrawList で描画するヘルパー。stops は position 昇順でソート済み前提。
static void DrawGradientBar(ImDrawList* dl, ImVec2 barMin, ImVec2 barMax, const std::vector<rock::ColorStop>& stops)
{
    if (stops.empty()) { dl->AddRectFilled(barMin, barMax, IM_COL32(0,0,0,255)); return; }
    const float w = barMax.x - barMin.x;
    const int segments = static_cast<int>(w);
    for (int i = 0; i < segments; ++i)
    {
        const float t0 = static_cast<float>(i) / static_cast<float>(segments);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(segments);
        // sample at midpoint
        float t = (t0 + t1) * 0.5f;
        // SampleColorGradient-like inline
        float r = stops.back().r, g = stops.back().g, b = stops.back().b;
        if (t <= stops.front().position) { r = stops.front().r; g = stops.front().g; b = stops.front().b; }
        else
        {
            for (size_t si = 0; si + 1 < stops.size(); ++si)
            {
                if (t <= stops[si+1].position)
                {
                    float span = stops[si+1].position - stops[si].position;
                    float a = span > 0.0f ? (t - stops[si].position) / span : 0.0f;
                    r = stops[si].r + a * (stops[si+1].r - stops[si].r);
                    g = stops[si].g + a * (stops[si+1].g - stops[si].g);
                    b = stops[si].b + a * (stops[si+1].b - stops[si].b);
                    break;
                }
            }
        }
        const ImVec2 p0(barMin.x + t0 * w, barMin.y);
        const ImVec2 p1(barMin.x + t1 * w, barMax.y);
        dl->AddRectFilled(p0, p1, IM_COL32(
            static_cast<int>(std::clamp(r * 255.0f, 0.0f, 255.0f)),
            static_cast<int>(std::clamp(g * 255.0f, 0.0f, 255.0f)),
            static_cast<int>(std::clamp(b * 255.0f, 0.0f, 255.0f)),
            255));
    }
}

bool DrawColorizeProperties(rock::Node& editableNode)
{
    rock::ColorizeSettings& cs = editableNode.colorize;

    // stops を position 昇順に保つ
    std::sort(cs.stops.begin(), cs.stops.end(), [](const rock::ColorStop& a, const rock::ColorStop& b) {
        return a.position < b.position;
    });
    // 最低 2 ストップを保証
    if (cs.stops.empty()) { cs.stops = {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}}; }
    else if (cs.stops.size() == 1) { cs.stops.push_back({1.0f, 1.0f, 1.0f, 1.0f}); }

    bool changed = false;

    const char* backendItems[] = {"CPU", "GPU"};
    int backendIndex = static_cast<int>(cs.backend);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo("Backend", &backendIndex, backendItems, IM_ARRAYSIZE(backendItems)))
    {
        cs.backend = static_cast<rock::ColorizeBackend>(std::clamp(
            backendIndex,
            static_cast<int>(rock::ColorizeBackend::CpuParallel),
            static_cast<int>(rock::ColorizeBackend::GpuCompute)));
        changed = true;
    }

    // --- グラデーションバー ---
    ImGui::Spacing();
    ImGui::Indent(8.0f);
    const float barHeight = 24.0f;
    const float barWidth = std::clamp(ImGui::GetContentRegionAvail().x - 16.0f, 160.0f, 520.0f);
    const ImVec2 barMin = ImGui::GetCursorScreenPos();
    const ImVec2 barMax(barMin.x + barWidth, barMin.y + barHeight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawGradientBar(dl, barMin, barMax, cs.stops);
    dl->AddRect(barMin, barMax, IM_COL32(80, 80, 80, 255));

    // ストップハンドル (三角形マーカー)
    static int s_selectedStop = 0;
    static int s_draggingStop = -1;
    if (s_selectedStop >= static_cast<int>(cs.stops.size())) s_selectedStop = 0;
    if (s_draggingStop >= static_cast<int>(cs.stops.size())) s_draggingStop = -1;

    // インビジブルボタンでバークリックを検出 (ストップ追加 / 選択)
    ImGui::SetCursorScreenPos(barMin);
    ImGui::InvisibleButton("##gradbar", ImVec2(barWidth, barHeight + 12.0f));
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const float clickX = ImGui::GetIO().MousePos.x;
        const float t = std::clamp((clickX - barMin.x) / barWidth, 0.0f, 1.0f);
        float bestDist = FLT_MAX;
        int nearestStop = 0;
        for (int i = 0; i < static_cast<int>(cs.stops.size()); ++i)
        {
            const float dist = std::abs(cs.stops[i].position - t);
            if (dist < bestDist)
            {
                bestDist = dist;
                nearestStop = i;
            }
        }
        auto addStopAt = [&]() {
            // 追加時の色: 隣接ストップ間を線形補間
            float r = 1.0f, g = 1.0f, b = 1.0f;
            for (size_t si = 0; si + 1 < cs.stops.size(); ++si)
            {
                if (t <= cs.stops[si+1].position)
                {
                    const float span = cs.stops[si+1].position - cs.stops[si].position;
                    const float a = span > 0.0f ? (t - cs.stops[si].position) / span : 0.0f;
                    r = cs.stops[si].r + a * (cs.stops[si+1].r - cs.stops[si].r);
                    g = cs.stops[si].g + a * (cs.stops[si+1].g - cs.stops[si].g);
                    b = cs.stops[si].b + a * (cs.stops[si+1].b - cs.stops[si].b);
                    break;
                }
            }
            cs.stops.push_back({t, r, g, b});
            std::sort(cs.stops.begin(), cs.stops.end(), [](const rock::ColorStop& a, const rock::ColorStop& b){ return a.position < b.position; });
            float newBestDist = FLT_MAX;
            for (int i = 0; i < static_cast<int>(cs.stops.size()); ++i)
            {
                const float dist = std::abs(cs.stops[i].position - t);
                if (dist < newBestDist)
                {
                    newBestDist = dist;
                    s_selectedStop = i;
                }
            }
            s_draggingStop = s_selectedStop;
            changed = true;
        };

        const float pickRadius = std::max(0.006f, 8.0f / std::max(barWidth, 1.0f));
        if (bestDist <= pickRadius)
        {
            // 最も近いストップを選択
            s_selectedStop = nearestStop;
            s_draggingStop = s_selectedStop;
        }
        else
        {
            addStopAt();
        }
    }
    if (s_draggingStop >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        const float mouseX = ImGui::GetIO().MousePos.x;
        const float newPosition = std::clamp((mouseX - barMin.x) / barWidth, 0.0f, 1.0f);
        cs.stops[s_draggingStop].position = newPosition;
        std::sort(cs.stops.begin(), cs.stops.end(), [](const rock::ColorStop& a, const rock::ColorStop& b){ return a.position < b.position; });
        float bestDist = FLT_MAX;
        for (int i = 0; i < static_cast<int>(cs.stops.size()); ++i)
        {
            const float dist = std::abs(cs.stops[i].position - newPosition);
            if (dist < bestDist)
            {
                bestDist = dist;
                s_selectedStop = i;
            }
        }
        s_draggingStop = s_selectedStop;
        changed = true;
    }
    if (s_draggingStop >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        s_draggingStop = -1;
        EvaluateGraph();
    }

    // ハンドル描画
    for (int i = 0; i < static_cast<int>(cs.stops.size()); ++i)
    {
        const float hx = barMin.x + cs.stops[i].position * barWidth;
        const float hy = barMax.y + 2.0f;
        const ImVec2 tri[3] = {{hx - 5, hy}, {hx + 5, hy}, {hx, hy + 8}};
        const ImU32 col = (i == s_selectedStop) ? IM_COL32(255, 220, 80, 255) : IM_COL32(200, 200, 200, 255);
        dl->AddTriangleFilled(tri[0], tri[1], tri[2], col);
        dl->AddTriangle(tri[0], tri[1], tri[2], IM_COL32(0, 0, 0, 200));
    }
    ImGui::SetCursorScreenPos(ImVec2(barMin.x, barMax.y + 14.0f));

    ImGui::TextDisabled("クリックで追加 / ドラッグで位置変更 / Deleteで削除");
    ImGui::Spacing();

    // --- 選択中ストップの編集 ---
    if (s_selectedStop < static_cast<int>(cs.stops.size()))
    {
        ImGui::Text("Stop %d", s_selectedStop);
        ImGui::SameLine();

        auto deleteSelectedStop = [&]() {
            if (cs.stops.size() <= 2 || s_selectedStop < 0 || s_selectedStop >= static_cast<int>(cs.stops.size()))
            {
                return false;
            }
            cs.stops.erase(cs.stops.begin() + s_selectedStop);
            s_selectedStop = std::clamp(s_selectedStop - 1, 0, static_cast<int>(cs.stops.size()) - 1);
            s_draggingStop = -1;
            return true;
        };

        bool stopDeleted = false;
        // 削除ボタン (ストップが 2 以上の場合のみ)
        ImGui::BeginDisabled(cs.stops.size() <= 2);
        if (ImGui::SmallButton("削除"))
        {
            stopDeleted = deleteSelectedStop();
        }
        ImGui::EndDisabled();
        if (cs.stops.size() > 2 &&
            s_draggingStop < 0 &&
            ImGui::IsWindowFocused() &&
            !ImGui::IsAnyItemActive() &&
            !ImGui::GetIO().WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            stopDeleted = deleteSelectedStop();
        }
        if (stopDeleted)
        {
            changed = true;
        }

        if (!stopDeleted)
        {
            float posVal = cs.stops[s_selectedStop].position;
            ImGui::SetNextItemWidth(116.0f);
            if (ImGui::DragFloat("Position", &posVal, 0.001f, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp))
            {
                cs.stops[s_selectedStop].position = posVal;
                std::sort(cs.stops.begin(), cs.stops.end(), [](const rock::ColorStop& a, const rock::ColorStop& b){ return a.position < b.position; });
                float bestDist = FLT_MAX;
                for (int i = 0; i < static_cast<int>(cs.stops.size()); ++i)
                {
                    const float dist = std::abs(cs.stops[i].position - posVal);
                    if (dist < bestDist)
                    {
                        bestDist = dist;
                        s_selectedStop = i;
                    }
                }
                changed = true;
            }
            rock::ColorStop& selectedStop = cs.stops[s_selectedStop];
            float col3[3] = {selectedStop.r, selectedStop.g, selectedStop.b};
            if (ImGui::ColorEdit3("##stopColor", col3, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB))
            {
                selectedStop.r = col3[0]; selectedStop.g = col3[1]; selectedStop.b = col3[2];
                changed = true;
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) { EvaluateGraph(); }
        }
        if (changed && !ImGui::IsAnyItemActive()) { EvaluateGraph(); }

        // スクリーンカラーピッカー
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool myPicking = (g_screenPick.nodeId == editableNode.id &&
                                g_screenPick.mode != ScreenPickMode::Idle);

        if (myPicking)
        {
            // ---- ピッキング中 UI ----
            const ImVec4 previewCol(g_screenPick.previewR, g_screenPick.previewG, g_screenPick.previewB, 1.0f);

            if (g_screenPick.mode == ScreenPickMode::DragArmed)
            {
                ImGui::ColorButton("##pickPreview", previewCol, ImGuiColorEditFlags_NoBorder, ImVec2(22, 22));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Ctrl 待機中...");
                ImGui::TextDisabled("取得したい色の上で Ctrl を押しながら移動してください");
            }
            else // DragCollecting
            {
                const int cnt = static_cast<int>(g_screenPick.dragSamples.size());
                ImGui::ColorButton("##pickPreview", previewCol, ImGuiColorEditFlags_NoBorder, ImVec2(22, 22));
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "収集中... %d サンプル", cnt);

                // ミニプレビューグラデーション (収集済みサンプルを縮小表示)
                if (cnt >= 2)
                {
                    const float miniW = ImGui::GetContentRegionAvail().x - 8.0f;
                    const float miniH = 10.0f;
                    const ImVec2 mMin = ImGui::GetCursorScreenPos();
                    const ImVec2 mMax(mMin.x + miniW, mMin.y + miniH);
                    ImDrawList* mDl = ImGui::GetWindowDrawList();
                    const int segs = std::min(cnt, static_cast<int>(miniW));
                    for (int si = 0; si < segs; ++si)
                    {
                        const size_t idx = static_cast<size_t>(si) * static_cast<size_t>(cnt - 1) / static_cast<size_t>(std::max(1, segs - 1));
                        const auto& sc = g_screenPick.dragSamples[std::min(idx, g_screenPick.dragSamples.size()-1)];
                        const ImVec2 sp0(mMin.x + static_cast<float>(si) * miniW / static_cast<float>(segs), mMin.y);
                        const ImVec2 sp1(mMin.x + static_cast<float>(si + 1) * miniW / static_cast<float>(segs), mMax.y);
                        mDl->AddRectFilled(sp0, sp1, IM_COL32(
                            static_cast<int>(sc[0]*255), static_cast<int>(sc[1]*255), static_cast<int>(sc[2]*255), 255));
                    }
                    mDl->AddRect(mMin, mMax, IM_COL32(80,80,80,255));
                    ImGui::Dummy(ImVec2(miniW, miniH + 2.0f));
                }
                ImGui::TextDisabled("Ctrl を離すとグラデーションに投影します");
            }

            ImGui::Spacing();
            if (ImGui::SmallButton("Esc でキャンセル"))
            {
                g_screenPick.mode = ScreenPickMode::Idle;
                g_screenPick.dragSamples.clear();
            }
        }
        else
        {
            // ---- 通常時: ピッカーボタン ----
            if (ImGui::Button("Ctrl で取得", ImVec2(128.0f, 30.0f)))
            {
                g_screenPick.mode    = ScreenPickMode::DragArmed;
                g_screenPick.nodeId  = editableNode.id;
                g_screenPick.dragSamples.clear();
                // ボタン離し直後の Ctrl 状態を初期値にしてフォルスエッジを防ぐ
                g_screenPick.prevCtrl = IsCtrlDown();
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Ctrl を押しながらマウスを移動すると軌跡の色を収集します。\n"
                    "Ctrl を離した時点で間引きし、収集時の位置を保ってグラデーション化します。\n"
                    "既存のストップはすべて置き換えられます。\n"
                    "他アプリ上でも使用可能。Esc でキャンセル。");
            }
        }
    }

    ImGui::Unindent(8.0f);
    ImGui::Spacing();
    return true;
}

bool DrawPathProperties(rock::Node& editableNode)
{
    bool changed = false;
    if (ImGui::BeginTable("PathPropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        changed |= DrawPropertyFloatRow("Default Width (m)", "PathDefaultWidth", &editableNode.path.defaultWidthMeters, 0.01f, 100000.0f, rock::PathSettings{}.defaultWidthMeters, "Path default width changed", false, "新しく作るエッジの幅です。", "%.1f");
        changed |= DrawPropertyFloatRow("Default Feather (m)", "PathDefaultFeather", &editableNode.path.defaultFeatherMeters, 0.0f, 100000.0f, rock::PathSettings{}.defaultFeatherMeters, "Path default feather changed", false, "新しく作るエッジのフェザー幅です。", "%.1f");
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Path");
    ImGui::Text("Points: %d", static_cast<int>(editableNode.path.points.size()));
    ImGui::Text("Edges: %d", static_cast<int>(editableNode.path.edges.size()));
    ImGui::TextWrapped("Path ノードを選択した状態で 2D/3D ビューをクリックするとポイントを追加します。ポイントクリックで選択、W キーで移動ギズモ表示、中心ドラッグでカメラ平面移動、エッジクリックでポイント挿入、Del キーで選択ポイント削除、Enter キーで現在の連続線を確定します。");
    const rock::GraphId selectedPointId = SelectedPathPointId(editableNode.id);
    rock::PathPoint* selectedPoint = selectedPointId != 0 ? FindMutablePathPoint(editableNode.path, selectedPointId) : nullptr;
    ImGui::SeparatorText("Selected Point");
    if (selectedPoint == nullptr)
    {
        ImGui::TextDisabled("No point selected");
    }
    else
    {
        ImGui::Text("ID: %llu", static_cast<unsigned long long>(selectedPoint->id));
        if (ImGui::BeginTable("PathSelectedPointRows", 2, ImGuiTableFlags_SizingStretchProp))
        {
            changed |= DrawPathPointPositionRow(*selectedPoint);
            changed |= DrawPathPointCompactFloatRow("Width (m)", "PathSelectedPointWidth", &selectedPoint->widthMeters, 0.01f, 100000.0f, editableNode.path.defaultWidthMeters, "Mask Path width around this point.", "%.1f");
            changed |= DrawPathPointCompactFloatRow("Feather (m)", "PathSelectedPointFeather", &selectedPoint->featherMeters, 0.0f, 100000.0f, editableNode.path.defaultFeatherMeters, "Mask Path feather around this point.", "%.1f");
            changed |= DrawPathPointCompactFloatRow("Intensity", "PathSelectedPointIntensity", &selectedPoint->intensity, 0.0f, 1.0f, 1.0f, "Mask Path strength around this point.", "%.3f");
            changed |= DrawPathHeightOffsetRow(*selectedPoint);

            int heightMode = PathHeightModeToIndex(selectedPoint->heightMode);
            if (DrawPropertyComboRow("Height Mode", "PathSelectedPointHeightMode", &heightMode, PathHeightModeItems(), nullptr, 0))
            {
                selectedPoint->heightMode = PathHeightModeFromIndex(heightMode);
                changed = true;
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::Button("Clear Path"))
    {
        editableNode.path.points.clear();
        editableNode.path.edges.clear();
        changed = true;
    }
    if (changed)
    {
        MarkGraphChanged("Path changed");
        EvaluateGraph();
    }
    return changed;
}


static void ProcessDragSamples(rock::NodeGraph& graph, rock::GraphId nodeId, const std::vector<std::array<float, 3>>& samples)
{
    if (samples.empty()) return;

    rock::Node* node = graph.FindMutableNode(nodeId);
    if (node == nullptr || node->kind != rock::NodeKind::Colorize) return;

    struct DragSamplePoint
    {
        std::array<float, 3> color{};
        float position = 0.0f;
    };

    auto samplePosition = [&](size_t index) {
        return samples.size() <= 1 ? 0.0f : static_cast<float>(index) / static_cast<float>(samples.size() - 1);
    };

    // --- 間引き (Douglas-Peucker 的な閾値フィルタ) ---
    // 隣接するサンプル間の色差が閾値未満なら省略。最初と最後は必ず保持。
    const float colorThreshold = 0.04f; // ~10/255 相当
    std::vector<DragSamplePoint> thinned;
    thinned.push_back({samples.front(), 0.0f});
    for (size_t i = 1; i + 1 < samples.size(); ++i)
    {
        const auto& prev = thinned.back().color;
        const auto& cur  = samples[i];
        float dr = cur[0] - prev[0];
        float dg = cur[1] - prev[1];
        float db = cur[2] - prev[2];
        if (std::sqrt(dr*dr + dg*dg + db*db) >= colorThreshold)
        {
            thinned.push_back({cur, samplePosition(i)});
        }
    }
    thinned.push_back({samples.back(), 1.0f});

    // 最低 2 ストップを保証
    if (thinned.size() < 2)
    {
        thinned = {{samples.front(), 0.0f}, {samples.back(), 1.0f}};
    }

    constexpr size_t maxGradientStops = 32;
    if (thinned.size() > maxGradientStops)
    {
        std::vector<size_t> kept = {0, thinned.size() - 1};
        kept.reserve(maxGradientStops);
        auto colorError = [&](size_t left, size_t mid, size_t right) {
            const float span = static_cast<float>(right - left);
            const float t = span > 0.0f ? static_cast<float>(mid - left) / span : 0.0f;
            const auto& a = thinned[left].color;
            const auto& b = thinned[right].color;
            const auto& c = thinned[mid].color;
            const float er = c[0] - (a[0] + (b[0] - a[0]) * t);
            const float eg = c[1] - (a[1] + (b[1] - a[1]) * t);
            const float eb = c[2] - (a[2] + (b[2] - a[2]) * t);
            return er * er + eg * eg + eb * eb;
        };

        while (kept.size() < maxGradientStops)
        {
            std::sort(kept.begin(), kept.end());
            size_t bestIndex = 0;
            float bestError = 0.0f;
            for (size_t segment = 0; segment + 1 < kept.size(); ++segment)
            {
                const size_t left = kept[segment];
                const size_t right = kept[segment + 1];
                for (size_t i = left + 1; i < right; ++i)
                {
                    const float error = colorError(left, i, right);
                    if (error > bestError)
                    {
                        bestError = error;
                        bestIndex = i;
                    }
                }
            }
            if (bestIndex == 0)
            {
                break;
            }
            kept.push_back(bestIndex);
        }

        std::sort(kept.begin(), kept.end());
        std::vector<DragSamplePoint> capped;
        capped.reserve(kept.size());
        for (const size_t index : kept)
        {
            capped.push_back(thinned[index]);
        }
        thinned = std::move(capped);
    }

    // --- グラデーションストップとして投影 ---
    node->colorize.stops.clear();
    const int n = static_cast<int>(thinned.size());
    for (int i = 0; i < n; ++i)
    {
        rock::ColorStop stop;
        stop.position = std::clamp(thinned[i].position, 0.0f, 1.0f);
        stop.r = thinned[i].color[0];
        stop.g = thinned[i].color[1];
        stop.b = thinned[i].color[2];
        node->colorize.stops.push_back(stop);
    }

    MarkGraphChanged("Drag color sampled");
    EvaluateGraph();
    if (g_callbacks.requestForeground) { g_callbacks.requestForeground(); }
}


// スクリーンカラーピッカーのフレーム更新。毎フレーム ImGui::NewFrame 直後に呼ぶ。
// Ctrl を押しながらマウスを移動すると色を収集し、Ctrl を離した瞬間にグラデーションへ投影。
void UpdateColorizeScreenPick(rock::NodeGraph& graph)
{
    if (g_screenPick.mode == ScreenPickMode::Idle) return;

    if (g_callbacks.sampleScreenPixel) { g_callbacks.sampleScreenPixel(g_screenPick.previewR, g_screenPick.previewG, g_screenPick.previewB); }

    const bool ctrlDown    = IsCtrlDown();
    const bool ctrlRising  = ctrlDown  && !g_screenPick.prevCtrl;
    const bool ctrlFalling = !ctrlDown &&  g_screenPick.prevCtrl;
    g_screenPick.prevCtrl = ctrlDown;

    // Escape でどのモードからもキャンセル
    if (g_callbacks.isEscapeDown && g_callbacks.isEscapeDown())
    {
        g_screenPick.mode = ScreenPickMode::Idle;
        g_screenPick.dragSamples.clear();
        return;
    }

    switch (g_screenPick.mode)
    {
    case ScreenPickMode::DragArmed:
        // Ctrl 押下でサンプリング開始。初期位置は自アプリのグレー UI 上であることが多いため
        // 最初のサンプルは追加せず、移動後の色変化から収集を始める。
        if (ctrlRising)
        {
            g_screenPick.mode = ScreenPickMode::DragCollecting;
            g_screenPick.dragSamples.clear();
        }
        break;

    case ScreenPickMode::DragCollecting:
        if (ctrlDown)
        {
            // Ctrl 押し中: サンプルが空なら無条件追加、以降は色変化が一定以上のときだけ追加
            if (g_screenPick.dragSamples.empty())
            {
                g_screenPick.dragSamples.push_back({g_screenPick.previewR, g_screenPick.previewG, g_screenPick.previewB});
            }
            else
            {
                const auto& last = g_screenPick.dragSamples.back();
                float dr = g_screenPick.previewR - last[0];
                float dg = g_screenPick.previewG - last[1];
                float db = g_screenPick.previewB - last[2];
                if (std::sqrt(dr*dr + dg*dg + db*db) >= 0.008f)
                {
                    g_screenPick.dragSamples.push_back({g_screenPick.previewR, g_screenPick.previewG, g_screenPick.previewB});
                }
            }
        }
        else if (ctrlFalling)
        {
            // Ctrl 離し: 最終色を追加してサンプル列を間引きグラデーションへ投影
            g_screenPick.dragSamples.push_back({g_screenPick.previewR, g_screenPick.previewG, g_screenPick.previewB});
            ProcessDragSamples(graph, g_screenPick.nodeId, g_screenPick.dragSamples);
            g_screenPick.dragSamples.clear();
            g_screenPick.mode = ScreenPickMode::Idle;
        }
        break;

    default:
        break;
    }
}

} // namespace terrain::ui
