#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "../node_graph.h"

namespace terrain::ui
{
struct DebugPanelRenderStats
{
    uint32_t drawCalls = 0;
    uint32_t indexedDrawCalls = 0;
    uint64_t submittedVertices = 0;
    uint64_t submittedTriangles = 0;
    uint64_t submittedLines = 0;
    uint32_t submittedPatches = 0;
    int renderTargetWidth = 0;
    int renderTargetHeight = 0;
    int displayMeshResolution = 0;
    bool gpuDisplacement = false;
    bool tessellation = false;
    float tessellationMaxFactor = 1.0f;
    bool surfacePass = false;
    bool wireframePass = false;
    bool gridPass = false;
    bool shadowPass = false;
    bool skyPass = false;
    bool cloudsPass = false;
    uint64_t displayedVertices = 0;
    uint64_t displayedTriangles = 0;
};

struct DebugPanelState
{
    rock::GraphSettings& settings;
    const rock::EvaluationSummary& evaluation;
    const std::string& lastEvaluationDuration;
    bool& showDrawStats;
    DebugPanelRenderStats renderStats;
    std::function<void()> saveAppSettings;
};

void DrawDebugPanel(DebugPanelState state);
} // namespace terrain::ui
