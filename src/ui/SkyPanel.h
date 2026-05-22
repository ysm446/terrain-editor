#pragma once

#include <functional>

#include "../node_graph.h"

namespace terrain::ui
{
struct SkyPanelSunPosition
{
    float azimuth = 0.0f;
    float elevation = 0.0f;
};

struct SkyPanelState
{
    rock::GraphSettings& settings;
    std::function<void()> saveAppSettings;
};

void DrawSkySettingsPanel(SkyPanelState state);
void DrawCloudSettingsPanel(SkyPanelState state);
} // namespace terrain::ui
