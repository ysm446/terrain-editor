#pragma once

#include <functional>

#include "../node_graph.h"

namespace terrain::ui
{
struct CameraPanelViewport
{
    float& yaw;
    float& pitch;
    float& fovDegrees;
    float& orbitDistance;
    bool& autoOrbitEnabled;
    float& autoOrbitSpeedDegreesPerSecond;
};

struct CameraPanelDefaults
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovDegrees = 45.0f;
    float orbitDistance = 1.0f;
    float maxOrbitDistance = 100000.0f;
};

struct CameraPanelState
{
    CameraPanelViewport viewport;
    rock::PreviewSettings& preview;
    int gridCellCount = 0;
    float gridCellSizeMeters = 0.0f;
    CameraPanelDefaults defaults;
    std::function<void()> resetViewport;
    bool focusPickActive = false;
    std::function<void()> requestFocusPick;
    std::function<void(const char*)> markGraphChanged;
    std::function<void()> saveAppSettings;
};

void DrawCameraPanel(CameraPanelState state);
} // namespace terrain::ui
