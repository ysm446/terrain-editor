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

struct DebugPanelFrameTiming
{
    float frameMs = 0.0f;
    float messagePumpMs = 0.0f;
    float newFrameMs = 0.0f;
    float mainThreadWorkMs = 0.0f;
    float drawUiMs = 0.0f;
    float viewportTabsMs = 0.0f;
    float nodeEditorMs = 0.0f;
    float nodeEditorDotsMs = 0.0f;
    float nodeEditorShadowsMs = 0.0f;
    float nodeEditorNodesMs = 0.0f;
    float nodeEditorLinksMs = 0.0f;
    float nodeEditorInteractionMs = 0.0f;
    float nodeEditorPositionMs = 0.0f;
    float inspectorMs = 0.0f;
    float statusBarMs = 0.0f;
    float gpuPreviewMs = 0.0f;
    float imguiRenderMs = 0.0f;
    float renderFrameMs = 0.0f;
    float presentMs = 0.0f;
    float frameLimitSleepMs = 0.0f;
    float backgroundSleepMs = 0.0f;
    float fenceWaitMs = 0.0f;
    int frameRateLimitFps = 0;
    bool windowActive = true;
    bool windowForeground = true;
    bool windowMinimized = false;
    bool backgroundThrottled = false;
    std::string gpuPreviewReason;
    int nodeCount = 0;
    int linkCount = 0;
};

struct DebugPanelState
{
    rock::GraphSettings& settings;
    const rock::EvaluationSummary& evaluation;
    const std::string& lastEvaluationDuration;
    bool& showDrawStats;
    bool& showFrameStats;
    DebugPanelRenderStats renderStats;
    DebugPanelFrameTiming frameTiming;
    std::function<void()> saveAppSettings;
};

void DrawDebugPanel(DebugPanelState state);
} // namespace terrain::ui
