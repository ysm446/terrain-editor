#pragma once

#include <functional>

#include "../node_graph.h"

namespace terrain::ui
{
struct DisplayPanelState
{
    rock::GraphSettings& settings;
    bool& meshPreview;
    float& orbitDistance;
    std::function<float()> defaultViewportOrbitDistance;
    std::function<void()> evaluateGraph;
    std::function<void(const char*)> markGraphChanged;
    std::function<void()> saveAppSettings;
};

void DrawDisplaySettingsPanel(DisplayPanelState state);
} // namespace terrain::ui
