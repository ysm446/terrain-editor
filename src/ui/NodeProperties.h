#pragma once

#include <array>
#include <functional>
#include <vector>

#include "../node_graph.h"

namespace terrain::ui
{
enum class ScreenPickMode
{
    Idle,
    DragArmed,
    DragCollecting,
};

struct ScreenColorPick
{
    ScreenPickMode mode = ScreenPickMode::Idle;
    rock::GraphId nodeId = 0;
    float previewR = 1.0f;
    float previewG = 1.0f;
    float previewB = 1.0f;
    bool prevCtrl = false;
    std::vector<std::array<float, 3>> dragSamples;
};

struct NodePropertyCallbacks
{
    std::function<void()> evaluateGraph;
    std::function<void(const char*)> markGraphChanged;
    std::function<bool()> isCtrlDown;
};

void SetNodePropertyCallbacks(NodePropertyCallbacks callbacks);
ScreenColorPick& ColorizeScreenPick();

bool DrawHeightmapLoadProperties(rock::Node& editableNode);
bool DrawShapeProperties(rock::Node& editableNode);
bool DrawHeightmapBlurProperties(rock::Node& editableNode);
bool DrawMultiScaleErosionProperties(rock::Node& editableNode);
bool DrawMaskNoiseProperties(rock::Node& editableNode);
bool DrawMaskBlendProperties(rock::Node& editableNode);
bool DrawMaskLevelsProperties(rock::Node& editableNode);
bool DrawMaskHeightProperties(rock::Node& editableNode);
bool DrawMaskSlopeProperties(rock::Node& editableNode);
bool DrawMaskCurvatureProperties(rock::Node& editableNode);
bool DrawMaskFluvialProperties(rock::Node& editableNode);
bool DrawRockProperties(rock::Node& editableNode);
bool DrawScatterProperties(rock::Node& editableNode);
bool DrawCrumblingProperties(rock::Node& editableNode);
bool DrawSedimentProperties(rock::Node& editableNode);
bool DrawSnowProperties(rock::Node& editableNode);
bool DrawColorizeProperties(rock::Node& editableNode);
} // namespace terrain::ui
