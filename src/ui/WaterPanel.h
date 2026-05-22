#pragma once

#include <functional>

#include "../node_graph.h"

namespace terrain::ui
{
struct WaterPanelState
{
    rock::GraphSettings& settings;
    std::function<void()> saveAppSettings;
};

void DrawWaterSettingsPanel(WaterPanelState state);
} // namespace terrain::ui
