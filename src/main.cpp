#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <nlohmann/json.hpp>

#include "D3D12Utils.h"
#include "gpu/ColorizeCompute.h"
#include "gpu/MaskFluvialCompute.h"
#include "gpu/FluvialErosionCompute.h"
#include "gpu/DropletErosionCompute.h"
#include "gpu/MaskNoiseCompute.h"
#include "gpu/MaskUtilityCompute.h"
#include "gpu/MseCompute.h"
#include "gpu/RockCompute.h"
#include "gpu/ScatterCompute.h"
#include "gpu/SedimentCompute.h"
#include "gpu/SnowCompute.h"
#include "node_graph.h"
#include "NodeSerialization.h"
#include "PathUtils.h"
#include "ProjectSettingsSerialization.h"
#include "rendering/MeshPreviewRenderer.h"
#include "platform/FileDialogs.h"
#include "platform/ShellUtils.h"
#include "resource.h"
#include "screenshot_capture.h"
#include "texture_exporter.h"
#include "ui/AppFonts.h"
#include "ui/AssetExportPanel.h"
#include "ui/CameraPanel.h"
#include "ui/DebugPanel.h"
#include "ui/DisplayPanel.h"
#include "ui/Localization.h"
#include "ui/NodeIcon.h"
#include "ui/NodePins.h"
#include "ui/NodeProperties.h"
#include "ViewportMath.h"
#include "ViewportSerialization.h"
#include "ui/PropertyWidgets.h"
#include "ui/SkyPanel.h"
#include "ui/UiTheme.h"
#include "ui/WaterPanel.h"
#include "Version.h"

using Microsoft::WRL::ComPtr;
namespace ed = ax::NodeEditor;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
using namespace terrain::ui;
using terrain::PathFromUtf8;
using terrain::PathToUtf8;
using terrain::CameraBasis;
using terrain::ProjectedPoint;
using terrain::Vec3;
using terrain::d3d12::BufferResourceDesc;
using terrain::d3d12::CreateRootSignatureFromDesc;
using terrain::d3d12::DefaultShaderCompileFlags;
using terrain::d3d12::DescriptorHeapDesc;
using terrain::d3d12::HeapProperties;
using terrain::d3d12::ShaderVisibleCbvSrvUavDescriptorHeapDesc;
using terrain::d3d12::Texture2DResourceDesc;
using terrain::d3d12::ThrowIfFailed;
using terrain::gpu::ColorizeComputeStatus;
using terrain::gpu::ProcessPendingColorizeGpuRequests;
using terrain::gpu::RunColorizeCompute;
using terrain::gpu::MaskFluvialComputeStatus;
using terrain::gpu::ProcessPendingMaskFluvialGpuRequests;
using terrain::gpu::RunMaskFluvialCompute;
using terrain::gpu::ProcessPendingFluvialErosionGpuRequests;
using terrain::gpu::RunFluvialErosionCompute;
using terrain::gpu::ProcessPendingDropletErosionGpuRequests;
using terrain::gpu::RunDropletErosionCompute;
using terrain::gpu::MaskNoiseComputeStatus;
using terrain::gpu::ProcessPendingMaskNoiseGpuRequests;
using terrain::gpu::RunMaskNoiseCompute;
using terrain::gpu::MaskUtilityComputeStatus;
using terrain::gpu::ProcessPendingMaskUtilityGpuRequests;
using terrain::gpu::RunHeightmapFromMaskCompute;
using terrain::gpu::RunMaskBlurCompute;
using terrain::gpu::RunMaskPathCompute;
using terrain::gpu::MseComputeStatus;
using terrain::gpu::ProcessPendingMseGpuRequests;
using terrain::gpu::RunMseComputeGrid;
using terrain::gpu::ProcessPendingSedimentGpuRequests;
using terrain::gpu::RunSedimentCompute;
using terrain::gpu::SedimentComputeStatus;
using terrain::gpu::ProcessPendingRockGpuRequests;
using terrain::gpu::RockComputeStatus;
using terrain::gpu::RunRockCompute;
using terrain::gpu::ProcessPendingScatterGpuRequests;
using terrain::gpu::RunScatterCompute;
using terrain::gpu::ScatterComputeStatus;
using terrain::gpu::ProcessPendingSnowGpuRequests;
using terrain::gpu::RunSnowCompute;
using terrain::gpu::RunSoilCompute;
using terrain::gpu::SnowComputeStatus;
using terrain::rendering::GpuMeshPreview;
using terrain::rendering::EnsureMeshPreviewDisplacementPipeline;
using terrain::rendering::EnsureMeshPreviewPipeline;
using terrain::rendering::MeshPreviewPipelineContext;
using terrain::rendering::MeshPreviewPipelineResources;
using terrain::rendering::PreviewRenderStats;
using terrain::rendering::ResetMeshPreviewPipelineResources;

constexpr int kFrameCount = 2;
constexpr int kSrvDescriptorCount = 128;
constexpr float kDegreesToRadians = 3.1415926535f / 180.0f;
constexpr float kFullFrameSensorHeightMm = 24.0f;
constexpr float kDefaultViewportYaw = -30.0f * kDegreesToRadians;
constexpr float kDefaultViewportPitch = 30.0f * kDegreesToRadians;
constexpr float kDefaultViewportFovDegrees = 45.0f;
constexpr float kDefaultViewportOrbitDistance = 2044.0f;
constexpr float kMaxViewportOrbitDistance = 100000.0f;
constexpr float kViewportFarPlane = 200000.0f;
constexpr std::array<int, 4> kTerrainSizePresets = {512, 1024, 2048, 4096};
constexpr std::array<int, 6> kResolutionPresets = {128, 256, 512, 1024, 2048, 4096};
constexpr std::array<int, 4> kShadowResolutionPresets = {512, 1024, 2048, 4096};

template <size_t N>
int NearestPreset(int value, const std::array<int, N>& presets, int fallback)
{
    const auto nearest = std::ranges::min_element(presets, [value](int lhs, int rhs) {
        return std::abs(lhs - value) < std::abs(rhs - value);
    });
    return nearest != presets.end() ? *nearest : fallback;
}

int NearestResolutionPreset(int value)
{
    return NearestPreset(value, kResolutionPresets, 512);
}

int NearestTerrainSizePreset(float value)
{
    return NearestPreset(static_cast<int>(std::round(value)), kTerrainSizePresets, 1024);
}

int NearestShadowResolutionPreset(int value)
{
    return NearestPreset(value, kShadowResolutionPresets, 1024);
}

int ClampFrameRateLimitFps(int value)
{
    switch (value)
    {
    case 30:
    case 60:
        return value;
    default:
        return 0;
    }
}

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    UINT64 fenceValue = 0;
};

struct NodeEditorContextScope
{
    explicit NodeEditorContextScope(ed::EditorContext* editor)
        : active(editor != nullptr)
    {
        if (active)
        {
            ed::SetCurrentEditor(editor);
        }
    }

    ~NodeEditorContextScope()
    {
        if (active)
        {
            ed::SetCurrentEditor(nullptr);
        }
    }

    NodeEditorContextScope(const NodeEditorContextScope&) = delete;
    NodeEditorContextScope& operator=(const NodeEditorContextScope&) = delete;

private:
    bool active = false;
};

constexpr UINT kDefaultWindowClientWidth = 1600;
constexpr UINT kDefaultWindowClientHeight = 900;
constexpr float kDefaultDebugLogHeight = 180.0f;

HWND g_hwnd = nullptr;
UINT g_width = kDefaultWindowClientWidth;
UINT g_height = kDefaultWindowClientHeight;
bool g_running = true;
bool g_windowActive = true;
bool g_windowMinimized = false;

ScreenColorPick& g_screenPick = ColorizeScreenPick();

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<IDXGISwapChain3> g_swapChain;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
ComPtr<ID3D12GraphicsCommandList> g_commandList;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceLastSignaledValue = 0;
UINT g_frameIndex = 0;
UINT g_rtvDescriptorSize = 0;
UINT g_srvDescriptorSize = 0;
std::array<FrameContext, kFrameCount> g_frameContexts;
std::array<ComPtr<ID3D12Resource>, kFrameCount> g_renderTargets;
std::array<bool, kSrvDescriptorCount> g_srvDescriptorUsed{};
bool g_lowDedicatedMemoryAdapter = false;
SIZE_T g_adapterDedicatedVideoMemory = 0;
SIZE_T g_adapterSharedSystemMemory = 0;
std::wstring g_adapterDescription;
ed::EditorContext* g_nodeEditor = nullptr;
bool g_nodeEditorFrameActive = false;
bool g_skipNodeMoveUndoThisFrame = false;
bool g_nodePositionsInitialized = false;
bool g_nodeGraphNavigatedToContent = false;
bool g_layoutSplitterActive = false;
ImGuiID g_activeLayoutSplitterId = 0;
rock::NodeGraph g_graph = rock::NodeGraph::CreateDefaultTerrainGraph();
std::string g_exportStatus;
std::string g_projectStatus = "No project file";
std::chrono::steady_clock::time_point g_projectStatusExpiresAt{};
std::string g_lastEvaluationDuration = "Eval --";
bool g_projectSettingsHadSimulationResolution = false;
std::filesystem::path g_projectPath;
std::optional<std::filesystem::path> g_projectSavePathForSerialization;
std::wstring g_windowTitle;
bool g_projectDirty = false;
std::vector<std::filesystem::path> g_recentProjectPaths;
std::vector<std::pair<rock::GraphId, ImVec2>> g_pendingNodePositions;
std::vector<std::pair<rock::GraphId, ImVec2>> g_nodePositionCache;
std::vector<rock::GraphId> g_pendingSelectedNodeIds;
std::optional<std::vector<rock::GraphId>> g_pendingPreviewSelectionRestore;
rock::UiThemeManager g_themeManager;
rock::GraphId g_selectedNodeId = 0;
rock::GraphId g_pendingPreviewPinId = 0;
bool g_focusPickMode = false;
bool g_focusPickCursorActive = false;
bool g_sunDirectionDragActive = false;
double g_sunDirectionGizmoVisibleUntil = 0.0;

void SetProjectStatus(std::string status)
{
    g_projectStatus = std::move(status);
    g_projectStatusExpiresAt = {};
}

void SetTransientProjectStatus(std::string status, std::chrono::seconds duration = std::chrono::seconds(4))
{
    g_projectStatus = std::move(status);
    g_projectStatusExpiresAt = std::chrono::steady_clock::now() + duration;
}

void RefreshProjectStatus()
{
    if (g_projectStatusExpiresAt != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() >= g_projectStatusExpiresAt)
    {
        SetProjectStatus("Ready");
    }
}

struct AsyncEvaluationResult
{
    uint64_t requestId = 0;
    rock::NodeGraph graph;
    std::string duration;
};

std::future<AsyncEvaluationResult> g_evaluationFuture;
bool g_evaluationInFlight = false;
bool g_evaluationPending = false;
uint64_t g_nextEvaluationRequestId = 0;
uint64_t g_activeEvaluationRequestId = 0;

std::array<float, 3> EstimateSectionColor(const rock::ColorGrid& colorGrid, const std::array<float, 3>& fallbackAlbedo)
{
    const int n = colorGrid.resolution;
    const size_t expectedPixels = static_cast<size_t>(n) * static_cast<size_t>(n) * 4u;
    if (n < 2 || colorGrid.pixels.size() < expectedPixels)
    {
        return {
            std::clamp(fallbackAlbedo[0] * 0.45f, 0.0f, 1.0f),
            std::clamp(fallbackAlbedo[1] * 0.45f, 0.0f, 1.0f),
            std::clamp(fallbackAlbedo[2] * 0.45f, 0.0f, 1.0f),
        };
    }

    constexpr int kSampleGrid = 32;
    double sumR = 0.0;
    double sumG = 0.0;
    double sumB = 0.0;
    int sampleCount = 0;
    for (int sy = 0; sy < kSampleGrid; ++sy)
    {
        const int y = (kSampleGrid <= 1)
            ? 0
            : std::clamp(static_cast<int>(std::round(static_cast<float>(sy) * static_cast<float>(n - 1) / static_cast<float>(kSampleGrid - 1))), 0, n - 1);
        for (int sx = 0; sx < kSampleGrid; ++sx)
        {
            const int x = (kSampleGrid <= 1)
                ? 0
                : std::clamp(static_cast<int>(std::round(static_cast<float>(sx) * static_cast<float>(n - 1) / static_cast<float>(kSampleGrid - 1))), 0, n - 1);
            const size_t src = (static_cast<size_t>(y) * static_cast<size_t>(n) + static_cast<size_t>(x)) * 4u;
            sumR += static_cast<double>(colorGrid.pixels[src + 0u]) / 255.0;
            sumG += static_cast<double>(colorGrid.pixels[src + 1u]) / 255.0;
            sumB += static_cast<double>(colorGrid.pixels[src + 2u]) / 255.0;
            ++sampleCount;
        }
    }

    const float scale = sampleCount > 0 ? 0.52f / static_cast<float>(sampleCount) : 0.45f;
    return {
        std::clamp(static_cast<float>(sumR) * scale, 0.04f, 0.85f),
        std::clamp(static_cast<float>(sumG) * scale, 0.04f, 0.85f),
        std::clamp(static_cast<float>(sumB) * scale, 0.04f, 0.85f),
    };
}

struct ClipboardNode
{
    rock::Node node;
    ImVec2 position;
};

struct NodeClipboard
{
    std::vector<ClipboardNode> nodes;
    std::vector<rock::Link> links;
};

NodeClipboard g_nodeClipboard;

struct GraphEditSnapshot
{
    std::vector<rock::Node> nodes;
    std::vector<rock::Link> links;
    std::vector<std::pair<rock::GraphId, ImVec2>> nodePositions;
    std::vector<rock::GraphId> selectedNodeIds;
    rock::GraphId selectedNodeId = 0;
    rock::GraphId previewNodeId = 0;
    rock::GraphId previewPinId = 0;
    rock::PreviewStage previewStage = rock::PreviewStage::Graph;
};

std::vector<GraphEditSnapshot> g_undoStack;
std::vector<GraphEditSnapshot> g_redoStack;
std::optional<GraphEditSnapshot> g_pendingPropertyEditUndo;
std::optional<GraphEditSnapshot> g_pendingNodeMoveUndo;

struct UiState
{
    bool meshPreview = true;
    bool showFps = true;
    int frameRateLimitFps = 0;
    bool newNodeBackendGpu = true;
    bool showDrawStats = false;
    bool showFrameStats = false;
    bool debugLogVisible = false;
    float rightPaneWidth = 0.0f;
    float nodePaneHeight = 0.0f;
    float debugLogHeight = kDefaultDebugLogHeight;
};

UiState g_ui;

struct DebugLogEntry
{
    std::string time;
    std::string message;
};

std::vector<DebugLogEntry> g_debugLogEntries;
bool g_debugLogAutoScroll = true;

struct FrameTimingStats
{
    double frameMs = 0.0;
    double messagePumpMs = 0.0;
    double newFrameMs = 0.0;
    double mainThreadWorkMs = 0.0;
    double drawUiMs = 0.0;
    double viewportTabsMs = 0.0;
    double nodeEditorMs = 0.0;
    double nodeEditorDotsMs = 0.0;
    double nodeEditorShadowsMs = 0.0;
    double nodeEditorNodesMs = 0.0;
    double nodeEditorLinksMs = 0.0;
    double nodeEditorInteractionMs = 0.0;
    double nodeEditorPositionMs = 0.0;
    double inspectorMs = 0.0;
    double statusBarMs = 0.0;
    double gpuPreviewMs = 0.0;
    double imguiRenderMs = 0.0;
    double renderFrameMs = 0.0;
    double presentMs = 0.0;
    double frameLimitSleepMs = 0.0;
    double backgroundSleepMs = 0.0;
    double fenceWaitMs = 0.0;
    int frameRateLimitFps = 0;
    bool windowActive = true;
    bool windowForeground = true;
    bool windowMinimized = false;
    bool backgroundThrottled = false;
    std::string gpuPreviewReason;
    int nodeCount = 0;
    int linkCount = 0;
};

FrameTimingStats g_frameTiming;
FrameTimingStats g_lastFrameTiming;

enum class ViewportDisplayMode
{
    Simple,
    Pbr,
    Sky,
};

struct ViewportState
{
    float yaw = kDefaultViewportYaw;
    float pitch = kDefaultViewportPitch;
    float fovDegrees = kDefaultViewportFovDegrees;
    float orbitDistance = kDefaultViewportOrbitDistance;
    ImVec2 pan = ImVec2(0.0f, 0.0f);
    bool autoOrbitEnabled = false;
    float autoOrbitSpeedDegreesPerSecond = 15.0f;
};

ViewportState g_viewport;

void NormalizeLoadedViewport(bool migrateCloseOrbitDistance)
{
    terrain::ViewportCameraState viewport{
        g_viewport.yaw,
        g_viewport.pitch,
        g_viewport.fovDegrees,
        g_viewport.orbitDistance,
        g_viewport.pan,
    };
    terrain::NormalizeViewportCameraState(viewport, migrateCloseOrbitDistance, kDefaultViewportPitch, kDefaultViewportOrbitDistance);
    g_viewport.yaw = viewport.yaw;
    g_viewport.pitch = viewport.pitch;
    g_viewport.fovDegrees = viewport.fovDegrees;
    g_viewport.orbitDistance = viewport.orbitDistance;
    g_viewport.pan = viewport.pan;
}

int DaysInMonth(int month)
{
    static constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int clampedMonth = std::clamp(month, 1, 12);
    return kDays[static_cast<size_t>(clampedMonth - 1)];
}

int DayOfYear(int month, int day)
{
    int total = 0;
    const int clampedMonth = std::clamp(month, 1, 12);
    for (int m = 1; m < clampedMonth; ++m)
    {
        total += DaysInMonth(m);
    }
    return total + std::clamp(day, 1, DaysInMonth(clampedMonth));
}

float NormalizeDegrees(float degrees)
{
    float normalized = std::fmod(degrees, 360.0f);
    if (normalized < 0.0f)
    {
        normalized += 360.0f;
    }
    return normalized;
}

struct SunPositionDegrees
{
    float azimuth = 0.0f;
    float elevation = 0.0f;
};

SunPositionDegrees ComputeDateTimeSunPosition(const rock::PreviewSettings& preview)
{
    const float latitude = std::clamp(preview.sunLatitudeDegrees, -90.0f, 90.0f) * kDegreesToRadians;
    const float longitude = std::clamp(preview.sunLongitudeDegrees, -180.0f, 180.0f);
    const float utcOffset = std::clamp(preview.sunUtcOffsetHours, -12.0f, 14.0f);
    const int dayOfYear = DayOfYear(preview.sunMonth, preview.sunDay);
    const float localHours = std::clamp(preview.sunTimeHours, 0.0f, 24.0f);
    const float fractionalYear = (2.0f * 3.1415926535f / 365.0f) *
        (static_cast<float>(dayOfYear - 1) + (localHours - 12.0f) / 24.0f);

    const float equationOfTime = 229.18f * (
        0.000075f +
        0.001868f * std::cos(fractionalYear) -
        0.032077f * std::sin(fractionalYear) -
        0.014615f * std::cos(2.0f * fractionalYear) -
        0.040849f * std::sin(2.0f * fractionalYear));
    const float declination =
        0.006918f -
        0.399912f * std::cos(fractionalYear) +
        0.070257f * std::sin(fractionalYear) -
        0.006758f * std::cos(2.0f * fractionalYear) +
        0.000907f * std::sin(2.0f * fractionalYear) -
        0.002697f * std::cos(3.0f * fractionalYear) +
        0.00148f * std::sin(3.0f * fractionalYear);

    float trueSolarMinutes = localHours * 60.0f + equationOfTime + 4.0f * longitude - 60.0f * utcOffset;
    trueSolarMinutes = std::fmod(trueSolarMinutes, 1440.0f);
    if (trueSolarMinutes < 0.0f)
    {
        trueSolarMinutes += 1440.0f;
    }

    float hourAngleDegrees = trueSolarMinutes / 4.0f - 180.0f;
    if (hourAngleDegrees < -180.0f)
    {
        hourAngleDegrees += 360.0f;
    }
    const float hourAngle = hourAngleDegrees * kDegreesToRadians;

    const float cosZenith = std::clamp(
        std::sin(latitude) * std::sin(declination) +
        std::cos(latitude) * std::cos(declination) * std::cos(hourAngle),
        -1.0f,
        1.0f);
    const float zenith = std::acos(cosZenith);
    const float elevation = 90.0f - zenith / kDegreesToRadians;

    const float northClockwiseAzimuth = NormalizeDegrees(
        std::atan2(
            std::sin(hourAngle),
            std::cos(hourAngle) * std::sin(latitude) - std::tan(declination) * std::cos(latitude)) /
        kDegreesToRadians + 180.0f);

    return {
        NormalizeDegrees(180.0f - northClockwiseAzimuth),
        std::clamp(elevation, -90.0f, 90.0f),
    };
}

SunPositionDegrees EffectiveSunPosition(const rock::PreviewSettings& preview)
{
    if (preview.sunDirectionMode == rock::SunDirectionMode::DateTime)
    {
        return ComputeDateTimeSunPosition(preview);
    }
    return {
        NormalizeDegrees(preview.sunAzimuthDegrees),
        std::clamp(preview.sunElevationDegrees, -90.0f, 90.0f),
    };
}

struct MapViewportState
{
    float zoom = 1.0f;
    ImVec2 pan = ImVec2(0.0f, 0.0f);
};

MapViewportState g_mapViewport;

std::optional<Vec3> g_focusPickHoverPoint;

struct MeshPreviewConstants
{
    float cameraPosition[4];
    float cameraRight[4];
    float cameraUp[4];
    float cameraForward[4];
    float projScaleX;
    float projScaleY;
    float panNdcX;
    float panNdcY;
    float nearPlane;
    float farPlane;
    float maskPreview;
    float lightingMode;
    float sunDirection[4];
    float albedoColor[4];
    float sunIntensity;
    float ambientStrength;
    float shadowStrength;
    float shadowMapResolution;
    float shadowBias;
    float shadowEnabled;
    float maskShadingMode;  // 0 = Grayscale, 1 = GrayOrange, 2 = GrayscaleHatched
    float colorTextureMode; // bit 0 = sample Colorize texture, bit 1 = sample mask texture in PS
    float lightRight[4];
    float lightUp[4];
    float lightForward[4];
    float lightCenter[4];
    float lightWorldRadius;
    float lightNearPlane;
    float lightFarPlane;
    float lightDepthMin;  // シャドウ深度レンジの最小値 (LightSpace01 で使用)
};

static_assert(offsetof(MeshPreviewConstants, lightRight) == 160);
static_assert(offsetof(MeshPreviewConstants, lightUp) == 176);
static_assert(offsetof(MeshPreviewConstants, lightForward) == 192);
static_assert(offsetof(MeshPreviewConstants, lightCenter) == 208);
static_assert(offsetof(MeshPreviewConstants, lightWorldRadius) == 224);
static_assert(offsetof(MeshPreviewConstants, lightDepthMin) == 236);
static_assert(sizeof(MeshPreviewConstants) == 240);

// Cloud shadow + sky environment data lives in its own cbuffer (b1) bound
// via a root CBV so the mesh root signature stays under the 64-DWORD limit.
// The sky colours drive the terrain's hemisphere ambient term so the scene
// stays visually coherent with the procedural sky.
struct CloudShadowMeshConstants
{
    float cloudShadowEnabled;
    float cloudShadowStrength;
    float cloudShadowAltitudeMin;
    float aoEnabled;  // 0 = off, 1 = on (PS のみ参照)
    float cloudShadowMinX;
    float cloudShadowMinZ;
    float cloudShadowSizeX;
    float cloudShadowSizeZ;
    float skyZenithColor[4];
    float skyHorizonColor[4];
    float skyGroundColor[4];
    float skySunColor[4];
    float sectionColor[4];
    float atmosphereDensity;
    float atmosphereMieStrength;
    float aoStrength;  // AO の暗化強度 (0–1)
    float pad1;
    float waterLevelParam;           // PSWater: 水面高さ (m)
    float waterWavesScale;           // PSWater: 主波長 (m)
    float waterRefractiveIndex;      // PSWater: 屈折率 → Schlick F0
    float waterFresnelPower;         // PSWater: フレネル反射の立ち上がり
    float waterRefractionStrength;
    float waterTimeSeconds;          // PSWater: アプリ起動からの経過秒数 (波アニメーション用)
    float waterAnimEnabled;          // PSWater: アニメーション有効 (0=静止, 1=アニメ)
    float waterReflectionStrength;   // PSWater: 反射強度スケール
    float waterSsrEnabled;           // PSWater: SSR 有効 (0=off, 1=on)
    float pad2[3];
};
static_assert(sizeof(CloudShadowMeshConstants) == 176);

MeshPreviewPipelineResources g_meshPreviewPipelines;
ComPtr<ID3D12RootSignature> g_aoComputeRootSignature;
ComPtr<ID3D12PipelineState> g_aoComputePso;
bool g_aoComputeReady = false;
ComPtr<ID3D12RootSignature> g_skyRootSignature;
ComPtr<ID3D12PipelineState> g_skyPso;
bool g_skyPipelineReady = false;
std::string g_skyPipelineStatus = "Sky pipeline not initialized";
DXGI_FORMAT g_skyPipelineFormat = DXGI_FORMAT_UNKNOWN;
ComPtr<ID3D12RootSignature> g_atmosphereMultiScatterRootSignature;
ComPtr<ID3D12PipelineState> g_atmosphereMultiScatterPso;
ComPtr<ID3D12Resource> g_atmosphereMultiScatterTexture;
D3D12_RESOURCE_STATES g_atmosphereMultiScatterState = D3D12_RESOURCE_STATE_COMMON;
D3D12_CPU_DESCRIPTOR_HANDLE g_atmosphereMultiScatterSrvCpu{};
D3D12_GPU_DESCRIPTOR_HANDLE g_atmosphereMultiScatterSrvGpu{};
bool g_atmosphereMultiScatterSrvAllocated = false;
bool g_atmosphereMultiScatterReady = false;
float g_atmosphereCachedDensity = std::numeric_limits<float>::quiet_NaN();
float g_atmosphereCachedMie = std::numeric_limits<float>::quiet_NaN();
float g_atmosphereCachedMieG = std::numeric_limits<float>::quiet_NaN();
ComPtr<ID3D12RootSignature> g_cloudVolumeRootSignature;
ComPtr<ID3D12PipelineState> g_cloudVolumePso;
ComPtr<ID3D12RootSignature> g_cloudRenderRootSignature;
ComPtr<ID3D12PipelineState> g_cloudRenderPso;
ComPtr<ID3D12RootSignature> g_cloudShadowRootSignature;
ComPtr<ID3D12PipelineState> g_cloudShadowPso;
bool g_cloudPipelinesReady = false;
std::string g_cloudPipelineStatus = "Cloud pipelines not initialized";
ComPtr<ID3D12RootSignature> g_dofRootSignature;
ComPtr<ID3D12PipelineState> g_dofPso;
bool g_dofPipelineReady = false;
std::string g_dofPipelineStatus = "Depth of Field pipeline not initialized";
DXGI_FORMAT g_dofPipelineFormat = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT g_cloudRenderPipelineFormat = DXGI_FORMAT_UNKNOWN;
ComPtr<ID3D12RootSignature> g_tonemapRootSignature;
ComPtr<ID3D12PipelineState> g_exposurePso;
ComPtr<ID3D12PipelineState> g_tonemapPso;
ComPtr<ID3D12PipelineState> g_colorGradePso;
bool g_tonemapPipelineReady = false;
std::string g_tonemapPipelineStatus = "Tonemap pipeline not initialized";

struct GpuClouds
{
    ComPtr<ID3D12Resource> volumeTexture;       // 128^3 R8_UNORM density volume
    D3D12_RESOURCE_STATES volumeState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE volumeSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE volumeSrvGpu{};
    bool volumeSrvAllocated = false;
    bool volumeReady = false;
    int cachedSeed = INT_MIN;
    ComPtr<ID3D12Resource> shadowTexture;       // 1024x1024 R8_UNORM transmittance map
    D3D12_RESOURCE_STATES shadowState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    bool shadowSrvAllocated = false;
    int shadowResolution = 0;
    ComPtr<ID3D12Resource> meshCbUploadBuffer;  // 256-byte CBV with CloudShadowMeshConstants
    void* meshCbMapped = nullptr;
    ComPtr<ID3D12Resource> dummyShadowTexture;  // 1x1 white R8 used when clouds are off
    D3D12_CPU_DESCRIPTOR_HANDLE dummyShadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE dummyShadowSrvGpu{};
    bool dummyShadowAllocated = false;
};
GpuClouds g_gpuClouds;
GpuMeshPreview g_gpuMeshPreview;
std::thread::id g_mainThreadId;

std::string MakeWindowTitleText()
{
    std::string title = "Terrain Editor v" + std::string(TERRAIN_EDITOR_VERSION_STRING) + " ";
    title += g_projectPath.empty() ? "Untitled" : g_projectPath.filename().string();
    if (g_projectDirty)
    {
        title += "*";
    }
    return title;
}

std::wstring MakeWindowTitle()
{
    std::wstring title = L"Terrain Editor v" +
        std::to_wstring(TERRAIN_EDITOR_VERSION_MAJOR) + L"." +
        std::to_wstring(TERRAIN_EDITOR_VERSION_MINOR) + L"." +
        std::to_wstring(TERRAIN_EDITOR_VERSION_PATCH) + L" ";
    title += g_projectPath.empty() ? L"Untitled" : g_projectPath.filename().wstring();
    if (g_projectDirty)
    {
        title += L"*";
    }
    return title;
}

void UpdateWindowTitle()
{
    if (g_hwnd != nullptr)
    {
        g_windowTitle = MakeWindowTitle();
        // nvspcap64.dll (NVIDIA Shadowplay) hooks SetWindowTextA/W and truncates to
        // first character. Call DefWindowProcW directly to bypass the IAT hook.
        DefWindowProcW(g_hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(g_windowTitle.c_str()));
    }
}

void SetProjectDirty(bool dirty)
{
    if (g_projectDirty == dirty)
    {
        return;
    }

    g_projectDirty = dirty;
    UpdateWindowTitle();
}

void MarkProjectDirty()
{
    SetProjectDirty(true);
}

void MarkGraphChanged(std::string_view reason)
{
    g_graph.MarkDirty(reason);
    MarkProjectDirty();
}

std::wstring ModuleDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (size == 0)
    {
        return L".";
    }
    std::filesystem::path modulePath(path);
    return modulePath.parent_path().wstring();
}

void AllocateSrvDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    for (int i = 0; i < kSrvDescriptorCount; ++i)
    {
        if (!g_srvDescriptorUsed[i])
        {
            g_srvDescriptorUsed[i] = true;
            *outCpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            *outGpuHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            outCpuHandle->ptr += static_cast<SIZE_T>(i) * g_srvDescriptorSize;
            outGpuHandle->ptr += static_cast<UINT64>(i) * g_srvDescriptorSize;
            return;
        }
    }

    throw std::runtime_error("No free ImGui SRV descriptors");
}

void AllocateSrvDescriptorRange(int count, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    if (count <= 0 || count > kSrvDescriptorCount)
    {
        throw std::runtime_error("Invalid SRV descriptor range size");
    }
    for (int i = 0; i <= kSrvDescriptorCount - count; ++i)
    {
        bool available = true;
        for (int j = 0; j < count; ++j)
        {
            if (g_srvDescriptorUsed[i + j])
            {
                available = false;
                break;
            }
        }
        if (!available)
        {
            continue;
        }

        for (int j = 0; j < count; ++j)
        {
            g_srvDescriptorUsed[i + j] = true;
        }
        *outCpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
        *outGpuHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
        outCpuHandle->ptr += static_cast<SIZE_T>(i) * g_srvDescriptorSize;
        outGpuHandle->ptr += static_cast<UINT64>(i) * g_srvDescriptorSize;
        return;
    }

    throw std::runtime_error("No free contiguous SRV descriptor range");
}

void FreeSrvDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE start = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    const SIZE_T offset = cpuHandle.ptr - start.ptr;
    const int index = static_cast<int>(offset / g_srvDescriptorSize);
    if (index >= 0 && index < kSrvDescriptorCount)
    {
        g_srvDescriptorUsed[index] = false;
    }
}

void WaitForFenceValue(UINT64 value)
{
    if (g_fence->GetCompletedValue() >= value)
    {
        return;
    }

    const auto waitStart = std::chrono::steady_clock::now();
    ThrowIfFailed(g_fence->SetEventOnCompletion(value, g_fenceEvent), "SetEventOnCompletion failed");
    WaitForSingleObject(g_fenceEvent, INFINITE);
    const auto waitEnd = std::chrono::steady_clock::now();
    g_frameTiming.fenceWaitMs += std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();
}

void WaitForLastSubmittedFrame()
{
    FrameContext& frameContext = g_frameContexts[g_frameIndex % kFrameCount];
    if (frameContext.fenceValue != 0)
    {
        WaitForFenceValue(frameContext.fenceValue);
        frameContext.fenceValue = 0;
    }
}

FrameContext& WaitForNextFrameResources()
{
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    FrameContext& frameContext = g_frameContexts[g_frameIndex % kFrameCount];
    if (frameContext.fenceValue != 0)
    {
        WaitForFenceValue(frameContext.fenceValue);
        frameContext.fenceValue = 0;
    }
    return frameContext;
}

void CreateRenderTarget()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i])), "GetBuffer failed");
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += g_rtvDescriptorSize;
    }
}

void CleanupRenderTarget()
{
    WaitForLastSubmittedFrame();
    for (auto& target : g_renderTargets)
    {
        target.Reset();
    }
}

void ResizeSwapChain(UINT width, UINT height)
{
    if (!g_swapChain || width == 0 || height == 0)
    {
        return;
    }

    g_width = width;
    g_height = height;
    CleanupRenderTarget();
    ThrowIfFailed(g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), "ResizeBuffers failed");
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTarget();
}

void InitD3D(HWND hwnd)
{
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
        {
            g_adapterDedicatedVideoMemory = desc.DedicatedVideoMemory;
            g_adapterSharedSystemMemory = desc.SharedSystemMemory;
            g_adapterDescription = desc.Description;
            constexpr SIZE_T kLowDedicatedMemoryThreshold = 512ull * 1024ull * 1024ull;
            g_lowDedicatedMemoryAdapter = desc.DedicatedVideoMemory < kLowDedicatedMemoryThreshold;
            break;
        }
    }

    if (!g_device)
    {
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice failed");
        g_lowDedicatedMemoryAdapter = true;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue)), "CreateCommandQueue failed");

    for (FrameContext& frameContext : g_frameContexts)
    {
        ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameContext.commandAllocator)), "CreateCommandAllocator failed");
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Width = g_width;
    swapChainDesc.Height = g_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(g_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain), "CreateSwapChainForHwnd failed");
    ThrowIfFailed(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation failed");
    ThrowIfFailed(swapChain.As(&g_swapChain), "SwapChain cast failed");
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc =
        DescriptorHeapDesc(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrameCount);
    ThrowIfFailed(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)), "CreateDescriptorHeap RTV failed");
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc =
        ShaderVisibleCbvSrvUavDescriptorHeapDesc(kSrvDescriptorCount);
    ThrowIfFailed(g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap)), "CreateDescriptorHeap SRV failed");
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContexts[0].commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList)), "CreateCommandList failed");
    ThrowIfFailed(g_commandList->Close(), "CommandList close failed");

    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence failed");
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent)
    {
        throw std::runtime_error("CreateEvent failed");
    }

    CreateRenderTarget();
}

void CleanupD3D()
{
    WaitForLastSubmittedFrame();
    CleanupRenderTarget();
    if (g_gpuMeshPreview.srvAllocated)
    {
        FreeSrvDescriptor(nullptr, g_gpuMeshPreview.srvCpu, g_gpuMeshPreview.srvGpu);
        g_gpuMeshPreview.srvAllocated = false;
    }
    if (g_gpuMeshPreview.postSrvAllocated)
    {
        FreeSrvDescriptor(nullptr, g_gpuMeshPreview.postSrvCpu, g_gpuMeshPreview.postSrvGpu);
        g_gpuMeshPreview.postSrvAllocated = false;
    }
    if (g_gpuMeshPreview.outputSrvAllocated)
    {
        FreeSrvDescriptor(nullptr, g_gpuMeshPreview.outputSrvCpu, g_gpuMeshPreview.outputSrvGpu);
        g_gpuMeshPreview.outputSrvAllocated = false;
    }
    if (g_gpuMeshPreview.exposureSrvAllocated)
    {
        FreeSrvDescriptor(nullptr, g_gpuMeshPreview.exposureSrvCpu[0], g_gpuMeshPreview.exposureSrvGpu[0]);
        FreeSrvDescriptor(nullptr, g_gpuMeshPreview.exposureSrvCpu[1], g_gpuMeshPreview.exposureSrvGpu[1]);
        g_gpuMeshPreview.exposureSrvAllocated = false;
    }
    if (g_gpuMeshPreview.sceneDepthSrvAllocated)
    {
        FreeSrvDescriptor(nullptr, g_gpuMeshPreview.sceneDepthSrvCpu, g_gpuMeshPreview.sceneDepthSrvGpu);
        g_gpuMeshPreview.sceneDepthSrvAllocated = false;
    }
    g_gpuMeshPreview.colorTarget.Reset();
    g_gpuMeshPreview.postTarget.Reset();
    g_gpuMeshPreview.outputTarget.Reset();
    g_gpuMeshPreview.exposureTargets[0].Reset();
    g_gpuMeshPreview.exposureTargets[1].Reset();
    g_gpuMeshPreview.depthTarget.Reset();
    g_gpuMeshPreview.sceneDepthTarget.Reset();
    g_gpuMeshPreview.vertexBuffer.Reset();
    g_gpuMeshPreview.indexBuffer.Reset();
    g_gpuMeshPreview.edgeIndexBuffer.Reset();
    g_gpuMeshPreview.gridVertexBuffer.Reset();
    g_gpuMeshPreview.terrainBoundaryLineVertexBuffer.Reset();
    g_gpuMeshPreview.waterVertexBuffer.Reset();
    g_gpuMeshPreview.waterIndexBuffer.Reset();
    g_gpuMeshPreview.gridVertexCount = 0;
    g_gpuMeshPreview.terrainBoundaryLineVertexCount = 0;
    g_gpuMeshPreview.waterIndexCount = 0;
    g_gpuMeshPreview.terrainBoundaryLineUploadKey = UINT64_MAX;
    ResetMeshPreviewPipelineResources(g_meshPreviewPipelines);
    g_gpuMeshPreview.displacementHeightTexture.Reset();
    g_gpuMeshPreview.displacementMaskTexture.Reset();
    g_gpuMeshPreview.colorizeTexture.Reset();
    g_gpuMeshPreview.colorizeTextureResolution = 0;
    g_gpuMeshPreview.colorizeTextureUploadKey = 0;
    g_gpuMeshPreview.displacementTriIndexBuffer.Reset();
    g_gpuMeshPreview.displacementEdgeIndexBuffer.Reset();
    g_gpuMeshPreview.displacementSrvAllocated = false;
    g_gpuMeshPreview.displacementTextureResolution = 0;
    g_gpuMeshPreview.displacementMeshResolution = 0;
    terrain::gpu::ResetMseComputeResources();
    terrain::gpu::ResetMaskNoiseComputeResources();
    terrain::gpu::ResetMaskUtilityComputeResources();
    terrain::gpu::ResetSedimentComputeResources();
    terrain::gpu::ResetRockComputeResources();
    terrain::gpu::ResetScatterComputeResources();
    terrain::gpu::ResetMaskFluvialComputeResources();
    terrain::gpu::ResetFluvialErosionComputeResources();
    terrain::gpu::ResetDropletErosionComputeResources();
    terrain::gpu::ResetSnowComputeResources();
    terrain::gpu::ResetColorizeComputeResources();
    g_aoComputePso.Reset();
    g_aoComputeRootSignature.Reset();
    g_aoComputeReady = false;
    g_gpuMeshPreview.aoTexture.Reset();
    g_gpuMeshPreview.aoTextureResolution = 0;
    g_gpuMeshPreview.aoUploadKey = 0;
    g_skyPso.Reset();
    g_skyRootSignature.Reset();
    g_skyPipelineReady = false;
    g_skyPipelineFormat = DXGI_FORMAT_UNKNOWN;
    g_atmosphereMultiScatterPso.Reset();
    g_atmosphereMultiScatterRootSignature.Reset();
    g_atmosphereMultiScatterTexture.Reset();
    g_atmosphereMultiScatterState = D3D12_RESOURCE_STATE_COMMON;
    g_atmosphereMultiScatterSrvAllocated = false;
    g_atmosphereMultiScatterReady = false;
    g_atmosphereCachedDensity = std::numeric_limits<float>::quiet_NaN();
    g_atmosphereCachedMie = std::numeric_limits<float>::quiet_NaN();
    g_atmosphereCachedMieG = std::numeric_limits<float>::quiet_NaN();
    g_cloudVolumePso.Reset();
    g_cloudVolumeRootSignature.Reset();
    g_cloudRenderPso.Reset();
    g_cloudRenderRootSignature.Reset();
    g_cloudShadowPso.Reset();
    g_cloudShadowRootSignature.Reset();
    g_cloudPipelinesReady = false;
    g_cloudRenderPipelineFormat = DXGI_FORMAT_UNKNOWN;
    g_dofPso.Reset();
    g_dofRootSignature.Reset();
    g_dofPipelineReady = false;
    g_dofPipelineFormat = DXGI_FORMAT_UNKNOWN;
    g_tonemapPso.Reset();
    g_colorGradePso.Reset();
    g_exposurePso.Reset();
    g_tonemapRootSignature.Reset();
    g_tonemapPipelineReady = false;
    if (g_gpuClouds.meshCbUploadBuffer && g_gpuClouds.meshCbMapped)
    {
        g_gpuClouds.meshCbUploadBuffer->Unmap(0, nullptr);
        g_gpuClouds.meshCbMapped = nullptr;
    }
    g_gpuClouds.volumeTexture.Reset();
    g_gpuClouds.shadowTexture.Reset();
    g_gpuClouds.dummyShadowTexture.Reset();
    g_gpuClouds.meshCbUploadBuffer.Reset();
    g_gpuClouds.volumeReady = false;
    g_gpuClouds.cachedSeed = INT_MIN;
    g_gpuClouds.volumeState = D3D12_RESOURCE_STATE_COMMON;
    g_gpuClouds.shadowState = D3D12_RESOURCE_STATE_COMMON;
    g_gpuClouds.shadowResolution = 0;
    g_gpuClouds.volumeSrvAllocated = false;
    g_gpuClouds.shadowSrvAllocated = false;
    g_gpuClouds.dummyShadowAllocated = false;
    if (g_fenceEvent)
    {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
}

std::filesystem::path ShaderPath(const char* fileName)
{
    const std::filesystem::path cwdPath = std::filesystem::path("shaders") / fileName;
    if (std::filesystem::exists(cwdPath))
    {
        return cwdPath;
    }

    const std::filesystem::path modulePath = std::filesystem::path(ModuleDirectory()) / "shaders" / fileName;
    if (std::filesystem::exists(modulePath))
    {
        return modulePath;
    }

    return cwdPath;
}

std::filesystem::path MeshPreviewShaderPath()
{
    return ShaderPath("mesh_preview.hlsl");
}

std::filesystem::path AOComputeShaderPath()
{
    return ShaderPath("ao_compute.hlsl");
}

std::filesystem::path SkyShaderPath()
{
    return ShaderPath("sky.hlsl");
}

std::filesystem::path AtmosphereMultiScatterShaderPath()
{
    return ShaderPath("atmosphere_multiscatter.hlsl");
}

std::filesystem::path CloudDensityShaderPath()
{
    return ShaderPath("cloud_density.hlsl");
}

std::filesystem::path CloudRenderShaderPath()
{
    return ShaderPath("cloud_render.hlsl");
}

std::filesystem::path CloudShadowShaderPath()
{
    return ShaderPath("cloud_shadow.hlsl");
}

std::filesystem::path DepthOfFieldShaderPath()
{
    return ShaderPath("depth_of_field.hlsl");
}

std::filesystem::path TonemapShaderPath()
{
    return ShaderPath("tonemap.hlsl");
}

DXGI_FORMAT MeshPreviewColorFormat()
{
    return g_graph.Settings().preview.hdrViewportEnabled
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool ColorTemperatureAdjusted()
{
    return std::abs(g_graph.Settings().preview.colorTemperatureKelvin - rock::PreviewSettings{}.colorTemperatureKelvin) > 0.5f;
}

void EvaluateGraph();
int CurrentPreviewMeshResolution();
bool IsTerrainNodeKind(rock::NodeKind kind);
terrain::ViewportCameraState MakeViewportCameraState();
void ResetViewport();
void UpdateMapViewportInteraction(const ImVec2& min, const ImVec2& max);
ImVec2 InitialNodePosition(rock::NodeKind kind);

std::optional<std::filesystem::path> ShowProjectFileDialog(bool save)
{
    return terrain::platform::ShowProjectFileDialog(g_hwnd, g_projectPath, save);
}

std::optional<std::filesystem::path> ShowHeightmapFileDialog(const std::string& currentPath)
{
    return terrain::platform::ShowHeightmapFileDialog(g_hwnd, g_projectPath, currentPath);
}

std::filesystem::path ScreenshotDirectory()
{
    return terrain::ScreenshotDirectoryForProject(g_projectPath);
}

void RevealFileInExplorer(const std::filesystem::path& path)
{
    terrain::platform::RevealFileInExplorer(path);
}

void OpenFolderInExplorer(const std::filesystem::path& folder)
{
    terrain::platform::OpenFolderInExplorer(folder);
}

std::filesystem::path ProjectFolder()
{
    if (g_projectPath.empty())
    {
        return {};
    }
    const std::filesystem::path parent = g_projectPath.parent_path();
    return parent.empty() ? std::filesystem::current_path() : parent;
}

std::filesystem::path ProjectFolderForPath(const std::filesystem::path& projectPath)
{
    return terrain::ProjectFolderForPath(projectPath);
}

std::filesystem::path ActiveProjectFolderForAssetPaths()
{
    if (g_projectSavePathForSerialization)
    {
        return ProjectFolderForPath(*g_projectSavePathForSerialization);
    }
    return ProjectFolder();
}

std::string ResolveProjectAssetPath(std::string_view value)
{
    return terrain::ResolveProjectAssetPath(value, ProjectFolder());
}

std::string MakeProjectAssetPathForJson(const std::string& value)
{
    return terrain::MakeProjectAssetPathForJson(value, ActiveProjectFolderForAssetPaths());
}

std::filesystem::path NormalizedProjectPath(const std::filesystem::path& path)
{
    return terrain::NormalizedProjectPath(path);
}

bool ProjectPathExists(const std::filesystem::path& path)
{
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool PruneMissingRecentProjectPaths()
{
    const auto missing = std::remove_if(g_recentProjectPaths.begin(), g_recentProjectPaths.end(), [](const std::filesystem::path& recentPath) {
        return !ProjectPathExists(recentPath);
    });
    if (missing == g_recentProjectPaths.end())
    {
        return false;
    }

    g_recentProjectPaths.erase(missing, g_recentProjectPaths.end());
    return true;
}

void AddRecentProjectPath(const std::filesystem::path& path)
{
    constexpr size_t kMaxRecentProjects = 8;
    const std::filesystem::path normalized = NormalizedProjectPath(path);
    if (!ProjectPathExists(normalized))
    {
        return;
    }

    PruneMissingRecentProjectPaths();
    const auto existing = std::remove_if(g_recentProjectPaths.begin(), g_recentProjectPaths.end(), [&](const std::filesystem::path& recentPath) {
        return NormalizedProjectPath(recentPath) == normalized;
    });
    g_recentProjectPaths.erase(existing, g_recentProjectPaths.end());
    g_recentProjectPaths.insert(g_recentProjectPaths.begin(), normalized);
    if (g_recentProjectPaths.size() > kMaxRecentProjects)
    {
        g_recentProjectPaths.resize(kMaxRecentProjects);
    }
}

std::filesystem::path DataDirectory()
{
    const std::filesystem::path cwdData = std::filesystem::path("data");
    if (std::filesystem::exists(cwdData))
    {
        return cwdData;
    }

    const std::filesystem::path moduleData = std::filesystem::path(ModuleDirectory()) / "data";
    if (std::filesystem::exists(moduleData))
    {
        return moduleData;
    }

    return cwdData;
}

std::filesystem::path AssetDirectory()
{
    const std::filesystem::path cwdAssets = std::filesystem::path("assets");
    if (std::filesystem::exists(cwdAssets))
    {
        return cwdAssets;
    }

    const std::filesystem::path moduleAssets = std::filesystem::path(ModuleDirectory()) / "assets";
    if (std::filesystem::exists(moduleAssets))
    {
        return moduleAssets;
    }

    return cwdAssets;
}

std::filesystem::path AppSettingsPath()
{
    return DataDirectory() / "app_settings.json";
}

void LoadSavedWindowSize()
{
    try
    {
        const std::filesystem::path path = AppSettingsPath();
        if (!std::filesystem::exists(path))
        {
            return;
        }

        std::ifstream stream(path);
        if (!stream)
        {
            return;
        }

        nlohmann::json root;
        stream >> root;
        const nlohmann::json windowJson = root.value("window", nlohmann::json::object());
        g_width = static_cast<UINT>(std::clamp(windowJson.value("width", static_cast<int>(g_width)), 640, 7680));
        g_height = static_cast<UINT>(std::clamp(windowJson.value("height", static_cast<int>(g_height)), 480, 4320));
    }
    catch (...)
    {
    }
}

bool SaveAppSettings(std::string* error = nullptr)
{
    try
    {
        const rock::GraphSettings& settings = g_graph.Settings();
        PruneMissingRecentProjectPaths();
        nlohmann::json root;
        root["format"] = "terrain_editor_app_settings";
        root["formatVersion"] = 1;
        root["appVersion"] = TERRAIN_EDITOR_VERSION_STRING;
        root["uiTheme"] = g_themeManager.CurrentThemeId();
        root["language"] = UiLanguageCode(CurrentLanguage());
        root["previewVisibility"] = {
            {"mesh", g_ui.meshPreview},
            {"fps", g_ui.showFps},
            {"frameRateLimitFps", g_ui.frameRateLimitFps},
            {"newNodeBackendGpu", g_ui.newNodeBackendGpu},
            {"drawStats", g_ui.showDrawStats},
            {"frameStats", g_ui.showFrameStats},
            {"debugLog", g_ui.debugLogVisible},
            {"meshSurface", settings.preview.showSurface},
            {"meshWireframe", settings.preview.showWireframe},
            {"terrainSizeMeters", settings.preview.terrainSizeMeters},
            {"simulationResolution", settings.preview.simulationResolution},
            {"previewResolution", settings.preview.resolution},
            {"previewLod", settings.preview.lod},
            {"lightingMode", settings.preview.lightingMode},
            {"meshBackend", static_cast<int>(settings.preview.meshBackend)},
            {"terrainBoundaryMode", static_cast<int>(settings.preview.terrainBoundaryMode)},
            {"viewportTessellation", settings.preview.viewportTessellation},
            {"tessellationMinFactor", settings.preview.tessellationMinFactor},
            {"tessellationMaxFactor", settings.preview.tessellationMaxFactor},
            {"tessellationNearDistance", settings.preview.tessellationNearDistance},
            {"tessellationFarDistance", settings.preview.tessellationFarDistance},
            {"waterEnabled", settings.preview.waterEnabled},
            {"waterLevelMeters", settings.preview.waterLevelMeters},
            {"waterOpacity", settings.preview.waterOpacity},
            {"waterColor", {
                settings.preview.waterColor[0],
                settings.preview.waterColor[1],
                settings.preview.waterColor[2],
            }},
            {"waterWavesScale", settings.preview.waterWavesScale},
            {"waterRefractiveIndex", settings.preview.waterRefractiveIndex},
            {"waterFresnelPower", settings.preview.waterFresnelPower},
            {"waterRefractionStrength", settings.preview.waterRefractionStrength},
            {"waterAnimationEnabled", settings.preview.waterAnimationEnabled},
            {"waterReflectionStrength", settings.preview.waterReflectionStrength},
            {"waterSsrEnabled", settings.preview.waterSsrEnabled},
            {"sunAzimuthDegrees", settings.preview.sunAzimuthDegrees},
            {"sunElevationDegrees", settings.preview.sunElevationDegrees},
            {"sunIntensity", settings.preview.sunIntensity},
            {"ambientStrength", settings.preview.ambientStrength},
            {"shadowStrength", settings.preview.shadowStrength},
            {"shadowMapResolution", settings.preview.shadowMapResolution},
            {"shadowBias", settings.preview.shadowBias},
            {"cloudQualitySamples", settings.clouds.qualitySamples},
            {"cloudShadowResolution", settings.clouds.shadowResolution},
            {"cloudShadowSamples", settings.clouds.shadowSamples},
            {"cloudLightSamples", settings.clouds.lightSamples},
            {"sunDirectionMode", static_cast<int>(settings.preview.sunDirectionMode)},
            {"sunLatitudeDegrees", settings.preview.sunLatitudeDegrees},
            {"sunLongitudeDegrees", settings.preview.sunLongitudeDegrees},
            {"sunUtcOffsetHours", settings.preview.sunUtcOffsetHours},
            {"sunMonth", settings.preview.sunMonth},
            {"sunDay", settings.preview.sunDay},
            {"sunTimeHours", settings.preview.sunTimeHours},
            {"sunTimeAnimate", settings.preview.sunTimeAnimate},
            {"sunTimeDayLengthSeconds", settings.preview.sunTimeDayLengthSeconds},
            {"sunTimeSkipNight", settings.preview.sunTimeSkipNight},
            {"pbrAlbedo", {
                settings.preview.pbrAlbedo[0],
                settings.preview.pbrAlbedo[1],
                settings.preview.pbrAlbedo[2],
            }},
            {"viewportBackground", {
                settings.preview.viewportBackground[0],
                settings.preview.viewportBackground[1],
                settings.preview.viewportBackground[2],
            }},
            {"maskShading", static_cast<int>(settings.preview.maskShading)},
            {"maskPreviewUseNearestHeightmap", settings.preview.maskPreviewUseNearestHeightmap},
        };
        root["layout"] = {
            {"rightPaneWidth", g_ui.rightPaneWidth},
            {"nodePaneHeight", g_ui.nodePaneHeight},
            {"debugLogHeight", g_ui.debugLogHeight},
        };
        root["window"] = {
            {"width", g_width},
            {"height", g_height},
        };
        root["recentProjects"] = nlohmann::json::array();
        for (const std::filesystem::path& recentPath : g_recentProjectPaths)
        {
            if (!ProjectPathExists(recentPath))
            {
                continue;
            }
            root["recentProjects"].push_back(PathToUtf8(recentPath));
        }
        root["viewport"] = {
            {"yaw", g_viewport.yaw},
            {"pitch", g_viewport.pitch},
            {"fovDegrees", g_viewport.fovDegrees},
            {"orbitDistance", g_viewport.orbitDistance},
            {"pan", {g_viewport.pan.x, g_viewport.pan.y}},
            {"autoOrbitEnabled", g_viewport.autoOrbitEnabled},
            {"autoOrbitSpeedDegreesPerSecond", g_viewport.autoOrbitSpeedDegreesPerSecond},
        };
        root["mapViewport"] = {
            {"zoom", g_mapViewport.zoom},
            {"pan", {g_mapViewport.pan.x, g_mapViewport.pan.y}},
        };

        const std::filesystem::path path = AppSettingsPath();
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream stream(path);
        if (!stream)
        {
            if (error) *error = "Failed to open app settings for writing";
            return false;
        }
        stream << root.dump(2);
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

void SaveAppSettingsSilently()
{
    std::string error;
    if (!SaveAppSettings(&error))
    {
        SetProjectStatus("App settings save failed: " + error);
    }
}

void SetUiLanguage(UiLanguage language)
{
    if (CurrentLanguage() == language)
    {
        return;
    }
    SetCurrentLanguage(language);
    SaveAppSettingsSilently();
}

void ResetLayoutToDefaults()
{
    g_ui.rightPaneWidth = 0.0f;
    g_ui.nodePaneHeight = 0.0f;
    g_ui.debugLogHeight = kDefaultDebugLogHeight;

    if (g_hwnd)
    {
        ShowWindow(g_hwnd, SW_RESTORE);
        RECT rect{
            0,
            0,
            static_cast<LONG>(kDefaultWindowClientWidth),
            static_cast<LONG>(kDefaultWindowClientHeight),
        };
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(
            g_hwnd,
            nullptr,
            0,
            0,
            rect.right - rect.left,
            rect.bottom - rect.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    g_width = kDefaultWindowClientWidth;
    g_height = kDefaultWindowClientHeight;
    SaveAppSettingsSilently();
}

ComPtr<ID3D12Resource> CreateUploadBuffer(const void* data, UINT64 byteSize, const char* message)
{
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_RESOURCE_DESC desc = BufferResourceDesc(std::max<UINT64>(byteSize, 1));
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)), message);
    if (byteSize > 0)
    {
        void* mapped = nullptr;
        ThrowIfFailed(buffer->Map(0, nullptr, &mapped), "Map upload buffer failed");
        std::memcpy(mapped, data, static_cast<size_t>(byteSize));
        buffer->Unmap(0, nullptr);
    }
    return buffer;
}

bool ApplyLowDedicatedMemoryViewportSafety()
{
    if (!g_lowDedicatedMemoryAdapter)
    {
        return false;
    }

    bool changed = false;
    rock::GraphSettings& settings = g_graph.Settings();
    rock::PreviewSettings& preview = settings.preview;

    const auto setBool = [&](bool& value, bool safeValue) {
        if (value != safeValue)
        {
            value = safeValue;
            changed = true;
        }
    };
    const auto clampIntMax = [&](int& value, int safeMax) {
        if (value > safeMax)
        {
            value = safeMax;
            changed = true;
        }
    };

    if (preview.meshBackend != rock::MeshPreviewBackend::CpuMesh)
    {
        preview.meshBackend = rock::MeshPreviewBackend::CpuMesh;
        changed = true;
    }
    setBool(preview.viewportTessellation, false);
    setBool(preview.hdrViewportEnabled, false);
    setBool(preview.depthOfFieldEnabled, false);
    setBool(preview.waterSsrEnabled, false);
    clampIntMax(preview.resolution, 512);
    clampIntMax(preview.shadowMapResolution, 1024);
    clampIntMax(settings.clouds.qualitySamples, 16);
    clampIntMax(settings.clouds.shadowResolution, 512);
    clampIntMax(settings.clouds.shadowSamples, 8);
    clampIntMax(settings.clouds.lightSamples, 4);

    if (changed)
    {
        SetTransientProjectStatus("Low-memory GPU: safe viewport settings applied");
    }
    return changed;
}

bool LoadAppSettings(std::string* error = nullptr)
{
    try
    {
        const std::filesystem::path path = AppSettingsPath();
        if (!std::filesystem::exists(path))
        {
            return false;
        }

        std::ifstream stream(path);
        if (!stream)
        {
            if (error) *error = "Failed to open app settings for reading";
            return false;
        }

        nlohmann::json root;
        stream >> root;
        const std::string format = root.value("format", std::string());
        if (format != "terrain_editor_app_settings" && format != "rock_generator_app_settings")
        {
            if (error) *error = "Unsupported app settings format";
            return false;
        }

        const std::string themeId = root.value("uiTheme", std::string());
        if (!themeId.empty())
        {
            g_themeManager.ApplyTheme(themeId);
        }
        SetCurrentLanguage(UiLanguageFromCode(root.value("language", std::string(UiLanguageCode(CurrentLanguage())))));

        rock::GraphSettings& settings = g_graph.Settings();

        const nlohmann::json visibilityJson = root.value("previewVisibility", nlohmann::json::object());
        g_ui.meshPreview = visibilityJson.value("mesh", g_ui.meshPreview);
        g_ui.showFps = visibilityJson.value("fps", g_ui.showFps);
        g_ui.frameRateLimitFps = ClampFrameRateLimitFps(visibilityJson.value("frameRateLimitFps", g_ui.frameRateLimitFps));
        g_ui.newNodeBackendGpu = visibilityJson.value("newNodeBackendGpu", g_ui.newNodeBackendGpu);
        g_ui.showDrawStats = visibilityJson.value("drawStats", g_ui.showDrawStats);
        g_ui.showFrameStats = visibilityJson.value("frameStats", g_ui.showFrameStats);
        g_ui.debugLogVisible = visibilityJson.value("debugLog", g_ui.debugLogVisible);
        settings.preview.showSurface = visibilityJson.value("meshSurface", settings.preview.showSurface);
        settings.preview.showWireframe = visibilityJson.value("meshWireframe", settings.preview.showWireframe);
        settings.preview.terrainSizeMeters = static_cast<float>(NearestTerrainSizePreset(visibilityJson.value("terrainSizeMeters", settings.preview.terrainSizeMeters)));
        settings.preview.simulationResolution = NearestResolutionPreset(visibilityJson.value("simulationResolution", settings.preview.simulationResolution));
        settings.preview.resolution = NearestResolutionPreset(visibilityJson.value("previewResolution", settings.preview.resolution));
        settings.preview.lod = std::clamp(visibilityJson.value("previewLod", settings.preview.lod), 0, 4);
        settings.preview.lightingMode = std::clamp(visibilityJson.value("lightingMode", settings.preview.lightingMode), 0, 1);
        {
            const int backendInt = std::clamp(visibilityJson.value("meshBackend", static_cast<int>(settings.preview.meshBackend)),
                static_cast<int>(rock::MeshPreviewBackend::CpuMesh),
                static_cast<int>(rock::MeshPreviewBackend::GpuDisplacement));
            settings.preview.meshBackend = static_cast<rock::MeshPreviewBackend>(backendInt);
        }
        {
            const int boundaryInt = std::clamp(visibilityJson.value("terrainBoundaryMode", static_cast<int>(settings.preview.terrainBoundaryMode)),
                static_cast<int>(rock::TerrainBoundaryMode::None),
                static_cast<int>(rock::TerrainBoundaryMode::Lines));
            settings.preview.terrainBoundaryMode = static_cast<rock::TerrainBoundaryMode>(boundaryInt);
        }
        settings.preview.viewportTessellation = visibilityJson.value("viewportTessellation", settings.preview.viewportTessellation);
        settings.preview.tessellationMinFactor = std::clamp(visibilityJson.value("tessellationMinFactor", settings.preview.tessellationMinFactor), 1.0f, 64.0f);
        settings.preview.tessellationMaxFactor = std::clamp(visibilityJson.value("tessellationMaxFactor", settings.preview.tessellationMaxFactor), settings.preview.tessellationMinFactor, 64.0f);
        settings.preview.tessellationNearDistance = std::clamp(visibilityJson.value("tessellationNearDistance", settings.preview.tessellationNearDistance), 1.0f, 100000.0f);
        settings.preview.tessellationFarDistance = std::clamp(visibilityJson.value("tessellationFarDistance", settings.preview.tessellationFarDistance), settings.preview.tessellationNearDistance + 1.0f, 200000.0f);
        settings.preview.waterEnabled = visibilityJson.value("waterEnabled", settings.preview.waterEnabled);
        settings.preview.waterLevelMeters = std::clamp(visibilityJson.value("waterLevelMeters", settings.preview.waterLevelMeters), 0.0f, 10000.0f);
        settings.preview.waterOpacity = std::clamp(visibilityJson.value("waterOpacity", settings.preview.waterOpacity), 0.0f, 1.0f);
        if (visibilityJson.contains("waterColor") && visibilityJson["waterColor"].is_array() && visibilityJson["waterColor"].size() == 3)
        {
            settings.preview.waterColor[0] = std::clamp(visibilityJson["waterColor"][0].get<float>(), 0.0f, 1.0f);
            settings.preview.waterColor[1] = std::clamp(visibilityJson["waterColor"][1].get<float>(), 0.0f, 1.0f);
            settings.preview.waterColor[2] = std::clamp(visibilityJson["waterColor"][2].get<float>(), 0.0f, 1.0f);
        }
        settings.preview.waterWavesScale = std::clamp(visibilityJson.value("waterWavesScale", settings.preview.waterWavesScale), 1.0f, 500.0f);
        settings.preview.waterRefractiveIndex = std::clamp(visibilityJson.value("waterRefractiveIndex", settings.preview.waterRefractiveIndex), 1.0f, 4.0f);
        settings.preview.waterFresnelPower = std::clamp(visibilityJson.value("waterFresnelPower", settings.preview.waterFresnelPower), 1.0f, 8.0f);
        settings.preview.waterRefractionStrength = std::clamp(visibilityJson.value("waterRefractionStrength", settings.preview.waterRefractionStrength), 0.0f, 2.0f);
        settings.preview.waterAnimationEnabled = visibilityJson.value("waterAnimationEnabled", settings.preview.waterAnimationEnabled);
        settings.preview.waterReflectionStrength = std::clamp(visibilityJson.value("waterReflectionStrength", settings.preview.waterReflectionStrength), 0.0f, 3.0f);
        settings.preview.waterSsrEnabled = visibilityJson.value("waterSsrEnabled", settings.preview.waterSsrEnabled);
        settings.preview.sunAzimuthDegrees = std::clamp(visibilityJson.value("sunAzimuthDegrees", settings.preview.sunAzimuthDegrees), 0.0f, 360.0f);
        settings.preview.sunElevationDegrees = std::clamp(visibilityJson.value("sunElevationDegrees", settings.preview.sunElevationDegrees), -10.0f, 89.0f);
        settings.preview.sunIntensity = std::clamp(visibilityJson.value("sunIntensity", settings.preview.sunIntensity), 0.0f, 5.0f);
        settings.preview.ambientStrength = std::clamp(visibilityJson.value("ambientStrength", settings.preview.ambientStrength), 0.0f, 2.0f);
        settings.preview.shadowStrength = std::clamp(visibilityJson.value("shadowStrength", settings.preview.shadowStrength), 0.0f, 1.0f);
        settings.preview.shadowMapResolution = NearestShadowResolutionPreset(visibilityJson.value("shadowMapResolution", settings.preview.shadowMapResolution));
        settings.preview.shadowBias = std::clamp(visibilityJson.value("shadowBias", settings.preview.shadowBias), 0.0f, 0.05f);
        settings.clouds.qualitySamples = std::clamp(visibilityJson.value("cloudQualitySamples", settings.clouds.qualitySamples), 8, 128);
        settings.clouds.shadowResolution = NearestShadowResolutionPreset(visibilityJson.value("cloudShadowResolution", settings.clouds.shadowResolution));
        settings.clouds.shadowSamples = std::clamp(visibilityJson.value("cloudShadowSamples", settings.clouds.shadowSamples), 4, 64);
        settings.clouds.lightSamples = std::clamp(visibilityJson.value("cloudLightSamples", settings.clouds.lightSamples), 1, 16);
        {
            const int sunModeInt = std::clamp(visibilityJson.value("sunDirectionMode", static_cast<int>(settings.preview.sunDirectionMode)),
                static_cast<int>(rock::SunDirectionMode::Manual),
                static_cast<int>(rock::SunDirectionMode::DateTime));
            settings.preview.sunDirectionMode = static_cast<rock::SunDirectionMode>(sunModeInt);
        }
        settings.preview.sunLatitudeDegrees = std::clamp(visibilityJson.value("sunLatitudeDegrees", settings.preview.sunLatitudeDegrees), -90.0f, 90.0f);
        settings.preview.sunLongitudeDegrees = std::clamp(visibilityJson.value("sunLongitudeDegrees", settings.preview.sunLongitudeDegrees), -180.0f, 180.0f);
        settings.preview.sunUtcOffsetHours = std::clamp(visibilityJson.value("sunUtcOffsetHours", settings.preview.sunUtcOffsetHours), -12.0f, 14.0f);
        settings.preview.sunMonth = std::clamp(visibilityJson.value("sunMonth", settings.preview.sunMonth), 1, 12);
        settings.preview.sunDay = std::clamp(visibilityJson.value("sunDay", settings.preview.sunDay), 1, DaysInMonth(settings.preview.sunMonth));
        settings.preview.sunTimeHours = std::clamp(visibilityJson.value("sunTimeHours", settings.preview.sunTimeHours), 0.0f, 24.0f);
        settings.preview.sunTimeAnimate = visibilityJson.value("sunTimeAnimate", settings.preview.sunTimeAnimate);
        settings.preview.sunTimeDayLengthSeconds = std::clamp(visibilityJson.value("sunTimeDayLengthSeconds", settings.preview.sunTimeDayLengthSeconds), 5.0f, 3600.0f);
        settings.preview.sunTimeSkipNight = visibilityJson.value("sunTimeSkipNight", settings.preview.sunTimeSkipNight);
        if (visibilityJson.contains("pbrAlbedo") && visibilityJson["pbrAlbedo"].is_array() && visibilityJson["pbrAlbedo"].size() == 3)
        {
            settings.preview.pbrAlbedo[0] = std::clamp(visibilityJson["pbrAlbedo"][0].get<float>(), 0.0f, 1.0f);
            settings.preview.pbrAlbedo[1] = std::clamp(visibilityJson["pbrAlbedo"][1].get<float>(), 0.0f, 1.0f);
            settings.preview.pbrAlbedo[2] = std::clamp(visibilityJson["pbrAlbedo"][2].get<float>(), 0.0f, 1.0f);
        }
        if (visibilityJson.contains("viewportBackground") && visibilityJson["viewportBackground"].is_array() && visibilityJson["viewportBackground"].size() == 3)
        {
            settings.preview.viewportBackground[0] = std::clamp(visibilityJson["viewportBackground"][0].get<float>(), 0.0f, 1.0f);
            settings.preview.viewportBackground[1] = std::clamp(visibilityJson["viewportBackground"][1].get<float>(), 0.0f, 1.0f);
            settings.preview.viewportBackground[2] = std::clamp(visibilityJson["viewportBackground"][2].get<float>(), 0.0f, 1.0f);
        }
        {
            const int maskShadingInt = visibilityJson.value("maskShading", static_cast<int>(settings.preview.maskShading));
            settings.preview.maskShading = static_cast<rock::MaskShadingMode>(std::clamp(maskShadingInt,
                static_cast<int>(rock::MaskShadingMode::Grayscale),
                static_cast<int>(rock::MaskShadingMode::GrayscaleHatched)));
        }
        settings.preview.maskPreviewUseNearestHeightmap = visibilityJson.value("maskPreviewUseNearestHeightmap", settings.preview.maskPreviewUseNearestHeightmap);

        const nlohmann::json layoutJson = root.value("layout", nlohmann::json::object());
        g_ui.rightPaneWidth = std::max(0.0f, layoutJson.value("rightPaneWidth", g_ui.rightPaneWidth));
        g_ui.nodePaneHeight = std::max(0.0f, layoutJson.value("nodePaneHeight", g_ui.nodePaneHeight));
        g_ui.debugLogHeight = std::clamp(layoutJson.value("debugLogHeight", g_ui.debugLogHeight), 100.0f, 600.0f);

        const nlohmann::json windowJson = root.value("window", nlohmann::json::object());
        g_width = static_cast<UINT>(std::clamp(windowJson.value("width", static_cast<int>(g_width)), 640, 7680));
        g_height = static_cast<UINT>(std::clamp(windowJson.value("height", static_cast<int>(g_height)), 480, 4320));

        g_recentProjectPaths.clear();
        if (root.contains("recentProjects") && root["recentProjects"].is_array())
        {
            constexpr size_t kMaxRecentProjects = 8;
            for (const nlohmann::json& recentJson : root["recentProjects"])
            {
                if (!recentJson.is_string())
                {
                    continue;
                }
                const std::filesystem::path normalized = NormalizedProjectPath(PathFromUtf8(recentJson.get<std::string>()));
                if (!ProjectPathExists(normalized))
                {
                    continue;
                }
                const auto duplicate = std::ranges::find_if(g_recentProjectPaths, [&](const std::filesystem::path& recentPath) {
                    return NormalizedProjectPath(recentPath) == normalized;
                });
                if (duplicate != g_recentProjectPaths.end())
                {
                    continue;
                }
                g_recentProjectPaths.push_back(normalized);
                if (g_recentProjectPaths.size() >= kMaxRecentProjects)
                {
                    break;
                }
            }
        }

        const nlohmann::json viewportJson = root.value("viewport", nlohmann::json::object());
        g_viewport.yaw = viewportJson.value("yaw", g_viewport.yaw);
        g_viewport.pitch = viewportJson.value("pitch", g_viewport.pitch);
        g_viewport.fovDegrees = viewportJson.value("fovDegrees", g_viewport.fovDegrees);
        g_viewport.orbitDistance = viewportJson.value("orbitDistance", g_viewport.orbitDistance);
        g_viewport.autoOrbitEnabled = viewportJson.value("autoOrbitEnabled", g_viewport.autoOrbitEnabled);
        g_viewport.autoOrbitSpeedDegreesPerSecond = std::clamp(viewportJson.value("autoOrbitSpeedDegreesPerSecond", g_viewport.autoOrbitSpeedDegreesPerSecond), -360.0f, 360.0f);
        const std::string savedAppVersion = root.value("appVersion", std::string());
        NormalizeLoadedViewport(savedAppVersion != TERRAIN_EDITOR_VERSION_STRING);
        if (viewportJson.contains("pan") && viewportJson["pan"].is_array() && viewportJson["pan"].size() == 2)
        {
            g_viewport.pan = ImVec2(viewportJson["pan"][0].get<float>(), viewportJson["pan"][1].get<float>());
        }

        const nlohmann::json mapViewportJson = root.value("mapViewport", nlohmann::json::object());
        g_mapViewport.zoom = std::clamp(mapViewportJson.value("zoom", g_mapViewport.zoom), 0.05f, 64.0f);
        if (mapViewportJson.contains("pan") && mapViewportJson["pan"].is_array() && mapViewportJson["pan"].size() == 2)
        {
            g_mapViewport.pan = ImVec2(mapViewportJson["pan"][0].get<float>(), mapViewportJson["pan"][1].get<float>());
        }

        SetTransientProjectStatus("Loaded app settings " + PathToUtf8(path));
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

void ApplyEnvironmentBackendDefault(rock::Node& node)
{
    if (g_ui.newNodeBackendGpu)
    {
        node.multiScaleErosion.backend = rock::MultiScaleErosionBackend::GpuCompute;
        node.maskNoise.backend = rock::MaskNoiseBackend::GpuCompute;
        node.maskBlur.backend = rock::MaskUtilityBackend::GpuCompute;
        node.maskPath.backend = rock::MaskUtilityBackend::GpuCompute;
        node.heightmapFromMask.backend = rock::MaskUtilityBackend::GpuCompute;
        node.maskFluvial.backend = rock::MaskFluvialBackend::GpuCompute;
        node.rock.backend = rock::RockBackend::GpuCompute;
        node.scatter.backend = rock::ScatterBackend::GpuCompute;
        node.sediment.backend = rock::SedimentBackend::GpuCompute;
        node.snow.backend = rock::SnowBackend::GpuCompute;
        node.soil.backend = rock::SoilBackend::GpuCompute;
        node.colorize.backend = rock::ColorizeBackend::GpuCompute;
        return;
    }

    node.multiScaleErosion.backend = rock::MultiScaleErosionBackend::CpuReference;
    node.maskNoise.backend = rock::MaskNoiseBackend::CpuParallel;
    node.maskBlur.backend = rock::MaskUtilityBackend::CpuParallel;
    node.maskPath.backend = rock::MaskUtilityBackend::CpuParallel;
    node.heightmapFromMask.backend = rock::MaskUtilityBackend::CpuParallel;
    node.maskFluvial.backend = rock::MaskFluvialBackend::CpuReference;
    node.rock.backend = rock::RockBackend::CpuReference;
    node.scatter.backend = rock::ScatterBackend::CpuReference;
    node.sediment.backend = rock::SedimentBackend::CpuReference;
    node.snow.backend = rock::SnowBackend::CpuReference;
    node.soil.backend = rock::SoilBackend::CpuReference;
    node.colorize.backend = rock::ColorizeBackend::CpuParallel;
}

rock::GraphId CreateNodeWithEnvironmentDefaults(rock::NodeKind kind)
{
    const rock::GraphId nodeId = g_graph.CreateNode(kind);
    if (rock::Node* node = g_graph.FindMutableNode(nodeId))
    {
        ApplyEnvironmentBackendDefault(*node);
    }
    return nodeId;
}

void ResetNodeEditorViewToDefault()
{
    g_selectedNodeId = 0;
    g_nodePositionsInitialized = false;
    g_nodeGraphNavigatedToContent = false;
    g_pendingNodePositions.clear();
    g_nodePositionCache.clear();
    g_pendingSelectedNodeIds.clear();
    if (g_nodeEditor != nullptr)
    {
        NodeEditorContextScope editorScope(g_nodeEditor);
        ed::ClearSelection();
        for (const rock::Node& node : g_graph.Nodes())
        {
            const ImVec2 position = InitialNodePosition(node.kind);
            ed::SetNodePosition(ed::NodeId(node.id), position);
            g_nodePositionCache.push_back({node.id, position});
        }
        ed::NavigateToContent(0.0f);
        g_nodePositionsInitialized = true;
        g_nodeGraphNavigatedToContent = true;
    }
}

std::vector<std::pair<rock::GraphId, ImVec2>> CachedNodePositions()
{
    std::vector<std::pair<rock::GraphId, ImVec2>> positions;
    positions.reserve(g_graph.Nodes().size());
    for (const rock::Node& node : g_graph.Nodes())
    {
        ImVec2 position = InitialNodePosition(node.kind);
        const auto cached = std::ranges::find_if(g_nodePositionCache, [&](const auto& entry) {
            return entry.first == node.id;
        });
        if (cached != g_nodePositionCache.end())
        {
            position = cached->second;
        }
        const auto pending = std::ranges::find_if(g_pendingNodePositions, [&](const auto& entry) {
            return entry.first == node.id;
        });
        if (pending != g_pendingNodePositions.end())
        {
            position = pending->second;
        }
        positions.push_back({node.id, position});
    }
    return positions;
}

std::vector<rock::GraphId> SelectedNodeIdsFromEditor()
{
    std::vector<ed::NodeId> selectedNodes(g_graph.Nodes().size());
    const int selectedCount = ed::GetSelectedNodes(selectedNodes.data(), static_cast<int>(selectedNodes.size()));
    std::vector<rock::GraphId> selectedNodeIds;
    selectedNodeIds.reserve(static_cast<size_t>(selectedCount));
    for (int index = 0; index < selectedCount; ++index)
    {
        const rock::GraphId nodeId = static_cast<rock::GraphId>(selectedNodes[static_cast<size_t>(index)].Get());
        if (g_graph.FindNode(nodeId) != nullptr)
        {
            selectedNodeIds.push_back(nodeId);
        }
    }
    if (selectedNodeIds.empty() && g_graph.FindNode(g_selectedNodeId) != nullptr)
    {
        selectedNodeIds.push_back(g_selectedNodeId);
    }
    return selectedNodeIds;
}

std::vector<rock::GraphId> CurrentSelectedNodeIds()
{
    if (g_nodeEditor == nullptr)
    {
        return g_pendingSelectedNodeIds.empty() && g_selectedNodeId != 0
            ? std::vector<rock::GraphId>{g_selectedNodeId}
            : g_pendingSelectedNodeIds;
    }

    if (!g_nodeEditorFrameActive)
    {
        NodeEditorContextScope editorScope(g_nodeEditor);
        return SelectedNodeIdsFromEditor();
    }
    return SelectedNodeIdsFromEditor();
}

void ApplyNodeSelection(const std::vector<rock::GraphId>& nodeIds)
{
    ed::ClearSelection();
    g_selectedNodeId = 0;
    bool append = false;
    for (const rock::GraphId nodeId : nodeIds)
    {
        if (g_graph.FindNode(nodeId) == nullptr)
        {
            continue;
        }

        ed::SelectNode(ed::NodeId(nodeId), append);
        append = true;
        if (g_selectedNodeId == 0)
        {
            g_selectedNodeId = nodeId;
        }
    }
}

GraphEditSnapshot CaptureGraphEditSnapshot()
{
    GraphEditSnapshot snapshot;
    snapshot.nodes = g_graph.Nodes();
    snapshot.links = g_graph.Links();
    snapshot.nodePositions = CachedNodePositions();
    snapshot.selectedNodeIds = CurrentSelectedNodeIds();
    snapshot.selectedNodeId = snapshot.selectedNodeIds.empty() ? g_selectedNodeId : snapshot.selectedNodeIds.front();
    snapshot.previewNodeId = g_graph.Evaluation().previewNodeId;
    snapshot.previewPinId = g_graph.Evaluation().previewPinId;
    snapshot.previewStage = g_graph.Preview();
    return snapshot;
}

GraphEditSnapshot CaptureGraphEditSnapshotWithPositions(const std::vector<std::pair<rock::GraphId, ImVec2>>& positions)
{
    GraphEditSnapshot snapshot = CaptureGraphEditSnapshot();
    snapshot.nodePositions = positions;
    return snapshot;
}

bool NodePositionsChanged(
    const std::vector<std::pair<rock::GraphId, ImVec2>>& a,
    const std::vector<std::pair<rock::GraphId, ImVec2>>& b)
{
    if (a.size() != b.size())
    {
        return true;
    }
    for (const auto& [nodeId, position] : a)
    {
        const auto it = std::ranges::find_if(b, [nodeId](const auto& entry) {
            return entry.first == nodeId;
        });
        if (it == b.end())
        {
            return true;
        }
        if (std::abs(position.x - it->second.x) > 0.5f || std::abs(position.y - it->second.y) > 0.5f)
        {
            return true;
        }
    }
    return false;
}

void CommitUndoSnapshot(GraphEditSnapshot snapshot)
{
    constexpr size_t kMaxUndoSnapshots = 64;
    g_undoStack.push_back(std::move(snapshot));
    if (g_undoStack.size() > kMaxUndoSnapshots)
    {
        g_undoStack.erase(g_undoStack.begin());
    }
    g_redoStack.clear();
}

void PushUndoSnapshot()
{
    CommitUndoSnapshot(CaptureGraphEditSnapshot());
}

void BeginPropertyUndoEdit()
{
    if (!g_pendingPropertyEditUndo)
    {
        g_pendingPropertyEditUndo = CaptureGraphEditSnapshot();
    }
}

void CommitPropertyUndoEdit()
{
    if (!g_pendingPropertyEditUndo)
    {
        return;
    }

    CommitUndoSnapshot(std::move(*g_pendingPropertyEditUndo));
    g_pendingPropertyEditUndo.reset();
}

void ClearUndoHistory()
{
    g_undoStack.clear();
    g_redoStack.clear();
    g_pendingPropertyEditUndo.reset();
    g_pendingNodeMoveUndo.reset();
}

void ApplyGraphEditSnapshot(const GraphEditSnapshot& snapshot)
{
    g_skipNodeMoveUndoThisFrame = true;
    g_graph.ReplaceNodes(snapshot.nodes);
    g_graph.ReplaceLinks(snapshot.links);
    g_graph.SetPreviewStage(snapshot.previewStage);
    if (snapshot.previewPinId != 0 && g_graph.FindPin(snapshot.previewPinId) != nullptr)
    {
        g_graph.SetPreviewPin(snapshot.previewPinId);
    }
    else if (g_graph.FindNode(snapshot.previewNodeId) != nullptr)
    {
        g_graph.SetPreviewNode(snapshot.previewNodeId);
    }
    g_pendingNodePositions = snapshot.nodePositions;
    g_nodePositionCache = snapshot.nodePositions;
    g_pendingSelectedNodeIds = snapshot.selectedNodeIds;
    g_selectedNodeId = snapshot.selectedNodeId;
    g_nodePositionsInitialized = false;
    EvaluateGraph();
}

void UndoGraphEdit()
{
    if (g_undoStack.empty())
    {
        return;
    }

    GraphEditSnapshot undoSnapshot = std::move(g_undoStack.back());
    g_undoStack.pop_back();
    g_redoStack.push_back(CaptureGraphEditSnapshot());
    ApplyGraphEditSnapshot(undoSnapshot);
    SetProjectStatus("Undo");
    MarkProjectDirty();
}

void RedoGraphEdit()
{
    if (g_redoStack.empty())
    {
        return;
    }

    GraphEditSnapshot redoSnapshot = std::move(g_redoStack.back());
    g_redoStack.pop_back();
    g_undoStack.push_back(CaptureGraphEditSnapshot());
    ApplyGraphEditSnapshot(redoSnapshot);
    SetProjectStatus("Redo");
    MarkProjectDirty();
}

void NewProject()
{
    ClearUndoHistory();
    g_graph = rock::NodeGraph::CreateDefaultTerrainGraph();
    g_projectPath.clear();
    SetProjectDirty(false);
    UpdateWindowTitle();
    SetProjectStatus("New project");
    g_exportStatus.clear();
    ResetViewport();
    ResetNodeEditorViewToDefault();
    EvaluateGraph();
}

nlohmann::json MakeViewportJson()
{
    nlohmann::json viewportJson = terrain::MakeViewportJson(MakeViewportCameraState());
    viewportJson["autoOrbitEnabled"] = g_viewport.autoOrbitEnabled;
    viewportJson["autoOrbitSpeedDegreesPerSecond"] = g_viewport.autoOrbitSpeedDegreesPerSecond;
    return viewportJson;
}

void WriteSelectedNodesJson(nlohmann::json& root)
{
    root["selectedNodeId"] = g_selectedNodeId;
    root["selectedNodeIds"] = nlohmann::json::array();
    if (g_nodeEditor == nullptr)
    {
        return;
    }

    NodeEditorContextScope editorScope(g_nodeEditor);
    std::vector<ed::NodeId> selectedNodes(g_graph.Nodes().size());
    const int selectedCount = ed::GetSelectedNodes(selectedNodes.data(), static_cast<int>(selectedNodes.size()));
    g_selectedNodeId = selectedCount > 0 ? static_cast<rock::GraphId>(selectedNodes.front().Get()) : 0;
    root["selectedNodeId"] = g_selectedNodeId;
    for (int i = 0; i < selectedCount; ++i)
    {
        root["selectedNodeIds"].push_back(static_cast<rock::GraphId>(selectedNodes[static_cast<size_t>(i)].Get()));
    }
}

nlohmann::json MakeNodePositionsJson()
{
    nlohmann::json nodePositionsJson = nlohmann::json::object();
    for (const rock::Node& node : g_graph.Nodes())
    {
        ImVec2 position = InitialNodePosition(node.kind);
        const auto cached = std::ranges::find_if(g_nodePositionCache, [&](const auto& entry) {
            return entry.first == node.id;
        });
        if (cached != g_nodePositionCache.end())
        {
            position = cached->second;
        }
        else if (g_nodeEditor != nullptr)
        {
            NodeEditorContextScope editorScope(g_nodeEditor);
            position = ed::GetNodePosition(ed::NodeId(node.id));
        }
        nodePositionsJson[std::to_string(node.id)] = {position.x, position.y};
    }
    return nodePositionsJson;
}

bool SaveProjectToFile(const std::filesystem::path& path, std::string* error)
{
    try
    {
        g_projectSavePathForSerialization = path;
        nlohmann::json root;
        root["format"] = "terrain_editor_project";
        root["formatVersion"] = 1;
        root["appVersion"] = TERRAIN_EDITOR_VERSION_STRING;
        WriteSelectedNodesJson(root);
        root["previewStage"] = static_cast<int>(g_graph.Preview());
        root["previewPinId"] = g_graph.Evaluation().previewPinId;
        root["settings"] = terrain::MakeProjectSettingsJson(g_graph.Settings());

        root["nodeSettings"] = nlohmann::json::object();
        root["viewport"] = MakeViewportJson();
        root["nodes"] = terrain::MakeSerializedNodesJson(g_graph.Nodes(), MakeProjectAssetPathForJson);
        root["links"] = terrain::MakeSerializedLinksJson(g_graph.Links());
        root["nodePositions"] = MakeNodePositionsJson();

        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream stream(path);
        if (!stream)
        {
            g_projectSavePathForSerialization.reset();
            if (error) *error = "Failed to open project for writing";
            return false;
        }
        stream << root.dump(2);
        g_projectSavePathForSerialization.reset();
        g_projectPath = path;
        SetProjectDirty(false);
        UpdateWindowTitle();
        AddRecentProjectPath(path);
        SaveAppSettingsSilently();
        SetProjectStatus("Saved " + PathToUtf8(path));
        return true;
    }
    catch (const std::exception& ex)
    {
        g_projectSavePathForSerialization.reset();
        if (error) *error = ex.what();
        return false;
    }
}

bool SaveCurrentProject()
{
    const std::optional<std::filesystem::path> path =
        g_projectPath.empty() ? ShowProjectFileDialog(true) : std::optional<std::filesystem::path>(g_projectPath);
    if (!path)
    {
        return false;
    }

    std::string error;
    if (!SaveProjectToFile(*path, &error))
    {
        SetProjectStatus("Save failed: " + error);
        return false;
    }

    return true;
}

bool ConfirmSaveUnsavedChanges()
{
    if (!g_projectDirty)
    {
        return true;
    }

    const int result = MessageBoxW(
        g_hwnd,
        CurrentLanguage() == UiLanguage::Japanese
            ? L"保存されていない変更があります。\n\n変更を保存しますか？"
            : L"There are unsaved changes.\n\nDo you want to save them?",
        CurrentLanguage() == UiLanguage::Japanese ? L"未保存の変更" : L"Unsaved Changes",
        MB_ICONWARNING | MB_YESNOCANCEL | MB_DEFBUTTON1);
    if (result == IDYES)
    {
        return SaveCurrentProject();
    }
    if (result == IDNO)
    {
        return true;
    }
    return false;
}

void ReadSerializedNodesJson(const nlohmann::json& root)
{
    std::vector<rock::Node> nodes = terrain::ReadSerializedNodesJson(root);
    if (!nodes.empty())
    {
        g_graph.ReplaceNodes(std::move(nodes));
    }
}

void MigrateLegacySimulationResolutionFromNodes()
{
    if (g_projectSettingsHadSimulationResolution)
    {
        return;
    }

    int resolution = 0;
    for (const rock::Node& node : g_graph.Nodes())
    {
        if (node.kind == rock::NodeKind::HeightmapLoad)
        {
            resolution = std::max(resolution, node.heightmap.simulationResolution);
        }
        else if (node.kind == rock::NodeKind::Shape)
        {
            resolution = std::max(resolution, node.shape.simulationResolution);
        }
        else if (node.kind == rock::NodeKind::MaskNoise)
        {
            resolution = std::max(resolution, node.maskNoise.simulationResolution);
        }
    }
    g_graph.Settings().preview.simulationResolution = NearestResolutionPreset(
        resolution > 0 ? resolution : rock::PreviewSettings{}.simulationResolution);
}

void ReadViewportJson(const nlohmann::json& root)
{
    const std::string savedAppVersion = root.value("appVersion", std::string());
    terrain::ViewportCameraState viewport = MakeViewportCameraState();
    terrain::ReadViewportJson(root, viewport, savedAppVersion != TERRAIN_EDITOR_VERSION_STRING, kDefaultViewportPitch, kDefaultViewportOrbitDistance);
    g_viewport.yaw = viewport.yaw;
    g_viewport.pitch = viewport.pitch;
    g_viewport.fovDegrees = viewport.fovDegrees;
    g_viewport.orbitDistance = viewport.orbitDistance;
    g_viewport.pan = viewport.pan;
    const nlohmann::json viewportJson = root.value("viewport", nlohmann::json::object());
    g_viewport.autoOrbitEnabled = viewportJson.value("autoOrbitEnabled", g_viewport.autoOrbitEnabled);
    g_viewport.autoOrbitSpeedDegreesPerSecond = std::clamp(viewportJson.value("autoOrbitSpeedDegreesPerSecond", g_viewport.autoOrbitSpeedDegreesPerSecond), -360.0f, 360.0f);
}

void ReadSerializedLinksJson(const nlohmann::json& root)
{
    std::vector<rock::Link> links = terrain::ReadSerializedLinksJson(root, [](rock::GraphId startPin, rock::GraphId endPin) {
        return g_graph.CanCreateLink(startPin, endPin);
    });
    g_graph.ReplaceLinks(std::move(links));
}

void ReadSelectedNodesJson(const nlohmann::json& root)
{
    g_selectedNodeId = root.value("selectedNodeId", 0);
    g_pendingSelectedNodeIds.clear();
    if (root.contains("selectedNodeIds") && root["selectedNodeIds"].is_array())
    {
        for (const nlohmann::json& nodeIdJson : root["selectedNodeIds"])
        {
            if (!nodeIdJson.is_number_integer())
            {
                continue;
            }
            const rock::GraphId nodeId = nodeIdJson.get<rock::GraphId>();
            if (g_graph.FindNode(nodeId) != nullptr)
            {
                g_pendingSelectedNodeIds.push_back(nodeId);
            }
        }
    }
    else if (g_graph.FindNode(g_selectedNodeId) != nullptr)
    {
        g_pendingSelectedNodeIds.push_back(g_selectedNodeId);
    }
}

void ReadPreviewSelectionJson(const nlohmann::json& root)
{
    g_graph.SetPreviewStage(terrain::ReadSerializedPreviewStage(root, g_graph.Preview()).value_or(rock::PreviewStage::Graph));
    const rock::GraphId previewPinId = root.value("previewPinId", 0);
    if (previewPinId != 0 && g_graph.FindPin(previewPinId) != nullptr)
    {
        g_graph.SetPreviewPin(previewPinId);
    }
}

void ReadNodePositionsJson(const nlohmann::json& root)
{
    g_pendingNodePositions.clear();
    g_nodePositionCache.clear();
    if (root.contains("nodePositions") && root["nodePositions"].is_object())
    {
        for (const rock::Node& node : g_graph.Nodes())
        {
            const std::string key = std::to_string(node.id);
            if (!root["nodePositions"].contains(key))
            {
                continue;
            }

            const nlohmann::json& positionJson = root["nodePositions"][key];
            if (positionJson.is_array() && positionJson.size() == 2)
            {
                const ImVec2 position(positionJson[0].get<float>(), positionJson[1].get<float>());
                g_pendingNodePositions.push_back({node.id, position});
                g_nodePositionCache.push_back({node.id, position});
            }
        }
    }
    g_nodePositionsInitialized = false;
    g_nodeGraphNavigatedToContent = false;
}

bool LoadProjectFromFile(const std::filesystem::path& path, std::string* error)
{
    try
    {
        std::ifstream stream(path);
        if (!stream)
        {
            if (error) *error = "Failed to open project for reading";
            return false;
        }

        nlohmann::json root;
        stream >> root;
        const std::string format = root.value("format", std::string());
        if (format != "terrain_editor_project" && format != "rock_generator_project")
        {
            if (error) *error = "Unsupported project format";
            return false;
        }

        g_projectSettingsHadSimulationResolution = terrain::ReadProjectSettingsJson(root, g_graph.Settings());
        ReadSerializedNodesJson(root);
        MigrateLegacySimulationResolutionFromNodes();
        ReadViewportJson(root);
        ReadSerializedLinksJson(root);
        ReadSelectedNodesJson(root);
        ReadPreviewSelectionJson(root);
        ReadNodePositionsJson(root);
        ApplyLowDedicatedMemoryViewportSafety();

        g_projectPath = path;
        SetProjectDirty(false);
        UpdateWindowTitle();
        AddRecentProjectPath(path);
        SaveAppSettingsSilently();
        ClearUndoHistory();
        SetTransientProjectStatus("Loaded " + PathToUtf8(path));
        EvaluateGraph();
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

// Phase 2 GPU vertex displacement: a separate root signature + 2 PSOs that
// read the heightfield as a texture from a static UV grid (no vertex
// buffer, just SV_VertexID + index buffer). The CPU mesh path is left
// untouched — switching backends just toggles which (rootsig, PSO,
// indexbuffer, optional vb) combination the render path uses.
struct DisplacementShaderConstants
{
    float gridResolution;
    float terrainSize;
    float halfSize;
    float worldDX;
    float tessellationMinFactor;
    float tessellationMaxFactor;
    float tessellationNearDistance;
    float tessellationFarDistance;
};
static_assert(sizeof(DisplacementShaderConstants) == 8 * sizeof(UINT), "DisplacementShaderConstants must be 8 DWORDs");

// Build the static index buffers used by the displacement render path.
// Triangle indices: 6 per cell × M1² cells. Edge indices: 2 × (3 × M*M1)
// (horizontal + vertical + diagonal). Both regenerate when meshResolution
// changes. SV_VertexID is implicit, so no vertex buffer is needed.
bool EnsureDisplacementGridIndexBuffers(int meshResolution, std::string* error)
{
    if (g_gpuMeshPreview.displacementMeshResolution == meshResolution &&
        g_gpuMeshPreview.displacementTriIndexBuffer &&
        g_gpuMeshPreview.displacementPatchIndexBuffer &&
        g_gpuMeshPreview.displacementSectionIndexBuffer &&
        g_gpuMeshPreview.displacementEdgeIndexBuffer)
    {
        return true;
    }
    if (!g_device) { if (error) *error = "D3D12 device not initialized"; return false; }
    if (meshResolution < 2) { if (error) *error = "Displacement mesh resolution too low"; return false; }

    const int M = meshResolution;
    const int M1 = M - 1;

    std::vector<UINT> triIndices;
    triIndices.reserve(static_cast<size_t>(M1) * static_cast<size_t>(M1) * 6u);
    std::vector<UINT> patchIndices;
    patchIndices.reserve(static_cast<size_t>(M1) * static_cast<size_t>(M1) * 4u);
    std::vector<UINT> sectionIndices;
    sectionIndices.reserve(static_cast<size_t>(M1) * static_cast<size_t>(M1) * 6u + static_cast<size_t>(M1) * 24u);
    for (int z = 0; z < M1; ++z)
    {
        for (int x = 0; x < M1; ++x)
        {
            const UINT a = static_cast<UINT>(z * M + x);
            const UINT b = static_cast<UINT>(z * M + x + 1);
            const UINT c = static_cast<UINT>((z + 1) * M + x + 1);
            const UINT d = static_cast<UINT>((z + 1) * M + x);
            triIndices.push_back(a); triIndices.push_back(b); triIndices.push_back(c);
            triIndices.push_back(a); triIndices.push_back(c); triIndices.push_back(d);
            patchIndices.push_back(a); patchIndices.push_back(b); patchIndices.push_back(c); patchIndices.push_back(d);
        }
    }

    const UINT wallVertexCount = static_cast<UINT>(8 * M);
    for (int side = 0; side < 4; ++side)
    {
        const UINT sideBase = static_cast<UINT>(side * 2 * M);
        for (int i = 0; i < M1; ++i)
        {
            const UINT topA = sideBase + static_cast<UINT>(2 * i);
            const UINT bottomA = topA + 1u;
            const UINT topB = sideBase + static_cast<UINT>(2 * (i + 1));
            const UINT bottomB = topB + 1u;
            sectionIndices.push_back(topA); sectionIndices.push_back(topB); sectionIndices.push_back(bottomB);
            sectionIndices.push_back(topA); sectionIndices.push_back(bottomB); sectionIndices.push_back(bottomA);
        }
    }
    for (int z = 0; z < M1; ++z)
    {
        for (int x = 0; x < M1; ++x)
        {
            const UINT a = wallVertexCount + static_cast<UINT>(z * M + x);
            const UINT b = wallVertexCount + static_cast<UINT>((z + 1) * M + x);
            const UINT c = wallVertexCount + static_cast<UINT>((z + 1) * M + x + 1);
            const UINT d = wallVertexCount + static_cast<UINT>(z * M + x + 1);
            sectionIndices.push_back(a); sectionIndices.push_back(b); sectionIndices.push_back(c);
            sectionIndices.push_back(a); sectionIndices.push_back(c); sectionIndices.push_back(d);
        }
    }

    // Edge layout matches BuildMeshFromHeightfield's top surface: horizontal,
    // vertical, diagonal — emit unique only.
    std::vector<UINT> edgeIndices;
    edgeIndices.reserve(static_cast<size_t>(M) * static_cast<size_t>(M1) * 4u + static_cast<size_t>(M1) * static_cast<size_t>(M1) * 2u);
    for (int z = 0; z < M; ++z)
    {
        for (int x = 0; x < M1; ++x)
        {
            edgeIndices.push_back(static_cast<UINT>(z * M + x));
            edgeIndices.push_back(static_cast<UINT>(z * M + x + 1));
        }
    }
    for (int z = 0; z < M1; ++z)
    {
        for (int x = 0; x < M; ++x)
        {
            edgeIndices.push_back(static_cast<UINT>(z * M + x));
            edgeIndices.push_back(static_cast<UINT>((z + 1) * M + x));
        }
        for (int x = 0; x < M1; ++x)
        {
            edgeIndices.push_back(static_cast<UINT>(z * M + x));
            edgeIndices.push_back(static_cast<UINT>((z + 1) * M + x + 1));
        }
    }

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    auto makeBuffer = [&](size_t bytes, ComPtr<ID3D12Resource>& out) -> bool {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        out.Reset();
        return SUCCEEDED(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&out)));
    };
    if (!makeBuffer(triIndices.size() * sizeof(UINT), g_gpuMeshPreview.displacementTriIndexBuffer))
    {
        if (error) *error = "Create displacement tri IB failed";
        return false;
    }
    if (!makeBuffer(patchIndices.size() * sizeof(UINT), g_gpuMeshPreview.displacementPatchIndexBuffer))
    {
        if (error) *error = "Create displacement patch IB failed";
        return false;
    }
    if (!makeBuffer(sectionIndices.size() * sizeof(UINT), g_gpuMeshPreview.displacementSectionIndexBuffer))
    {
        if (error) *error = "Create displacement section IB failed";
        return false;
    }
    if (!makeBuffer(edgeIndices.size() * sizeof(UINT), g_gpuMeshPreview.displacementEdgeIndexBuffer))
    {
        if (error) *error = "Create displacement edge IB failed";
        return false;
    }

    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        g_gpuMeshPreview.displacementTriIndexBuffer->Map(0, &readRange, &mapped);
        std::memcpy(mapped, triIndices.data(), triIndices.size() * sizeof(UINT));
        g_gpuMeshPreview.displacementTriIndexBuffer->Unmap(0, nullptr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        g_gpuMeshPreview.displacementPatchIndexBuffer->Map(0, &readRange, &mapped);
        std::memcpy(mapped, patchIndices.data(), patchIndices.size() * sizeof(UINT));
        g_gpuMeshPreview.displacementPatchIndexBuffer->Unmap(0, nullptr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        g_gpuMeshPreview.displacementSectionIndexBuffer->Map(0, &readRange, &mapped);
        std::memcpy(mapped, sectionIndices.data(), sectionIndices.size() * sizeof(UINT));
        g_gpuMeshPreview.displacementSectionIndexBuffer->Unmap(0, nullptr);
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        g_gpuMeshPreview.displacementEdgeIndexBuffer->Map(0, &readRange, &mapped);
        std::memcpy(mapped, edgeIndices.data(), edgeIndices.size() * sizeof(UINT));
        g_gpuMeshPreview.displacementEdgeIndexBuffer->Unmap(0, nullptr);
    }

    g_gpuMeshPreview.displacementTriIndexCount = static_cast<UINT>(triIndices.size());
    g_gpuMeshPreview.displacementPatchIndexCount = static_cast<UINT>(patchIndices.size());
    g_gpuMeshPreview.displacementSectionIndexCount = static_cast<UINT>(sectionIndices.size());
    g_gpuMeshPreview.displacementEdgeIndexCount = static_cast<UINT>(edgeIndices.size());
    g_gpuMeshPreview.displacementMeshResolution = meshResolution;
    return true;
}

// Allocate / re-allocate the height + mask textures at the given
// simulation resolution. Both are sampled in the displacement VS.
bool EnsureDisplacementHeightTextures(int simulationResolution, std::string* error)
{
    if (g_gpuMeshPreview.displacementHeightTexture &&
        g_gpuMeshPreview.displacementMaskTexture &&
        g_gpuMeshPreview.displacementTextureResolution == simulationResolution &&
        g_gpuMeshPreview.displacementSrvAllocated)
    {
        return true;
    }
    if (!g_device) { if (error) *error = "D3D12 device not initialized"; return false; }
    if (simulationResolution < 2) { if (error) *error = "Simulation resolution too low"; return false; }

    g_gpuMeshPreview.displacementHeightTexture.Reset();
    g_gpuMeshPreview.displacementMaskTexture.Reset();

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    auto makeTexture = [&](DXGI_FORMAT format, ComPtr<ID3D12Resource>& out) -> bool {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(simulationResolution);
        desc.Height = static_cast<UINT>(simulationResolution);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        return SUCCEEDED(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&out)));
    };
    if (!makeTexture(DXGI_FORMAT_R32_FLOAT, g_gpuMeshPreview.displacementHeightTexture))
    {
        if (error) *error = "Create displacement height texture failed";
        return false;
    }
    if (!makeTexture(DXGI_FORMAT_R32_FLOAT, g_gpuMeshPreview.displacementMaskTexture))
    {
        if (error) *error = "Create displacement mask texture failed";
        return false;
    }

    if (!g_gpuMeshPreview.displacementSrvAllocated)
    {
        AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.displacementHeightSrvCpu, &g_gpuMeshPreview.displacementHeightSrvGpu);
        AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.displacementMaskSrvCpu, &g_gpuMeshPreview.displacementMaskSrvGpu);
        g_gpuMeshPreview.displacementSrvAllocated = true;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_gpuMeshPreview.displacementHeightTexture.Get(), &srvDesc, g_gpuMeshPreview.displacementHeightSrvCpu);
    g_device->CreateShaderResourceView(g_gpuMeshPreview.displacementMaskTexture.Get(), &srvDesc, g_gpuMeshPreview.displacementMaskSrvCpu);

    g_gpuMeshPreview.displacementTextureResolution = simulationResolution;
    g_gpuMeshPreview.displacementHeightUploadKey = 0;
    return true;
}

// Copy heights and mask from the CPU heightfield into the GPU textures.
// Uses an upload buffer + CopyTextureRegion. Skipped when the cached key
// matches (no new evaluation has happened).
bool UploadDisplacementHeightfield(ID3D12GraphicsCommandList* commandList, const rock::HeightfieldGrid& grid, uint64_t graphVersion, std::string* error)
{
    if (!g_gpuMeshPreview.displacementHeightTexture || !g_gpuMeshPreview.displacementMaskTexture)
    {
        if (error) *error = "Displacement textures not allocated";
        return false;
    }
    if (g_gpuMeshPreview.displacementHeightUploadKey == graphVersion && graphVersion != 0)
    {
        return true; // already up to date
    }
    const int n = g_gpuMeshPreview.displacementTextureResolution;
    if (grid.resolution != n)
    {
        // Resolution mismatch — caller should have reallocated. Return ok
        // without uploading; the texture stays as-is.
        return true;
    }

    const UINT64 rowPitch = (static_cast<UINT64>(n) * sizeof(float) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 totalBytes = rowPitch * static_cast<UINT64>(n);

    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = totalBytes * 2; // height + mask in one buffer
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    HRESULT hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr)) { if (error) *error = "Create displacement upload buffer failed"; return false; }

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    uploadBuffer->Map(0, &readRange, &mapped);
    auto writeChannel = [&](const std::vector<float>& src, UINT64 offset) {
        const float* base = src.empty() ? nullptr : src.data();
        for (int z = 0; z < n; ++z)
        {
            void* row = static_cast<char*>(mapped) + offset + static_cast<UINT64>(z) * rowPitch;
            if (base) std::memcpy(row, base + static_cast<size_t>(z) * static_cast<size_t>(n), static_cast<size_t>(n) * sizeof(float));
            else std::memset(row, 0, static_cast<size_t>(n) * sizeof(float));
        }
    };
    writeChannel(grid.heights, 0);
    writeChannel(grid.mask, totalBytes);
    uploadBuffer->Unmap(0, nullptr);

    auto copyTo = [&](ComPtr<ID3D12Resource>& tex, UINT64 srcOffset) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = srcOffset;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
        src.PlacedFootprint.Footprint.Width = static_cast<UINT>(n);
        src.PlacedFootprint.Footprint.Height = static_cast<UINT>(n);
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };

    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = g_gpuMeshPreview.displacementHeightTexture.Get();
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1] = barriers[0];
    barriers[1].Transition.pResource = g_gpuMeshPreview.displacementMaskTexture.Get();
    // First-time upload: resources are already in COPY_DEST. Skip the
    // transition then.
    if (g_gpuMeshPreview.displacementHeightUploadKey != 0)
    {
        commandList->ResourceBarrier(2, barriers);
    }

    copyTo(g_gpuMeshPreview.displacementHeightTexture, 0);
    copyTo(g_gpuMeshPreview.displacementMaskTexture, totalBytes);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(2, barriers);

    g_gpuMeshPreview.displacementHeightUploadKey = graphVersion;
    // Defer-release: keep the upload buffer alive until command list completes.
    // The simplest safe approach in this codebase is to attach to a list of
    // pending releases; for now, leaking a single ~16MB buffer per
    // evaluation is unacceptable. Use a static holder keyed to the next
    // fence wait.
    // TODO: integrate with a proper resource defer-release list.
    static ComPtr<ID3D12Resource> s_keepAlive;
    s_keepAlive = uploadBuffer;
    (void)s_keepAlive;

    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE OffsetCpuSrv(D3D12_CPU_DESCRIPTOR_HANDLE base, int offset)
{
    base.ptr += static_cast<SIZE_T>(offset) * g_srvDescriptorSize;
    return base;
}

D3D12_GPU_DESCRIPTOR_HANDLE OffsetGpuSrv(D3D12_GPU_DESCRIPTOR_HANDLE base, int offset)
{
    base.ptr += static_cast<UINT64>(offset) * g_srvDescriptorSize;
    return base;
}

bool EnsureMeshResourceTable(std::string* error)
{
    if (g_gpuMeshPreview.meshResourceTableAllocated)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device not initialized";
        return false;
    }
    try
    {
        AllocateSrvDescriptorRange(8, &g_gpuMeshPreview.meshResourceTableCpu, &g_gpuMeshPreview.meshResourceTableGpu);
        g_gpuMeshPreview.meshResourceTableAllocated = true;
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

bool UploadColorizeTexture(ID3D12GraphicsCommandList* commandList, const rock::ColorGrid& colorGrid, uint64_t graphVersion, std::string* error)
{
    if (colorGrid.resolution < 2 ||
        colorGrid.pixels.size() < static_cast<size_t>(colorGrid.resolution) * static_cast<size_t>(colorGrid.resolution) * 4u)
    {
        return true;
    }
    if (!EnsureMeshResourceTable(error))
    {
        return false;
    }
    if (g_gpuMeshPreview.colorizeTextureUploadKey == graphVersion && graphVersion != 0)
    {
        return true;
    }

    const int n = colorGrid.resolution;
    if (!g_gpuMeshPreview.colorizeTexture || g_gpuMeshPreview.colorizeTextureResolution != n)
    {
        g_gpuMeshPreview.colorizeTexture.Reset();
        const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(static_cast<UINT>(n), static_cast<UINT>(n), DXGI_FORMAT_R8G8B8A8_UNORM);
        HRESULT hr = g_device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&g_gpuMeshPreview.colorizeTexture));
        if (FAILED(hr))
        {
            if (error) *error = "Create Colorize texture failed";
            return false;
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(
            g_gpuMeshPreview.colorizeTexture.Get(), &srvDesc,
            OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, 4));

        g_gpuMeshPreview.colorizeTextureResolution = n;
        g_gpuMeshPreview.colorizeTextureUploadKey = 0;
    }

    const UINT64 rowPitch = (static_cast<UINT64>(n) * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    const UINT64 totalBytes = rowPitch * static_cast<UINT64>(n);

    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalBytes;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> uploadBuffer;
    HRESULT hr = g_device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr))
    {
        if (error) *error = "Create Colorize upload buffer failed";
        return false;
    }

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    uploadBuffer->Map(0, &readRange, &mapped);
    for (int z = 0; z < n; ++z)
    {
        void* row = static_cast<char*>(mapped) + static_cast<UINT64>(z) * rowPitch;
        const uint8_t* src = colorGrid.pixels.data() + static_cast<size_t>(z) * static_cast<size_t>(n) * 4u;
        std::memcpy(row, src, static_cast<size_t>(n) * 4u);
    }
    uploadBuffer->Unmap(0, nullptr);

    if (g_gpuMeshPreview.colorizeTextureUploadKey != 0)
    {
        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = g_gpuMeshPreview.colorizeTexture.Get();
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        commandList->ResourceBarrier(1, &toCopy);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_gpuMeshPreview.colorizeTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = uploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(n);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(n);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);
    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = g_gpuMeshPreview.colorizeTexture.Get();
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &toSrv);

    g_gpuMeshPreview.colorizeTextureUploadKey = graphVersion;
    static ComPtr<ID3D12Resource> s_colorUploadKeepAlive;
    s_colorUploadKeepAlive = uploadBuffer;
    (void)s_colorUploadKeepAlive;
    return true;
}

void UpdateMeshResourceTable(D3D12_GPU_DESCRIPTOR_HANDLE cloudShadowGpu)
{
    if (!g_gpuMeshPreview.meshResourceTableAllocated)
    {
        return;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE src[] = {
        g_gpuMeshPreview.shadowSrvCpu,
        (cloudShadowGpu.ptr == g_gpuClouds.shadowSrvGpu.ptr && g_gpuClouds.shadowSrvAllocated)
            ? g_gpuClouds.shadowSrvCpu
            : g_gpuClouds.dummyShadowSrvCpu,
        g_gpuMeshPreview.displacementHeightSrvCpu.ptr ? g_gpuMeshPreview.displacementHeightSrvCpu : g_gpuClouds.dummyShadowSrvCpu,
        g_gpuMeshPreview.displacementMaskSrvCpu.ptr ? g_gpuMeshPreview.displacementMaskSrvCpu : g_gpuClouds.dummyShadowSrvCpu,
        g_gpuClouds.dummyShadowSrvCpu,  // slot 4: colorize (fallback)
        g_gpuMeshPreview.aoSrvAllocated && g_gpuMeshPreview.aoTexture
            ? g_gpuMeshPreview.aoSrvCpu
            : g_gpuClouds.dummyShadowSrvCpu,  // slot 5: AO
        g_gpuMeshPreview.postSrvAllocated && g_gpuMeshPreview.postTarget
            ? g_gpuMeshPreview.postSrvCpu
            : g_gpuClouds.dummyShadowSrvCpu,  // slot 6: scene color before water
        g_gpuMeshPreview.sceneDepthSrvAllocated && g_gpuMeshPreview.sceneDepthTarget
            ? g_gpuMeshPreview.sceneDepthSrvCpu
            : g_gpuClouds.dummyShadowSrvCpu,  // slot 7: scene depth before water
    };
    for (int i = 0; i < 4; ++i)
    {
        g_device->CopyDescriptorsSimple(1, OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, i), src[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!g_gpuMeshPreview.colorizeTexture)
    {
        g_device->CopyDescriptorsSimple(1, OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, 4), src[4], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    g_device->CopyDescriptorsSimple(1, OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, 5), src[5], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_device->CopyDescriptorsSimple(1, OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, 6), src[6], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_device->CopyDescriptorsSimple(1, OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, 7), src[7], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}


// =============================================================================
// Horizon AO コンピュートパイプライン
// =============================================================================

bool EnsureAOComputePipeline(std::string* error)
{
    if (g_aoComputeReady && g_aoComputeRootSignature && g_aoComputePso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 4;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                             rsDesc,
                                             g_aoComputeRootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create AO root sig failed";
        return false;
    }

    const std::filesystem::path shaderPath = AOComputeShaderPath();
    const UINT compileFlags = DefaultShaderCompileFlags();
    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                 "CSAmbientOcclusion", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile AO shader failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_aoComputeRootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    hr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_aoComputePso));
    if (FAILED(hr)) { if (error) *error = "Create AO PSO failed"; return false; }

    g_aoComputeReady = true;
    return true;
}

bool EnsureAOTexture(int resolution, std::string* error)
{
    if (g_gpuMeshPreview.aoTexture && g_gpuMeshPreview.aoTextureResolution == resolution)
    {
        return true;
    }
    if (!g_device) { if (error) *error = "D3D12 device not available"; return false; }

    g_gpuMeshPreview.aoTexture.Reset();
    g_gpuMeshPreview.aoTextureResolution = resolution;
    g_gpuMeshPreview.aoUploadKey = 0;

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
        static_cast<UINT>(resolution), static_cast<UINT>(resolution),
        DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&g_gpuMeshPreview.aoTexture));
    if (FAILED(hr)) { if (error) *error = "Create AO texture failed"; return false; }
    g_gpuMeshPreview.aoTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (!g_gpuMeshPreview.aoSrvAllocated)
    {
        AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.aoSrvCpu, &g_gpuMeshPreview.aoSrvGpu);
        g_gpuMeshPreview.aoSrvAllocated = true;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_gpuMeshPreview.aoTexture.Get(), &srvDesc, g_gpuMeshPreview.aoSrvCpu);

    if (!g_gpuMeshPreview.aoUavAllocated)
    {
        AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.aoUavCpu, &g_gpuMeshPreview.aoUavGpu);
        g_gpuMeshPreview.aoUavAllocated = true;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_gpuMeshPreview.aoTexture.Get(), nullptr, &uavDesc, g_gpuMeshPreview.aoUavCpu);

    return true;
}

// ハイトフィールドテクスチャから AO をコンピュートシェーダーで生成する。
// commandList は g_srvHeap がバインド済みであることを前提とする。
bool DispatchAOCompute(ID3D12GraphicsCommandList* commandList,
                       int resolution, float terrainSizeMeters,
                       float maxDistanceMeters, uint64_t graphVersion,
                       std::string* error)
{
    // サンプル半径が変わった場合もキャッシュを無効化する
    if (g_gpuMeshPreview.aoCachedRadius != maxDistanceMeters)
    {
        g_gpuMeshPreview.aoUploadKey = 0;
        g_gpuMeshPreview.aoCachedRadius = maxDistanceMeters;
    }
    if (g_gpuMeshPreview.aoUploadKey == graphVersion && graphVersion != 0)
    {
        return true;
    }
    if (!EnsureAOComputePipeline(error))
    {
        return false;
    }
    if (!EnsureAOTexture(resolution, error))
    {
        return false;
    }
    if (!g_gpuMeshPreview.displacementHeightSrvGpu.ptr)
    {
        return false;
    }

    if (g_gpuMeshPreview.aoTextureState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_gpuMeshPreview.aoTexture.Get();
        b.Transition.StateBefore = g_gpuMeshPreview.aoTextureState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &b);
        g_gpuMeshPreview.aoTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    struct AOShaderConstants
    {
        UINT  resolution;
        float worldDX;
        float maxDistanceMeters;
        float pad0;
    };
    AOShaderConstants aoConsts{};
    aoConsts.resolution = static_cast<UINT>(resolution);
    aoConsts.worldDX = (resolution > 1)
        ? terrainSizeMeters / static_cast<float>(resolution - 1)
        : 1.0f;
    aoConsts.maxDistanceMeters = std::max(1.0f, maxDistanceMeters);

    commandList->SetComputeRootSignature(g_aoComputeRootSignature.Get());
    commandList->SetPipelineState(g_aoComputePso.Get());
    commandList->SetComputeRoot32BitConstants(0, 4, &aoConsts, 0);
    commandList->SetComputeRootDescriptorTable(1, g_gpuMeshPreview.displacementHeightSrvGpu);
    commandList->SetComputeRootDescriptorTable(2, g_gpuMeshPreview.aoUavGpu);

    const UINT groupCount = (static_cast<UINT>(resolution) + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = g_gpuMeshPreview.aoTexture.Get();
    commandList->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = g_gpuMeshPreview.aoTexture.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toSrv);
    g_gpuMeshPreview.aoTextureState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    g_gpuMeshPreview.aoUploadKey = graphVersion;
    return true;
}

// Mirrors the cbuffer in shaders/sky.hlsl. Packed manually to match HLSL's
// 16-byte alignment rules so we can splat it as 32-bit root constants.
struct SkyShaderConstants
{
    float cameraRight[4];
    float cameraUp[4];
    float cameraForward[4];
    float projScaleX;
    float projScaleY;
    float panNdcX;
    float panNdcY;
    float sunDirection[4];
    float atmosphereDensity;
    float mieStrength;
    float mieEccentricity;
    float sunSize;
    float sunGlowStrength;
    float pad0;
    float pad1;
    float pad2;
    float groundAlbedo[4];
};
static_assert(sizeof(SkyShaderConstants) == 32 * sizeof(UINT), "SkyShaderConstants must be 32 DWORDs");

struct DepthOfFieldShaderConstants
{
    float focusDistance;
    float focalLengthMm;
    float fStop;
    float sensorHeightMm;
    float maxBlurPixels;
    float nearPlane;
    float farPlane;
    float apertureShape;
    float apertureBlades;
    float apertureRotationRadians;
    float highlightBoost;
    float miniatureScale;
};
static_assert(sizeof(DepthOfFieldShaderConstants) == 12 * sizeof(UINT));

struct TonemapShaderConstants
{
    float exposureMode;
    float exposureEv;
    float autoExposureBiasEv;
    float autoExposureMinEv;
    float autoExposureMaxEv;
    float adaptationRate;
    float deltaTimeSeconds;
    float colorTemperatureKelvin;
};
static_assert(sizeof(TonemapShaderConstants) == 8 * sizeof(UINT));

bool EnsureDepthOfFieldPipeline(std::string* error)
{
    const DXGI_FORMAT targetFormat = MeshPreviewColorFormat();
    if (g_dofPipelineReady && g_dofRootSignature && g_dofPso && g_dofPipelineFormat == targetFormat)
    {
        return true;
    }
    if (g_dofPipelineFormat != DXGI_FORMAT_UNKNOWN && g_dofPipelineFormat != targetFormat)
    {
        g_dofPso.Reset();
        g_dofRootSignature.Reset();
        g_dofPipelineReady = false;
        g_dofPipelineFormat = DXGI_FORMAT_UNKNOWN;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_dofPipelineStatus = "Depth of Field pipeline unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE colorRange{};
    colorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    colorRange.NumDescriptors = 1;
    colorRange.BaseShaderRegister = 0;
    colorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE depthRange{};
    depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    depthRange.NumDescriptors = 1;
    depthRange.BaseShaderRegister = 1;
    depthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.Num32BitValues = 12;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &colorRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &depthRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                             rsDesc,
                                             g_dofRootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create DOF root signature failed";
        g_dofPipelineStatus = "Depth of Field root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = DepthOfFieldShaderPath();
    const UINT compileFlags = DefaultShaderCompileFlags();

    auto compileEntry = [&](const char* entryPoint, const char* target, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, target, compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile DOF shader failed";
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!compileEntry("DofVS", "vs_5_0", vsBlob)) { g_dofPipelineStatus = "Depth of Field VS compile failed"; return false; }
    if (!compileEntry("DofPS", "ps_5_0", psBlob)) { g_dofPipelineStatus = "Depth of Field PS compile failed"; return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_dofRootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = targetFormat;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_dofPso));
    if (FAILED(hr))
    {
        if (error) *error = "Create DOF PSO failed";
        g_dofPipelineStatus = "Depth of Field PSO failed";
        return false;
    }

    g_dofPipelineReady = true;
    g_dofPipelineStatus = "Depth of Field pipeline ready";
    g_dofPipelineFormat = targetFormat;
    return true;
}

bool EnsureTonemapPipeline(std::string* error)
{
    if (g_tonemapPipelineReady && g_tonemapRootSignature && g_exposurePso && g_tonemapPso && g_colorGradePso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_tonemapPipelineStatus = "Tonemap pipeline unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE colorRange{};
    colorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    colorRange.NumDescriptors = 1;
    colorRange.BaseShaderRegister = 0;
    colorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE exposureRange{};
    exposureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    exposureRange.NumDescriptors = 1;
    exposureRange.BaseShaderRegister = 1;
    exposureRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.Num32BitValues = 8;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &colorRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &exposureRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                             rsDesc,
                                             g_tonemapRootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create tonemap root signature failed";
        g_tonemapPipelineStatus = "Tonemap root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = TonemapShaderPath();
    const UINT compileFlags = DefaultShaderCompileFlags();

    auto compileEntry = [&](const char* entryPoint, const char* target, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, target, compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile tonemap shader failed";
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob, exposurePsBlob, psBlob, colorGradePsBlob;
    if (!compileEntry("TonemapVS", "vs_5_0", vsBlob)) { g_tonemapPipelineStatus = "Tonemap VS compile failed"; return false; }
    if (!compileEntry("ExposurePS", "ps_5_0", exposurePsBlob)) { g_tonemapPipelineStatus = "Exposure PS compile failed"; return false; }
    if (!compileEntry("TonemapPS", "ps_5_0", psBlob)) { g_tonemapPipelineStatus = "Tonemap PS compile failed"; return false; }
    if (!compileEntry("ColorGradePS", "ps_5_0", colorGradePsBlob)) { g_tonemapPipelineStatus = "Color grade PS compile failed"; return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_tonemapRootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {exposurePsBlob->GetBufferPointer(), exposurePsBlob->GetBufferSize()};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16_FLOAT;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_exposurePso));
    if (FAILED(hr))
    {
        if (error) *error = "Create exposure PSO failed";
        g_tonemapPipelineStatus = "Exposure PSO failed";
        return false;
    }

    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_tonemapPso));
    if (FAILED(hr))
    {
        if (error) *error = "Create tonemap PSO failed";
        g_tonemapPipelineStatus = "Tonemap PSO failed";
        return false;
    }

    psoDesc.PS = {colorGradePsBlob->GetBufferPointer(), colorGradePsBlob->GetBufferSize()};
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_colorGradePso));
    if (FAILED(hr))
    {
        if (error) *error = "Create color grade PSO failed";
        g_tonemapPipelineStatus = "Color grade PSO failed";
        return false;
    }

    g_tonemapPipelineReady = true;
    g_tonemapPipelineStatus = "Tonemap pipeline ready";
    return true;
}

bool EnsureSkyPipeline(std::string* error)
{
    const DXGI_FORMAT targetFormat = MeshPreviewColorFormat();
    if (g_skyPipelineReady && g_skyRootSignature && g_skyPso && g_skyPipelineFormat == targetFormat)
    {
        return true;
    }
    if (g_skyPipelineFormat != DXGI_FORMAT_UNKNOWN && g_skyPipelineFormat != targetFormat)
    {
        g_skyPso.Reset();
        g_skyRootSignature.Reset();
        g_skyPipelineReady = false;
        g_skyPipelineFormat = DXGI_FORMAT_UNKNOWN;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_skyPipelineStatus = "Sky pipeline unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE lutRange{};
    lutRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    lutRange.NumDescriptors = 1;
    lutRange.BaseShaderRegister = 0;
    lutRange.RegisterSpace = 0;
    lutRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 32;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &lutRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC lutSampler{};
    lutSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    lutSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    lutSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    lutSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    lutSampler.ShaderRegister = 0;
    lutSampler.RegisterSpace = 0;
    lutSampler.MaxLOD = D3D12_FLOAT32_MAX;
    lutSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &lutSampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                             rsDesc,
                                             g_skyRootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create sky root signature failed";
        g_skyPipelineStatus = "Sky root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = SkyShaderPath();
    const UINT compileFlags = DefaultShaderCompileFlags();

    auto compileEntry = [&](const char* entryPoint, const char* target, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, target, compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile sky shader failed";
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!compileEntry("SkyVS", "vs_5_0", vsBlob)) { g_skyPipelineStatus = "Sky VS compile failed"; return false; }
    if (!compileEntry("SkyPS", "ps_5_0", psBlob)) { g_skyPipelineStatus = "Sky PS compile failed"; return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_skyRootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = targetFormat;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    HRESULT psoHr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_skyPso));
    if (FAILED(psoHr))
    {
        if (error) *error = "Create sky PSO failed";
        g_skyPipelineStatus = "Sky PSO failed";
        return false;
    }

    g_skyPipelineReady = true;
    g_skyPipelineStatus = "Sky pipeline ready";
    g_skyPipelineFormat = targetFormat;
    return true;
}

// 32×32 R16G16B16A16_FLOAT multi-scatter LUT, regenerated only when the
// Mie atmospheric parameters change.
static constexpr UINT kAtmMultiScatterResolution = 32u;

bool EnsureAtmosphereMultiScatterPipeline(std::string* error)
{
    if (g_atmosphereMultiScatterPso && g_atmosphereMultiScatterRootSignature)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.Num32BitValues = 4;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;

    ComPtr<ID3DBlob> errBlob;
    HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                             rsDesc,
                                             g_atmosphereMultiScatterRootSignature.ReleaseAndGetAddressOf(),
                                             errBlob.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create multi-scatter root signature failed"; return false; }

    const UINT compileFlags = DefaultShaderCompileFlags();

    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    const std::filesystem::path shaderPath = AtmosphereMultiScatterShaderPath();
    HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                            "CSGenerate", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile multi-scatter shader failed"; return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_atmosphereMultiScatterRootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    hr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_atmosphereMultiScatterPso));
    if (FAILED(hr)) { if (error) *error = "Create multi-scatter PSO failed"; return false; }
    return true;
}

bool EnsureAtmosphereMultiScatterLut(float density, float mieStrength, float mieEccentricity, std::string* error)
{
    if (!EnsureAtmosphereMultiScatterPipeline(error)) return false;

    if (!g_atmosphereMultiScatterTexture)
    {
        D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(kAtmMultiScatterResolution, kAtmMultiScatterResolution,
                                                         DXGI_FORMAT_R16G16B16A16_FLOAT,
                                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&g_atmosphereMultiScatterTexture));
        if (FAILED(hr)) { if (error) *error = "Create multi-scatter texture failed"; return false; }
        g_atmosphereMultiScatterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        g_atmosphereMultiScatterReady = false;

        if (!g_atmosphereMultiScatterSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_atmosphereMultiScatterSrvCpu, &g_atmosphereMultiScatterSrvGpu);
            g_atmosphereMultiScatterSrvAllocated = true;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(g_atmosphereMultiScatterTexture.Get(), &srvDesc, g_atmosphereMultiScatterSrvCpu);
    }

    if (g_atmosphereMultiScatterReady &&
        g_atmosphereCachedDensity == density &&
        g_atmosphereCachedMie == mieStrength &&
        g_atmosphereCachedMieG == mieEccentricity)
    {
        return true;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Multi-scatter allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Multi-scatter CL failed");

    if (g_atmosphereMultiScatterState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_atmosphereMultiScatterTexture.Get();
        b.Transition.StateBefore = g_atmosphereMultiScatterState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &b);
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc =
        ShaderVisibleCbvSrvUavDescriptorHeapDesc(1);
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap));
    if (FAILED(hr)) { if (error) *error = "Create multi-scatter UAV heap failed"; return false; }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_atmosphereMultiScatterTexture.Get(), nullptr, &uavDesc,
                                         uavHeap->GetCPUDescriptorHandleForHeapStart());

    struct MultiScatterConstants
    {
        float atmosphereDensity;
        float mieStrength;
        float mieEccentricity;
        float pad0;
    };
    MultiScatterConstants mc{};
    mc.atmosphereDensity = density;
    mc.mieStrength = mieStrength;
    mc.mieEccentricity = mieEccentricity;

    ID3D12DescriptorHeap* heaps[] = {uavHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_atmosphereMultiScatterRootSignature.Get());
    commandList->SetPipelineState(g_atmosphereMultiScatterPso.Get());
    commandList->SetComputeRoot32BitConstants(0, 4, &mc, 0);
    commandList->SetComputeRootDescriptorTable(1, uavHeap->GetGPUDescriptorHandleForHeapStart());
    const UINT groupCount = (kAtmMultiScatterResolution + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = g_atmosphereMultiScatterTexture.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toSrv);
    g_atmosphereMultiScatterState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    ThrowIfFailed(commandList->Close(), "Close multi-scatter CL failed");
    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal multi-scatter fence failed");
    WaitForFenceValue(fenceValue);

    g_atmosphereCachedDensity = density;
    g_atmosphereCachedMie = mieStrength;
    g_atmosphereCachedMieG = mieEccentricity;
    g_atmosphereMultiScatterReady = true;
    return true;
}

// CPU port of the atmospheric scattering model in shaders/atmosphere.hlsli.
// Used to sample 4 representative directions per frame and feed those to
// the mesh / cloud shaders so terrain ambient and cloud lighting stay in
// lockstep with the rendered sky. Constants must match the HLSL file.
namespace atmosphere_cpu
{
constexpr float kEarthRadius      = 6360e3f;
constexpr float kAtmosphereRadius = 6420e3f;
constexpr float kHeightR          = 7994.0f;
constexpr float kHeightM          = 1200.0f;
constexpr float kBetaR[3]         = {5.802e-6f, 13.558e-6f, 33.1e-6f};
constexpr float kBetaM            = 21e-6f;
constexpr float kSunIntensity     = 22.0f;
constexpr float kCameraHeight     = 500.0f;
constexpr int   kNumViewSteps     = 32;
constexpr int   kNumSunSteps      = 8;

struct Vec3 { float x, y, z; };
inline float Dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
inline Vec3 Scale(const Vec3& v, float s) { return {v.x*s, v.y*s, v.z*s}; }

struct Hit { float tNear, tFar; bool hit; };
inline Hit RaySphere(const Vec3& origin, const Vec3& dir, float radius)
{
    float b = Dot(origin, dir);
    float c = Dot(origin, origin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0f) return {0.0f, 0.0f, false};
    float sq = std::sqrt(d);
    return {-b - sq, -b + sq, true};
}

inline std::array<float, 3> ComputeScattering(const Vec3& viewDir, const Vec3& sunDir,
                                              float density, float mieStrength, float mieG)
{
    Vec3 origin = {0.0f, kEarthRadius + kCameraHeight, 0.0f};

    Hit atmHit = RaySphere(origin, viewDir, kAtmosphereRadius);
    if (!atmHit.hit || atmHit.tFar <= 0.0f) return {0.0f, 0.0f, 0.0f};
    float marchEnd = atmHit.tFar;

    Hit earthHit = RaySphere(origin, viewDir, kEarthRadius);
    if (earthHit.hit && earthHit.tNear > 0.0f) marchEnd = std::min(marchEnd, earthHit.tNear);

    float stepLen = marchEnd / static_cast<float>(kNumViewSteps);
    float opticalR = 0.0f;
    float opticalM = 0.0f;
    float sumR[3] = {0.0f, 0.0f, 0.0f};
    float sumM[3] = {0.0f, 0.0f, 0.0f};

    for (int i = 0; i < kNumViewSteps; ++i)
    {
        float t = (static_cast<float>(i) + 0.5f) * stepLen;
        Vec3 p = Add(origin, Scale(viewDir, t));
        float h = Length(p) - kEarthRadius;
        if (h < 0.0f) break;

        float dR = std::exp(-h / kHeightR) * stepLen;
        float dM = std::exp(-h / kHeightM) * stepLen;
        opticalR += dR;
        opticalM += dM;

        Hit sunHit = RaySphere(p, sunDir, kAtmosphereRadius);
        if (!sunHit.hit || sunHit.tFar <= 0.0f) continue;
        float sunStep = sunHit.tFar / static_cast<float>(kNumSunSteps);
        float sunR = 0.0f;
        float sunM = 0.0f;
        bool sunBlocked = false;
        for (int j = 0; j < kNumSunSteps; ++j)
        {
            float st = (static_cast<float>(j) + 0.5f) * sunStep;
            Vec3 sp = Add(p, Scale(sunDir, st));
            float sh = Length(sp) - kEarthRadius;
            if (sh < 0.0f) { sunBlocked = true; break; }
            sunR += std::exp(-sh / kHeightR) * sunStep;
            sunM += std::exp(-sh / kHeightM) * sunStep;
        }
        if (sunBlocked) continue;

        const float bR0 = kBetaR[0] * density;
        const float bR1 = kBetaR[1] * density;
        const float bR2 = kBetaR[2] * density;
        const float bM  = kBetaM * mieStrength;
        float tau[3];
        tau[0] = bR0 * (opticalR + sunR) + bM * 1.1f * (opticalM + sunM);
        tau[1] = bR1 * (opticalR + sunR) + bM * 1.1f * (opticalM + sunM);
        tau[2] = bR2 * (opticalR + sunR) + bM * 1.1f * (opticalM + sunM);
        float atten[3] = {std::exp(-tau[0]), std::exp(-tau[1]), std::exp(-tau[2])};
        sumR[0] += atten[0] * dR; sumR[1] += atten[1] * dR; sumR[2] += atten[2] * dR;
        sumM[0] += atten[0] * dM; sumM[1] += atten[1] * dM; sumM[2] += atten[2] * dM;
    }

    float cosTheta = Dot(viewDir, sunDir);
    float phaseR = 0.0596831f * (1.0f + cosTheta * cosTheta);
    float gg = mieG * mieG;
    float phaseM = 0.0795775f * (1.0f - gg) /
                   std::pow(std::max(1.0f + gg - 2.0f * mieG * cosTheta, 1e-6f), 1.5f);

    const float bM = kBetaM * mieStrength;
    return {
        kSunIntensity * (sumR[0] * kBetaR[0] * density * phaseR + sumM[0] * bM * phaseM),
        kSunIntensity * (sumR[1] * kBetaR[1] * density * phaseR + sumM[1] * bM * phaseM),
        kSunIntensity * (sumR[2] * kBetaR[2] * density * phaseR + sumM[2] * bM * phaseM),
    };
}

inline std::array<float, 3> ComputeSunTransmittance(const Vec3& sunDir, float density, float mieStrength)
{
    Vec3 origin = {0.0f, kEarthRadius + kCameraHeight, 0.0f};
    Hit hit = RaySphere(origin, sunDir, kAtmosphereRadius);
    if (!hit.hit || hit.tFar <= 0.0f) return {0.0f, 0.0f, 0.0f};
    float stepLen = hit.tFar / static_cast<float>(kNumSunSteps);
    float opticalR = 0.0f;
    float opticalM = 0.0f;
    for (int j = 0; j < kNumSunSteps; ++j)
    {
        float st = (static_cast<float>(j) + 0.5f) * stepLen;
        Vec3 sp = Add(origin, Scale(sunDir, st));
        float sh = Length(sp) - kEarthRadius;
        if (sh < 0.0f) return {0.0f, 0.0f, 0.0f};
        opticalR += std::exp(-sh / kHeightR) * stepLen;
        opticalM += std::exp(-sh / kHeightM) * stepLen;
    }
    return {
        std::exp(-(kBetaR[0] * density * opticalR + kBetaM * mieStrength * 1.1f * opticalM)),
        std::exp(-(kBetaR[1] * density * opticalR + kBetaM * mieStrength * 1.1f * opticalM)),
        std::exp(-(kBetaR[2] * density * opticalR + kBetaM * mieStrength * 1.1f * opticalM)),
    };
}
} // namespace atmosphere_cpu

// Effective lighting colours derived from the atmospheric model for one
// sun direction. Sampled once per frame and pushed to mesh / cloud
// shaders so the whole scene is internally consistent.
struct AtmosphereSamples
{
    std::array<float, 3> zenith;
    std::array<float, 3> horizon;
    std::array<float, 3> ground;
    std::array<float, 3> sun;
};

AtmosphereSamples SampleAtmosphericEnvironment(const rock::SkySettings& sky,
                                               float sunDirX, float sunDirY, float sunDirZ)
{
    using atmosphere_cpu::Vec3;
    Vec3 sun{sunDirX, sunDirY, sunDirZ};

    const float density = std::clamp(sky.atmosphereDensity, 0.05f, 8.0f);
    const float mie = std::clamp(sky.mieStrength, 0.0f, 8.0f);
    const float mieG = std::clamp(sky.mieEccentricity, -0.99f, 0.99f);

    Vec3 up{0.0f, 1.0f, 0.0f};

    // Horizon view perpendicular to the sun in the XZ plane: gives a
    // colour that's a balanced average of the warm and cool sides.
    Vec3 sunHorizontal = {sun.x, 0.0f, sun.z};
    float lenH = std::sqrt(sunHorizontal.x * sunHorizontal.x + sunHorizontal.z * sunHorizontal.z);
    Vec3 horizonDir;
    if (lenH > 1e-4f)
    {
        sunHorizontal.x /= lenH; sunHorizontal.z /= lenH;
        // perpendicular in XZ plane (rotate 90°)
        horizonDir = {-sunHorizontal.z, 0.05f, sunHorizontal.x};
        float ln = std::sqrt(horizonDir.x * horizonDir.x + horizonDir.y * horizonDir.y + horizonDir.z * horizonDir.z);
        horizonDir = {horizonDir.x / ln, horizonDir.y / ln, horizonDir.z / ln};
    }
    else
    {
        horizonDir = {1.0f, 0.05f, 0.0f};
    }

    AtmosphereSamples out{};
    out.zenith  = atmosphere_cpu::ComputeScattering(up, sun, density, mie, mieG);
    out.horizon = atmosphere_cpu::ComputeScattering(horizonDir, sun, density, mie, mieG);

    // Ground = albedo × upward-hemisphere irradiance approximation.
    // Simple heuristic: 0.6 × zenith + 0.4 × horizon, multiplied by the
    // user-tunable groundAlbedo. This is what surfaces facing down "see"
    // reflected back up to them.
    std::array<float, 3> avgSky = {
        out.zenith[0] * 0.6f + out.horizon[0] * 0.4f,
        out.zenith[1] * 0.6f + out.horizon[1] * 0.4f,
        out.zenith[2] * 0.6f + out.horizon[2] * 0.4f,
    };
    out.ground = {
        avgSky[0] * sky.groundAlbedo[0],
        avgSky[1] * sky.groundAlbedo[1],
        avgSky[2] * sky.groundAlbedo[2],
    };

    // Sun colour as seen from the ground: white × atmospheric transmittance.
    out.sun = atmosphere_cpu::ComputeSunTransmittance(sun, density, mie);
    return out;
}

// Records a fullscreen sky pass into the supplied command list. Caller is
// responsible for: render target / viewport / scissor already bound, and
// re-binding any other root signatures it needs after the call returns.
bool RenderSkyPass(ID3D12GraphicsCommandList* commandList, const rock::SkySettings& sky, const SkyShaderConstants& base)
{
    if (sky.mode != rock::SkyMode::Atmospheric)
    {
        return false;
    }
    std::string ignoredError;
    if (!EnsureSkyPipeline(&ignoredError))
    {
        return false;
    }

    const float density = std::clamp(sky.atmosphereDensity, 0.05f, 8.0f);
    const float mieS = std::clamp(sky.mieStrength, 0.0f, 8.0f);
    const float mieG = std::clamp(sky.mieEccentricity, -0.99f, 0.99f);
    std::string lutError;
    const bool lutReady = EnsureAtmosphereMultiScatterLut(density, mieS, mieG, &lutError);
    if (!lutReady || !g_atmosphereMultiScatterSrvAllocated)
    {
        return false;
    }

    SkyShaderConstants constants = base;
    constants.atmosphereDensity = density;
    constants.mieStrength = mieS;
    constants.mieEccentricity = mieG;
    const float sunHalfAngleRad = std::clamp(sky.sunSizeDegrees, 0.05f, 30.0f) * 0.5f * 3.14159265358979323846f / 180.0f;
    constants.sunSize = std::cos(sunHalfAngleRad);
    constants.sunGlowStrength = std::clamp(sky.sunGlowStrength, 0.0f, 4.0f);
    constants.pad0 = constants.pad1 = constants.pad2 = 0.0f;
    constants.groundAlbedo[0] = sky.groundAlbedo[0];
    constants.groundAlbedo[1] = sky.groundAlbedo[1];
    constants.groundAlbedo[2] = sky.groundAlbedo[2];
    constants.groundAlbedo[3] = 1.0f;

    // The multi-scatter LUT SRV lives in g_srvHeap, so that heap must be
    // bound before SetGraphicsRootDescriptorTable. The mesh pass below
    // also sets it but we need it earlier for the sky.
    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(g_skyRootSignature.Get());
    commandList->SetPipelineState(g_skyPso.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
    commandList->SetGraphicsRootDescriptorTable(1, g_atmosphereMultiScatterSrvGpu);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Sky PSO has no input layout, so the IA stage ignores any VB/IB still
    // bound from the prior shadow pass. We leave them alone so the surface
    // draw below doesn't need to rebind the main mesh VB.
    commandList->DrawInstanced(3, 1, 0, 0);
    return true;
}

// 128^3 cloud density volume — small enough to regenerate quickly when params
// change (a few ms on the GPU) and big enough to give the noise some variety.
static constexpr UINT kCloudVolumeResolution = 128u;

struct CloudVolumeShaderConstants
{
    UINT  resolution;
    INT   seed;
    float pad0;
    float pad1;
};
static_assert(sizeof(CloudVolumeShaderConstants) == 4 * sizeof(UINT), "CloudVolumeShaderConstants must be 4 DWORDs");

struct CloudRenderShaderConstants
{
    float cameraPosition[4];
    float cameraRight[4];
    float cameraUp[4];
    float cameraForward[4];
    float projScaleX;
    float projScaleY;
    float panNdcX;
    float panNdcY;
    float sunDirection[4];
    float cloudColor[4];
    float altitudeMin;
    float altitudeMax;
    float horizontalScale;
    float coverage;
    float densityMultiplier;
    float absorption;
    float windOffsetX;
    float windOffsetZ;
    INT   qualitySamples;
    float nearPlane;
    float farPlane;
    float pad0;
    float fieldCenterX;
    float fieldCenterZ;
    float fieldRadius;
    float fieldFalloff;
    float atmosphereSunColor[4];
    float atmosphereSkyColor[4];
    INT   lightSamples;
    float lightStepMeters;
    float phaseEccentricity;
    float shadowAmbientStrength;
};
static_assert(sizeof(CloudRenderShaderConstants) == 56 * sizeof(UINT), "CloudRenderShaderConstants must be 56 DWORDs");

bool EnsureCloudPipelines(std::string* error)
{
    const DXGI_FORMAT targetFormat = MeshPreviewColorFormat();
    if (g_cloudPipelinesReady && g_cloudVolumePso && g_cloudVolumeRootSignature && g_cloudRenderPso && g_cloudRenderRootSignature &&
        g_cloudRenderPipelineFormat == targetFormat)
    {
        return true;
    }
    if (g_cloudRenderPipelineFormat != DXGI_FORMAT_UNKNOWN && g_cloudRenderPipelineFormat != targetFormat)
    {
        g_cloudVolumePso.Reset();
        g_cloudVolumeRootSignature.Reset();
        g_cloudRenderPso.Reset();
        g_cloudRenderRootSignature.Reset();
        g_cloudShadowPso.Reset();
        g_cloudShadowRootSignature.Reset();
        g_cloudPipelinesReady = false;
        g_cloudRenderPipelineFormat = DXGI_FORMAT_UNKNOWN;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_cloudPipelineStatus = "Cloud pipelines unavailable";
        return false;
    }

    const UINT compileFlags = DefaultShaderCompileFlags();

    // -------- Cloud volume compute pipeline --------
    {
        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParams[2]{};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[0].Constants.ShaderRegister = 0;
        rootParams[0].Constants.Num32BitValues = 4;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 2;
        rsDesc.pParameters = rootParams;

        ComPtr<ID3DBlob> errBlob;
        HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                                 rsDesc,
                                                 g_cloudVolumeRootSignature.ReleaseAndGetAddressOf(),
                                                 errBlob.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create cloud volume root sig failed"; g_cloudPipelineStatus = "Cloud volume root signature failed"; return false; }

        ComPtr<ID3DBlob> csBlob;
        errBlob.Reset();
        const std::filesystem::path shaderPath = CloudDensityShaderPath();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                      "CSGenerate", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
        if (FAILED(compileHr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile cloud volume shader failed"; g_cloudPipelineStatus = "Cloud volume shader compile failed"; return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_cloudVolumeRootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        hr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_cloudVolumePso));
        if (FAILED(hr)) { if (error) *error = "Create cloud volume PSO failed"; g_cloudPipelineStatus = "Cloud volume PSO failed"; return false; }
    }

    // -------- Cloud render graphics pipeline --------
    {
        // Two SRVs in the same descriptor range: t0 = cloud volume,
        // t1 = depth buffer. They live in the shared g_srvHeap but the
        // table only specifies a base GPU handle, so the caller must point
        // it at a heap region with both descriptors contiguous. We work
        // around the contiguous requirement by using two separate tables.
        D3D12_DESCRIPTOR_RANGE volumeRange{};
        volumeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        volumeRange.NumDescriptors = 1;
        volumeRange.BaseShaderRegister = 0;
        volumeRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE depthRange{};
        depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        depthRange.NumDescriptors = 1;
        depthRange.BaseShaderRegister = 1;
        depthRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParams[3]{};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[0].Constants.ShaderRegister = 0;
        rootParams[0].Constants.Num32BitValues = 56;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &volumeRange;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[2].DescriptorTable.pDescriptorRanges = &depthRange;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters = rootParams;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> errBlob;
        HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                                 rsDesc,
                                                 g_cloudRenderRootSignature.ReleaseAndGetAddressOf(),
                                                 errBlob.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create cloud render root sig failed"; g_cloudPipelineStatus = "Cloud render root signature failed"; return false; }

        const std::filesystem::path shaderPath = CloudRenderShaderPath();
        auto compileEntry = [&](const char* entryPoint, const char* target, ComPtr<ID3DBlob>& outBlob) -> bool {
            errBlob.Reset();
            const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                          entryPoint, target, compileFlags, 0, &outBlob, &errBlob);
            if (FAILED(compileHr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile cloud render shader failed"; return false; }
            return true;
        };
        ComPtr<ID3DBlob> vsBlob, psBlob;
        if (!compileEntry("CloudVS", "vs_5_0", vsBlob)) { g_cloudPipelineStatus = "Cloud VS compile failed"; return false; }
        if (!compileEntry("CloudPS", "ps_5_0", psBlob)) { g_cloudPipelineStatus = "Cloud PS compile failed"; return false; }

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_cloudRenderRootSignature.Get();
        psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
        psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.SampleDesc.Count = 1;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = targetFormat;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        // Premultiplied-alpha blend. The PS accumulates `lit * dA` over the
        // ray-march, which is already alpha-weighted (Σ dA = alpha), so the
        // GPU must NOT multiply by SRC_ALPHA again — that would crush thin
        // clouds darker than the sky behind them.
        D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        HRESULT psoHr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_cloudRenderPso));
        if (FAILED(psoHr)) { if (error) *error = "Create cloud render PSO failed"; g_cloudPipelineStatus = "Cloud render PSO failed"; return false; }
    }

    // -------- Cloud shadow compute pipeline --------
    {
        D3D12_DESCRIPTOR_RANGE volumeRange{};
        volumeRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        volumeRange.NumDescriptors = 1;
        volumeRange.BaseShaderRegister = 0;
        volumeRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_DESCRIPTOR_RANGE uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rootParams[3]{};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[0].Constants.ShaderRegister = 0;
        rootParams[0].Constants.Num32BitValues = 24;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &volumeRange;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ShaderRegister = 0;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters = 3;
        rsDesc.pParameters = rootParams;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> errBlob;
        HRESULT hr = CreateRootSignatureFromDesc(g_device.Get(),
                                                 rsDesc,
                                                 g_cloudShadowRootSignature.ReleaseAndGetAddressOf(),
                                                 errBlob.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Create cloud shadow root sig failed"; g_cloudPipelineStatus = "Cloud shadow root signature failed"; return false; }

        ComPtr<ID3DBlob> csBlob;
        errBlob.Reset();
        const std::filesystem::path shaderPath = CloudShadowShaderPath();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                      "CSGenerate", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
        if (FAILED(compileHr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile cloud shadow shader failed"; g_cloudPipelineStatus = "Cloud shadow shader compile failed"; return false; }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_cloudShadowRootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        hr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_cloudShadowPso));
        if (FAILED(hr)) { if (error) *error = "Create cloud shadow PSO failed"; g_cloudPipelineStatus = "Cloud shadow PSO failed"; return false; }
    }

    g_cloudPipelinesReady = true;
    g_cloudPipelineStatus = "Cloud pipelines ready";
    g_cloudRenderPipelineFormat = targetFormat;
    return true;
}

bool EnsureCloudVolume(int seed, std::string* error)
{
    if (!EnsureCloudPipelines(error)) return false;

    if (!g_gpuClouds.volumeTexture)
    {
        D3D12_HEAP_PROPERTIES heap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        desc.Width = kCloudVolumeResolution;
        desc.Height = kCloudVolumeResolution;
        desc.DepthOrArraySize = static_cast<UINT16>(kCloudVolumeResolution);
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                       IID_PPV_ARGS(&g_gpuClouds.volumeTexture));
        if (FAILED(hr)) { if (error) *error = "Create cloud volume texture failed"; return false; }
        g_gpuClouds.volumeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

        if (!g_gpuClouds.volumeSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuClouds.volumeSrvCpu, &g_gpuClouds.volumeSrvGpu);
            g_gpuClouds.volumeSrvAllocated = true;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture3D.MipLevels = 1;
        g_device->CreateShaderResourceView(g_gpuClouds.volumeTexture.Get(), &srvDesc, g_gpuClouds.volumeSrvCpu);
    }

    if (g_gpuClouds.volumeReady && g_gpuClouds.cachedSeed == seed)
    {
        return true;
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Cloud volume allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Cloud volume CL failed");

    if (g_gpuClouds.volumeState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_gpuClouds.volumeTexture.Get();
        b.Transition.StateBefore = g_gpuClouds.volumeState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &b);
    }

    // Per-call descriptor heap (1 UAV slot — cheap and avoids stomping the
    // shared SRV heap that ImGui owns).
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc =
        ShaderVisibleCbvSrvUavDescriptorHeapDesc(1);
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap));
    if (FAILED(hr)) { if (error) *error = "Create cloud volume UAV heap failed"; return false; }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uavDesc.Texture3D.WSize = kCloudVolumeResolution;
    g_device->CreateUnorderedAccessView(g_gpuClouds.volumeTexture.Get(), nullptr, &uavDesc,
                                         uavHeap->GetCPUDescriptorHandleForHeapStart());

    CloudVolumeShaderConstants vc{};
    vc.resolution = kCloudVolumeResolution;
    vc.seed = seed;
    vc.pad0 = 0.0f;
    vc.pad1 = 0.0f;

    ID3D12DescriptorHeap* heaps[] = {uavHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_cloudVolumeRootSignature.Get());
    commandList->SetPipelineState(g_cloudVolumePso.Get());
    commandList->SetComputeRoot32BitConstants(0, 4, &vc, 0);
    commandList->SetComputeRootDescriptorTable(1, uavHeap->GetGPUDescriptorHandleForHeapStart());
    const UINT groupCount = (kCloudVolumeResolution + 3u) / 4u;
    commandList->Dispatch(groupCount, groupCount, groupCount);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = g_gpuClouds.volumeTexture.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toSrv);
    g_gpuClouds.volumeState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    ThrowIfFailed(commandList->Close(), "Close cloud volume CL failed");
    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal cloud volume fence failed");
    WaitForFenceValue(fenceValue);

    g_gpuClouds.cachedSeed = seed;
    g_gpuClouds.volumeReady = true;
    return true;
}

// Records the cloud render pass into the supplied command list. Caller must
// have the color RT bound and the cloud volume in PIXEL_SHADER_RESOURCE state.
// Caller is responsible for re-binding any other root signatures it needs.
bool RenderCloudPass(ID3D12GraphicsCommandList* commandList,
                     const rock::CloudSettings& clouds,
                     const CloudRenderShaderConstants& base,
                     float windOffsetX,
                     float windOffsetZ,
                     float fieldCenterX,
                     float fieldCenterZ,
                     D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu)
{
    if (!clouds.enabled) return false;
    if (!g_gpuClouds.volumeReady || !g_gpuClouds.volumeTexture) return false;
    if (!g_cloudPipelinesReady) return false;

    CloudRenderShaderConstants c = base;
    c.cloudColor[0] = clouds.color[0];
    c.cloudColor[1] = clouds.color[1];
    c.cloudColor[2] = clouds.color[2];
    c.cloudColor[3] = 1.0f;
    c.altitudeMin = clouds.altitudeMin;
    c.altitudeMax = std::max(clouds.altitudeMax, clouds.altitudeMin + 1.0f);
    c.horizontalScale = std::max(clouds.horizontalScale, 1.0f);
    c.coverage = std::clamp(clouds.coverage, 0.0f, 1.0f);
    c.densityMultiplier = std::clamp(clouds.densityMultiplier, 0.0f, 8.0f);
    c.absorption = std::max(clouds.absorption, 0.0f);
    c.windOffsetX = windOffsetX;
    c.windOffsetZ = windOffsetZ;
    c.qualitySamples = std::clamp(clouds.qualitySamples, 8, 128);
    c.pad0 = 0.0f;
    c.fieldCenterX = fieldCenterX;
    c.fieldCenterZ = fieldCenterZ;
    c.fieldRadius = std::max(clouds.fieldRadius, 1.0f);
    c.fieldFalloff = std::max(clouds.fieldFalloff, 1.0f);
    c.lightSamples = clouds.selfShadowEnabled ? std::clamp(clouds.lightSamples, 0, 16) : 0;
    c.lightStepMeters = std::clamp(clouds.lightStepMeters, 1.0f, 2000.0f);
    c.phaseEccentricity = std::clamp(clouds.phaseEccentricity, -0.99f, 0.99f);
    c.shadowAmbientStrength = std::clamp(clouds.shadowAmbientStrength, 0.0f, 2.0f);

    commandList->SetGraphicsRootSignature(g_cloudRenderRootSignature.Get());
    commandList->SetPipelineState(g_cloudRenderPso.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    commandList->SetGraphicsRootDescriptorTable(1, g_gpuClouds.volumeSrvGpu);
    commandList->SetGraphicsRootDescriptorTable(2, depthSrvGpu);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
    return true;
}

// Mirrors cbuffer in shaders/cloud_shadow.hlsl. 24 DWORDs.
struct CloudShadowShaderConstants
{
    float boundsMinX;
    float boundsMinZ;
    float boundsSizeX;
    float boundsSizeZ;
    float altitudeMin;
    float altitudeMax;
    float horizontalScale;
    float coverage;
    float densityMultiplier;
    float absorption;
    float windOffsetX;
    float windOffsetZ;
    float sunDirection[4];
    UINT  resolution;
    UINT  numSamples;
    float pad0;
    float pad1;
    float fieldCenterX;
    float fieldCenterZ;
    float fieldRadius;
    float fieldFalloff;
};
static_assert(sizeof(CloudShadowShaderConstants) == 24 * sizeof(UINT), "CloudShadowShaderConstants must be 24 DWORDs");

bool EnsureDummyCloudShadowTexture(std::string* error)
{
    if (g_gpuClouds.dummyShadowTexture && g_gpuClouds.dummyShadowAllocated)
    {
        return true;
    }
    D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(1, 1, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    HRESULT hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&g_gpuClouds.dummyShadowTexture));
    if (FAILED(hr)) { if (error) *error = "Create dummy cloud shadow texture failed"; return false; }

    UINT64 uploadSize = 0;
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    D3D12_RESOURCE_DESC uploadDesc = BufferResourceDesc(uploadSize);
    ComPtr<ID3D12Resource> upload;
    hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr)) { if (error) *error = "Create dummy upload heap failed"; return false; }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeBytes = 0;
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &numRows, &rowSizeBytes, &uploadSize);

    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    static_cast<uint8_t*>(mapped)[footprint.Offset] = 255;
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList));

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_gpuClouds.dummyShadowTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = g_gpuClouds.dummyShadowTexture.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toSrv);
    commandList->Close();
    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceVal = ++g_fenceLastSignaledValue;
    g_commandQueue->Signal(g_fence.Get(), fenceVal);
    WaitForFenceValue(fenceVal);

    AllocateSrvDescriptor(nullptr, &g_gpuClouds.dummyShadowSrvCpu, &g_gpuClouds.dummyShadowSrvGpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_gpuClouds.dummyShadowTexture.Get(), &srvDesc, g_gpuClouds.dummyShadowSrvCpu);
    g_gpuClouds.dummyShadowAllocated = true;
    return true;
}

bool EnsureCloudShadowMeshCb(std::string* error)
{
    if (g_gpuClouds.meshCbUploadBuffer && g_gpuClouds.meshCbMapped)
    {
        return true;
    }
    D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC desc = BufferResourceDesc(256);  // CBVs are 256-byte aligned
    HRESULT hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&g_gpuClouds.meshCbUploadBuffer));
    if (FAILED(hr)) { if (error) *error = "Create cloud shadow mesh CB failed"; return false; }
    hr = g_gpuClouds.meshCbUploadBuffer->Map(0, nullptr, &g_gpuClouds.meshCbMapped);
    if (FAILED(hr)) { if (error) *error = "Map cloud shadow mesh CB failed"; return false; }
    CloudShadowMeshConstants zeros{};
    std::memcpy(g_gpuClouds.meshCbMapped, &zeros, sizeof(zeros));
    return true;
}

bool EnsureCloudShadowTexture(int resolution, std::string* error)
{
    if (g_gpuClouds.shadowTexture && g_gpuClouds.shadowResolution == resolution)
    {
        return true;
    }
    g_gpuClouds.shadowTexture.Reset();
    g_gpuClouds.shadowResolution = resolution;

    D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(static_cast<UINT>(resolution), static_cast<UINT>(resolution),
                                                      DXGI_FORMAT_R8_UNORM,
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&g_gpuClouds.shadowTexture));
    if (FAILED(hr)) { if (error) *error = "Create cloud shadow texture failed"; return false; }
    g_gpuClouds.shadowState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    if (!g_gpuClouds.shadowSrvAllocated)
    {
        AllocateSrvDescriptor(nullptr, &g_gpuClouds.shadowSrvCpu, &g_gpuClouds.shadowSrvGpu);
        g_gpuClouds.shadowSrvAllocated = true;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_gpuClouds.shadowTexture.Get(), &srvDesc, g_gpuClouds.shadowSrvCpu);
    return true;
}

// Generates the cloud shadow texture. Called each frame the viewport renders
// (cheap — ~0.5ms for 1024² × 16 samples). Reads the same 3D cloud volume the
// cloud render pass uses, so the visible cloud and its cast shadow stay in sync.
bool RunCloudShadowGeneration(const rock::CloudSettings& clouds,
                              float boundsMinX, float boundsMinZ,
                              float boundsSizeX, float boundsSizeZ,
                              const float sunDirection[3],
                              float windOffsetX, float windOffsetZ,
                              float fieldCenterX, float fieldCenterZ,
                              std::string* error)
{
    if (!g_cloudPipelinesReady || !g_gpuClouds.volumeReady) return false;
    const int resolution = std::clamp(clouds.shadowResolution, 256, 4096);
    if (!EnsureCloudShadowTexture(resolution, error)) return false;

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Cloud shadow allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Cloud shadow CL failed");

    if (g_gpuClouds.shadowState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = g_gpuClouds.shadowTexture.Get();
        b.Transition.StateBefore = g_gpuClouds.shadowState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &b);
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc =
        ShaderVisibleCbvSrvUavDescriptorHeapDesc(2);
    ComPtr<ID3D12DescriptorHeap> tableHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&tableHeap));
    if (FAILED(hr)) { if (error) *error = "Create cloud shadow descriptor heap failed"; return false; }

    const UINT incSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = tableHeap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC volumeSrv{};
    volumeSrv.Format = DXGI_FORMAT_R8_UNORM;
    volumeSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    volumeSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    volumeSrv.Texture3D.MipLevels = 1;
    g_device->CreateShaderResourceView(g_gpuClouds.volumeTexture.Get(), &volumeSrv, cpuStart);

    D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = cpuStart;
    uavCpu.ptr += incSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_R8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_gpuClouds.shadowTexture.Get(), nullptr, &uavDesc, uavCpu);

    CloudShadowShaderConstants c{};
    c.boundsMinX = boundsMinX;
    c.boundsMinZ = boundsMinZ;
    c.boundsSizeX = std::max(boundsSizeX, 1.0f);
    c.boundsSizeZ = std::max(boundsSizeZ, 1.0f);
    c.altitudeMin = clouds.altitudeMin;
    c.altitudeMax = std::max(clouds.altitudeMax, clouds.altitudeMin + 1.0f);
    c.horizontalScale = std::max(clouds.horizontalScale, 1.0f);
    c.coverage = std::clamp(clouds.coverage, 0.0f, 1.0f);
    c.densityMultiplier = std::clamp(clouds.densityMultiplier, 0.0f, 8.0f);
    c.absorption = std::max(clouds.absorption, 0.0f);
    c.windOffsetX = windOffsetX;
    c.windOffsetZ = windOffsetZ;
    c.sunDirection[0] = sunDirection[0];
    c.sunDirection[1] = sunDirection[1];
    c.sunDirection[2] = sunDirection[2];
    c.sunDirection[3] = 0.0f;
    c.resolution = static_cast<UINT>(resolution);
    c.numSamples = static_cast<UINT>(std::clamp(clouds.shadowSamples, 4, 64));
    c.pad0 = c.pad1 = 0.0f;
    c.fieldCenterX = fieldCenterX;
    c.fieldCenterZ = fieldCenterZ;
    c.fieldRadius = std::max(clouds.fieldRadius, 1.0f);
    c.fieldFalloff = std::max(clouds.fieldFalloff, 1.0f);

    ID3D12DescriptorHeap* heaps[] = {tableHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_cloudShadowRootSignature.Get());
    commandList->SetPipelineState(g_cloudShadowPso.Get());
    commandList->SetComputeRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE tableGpu = tableHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE uavGpu = tableGpu;
    uavGpu.ptr += incSize;
    commandList->SetComputeRootDescriptorTable(1, tableGpu);
    commandList->SetComputeRootDescriptorTable(2, uavGpu);

    const UINT groupCount = (static_cast<UINT>(resolution) + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER toSrv{};
    toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrv.Transition.pResource = g_gpuClouds.shadowTexture.Get();
    toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toSrv);
    g_gpuClouds.shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    ThrowIfFailed(commandList->Close(), "Close cloud shadow CL failed");
    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal cloud shadow fence failed");
    WaitForFenceValue(fenceValue);

    return true;
}

int EffectiveMeshResolution(int resolution, int lod)
{
    return std::clamp(resolution / (1 << std::clamp(lod, 0, 4)), 16, 2048);
}

bool IsTerrainNodeKind(rock::NodeKind kind)
{
    return kind == rock::NodeKind::HeightmapLoad ||
        kind == rock::NodeKind::Shape ||
        kind == rock::NodeKind::HeightmapFromMask ||
        kind == rock::NodeKind::HeightmapBlur ||
        kind == rock::NodeKind::MultiScaleErosion ||
        kind == rock::NodeKind::FluvialErosion ||
        kind == rock::NodeKind::DropletErosion ||
        kind == rock::NodeKind::MaskNoise ||
        kind == rock::NodeKind::MaskBlend ||
        kind == rock::NodeKind::MaskLevels ||
        kind == rock::NodeKind::MaskBlur ||
        kind == rock::NodeKind::MaskSlope ||
        kind == rock::NodeKind::MaskHeight ||
        kind == rock::NodeKind::MaskPath ||
        kind == rock::NodeKind::Crumbling ||
        kind == rock::NodeKind::MaskCurvature ||
        kind == rock::NodeKind::MaskFluvial ||
        kind == rock::NodeKind::Rock ||
        kind == rock::NodeKind::Scatter ||
        kind == rock::NodeKind::Sediment ||
        kind == rock::NodeKind::Snow ||
        kind == rock::NodeKind::Soil ||
        kind == rock::NodeKind::Colorize;
}

int CurrentPreviewMeshResolution()
{
    const rock::PreviewSettings& preview = g_graph.Settings().preview;
    return EffectiveMeshResolution(preview.resolution, preview.lod);
}

std::string FormatEvaluationDuration(std::chrono::steady_clock::time_point startedAt, std::chrono::steady_clock::time_point finishedAt)
{
    const double elapsedMs = std::chrono::duration<double, std::milli>(finishedAt - startedAt).count();
    char buffer[64]{};
    if (elapsedMs >= 1000.0)
    {
        std::snprintf(buffer, sizeof(buffer), "Eval %.2f s", elapsedMs / 1000.0);
    }
    else
    {
        std::snprintf(buffer, sizeof(buffer), "Eval %.1f ms", elapsedMs);
    }
    return buffer;
}

void EvaluateGraphSync()
{
    if (g_evaluationInFlight)
    {
        g_evaluationFuture.get();
        g_evaluationInFlight = false;
        g_evaluationPending = false;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    const int meshResolution = CurrentPreviewMeshResolution();
    g_graph.Evaluate(meshResolution);
    g_lastEvaluationDuration = FormatEvaluationDuration(startedAt, std::chrono::steady_clock::now());
}

void StartAsyncEvaluation()
{
    const uint64_t requestId = ++g_nextEvaluationRequestId;
    const int meshResolution = CurrentPreviewMeshResolution();
    rock::NodeGraph graphSnapshot = g_graph;
    graphSnapshot.SetEvaluationPending("Evaluating preview...");

    g_activeEvaluationRequestId = requestId;
    g_evaluationInFlight = true;
    g_evaluationPending = false;
    g_lastEvaluationDuration = "Eval running...";
    g_graph.SetEvaluationPending("Evaluating preview...");

    g_evaluationFuture = std::async(std::launch::async, [requestId, meshResolution, graphSnapshot = std::move(graphSnapshot)]() mutable {
        const auto startedAt = std::chrono::steady_clock::now();
        graphSnapshot.Evaluate(meshResolution);
        const auto finishedAt = std::chrono::steady_clock::now();
        AsyncEvaluationResult result;
        result.requestId = requestId;
        result.graph = std::move(graphSnapshot);
        result.duration = FormatEvaluationDuration(startedAt, finishedAt);
        return result;
    });
}

void EvaluateGraph()
{
    if (g_evaluationInFlight)
    {
        g_evaluationPending = true;
        g_graph.SetEvaluationPending("Evaluation queued...");
        g_lastEvaluationDuration = "Eval queued...";
        return;
    }

    StartAsyncEvaluation();
}

// カーソル位置のスクリーンピクセル色を取得する。
// アプリ全体は DPI-unaware のまま Windows の自動拡大に任せる。
// ピッカーだけ Per-Monitor aware に切り替え、GetPhysicalCursorPos の物理座標を
// GetDC(NULL)+GetPixel に渡して DPI スケーリング環境でも読み取り位置を揃える。
static void SampleScreenPixel(float& r, float& g, float& b)
{
    DPI_AWARENESS_CONTEXT prevCtx =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    POINT pt{};
    if (!GetPhysicalCursorPos(&pt))
    {
        GetCursorPos(&pt);
    }
    HDC hdc = GetDC(nullptr);
    COLORREF cr = GetPixel(hdc, pt.x, pt.y);
    ReleaseDC(nullptr, hdc);

    SetThreadDpiAwarenessContext(prevCtx);

    if (cr == CLR_INVALID) { return; }
    r = GetRValue(cr) / 255.0f;
    g = GetGValue(cr) / 255.0f;
    b = GetBValue(cr) / 255.0f;
}

void PollAsyncEvaluation()
{
    if (!g_evaluationInFlight)
    {
        return;
    }

    if (g_evaluationFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        return;
    }

    AsyncEvaluationResult result = g_evaluationFuture.get();
    g_evaluationInFlight = false;
    if (!g_evaluationPending && result.requestId == g_activeEvaluationRequestId)
    {
        g_graph.ApplyEvaluationResultFrom(result.graph);
        g_lastEvaluationDuration = result.duration;
    }

    if (g_evaluationPending)
    {
        StartAsyncEvaluation();
    }
}

void WaitForAsyncEvaluationForShutdown()
{
    if (!g_evaluationInFlight)
    {
        return;
    }

    while (g_evaluationFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
    {
        ProcessPendingMseGpuRequests();
        ProcessPendingMaskNoiseGpuRequests();
        ProcessPendingMaskUtilityGpuRequests();
        ProcessPendingSedimentGpuRequests();
        ProcessPendingRockGpuRequests();
        ProcessPendingMaskFluvialGpuRequests();
        ProcessPendingFluvialErosionGpuRequests();
        ProcessPendingDropletErosionGpuRequests();
        ProcessPendingSnowGpuRequests();
        ProcessPendingColorizeGpuRequests();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    AsyncEvaluationResult result = g_evaluationFuture.get();
    g_evaluationInFlight = false;
    if (!g_evaluationPending && result.requestId == g_activeEvaluationRequestId)
    {
        g_graph.ApplyEvaluationResultFrom(result.graph);
        g_lastEvaluationDuration = result.duration;
    }
    g_evaluationPending = false;
}

bool ExportCurrentPreviewTexture(const std::filesystem::path& path, int resolution, std::string* error)
{
    if (g_evaluationInFlight)
    {
        if (error != nullptr) *error = "Evaluation is still running";
        return false;
    }

    rock::NodeGraph exportGraph = g_graph;
    exportGraph.MarkDirty("Texture export");
    exportGraph.Evaluate(0);
    return terrain::ExportPreviewTexturePng(exportGraph.Evaluation(), path, resolution, error);
}

void OpenExportFolder(const std::filesystem::path& exportPath)
{
    std::filesystem::path folder = exportPath.parent_path();
    if (folder.empty())
    {
        folder = std::filesystem::current_path();
    }

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    OpenFolderInExplorer(folder);
}

terrain::ViewportCameraState MakeViewportCameraState()
{
    return terrain::ViewportCameraState{
        g_viewport.yaw,
        g_viewport.pitch,
        g_viewport.fovDegrees,
        g_viewport.orbitDistance,
        g_viewport.pan,
    };
}

float DefaultViewportOrbitDistance()
{
    return terrain::DefaultViewportOrbitDistance(g_graph.Settings().preview.terrainSizeMeters, g_viewport.fovDegrees);
}

void ResetViewport()
{
    const float fovDegrees = std::clamp(g_viewport.fovDegrees, 15.0f, 90.0f);
    g_viewport = {};
    g_viewport.yaw = kDefaultViewportYaw;
    g_viewport.pitch = kDefaultViewportPitch;
    g_viewport.fovDegrees = fovDegrees;
    g_viewport.orbitDistance = DefaultViewportOrbitDistance();
}

float CameraFocalLengthMmFromFovYDegrees(float fovYDegrees)
{
    return terrain::CameraFocalLengthMmFromFovYDegrees(fovYDegrees);
}

float CameraFovYDegreesFromFocalLengthMm(float focalLengthMm)
{
    return terrain::CameraFovYDegreesFromFocalLengthMm(focalLengthMm);
}

bool TryPickViewportFocusPoint(const ImVec2& min, const ImVec2& max, const ImVec2& mouse, Vec3* outPoint, float* outFocusDistance, ImVec2* outScreenPoint);
const rock::PathPoint* FindPathPoint(const rock::PathSettings& path, rock::GraphId pointId);
float PathPointDisplayHeight(const rock::PathPoint& point);
bool PathEdgePositionAt(const rock::PathSettings& path, const rock::PathEdge& edge, float t, Vec3* outPosition);
int PathEdgeDisplayStepCount(const rock::PathEdge& edge);
ProjectedPoint ProjectWorldToScreen(float x, float y, float z, const ImVec2& center, float scale);

rock::GraphId g_pathEditNodeId = 0;
rock::GraphId g_pathActiveTailPointId = 0;
enum class PathSelectionKind
{
    None,
    Point,
    Edge,
};

enum class PathMoveGizmoAxis
{
    None,
    Center,
    X,
    Y,
    Z,
};

PathSelectionKind g_pathSelectionKind = PathSelectionKind::None;
rock::GraphId g_pathSelectedElementId = 0;
bool g_pathMoveGizmoVisible = false;
PathMoveGizmoAxis g_pathActiveMoveGizmoAxis = PathMoveGizmoAxis::None;
rock::GraphId g_pathMoveGizmoDragPointId = 0;
rock::PathPoint g_pathMoveGizmoDragStartPoint;
ImVec2 g_pathMoveGizmoDragStartMouse;

struct PathHitResult
{
    PathSelectionKind kind = PathSelectionKind::None;
    rock::GraphId elementId = 0;
    float t = 0.0f;
};

const rock::Node* SelectedPathNode()
{
    const rock::Node* node = g_graph.FindNode(g_selectedNodeId);
    return node != nullptr && node->kind == rock::NodeKind::Path ? node : nullptr;
}

rock::Node* SelectedMutablePathNode()
{
    rock::Node* node = g_graph.FindMutableNode(g_selectedNodeId);
    return node != nullptr && node->kind == rock::NodeKind::Path ? node : nullptr;
}

bool PathContainsPoint(const rock::Node& node, rock::GraphId pointId)
{
    return std::ranges::any_of(node.path.points, [pointId](const rock::PathPoint& point) {
        return point.id == pointId;
    });
}

void ResetPathActiveTail()
{
    g_pathEditNodeId = g_selectedNodeId;
    g_pathActiveTailPointId = 0;
}

void ClearPathSelection()
{
    g_pathSelectionKind = PathSelectionKind::None;
    g_pathSelectedElementId = 0;
    g_pathMoveGizmoVisible = false;
    g_pathActiveMoveGizmoAxis = PathMoveGizmoAxis::None;
    g_pathMoveGizmoDragPointId = 0;
}

bool FinishPathSegmentFromShortcut()
{
    if (!ImGui::IsKeyPressed(ImGuiKey_Enter) && !ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
    {
        return false;
    }
    ResetPathActiveTail();
    SetProjectStatus("Path segment finished");
    return true;
}

float DistanceSquared(ImVec2 a, ImVec2 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

float DistanceToSegmentSquared(ImVec2 point, ImVec2 a, ImVec2 b, float* outT)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float lengthSq = dx * dx + dy * dy;
    float t = 0.0f;
    if (lengthSq > 0.000001f)
    {
        t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / lengthSq, 0.0f, 1.0f);
    }
    if (outT)
    {
        *outT = t;
    }
    return DistanceSquared(point, ImVec2(a.x + dx * t, a.y + dy * t));
}

Vec3 PathMoveGizmoAxisDirection(PathMoveGizmoAxis axis)
{
    switch (axis)
    {
    case PathMoveGizmoAxis::Center:
        return Vec3(0.0f, 0.0f, 0.0f);
    case PathMoveGizmoAxis::X:
        return Vec3(1.0f, 0.0f, 0.0f);
    case PathMoveGizmoAxis::Y:
        return Vec3(0.0f, 1.0f, 0.0f);
    case PathMoveGizmoAxis::Z:
        return Vec3(0.0f, 0.0f, 1.0f);
    default:
        return Vec3(0.0f, 0.0f, 0.0f);
    }
}

rock::PathPoint* FindMutablePathPoint(rock::PathSettings& path, rock::GraphId pointId)
{
    const auto it = std::ranges::find_if(path.points, [pointId](const rock::PathPoint& point) {
        return point.id == pointId;
    });
    return it != path.points.end() ? &*it : nullptr;
}

bool SelectedPathPointPosition(Vec3* outPosition)
{
    const rock::Node* node = SelectedPathNode();
    if (node == nullptr || g_pathSelectionKind != PathSelectionKind::Point)
    {
        return false;
    }
    const rock::PathPoint* point = FindPathPoint(node->path, g_pathSelectedElementId);
    if (point == nullptr)
    {
        return false;
    }
    if (outPosition)
    {
        *outPosition = Vec3(point->x, PathPointDisplayHeight(*point), point->z);
    }
    return true;
}

float PathMoveGizmoAxisLength(const Vec3& pivot, const ImVec2& center, float scale)
{
    constexpr float kTargetPixels = 72.0f;
    const ProjectedPoint pivotScreen = ProjectWorldToScreen(pivot.x, pivot.y, pivot.z, center, scale);
    if (pivotScreen.depth <= 0.05f)
    {
        return 1.0f;
    }

    const Vec3 offsets[] = {
        Vec3(1.0f, 0.0f, 0.0f),
        Vec3(0.0f, 1.0f, 0.0f),
        Vec3(0.0f, 0.0f, 1.0f),
    };
    float pixelsPerUnit = 0.0f;
    int sampleCount = 0;
    for (const Vec3& offset : offsets)
    {
        const ProjectedPoint sample = ProjectWorldToScreen(pivot.x + offset.x, pivot.y + offset.y, pivot.z + offset.z, center, scale);
        if (sample.depth <= 0.05f)
        {
            continue;
        }
        const float distance = std::sqrt(DistanceSquared(pivotScreen.screen, sample.screen));
        if (distance <= 0.001f)
        {
            continue;
        }
        pixelsPerUnit += distance;
        ++sampleCount;
    }
    if (sampleCount <= 0)
    {
        return 1.0f;
    }
    pixelsPerUnit /= static_cast<float>(sampleCount);
    const float terrainSize = std::max(1.0f, g_graph.Settings().preview.terrainSizeMeters);
    return std::clamp(kTargetPixels / pixelsPerUnit, terrainSize * 0.002f, terrainSize * 0.25f);
}

PathMoveGizmoAxis HitTestPathMoveGizmo(const Vec3& pivot, const ImVec2& min, const ImVec2& max, const ImVec2& mouse)
{
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const ProjectedPoint pivotScreen = ProjectWorldToScreen(pivot.x, pivot.y, pivot.z, center, scale);
    if (pivotScreen.depth <= 0.05f)
    {
        return PathMoveGizmoAxis::None;
    }

    constexpr float kCenterRadius = 10.0f;
    if (DistanceSquared(mouse, pivotScreen.screen) <= kCenterRadius * kCenterRadius)
    {
        return PathMoveGizmoAxis::Center;
    }

    const float axisLength = PathMoveGizmoAxisLength(pivot, center, scale);
    constexpr float kAxisHitRadius = 10.0f;
    float bestDistanceSq = kAxisHitRadius * kAxisHitRadius;
    PathMoveGizmoAxis bestAxis = PathMoveGizmoAxis::None;
    for (PathMoveGizmoAxis axis : {PathMoveGizmoAxis::X, PathMoveGizmoAxis::Y, PathMoveGizmoAxis::Z})
    {
        const Vec3 dir = PathMoveGizmoAxisDirection(axis);
        const ProjectedPoint end = ProjectWorldToScreen(
            pivot.x + dir.x * axisLength,
            pivot.y + dir.y * axisLength,
            pivot.z + dir.z * axisLength,
            center,
            scale);
        if (end.depth <= 0.05f)
        {
            continue;
        }
        float axisT = 0.0f;
        const float distanceSq = DistanceToSegmentSquared(mouse, pivotScreen.screen, end.screen, &axisT);
        const float axisScreenLength = std::sqrt(DistanceSquared(pivotScreen.screen, end.screen));
        if (axisScreenLength * axisT < kCenterRadius)
        {
            continue;
        }
        if (distanceSq < bestDistanceSq)
        {
            bestDistanceSq = distanceSq;
            bestAxis = axis;
        }
    }
    return bestAxis;
}

rock::GraphId AppendPathPoint(rock::Node& node, float x, float z, float height, rock::GraphId connectFromPointId)
{
    rock::PathPoint point;
    point.id = g_graph.AllocatePathElementId();
    point.x = x;
    point.z = z;
    point.height = height;
    point.heightOffset = node.path.defaultHeightOffset;
    point.heightMode = node.path.defaultHeightMode;
    point.widthMeters = node.path.defaultWidthMeters;
    point.featherMeters = node.path.defaultFeatherMeters;
    point.intensity = 1.0f;

    const rock::GraphId pointId = point.id;
    node.path.points.push_back(point);
    if (connectFromPointId != 0)
    {
        rock::PathEdge edge;
        edge.id = g_graph.AllocatePathElementId();
        edge.fromPoint = connectFromPointId;
        edge.toPoint = pointId;
        edge.widthMeters = node.path.defaultWidthMeters;
        edge.featherMeters = node.path.defaultFeatherMeters;
        edge.segmentType = node.path.defaultSegmentType;
        node.path.edges.push_back(edge);
    }
    return pointId;
}

void AddPathPointFromViewport(float x, float z, float height)
{
    rock::Node* node = SelectedMutablePathNode();
    if (node == nullptr)
    {
        return;
    }
    if (g_pathEditNodeId != node->id)
    {
        ResetPathActiveTail();
    }
    const rock::GraphId connectFromPointId = PathContainsPoint(*node, g_pathActiveTailPointId) ? g_pathActiveTailPointId : 0;
    PushUndoSnapshot();
    g_pathActiveTailPointId = AppendPathPoint(*node, x, z, height, connectFromPointId);
    g_pathEditNodeId = node->id;
    MarkGraphChanged("Path point added");
    EvaluateGraph();
    SetProjectStatus("Path point added");
}

void SelectPathPoint(rock::GraphId pointId)
{
    g_pathSelectionKind = PathSelectionKind::Point;
    g_pathSelectedElementId = pointId;
    g_pathActiveTailPointId = pointId;
    g_pathEditNodeId = g_selectedNodeId;
    SetProjectStatus("Path point selected");
}

void SelectPathEdge(rock::GraphId edgeId)
{
    g_pathSelectionKind = PathSelectionKind::Edge;
    g_pathSelectedElementId = edgeId;
    g_pathActiveTailPointId = 0;
    g_pathEditNodeId = g_selectedNodeId;
    SetProjectStatus("Path edge selected");
}

void DeleteSelectedPathElement()
{
    rock::Node* node = SelectedMutablePathNode();
    if (node == nullptr || g_pathSelectedElementId == 0)
    {
        return;
    }

    if (g_pathSelectionKind == PathSelectionKind::Point)
    {
        const rock::GraphId pointId = g_pathSelectedElementId;
        const auto pointIt = std::ranges::find_if(node->path.points, [pointId](const rock::PathPoint& point) {
            return point.id == pointId;
        });
        if (pointIt == node->path.points.end())
        {
            ClearPathSelection();
            return;
        }
        PushUndoSnapshot();
        node->path.points.erase(pointIt);
        std::erase_if(node->path.edges, [pointId](const rock::PathEdge& edge) {
            return edge.fromPoint == pointId || edge.toPoint == pointId;
        });
        if (g_pathActiveTailPointId == pointId)
        {
            g_pathActiveTailPointId = 0;
        }
        ClearPathSelection();
        MarkGraphChanged("Path point deleted");
        EvaluateGraph();
        SetProjectStatus("Path point deleted");
        return;
    }

    if (g_pathSelectionKind == PathSelectionKind::Edge)
    {
        const rock::GraphId edgeId = g_pathSelectedElementId;
        const auto edgeIt = std::ranges::find_if(node->path.edges, [edgeId](const rock::PathEdge& edge) {
            return edge.id == edgeId;
        });
        if (edgeIt == node->path.edges.end())
        {
            ClearPathSelection();
            return;
        }
        PushUndoSnapshot();
        node->path.edges.erase(edgeIt);
        ClearPathSelection();
        MarkGraphChanged("Path edge deleted");
        EvaluateGraph();
        SetProjectStatus("Path edge deleted");
    }
}

void EnablePathMoveGizmo()
{
    if (g_pathSelectionKind == PathSelectionKind::Point && g_pathSelectedElementId != 0)
    {
        g_pathMoveGizmoVisible = true;
        SetProjectStatus("Path move gizmo");
    }
}

bool StartPathMoveGizmoDrag(const ImVec2& min, const ImVec2& max)
{
    if (!g_pathMoveGizmoVisible || g_pathSelectionKind != PathSelectionKind::Point)
    {
        return false;
    }
    Vec3 pivot;
    if (!SelectedPathPointPosition(&pivot))
    {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const PathMoveGizmoAxis axis = HitTestPathMoveGizmo(pivot, min, max, io.MousePos);
    if (axis == PathMoveGizmoAxis::None)
    {
        return false;
    }

    rock::Node* node = SelectedMutablePathNode();
    if (node == nullptr)
    {
        return false;
    }
    const rock::PathPoint* point = FindPathPoint(node->path, g_pathSelectedElementId);
    if (point == nullptr)
    {
        return false;
    }

    PushUndoSnapshot();
    g_pathActiveMoveGizmoAxis = axis;
    g_pathMoveGizmoDragPointId = point->id;
    g_pathMoveGizmoDragStartPoint = *point;
    g_pathMoveGizmoDragStartMouse = io.MousePos;
    return true;
}

bool UpdatePathMoveGizmoDrag(const ImVec2& min, const ImVec2& max)
{
    if (g_pathActiveMoveGizmoAxis == PathMoveGizmoAxis::None || g_pathMoveGizmoDragPointId == 0)
    {
        return false;
    }

    rock::Node* node = SelectedMutablePathNode();
    if (node == nullptr)
    {
        g_pathActiveMoveGizmoAxis = PathMoveGizmoAxis::None;
        g_pathMoveGizmoDragPointId = 0;
        return false;
    }
    rock::PathPoint* point = FindMutablePathPoint(node->path, g_pathMoveGizmoDragPointId);
    if (point == nullptr)
    {
        g_pathActiveMoveGizmoAxis = PathMoveGizmoAxis::None;
        g_pathMoveGizmoDragPointId = 0;
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        g_pathActiveMoveGizmoAxis = PathMoveGizmoAxis::None;
        g_pathMoveGizmoDragPointId = 0;
        MarkGraphChanged("Path point moved");
        EvaluateGraph();
        SetProjectStatus("Path point moved");
        return true;
    }

    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const Vec3 startPivot(g_pathMoveGizmoDragStartPoint.x, PathPointDisplayHeight(g_pathMoveGizmoDragStartPoint), g_pathMoveGizmoDragStartPoint.z);
    const ImVec2 mouseDelta(io.MousePos.x - g_pathMoveGizmoDragStartMouse.x, io.MousePos.y - g_pathMoveGizmoDragStartMouse.y);
    if (g_pathActiveMoveGizmoAxis == PathMoveGizmoAxis::Center)
    {
        const ProjectedPoint startScreen = ProjectWorldToScreen(startPivot.x, startPivot.y, startPivot.z, center, scale);
        if (startScreen.depth <= 0.05f)
        {
            return true;
        }
        const float fovRadians = std::clamp(g_viewport.fovDegrees, 15.0f, 90.0f) * kDegreesToRadians;
        const float focalLength = 1.0f / std::tan(fovRadians * 0.5f);
        const float worldUnitsPerPixel = startScreen.depth / (focalLength * scale);
        const CameraBasis basis = terrain::BuildCameraBasis(MakeViewportCameraState());
        const Vec3 deltaWorld = terrain::Add(
            terrain::Scale(basis.right, mouseDelta.x * worldUnitsPerPixel),
            terrain::Scale(basis.up, -mouseDelta.y * worldUnitsPerPixel));

        *point = g_pathMoveGizmoDragStartPoint;
        point->x += deltaWorld.x;
        point->height = startPivot.y + deltaWorld.y;
        point->z += deltaWorld.z;
        point->heightMode = rock::PathPointHeightMode::Absolute;
        MarkProjectDirty();
        return true;
    }

    const Vec3 axisDir = PathMoveGizmoAxisDirection(g_pathActiveMoveGizmoAxis);
    const float axisLength = PathMoveGizmoAxisLength(startPivot, center, scale);
    const ProjectedPoint startScreen = ProjectWorldToScreen(startPivot.x, startPivot.y, startPivot.z, center, scale);
    const ProjectedPoint endScreen = ProjectWorldToScreen(
        startPivot.x + axisDir.x * axisLength,
        startPivot.y + axisDir.y * axisLength,
        startPivot.z + axisDir.z * axisLength,
        center,
        scale);
    const float axisScreenLength = std::sqrt(DistanceSquared(startScreen.screen, endScreen.screen));
    if (startScreen.depth <= 0.05f || endScreen.depth <= 0.05f || axisScreenLength <= 0.001f)
    {
        return true;
    }

    const ImVec2 axisScreenDir(
        (endScreen.screen.x - startScreen.screen.x) / axisScreenLength,
        (endScreen.screen.y - startScreen.screen.y) / axisScreenLength);
    const float deltaPixels = mouseDelta.x * axisScreenDir.x + mouseDelta.y * axisScreenDir.y;
    const float deltaWorld = (deltaPixels / axisScreenLength) * axisLength;

    *point = g_pathMoveGizmoDragStartPoint;
    point->x += axisDir.x * deltaWorld;
    point->height += axisDir.y * deltaWorld;
    point->z += axisDir.z * deltaWorld;
    if (g_pathActiveMoveGizmoAxis == PathMoveGizmoAxis::Y)
    {
        point->heightMode = rock::PathPointHeightMode::Absolute;
    }
    MarkProjectDirty();
    return true;
}

void InsertPathPointOnEdge(rock::Node& node, rock::GraphId edgeId, float t)
{
    const auto edgeIt = std::ranges::find_if(node.path.edges, [edgeId](const rock::PathEdge& edge) {
        return edge.id == edgeId;
    });
    if (edgeIt == node.path.edges.end())
    {
        return;
    }
    const rock::PathPoint* fromPoint = FindPathPoint(node.path, edgeIt->fromPoint);
    const rock::PathPoint* toPoint = FindPathPoint(node.path, edgeIt->toPoint);
    if (fromPoint == nullptr || toPoint == nullptr)
    {
        return;
    }

    t = std::clamp(t, 0.02f, 0.98f);
    PushUndoSnapshot();
    Vec3 splitPosition;
    if (!PathEdgePositionAt(node.path, *edgeIt, t, &splitPosition))
    {
        splitPosition = Vec3(
            std::lerp(fromPoint->x, toPoint->x, t),
            std::lerp(PathPointDisplayHeight(*fromPoint), PathPointDisplayHeight(*toPoint), t),
            std::lerp(fromPoint->z, toPoint->z, t));
    }
    rock::PathPoint point;
    point.id = g_graph.AllocatePathElementId();
    point.x = splitPosition.x;
    point.z = splitPosition.z;
    point.height = splitPosition.y;
    point.heightOffset = node.path.defaultHeightOffset;
    point.widthMeters = std::lerp(fromPoint->widthMeters, toPoint->widthMeters, t);
    point.featherMeters = std::lerp(fromPoint->featherMeters, toPoint->featherMeters, t);
    point.intensity = std::lerp(fromPoint->intensity, toPoint->intensity, t);
    point.heightMode = node.path.defaultHeightMode;

    const rock::PathEdge originalEdge = *edgeIt;
    edgeIt->toPoint = point.id;

    rock::PathEdge secondEdge = originalEdge;
    secondEdge.id = g_graph.AllocatePathElementId();
    secondEdge.fromPoint = point.id;
    node.path.points.push_back(point);
    node.path.edges.push_back(secondEdge);

    SelectPathPoint(point.id);
    MarkGraphChanged("Path edge split");
    EvaluateGraph();
    SetProjectStatus("Path point inserted");
}

PathHitResult HitTestPathViewport(const rock::Node& node, const ImVec2& min, const ImVec2& max, const ImVec2& mouse)
{
    constexpr float kPointHitRadius = 8.0f;
    constexpr float kEdgeHitRadius = 7.0f;
    PathHitResult result;
    float bestPointDistanceSq = kPointHitRadius * kPointHitRadius;
    float bestEdgeDistanceSq = kEdgeHitRadius * kEdgeHitRadius;

    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);

    for (const rock::PathPoint& point : node.path.points)
    {
        const ProjectedPoint projected = ProjectWorldToScreen(point.x, PathPointDisplayHeight(point), point.z, center, scale);
        if (projected.depth <= 0.05f)
        {
            continue;
        }
        const float distanceSq = DistanceSquared(mouse, projected.screen);
        if (distanceSq <= bestPointDistanceSq)
        {
            bestPointDistanceSq = distanceSq;
            result.kind = PathSelectionKind::Point;
            result.elementId = point.id;
            result.t = 0.0f;
        }
    }
    if (result.kind == PathSelectionKind::Point)
    {
        return result;
    }

    for (const rock::PathEdge& edge : node.path.edges)
    {
        if (!edge.enabled)
        {
            continue;
        }
        Vec3 prev;
        if (!PathEdgePositionAt(node.path, edge, 0.0f, &prev))
        {
            continue;
        }
        ProjectedPoint prevProjected = ProjectWorldToScreen(prev.x, prev.y, prev.z, center, scale);
        const int steps = PathEdgeDisplayStepCount(edge);
        for (int step = 1; step <= steps; ++step)
        {
            const float edgeT0 = static_cast<float>(step - 1) / static_cast<float>(steps);
            const float edgeT1 = static_cast<float>(step) / static_cast<float>(steps);
            Vec3 next;
            if (!PathEdgePositionAt(node.path, edge, edgeT1, &next))
            {
                break;
            }
            const ProjectedPoint nextProjected = ProjectWorldToScreen(next.x, next.y, next.z, center, scale);
            if (prevProjected.depth > 0.05f && nextProjected.depth > 0.05f)
            {
                float segmentT = 0.0f;
                const float distanceSq = DistanceToSegmentSquared(mouse, prevProjected.screen, nextProjected.screen, &segmentT);
                if (distanceSq <= bestEdgeDistanceSq)
                {
                    bestEdgeDistanceSq = distanceSq;
                    result.kind = PathSelectionKind::Edge;
                    result.elementId = edge.id;
                    result.t = std::lerp(edgeT0, edgeT1, segmentT);
                }
            }
            prevProjected = nextProjected;
        }
    }
    return result;
}

bool TryPickViewportGroundPlanePoint(const ImVec2& min, const ImVec2& max, const ImVec2& mouse, Vec3* outPoint)
{
    const float viewportWidth = std::max(1.0f, max.x - min.x);
    const float viewportHeight = std::max(1.0f, max.y - min.y);
    const float viewportSize = std::min(viewportWidth, viewportHeight);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const float fovRadians = std::clamp(g_viewport.fovDegrees, 15.0f, 90.0f) * kDegreesToRadians;
    const float focalLength = 1.0f / std::tan(fovRadians * 0.5f);
    const float cameraX = (mouse.x - center.x) / (focalLength * scale);
    const float cameraY = -(mouse.y - center.y) / (focalLength * scale);

    const CameraBasis basis = terrain::BuildCameraBasis(MakeViewportCameraState());
    const Vec3 rayDir = terrain::Normalize(
        terrain::Add(terrain::Add(basis.forward, terrain::Scale(basis.right, cameraX)), terrain::Scale(basis.up, cameraY)),
        basis.forward);
    if (std::abs(rayDir.y) <= 0.000001f)
    {
        return false;
    }

    const float t = -basis.position.y / rayDir.y;
    if (t <= 0.0f)
    {
        return false;
    }

    Vec3 hitPoint = terrain::Add(basis.position, terrain::Scale(rayDir, t));
    const float halfSize = std::max(1.0f, g_graph.Settings().preview.terrainSizeMeters) * 0.5f;
    hitPoint.x = std::clamp(hitPoint.x, -halfSize, halfSize);
    hitPoint.y = 0.0f;
    hitPoint.z = std::clamp(hitPoint.z, -halfSize, halfSize);
    if (outPoint)
    {
        *outPoint = hitPoint;
    }
    return true;
}

bool IsMouseOverViewportDisplayUi(const ImVec2& viewportMin)
{
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 buttonMin(viewportMin.x + 14.0f, viewportMin.y + 12.0f);
    const ImVec2 buttonMax(buttonMin.x + 54.0f, buttonMin.y + 28.0f);
    if (mouse.x >= buttonMin.x && mouse.x <= buttonMax.x && mouse.y >= buttonMin.y && mouse.y <= buttonMax.y)
    {
        return true;
    }

    if (ImGuiWindow* popup = ImGui::FindWindowByName("ViewportDisplayMenu"))
    {
        if (popup->WasActive)
        {
            const ImVec2 popupMin = popup->Pos;
            const ImVec2 popupMax(popup->Pos.x + popup->Size.x, popup->Pos.y + popup->Size.y);
            if (mouse.x >= popupMin.x && mouse.x <= popupMax.x && mouse.y >= popupMin.y && mouse.y <= popupMax.y)
            {
                return true;
            }
        }
    }
    return false;
}

bool UpdatePathViewportInteraction(const ImVec2& min, const ImVec2& max)
{
    const rock::Node* pathNode = SelectedPathNode();
    if (pathNode == nullptr)
    {
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsMouseHoveringRect(min, max);
    if (!hovered || io.WantTextInput)
    {
        return false;
    }

    if (FinishPathSegmentFromShortcut())
    {
        return true;
    }

    if (UpdatePathMoveGizmoDrag(min, max))
    {
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_W) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt)
    {
        EnablePathMoveGizmo();
        return true;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        DeleteSelectedPathElement();
        return true;
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyShift && !io.KeyAlt)
    {
        if (IsMouseOverViewportDisplayUi(min))
        {
            return false;
        }

        if (!io.KeyCtrl && StartPathMoveGizmoDrag(min, max))
        {
            return true;
        }

        const PathHitResult hit = HitTestPathViewport(*pathNode, min, max, io.MousePos);
        if (!io.KeyCtrl && hit.kind == PathSelectionKind::Point)
        {
            SelectPathPoint(hit.elementId);
            return true;
        }
        if (hit.kind == PathSelectionKind::Edge)
        {
            if (io.KeyCtrl)
            {
                rock::Node* mutablePathNode = SelectedMutablePathNode();
                if (mutablePathNode != nullptr)
                {
                    InsertPathPointOnEdge(*mutablePathNode, hit.elementId, hit.t);
                }
                return true;
            }
            SelectPathEdge(hit.elementId);
            return true;
        }

        if (io.KeyCtrl)
        {
            return false;
        }

        Vec3 hitPoint;
        if (TryPickViewportFocusPoint(min, max, io.MousePos, &hitPoint, nullptr, nullptr) ||
            TryPickViewportGroundPlanePoint(min, max, io.MousePos, &hitPoint))
        {
            AddPathPointFromViewport(hitPoint.x, hitPoint.z, hitPoint.y);
            return true;
        }
    }
    return false;
}

void BeginSunDirectionDrag()
{
    rock::PreviewSettings& preview = g_graph.Settings().preview;
    if (preview.sunDirectionMode == rock::SunDirectionMode::DateTime)
    {
        const SunPositionDegrees currentSun = EffectiveSunPosition(preview);
        preview.sunAzimuthDegrees = currentSun.azimuth;
        preview.sunElevationDegrees = std::clamp(currentSun.elevation, -10.0f, 89.0f);
        preview.sunDirectionMode = rock::SunDirectionMode::Manual;
    }
    g_sunDirectionDragActive = true;
    g_sunDirectionGizmoVisibleUntil = ImGui::GetTime() + 0.35;
}

void UpdateSunDirectionDrag(const ImVec2& mouseDelta)
{
    rock::PreviewSettings& preview = g_graph.Settings().preview;
    constexpr float kDegreesPerPixel = 0.25f;
    preview.sunAzimuthDegrees = NormalizeDegrees(preview.sunAzimuthDegrees + mouseDelta.x * kDegreesPerPixel);
    preview.sunElevationDegrees = std::clamp(preview.sunElevationDegrees - mouseDelta.y * kDegreesPerPixel, -10.0f, 89.0f);
    g_sunDirectionGizmoVisibleUntil = ImGui::GetTime() + 0.35;
}

void UpdateCameraAutoOrbit(float deltaSeconds)
{
    if (!g_viewport.autoOrbitEnabled || deltaSeconds <= 0.0f)
    {
        return;
    }
    const float speedRadiansPerSecond = g_viewport.autoOrbitSpeedDegreesPerSecond * kDegreesToRadians;
    g_viewport.yaw = std::remainder(g_viewport.yaw + speedRadiansPerSecond * deltaSeconds, 2.0f * 3.1415926535f);
}

void UpdateViewportInteraction(const ImVec2& min, const ImVec2& max)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsMouseHoveringRect(min, max);
    const bool viewportInputAvailable = hovered && !io.WantTextInput;
    if (g_sunDirectionDragActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        g_sunDirectionDragActive = false;
        g_sunDirectionGizmoVisibleUntil = ImGui::GetTime() + 0.35;
        SaveAppSettingsSilently();
    }

    const bool pathEditMode = SelectedPathNode() != nullptr;
    const bool focusPickAvailable = g_graph.Settings().preview.depthOfFieldEnabled && !pathEditMode;
    if (!focusPickAvailable)
    {
        g_focusPickMode = false;
    }
    const bool focusPickShortcut =
        focusPickAvailable && viewportInputAvailable && ImGui::IsKeyDown(ImGuiKey_F) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
    g_focusPickCursorActive = focusPickAvailable && (g_focusPickMode || focusPickShortcut);
    g_focusPickHoverPoint.reset();

    if (g_focusPickCursorActive && viewportInputAvailable)
    {
        Vec3 hitPoint;
        float focusDistance = 0.0f;
        if (TryPickViewportFocusPoint(min, max, io.MousePos, &hitPoint, &focusDistance, nullptr))
        {
            g_focusPickHoverPoint = hitPoint;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                g_graph.Settings().preview.dofFocusDistanceMeters = focusDistance;
                g_focusPickMode = false;
                g_focusPickCursorActive = false;
                MarkProjectDirty();
            }
        }
    }

    if (g_focusPickMode && ImGui::IsKeyPressed(ImGuiKey_Escape))
    {
        g_focusPickMode = false;
        g_focusPickCursorActive = false;
    }

    if (g_focusPickCursorActive)
    {
        return;
    }

    if (!hovered)
    {
        return;
    }

    const bool sunDirectionDragShortcut = ImGui::IsKeyDown(ImGuiKey_L) && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt;
    if (sunDirectionDragShortcut && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            if (!g_sunDirectionDragActive)
            {
                BeginSunDirectionDrag();
            }
            UpdateSunDirectionDrag(io.MouseDelta);
        }
        return;
    }
    if ((g_sunDirectionDragActive || sunDirectionDragShortcut) && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
    {
        if (!g_sunDirectionDragActive)
        {
            BeginSunDirectionDrag();
        }
        UpdateSunDirectionDrag(io.MouseDelta);
        return;
    }

    if (UpdatePathViewportInteraction(min, max))
    {
        return;
    }

    const bool altNavigation = io.KeyAlt;
    if (altNavigation && io.MouseWheel != 0.0f)
    {
        const float zoomFactor = std::pow(0.86f, io.MouseWheel);
        g_viewport.orbitDistance = std::clamp(g_viewport.orbitDistance * zoomFactor, 1.0f, kMaxViewportOrbitDistance);
    }

    if (altNavigation && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !io.KeyCtrl && !io.KeyShift)
    {
        g_viewport.yaw -= io.MouseDelta.x * 0.01f;
        g_viewport.pitch += io.MouseDelta.y * 0.01f;
        g_viewport.pitch = std::clamp(g_viewport.pitch, -1.25f, 1.25f);
    }

    if (altNavigation && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) && hovered)
    {
        g_viewport.pan.x += io.MouseDelta.x;
        g_viewport.pan.y += io.MouseDelta.y;
    }
}

CameraBasis BuildCameraBasis()
{
    return terrain::BuildCameraBasis(MakeViewportCameraState());
}

ImVec2 ProjectWorldNormalized(float x, float y, float z)
{
    return terrain::ProjectWorldNormalized(MakeViewportCameraState(), x, y, z);
}

ProjectedPoint ProjectWorldToScreen(float x, float y, float z, const ImVec2& center, float scale)
{
    return terrain::ProjectWorldToScreen(MakeViewportCameraState(), x, y, z, center, scale);
}

bool TryPickViewportFocusPoint(const ImVec2& min, const ImVec2& max, const ImVec2& mouse, Vec3* outPoint, float* outFocusDistance, ImVec2* outScreenPoint)
{
    return terrain::TryPickViewportFocusPoint(
        MakeViewportCameraState(),
        g_graph.Evaluation().previewHeightfield,
        min,
        max,
        mouse,
        kViewportFarPlane,
        outPoint,
        outFocusDistance,
        outScreenPoint);
}
void DrawFocusPickOverlay(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
{
    const bool showHover = g_focusPickHoverPoint.has_value();
    if (!showHover && !g_focusPickMode && !g_focusPickCursorActive)
    {
        return;
    }

    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    drawList->PushClipRect(min, max, true);
    if (g_focusPickCursorActive)
    {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const ImU32 cursorColor = IM_COL32(235, 238, 236, 245);
        constexpr float outer = 13.0f;
        constexpr float corner = 5.5f;
        constexpr float box = 3.5f;
        constexpr float thickness = 1.5f;
        drawList->PathLineTo(ImVec2(m.x - outer + corner, m.y - outer));
        drawList->PathLineTo(ImVec2(m.x - outer, m.y - outer));
        drawList->PathLineTo(ImVec2(m.x - outer, m.y - outer + corner));
        drawList->PathStroke(cursorColor, 0, thickness);
        drawList->PathLineTo(ImVec2(m.x + outer - corner, m.y - outer));
        drawList->PathLineTo(ImVec2(m.x + outer, m.y - outer));
        drawList->PathLineTo(ImVec2(m.x + outer, m.y - outer + corner));
        drawList->PathStroke(cursorColor, 0, thickness);
        drawList->PathLineTo(ImVec2(m.x - outer + corner, m.y + outer));
        drawList->PathLineTo(ImVec2(m.x - outer, m.y + outer));
        drawList->PathLineTo(ImVec2(m.x - outer, m.y + outer - corner));
        drawList->PathStroke(cursorColor, 0, thickness);
        drawList->PathLineTo(ImVec2(m.x + outer - corner, m.y + outer));
        drawList->PathLineTo(ImVec2(m.x + outer, m.y + outer));
        drawList->PathLineTo(ImVec2(m.x + outer, m.y + outer - corner));
        drawList->PathStroke(cursorColor, 0, thickness);
        drawList->AddRect(ImVec2(m.x - box, m.y - box), ImVec2(m.x + box, m.y + box), cursorColor, 0.0f, 0, thickness);
    }

    const auto drawFocusFrame = [&](const ImVec2& p, ImU32 color, float outer, float corner, float box, float thickness) {
        drawList->PathLineTo(ImVec2(p.x - outer + corner, p.y - outer));
        drawList->PathLineTo(ImVec2(p.x - outer, p.y - outer));
        drawList->PathLineTo(ImVec2(p.x - outer, p.y - outer + corner));
        drawList->PathStroke(color, 0, thickness);
        drawList->PathLineTo(ImVec2(p.x + outer - corner, p.y - outer));
        drawList->PathLineTo(ImVec2(p.x + outer, p.y - outer));
        drawList->PathLineTo(ImVec2(p.x + outer, p.y - outer + corner));
        drawList->PathStroke(color, 0, thickness);
        drawList->PathLineTo(ImVec2(p.x - outer + corner, p.y + outer));
        drawList->PathLineTo(ImVec2(p.x - outer, p.y + outer));
        drawList->PathLineTo(ImVec2(p.x - outer, p.y + outer - corner));
        drawList->PathStroke(color, 0, thickness);
        drawList->PathLineTo(ImVec2(p.x + outer - corner, p.y + outer));
        drawList->PathLineTo(ImVec2(p.x + outer, p.y + outer));
        drawList->PathLineTo(ImVec2(p.x + outer, p.y + outer - corner));
        drawList->PathStroke(color, 0, thickness);
        drawList->AddRect(ImVec2(p.x - box, p.y - box), ImVec2(p.x + box, p.y + box), color, 0.0f, 0, thickness);
    };

    const auto drawTarget = [&](const Vec3& point, ImU32 color, float radius, float thickness) {
        const ProjectedPoint projected = ProjectWorldToScreen(point.x, point.y, point.z, center, scale);
        if (projected.depth <= 0.05f)
        {
            return;
        }
        const ImVec2 p = projected.screen;
        drawFocusFrame(p, color, radius + 2.0f, 5.5f, 3.5f, thickness);
    };

    if (showHover)
    {
        drawTarget(*g_focusPickHoverPoint, IM_COL32(232, 235, 233, 235), 11.0f, 2.0f);
        float focusDistance = 0.0f;
        ImVec2 screenPoint{};
        if (TryPickViewportFocusPoint(min, max, ImGui::GetIO().MousePos, nullptr, &focusDistance, &screenPoint))
        {
            char text[64]{};
            std::snprintf(text, sizeof(text), "Focus %.1f m", focusDistance);
            const ImVec2 textSize = ImGui::CalcTextSize(text);
            const ImVec2 padding(8.0f, 5.0f);
            ImVec2 textMin(screenPoint.x + 14.0f, screenPoint.y - textSize.y - 12.0f);
            textMin.x = std::clamp(textMin.x, min.x + 8.0f, max.x - textSize.x - padding.x * 2.0f - 8.0f);
            textMin.y = std::clamp(textMin.y, min.y + 8.0f, max.y - textSize.y - padding.y * 2.0f - 8.0f);
            const ImVec2 textMax(textMin.x + textSize.x + padding.x * 2.0f, textMin.y + textSize.y + padding.y * 2.0f);
            drawList->AddRectFilled(textMin, textMax, IM_COL32(8, 10, 10, 190), 4.0f);
            drawList->AddRect(textMin, textMax, IM_COL32(172, 178, 175, 210), 4.0f);
            drawList->AddText(ImVec2(textMin.x + padding.x, textMin.y + padding.y), IM_COL32(232, 235, 233, 255), text);
        }
    }
    else if (g_focusPickMode)
    {
        const char* text = Tr("Click terrain to set focus distance", "地形をクリックしてフォーカス距離を設定");
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 padding(9.0f, 6.0f);
        const ImVec2 textMin(min.x + 14.0f, max.y - textSize.y - padding.y * 2.0f - 14.0f);
        const ImVec2 textMax(textMin.x + textSize.x + padding.x * 2.0f, textMin.y + textSize.y + padding.y * 2.0f);
        drawList->AddRectFilled(textMin, textMax, IM_COL32(8, 10, 10, 190), 4.0f);
        drawList->AddText(ImVec2(textMin.x + padding.x, textMin.y + padding.y), IM_COL32(232, 235, 233, 255), text);
    }

    drawList->PopClipRect();
}

void DrawSunDirectionGizmo(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
{
    const double now = ImGui::GetTime();
    if (!g_sunDirectionDragActive && now >= g_sunDirectionGizmoVisibleUntil)
    {
        return;
    }

    const float fadeAlpha = g_sunDirectionDragActive ? 1.0f : static_cast<float>(std::clamp((g_sunDirectionGizmoVisibleUntil - now) / 0.35, 0.0, 1.0));
    if (fadeAlpha <= 0.001f)
    {
        return;
    }

    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const rock::PreviewSettings& preview = g_graph.Settings().preview;
    const SunPositionDegrees sun = EffectiveSunPosition(preview);
    const float azimuth = sun.azimuth * kDegreesToRadians;
    const float elevation = sun.elevation * kDegreesToRadians;
    const float cosElevation = std::cos(elevation);
    const Vec3 sunDir(
        std::sin(azimuth) * cosElevation,
        std::sin(elevation),
        std::cos(azimuth) * cosElevation);
    const Vec3 horizontalDir(std::sin(azimuth), 0.0f, std::cos(azimuth));
    const float terrainSize = std::max(1.0f, preview.terrainSizeMeters);
    const float radius = terrainSize * 0.24f;
    const Vec3 origin(
        0.0f,
        terrain::SampleHeightAtWorld(g_graph.Evaluation().previewHeightfield, 0.0f, 0.0f) + terrainSize * 0.018f,
        0.0f);

    const auto color = [fadeAlpha](int r, int g, int b, int a) {
        return IM_COL32(r, g, b, static_cast<int>(std::clamp(static_cast<float>(a) * fadeAlpha, 0.0f, 255.0f)));
    };
    const auto worldPoint = [](const Vec3& a, const Vec3& b, float amount) {
        return Vec3(a.x + b.x * amount, a.y + b.y * amount, a.z + b.z * amount);
    };
    const auto project = [&](const Vec3& p) {
        return ProjectWorldToScreen(p.x, p.y, p.z, center, scale);
    };
    const auto drawWorldLine = [&](const Vec3& a, const Vec3& b, ImU32 lineColor, float thickness) {
        const ProjectedPoint pa = project(a);
        const ProjectedPoint pb = project(b);
        if (pa.depth > 0.05f && pb.depth > 0.05f)
        {
            drawList->AddLine(pa.screen, pb.screen, lineColor, thickness);
        }
    };
    const auto drawRing = [&](float ringRadius, ImU32 ringColor, float thickness) {
        constexpr int kSegments = 96;
        ProjectedPoint prev{};
        bool havePrev = false;
        for (int i = 0; i <= kSegments; ++i)
        {
            const float t = (static_cast<float>(i) / static_cast<float>(kSegments)) * 2.0f * 3.1415926535f;
            const Vec3 p(origin.x + std::sin(t) * ringRadius, origin.y, origin.z + std::cos(t) * ringRadius);
            const ProjectedPoint current = project(p);
            if (havePrev && prev.depth > 0.05f && current.depth > 0.05f)
            {
                drawList->AddLine(prev.screen, current.screen, ringColor, thickness);
            }
            prev = current;
            havePrev = true;
        }
    };

    drawList->PushClipRect(min, max, true);

    drawRing(radius, color(108, 151, 255, 178), 2.2f);
    drawRing(radius * 0.67f, color(108, 151, 255, 82), 1.2f);
    drawRing(radius * 0.34f, color(108, 151, 255, 58), 1.0f);

    drawWorldLine(origin, worldPoint(origin, Vec3(0.0f, 0.0f, 1.0f), radius), color(145, 171, 255, 128), 1.4f);

    constexpr float kSunArrowTipRadius = 0.18f;
    const float projectionLength = radius;
    const Vec3 projectionEnd = worldPoint(origin, horizontalDir, projectionLength);
    drawWorldLine(origin, projectionEnd, color(124, 169, 255, 176), 2.0f);

    constexpr int kArcSegments = 48;
    ProjectedPoint prevArc{};
    bool havePrevArc = false;
    for (int i = 0; i <= kArcSegments; ++i)
    {
        const float t = static_cast<float>(i) / static_cast<float>(kArcSegments);
        const float angle = elevation * t;
        const Vec3 p(
            origin.x + horizontalDir.x * std::cos(angle) * radius,
            origin.y + std::sin(angle) * radius,
            origin.z + horizontalDir.z * std::cos(angle) * radius);
        const ProjectedPoint current = project(p);
        if (havePrevArc && prevArc.depth > 0.05f && current.depth > 0.05f)
        {
            drawList->AddLine(prevArc.screen, current.screen, color(255, 206, 112, 126), 1.6f);
        }
        prevArc = current;
        havePrevArc = true;
    }

    const ProjectedPoint originScreen = project(origin);
    const Vec3 sunArrowStart = worldPoint(origin, sunDir, radius);
    const Vec3 sunArrowEnd = worldPoint(origin, sunDir, radius * kSunArrowTipRadius);
    const ProjectedPoint sunStartScreen = project(sunArrowStart);
    const ProjectedPoint sunEndScreen = project(sunArrowEnd);
    if (sunStartScreen.depth > 0.05f && sunEndScreen.depth > 0.05f)
    {
        const ImU32 sunColor = color(255, 188, 76, 245);
        ImVec2 screenDir(sunEndScreen.screen.x - sunStartScreen.screen.x, sunEndScreen.screen.y - sunStartScreen.screen.y);
        const float screenLen = std::sqrt(screenDir.x * screenDir.x + screenDir.y * screenDir.y);
        if (screenLen > 0.001f)
        {
            screenDir.x /= screenLen;
            screenDir.y /= screenLen;
            const ImVec2 side(-screenDir.y, screenDir.x);
            constexpr float headLength = 15.0f;
            constexpr float headHalfWidth = 6.5f;
            const ImVec2 base(sunEndScreen.screen.x - screenDir.x * headLength, sunEndScreen.screen.y - screenDir.y * headLength);
            drawList->AddLine(sunStartScreen.screen, base, sunColor, 4.0f);
            drawList->AddTriangleFilled(
                sunEndScreen.screen,
                ImVec2(base.x + side.x * headHalfWidth, base.y + side.y * headHalfWidth),
                ImVec2(base.x - side.x * headHalfWidth, base.y - side.y * headHalfWidth),
                sunColor);
        }
    }
    if (originScreen.depth > 0.05f)
    {
        drawList->AddCircle(originScreen.screen, 6.5f, color(108, 151, 255, 235), 20, 2.0f);
    }

    drawList->PopClipRect();
}

const rock::PathPoint* FindPathPoint(const rock::PathSettings& path, rock::GraphId pointId)
{
    const auto it = std::ranges::find_if(path.points, [pointId](const rock::PathPoint& point) {
        return point.id == pointId;
    });
    return it != path.points.end() ? &*it : nullptr;
}

float PathPointDisplayHeight(const rock::PathPoint& point)
{
    switch (point.heightMode)
    {
    case rock::PathPointHeightMode::Absolute:
        return point.height;
    case rock::PathPointHeightMode::TerrainOffset:
        return terrain::SampleHeightAtWorld(g_graph.Evaluation().previewHeightfield, point.x, point.z) + point.heightOffset;
    case rock::PathPointHeightMode::ProjectToTerrain:
    default:
        return terrain::SampleHeightAtWorld(g_graph.Evaluation().previewHeightfield, point.x, point.z);
    }
}

const rock::PathPoint* FindConnectedPathPoint(const rock::PathSettings& path, rock::GraphId pointId, rock::GraphId excludePointId)
{
    for (const rock::PathEdge& edge : path.edges)
    {
        if (!edge.enabled)
        {
            continue;
        }
        if (edge.fromPoint == pointId && edge.toPoint != excludePointId)
        {
            return FindPathPoint(path, edge.toPoint);
        }
        if (edge.toPoint == pointId && edge.fromPoint != excludePointId)
        {
            return FindPathPoint(path, edge.fromPoint);
        }
    }
    return nullptr;
}

float CatmullRom(float p0, float p1, float p2, float p3, float t)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

int PathEdgeDisplayStepCount(const rock::PathEdge& edge)
{
    return edge.segmentType == rock::PathSegmentType::CatmullRom ? 16 : 1;
}

bool PathEdgePositionAt(const rock::PathSettings& path, const rock::PathEdge& edge, float t, Vec3* outPosition)
{
    const rock::PathPoint* a = FindPathPoint(path, edge.fromPoint);
    const rock::PathPoint* b = FindPathPoint(path, edge.toPoint);
    if (a == nullptr || b == nullptr || outPosition == nullptr)
    {
        return false;
    }

    t = std::clamp(t, 0.0f, 1.0f);
    if (edge.segmentType != rock::PathSegmentType::CatmullRom)
    {
        *outPosition = Vec3(
            std::lerp(a->x, b->x, t),
            std::lerp(PathPointDisplayHeight(*a), PathPointDisplayHeight(*b), t),
            std::lerp(a->z, b->z, t));
        return true;
    }

    const rock::PathPoint* p0 = FindConnectedPathPoint(path, a->id, b->id);
    const rock::PathPoint* p3 = FindConnectedPathPoint(path, b->id, a->id);
    if (p0 == nullptr)
    {
        p0 = a;
    }
    if (p3 == nullptr)
    {
        p3 = b;
    }
    *outPosition = Vec3(
        CatmullRom(p0->x, a->x, b->x, p3->x, t),
        CatmullRom(PathPointDisplayHeight(*p0), PathPointDisplayHeight(*a), PathPointDisplayHeight(*b), PathPointDisplayHeight(*p3), t),
        CatmullRom(p0->z, a->z, b->z, p3->z, t));
    return true;
}

void DrawPathMoveGizmo(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
{
    if (!g_pathMoveGizmoVisible || g_pathSelectionKind != PathSelectionKind::Point)
    {
        return;
    }
    Vec3 pivot;
    if (!SelectedPathPointPosition(&pivot))
    {
        return;
    }

    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const ProjectedPoint pivotScreen = ProjectWorldToScreen(pivot.x, pivot.y, pivot.z, center, scale);
    if (pivotScreen.depth <= 0.05f)
    {
        return;
    }

    const float axisLength = PathMoveGizmoAxisLength(pivot, center, scale);
    const ImU32 axisXColor = IM_COL32(255, 76, 76, 255);
    const ImU32 axisYColor = IM_COL32(92, 230, 92, 255);
    const ImU32 axisZColor = IM_COL32(82, 156, 255, 255);
    const ImU32 pivotColor = IM_COL32(255, 255, 255, 235);
    const bool centerActive = g_pathActiveMoveGizmoAxis == PathMoveGizmoAxis::Center;
    drawList->AddCircleFilled(pivotScreen.screen, centerActive ? 7.0f : 5.5f, pivotColor, 16);
    drawList->AddCircle(pivotScreen.screen, centerActive ? 9.0f : 7.5f, IM_COL32(30, 34, 38, 255), 16, centerActive ? 2.5f : 1.5f);

    struct AxisDrawInfo
    {
        PathMoveGizmoAxis axis;
        ImU32 color;
    };
    const AxisDrawInfo axes[] = {
        {PathMoveGizmoAxis::X, axisXColor},
        {PathMoveGizmoAxis::Y, axisYColor},
        {PathMoveGizmoAxis::Z, axisZColor},
    };

    for (const AxisDrawInfo& axisInfo : axes)
    {
        const Vec3 dir = PathMoveGizmoAxisDirection(axisInfo.axis);
        const ProjectedPoint end = ProjectWorldToScreen(
            pivot.x + dir.x * axisLength,
            pivot.y + dir.y * axisLength,
            pivot.z + dir.z * axisLength,
            center,
            scale);
        if (end.depth <= 0.05f)
        {
            continue;
        }
        const bool active = g_pathActiveMoveGizmoAxis == axisInfo.axis;
        const float thickness = active ? 4.0f : 2.6f;
        drawList->AddLine(pivotScreen.screen, end.screen, axisInfo.color, thickness);

        ImVec2 screenDir(end.screen.x - pivotScreen.screen.x, end.screen.y - pivotScreen.screen.y);
        const float screenLen = std::sqrt(screenDir.x * screenDir.x + screenDir.y * screenDir.y);
        if (screenLen > 0.001f)
        {
            screenDir.x /= screenLen;
            screenDir.y /= screenLen;
            const ImVec2 side(-screenDir.y, screenDir.x);
            const float headLength = active ? 14.0f : 12.0f;
            const float headHalfWidth = active ? 6.0f : 5.0f;
            const ImVec2 base(end.screen.x - screenDir.x * headLength, end.screen.y - screenDir.y * headLength);
            drawList->AddTriangleFilled(
                end.screen,
                ImVec2(base.x + side.x * headHalfWidth, base.y + side.y * headHalfWidth),
                ImVec2(base.x - side.x * headHalfWidth, base.y - side.y * headHalfWidth),
                axisInfo.color);
        }
    }
}

void DrawPathViewportOverlay(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
{
    const rock::Node* node = SelectedPathNode();
    if (node == nullptr)
    {
        return;
    }

    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const ImU32 edgeColor = IM_COL32(90, 182, 255, 230);
    const ImU32 selectedEdgeColor = IM_COL32(255, 210, 92, 255);
    const ImU32 pointColor = IM_COL32(250, 247, 224, 255);
    const ImU32 selectedPointColor = IM_COL32(255, 210, 92, 255);
    const ImU32 pointBorderColor = IM_COL32(31, 35, 38, 255);
    const ImU32 terrainBoundsColor = IM_COL32(150, 156, 158, 135);

    drawList->PushClipRect(min, max, true);
    {
        const float halfSize = std::max(1.0f, g_graph.Settings().preview.terrainSizeMeters) * 0.5f;
        const ProjectedPoint corners[] = {
            ProjectWorldToScreen(-halfSize, 0.0f, -halfSize, center, scale),
            ProjectWorldToScreen( halfSize, 0.0f, -halfSize, center, scale),
            ProjectWorldToScreen( halfSize, 0.0f,  halfSize, center, scale),
            ProjectWorldToScreen(-halfSize, 0.0f,  halfSize, center, scale),
        };
        for (int i = 0; i < 4; ++i)
        {
            const ProjectedPoint& a = corners[i];
            const ProjectedPoint& b = corners[(i + 1) % 4];
            if (a.depth > 0.05f && b.depth > 0.05f)
            {
                drawList->AddLine(a.screen, b.screen, terrainBoundsColor, 1.5f);
            }
        }
    }
    for (const rock::PathEdge& edge : node->path.edges)
    {
        if (!edge.enabled)
        {
            continue;
        }
        Vec3 prev;
        if (!PathEdgePositionAt(node->path, edge, 0.0f, &prev))
        {
            continue;
        }
        ProjectedPoint prevProjected = ProjectWorldToScreen(prev.x, prev.y, prev.z, center, scale);
        const int steps = PathEdgeDisplayStepCount(edge);
        for (int step = 1; step <= steps; ++step)
        {
            Vec3 next;
            if (!PathEdgePositionAt(node->path, edge, static_cast<float>(step) / static_cast<float>(steps), &next))
            {
                break;
            }
            const ProjectedPoint nextProjected = ProjectWorldToScreen(next.x, next.y, next.z, center, scale);
            if (prevProjected.depth > 0.05f && nextProjected.depth > 0.05f)
            {
                const bool selected = g_pathSelectionKind == PathSelectionKind::Edge && g_pathSelectedElementId == edge.id;
                drawList->AddLine(prevProjected.screen, nextProjected.screen, selected ? selectedEdgeColor : edgeColor, selected ? 4.0f : 2.5f);
            }
            prevProjected = nextProjected;
        }
    }
    for (const rock::PathPoint& point : node->path.points)
    {
        const ProjectedPoint projected = ProjectWorldToScreen(point.x, PathPointDisplayHeight(point), point.z, center, scale);
        if (projected.depth <= 0.05f)
        {
            continue;
        }
        const bool selected = g_pathSelectionKind == PathSelectionKind::Point && g_pathSelectedElementId == point.id;
        drawList->AddCircleFilled(projected.screen, selected ? 6.5f : 5.0f, selected ? selectedPointColor : pointColor, 16);
        drawList->AddCircle(projected.screen, selected ? 6.5f : 5.0f, pointBorderColor, 16, 1.5f);
    }
    DrawPathMoveGizmo(drawList, min, max);
    const char* modeText = "Path mode: click/select, Ctrl+edge inserts, Del deletes, Enter finishes";
    const ImVec2 textSize = ImGui::CalcTextSize(modeText);
    const ImVec2 padding(9.0f, 6.0f);
    const ImVec2 textMin(min.x + 14.0f, max.y - textSize.y - padding.y * 2.0f - 14.0f);
    const ImVec2 textMax(textMin.x + textSize.x + padding.x * 2.0f, textMin.y + textSize.y + padding.y * 2.0f);
    drawList->AddRectFilled(textMin, textMax, IM_COL32(8, 10, 10, 190), 4.0f);
    drawList->AddText(ImVec2(textMin.x + padding.x, textMin.y + padding.y), IM_COL32(232, 235, 233, 255), modeText);
    drawList->PopClipRect();
}

ImVec2 RotatePoint(float x, float y, float z, float, float)
{
    return ProjectWorldNormalized(x, y, z);
}

ImU32 ColorToU32(const ImVec4& color);
ImU32 ThemeColor(const std::string& name, const ImVec4& fallback);

bool EnsureMeshPreviewRenderTarget(int width, int height, std::string* error)
{
    const DXGI_FORMAT meshPreviewColorFormat = MeshPreviewColorFormat();
    const int shadowResolution = std::clamp(g_graph.Settings().preview.shadowMapResolution, 512, 4096);
    if (g_gpuMeshPreview.colorTarget && g_gpuMeshPreview.postTarget && g_gpuMeshPreview.outputTarget &&
        g_gpuMeshPreview.exposureTargets[0] && g_gpuMeshPreview.exposureTargets[1] && g_gpuMeshPreview.shadowTarget &&
        g_gpuMeshPreview.width == width && g_gpuMeshPreview.height == height &&
        g_gpuMeshPreview.shadowMapResolution == shadowResolution &&
        g_gpuMeshPreview.colorFormat == meshPreviewColorFormat)
    {
        return true;
    }

    try
    {
        WaitForLastSubmittedFrame();
        g_gpuMeshPreview.colorTarget.Reset();
        g_gpuMeshPreview.postTarget.Reset();
        g_gpuMeshPreview.outputTarget.Reset();
        g_gpuMeshPreview.exposureTargets[0].Reset();
        g_gpuMeshPreview.exposureTargets[1].Reset();
        g_gpuMeshPreview.depthTarget.Reset();
        g_gpuMeshPreview.sceneDepthTarget.Reset();
        g_gpuMeshPreview.shadowTarget.Reset();
        g_gpuMeshPreview.width = width;
        g_gpuMeshPreview.height = height;
        g_gpuMeshPreview.shadowMapResolution = shadowResolution;
        g_gpuMeshPreview.colorFormat = meshPreviewColorFormat;
        g_gpuMeshPreview.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_gpuMeshPreview.postState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_gpuMeshPreview.outputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_gpuMeshPreview.exposureStates = {D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET};
        g_gpuMeshPreview.exposureInitialized = false;
        g_gpuMeshPreview.exposureHistoryIndex = 0;
        g_gpuMeshPreview.shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_gpuMeshPreview.sceneDepthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!g_meshPreviewPipelines.rtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc =
                DescriptorHeapDesc(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 5);
            ThrowIfFailed(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_meshPreviewPipelines.rtvHeap)), "Create mesh RTV heap failed");
            g_gpuMeshPreview.rtvCpu = g_meshPreviewPipelines.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            g_gpuMeshPreview.postRtvCpu = g_gpuMeshPreview.rtvCpu;
            g_gpuMeshPreview.postRtvCpu.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            g_gpuMeshPreview.outputRtvCpu = g_gpuMeshPreview.postRtvCpu;
            g_gpuMeshPreview.outputRtvCpu.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            g_gpuMeshPreview.exposureRtvCpu[0] = g_gpuMeshPreview.outputRtvCpu;
            g_gpuMeshPreview.exposureRtvCpu[0].ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            g_gpuMeshPreview.exposureRtvCpu[1] = g_gpuMeshPreview.exposureRtvCpu[0];
            g_gpuMeshPreview.exposureRtvCpu[1].ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        if (!g_meshPreviewPipelines.dsvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc =
                DescriptorHeapDesc(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2);
            ThrowIfFailed(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_meshPreviewPipelines.dsvHeap)), "Create mesh DSV heap failed");
            g_gpuMeshPreview.dsvCpu = g_meshPreviewPipelines.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_gpuMeshPreview.shadowDsvCpu = g_gpuMeshPreview.dsvCpu;
            g_gpuMeshPreview.shadowDsvCpu.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        }
        if (!g_gpuMeshPreview.srvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.srvCpu, &g_gpuMeshPreview.srvGpu);
            g_gpuMeshPreview.srvAllocated = true;
        }
        if (!g_gpuMeshPreview.postSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.postSrvCpu, &g_gpuMeshPreview.postSrvGpu);
            g_gpuMeshPreview.postSrvAllocated = true;
        }
        if (!g_gpuMeshPreview.outputSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.outputSrvCpu, &g_gpuMeshPreview.outputSrvGpu);
            g_gpuMeshPreview.outputSrvAllocated = true;
        }
        if (!g_gpuMeshPreview.exposureSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.exposureSrvCpu[0], &g_gpuMeshPreview.exposureSrvGpu[0]);
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.exposureSrvCpu[1], &g_gpuMeshPreview.exposureSrvGpu[1]);
            g_gpuMeshPreview.exposureSrvAllocated = true;
        }
        if (!g_gpuMeshPreview.shadowSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.shadowSrvCpu, &g_gpuMeshPreview.shadowSrvGpu);
            g_gpuMeshPreview.shadowSrvAllocated = true;
        }
        if (!g_gpuMeshPreview.sceneDepthSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.sceneDepthSrvCpu, &g_gpuMeshPreview.sceneDepthSrvGpu);
            g_gpuMeshPreview.sceneDepthSrvAllocated = true;
        }

        const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);

        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = meshPreviewColorFormat;
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                meshPreviewColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.colorTarget)),
                "Create mesh color RT failed");
            g_device->CreateRenderTargetView(g_gpuMeshPreview.colorTarget.Get(), nullptr, g_gpuMeshPreview.rtvCpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = meshPreviewColorFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.colorTarget.Get(), &srvDesc, g_gpuMeshPreview.srvCpu);
        }
        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = meshPreviewColorFormat;
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                meshPreviewColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.postTarget)),
                "Create mesh post RT failed");
            g_device->CreateRenderTargetView(g_gpuMeshPreview.postTarget.Get(), nullptr, g_gpuMeshPreview.postRtvCpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = meshPreviewColorFormat;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.postTarget.Get(), &srvDesc, g_gpuMeshPreview.postSrvCpu);
        }
        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.outputTarget)),
                "Create mesh output RT failed");
            g_device->CreateRenderTargetView(g_gpuMeshPreview.outputTarget.Get(), nullptr, g_gpuMeshPreview.outputRtvCpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.outputTarget.Get(), &srvDesc, g_gpuMeshPreview.outputSrvCpu);
        }
        for (int i = 0; i < 2; ++i)
        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = DXGI_FORMAT_R16_FLOAT;
            clearVal.Color[0] = 0.0f;
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                1u, 1u, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.exposureTargets[static_cast<size_t>(i)])),
                "Create mesh exposure RT failed");
            g_device->CreateRenderTargetView(g_gpuMeshPreview.exposureTargets[static_cast<size_t>(i)].Get(), nullptr, g_gpuMeshPreview.exposureRtvCpu[static_cast<size_t>(i)]);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R16_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.exposureTargets[static_cast<size_t>(i)].Get(), &srvDesc, g_gpuMeshPreview.exposureSrvCpu[static_cast<size_t>(i)]);
        }
        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = DXGI_FORMAT_D32_FLOAT;
            clearVal.DepthStencil.Depth = 1.0f;
            // Typeless format so we can bind both as DSV (D32_FLOAT) for the
            // mesh pass and as SRV (R32_FLOAT) for the cloud ray-march pass.
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.depthTarget)),
                "Create mesh depth buffer failed");
            g_gpuMeshPreview.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            g_device->CreateDepthStencilView(g_gpuMeshPreview.depthTarget.Get(), &dsvDesc, g_gpuMeshPreview.dsvCpu);

            if (!g_gpuMeshPreview.depthSrvAllocated)
            {
                AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.depthSrvCpu, &g_gpuMeshPreview.depthSrvGpu);
                g_gpuMeshPreview.depthSrvAllocated = true;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.depthTarget.Get(), &srvDesc, g_gpuMeshPreview.depthSrvCpu);
        }
        {
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_NONE);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_gpuMeshPreview.sceneDepthTarget)),
                "Create mesh scene depth copy failed");
            g_gpuMeshPreview.sceneDepthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.sceneDepthTarget.Get(), &srvDesc, g_gpuMeshPreview.sceneDepthSrvCpu);
        }
        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = DXGI_FORMAT_D32_FLOAT;
            clearVal.DepthStencil.Depth = 1.0f;
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(shadowResolution), static_cast<UINT>(shadowResolution),
                DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.shadowTarget)),
                "Create mesh shadow map failed");
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            g_device->CreateDepthStencilView(g_gpuMeshPreview.shadowTarget.Get(), &dsvDesc, g_gpuMeshPreview.shadowDsvCpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.shadowTarget.Get(), &srvDesc, g_gpuMeshPreview.shadowSrvCpu);
        }
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

void UpdateMeshPreviewBuffers(const rock::MeshData& mesh)
{
    g_gpuMeshPreview.vertexBuffer.Reset();
    g_gpuMeshPreview.indexBuffer.Reset();
    g_gpuMeshPreview.edgeIndexBuffer.Reset();
    g_gpuMeshPreview.vertexCount = 0;
    g_gpuMeshPreview.triIndexCount = 0;
    g_gpuMeshPreview.edgeIndexCount = 0;

    if (mesh.vertices.empty()) return;

    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RANGE readRange{};
    void* mapped = nullptr;

    const UINT64 vbSize = mesh.vertices.size() * sizeof(rock::MeshVertex);
    const D3D12_RESOURCE_DESC vbDesc = BufferResourceDesc(vbSize);
    ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_gpuMeshPreview.vertexBuffer)), "Create mesh VB failed");
    g_gpuMeshPreview.vertexBuffer->Map(0, &readRange, &mapped);
    std::memcpy(mapped, mesh.vertices.data(), static_cast<size_t>(vbSize));
    g_gpuMeshPreview.vertexBuffer->Unmap(0, nullptr);
    g_gpuMeshPreview.vertexCount = static_cast<UINT>(mesh.vertices.size());

    if (!mesh.triangles.empty())
    {
        std::vector<UINT> indices;
        indices.reserve(mesh.triangles.size() * 3);
        for (const auto& tri : mesh.triangles) { indices.push_back(tri.a); indices.push_back(tri.b); indices.push_back(tri.c); }
        const UINT64 ibSize = indices.size() * sizeof(UINT);
        const D3D12_RESOURCE_DESC ibDesc = BufferResourceDesc(ibSize);
        ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&g_gpuMeshPreview.indexBuffer)), "Create mesh IB failed");
        g_gpuMeshPreview.indexBuffer->Map(0, &readRange, &mapped);
        std::memcpy(mapped, indices.data(), static_cast<size_t>(ibSize));
        g_gpuMeshPreview.indexBuffer->Unmap(0, nullptr);
        g_gpuMeshPreview.triIndexCount = static_cast<UINT>(indices.size());
    }

    if (!mesh.edges.empty())
    {
        std::vector<UINT> edgeIdx;
        edgeIdx.reserve(mesh.edges.size() * 2);
        for (const auto& e : mesh.edges) { edgeIdx.push_back(e.a); edgeIdx.push_back(e.b); }
        const UINT64 ebSize = edgeIdx.size() * sizeof(UINT);
        const D3D12_RESOURCE_DESC ebDesc = BufferResourceDesc(ebSize);
        ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &ebDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&g_gpuMeshPreview.edgeIndexBuffer)), "Create mesh edge IB failed");
        g_gpuMeshPreview.edgeIndexBuffer->Map(0, &readRange, &mapped);
        std::memcpy(mapped, edgeIdx.data(), static_cast<size_t>(ebSize));
        g_gpuMeshPreview.edgeIndexBuffer->Unmap(0, nullptr);
        g_gpuMeshPreview.edgeIndexCount = static_cast<UINT>(edgeIdx.size());
    }
}

void EnsureGridPreviewBuffer()
{
    const rock::PreviewSettings& preview = g_graph.Settings().preview;
    const int cellCount = std::clamp(preview.gridCellCount, 1, 200);
    const float cellSizeMeters = std::clamp(preview.gridCellSizeMeters, 1.0f, 10000.0f);
    if (g_gpuMeshPreview.gridVertexBuffer &&
        g_gpuMeshPreview.gridVertexCount > 0 &&
        g_gpuMeshPreview.gridCellCount == cellCount &&
        g_gpuMeshPreview.gridCellSizeMeters == cellSizeMeters)
    {
        return;
    }

    g_gpuMeshPreview.gridVertexBuffer.Reset();
    g_gpuMeshPreview.gridVertexCount = 0;
    std::vector<rock::MeshVertex> vertices;
    vertices.reserve(static_cast<size_t>(cellCount + 1) * 4u);
    const auto addVertex = [&](float x, float z, float axisTag) {
        vertices.push_back({x, 0.0f, z, 0.0f, 1.0f, 0.0f, axisTag});
    };
    const float halfExtent = static_cast<float>(cellCount) * cellSizeMeters * 0.5f;
    for (int i = 0; i <= cellCount; ++i)
    {
        const float offset = (static_cast<float>(i) - static_cast<float>(cellCount) * 0.5f) * cellSizeMeters;
        const bool axisLine = std::abs(offset) <= 0.0001f;
        if (axisLine)
        {
            continue;
        }
        addVertex(-halfExtent, offset, 0.0f);
        addVertex(halfExtent, offset, 0.0f);
        addVertex(offset, -halfExtent, 0.0f);
        addVertex(offset, halfExtent, 0.0f);
    }
    if (cellCount % 2 == 0)
    {
        addVertex(-halfExtent, 0.0f, -1.0f);
        addVertex(halfExtent, 0.0f, -1.0f);
        addVertex(0.0f, -halfExtent, -2.0f);
        addVertex(0.0f, halfExtent, -2.0f);
    }

    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const UINT64 vbSize = vertices.size() * sizeof(rock::MeshVertex);
    const D3D12_RESOURCE_DESC vbDesc = BufferResourceDesc(vbSize);
    ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_gpuMeshPreview.gridVertexBuffer)), "Create grid VB failed");
    D3D12_RANGE readRange{};
    void* mapped = nullptr;
    g_gpuMeshPreview.gridVertexBuffer->Map(0, &readRange, &mapped);
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(vbSize));
    g_gpuMeshPreview.gridVertexBuffer->Unmap(0, nullptr);
    g_gpuMeshPreview.gridVertexCount = static_cast<UINT>(vertices.size());
    g_gpuMeshPreview.gridCellCount = cellCount;
    g_gpuMeshPreview.gridCellSizeMeters = cellSizeMeters;
}

void EnsureWaterPreviewBuffer(float terrainSizeMeters, const rock::HeightfieldGrid& grid, uint64_t gridVersion)
{
    const rock::PreviewSettings& preview = g_graph.Settings().preview;
    const bool enabled = preview.waterEnabled;
    const float terrainSize = std::max(1.0f, terrainSizeMeters);
    const float waterLevel = preview.waterLevelMeters;
    const float waterOpacity = std::clamp(preview.waterOpacity, 0.0f, 1.0f);
    if (g_gpuMeshPreview.waterVertexBuffer &&
        g_gpuMeshPreview.waterIndexBuffer &&
        g_gpuMeshPreview.waterIndexCount > 0 &&
        g_gpuMeshPreview.waterEnabled == enabled &&
        g_gpuMeshPreview.waterLevelMeters == waterLevel &&
        g_gpuMeshPreview.waterOpacity == waterOpacity &&
        g_gpuMeshPreview.waterColor == preview.waterColor &&
        g_gpuMeshPreview.waterTerrainSizeMeters == terrainSize &&
        g_gpuMeshPreview.waterHeightfieldVersion == gridVersion)
    {
        return;
    }

    g_gpuMeshPreview.waterVertexBuffer.Reset();
    g_gpuMeshPreview.waterIndexBuffer.Reset();
    g_gpuMeshPreview.waterIndexCount = 0;
    g_gpuMeshPreview.waterVertexCount = 0;
    g_gpuMeshPreview.waterEnabled = enabled;
    g_gpuMeshPreview.waterLevelMeters = waterLevel;
    g_gpuMeshPreview.waterOpacity = waterOpacity;
    g_gpuMeshPreview.waterColor = preview.waterColor;
    g_gpuMeshPreview.waterTerrainSizeMeters = terrainSize;
    g_gpuMeshPreview.waterHeightfieldVersion = gridVersion;
    if (!enabled)
    {
        return;
    }

    const float half = terrainSize * 0.5f;
    const auto makeVertex = [&](float x, float y, float z, float nx, float ny, float nz) {
        return rock::MeshVertex{
            x, y, z,
            nx, ny, nz,
            waterOpacity,
            preview.waterColor[0], preview.waterColor[1], preview.waterColor[2]};
    };

    // ハイトフィールドを UV(0-1) でバイリニアサンプリング
    // u: worldX = lerp(-half, half, u)
    // v: worldZ = lerp( half,-half, v)
    const auto sampleH = [&](float u, float v) -> float {
        if (grid.heights.empty() || grid.resolution < 2) return 0.0f;
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);
        float fx = u * (grid.resolution - 1);
        float fz = v * (grid.resolution - 1);
        int x0 = std::clamp(static_cast<int>(fx), 0, grid.resolution - 2);
        int z0 = std::clamp(static_cast<int>(fz), 0, grid.resolution - 2);
        float tx = fx - x0, tz = fz - z0;
        float h00 = grid.heights[ z0      * grid.resolution + x0];
        float h10 = grid.heights[ z0      * grid.resolution + x0 + 1];
        float h01 = grid.heights[(z0 + 1) * grid.resolution + x0];
        float h11 = grid.heights[(z0 + 1) * grid.resolution + x0 + 1];
        return h00*(1-tx)*(1-tz) + h10*tx*(1-tz) + h01*(1-tx)*tz + h11*tx*tz;
    };

    std::vector<rock::MeshVertex> vertices;
    std::vector<UINT> indices;

    struct WaterClipPoint
    {
        float u = 0.0f;
        float v = 0.0f;
        float depth = 0.0f;
    };

    const auto makeClipPoint = [&](float u, float v) -> WaterClipPoint {
        return WaterClipPoint{u, v, waterLevel - sampleH(u, v)};
    };
    const auto interpolateClipPoint = [](const WaterClipPoint& a, const WaterClipPoint& b) -> WaterClipPoint {
        const float denom = a.depth - b.depth;
        const float t = std::abs(denom) > 1e-6f ? std::clamp(a.depth / denom, 0.0f, 1.0f) : 0.5f;
        return WaterClipPoint{
            a.u + (b.u - a.u) * t,
            a.v + (b.v - a.v) * t,
            0.0f};
    };
    const auto addSurfaceVertex = [&](const WaterClipPoint& p) -> UINT {
        const float wx = -half + p.u * terrainSize;
        const float wz =  half - p.v * terrainSize;
        vertices.push_back(makeVertex(wx, waterLevel, wz, 0.0f, 1.0f, 0.0f));
        return static_cast<UINT>(vertices.size() - 1);
    };

    // 水面上面: ハイトフィールドでクリップし、水位より低い領域だけ描画する。
    const int surfaceRes = grid.resolution >= 2 ? std::min(grid.resolution - 1, 512) : 1;
    vertices.reserve(static_cast<size_t>(surfaceRes) * static_cast<size_t>(surfaceRes) * 4u);
    indices.reserve(static_cast<size_t>(surfaceRes) * static_cast<size_t>(surfaceRes) * 6u);
    for (int z = 0; z < surfaceRes; ++z)
    {
        const float v0 = static_cast<float>(z) / surfaceRes;
        const float v1 = static_cast<float>(z + 1) / surfaceRes;
        for (int x = 0; x < surfaceRes; ++x)
        {
            const float u0 = static_cast<float>(x) / surfaceRes;
            const float u1 = static_cast<float>(x + 1) / surfaceRes;
            const WaterClipPoint corners[4] = {
                makeClipPoint(u0, v0),
                makeClipPoint(u1, v0),
                makeClipPoint(u1, v1),
                makeClipPoint(u0, v1),
            };

            std::vector<WaterClipPoint> polygon;
            polygon.reserve(6);
            for (int i = 0; i < 4; ++i)
            {
                const WaterClipPoint& a = corners[i];
                const WaterClipPoint& b = corners[(i + 1) % 4];
                const bool aWet = a.depth > 0.0f;
                const bool bWet = b.depth > 0.0f;
                if (aWet)
                {
                    polygon.push_back(a);
                }
                if (aWet != bWet)
                {
                    polygon.push_back(interpolateClipPoint(a, b));
                }
            }
            if (polygon.size() < 3)
            {
                continue;
            }

            const UINT base = addSurfaceVertex(polygon[0]);
            UINT prev = addSurfaceVertex(polygon[1]);
            for (size_t i = 2; i < polygon.size(); ++i)
            {
                const UINT next = addSurfaceVertex(polygon[i]);
                indices.push_back(base);
                indices.push_back(prev);
                indices.push_back(next);
                prev = next;
            }
        }
    }
    const size_t surfaceIndexCount = indices.size();

    // 断面側壁 (水位 > 0 の場合のみ): 地形高さに沿った輪郭
    if (waterLevel > 0.01f)
    {
        const int wallRes = grid.resolution >= 2 ? std::min(grid.resolution - 1, 256) : 1;

        // 各辺の定義: 法線 nx/nz, パラメータ t ∈ [0,1] での worldX/Z と UV を返す lambda
        struct WallConfig {
            float nx, nz;
            std::function<float(float)> worldX, worldZ, uFn, vFn;
        };
        const WallConfig walls[4] = {
            // 前面 (z=+half), x: -half → +half
            { 0,  1,
              [&](float t){ return -half + t * terrainSize; },
              [&](float  ){ return  half; },
              [&](float t){ return t; },
              [&](float  ){ return 0.0f; } },
            // 右面 (x=+half), z: +half → -half
            { 1,  0,
              [&](float  ){ return  half; },
              [&](float t){ return  half - t * terrainSize; },
              [&](float  ){ return 1.0f; },
              [&](float t){ return t; } },
            // 後面 (z=-half), x: +half → -half
            { 0, -1,
              [&](float t){ return  half - t * terrainSize; },
              [&](float  ){ return -half; },
              [&](float t){ return 1.0f - t; },
              [&](float  ){ return 1.0f; } },
            // 左面 (x=-half), z: -half → +half
            {-1,  0,
              [&](float  ){ return -half; },
              [&](float t){ return -half + t * terrainSize; },
              [&](float  ){ return 0.0f; },
              [&](float t){ return 1.0f - t; } },
        };

        for (const auto& wl : walls)
        {
            const UINT base = static_cast<UINT>(vertices.size());
            // 各列: 頂点 2 つ (上辺 waterLevel, 下辺 min(terrainH, waterLevel))
            for (int i = 0; i <= wallRes; ++i)
            {
                float t = static_cast<float>(i) / wallRes;
                float wx = wl.worldX(t);
                float wz = wl.worldZ(t);
                float u  = wl.uFn(t);
                float v  = wl.vFn(t);
                float terrainH = sampleH(u, v);
                float botY = std::min(terrainH, waterLevel);
                vertices.push_back(makeVertex(wx, waterLevel, wz, wl.nx, 0.0f, wl.nz));
                vertices.push_back(makeVertex(wx, botY,       wz, wl.nx, 0.0f, wl.nz));
            }
            // 三角形リスト
            for (int i = 0; i < wallRes; ++i)
            {
                UINT tl = base + static_cast<UINT>(i) * 2u;
                UINT tr = tl + 2u;
                UINT bl = tl + 1u;
                UINT br = tr + 1u;
                indices.push_back(tl); indices.push_back(br); indices.push_back(tr);
                indices.push_back(tl); indices.push_back(bl); indices.push_back(br);
            }
        }
    }

    if (surfaceIndexCount > 0 && indices.size() > surfaceIndexCount)
    {
        std::rotate(indices.begin(), indices.begin() + static_cast<std::ptrdiff_t>(surfaceIndexCount), indices.end());
    }

    if (indices.empty())
    {
        return;
    }

    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const UINT64 vbSize = vertices.size() * sizeof(rock::MeshVertex);
    const D3D12_RESOURCE_DESC vbDesc = BufferResourceDesc(vbSize);
    ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_gpuMeshPreview.waterVertexBuffer)), "Create water VB failed");
    D3D12_RANGE readRange{};
    void* mapped = nullptr;
    g_gpuMeshPreview.waterVertexBuffer->Map(0, &readRange, &mapped);
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(vbSize));
    g_gpuMeshPreview.waterVertexBuffer->Unmap(0, nullptr);

    const UINT64 ibSize = indices.size() * sizeof(UINT);
    const D3D12_RESOURCE_DESC ibDesc = BufferResourceDesc(ibSize);
    ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &ibDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_gpuMeshPreview.waterIndexBuffer)), "Create water IB failed");
    g_gpuMeshPreview.waterIndexBuffer->Map(0, &readRange, &mapped);
    std::memcpy(mapped, indices.data(), static_cast<size_t>(ibSize));
    g_gpuMeshPreview.waterIndexBuffer->Unmap(0, nullptr);
    g_gpuMeshPreview.waterIndexCount  = static_cast<UINT>(indices.size());
    g_gpuMeshPreview.waterVertexCount = static_cast<UINT>(vertices.size());
}

void EnsureTerrainBoundaryLineBuffer(const rock::HeightfieldGrid& grid, uint64_t uploadKey)
{
    if (g_gpuMeshPreview.terrainBoundaryLineVertexBuffer &&
        g_gpuMeshPreview.terrainBoundaryLineVertexCount > 0 &&
        g_gpuMeshPreview.terrainBoundaryLineUploadKey == uploadKey)
    {
        return;
    }

    g_gpuMeshPreview.terrainBoundaryLineVertexBuffer.Reset();
    g_gpuMeshPreview.terrainBoundaryLineVertexCount = 0;
    g_gpuMeshPreview.terrainBoundaryLineUploadKey = UINT64_MAX;
    const int n = grid.resolution;
    if (n < 2 || grid.heights.size() < static_cast<size_t>(n) * static_cast<size_t>(n))
    {
        return;
    }

    const float halfSize = std::max(grid.terrainSizeMeters, 1.0f) * 0.5f;
    const auto makeVertex = [](float x, float y, float z) {
        return rock::MeshVertex{x, y, z, 0.0f, 1.0f, 0.0f, 0.0f};
    };
    const auto heightAt = [&](int x, int y) {
        return grid.heights[static_cast<size_t>(y) * static_cast<size_t>(n) + static_cast<size_t>(x)];
    };
    const std::array<rock::MeshVertex, 4> topCorners = {{
        makeVertex(-halfSize, heightAt(0, 0), halfSize),
        makeVertex(halfSize, heightAt(n - 1, 0), halfSize),
        makeVertex(halfSize, heightAt(n - 1, n - 1), -halfSize),
        makeVertex(-halfSize, heightAt(0, n - 1), -halfSize),
    }};
    const std::array<rock::MeshVertex, 4> bottomCorners = {{
        makeVertex(-halfSize, 0.0f, halfSize),
        makeVertex(halfSize, 0.0f, halfSize),
        makeVertex(halfSize, 0.0f, -halfSize),
        makeVertex(-halfSize, 0.0f, -halfSize),
    }};

    std::vector<rock::MeshVertex> vertices;
    vertices.reserve(16);
    const auto addLine = [&](const rock::MeshVertex& a, const rock::MeshVertex& b) {
        vertices.push_back(a);
        vertices.push_back(b);
    };
    for (int i = 0; i < 4; ++i)
    {
        addLine(topCorners[static_cast<size_t>(i)], bottomCorners[static_cast<size_t>(i)]);
        addLine(bottomCorners[static_cast<size_t>(i)], bottomCorners[static_cast<size_t>((i + 1) % 4)]);
    }

    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const UINT64 vbSize = vertices.size() * sizeof(rock::MeshVertex);
    const D3D12_RESOURCE_DESC vbDesc = BufferResourceDesc(vbSize);
    ThrowIfFailed(g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
        &vbDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&g_gpuMeshPreview.terrainBoundaryLineVertexBuffer)), "Create terrain boundary line VB failed");
    D3D12_RANGE readRange{};
    void* mapped = nullptr;
    g_gpuMeshPreview.terrainBoundaryLineVertexBuffer->Map(0, &readRange, &mapped);
    std::memcpy(mapped, vertices.data(), static_cast<size_t>(vbSize));
    g_gpuMeshPreview.terrainBoundaryLineVertexBuffer->Unmap(0, nullptr);
    g_gpuMeshPreview.terrainBoundaryLineVertexCount = static_cast<UINT>(vertices.size());
    g_gpuMeshPreview.terrainBoundaryLineUploadKey = uploadKey;
}

ImVec2 ProjectPreviewPoint(float x, float y, float z, const ImVec2& center, float scale)
{
    ImVec2 p = RotatePoint(x, y, z, g_viewport.yaw, g_viewport.pitch);
    return ImVec2(center.x + p.x * scale, center.y + p.y * scale);
}

void DrawViewportAxisGizmo(ImDrawList* drawList, const ImVec2& min, const ImVec2& max)
{
    const ImVec2 center(min.x + 58.0f, max.y - 58.0f);
    constexpr float axisLength = 28.0f;

    struct AxisLine
    {
        const char* label;
        ImU32 color;
        ImVec2 dir;
        float depth;
    };

    const CameraBasis basis = BuildCameraBasis();
    auto projectDirection = [&basis](float x, float y, float z) {
        const Vec3 axis(x, y, z);
        const ImVec2 dir(Dot(axis, basis.right), -Dot(axis, basis.up));
        return std::pair<ImVec2, float>(dir, Dot(axis, basis.forward));
    };

    const auto [xDir, xDepth] = projectDirection(1.0f, 0.0f, 0.0f);
    const auto [yDir, yDepth] = projectDirection(0.0f, 1.0f, 0.0f);
    const auto [zDir, zDepth] = projectDirection(0.0f, 0.0f, 1.0f);
    std::array<AxisLine, 3> axes{{
        {"X", IM_COL32(255, 90, 90, 255), xDir, xDepth},
        {"Y", IM_COL32(90, 255, 120, 255), yDir, yDepth},
        {"Z", IM_COL32(90, 160, 255, 255), zDir, zDepth},
    }};

    std::ranges::sort(axes, [](const AxisLine& a, const AxisLine& b) {
        return a.depth > b.depth;
    });

    drawList->PushClipRect(min, max, true);
    for (const AxisLine& axis : axes)
    {
        const ImVec2 end(center.x + axis.dir.x * axisLength, center.y + axis.dir.y * axisLength);
        const float thickness = axis.depth < 0.0f ? 2.6f : 1.8f;
        drawList->AddLine(center, end, axis.color, thickness);
        drawList->AddText(ImVec2(end.x + 8.0f, end.y - 8.0f), axis.color, axis.label);
    }
    drawList->AddCircleFilled(center, 4.0f, IM_COL32(235, 235, 235, 220), 16);
    drawList->PopClipRect();
}

void DrawMeshPreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::MeshData& mesh, bool showSurface, bool showWireframe)
{
    if (mesh.vertices.empty() || mesh.triangles.empty() || (!showSurface && !showWireframe))
    {
        return;
    }

    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;

    for (const rock::MeshTriangle& triangle : mesh.triangles)
    {
        if (triangle.a >= mesh.vertices.size() || triangle.b >= mesh.vertices.size() || triangle.c >= mesh.vertices.size())
        {
            continue;
        }

        const rock::MeshVertex& va = mesh.vertices[triangle.a];
        const rock::MeshVertex& vb = mesh.vertices[triangle.b];
        const rock::MeshVertex& vc = mesh.vertices[triangle.c];
        const ImVec2 a = ProjectPreviewPoint(va.x, va.y, va.z, center, scale);
        const ImVec2 b = ProjectPreviewPoint(vb.x, vb.y, vb.z, center, scale);
        const ImVec2 c = ProjectPreviewPoint(vc.x, vc.y, vc.z, center, scale);

        if ((a.x < min.x && b.x < min.x && c.x < min.x) || (a.x > max.x && b.x > max.x && c.x > max.x) ||
            (a.y < min.y && b.y < min.y && c.y < min.y) || (a.y > max.y && b.y > max.y && c.y > max.y))
        {
            continue;
        }

        if (showSurface)
        {
            drawList->AddTriangleFilled(a, b, c, ThemeColor("surfaceFill", ImVec4(0.42f, 0.42f, 0.42f, 1.0f)));
        }
        if (showWireframe)
        {
            drawList->AddTriangle(a, b, c, ThemeColor("surfaceWire", ImVec4(0.34f, 0.34f, 0.34f, 0.70f)), 0.8f);
        }
    }
}

void DrawMeshEdgePreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::MeshData& mesh)
{
    if (mesh.vertices.empty() || mesh.edges.empty())
    {
        return;
    }

    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f;

    for (const rock::MeshEdge& edge : mesh.edges)
    {
        if (edge.a >= mesh.vertices.size() || edge.b >= mesh.vertices.size())
        {
            continue;
        }

        const rock::MeshVertex& va = mesh.vertices[edge.a];
        const rock::MeshVertex& vb = mesh.vertices[edge.b];
        ImVec2 a = ProjectPreviewPoint(va.x, va.y, va.z, center, scale);
        ImVec2 b = ProjectPreviewPoint(vb.x, vb.y, vb.z, center, scale);
        if ((a.x < min.x && b.x < min.x) || (a.x > max.x && b.x > max.x) || (a.y < min.y && b.y < min.y) || (a.y > max.y && b.y > max.y))
        {
            continue;
        }
        drawList->AddLine(a, b, ThemeColor("surfaceWire", ImVec4(0.34f, 0.34f, 0.34f, 0.70f)), 0.9f);
    }
}

struct CloudLoopVector
{
    float xMeters = 0.0f;
    float zMeters = 0.0f;
    float distanceMeters = 1.0f;
};

CloudLoopVector ComputeCloudLoopVector(const rock::CloudSettings& clouds)
{
    constexpr int kMaxTileStep = 4;
    const float windRad = clouds.windDirectionDegrees * 3.14159265358979323846f / 180.0f;
    const float windX = std::cos(windRad);
    const float windZ = std::sin(windRad);

    int bestX = 1;
    int bestZ = 0;
    float bestScore = -2.0f;
    float bestLengthTiles = 1.0f;
    for (int z = -kMaxTileStep; z <= kMaxTileStep; ++z)
    {
        for (int x = -kMaxTileStep; x <= kMaxTileStep; ++x)
        {
            if (x == 0 && z == 0)
            {
                continue;
            }
            const float lengthTiles = std::sqrt(static_cast<float>(x * x + z * z));
            const float score = (static_cast<float>(x) * windX + static_cast<float>(z) * windZ) / lengthTiles;
            if (score > bestScore + 1.0e-4f ||
                (std::abs(score - bestScore) <= 1.0e-4f && lengthTiles < bestLengthTiles))
            {
                bestScore = score;
                bestLengthTiles = lengthTiles;
                bestX = x;
                bestZ = z;
            }
        }
    }

    const float scale = std::max(clouds.horizontalScale, 1.0f);
    return {
        static_cast<float>(bestX) * scale,
        static_cast<float>(bestZ) * scale,
        std::max(bestLengthTiles * scale, 1.0f),
    };
}

bool RenderGpuMeshPreview(const ImVec2& min, const ImVec2& max, bool showSurface, bool showWireframe, std::string* error)
{
    static const auto s_waterStartTime = std::chrono::steady_clock::now();
    static auto s_lastExposureUpdateTime = std::chrono::steady_clock::now();
    const auto renderTimeNow = std::chrono::steady_clock::now();
    const float s_waterTimeSeconds = std::chrono::duration<float>(renderTimeNow - s_waterStartTime).count();
    const float exposureDeltaSeconds = std::clamp(std::chrono::duration<float>(renderTimeNow - s_lastExposureUpdateTime).count(), 1.0f / 240.0f, 0.25f);
    s_lastExposureUpdateTime = renderTimeNow;
    const rock::PreviewSettings& previewSettings = g_graph.Settings().preview;
    const bool showGrid = previewSettings.showGrid;
    const bool showWater = previewSettings.waterEnabled && showSurface;
    if (!showSurface && !showWireframe && !showGrid && !showWater)
    {
        g_gpuMeshPreview.renderStats = {};
        return true;
    }
    const MeshPreviewPipelineContext pipelineContext{
        g_device.Get(),
        MeshPreviewShaderPath(),
        MeshPreviewColorFormat(),
        DXGI_FORMAT_D32_FLOAT,
        sizeof(MeshPreviewConstants) / sizeof(UINT),
        sizeof(DisplacementShaderConstants) / sizeof(UINT),
        (sizeof(MeshPreviewConstants) + 255u) & ~255u,
    };
    if (!EnsureMeshPreviewPipeline(g_meshPreviewPipelines, pipelineContext, error)) return false;

    const float viewportWidth = std::max(1.0f, max.x - min.x);
    const float viewportHeight = std::max(1.0f, max.y - min.y);
    // Clamp the offscreen RT size up to 4K so it matches the on-screen
    // viewport 1:1 in most setups. With a smaller cap (the previous
    // 960×720) the offscreen RT got bilinearly upscaled by ImGui's
    // sampler, smearing fine 1-px patterns (e.g. mask shading hatching)
    // into wide horizontal bands.
    const int targetWidth = std::clamp(static_cast<int>(viewportWidth), 160, 3840);
    const int targetHeight = std::clamp(static_cast<int>(viewportHeight), 120, 2160);
    if (!EnsureMeshPreviewRenderTarget(targetWidth, targetHeight, error)) return false;

    const rock::MeshData& mesh = g_graph.Evaluation().previewMesh;
    const uint64_t currentVersion = g_graph.Evaluation().version;
    const bool useDisplacement = g_graph.Settings().preview.meshBackend == rock::MeshPreviewBackend::GpuDisplacement;
    const bool useTessellation = useDisplacement && g_graph.Settings().preview.viewportTessellation;
    const bool useDepthOfField = g_graph.Settings().preview.depthOfFieldEnabled && showSurface;
    const rock::TerrainBoundaryMode terrainBoundaryMode = g_graph.Settings().preview.terrainBoundaryMode;
    const SunPositionDegrees sunPosition = EffectiveSunPosition(g_graph.Settings().preview);
    const bool showSectionPolygons = showSurface && terrainBoundaryMode == rock::TerrainBoundaryMode::SectionPolygon;
    const bool showTerrainBoundaryLines = showSurface && terrainBoundaryMode == rock::TerrainBoundaryMode::Lines;
    const bool meshHasVertices = !mesh.vertices.empty();
    const bool meshDirty = (g_gpuMeshPreview.graphVersion != currentVersion || (meshHasVertices && !g_gpuMeshPreview.vertexBuffer));
    const bool viewportDirty =
        g_gpuMeshPreview.yaw != g_viewport.yaw ||
        g_gpuMeshPreview.pitch != g_viewport.pitch ||
        g_gpuMeshPreview.fovDegrees != g_viewport.fovDegrees ||
        g_gpuMeshPreview.orbitDistance != g_viewport.orbitDistance ||
        g_gpuMeshPreview.pan.x != g_viewport.pan.x ||
        g_gpuMeshPreview.pan.y != g_viewport.pan.y ||
        g_gpuMeshPreview.showSurface != showSurface ||
        g_gpuMeshPreview.showWireframe != showWireframe ||
        g_gpuMeshPreview.showGrid != showGrid ||
        g_gpuMeshPreview.hdrViewportEnabled != g_graph.Settings().preview.hdrViewportEnabled ||
        g_gpuMeshPreview.colorFormat != MeshPreviewColorFormat() ||
        g_gpuMeshPreview.maskPreview != g_graph.Evaluation().previewShowsMask ||
        g_gpuMeshPreview.maskShading != static_cast<int>(g_graph.Settings().preview.maskShading) ||
        g_gpuMeshPreview.terrainBoundaryMode != static_cast<int>(terrainBoundaryMode) ||
        g_gpuMeshPreview.lightingMode != g_graph.Settings().preview.lightingMode ||
        g_gpuMeshPreview.exposureMode != static_cast<int>(g_graph.Settings().preview.exposureMode) ||
        g_gpuMeshPreview.exposureEv != g_graph.Settings().preview.exposureEv ||
        g_gpuMeshPreview.autoExposureBiasEv != g_graph.Settings().preview.autoExposureBiasEv ||
        g_gpuMeshPreview.autoExposureMinEv != g_graph.Settings().preview.autoExposureMinEv ||
        g_gpuMeshPreview.autoExposureMaxEv != g_graph.Settings().preview.autoExposureMaxEv ||
        g_gpuMeshPreview.autoExposureSpeed != g_graph.Settings().preview.autoExposureSpeed ||
        g_gpuMeshPreview.colorTemperatureKelvin != g_graph.Settings().preview.colorTemperatureKelvin ||
        g_gpuMeshPreview.sunAzimuthDegrees != sunPosition.azimuth ||
        g_gpuMeshPreview.sunElevationDegrees != sunPosition.elevation ||
        g_gpuMeshPreview.sunIntensity != g_graph.Settings().preview.sunIntensity ||
        g_gpuMeshPreview.ambientStrength != g_graph.Settings().preview.ambientStrength ||
        g_gpuMeshPreview.shadowStrength != g_graph.Settings().preview.shadowStrength ||
        g_gpuMeshPreview.shadowBias != g_graph.Settings().preview.shadowBias ||
        g_gpuMeshPreview.pbrAlbedo != g_graph.Settings().preview.pbrAlbedo ||
        g_gpuMeshPreview.gridColor != g_graph.Settings().preview.gridColor ||
        g_gpuMeshPreview.gridCellCount != std::clamp(g_graph.Settings().preview.gridCellCount, 1, 200) ||
        g_gpuMeshPreview.gridCellSizeMeters != std::clamp(g_graph.Settings().preview.gridCellSizeMeters, 1.0f, 10000.0f) ||
        g_gpuMeshPreview.skyMode != static_cast<int>(g_graph.Settings().sky.mode) ||
        g_gpuMeshPreview.skyAtmosphereDensity != g_graph.Settings().sky.atmosphereDensity ||
        g_gpuMeshPreview.skyMieStrength != g_graph.Settings().sky.mieStrength ||
        g_gpuMeshPreview.skyMieEccentricity != g_graph.Settings().sky.mieEccentricity ||
        g_gpuMeshPreview.skyGroundAlbedo != g_graph.Settings().sky.groundAlbedo ||
        g_gpuMeshPreview.skySunSizeDegrees != g_graph.Settings().sky.sunSizeDegrees ||
        g_gpuMeshPreview.skySunGlowStrength != g_graph.Settings().sky.sunGlowStrength ||
        g_gpuMeshPreview.cloudsEnabled != ((g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric && g_graph.Settings().clouds.enabled) ? 1 : 0) ||
        g_gpuMeshPreview.cloudSeed != g_graph.Settings().clouds.seed ||
        g_gpuMeshPreview.cloudCoverage != g_graph.Settings().clouds.coverage ||
        g_gpuMeshPreview.cloudDensityMultiplier != g_graph.Settings().clouds.densityMultiplier ||
        g_gpuMeshPreview.cloudAltitudeMin != g_graph.Settings().clouds.altitudeMin ||
        g_gpuMeshPreview.cloudAltitudeMax != g_graph.Settings().clouds.altitudeMax ||
        g_gpuMeshPreview.cloudHorizontalScale != g_graph.Settings().clouds.horizontalScale ||
        g_gpuMeshPreview.cloudAbsorption != g_graph.Settings().clouds.absorption ||
        g_gpuMeshPreview.cloudColor != g_graph.Settings().clouds.color ||
        g_gpuMeshPreview.cloudAnimate != (g_graph.Settings().clouds.animate ? 1 : 0) ||
        g_gpuMeshPreview.cloudLoopPhase != g_graph.Settings().clouds.loopPhase ||
        g_gpuMeshPreview.cloudWindDirectionDegrees != g_graph.Settings().clouds.windDirectionDegrees ||
        g_gpuMeshPreview.cloudWindSpeed != g_graph.Settings().clouds.windSpeedMetersPerSec ||
        g_gpuMeshPreview.cloudQualitySamples != g_graph.Settings().clouds.qualitySamples ||
        g_gpuMeshPreview.cloudShadowStrength != g_graph.Settings().clouds.shadowStrength ||
        g_gpuMeshPreview.cloudShadowResolution != g_graph.Settings().clouds.shadowResolution ||
        g_gpuMeshPreview.cloudShadowSamples != g_graph.Settings().clouds.shadowSamples ||
        g_gpuMeshPreview.cloudFieldRadius != g_graph.Settings().clouds.fieldRadius ||
        g_gpuMeshPreview.cloudFieldFalloff != g_graph.Settings().clouds.fieldFalloff ||
        g_gpuMeshPreview.cloudSelfShadowEnabled != (g_graph.Settings().clouds.selfShadowEnabled ? 1 : 0) ||
        g_gpuMeshPreview.cloudLightSamples != g_graph.Settings().clouds.lightSamples ||
        g_gpuMeshPreview.cloudLightStepMeters != g_graph.Settings().clouds.lightStepMeters ||
        g_gpuMeshPreview.cloudPhaseEccentricity != g_graph.Settings().clouds.phaseEccentricity ||
        g_gpuMeshPreview.cloudShadowAmbientStrength != g_graph.Settings().clouds.shadowAmbientStrength ||
        g_gpuMeshPreview.meshBackend != static_cast<int>(g_graph.Settings().preview.meshBackend) ||
        g_gpuMeshPreview.viewportTessellation != g_graph.Settings().preview.viewportTessellation ||
        g_gpuMeshPreview.tessellationMinFactor != g_graph.Settings().preview.tessellationMinFactor ||
        g_gpuMeshPreview.tessellationMaxFactor != g_graph.Settings().preview.tessellationMaxFactor ||
        g_gpuMeshPreview.tessellationNearDistance != g_graph.Settings().preview.tessellationNearDistance ||
        g_gpuMeshPreview.tessellationFarDistance != g_graph.Settings().preview.tessellationFarDistance ||
        g_gpuMeshPreview.depthOfFieldEnabled != useDepthOfField ||
        g_gpuMeshPreview.dofFStop != g_graph.Settings().preview.dofFStop ||
        g_gpuMeshPreview.dofFocusDistanceMeters != g_graph.Settings().preview.dofFocusDistanceMeters ||
        g_gpuMeshPreview.dofSensorHeightMm != g_graph.Settings().preview.dofSensorHeightMm ||
        g_gpuMeshPreview.dofMaxBlurPixels != g_graph.Settings().preview.dofMaxBlurPixels ||
        g_gpuMeshPreview.dofApertureShape != g_graph.Settings().preview.dofApertureShape ||
        g_gpuMeshPreview.dofApertureBlades != g_graph.Settings().preview.dofApertureBlades ||
        g_gpuMeshPreview.dofApertureRotationDegrees != g_graph.Settings().preview.dofApertureRotationDegrees ||
        g_gpuMeshPreview.dofHighlightBoost != g_graph.Settings().preview.dofHighlightBoost ||
        g_gpuMeshPreview.dofMiniatureEnabled != g_graph.Settings().preview.dofMiniatureEnabled ||
        g_gpuMeshPreview.dofMiniatureScale != g_graph.Settings().preview.dofMiniatureScale ||
        g_gpuMeshPreview.waterEnabled != previewSettings.waterEnabled ||
        g_gpuMeshPreview.waterLevelMeters != previewSettings.waterLevelMeters ||
        g_gpuMeshPreview.waterOpacity != previewSettings.waterOpacity ||
        g_gpuMeshPreview.waterColor != previewSettings.waterColor ||
        g_gpuMeshPreview.waterTerrainSizeMeters != g_graph.Evaluation().previewHeightfield.terrainSizeMeters ||
        g_gpuMeshPreview.waterWavesScale != previewSettings.waterWavesScale ||
        g_gpuMeshPreview.waterRefractiveIndex != previewSettings.waterRefractiveIndex ||
        g_gpuMeshPreview.waterFresnelPower != previewSettings.waterFresnelPower ||
        g_gpuMeshPreview.waterRefractionStrength != previewSettings.waterRefractionStrength ||
        g_gpuMeshPreview.waterAnimationEnabled != previewSettings.waterAnimationEnabled ||
        g_gpuMeshPreview.waterReflectionStrength != previewSettings.waterReflectionStrength ||
        g_gpuMeshPreview.waterSsrEnabled != previewSettings.waterSsrEnabled ||
        (g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric && g_graph.Settings().clouds.enabled && g_graph.Settings().clouds.animate && g_graph.Settings().clouds.windSpeedMetersPerSec > 0.0f) ||
        (showGrid && !g_gpuMeshPreview.gridVertexBuffer) ||
        (showWater && (!g_gpuMeshPreview.waterVertexBuffer || !g_gpuMeshPreview.waterIndexBuffer)) ||
        (showTerrainBoundaryLines && !g_gpuMeshPreview.terrainBoundaryLineVertexBuffer) ||
        g_gpuMeshPreview.colorState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
        g_gpuMeshPreview.outputState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
        (g_graph.Settings().preview.hdrViewportEnabled && g_graph.Settings().preview.exposureMode == rock::ExposureMode::Auto) ||
        (useDepthOfField && g_gpuMeshPreview.postState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    auto addDirtyReason = [](std::string& reason, const char* text) {
        if (!reason.empty())
        {
            reason += ", ";
        }
        reason += text;
    };
    std::string dirtyReason;
    if (meshDirty) addDirtyReason(dirtyReason, "mesh");
    if (g_gpuMeshPreview.yaw != g_viewport.yaw || g_gpuMeshPreview.pitch != g_viewport.pitch ||
        g_gpuMeshPreview.fovDegrees != g_viewport.fovDegrees || g_gpuMeshPreview.orbitDistance != g_viewport.orbitDistance ||
        g_gpuMeshPreview.pan.x != g_viewport.pan.x || g_gpuMeshPreview.pan.y != g_viewport.pan.y)
    {
        addDirtyReason(dirtyReason, "camera");
    }
    if (g_gpuMeshPreview.showSurface != showSurface || g_gpuMeshPreview.showWireframe != showWireframe ||
        g_gpuMeshPreview.showGrid != showGrid)
    {
        addDirtyReason(dirtyReason, "visibility");
    }
    if (g_gpuMeshPreview.hdrViewportEnabled != g_graph.Settings().preview.hdrViewportEnabled ||
        g_gpuMeshPreview.colorFormat != MeshPreviewColorFormat())
    {
        addDirtyReason(dirtyReason, "hdr");
    }
    if (g_gpuMeshPreview.exposureMode != static_cast<int>(g_graph.Settings().preview.exposureMode) ||
        g_gpuMeshPreview.exposureEv != g_graph.Settings().preview.exposureEv ||
        g_gpuMeshPreview.autoExposureBiasEv != g_graph.Settings().preview.autoExposureBiasEv ||
        g_gpuMeshPreview.autoExposureMinEv != g_graph.Settings().preview.autoExposureMinEv ||
        g_gpuMeshPreview.autoExposureMaxEv != g_graph.Settings().preview.autoExposureMaxEv ||
        g_gpuMeshPreview.autoExposureSpeed != g_graph.Settings().preview.autoExposureSpeed ||
        g_gpuMeshPreview.colorTemperatureKelvin != g_graph.Settings().preview.colorTemperatureKelvin)
    {
        addDirtyReason(dirtyReason, "exposure");
    }
    else if (g_graph.Settings().preview.hdrViewportEnabled && g_graph.Settings().preview.exposureMode == rock::ExposureMode::Auto)
    {
        addDirtyReason(dirtyReason, "auto exposure");
    }
    if (g_gpuMeshPreview.skyMode != static_cast<int>(g_graph.Settings().sky.mode) ||
        g_gpuMeshPreview.skyAtmosphereDensity != g_graph.Settings().sky.atmosphereDensity ||
        g_gpuMeshPreview.skyMieStrength != g_graph.Settings().sky.mieStrength ||
        g_gpuMeshPreview.skyMieEccentricity != g_graph.Settings().sky.mieEccentricity ||
        g_gpuMeshPreview.skyGroundAlbedo != g_graph.Settings().sky.groundAlbedo ||
        g_gpuMeshPreview.skySunSizeDegrees != g_graph.Settings().sky.sunSizeDegrees ||
        g_gpuMeshPreview.skySunGlowStrength != g_graph.Settings().sky.sunGlowStrength)
    {
        addDirtyReason(dirtyReason, "sky");
    }
    const bool cloudsAnimated =
        g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric &&
        g_graph.Settings().clouds.enabled &&
        g_graph.Settings().clouds.animate &&
        g_graph.Settings().clouds.windSpeedMetersPerSec > 0.0f;
    if (cloudsAnimated)
    {
        addDirtyReason(dirtyReason, "cloud animation");
    }
    else if (g_gpuMeshPreview.cloudsEnabled != ((g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric && g_graph.Settings().clouds.enabled) ? 1 : 0) ||
        g_gpuMeshPreview.cloudSeed != g_graph.Settings().clouds.seed ||
        g_gpuMeshPreview.cloudCoverage != g_graph.Settings().clouds.coverage ||
        g_gpuMeshPreview.cloudDensityMultiplier != g_graph.Settings().clouds.densityMultiplier ||
        g_gpuMeshPreview.cloudAltitudeMin != g_graph.Settings().clouds.altitudeMin ||
        g_gpuMeshPreview.cloudAltitudeMax != g_graph.Settings().clouds.altitudeMax ||
        g_gpuMeshPreview.cloudHorizontalScale != g_graph.Settings().clouds.horizontalScale ||
        g_gpuMeshPreview.cloudAbsorption != g_graph.Settings().clouds.absorption ||
        g_gpuMeshPreview.cloudColor != g_graph.Settings().clouds.color ||
        g_gpuMeshPreview.cloudAnimate != (g_graph.Settings().clouds.animate ? 1 : 0) ||
        g_gpuMeshPreview.cloudLoopPhase != g_graph.Settings().clouds.loopPhase ||
        g_gpuMeshPreview.cloudWindDirectionDegrees != g_graph.Settings().clouds.windDirectionDegrees ||
        g_gpuMeshPreview.cloudWindSpeed != g_graph.Settings().clouds.windSpeedMetersPerSec ||
        g_gpuMeshPreview.cloudQualitySamples != g_graph.Settings().clouds.qualitySamples ||
        g_gpuMeshPreview.cloudShadowStrength != g_graph.Settings().clouds.shadowStrength ||
        g_gpuMeshPreview.cloudShadowResolution != g_graph.Settings().clouds.shadowResolution ||
        g_gpuMeshPreview.cloudShadowSamples != g_graph.Settings().clouds.shadowSamples ||
        g_gpuMeshPreview.cloudFieldRadius != g_graph.Settings().clouds.fieldRadius ||
        g_gpuMeshPreview.cloudFieldFalloff != g_graph.Settings().clouds.fieldFalloff ||
        g_gpuMeshPreview.cloudSelfShadowEnabled != (g_graph.Settings().clouds.selfShadowEnabled ? 1 : 0) ||
        g_gpuMeshPreview.cloudLightSamples != g_graph.Settings().clouds.lightSamples ||
        g_gpuMeshPreview.cloudLightStepMeters != g_graph.Settings().clouds.lightStepMeters ||
        g_gpuMeshPreview.cloudPhaseEccentricity != g_graph.Settings().clouds.phaseEccentricity ||
        g_gpuMeshPreview.cloudShadowAmbientStrength != g_graph.Settings().clouds.shadowAmbientStrength)
    {
        addDirtyReason(dirtyReason, "cloud settings");
    }
    if (g_gpuMeshPreview.meshBackend != static_cast<int>(g_graph.Settings().preview.meshBackend) ||
        g_gpuMeshPreview.viewportTessellation != g_graph.Settings().preview.viewportTessellation ||
        g_gpuMeshPreview.tessellationMinFactor != g_graph.Settings().preview.tessellationMinFactor ||
        g_gpuMeshPreview.tessellationMaxFactor != g_graph.Settings().preview.tessellationMaxFactor ||
        g_gpuMeshPreview.tessellationNearDistance != g_graph.Settings().preview.tessellationNearDistance ||
        g_gpuMeshPreview.tessellationFarDistance != g_graph.Settings().preview.tessellationFarDistance)
    {
        addDirtyReason(dirtyReason, "mesh backend");
    }
    if (g_gpuMeshPreview.colorState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
        g_gpuMeshPreview.outputState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
        (useDepthOfField && g_gpuMeshPreview.postState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
    {
        addDirtyReason(dirtyReason, "resource state");
    }
    if (g_gpuMeshPreview.waterEnabled != previewSettings.waterEnabled ||
        g_gpuMeshPreview.waterLevelMeters != previewSettings.waterLevelMeters ||
        g_gpuMeshPreview.waterOpacity != previewSettings.waterOpacity ||
        g_gpuMeshPreview.waterColor != previewSettings.waterColor ||
        g_gpuMeshPreview.waterTerrainSizeMeters != g_graph.Evaluation().previewHeightfield.terrainSizeMeters ||
        g_gpuMeshPreview.waterWavesScale != previewSettings.waterWavesScale ||
        g_gpuMeshPreview.waterRefractiveIndex != previewSettings.waterRefractiveIndex ||
        g_gpuMeshPreview.waterFresnelPower != previewSettings.waterFresnelPower ||
        g_gpuMeshPreview.waterRefractionStrength != previewSettings.waterRefractionStrength ||
        g_gpuMeshPreview.waterAnimationEnabled != previewSettings.waterAnimationEnabled ||
        g_gpuMeshPreview.waterReflectionStrength != previewSettings.waterReflectionStrength ||
        g_gpuMeshPreview.waterSsrEnabled != previewSettings.waterSsrEnabled ||
        g_gpuMeshPreview.waterHeightfieldVersion != currentVersion)
    {
        addDirtyReason(dirtyReason, "water");
    }
    if (dirtyReason.empty() && viewportDirty)
    {
        addDirtyReason(dirtyReason, "other viewport setting");
    }
    g_frameTiming.gpuPreviewReason = dirtyReason.empty() ? "cached" : dirtyReason;
    if (!meshDirty && !viewportDirty) return true;

    try
    {
        PreviewRenderStats renderStats{};
        renderStats.renderTargetWidth = targetWidth;
        renderStats.renderTargetHeight = targetHeight;
        renderStats.displayMeshResolution = CurrentPreviewMeshResolution();
        renderStats.gpuDisplacement = useDisplacement;
        renderStats.tessellation = useTessellation;
        renderStats.tessellationMaxFactor = g_graph.Settings().preview.tessellationMaxFactor;
        const auto recordDraw = [&](UINT vertexCount, bool triangles) {
            ++renderStats.drawCalls;
            renderStats.submittedVertices += vertexCount;
            if (triangles)
            {
                renderStats.submittedTriangles += vertexCount / 3u;
            }
            else
            {
                renderStats.submittedLines += vertexCount / 2u;
            }
        };
        const auto recordIndexedDraw = [&](UINT indexCount, bool triangles) {
            ++renderStats.drawCalls;
            ++renderStats.indexedDrawCalls;
            renderStats.submittedIndices += indexCount;
            if (triangles)
            {
                renderStats.submittedTriangles += indexCount / 3u;
            }
            else
            {
                renderStats.submittedLines += indexCount / 2u;
            }
        };
        const auto recordPatchDraw = [&](UINT indexCount, float maxTessFactor) {
            ++renderStats.drawCalls;
            ++renderStats.indexedDrawCalls;
            renderStats.submittedIndices += indexCount;
            renderStats.submittedPatches += indexCount / 4u;
            const uint64_t patches = static_cast<uint64_t>(indexCount / 4u);
            const uint64_t tess = static_cast<uint64_t>(std::ceil(std::max(maxTessFactor, 1.0f)));
            renderStats.submittedTriangles += patches * tess * tess * 2u;
        };
        const int previewMeshResolution = CurrentPreviewMeshResolution();
        const int previewMeshResolutionM1 = std::max(0, previewMeshResolution - 1);
        const UINT topSurfaceTriIndexCount = static_cast<UINT>(previewMeshResolutionM1 * previewMeshResolutionM1 * 6);
        const UINT topSurfaceEdgeIndexCount = static_cast<UINT>(
            (previewMeshResolution * previewMeshResolutionM1 * 2 + previewMeshResolutionM1 * previewMeshResolutionM1) * 2);
        const auto cpuSurfaceIndexCount = [&]() {
            return terrainBoundaryMode == rock::TerrainBoundaryMode::SectionPolygon
                ? g_gpuMeshPreview.triIndexCount
                : std::min(g_gpuMeshPreview.triIndexCount, topSurfaceTriIndexCount);
        };
        const auto cpuEdgeIndexCount = [&]() {
            return terrainBoundaryMode == rock::TerrainBoundaryMode::SectionPolygon
                ? g_gpuMeshPreview.edgeIndexCount
                : std::min(g_gpuMeshPreview.edgeIndexCount, topSurfaceEdgeIndexCount);
        };

        // Phase 2c-1: skip the CPU mesh upload (vb / ib / edge ib) when
        // the GPU displacement backend is on. The CPU mesh struct is still
        // built (Evaluate needs it for the 2D edge preview / OBJ export),
        // it just doesn't get pushed to GPU. Shadow and wireframe in this
        // mode are silently disabled — Phase 2c-2 will re-add a
        // displacement shadow path once we're sure the CPU-skip itself is
        // stable.
        if (meshDirty)
        {
            if (!useDisplacement)
            {
                UpdateMeshPreviewBuffers(mesh);
            }
            else
            {
                // Release any CPU buffers carried over from CpuMesh mode
                // so the dirty check stays accurate when we toggle back.
                g_gpuMeshPreview.vertexBuffer.Reset();
                g_gpuMeshPreview.indexBuffer.Reset();
                g_gpuMeshPreview.edgeIndexBuffer.Reset();
                g_gpuMeshPreview.vertexCount = 0;
                g_gpuMeshPreview.triIndexCount = 0;
                g_gpuMeshPreview.edgeIndexCount = 0;
            }
            g_gpuMeshPreview.graphVersion = currentVersion;
        }
        if (showGrid)
        {
            EnsureGridPreviewBuffer();
        }

        const rock::HeightfieldGrid& previewGrid = g_graph.Evaluation().previewHeightfield;
        if (showWater)
        {
            EnsureWaterPreviewBuffer(previewGrid.terrainSizeMeters, previewGrid, currentVersion);
        }
        if (showTerrainBoundaryLines)
        {
            EnsureTerrainBoundaryLineBuffer(previewGrid, currentVersion);
        }
        bool previewGridTexturesReady = false;
        if (previewGrid.resolution >= 2)
        {
            std::string ignoredErr;
            previewGridTexturesReady =
                EnsureDisplacementHeightTextures(previewGrid.resolution, &ignoredErr) &&
                EnsureDummyCloudShadowTexture(&ignoredErr);
        }

        bool displacementReady = false;
        if (useDisplacement && previewGrid.resolution >= 2)
        {
            std::string ignoredErr;
            // Cloud-shadow CBV + dummy SRV are normally created lazily on
            // the first cloudy frame. The displacement root signature
            // requires them bound, so force initialisation here regardless
            // of whether clouds are enabled this frame.
            if (previewGridTexturesReady &&
                EnsureMeshPreviewDisplacementPipeline(g_meshPreviewPipelines, pipelineContext, &ignoredErr) &&
                EnsureDisplacementGridIndexBuffers(previewMeshResolution, &ignoredErr) &&
                EnsureCloudShadowMeshCb(&ignoredErr) &&
                EnsureDummyCloudShadowTexture(&ignoredErr))
            {
                displacementReady = true;
            }
        }

        const bool hasMeshVertices = g_gpuMeshPreview.vertexCount > 0 && g_gpuMeshPreview.vertexBuffer;
        if (!hasMeshVertices && !displacementReady && (!showGrid || g_gpuMeshPreview.gridVertexCount == 0) && (!showWater || g_gpuMeshPreview.waterIndexCount == 0))
        {
            return false;
        }

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Mesh preview allocator failed");
        ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Mesh preview CL failed");

        bool previewGridTextureUploaded = false;
        if (previewGridTexturesReady)
        {
            std::string ignoredErr;
            // Upload only when the underlying graph has changed. CPU mesh
            // previews also sample this mask texture in the pixel shader,
            // while the displacement backend uses height + mask in the VS.
            previewGridTextureUploaded = UploadDisplacementHeightfield(commandList.Get(), previewGrid, currentVersion, &ignoredErr);
        }

        // AO コンピュート: ハイトフィールドが更新されたフレームで再計算する。
        // SetDescriptorHeaps を早期に呼んでコンピュートディスパッチを inline 実行。
        bool aoTextureReady = false;
        const bool wantsAO = g_graph.Settings().preview.aoEnabled
            && !g_graph.Evaluation().previewShowsMask
            && previewGridTexturesReady;
        if (wantsAO)
        {
            ID3D12DescriptorHeap* aoHeaps[] = {g_srvHeap.Get()};
            commandList->SetDescriptorHeaps(1, aoHeaps);
            std::string aoErr;
            aoTextureReady = DispatchAOCompute(
                commandList.Get(),
                previewGrid.resolution,
                previewGrid.terrainSizeMeters,
                g_graph.Settings().preview.aoRadius,
                currentVersion,
                &aoErr);
        }
        else if (g_gpuMeshPreview.aoTexture)
        {
            aoTextureReady = (g_gpuMeshPreview.aoUploadKey == currentVersion);
        }

        bool colorTextureReady = false;
        if (g_graph.Evaluation().previewIsColor)
        {
            const rock::ColorGrid& colorGrid = g_graph.Evaluation().previewColorGrid;
            colorTextureReady =
                colorGrid.resolution >= 2 &&
                colorGrid.pixels.size() >= static_cast<size_t>(colorGrid.resolution) * static_cast<size_t>(colorGrid.resolution) * 4u;
            if (colorTextureReady)
            {
                std::string ignoredErr;
                colorTextureReady = UploadColorizeTexture(commandList.Get(), colorGrid, currentVersion, &ignoredErr);
            }
        }

        if (g_gpuMeshPreview.colorState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_gpuMeshPreview.colorTarget.Get();
            b.Transition.StateBefore = g_gpuMeshPreview.colorState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &b);
            g_gpuMeshPreview.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        if (g_gpuMeshPreview.depthState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
        {
            // Cloud pass at the end of last frame left depth as SRV; flip it
            // back to DEPTH_WRITE before clearing.
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_gpuMeshPreview.depthTarget.Get();
            b.Transition.StateBefore = g_gpuMeshPreview.depthState;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &b);
            g_gpuMeshPreview.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        }

        const std::array<float, 3>& viewportBackground = g_graph.Settings().preview.viewportBackground;
        const bool atmosphericSky = g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric;
        const float clearColor[] = {
            atmosphericSky ? 0.0f : viewportBackground[0],
            atmosphericSky ? 0.0f : viewportBackground[1],
            atmosphericSky ? 0.0f : viewportBackground[2],
            1.0f,
        };
        commandList->ClearRenderTargetView(g_gpuMeshPreview.rtvCpu, clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(g_gpuMeshPreview.dsvCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        commandList->OMSetRenderTargets(1, &g_gpuMeshPreview.rtvCpu, FALSE, &g_gpuMeshPreview.dsvCpu);

        D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0.0f, 1.0f};
        D3D12_RECT scissor{0, 0, targetWidth, targetHeight};
        commandList->RSSetViewports(1, &vp);
        commandList->RSSetScissorRects(1, &scissor);

        const CameraBasis basis = BuildCameraBasis();
        const float fovRad = std::clamp(g_viewport.fovDegrees, 15.0f, 90.0f) * 3.14159265f / 180.0f;
        const float focalLength = 1.0f / std::tan(fovRad * 0.5f);
        const float viewportSize = std::min(viewportWidth, viewportHeight);
        const float scale = viewportSize * 1.20f;

        MeshPreviewConstants constants{};
        constants.cameraPosition[0] = basis.position.x; constants.cameraPosition[1] = basis.position.y; constants.cameraPosition[2] = basis.position.z;
        constants.cameraRight[0]    = basis.right.x;    constants.cameraRight[1]    = basis.right.y;    constants.cameraRight[2]    = basis.right.z;
        constants.cameraUp[0]       = basis.up.x;       constants.cameraUp[1]       = basis.up.y;       constants.cameraUp[2]       = basis.up.z;
        constants.cameraForward[0]  = basis.forward.x;  constants.cameraForward[1]  = basis.forward.y;  constants.cameraForward[2]  = basis.forward.z;
        constants.projScaleX = focalLength * scale * 2.0f / viewportWidth;
        constants.projScaleY = focalLength * scale * 2.0f / viewportHeight;
        constants.panNdcX    = g_viewport.pan.x * 2.0f / viewportWidth;
        constants.panNdcY    = -g_viewport.pan.y * 2.0f / viewportHeight;
        constants.nearPlane  = 0.05f;
        constants.farPlane   = kViewportFarPlane;
        constants.maskPreview = g_graph.Evaluation().previewShowsMask ? 1.0f : 0.0f;
        constants.maskShadingMode = static_cast<float>(g_graph.Settings().preview.maskShading);
        constants.colorTextureMode = (colorTextureReady ? 1.0f : 0.0f) + (previewGridTextureUploaded ? 2.0f : 0.0f);
        constants.lightingMode = static_cast<float>(g_graph.Settings().preview.lightingMode);
        const float azimuth = sunPosition.azimuth * kDegreesToRadians;
        const float elevation = sunPosition.elevation * kDegreesToRadians;
        const float cosElevation = std::cos(elevation);
        constants.sunDirection[0] = std::sin(azimuth) * cosElevation;
        constants.sunDirection[1] = std::sin(elevation);
        constants.sunDirection[2] = std::cos(azimuth) * cosElevation;
        constants.sunDirection[3] = 0.0f;
        constants.albedoColor[0] = g_graph.Settings().preview.pbrAlbedo[0];
        constants.albedoColor[1] = g_graph.Settings().preview.pbrAlbedo[1];
        constants.albedoColor[2] = g_graph.Settings().preview.pbrAlbedo[2];
        constants.albedoColor[3] = std::max(previewGrid.terrainSizeMeters, 1.0f);
        constants.sunIntensity = g_graph.Settings().preview.sunIntensity;
        constants.ambientStrength = g_graph.Settings().preview.ambientStrength;
        constants.shadowStrength = g_graph.Settings().preview.shadowStrength;
        constants.shadowMapResolution = static_cast<float>(g_gpuMeshPreview.shadowMapResolution);
        constants.shadowBias = g_graph.Settings().preview.shadowBias;
        constants.shadowEnabled = (g_graph.Settings().preview.lightingMode >= 1 && !g_graph.Evaluation().previewShowsMask) ? 1.0f : 0.0f;

        Vec3 sunDirection(constants.sunDirection[0], constants.sunDirection[1], constants.sunDirection[2]);
        sunDirection = Normalize(sunDirection, Vec3(0.35f, 0.65f, 0.68f));
        const Vec3 lightForward = Scale(sunDirection, -1.0f);
        const Vec3 guideUp = (std::abs(Dot(lightForward, Vec3(0.0f, 1.0f, 0.0f))) > 0.92f) ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
        const Vec3 lightRight = Normalize(Cross(guideUp, lightForward), Vec3(1.0f, 0.0f, 0.0f));
        const Vec3 lightUp = Normalize(Cross(lightForward, lightRight), Vec3(0.0f, 1.0f, 0.0f));

        Vec3 boundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
        Vec3 boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        if (hasMeshVertices)
        {
            for (const rock::MeshVertex& vertex : mesh.vertices)
            {
                boundsMin.x = std::min(boundsMin.x, vertex.x);
                boundsMin.y = std::min(boundsMin.y, vertex.y);
                boundsMin.z = std::min(boundsMin.z, vertex.z);
                boundsMax.x = std::max(boundsMax.x, vertex.x);
                boundsMax.y = std::max(boundsMax.y, vertex.y);
                boundsMax.z = std::max(boundsMax.z, vertex.z);
            }
        }
        else
        {
            boundsMin = Vec3(-2000.0f, 0.0f, -2000.0f);
            boundsMax = Vec3(2000.0f, 0.0f, 2000.0f);
        }
        const Vec3 boundsCenter = Scale(Add(boundsMin, boundsMax), 0.5f);
        const float boundsDiagonal = Length(Subtract(boundsMax, boundsMin));
        const float lightHalfXY = std::max(512.0f, boundsDiagonal * 1.25f);
        const Vec3 corners[] =
        {
            Vec3(boundsMin.x, boundsMin.y, boundsMin.z),
            Vec3(boundsMax.x, boundsMin.y, boundsMin.z),
            Vec3(boundsMin.x, boundsMax.y, boundsMin.z),
            Vec3(boundsMax.x, boundsMax.y, boundsMin.z),
            Vec3(boundsMin.x, boundsMin.y, boundsMax.z),
            Vec3(boundsMax.x, boundsMin.y, boundsMax.z),
            Vec3(boundsMin.x, boundsMax.y, boundsMax.z),
            Vec3(boundsMax.x, boundsMax.y, boundsMax.z),
        };
        float lightMinZ = FLT_MAX;
        float lightMaxZ = -FLT_MAX;
        for (const Vec3& corner : corners)
        {
            const float z = Dot(corner, lightForward);
            lightMinZ = std::min(lightMinZ, z);
            lightMaxZ = std::max(lightMaxZ, z);
        }
        const float lightDepthPadding = std::max(64.0f, boundsDiagonal * 0.08f);
        const float lightDepthMin = lightMinZ - lightDepthPadding;
        const float lightDepthRange = std::max(1.0f, (lightMaxZ - lightMinZ) + lightDepthPadding * 2.0f);
        constants.lightRight[0] = lightRight.x; constants.lightRight[1] = lightRight.y; constants.lightRight[2] = lightRight.z;
        constants.lightUp[0] = lightUp.x; constants.lightUp[1] = lightUp.y; constants.lightUp[2] = lightUp.z;
        constants.lightForward[0] = lightForward.x; constants.lightForward[1] = lightForward.y; constants.lightForward[2] = lightForward.z;
        constants.lightCenter[0] = boundsCenter.x; constants.lightCenter[1] = boundsCenter.y; constants.lightCenter[2] = boundsCenter.z;
        constants.lightWorldRadius = lightHalfXY;
        constants.lightNearPlane = lightHalfXY;
        constants.lightFarPlane = lightDepthRange;
        constants.lightDepthMin = lightDepthMin;

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        if (hasMeshVertices)
        {
            vbv.BufferLocation = g_gpuMeshPreview.vertexBuffer->GetGPUVirtualAddress();
            vbv.SizeInBytes    = g_gpuMeshPreview.vertexCount * static_cast<UINT>(sizeof(rock::MeshVertex));
            vbv.StrideInBytes  = static_cast<UINT>(sizeof(rock::MeshVertex));
            commandList->IASetVertexBuffers(0, 1, &vbv);
        }
        commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.rootSignature.Get());
        commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);

        const bool wantsShadow = constants.shadowEnabled > 0.5f && showSurface;
        const bool canCpuShadow = hasMeshVertices && g_gpuMeshPreview.triIndexCount > 0;
        const bool canDisplacementShadow = useDisplacement && displacementReady &&
            (useTessellation ? g_gpuMeshPreview.displacementPatchIndexCount > 0 : g_gpuMeshPreview.displacementTriIndexCount > 0);
        if (wantsShadow && (canCpuShadow || canDisplacementShadow))
        {
            if (g_gpuMeshPreview.shadowState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
            {
                D3D12_RESOURCE_BARRIER shadowToDepth{};
                shadowToDepth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                shadowToDepth.Transition.pResource = g_gpuMeshPreview.shadowTarget.Get();
                shadowToDepth.Transition.StateBefore = g_gpuMeshPreview.shadowState;
                shadowToDepth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                shadowToDepth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &shadowToDepth);
                g_gpuMeshPreview.shadowState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            }

            const int shadowResolution = std::max(1, g_gpuMeshPreview.shadowMapResolution);
            D3D12_VIEWPORT shadowVp{0.0f, 0.0f, static_cast<float>(shadowResolution), static_cast<float>(shadowResolution), 0.0f, 1.0f};
            D3D12_RECT shadowScissor{0, 0, shadowResolution, shadowResolution};
            commandList->RSSetViewports(1, &shadowVp);
            commandList->RSSetScissorRects(1, &shadowScissor);
            commandList->ClearDepthStencilView(g_gpuMeshPreview.shadowDsvCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            commandList->OMSetRenderTargets(0, nullptr, FALSE, &g_gpuMeshPreview.shadowDsvCpu);

            if (canDisplacementShadow)
            {
                // Phase 2c-2: displacement shadow path. The shadow VS only
                // reads cbuffer Constants (b0) for the light* fields and
                // samples the height texture (t2) — it doesn't touch the
                // shadow map / cloud shadow / mask. We bind dummy SRVs at
                // the slots the shader doesn't access (especially t0 — the
                // shadow target's own SRV, which would otherwise alias the
                // currently-bound DSV and trip up D3D12 validation).
                // Descriptor heap MUST be set before binding any descriptor
                // table (CBV_SRV_UAV). The CPU shadow PSO only used 32-bit
                // constants so it never needed this — the displacement
                // shadow PSO does, hence the explicit set here.
                ID3D12DescriptorHeap* shadowHeaps[] = {g_srvHeap.Get()};
                commandList->SetDescriptorHeaps(1, shadowHeaps);

                void* mappedCbv = nullptr;
                D3D12_RANGE readRange{0, 0};
                g_meshPreviewPipelines.displacementCbv->Map(0, &readRange, &mappedCbv);
                std::memcpy(mappedCbv, &constants, sizeof(constants));
                g_meshPreviewPipelines.displacementCbv->Unmap(0, nullptr);

                DisplacementShaderConstants dispConsts{};
                const int M = g_gpuMeshPreview.displacementMeshResolution;
                dispConsts.gridResolution = static_cast<float>(M);
                dispConsts.terrainSize = previewGrid.terrainSizeMeters;
                dispConsts.halfSize = previewGrid.terrainSizeMeters * 0.5f;
                dispConsts.worldDX = (M > 1) ? previewGrid.terrainSizeMeters / static_cast<float>(M - 1) : 1.0f;
                dispConsts.tessellationMinFactor = std::clamp(g_graph.Settings().preview.tessellationMinFactor, 1.0f, 64.0f);
                dispConsts.tessellationMaxFactor = std::clamp(g_graph.Settings().preview.tessellationMaxFactor, dispConsts.tessellationMinFactor, 64.0f);
                dispConsts.tessellationNearDistance = std::max(1.0f, g_graph.Settings().preview.tessellationNearDistance);
                dispConsts.tessellationFarDistance = std::max(dispConsts.tessellationNearDistance + 1.0f, g_graph.Settings().preview.tessellationFarDistance);

                commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.displacementRootSignature.Get());
                commandList->SetGraphicsRootConstantBufferView(0, g_meshPreviewPipelines.displacementCbv->GetGPUVirtualAddress());
                commandList->SetGraphicsRootConstantBufferView(1, g_gpuClouds.meshCbUploadBuffer->GetGPUVirtualAddress());
                commandList->SetGraphicsRoot32BitConstants(2, sizeof(dispConsts) / 4, &dispConsts, 0);
                // Slot 3 (shadow SRV) — we are CURRENTLY writing the shadow
                // target as DSV. Binding its own SRV here would alias and
                // is the most likely cause of the original Phase 2c crash.
                // The shadow PSO has no PS so this slot is never sampled,
                // but the root signature still requires a valid descriptor
                // — bind the cloud dummy SRV instead.
                commandList->SetGraphicsRootDescriptorTable(3, g_gpuClouds.dummyShadowSrvGpu);
                commandList->SetGraphicsRootDescriptorTable(4, g_gpuClouds.dummyShadowSrvGpu);
                commandList->SetGraphicsRootDescriptorTable(5, g_gpuMeshPreview.displacementHeightSrvGpu);
                commandList->SetGraphicsRootDescriptorTable(6, g_gpuMeshPreview.displacementMaskSrvGpu);
                commandList->SetGraphicsRootDescriptorTable(7, g_gpuClouds.dummyShadowSrvGpu);
                commandList->SetGraphicsRootDescriptorTable(8, g_gpuClouds.dummyShadowSrvGpu);

                ID3D12Resource* shadowIndexBuffer = useTessellation
                    ? g_gpuMeshPreview.displacementPatchIndexBuffer.Get()
                    : g_gpuMeshPreview.displacementTriIndexBuffer.Get();
                const UINT shadowIndexCount = useTessellation
                    ? g_gpuMeshPreview.displacementPatchIndexCount
                    : g_gpuMeshPreview.displacementTriIndexCount;
                D3D12_INDEX_BUFFER_VIEW shadowIbv{
                    shadowIndexBuffer->GetGPUVirtualAddress(),
                    shadowIndexCount * static_cast<UINT>(sizeof(UINT)),
                    DXGI_FORMAT_R32_UINT};
                commandList->IASetIndexBuffer(&shadowIbv);
                commandList->IASetVertexBuffers(0, 0, nullptr);
                commandList->SetPipelineState(useTessellation
                    ? g_meshPreviewPipelines.displacementTessShadowPso.Get()
                    : g_meshPreviewPipelines.displacementShadowPso.Get());
                commandList->IASetPrimitiveTopology(useTessellation
                    ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                    : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawIndexedInstanced(shadowIndexCount, 1, 0, 0, 0);
                if (useTessellation)
                {
                    recordPatchDraw(shadowIndexCount, dispConsts.tessellationMaxFactor);
                }
                else
                {
                    recordIndexedDraw(shadowIndexCount, true);
                }
                if (showSectionPolygons && g_gpuMeshPreview.displacementSectionIndexCount > 0)
                {
                    D3D12_INDEX_BUFFER_VIEW sectionIbv{
                        g_gpuMeshPreview.displacementSectionIndexBuffer->GetGPUVirtualAddress(),
                        g_gpuMeshPreview.displacementSectionIndexCount * static_cast<UINT>(sizeof(UINT)),
                        DXGI_FORMAT_R32_UINT};
                    commandList->IASetIndexBuffer(&sectionIbv);
                    commandList->SetPipelineState(g_meshPreviewPipelines.displacementSectionShadowPso.Get());
                    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    commandList->DrawIndexedInstanced(g_gpuMeshPreview.displacementSectionIndexCount, 1, 0, 0, 0);
                    recordIndexedDraw(g_gpuMeshPreview.displacementSectionIndexCount, true);
                }
                renderStats.shadowPass = true;

                // Restore CPU root sig + bindings for the surface / grid
                // draws below (they assume the CPU root sig is current).
                commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.rootSignature.Get());
                commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
                if (g_gpuClouds.meshCbUploadBuffer)
                {
                    commandList->SetGraphicsRootConstantBufferView(1, g_gpuClouds.meshCbUploadBuffer->GetGPUVirtualAddress());
                }
                if (hasMeshVertices)
                {
                    commandList->IASetVertexBuffers(0, 1, &vbv);
                }
            }
            else
            {
                const UINT shadowIndexCount = cpuSurfaceIndexCount();
                D3D12_INDEX_BUFFER_VIEW shadowIbv{g_gpuMeshPreview.indexBuffer->GetGPUVirtualAddress(), shadowIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
                commandList->IASetIndexBuffer(&shadowIbv);
                commandList->SetPipelineState(g_meshPreviewPipelines.shadowPso.Get());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawIndexedInstanced(shadowIndexCount, 1, 0, 0, 0);
                recordIndexedDraw(shadowIndexCount, true);
                renderStats.shadowPass = true;
            }

            D3D12_RESOURCE_BARRIER shadowToSrv{};
            shadowToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            shadowToSrv.Transition.pResource = g_gpuMeshPreview.shadowTarget.Get();
            shadowToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            shadowToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            shadowToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &shadowToSrv);
            g_gpuMeshPreview.shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        commandList->OMSetRenderTargets(1, &g_gpuMeshPreview.rtvCpu, FALSE, &g_gpuMeshPreview.dsvCpu);
        commandList->RSSetViewports(1, &vp);
        commandList->RSSetScissorRects(1, &scissor);

        // Sky pass (drawn before terrain so the mesh's depth-test wins). Sets
        // its own root signature and PSO; we restore the mesh root sig +
        // constants below so the descriptor table binding works.
        SkyShaderConstants skyBase{};
        skyBase.cameraRight[0]   = constants.cameraRight[0];
        skyBase.cameraRight[1]   = constants.cameraRight[1];
        skyBase.cameraRight[2]   = constants.cameraRight[2];
        skyBase.cameraUp[0]      = constants.cameraUp[0];
        skyBase.cameraUp[1]      = constants.cameraUp[1];
        skyBase.cameraUp[2]      = constants.cameraUp[2];
        skyBase.cameraForward[0] = constants.cameraForward[0];
        skyBase.cameraForward[1] = constants.cameraForward[1];
        skyBase.cameraForward[2] = constants.cameraForward[2];
        skyBase.projScaleX = constants.projScaleX;
        skyBase.projScaleY = constants.projScaleY;
        skyBase.panNdcX = constants.panNdcX;
        skyBase.panNdcY = constants.panNdcY;
        skyBase.sunDirection[0] = constants.sunDirection[0];
        skyBase.sunDirection[1] = constants.sunDirection[1];
        skyBase.sunDirection[2] = constants.sunDirection[2];
        if (RenderSkyPass(commandList.Get(), g_graph.Settings().sky, skyBase))
        {
            recordDraw(3, true);
            renderStats.skyPass = true;
        }

        // Cloud shadow texture: regenerated each frame from the same cloud
        // volume the cloud render pass uses. Before the mesh draw so the
        // surface PS can sample it. CloudShadowMeshConstants in the upload
        // CB tells the shader where the texture lives in world XZ.
        const rock::CloudSettings& cloudSettingsForShadow = g_graph.Settings().clouds;
        const CloudLoopVector cloudLoop = ComputeCloudLoopVector(cloudSettingsForShadow);
        const float cloudLoopPhase = std::clamp(cloudSettingsForShadow.loopPhase, 0.0f, 1.0f);
        const float windOffsetX = cloudLoop.xMeters * cloudLoopPhase;
        const float windOffsetZ = cloudLoop.zMeters * cloudLoopPhase;

        // Expand the shadow footprint a bit beyond the mesh so projected
        // shadows don't get clamped to the mesh edges.
        const float shadowMargin = std::max(boundsDiagonal * 0.4f, 1024.0f);
        const float shadowMinX = boundsMin.x - shadowMargin;
        const float shadowMinZ = boundsMin.z - shadowMargin;
        const float shadowSizeX = (boundsMax.x - boundsMin.x) + shadowMargin * 2.0f;
        const float shadowSizeZ = (boundsMax.z - boundsMin.z) + shadowMargin * 2.0f;

        bool cloudShadowReady = false;
        if (g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric &&
            cloudSettingsForShadow.enabled &&
            cloudSettingsForShadow.shadowStrength > 0.001f)
        {
            std::string ignored;
            if (EnsureCloudVolume(cloudSettingsForShadow.seed, &ignored))
            {
                cloudShadowReady = RunCloudShadowGeneration(
                    cloudSettingsForShadow,
                    shadowMinX, shadowMinZ, shadowSizeX, shadowSizeZ,
                    constants.sunDirection, windOffsetX, windOffsetZ,
                    boundsCenter.x, boundsCenter.z, &ignored);
            }
        }

        std::string cloudShadowCbError;
        EnsureCloudShadowMeshCb(&cloudShadowCbError);
        EnsureDummyCloudShadowTexture(&cloudShadowCbError);

        CloudShadowMeshConstants cloudShadowCb{};
        cloudShadowCb.cloudShadowEnabled = cloudShadowReady ? 1.0f : 0.0f;
        cloudShadowCb.cloudShadowStrength = cloudShadowReady ? std::clamp(cloudSettingsForShadow.shadowStrength, 0.0f, 1.0f) : 0.0f;
        cloudShadowCb.cloudShadowAltitudeMin = cloudSettingsForShadow.altitudeMin;
        cloudShadowCb.aoEnabled = (g_graph.Settings().preview.aoEnabled && aoTextureReady && !g_graph.Evaluation().previewShowsMask) ? 1.0f : 0.0f;
        cloudShadowCb.cloudShadowMinX = shadowMinX;
        cloudShadowCb.cloudShadowMinZ = shadowMinZ;
        cloudShadowCb.cloudShadowSizeX = shadowSizeX;
        cloudShadowCb.cloudShadowSizeZ = shadowSizeZ;

        // Atmospheric environment: in Atmospheric mode the four colours are
        // sampled from the same Nishita model the sky shader uses, so the
        // terrain ambient and the sky stay consistent across sun elevation
        // (warm everything at sunset, dim everything at night). SolidColor
        // mode falls back to the viewport background as a uniform dome.
        const rock::SkySettings& sky = g_graph.Settings().sky;
        const auto fillColor4 = [](float dst[4], const std::array<float, 3>& src) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 1.0f;
        };
        if (sky.mode == rock::SkyMode::Atmospheric)
        {
            const AtmosphereSamples atm = SampleAtmosphericEnvironment(
                sky, constants.sunDirection[0], constants.sunDirection[1], constants.sunDirection[2]);
            fillColor4(cloudShadowCb.skyZenithColor, atm.zenith);
            fillColor4(cloudShadowCb.skyHorizonColor, atm.horizon);
            fillColor4(cloudShadowCb.skyGroundColor, atm.ground);
            fillColor4(cloudShadowCb.skySunColor, atm.sun);
        }
        else
        {
            const auto& bg = g_graph.Settings().preview.viewportBackground;
            fillColor4(cloudShadowCb.skyZenithColor, bg);
            fillColor4(cloudShadowCb.skyHorizonColor, bg);
            fillColor4(cloudShadowCb.skyGroundColor, bg);
            // White sun in flat-sky mode (no atmosphere reddening to simulate).
            const std::array<float, 3> white{1.0f, 1.0f, 1.0f};
            fillColor4(cloudShadowCb.skySunColor, white);
        }

        bool dofReady = false;
        if (useDepthOfField)
        {
            std::string ignoredErr;
            dofReady = EnsureDepthOfFieldPipeline(&ignoredErr);
        }
        bool tonemapReady = false;
        if (g_graph.Settings().preview.hdrViewportEnabled || ColorTemperatureAdjusted())
        {
            std::string tonemapErr;
            tonemapReady = EnsureTonemapPipeline(&tonemapErr);
            if (!tonemapReady)
            {
                if (error) *error = tonemapErr;
                return false;
            }
        }
        const std::array<float, 3> sectionColor = colorTextureReady
            ? EstimateSectionColor(g_graph.Evaluation().previewColorGrid, g_graph.Settings().preview.pbrAlbedo)
            : EstimateSectionColor(rock::ColorGrid{}, g_graph.Settings().preview.pbrAlbedo);
        fillColor4(cloudShadowCb.sectionColor, sectionColor);
        cloudShadowCb.atmosphereDensity =
            (sky.mode == rock::SkyMode::Atmospheric) ? std::clamp(sky.atmosphereDensity, 0.05f, 8.0f) : 0.0f;
        cloudShadowCb.atmosphereMieStrength =
            (sky.mode == rock::SkyMode::Atmospheric) ? std::clamp(sky.mieStrength, 0.0f, 8.0f) : 0.0f;
        cloudShadowCb.aoStrength = std::clamp(g_graph.Settings().preview.aoStrength, 0.0f, 1.0f);
        cloudShadowCb.pad1 = 0.0f;
        cloudShadowCb.waterLevelParam = previewSettings.waterLevelMeters;
        cloudShadowCb.waterWavesScale = std::max(previewSettings.waterWavesScale, 0.01f);
        cloudShadowCb.waterRefractiveIndex = std::clamp(previewSettings.waterRefractiveIndex, 1.0f, 4.0f);
        cloudShadowCb.waterFresnelPower = std::clamp(previewSettings.waterFresnelPower, 1.0f, 8.0f);
        cloudShadowCb.waterRefractionStrength = std::clamp(previewSettings.waterRefractionStrength, 0.0f, 2.0f);
        cloudShadowCb.waterTimeSeconds = s_waterTimeSeconds;
        cloudShadowCb.waterAnimEnabled = previewSettings.waterAnimationEnabled ? 1.0f : 0.0f;
        cloudShadowCb.waterReflectionStrength = std::clamp(previewSettings.waterReflectionStrength, 0.0f, 3.0f);
        cloudShadowCb.waterSsrEnabled = previewSettings.waterSsrEnabled ? 1.0f : 0.0f;
        cloudShadowCb.pad2[0] = 0.0f;
        cloudShadowCb.pad2[1] = 0.0f;
        cloudShadowCb.pad2[2] = 0.0f;

        if (g_gpuClouds.meshCbMapped)
        {
            std::memcpy(g_gpuClouds.meshCbMapped, &cloudShadowCb, sizeof(cloudShadowCb));
        }

        // Restore mesh state for the surface draws below. Cloud pass moves to
        // after the mesh draws so it can sample depth and limit ray-march.
        ID3D12DescriptorHeap* descriptorHeaps[] = {g_srvHeap.Get()};
        commandList->SetDescriptorHeaps(1, descriptorHeaps);
        commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.rootSignature.Get());
        commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
        if (g_gpuClouds.meshCbUploadBuffer)
        {
            commandList->SetGraphicsRootConstantBufferView(1, g_gpuClouds.meshCbUploadBuffer->GetGPUVirtualAddress());
        }
        D3D12_GPU_DESCRIPTOR_HANDLE cloudShadowGpu = cloudShadowReady && g_gpuClouds.shadowSrvAllocated
            ? g_gpuClouds.shadowSrvGpu
            : g_gpuClouds.dummyShadowSrvGpu;
        std::string meshResourceError;
        if (!EnsureMeshResourceTable(&meshResourceError))
        {
            if (error) *error = meshResourceError;
            return false;
        }
        UpdateMeshResourceTable(cloudShadowGpu);
        commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.meshResourceTableGpu);

        if (showSurface && displacementReady &&
            (useTessellation ? g_gpuMeshPreview.displacementPatchIndexCount > 0 : g_gpuMeshPreview.displacementTriIndexCount > 0))
        {
            // GPU displacement path: switch root signature + bind everything
            // the displacement PSOs need. CPU mesh root sig is restored in
            // the trailing block so subsequent wireframe / grid draws still
            // work as before.
            void* mappedCbv = nullptr;
            D3D12_RANGE readRange{0, 0};
            g_meshPreviewPipelines.displacementCbv->Map(0, &readRange, &mappedCbv);
            std::memcpy(mappedCbv, &constants, sizeof(constants));
            g_meshPreviewPipelines.displacementCbv->Unmap(0, nullptr);

            DisplacementShaderConstants dispConsts{};
            const int previewMeshResolutionForDisp = g_gpuMeshPreview.displacementMeshResolution;
            dispConsts.gridResolution = static_cast<float>(previewMeshResolutionForDisp);
            dispConsts.terrainSize = previewGrid.terrainSizeMeters;
            dispConsts.halfSize = previewGrid.terrainSizeMeters * 0.5f;
            dispConsts.worldDX = (previewMeshResolutionForDisp > 1)
                ? previewGrid.terrainSizeMeters / static_cast<float>(previewMeshResolutionForDisp - 1)
                : 1.0f;
            dispConsts.tessellationMinFactor = std::clamp(g_graph.Settings().preview.tessellationMinFactor, 1.0f, 64.0f);
            dispConsts.tessellationMaxFactor = std::clamp(g_graph.Settings().preview.tessellationMaxFactor, dispConsts.tessellationMinFactor, 64.0f);
            dispConsts.tessellationNearDistance = std::max(1.0f, g_graph.Settings().preview.tessellationNearDistance);
            dispConsts.tessellationFarDistance = std::max(dispConsts.tessellationNearDistance + 1.0f, g_graph.Settings().preview.tessellationFarDistance);

            commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.displacementRootSignature.Get());
            commandList->SetGraphicsRootConstantBufferView(0, g_meshPreviewPipelines.displacementCbv->GetGPUVirtualAddress());
            if (g_gpuClouds.meshCbUploadBuffer)
            {
                commandList->SetGraphicsRootConstantBufferView(1, g_gpuClouds.meshCbUploadBuffer->GetGPUVirtualAddress());
            }
            commandList->SetGraphicsRoot32BitConstants(2, sizeof(dispConsts) / 4, &dispConsts, 0);
            commandList->SetGraphicsRootDescriptorTable(3, g_gpuMeshPreview.shadowSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(4, cloudShadowGpu);
            commandList->SetGraphicsRootDescriptorTable(5, g_gpuMeshPreview.displacementHeightSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(6, g_gpuMeshPreview.displacementMaskSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(7, g_gpuMeshPreview.meshResourceTableAllocated ? OffsetGpuSrv(g_gpuMeshPreview.meshResourceTableGpu, 4) : g_gpuClouds.dummyShadowSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(8, g_gpuMeshPreview.meshResourceTableAllocated ? OffsetGpuSrv(g_gpuMeshPreview.meshResourceTableGpu, 5) : g_gpuClouds.dummyShadowSrvGpu);

            ID3D12Resource* surfaceIndexBuffer = useTessellation
                ? g_gpuMeshPreview.displacementPatchIndexBuffer.Get()
                : g_gpuMeshPreview.displacementTriIndexBuffer.Get();
            const UINT surfaceIndexCount = useTessellation
                ? g_gpuMeshPreview.displacementPatchIndexCount
                : g_gpuMeshPreview.displacementTriIndexCount;
            D3D12_INDEX_BUFFER_VIEW ibv{
                surfaceIndexBuffer->GetGPUVirtualAddress(),
                surfaceIndexCount * static_cast<UINT>(sizeof(UINT)),
                DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            // No vertex buffer for displacement — VS reads SV_VertexID.
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->SetPipelineState(useTessellation
                ? g_meshPreviewPipelines.displacementTessSurfacePso.Get()
                : g_meshPreviewPipelines.displacementSurfacePso.Get());
            commandList->IASetPrimitiveTopology(useTessellation
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(surfaceIndexCount, 1, 0, 0, 0);
            if (useTessellation)
            {
                recordPatchDraw(surfaceIndexCount, dispConsts.tessellationMaxFactor);
            }
            else
            {
                recordIndexedDraw(surfaceIndexCount, true);
            }
            if (showSectionPolygons && g_gpuMeshPreview.displacementSectionIndexCount > 0)
            {
                D3D12_INDEX_BUFFER_VIEW sectionIbv{
                    g_gpuMeshPreview.displacementSectionIndexBuffer->GetGPUVirtualAddress(),
                    g_gpuMeshPreview.displacementSectionIndexCount * static_cast<UINT>(sizeof(UINT)),
                    DXGI_FORMAT_R32_UINT};
                commandList->IASetIndexBuffer(&sectionIbv);
                commandList->SetPipelineState(g_meshPreviewPipelines.displacementSectionPso.Get());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawIndexedInstanced(g_gpuMeshPreview.displacementSectionIndexCount, 1, 0, 0, 0);
                recordIndexedDraw(g_gpuMeshPreview.displacementSectionIndexCount, true);
            }
            renderStats.surfacePass = true;

            // Restore the CPU root sig + constants for wireframe / grid
            // draws below (those still expect the CPU mesh root sig).
            commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.rootSignature.Get());
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            if (g_gpuClouds.meshCbUploadBuffer)
            {
                commandList->SetGraphicsRootConstantBufferView(1, g_gpuClouds.meshCbUploadBuffer->GetGPUVirtualAddress());
            }
            commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.meshResourceTableGpu);
            // Re-bind the CPU vertex buffer for grid / wireframe.
            if (hasMeshVertices)
            {
                commandList->IASetVertexBuffers(0, 1, &vbv);
            }
        }
        else if (showSurface && g_gpuMeshPreview.triIndexCount > 0)
        {
            const UINT surfaceIndexCount = cpuSurfaceIndexCount();
            D3D12_INDEX_BUFFER_VIEW ibv{g_gpuMeshPreview.indexBuffer->GetGPUVirtualAddress(), surfaceIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            commandList->SetPipelineState(g_meshPreviewPipelines.surfacePso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(surfaceIndexCount, 1, 0, 0, 0);
            recordIndexedDraw(surfaceIndexCount, true);
            renderStats.surfacePass = true;
        }
        if (showWater && g_gpuMeshPreview.postTarget && g_gpuMeshPreview.sceneDepthTarget)
        {
            commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

            D3D12_RESOURCE_BARRIER toCopy[4]{};
            toCopy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[0].Transition.pResource = g_gpuMeshPreview.colorTarget.Get();
            toCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            toCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[1].Transition.pResource = g_gpuMeshPreview.postTarget.Get();
            toCopy[1].Transition.StateBefore = g_gpuMeshPreview.postState;
            toCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[2].Transition.pResource = g_gpuMeshPreview.depthTarget.Get();
            toCopy[2].Transition.StateBefore = g_gpuMeshPreview.depthState;
            toCopy[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            toCopy[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[3].Transition.pResource = g_gpuMeshPreview.sceneDepthTarget.Get();
            toCopy[3].Transition.StateBefore = g_gpuMeshPreview.sceneDepthState;
            toCopy[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            toCopy[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(4, toCopy);

            commandList->CopyResource(g_gpuMeshPreview.postTarget.Get(), g_gpuMeshPreview.colorTarget.Get());
            commandList->CopyResource(g_gpuMeshPreview.sceneDepthTarget.Get(), g_gpuMeshPreview.depthTarget.Get());

            D3D12_RESOURCE_BARRIER afterCopy[4]{};
            afterCopy[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            afterCopy[0].Transition.pResource = g_gpuMeshPreview.colorTarget.Get();
            afterCopy[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            afterCopy[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            afterCopy[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            afterCopy[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            afterCopy[1].Transition.pResource = g_gpuMeshPreview.postTarget.Get();
            afterCopy[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            afterCopy[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            afterCopy[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            afterCopy[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            afterCopy[2].Transition.pResource = g_gpuMeshPreview.depthTarget.Get();
            afterCopy[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            afterCopy[2].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            afterCopy[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            afterCopy[3].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            afterCopy[3].Transition.pResource = g_gpuMeshPreview.sceneDepthTarget.Get();
            afterCopy[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            afterCopy[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            afterCopy[3].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(4, afterCopy);

            g_gpuMeshPreview.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            g_gpuMeshPreview.postState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_gpuMeshPreview.depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            g_gpuMeshPreview.sceneDepthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            commandList->OMSetRenderTargets(1, &g_gpuMeshPreview.rtvCpu, FALSE, &g_gpuMeshPreview.dsvCpu);
            commandList->RSSetViewports(1, &vp);
            commandList->RSSetScissorRects(1, &scissor);
        }

        if (showWater && g_gpuMeshPreview.waterVertexBuffer && g_gpuMeshPreview.waterIndexBuffer && g_gpuMeshPreview.waterIndexCount > 0)
        {
            MeshPreviewConstants waterConstants = constants;
            waterConstants.albedoColor[0] = std::clamp(g_graph.Settings().preview.waterColor[0], 0.0f, 1.0f);
            waterConstants.albedoColor[1] = std::clamp(g_graph.Settings().preview.waterColor[1], 0.0f, 1.0f);
            waterConstants.albedoColor[2] = std::clamp(g_graph.Settings().preview.waterColor[2], 0.0f, 1.0f);
            waterConstants.albedoColor[3] = std::max(previewGrid.terrainSizeMeters, 1.0f);
            waterConstants.maskPreview = 0.0f;
            waterConstants.colorTextureMode = 0.0f;
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(waterConstants) / 4, &waterConstants, 0);

            D3D12_VERTEX_BUFFER_VIEW waterVbv{};
            waterVbv.BufferLocation = g_gpuMeshPreview.waterVertexBuffer->GetGPUVirtualAddress();
            waterVbv.SizeInBytes = g_gpuMeshPreview.waterVertexCount * static_cast<UINT>(sizeof(rock::MeshVertex));
            waterVbv.StrideInBytes = static_cast<UINT>(sizeof(rock::MeshVertex));
            D3D12_INDEX_BUFFER_VIEW waterIbv{
                g_gpuMeshPreview.waterIndexBuffer->GetGPUVirtualAddress(),
                g_gpuMeshPreview.waterIndexCount * static_cast<UINT>(sizeof(UINT)),
                DXGI_FORMAT_R32_UINT};
            commandList->IASetVertexBuffers(0, 1, &waterVbv);
            commandList->IASetIndexBuffer(&waterIbv);
            commandList->SetPipelineState(g_meshPreviewPipelines.waterPso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(g_gpuMeshPreview.waterIndexCount, 1, 0, 0, 0);
            recordIndexedDraw(g_gpuMeshPreview.waterIndexCount, true);
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            if (hasMeshVertices)
            {
                commandList->IASetVertexBuffers(0, 1, &vbv);
            }
        }
        if (showWireframe && displacementReady &&
            (useTessellation ? g_gpuMeshPreview.displacementPatchIndexCount > 0 : g_gpuMeshPreview.displacementTriIndexCount > 0))
        {
            MeshPreviewConstants wireConstants = constants;
            wireConstants.albedoColor[0] = 0.05f;
            wireConstants.albedoColor[1] = 0.86f;
            wireConstants.albedoColor[2] = 1.00f;
            wireConstants.albedoColor[3] = 0.18f;
            wireConstants.colorTextureMode = 0.0f;

            void* mappedCbv = nullptr;
            D3D12_RANGE readRange{0, 0};
            g_meshPreviewPipelines.displacementCbv->Map(0, &readRange, &mappedCbv);
            std::memcpy(mappedCbv, &wireConstants, sizeof(wireConstants));
            g_meshPreviewPipelines.displacementCbv->Unmap(0, nullptr);

            DisplacementShaderConstants dispConsts{};
            const int previewMeshResolutionForDisp = g_gpuMeshPreview.displacementMeshResolution;
            dispConsts.gridResolution = static_cast<float>(previewMeshResolutionForDisp);
            dispConsts.terrainSize = previewGrid.terrainSizeMeters;
            dispConsts.halfSize = previewGrid.terrainSizeMeters * 0.5f;
            dispConsts.worldDX = (previewMeshResolutionForDisp > 1)
                ? previewGrid.terrainSizeMeters / static_cast<float>(previewMeshResolutionForDisp - 1)
                : 1.0f;
            dispConsts.tessellationMinFactor = std::clamp(g_graph.Settings().preview.tessellationMinFactor, 1.0f, 64.0f);
            dispConsts.tessellationMaxFactor = std::clamp(g_graph.Settings().preview.tessellationMaxFactor, dispConsts.tessellationMinFactor, 64.0f);
            dispConsts.tessellationNearDistance = std::max(1.0f, g_graph.Settings().preview.tessellationNearDistance);
            dispConsts.tessellationFarDistance = std::max(dispConsts.tessellationNearDistance + 1.0f, g_graph.Settings().preview.tessellationFarDistance);

            commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.displacementRootSignature.Get());
            commandList->SetGraphicsRootConstantBufferView(0, g_meshPreviewPipelines.displacementCbv->GetGPUVirtualAddress());
            if (g_gpuClouds.meshCbUploadBuffer)
            {
                commandList->SetGraphicsRootConstantBufferView(1, g_gpuClouds.meshCbUploadBuffer->GetGPUVirtualAddress());
            }
            commandList->SetGraphicsRoot32BitConstants(2, sizeof(dispConsts) / 4, &dispConsts, 0);
            commandList->SetGraphicsRootDescriptorTable(3, g_gpuMeshPreview.shadowSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(4, cloudShadowGpu);
            commandList->SetGraphicsRootDescriptorTable(5, g_gpuMeshPreview.displacementHeightSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(6, g_gpuMeshPreview.displacementMaskSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(7, g_gpuClouds.dummyShadowSrvGpu);
            commandList->SetGraphicsRootDescriptorTable(8, g_gpuClouds.dummyShadowSrvGpu);

            ID3D12Resource* wireIndexBuffer = useTessellation
                ? g_gpuMeshPreview.displacementPatchIndexBuffer.Get()
                : g_gpuMeshPreview.displacementTriIndexBuffer.Get();
            const UINT wireIndexCount = useTessellation
                ? g_gpuMeshPreview.displacementPatchIndexCount
                : g_gpuMeshPreview.displacementTriIndexCount;
            D3D12_INDEX_BUFFER_VIEW ibv{
                wireIndexBuffer->GetGPUVirtualAddress(),
                wireIndexCount * static_cast<UINT>(sizeof(UINT)),
                DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->SetPipelineState(useTessellation
                ? g_meshPreviewPipelines.displacementTessWirePso.Get()
                : g_meshPreviewPipelines.displacementWirePso.Get());
            commandList->IASetPrimitiveTopology(useTessellation
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(wireIndexCount, 1, 0, 0, 0);
            if (useTessellation)
            {
                recordPatchDraw(wireIndexCount, dispConsts.tessellationMaxFactor);
            }
            else
            {
                recordIndexedDraw(wireIndexCount, true);
            }
            if (terrainBoundaryMode == rock::TerrainBoundaryMode::SectionPolygon && g_gpuMeshPreview.displacementSectionIndexCount > 0)
            {
                D3D12_INDEX_BUFFER_VIEW sectionIbv{
                    g_gpuMeshPreview.displacementSectionIndexBuffer->GetGPUVirtualAddress(),
                    g_gpuMeshPreview.displacementSectionIndexCount * static_cast<UINT>(sizeof(UINT)),
                    DXGI_FORMAT_R32_UINT};
                commandList->IASetIndexBuffer(&sectionIbv);
                commandList->SetPipelineState(g_meshPreviewPipelines.displacementSectionWirePso.Get());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawIndexedInstanced(g_gpuMeshPreview.displacementSectionIndexCount, 1, 0, 0, 0);
                recordIndexedDraw(g_gpuMeshPreview.displacementSectionIndexCount, true);
            }
            renderStats.wireframePass = true;

            commandList->SetGraphicsRootSignature(g_meshPreviewPipelines.rootSignature.Get());
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            if (g_gpuClouds.meshCbUploadBuffer)
            {
                commandList->SetGraphicsRootConstantBufferView(1, g_gpuClouds.meshCbUploadBuffer->GetGPUVirtualAddress());
            }
            commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.meshResourceTableGpu);
            if (hasMeshVertices)
            {
                commandList->IASetVertexBuffers(0, 1, &vbv);
            }
        }
        if (showTerrainBoundaryLines && g_gpuMeshPreview.terrainBoundaryLineVertexCount > 0)
        {
            constants.albedoColor[0] = 0.42f;
            constants.albedoColor[1] = 0.42f;
            constants.albedoColor[2] = 0.42f;
            constants.albedoColor[3] = 1.0f;
            constants.colorTextureMode = 0.0f;
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            D3D12_VERTEX_BUFFER_VIEW boundaryVbv{};
            boundaryVbv.BufferLocation = g_gpuMeshPreview.terrainBoundaryLineVertexBuffer->GetGPUVirtualAddress();
            boundaryVbv.SizeInBytes = g_gpuMeshPreview.terrainBoundaryLineVertexCount * static_cast<UINT>(sizeof(rock::MeshVertex));
            boundaryVbv.StrideInBytes = static_cast<UINT>(sizeof(rock::MeshVertex));
            commandList->IASetVertexBuffers(0, 1, &boundaryVbv);
            commandList->SetPipelineState(g_meshPreviewPipelines.gridPso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            commandList->DrawInstanced(g_gpuMeshPreview.terrainBoundaryLineVertexCount, 1, 0, 0);
            recordDraw(g_gpuMeshPreview.terrainBoundaryLineVertexCount, false);
            if (hasMeshVertices)
            {
                commandList->IASetVertexBuffers(0, 1, &vbv);
            }
        }
        if (showGrid && g_gpuMeshPreview.gridVertexCount > 0)
        {
            constants.albedoColor[0] = g_graph.Settings().preview.gridColor[0];
            constants.albedoColor[1] = g_graph.Settings().preview.gridColor[1];
            constants.albedoColor[2] = g_graph.Settings().preview.gridColor[2];
            constants.albedoColor[3] = 1.0f;
            constants.colorTextureMode = 0.0f;
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            D3D12_VERTEX_BUFFER_VIEW gridVbv{};
            gridVbv.BufferLocation = g_gpuMeshPreview.gridVertexBuffer->GetGPUVirtualAddress();
            gridVbv.SizeInBytes = g_gpuMeshPreview.gridVertexCount * static_cast<UINT>(sizeof(rock::MeshVertex));
            gridVbv.StrideInBytes = static_cast<UINT>(sizeof(rock::MeshVertex));
            commandList->IASetVertexBuffers(0, 1, &gridVbv);
            commandList->SetPipelineState(g_meshPreviewPipelines.gridPso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            commandList->DrawInstanced(g_gpuMeshPreview.gridVertexCount, 1, 0, 0);
            recordDraw(g_gpuMeshPreview.gridVertexCount, false);
            renderStats.gridPass = true;
            if (hasMeshVertices)
            {
                commandList->IASetVertexBuffers(0, 1, &vbv);
            }
        }
        if (showWireframe && g_gpuMeshPreview.edgeIndexCount > 0)
        {
            constants.albedoColor[0] = 0.18f;
            constants.albedoColor[1] = 0.20f;
            constants.albedoColor[2] = 0.19f;
            constants.albedoColor[3] = 1.0f;
            constants.colorTextureMode = 0.0f;
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            const UINT edgeIndexCount = cpuEdgeIndexCount();
            D3D12_INDEX_BUFFER_VIEW ibv{g_gpuMeshPreview.edgeIndexBuffer->GetGPUVirtualAddress(), edgeIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            commandList->SetPipelineState(g_meshPreviewPipelines.wirePso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            commandList->DrawIndexedInstanced(edgeIndexCount, 1, 0, 0, 0);
            recordIndexedDraw(edgeIndexCount, false);
            renderStats.wireframePass = true;
        }

        // Cloud pass: now that terrain has written depth, transition depth to
        // SRV and ray-march cloud over the existing color. Each pixel reads
        // depth to clamp tExit so cloud renders correctly in front of distant
        // terrain and is occluded by closer terrain. Alpha-blended over the
        // already-rendered scene with SRC_ALPHA / INV_SRC_ALPHA.
        const rock::CloudSettings& cloudSettings = g_graph.Settings().clouds;
        if (g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric && cloudSettings.enabled)
        {
            D3D12_RESOURCE_BARRIER depthToSrv{};
            depthToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            depthToSrv.Transition.pResource = g_gpuMeshPreview.depthTarget.Get();
            depthToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            depthToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            depthToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &depthToSrv);
            g_gpuMeshPreview.depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

            CloudRenderShaderConstants cloudBase{};
            cloudBase.cameraPosition[0] = constants.cameraPosition[0];
            cloudBase.cameraPosition[1] = constants.cameraPosition[1];
            cloudBase.cameraPosition[2] = constants.cameraPosition[2];
            cloudBase.cameraRight[0] = constants.cameraRight[0];
            cloudBase.cameraRight[1] = constants.cameraRight[1];
            cloudBase.cameraRight[2] = constants.cameraRight[2];
            cloudBase.cameraUp[0] = constants.cameraUp[0];
            cloudBase.cameraUp[1] = constants.cameraUp[1];
            cloudBase.cameraUp[2] = constants.cameraUp[2];
            cloudBase.cameraForward[0] = constants.cameraForward[0];
            cloudBase.cameraForward[1] = constants.cameraForward[1];
            cloudBase.cameraForward[2] = constants.cameraForward[2];
            cloudBase.projScaleX = constants.projScaleX;
            cloudBase.projScaleY = constants.projScaleY;
            cloudBase.panNdcX = constants.panNdcX;
            cloudBase.panNdcY = constants.panNdcY;
            cloudBase.sunDirection[0] = constants.sunDirection[0];
            cloudBase.sunDirection[1] = constants.sunDirection[1];
            cloudBase.sunDirection[2] = constants.sunDirection[2];
            cloudBase.nearPlane = constants.nearPlane;
            cloudBase.farPlane = constants.farPlane;

            // Pull atmosphere-derived sun + ambient sky colours so the
            // cloud body warms with sunset, dims at night and stays
            // visually consistent with the sky / terrain lighting. In
            // SolidColor sky mode pass white so the cloud user-colour
            // shows through unchanged.
            const rock::SkySettings& skyForCloud = g_graph.Settings().sky;
            std::array<float, 3> atmSunColor{1.0f, 1.0f, 1.0f};
            std::array<float, 3> atmSkyColor{1.0f, 1.0f, 1.0f};
            if (skyForCloud.mode == rock::SkyMode::Atmospheric)
            {
                const AtmosphereSamples atm = SampleAtmosphericEnvironment(
                    skyForCloud, constants.sunDirection[0], constants.sunDirection[1], constants.sunDirection[2]);
                atmSunColor = atm.sun;
                // Use a hemisphere-ish hue for clouds so their ambient stays
                // close to terrain ambient instead of leaning only on the
                // often more violet zenith colour. Keep the old zenith
                // luminance and chroma amount so this remains a hue adjustment
                // rather than a brightness/saturation change.
                constexpr float kCloudAmbientHorizonMix = 0.4f;
                std::array<float, 3> hemiSkyColor = {
                    atm.zenith[0] * (1.0f - kCloudAmbientHorizonMix) + atm.horizon[0] * kCloudAmbientHorizonMix,
                    atm.zenith[1] * (1.0f - kCloudAmbientHorizonMix) + atm.horizon[1] * kCloudAmbientHorizonMix,
                    atm.zenith[2] * (1.0f - kCloudAmbientHorizonMix) + atm.horizon[2] * kCloudAmbientHorizonMix,
                };
                const float oldLum = atm.zenith[0] * 0.2126f + atm.zenith[1] * 0.7152f + atm.zenith[2] * 0.0722f;
                const float hemiLum = hemiSkyColor[0] * 0.2126f + hemiSkyColor[1] * 0.7152f + hemiSkyColor[2] * 0.0722f;
                const float oldChromaR = atm.zenith[0] - oldLum;
                const float oldChromaG = atm.zenith[1] - oldLum;
                const float oldChromaB = atm.zenith[2] - oldLum;
                const float hemiChromaR = hemiSkyColor[0] - hemiLum;
                const float hemiChromaG = hemiSkyColor[1] - hemiLum;
                const float hemiChromaB = hemiSkyColor[2] - hemiLum;
                const float oldChromaLen = std::sqrt(oldChromaR * oldChromaR + oldChromaG * oldChromaG + oldChromaB * oldChromaB);
                const float hemiChromaLen = std::sqrt(hemiChromaR * hemiChromaR + hemiChromaG * hemiChromaG + hemiChromaB * hemiChromaB);
                const float chromaScale = oldChromaLen / std::max(hemiChromaLen, 1.0e-5f);
                atmSkyColor = {
                    std::max(oldLum + hemiChromaR * chromaScale, 0.0f),
                    std::max(oldLum + hemiChromaG * chromaScale, 0.0f),
                    std::max(oldLum + hemiChromaB * chromaScale, 0.0f),
                };
            }
            cloudBase.atmosphereSunColor[0] = atmSunColor[0];
            cloudBase.atmosphereSunColor[1] = atmSunColor[1];
            cloudBase.atmosphereSunColor[2] = atmSunColor[2];
            cloudBase.atmosphereSunColor[3] = 1.0f;
            cloudBase.atmosphereSkyColor[0] = atmSkyColor[0];
            cloudBase.atmosphereSkyColor[1] = atmSkyColor[1];
            cloudBase.atmosphereSkyColor[2] = atmSkyColor[2];
            cloudBase.atmosphereSkyColor[3] = 1.0f;

            // windOffsetX / windOffsetZ are already computed earlier for the
            // cloud-shadow generation pass.
            std::string cloudVolumeError;
            if (EnsureCloudVolume(cloudSettings.seed, &cloudVolumeError))
            {
                if (RenderCloudPass(commandList.Get(), cloudSettings, cloudBase,
                                    windOffsetX, windOffsetZ,
                                    boundsCenter.x, boundsCenter.z,
                                    g_gpuMeshPreview.depthSrvGpu))
                {
                    recordDraw(3, true);
                    renderStats.cloudsPass = true;
                }
            }
        }

        D3D12_RESOURCE_BARRIER toSrv{};
        toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrv.Transition.pResource = g_gpuMeshPreview.colorTarget.Get();
        toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toSrv);
        g_gpuMeshPreview.colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (useDepthOfField && dofReady && g_gpuMeshPreview.postTarget)
        {
            if (g_gpuMeshPreview.depthState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            {
                D3D12_RESOURCE_BARRIER depthToSrv{};
                depthToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                depthToSrv.Transition.pResource = g_gpuMeshPreview.depthTarget.Get();
                depthToSrv.Transition.StateBefore = g_gpuMeshPreview.depthState;
                depthToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                depthToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &depthToSrv);
                g_gpuMeshPreview.depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            if (g_gpuMeshPreview.postState != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                D3D12_RESOURCE_BARRIER postToRt{};
                postToRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                postToRt.Transition.pResource = g_gpuMeshPreview.postTarget.Get();
                postToRt.Transition.StateBefore = g_gpuMeshPreview.postState;
                postToRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                postToRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &postToRt);
                g_gpuMeshPreview.postState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }

            const float clearPost[] = {0.0f, 0.0f, 0.0f, 0.0f};
            commandList->ClearRenderTargetView(g_gpuMeshPreview.postRtvCpu, clearPost, 0, nullptr);
            commandList->OMSetRenderTargets(1, &g_gpuMeshPreview.postRtvCpu, FALSE, nullptr);
            commandList->RSSetViewports(1, &vp);
            commandList->RSSetScissorRects(1, &scissor);

            DepthOfFieldShaderConstants dof{};
            dof.focusDistance = std::max(0.1f, g_graph.Settings().preview.dofFocusDistanceMeters);
            dof.focalLengthMm = std::clamp(CameraFocalLengthMmFromFovYDegrees(g_viewport.fovDegrees), 8.0f, 300.0f);
            dof.fStop = std::clamp(g_graph.Settings().preview.dofFStop, 0.7f, 32.0f);
            dof.sensorHeightMm = std::clamp(g_graph.Settings().preview.dofSensorHeightMm, 4.0f, 80.0f);
            dof.maxBlurPixels = std::clamp(g_graph.Settings().preview.dofMaxBlurPixels, 0.0f, 64.0f);
            dof.nearPlane = constants.nearPlane;
            dof.farPlane = constants.farPlane;
            dof.apertureShape = static_cast<float>(std::clamp(g_graph.Settings().preview.dofApertureShape, 0, 4));
            dof.apertureBlades = static_cast<float>(std::clamp(g_graph.Settings().preview.dofApertureBlades, 3, 12));
            dof.apertureRotationRadians = std::clamp(g_graph.Settings().preview.dofApertureRotationDegrees, -180.0f, 180.0f) * 3.1415926535f / 180.0f;
            dof.highlightBoost = std::clamp(g_graph.Settings().preview.dofHighlightBoost, 0.0f, 4.0f);
            dof.miniatureScale = g_graph.Settings().preview.dofMiniatureEnabled
                ? std::clamp(g_graph.Settings().preview.dofMiniatureScale, 1.0f, 50.0f)
                : 1.0f;

            ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->SetGraphicsRootSignature(g_dofRootSignature.Get());
            commandList->SetPipelineState(g_dofPso.Get());
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(dof) / 4, &dof, 0);
            commandList->SetGraphicsRootDescriptorTable(1, g_gpuMeshPreview.srvGpu);
            commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.depthSrvGpu);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->IASetIndexBuffer(nullptr);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawInstanced(3, 1, 0, 0);
            recordDraw(3, true);

            D3D12_RESOURCE_BARRIER postToSrv{};
            postToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postToSrv.Transition.pResource = g_gpuMeshPreview.postTarget.Get();
            postToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            postToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            postToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &postToSrv);
            g_gpuMeshPreview.postState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        if (g_graph.Settings().preview.hdrViewportEnabled && tonemapReady && g_gpuMeshPreview.outputTarget)
        {
            const bool useDofSource =
                useDepthOfField &&
                dofReady &&
                g_gpuMeshPreview.postSrvAllocated &&
                g_gpuMeshPreview.postState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            const D3D12_GPU_DESCRIPTOR_HANDLE tonemapSource = useDofSource ? g_gpuMeshPreview.postSrvGpu : g_gpuMeshPreview.srvGpu;

            if (!g_gpuMeshPreview.exposureInitialized &&
                g_gpuMeshPreview.exposureTargets[0] &&
                g_gpuMeshPreview.exposureTargets[1])
            {
                const float clearExposure[] = {0.0f, 0.0f, 0.0f, 1.0f};
                for (int i = 0; i < 2; ++i)
                {
                    if (g_gpuMeshPreview.exposureStates[static_cast<size_t>(i)] != D3D12_RESOURCE_STATE_RENDER_TARGET)
                    {
                        D3D12_RESOURCE_BARRIER b{};
                        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        b.Transition.pResource = g_gpuMeshPreview.exposureTargets[static_cast<size_t>(i)].Get();
                        b.Transition.StateBefore = g_gpuMeshPreview.exposureStates[static_cast<size_t>(i)];
                        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        commandList->ResourceBarrier(1, &b);
                        g_gpuMeshPreview.exposureStates[static_cast<size_t>(i)] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                    }
                    commandList->ClearRenderTargetView(g_gpuMeshPreview.exposureRtvCpu[static_cast<size_t>(i)], clearExposure, 0, nullptr);
                }
                D3D12_RESOURCE_BARRIER historyToSrv{};
                historyToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                historyToSrv.Transition.pResource = g_gpuMeshPreview.exposureTargets[0].Get();
                historyToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                historyToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                historyToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &historyToSrv);
                g_gpuMeshPreview.exposureStates[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                g_gpuMeshPreview.exposureStates[1] = D3D12_RESOURCE_STATE_RENDER_TARGET;
                g_gpuMeshPreview.exposureHistoryIndex = 0;
                g_gpuMeshPreview.exposureInitialized = true;
            }

            const int previousExposureIndex = std::clamp(g_gpuMeshPreview.exposureHistoryIndex, 0, 1);
            const int currentExposureIndex = 1 - previousExposureIndex;
            if (g_gpuMeshPreview.exposureTargets[static_cast<size_t>(currentExposureIndex)] &&
                g_gpuMeshPreview.exposureStates[static_cast<size_t>(currentExposureIndex)] != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                D3D12_RESOURCE_BARRIER exposureToRt{};
                exposureToRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                exposureToRt.Transition.pResource = g_gpuMeshPreview.exposureTargets[static_cast<size_t>(currentExposureIndex)].Get();
                exposureToRt.Transition.StateBefore = g_gpuMeshPreview.exposureStates[static_cast<size_t>(currentExposureIndex)];
                exposureToRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                exposureToRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &exposureToRt);
                g_gpuMeshPreview.exposureStates[static_cast<size_t>(currentExposureIndex)] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }

            TonemapShaderConstants tonemap{};
            tonemap.exposureMode = g_graph.Settings().preview.exposureMode == rock::ExposureMode::Auto ? 1.0f : 0.0f;
            tonemap.exposureEv = std::clamp(g_graph.Settings().preview.exposureEv, -8.0f, 8.0f);
            tonemap.autoExposureBiasEv = std::clamp(g_graph.Settings().preview.autoExposureBiasEv, -4.0f, 4.0f);
            tonemap.autoExposureMinEv = std::clamp(g_graph.Settings().preview.autoExposureMinEv, -8.0f, 8.0f);
            tonemap.autoExposureMaxEv = std::clamp(g_graph.Settings().preview.autoExposureMaxEv, tonemap.autoExposureMinEv, 8.0f);
            tonemap.adaptationRate = std::clamp(g_graph.Settings().preview.autoExposureSpeed, 0.05f, 8.0f);
            tonemap.deltaTimeSeconds = exposureDeltaSeconds;
            tonemap.colorTemperatureKelvin = std::clamp(g_graph.Settings().preview.colorTemperatureKelvin, 2000.0f, 12000.0f);

            ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
            commandList->SetDescriptorHeaps(1, heaps);

            if (g_gpuMeshPreview.exposureInitialized)
            {
                commandList->OMSetRenderTargets(1, &g_gpuMeshPreview.exposureRtvCpu[static_cast<size_t>(currentExposureIndex)], FALSE, nullptr);
                D3D12_VIEWPORT exposureVp{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
                D3D12_RECT exposureScissor{0, 0, 1, 1};
                commandList->RSSetViewports(1, &exposureVp);
                commandList->RSSetScissorRects(1, &exposureScissor);
                commandList->SetGraphicsRootSignature(g_tonemapRootSignature.Get());
                commandList->SetPipelineState(g_exposurePso.Get());
                commandList->SetGraphicsRoot32BitConstants(0, sizeof(tonemap) / 4, &tonemap, 0);
                commandList->SetGraphicsRootDescriptorTable(1, tonemapSource);
                commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.exposureSrvGpu[static_cast<size_t>(previousExposureIndex)]);
                commandList->IASetVertexBuffers(0, 0, nullptr);
                commandList->IASetIndexBuffer(nullptr);
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawInstanced(3, 1, 0, 0);
                recordDraw(3, true);

                D3D12_RESOURCE_BARRIER exposureToSrv{};
                exposureToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                exposureToSrv.Transition.pResource = g_gpuMeshPreview.exposureTargets[static_cast<size_t>(currentExposureIndex)].Get();
                exposureToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                exposureToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                exposureToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &exposureToSrv);
                g_gpuMeshPreview.exposureStates[static_cast<size_t>(currentExposureIndex)] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                g_gpuMeshPreview.exposureHistoryIndex = currentExposureIndex;
            }

            if (g_gpuMeshPreview.outputState != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                D3D12_RESOURCE_BARRIER outputToRt{};
                outputToRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                outputToRt.Transition.pResource = g_gpuMeshPreview.outputTarget.Get();
                outputToRt.Transition.StateBefore = g_gpuMeshPreview.outputState;
                outputToRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                outputToRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &outputToRt);
                g_gpuMeshPreview.outputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }

            const float clearOutput[] = {0.0f, 0.0f, 0.0f, 1.0f};
            commandList->ClearRenderTargetView(g_gpuMeshPreview.outputRtvCpu, clearOutput, 0, nullptr);
            commandList->OMSetRenderTargets(1, &g_gpuMeshPreview.outputRtvCpu, FALSE, nullptr);
            commandList->RSSetViewports(1, &vp);
            commandList->RSSetScissorRects(1, &scissor);

            commandList->SetGraphicsRootSignature(g_tonemapRootSignature.Get());
            commandList->SetPipelineState(g_tonemapPso.Get());
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(tonemap) / 4, &tonemap, 0);
            commandList->SetGraphicsRootDescriptorTable(1, tonemapSource);
            commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.exposureSrvGpu[static_cast<size_t>(g_gpuMeshPreview.exposureHistoryIndex)]);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->IASetIndexBuffer(nullptr);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawInstanced(3, 1, 0, 0);
            recordDraw(3, true);

            D3D12_RESOURCE_BARRIER outputToSrv{};
            outputToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            outputToSrv.Transition.pResource = g_gpuMeshPreview.outputTarget.Get();
            outputToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            outputToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            outputToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &outputToSrv);
            g_gpuMeshPreview.outputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        else if (ColorTemperatureAdjusted() && tonemapReady && g_gpuMeshPreview.outputTarget)
        {
            const bool useDofSource =
                useDepthOfField &&
                dofReady &&
                g_gpuMeshPreview.postSrvAllocated &&
                g_gpuMeshPreview.postState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            const D3D12_GPU_DESCRIPTOR_HANDLE colorGradeSource = useDofSource ? g_gpuMeshPreview.postSrvGpu : g_gpuMeshPreview.srvGpu;

            TonemapShaderConstants colorGrade{};
            colorGrade.colorTemperatureKelvin = std::clamp(g_graph.Settings().preview.colorTemperatureKelvin, 2000.0f, 12000.0f);

            ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
            commandList->SetDescriptorHeaps(1, heaps);

            if (g_gpuMeshPreview.outputState != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                D3D12_RESOURCE_BARRIER outputToRt{};
                outputToRt.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                outputToRt.Transition.pResource = g_gpuMeshPreview.outputTarget.Get();
                outputToRt.Transition.StateBefore = g_gpuMeshPreview.outputState;
                outputToRt.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                outputToRt.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                commandList->ResourceBarrier(1, &outputToRt);
                g_gpuMeshPreview.outputState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }

            const float clearOutput[] = {0.0f, 0.0f, 0.0f, 1.0f};
            commandList->ClearRenderTargetView(g_gpuMeshPreview.outputRtvCpu, clearOutput, 0, nullptr);
            commandList->OMSetRenderTargets(1, &g_gpuMeshPreview.outputRtvCpu, FALSE, nullptr);
            commandList->RSSetViewports(1, &vp);
            commandList->RSSetScissorRects(1, &scissor);

            commandList->SetGraphicsRootSignature(g_tonemapRootSignature.Get());
            commandList->SetPipelineState(g_colorGradePso.Get());
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(colorGrade) / 4, &colorGrade, 0);
            commandList->SetGraphicsRootDescriptorTable(1, colorGradeSource);
            commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.exposureSrvGpu[0]);
            commandList->IASetVertexBuffers(0, 0, nullptr);
            commandList->IASetIndexBuffer(nullptr);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawInstanced(3, 1, 0, 0);
            recordDraw(3, true);

            D3D12_RESOURCE_BARRIER outputToSrv{};
            outputToSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            outputToSrv.Transition.pResource = g_gpuMeshPreview.outputTarget.Get();
            outputToSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            outputToSrv.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            outputToSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            commandList->ResourceBarrier(1, &outputToSrv);
            g_gpuMeshPreview.outputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        ThrowIfFailed(commandList->Close(), "Close mesh preview CL failed");
        ID3D12CommandList* cls[] = {commandList.Get()};
        g_commandQueue->ExecuteCommandLists(1, cls);
        const UINT64 fenceVal = ++g_fenceLastSignaledValue;
        ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceVal), "Signal mesh preview failed");
        WaitForFenceValue(fenceVal);

        g_gpuMeshPreview.renderStats = renderStats;
        g_gpuMeshPreview.yaw           = g_viewport.yaw;
        g_gpuMeshPreview.pitch         = g_viewport.pitch;
        g_gpuMeshPreview.fovDegrees    = g_viewport.fovDegrees;
        g_gpuMeshPreview.orbitDistance = g_viewport.orbitDistance;
        g_gpuMeshPreview.pan           = g_viewport.pan;
        g_gpuMeshPreview.showSurface   = showSurface;
        g_gpuMeshPreview.showWireframe = showWireframe;
        g_gpuMeshPreview.showGrid      = showGrid;
        g_gpuMeshPreview.hdrViewportEnabled = g_graph.Settings().preview.hdrViewportEnabled;
        g_gpuMeshPreview.colorFormat = MeshPreviewColorFormat();
        g_gpuMeshPreview.maskPreview   = g_graph.Evaluation().previewShowsMask;
        g_gpuMeshPreview.maskShading   = static_cast<int>(g_graph.Settings().preview.maskShading);
        g_gpuMeshPreview.terrainBoundaryMode = static_cast<int>(terrainBoundaryMode);
        g_gpuMeshPreview.lightingMode  = g_graph.Settings().preview.lightingMode;
        g_gpuMeshPreview.exposureMode = static_cast<int>(g_graph.Settings().preview.exposureMode);
        g_gpuMeshPreview.exposureEv = g_graph.Settings().preview.exposureEv;
        g_gpuMeshPreview.autoExposureBiasEv = g_graph.Settings().preview.autoExposureBiasEv;
        g_gpuMeshPreview.autoExposureMinEv = g_graph.Settings().preview.autoExposureMinEv;
        g_gpuMeshPreview.autoExposureMaxEv = g_graph.Settings().preview.autoExposureMaxEv;
        g_gpuMeshPreview.autoExposureSpeed = g_graph.Settings().preview.autoExposureSpeed;
        g_gpuMeshPreview.colorTemperatureKelvin = g_graph.Settings().preview.colorTemperatureKelvin;
        g_gpuMeshPreview.sunAzimuthDegrees = sunPosition.azimuth;
        g_gpuMeshPreview.sunElevationDegrees = sunPosition.elevation;
        g_gpuMeshPreview.sunIntensity = g_graph.Settings().preview.sunIntensity;
        g_gpuMeshPreview.ambientStrength = g_graph.Settings().preview.ambientStrength;
        g_gpuMeshPreview.shadowStrength = g_graph.Settings().preview.shadowStrength;
        g_gpuMeshPreview.shadowBias = g_graph.Settings().preview.shadowBias;
        g_gpuMeshPreview.pbrAlbedo = g_graph.Settings().preview.pbrAlbedo;
        g_gpuMeshPreview.gridColor = g_graph.Settings().preview.gridColor;
        g_gpuMeshPreview.skyMode = static_cast<int>(g_graph.Settings().sky.mode);
        g_gpuMeshPreview.skyAtmosphereDensity = g_graph.Settings().sky.atmosphereDensity;
        g_gpuMeshPreview.skyMieStrength = g_graph.Settings().sky.mieStrength;
        g_gpuMeshPreview.skyMieEccentricity = g_graph.Settings().sky.mieEccentricity;
        g_gpuMeshPreview.skyGroundAlbedo = g_graph.Settings().sky.groundAlbedo;
        g_gpuMeshPreview.skySunSizeDegrees = g_graph.Settings().sky.sunSizeDegrees;
        g_gpuMeshPreview.skySunGlowStrength = g_graph.Settings().sky.sunGlowStrength;
        g_gpuMeshPreview.cloudsEnabled =
            (g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric && g_graph.Settings().clouds.enabled) ? 1 : 0;
        g_gpuMeshPreview.cloudSeed = g_graph.Settings().clouds.seed;
        g_gpuMeshPreview.cloudCoverage = g_graph.Settings().clouds.coverage;
        g_gpuMeshPreview.cloudDensityMultiplier = g_graph.Settings().clouds.densityMultiplier;
        g_gpuMeshPreview.cloudAltitudeMin = g_graph.Settings().clouds.altitudeMin;
        g_gpuMeshPreview.cloudAltitudeMax = g_graph.Settings().clouds.altitudeMax;
        g_gpuMeshPreview.cloudHorizontalScale = g_graph.Settings().clouds.horizontalScale;
        g_gpuMeshPreview.cloudAbsorption = g_graph.Settings().clouds.absorption;
        g_gpuMeshPreview.cloudColor = g_graph.Settings().clouds.color;
        g_gpuMeshPreview.cloudAnimate = g_graph.Settings().clouds.animate ? 1 : 0;
        g_gpuMeshPreview.cloudLoopPhase = g_graph.Settings().clouds.loopPhase;
        g_gpuMeshPreview.cloudWindDirectionDegrees = g_graph.Settings().clouds.windDirectionDegrees;
        g_gpuMeshPreview.cloudWindSpeed = g_graph.Settings().clouds.windSpeedMetersPerSec;
        g_gpuMeshPreview.cloudQualitySamples = g_graph.Settings().clouds.qualitySamples;
        g_gpuMeshPreview.cloudShadowStrength = g_graph.Settings().clouds.shadowStrength;
        g_gpuMeshPreview.cloudShadowResolution = g_graph.Settings().clouds.shadowResolution;
        g_gpuMeshPreview.cloudShadowSamples = g_graph.Settings().clouds.shadowSamples;
        g_gpuMeshPreview.cloudFieldRadius = g_graph.Settings().clouds.fieldRadius;
        g_gpuMeshPreview.cloudFieldFalloff = g_graph.Settings().clouds.fieldFalloff;
        g_gpuMeshPreview.cloudSelfShadowEnabled = g_graph.Settings().clouds.selfShadowEnabled ? 1 : 0;
        g_gpuMeshPreview.cloudLightSamples = g_graph.Settings().clouds.lightSamples;
        g_gpuMeshPreview.cloudLightStepMeters = g_graph.Settings().clouds.lightStepMeters;
        g_gpuMeshPreview.cloudPhaseEccentricity = g_graph.Settings().clouds.phaseEccentricity;
        g_gpuMeshPreview.cloudShadowAmbientStrength = g_graph.Settings().clouds.shadowAmbientStrength;
        g_gpuMeshPreview.meshBackend = static_cast<int>(g_graph.Settings().preview.meshBackend);
        g_gpuMeshPreview.viewportTessellation = g_graph.Settings().preview.viewportTessellation;
        g_gpuMeshPreview.tessellationMinFactor = g_graph.Settings().preview.tessellationMinFactor;
        g_gpuMeshPreview.tessellationMaxFactor = g_graph.Settings().preview.tessellationMaxFactor;
        g_gpuMeshPreview.tessellationNearDistance = g_graph.Settings().preview.tessellationNearDistance;
        g_gpuMeshPreview.tessellationFarDistance = g_graph.Settings().preview.tessellationFarDistance;
        g_gpuMeshPreview.depthOfFieldEnabled = useDepthOfField && dofReady;
        g_gpuMeshPreview.dofFStop = g_graph.Settings().preview.dofFStop;
        g_gpuMeshPreview.dofFocusDistanceMeters = g_graph.Settings().preview.dofFocusDistanceMeters;
        g_gpuMeshPreview.dofSensorHeightMm = g_graph.Settings().preview.dofSensorHeightMm;
        g_gpuMeshPreview.dofMaxBlurPixels = g_graph.Settings().preview.dofMaxBlurPixels;
        g_gpuMeshPreview.dofApertureShape = g_graph.Settings().preview.dofApertureShape;
        g_gpuMeshPreview.dofApertureBlades = g_graph.Settings().preview.dofApertureBlades;
        g_gpuMeshPreview.dofApertureRotationDegrees = g_graph.Settings().preview.dofApertureRotationDegrees;
        g_gpuMeshPreview.dofHighlightBoost = g_graph.Settings().preview.dofHighlightBoost;
        g_gpuMeshPreview.dofMiniatureEnabled = g_graph.Settings().preview.dofMiniatureEnabled;
        g_gpuMeshPreview.dofMiniatureScale = g_graph.Settings().preview.dofMiniatureScale;
        g_gpuMeshPreview.waterEnabled = previewSettings.waterEnabled;
        g_gpuMeshPreview.waterLevelMeters = previewSettings.waterLevelMeters;
        g_gpuMeshPreview.waterOpacity = previewSettings.waterOpacity;
        g_gpuMeshPreview.waterColor = previewSettings.waterColor;
        g_gpuMeshPreview.waterTerrainSizeMeters = g_graph.Evaluation().previewHeightfield.terrainSizeMeters;
        g_gpuMeshPreview.waterWavesScale = previewSettings.waterWavesScale;
        g_gpuMeshPreview.waterRefractiveIndex = previewSettings.waterRefractiveIndex;
        g_gpuMeshPreview.waterFresnelPower = previewSettings.waterFresnelPower;
        g_gpuMeshPreview.waterRefractionStrength = previewSettings.waterRefractionStrength;
        g_gpuMeshPreview.waterAnimationEnabled = previewSettings.waterAnimationEnabled;
        g_gpuMeshPreview.waterReflectionStrength = previewSettings.waterReflectionStrength;
        g_gpuMeshPreview.waterSsrEnabled = previewSettings.waterSsrEnabled;
        g_gpuMeshPreview.waterHeightfieldVersion = currentVersion;
        g_gpuMeshPreview.colorState    = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

void DrawGpuMeshPreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max,
                        const rock::MeshData& mesh, bool showSurface, bool showWireframe)
{
    const auto previewStart = std::chrono::steady_clock::now();
    std::string error;
    if (!RenderGpuMeshPreview(min, max, showSurface, showWireframe, &error))
    {
        DrawMeshPreview(drawList, min, max, mesh, showSurface, showWireframe);
        const auto previewEnd = std::chrono::steady_clock::now();
        g_frameTiming.gpuPreviewMs += std::chrono::duration<double, std::milli>(previewEnd - previewStart).count();
        return;
    }
    const bool useOutputImage =
        (g_gpuMeshPreview.hdrViewportEnabled || ColorTemperatureAdjusted()) &&
        g_tonemapPipelineReady &&
        g_gpuMeshPreview.outputSrvAllocated &&
        g_gpuMeshPreview.outputState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const bool usePostImage =
        !g_gpuMeshPreview.hdrViewportEnabled &&
        g_gpuMeshPreview.depthOfFieldEnabled &&
        g_dofPipelineReady &&
        g_gpuMeshPreview.postSrvAllocated &&
        g_gpuMeshPreview.postState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_GPU_DESCRIPTOR_HANDLE imageSrv =
        useOutputImage ? g_gpuMeshPreview.outputSrvGpu : (usePostImage ? g_gpuMeshPreview.postSrvGpu : g_gpuMeshPreview.srvGpu);
    if ((useOutputImage || usePostImage || g_gpuMeshPreview.srvAllocated) && g_gpuMeshPreview.colorState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        // Snap the destination rect to integer pixel boundaries so the
        // offscreen texture grid aligns 1:1 with screen pixels. Without
        // this, sub-pixel offsets (e.g. an ImGui window at x=123.5) make
        // the bilinear sampler smear fine 1-px patterns.
        const ImVec2 snappedMin(std::round(min.x), std::round(min.y));
        const ImVec2 snappedMax(std::round(max.x), std::round(max.y));
        drawList->PushClipRect(min, max, true);
        drawList->AddImage(static_cast<ImTextureID>(imageSrv.ptr), snappedMin, snappedMax);
        drawList->PopClipRect();
    }
    const auto previewEnd = std::chrono::steady_clock::now();
    g_frameTiming.gpuPreviewMs += std::chrono::duration<double, std::milli>(previewEnd - previewStart).count();
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
    case 0:
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

const char* ViewportDisplayModeLabel(ViewportDisplayMode mode)
{
    switch (mode)
    {
    case ViewportDisplayMode::Pbr:
        return "PBR";
    case ViewportDisplayMode::Sky:
        return Tr("Sky", "天球");
    case ViewportDisplayMode::Simple:
    default:
        return Tr("Simple", "シンプル");
    }
}

void DrawViewportDisplayMenu(const ImVec2& min)
{
    rock::GraphSettings& settings = g_graph.Settings();
    const ImVec2 buttonPos(min.x + 14.0f, min.y + 12.0f);
    const ImVec2 buttonSize(54.0f, 28.0f);

    ImGui::SetCursorScreenPos(buttonPos);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(8, 10, 10, 176));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(32, 38, 36, 220));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(54, 70, 62, 235));
    ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)));
    if (ImGui::Button(Tr("View", "表示"), buttonSize))
    {
        ImGui::OpenPopup("ViewportDisplayMenu");
    }
    ImGui::PopStyleColor(4);

    ImGui::SetNextWindowPos(ImVec2(buttonPos.x, buttonPos.y + buttonSize.y + 6.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(172.0f, 0.0f), ImGuiCond_Appearing);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    if (ImGui::BeginPopup("ViewportDisplayMenu"))
    {
        ViewportDisplayMode displayMode = CurrentViewportDisplayMode(settings);
        const auto drawSmallToggle = [](const char* id, const char* label, bool* value) -> bool {
            ImGui::PushID(id);
            const float rowHeight = std::max(ImGui::GetTextLineHeight() + 4.0f, 20.0f);
            const float rowWidth = ImGui::GetContentRegionAvail().x;
            const bool pressed = ImGui::Selectable("##toggle_row", false, ImGuiSelectableFlags_NoAutoClosePopups, ImVec2(rowWidth, rowHeight));
            if (pressed)
            {
                *value = !*value;
            }

            const ImVec2 rowMin = ImGui::GetItemRectMin();
            const ImVec2 rowMax = ImGui::GetItemRectMax();
            const float boxSize = 13.0f;
            const ImVec2 boxMin(rowMin.x + 2.0f, rowMin.y + (rowHeight - boxSize) * 0.5f);
            const ImVec2 boxMax(boxMin.x + boxSize, boxMin.y + boxSize);
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const bool hovered = ImGui::IsItemHovered();
            const ImU32 boxFill = hovered ? IM_COL32(44, 50, 50, 230) : IM_COL32(28, 31, 31, 230);
            const ImU32 boxBorder = *value ? IM_COL32(92, 168, 218, 255) : IM_COL32(76, 80, 80, 230);
            drawList->AddRectFilled(boxMin, boxMax, boxFill, 1.0f);
            drawList->AddRect(boxMin, boxMax, boxBorder, 1.0f);
            if (*value)
            {
                const ImU32 checkColor = IM_COL32(91, 177, 232, 255);
                drawList->AddLine(ImVec2(boxMin.x + 3.0f, boxMin.y + 6.5f), ImVec2(boxMin.x + 5.6f, boxMin.y + 9.2f), checkColor, 2.2f);
                drawList->AddLine(ImVec2(boxMin.x + 5.6f, boxMin.y + 9.2f), ImVec2(boxMin.x + 10.4f, boxMin.y + 3.8f), checkColor, 2.2f);
            }
            drawList->AddText(ImVec2(boxMax.x + 9.0f, rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f),
                              ImGui::GetColorU32(ImGuiCol_Text), label);
            ImGui::PopID();
            return pressed;
        };

        if (drawSmallToggle("ViewportFpsToggle", Tr("Show FPS", "FPSを表示"), &g_ui.showFps))
        {
            SaveAppSettingsSilently();
        }
        if (drawSmallToggle("ViewportGridToggle", Tr("Show Grid", "グリッドを表示"), &settings.preview.showGrid))
        {
            SaveAppSettingsSilently();
        }
        ImGui::Separator();
        ImGui::TextUnformatted(Tr("Display Mode", "表示モード"));
        ImGui::Separator();
        const auto drawModeItem = [&](const char* label, ViewportDisplayMode mode) {
            const bool selected = displayMode == mode;
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_NoAutoClosePopups))
            {
                displayMode = mode;
                ApplyViewportDisplayMode(settings, mode);
                SaveAppSettingsSilently();
            }
        };
        drawModeItem(ViewportDisplayModeLabel(ViewportDisplayMode::Simple), ViewportDisplayMode::Simple);
        drawModeItem(ViewportDisplayModeLabel(ViewportDisplayMode::Pbr), ViewportDisplayMode::Pbr);
        drawModeItem(ViewportDisplayModeLabel(ViewportDisplayMode::Sky), ViewportDisplayMode::Sky);

        if (displayMode == ViewportDisplayMode::Sky)
        {
            ImGui::Spacing();
            if (drawSmallToggle("ViewportCloudToggle", Tr("Draw Clouds", "雲を描画"), &settings.clouds.enabled))
            {
                SaveAppSettingsSilently();
            }
            if (settings.clouds.enabled && drawSmallToggle("ViewportCloudAnimateToggle", Tr("Animate Clouds", "雲を動かす"), &settings.clouds.animate))
            {
                SaveAppSettingsSilently();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
}

void DrawViewportCube(const ImVec2& min, const ImVec2& max, float timeSeconds)
{
    (void)timeSeconds;
    UpdateViewportInteraction(min, max);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const std::array<float, 3>& viewportBackground = g_graph.Settings().preview.viewportBackground;
    drawList->AddRectFilled(min, max, ColorToU32(ImVec4(viewportBackground[0], viewportBackground[1], viewportBackground[2], 1.0f)));
    const rock::PreviewSettings& preview = g_graph.Settings().preview;
    const bool drawMeshSurface = g_ui.meshPreview;
    if (drawMeshSurface)
    {
        DrawGpuMeshPreview(drawList, min, max, g_graph.Evaluation().previewMesh,
                           drawMeshSurface && preview.showSurface,
                           drawMeshSurface && preview.showWireframe);
    }
    DrawPathViewportOverlay(drawList, min, max);
    DrawSunDirectionGizmo(drawList, min, max);
    DrawFocusPickOverlay(drawList, min, max);
    DrawViewportDisplayMenu(min);
    float overlayTop = min.y + 14.0f;
    const ImU32 overlayBgColor = IM_COL32(12, 12, 13, 176);
    const ImU32 overlayBorderColor = IM_COL32(74, 74, 76, 190);
    const ImU32 overlayLabelColor = IM_COL32(222, 222, 220, 255);
    const ImU32 overlayValueColor = IM_COL32(196, 196, 194, 255);
    if (g_ui.showFps)
    {
        char fpsText[32]{};
        const double measuredFrameMs = g_lastFrameTiming.frameMs;
        const double measuredFps = measuredFrameMs > 0.0001 ? 1000.0 / measuredFrameMs : 0.0;
        std::snprintf(fpsText, sizeof(fpsText), "FPS %.1f", measuredFps);
        const ImVec2 fpsSize = ImGui::CalcTextSize(fpsText);
        const ImVec2 fpsPadding(9.0f, 5.0f);
        const ImVec2 fpsMax(max.x - 14.0f, overlayTop + fpsSize.y + fpsPadding.y * 2.0f);
        const ImVec2 fpsMin(fpsMax.x - fpsSize.x - fpsPadding.x * 2.0f, overlayTop);
        drawList->AddRectFilled(fpsMin, fpsMax, overlayBgColor, 4.0f);
        drawList->AddRect(fpsMin, fpsMax, overlayBorderColor, 4.0f);
        drawList->AddText(ImVec2(fpsMin.x + fpsPadding.x, fpsMin.y + fpsPadding.y), overlayLabelColor, fpsText);
        overlayTop = fpsMax.y + 6.0f;
    }
    if (g_ui.showFrameStats)
    {
        const auto formatMs = [](double value) {
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%.2f ms", value);
            return std::string(buffer);
        };

        struct OverlayStatLine
        {
            const char* label;
            std::string value;
        };
        std::vector<OverlayStatLine> lines = {
            {"Frame:", formatMs(g_lastFrameTiming.frameMs)},
            {"Message Pump:", formatMs(g_lastFrameTiming.messagePumpMs)},
            {"NewFrame:", formatMs(g_lastFrameTiming.newFrameMs)},
            {"Main Thread Work:", formatMs(g_lastFrameTiming.mainThreadWorkMs)},
            {"DrawUi:", formatMs(g_lastFrameTiming.drawUiMs)},
            {"Viewport Tabs:", formatMs(g_lastFrameTiming.viewportTabsMs)},
            {"Node Editor:", formatMs(g_lastFrameTiming.nodeEditorMs)},
            {"  Dots:", formatMs(g_lastFrameTiming.nodeEditorDotsMs)},
            {"  Shadows:", formatMs(g_lastFrameTiming.nodeEditorShadowsMs)},
            {"  Nodes:", formatMs(g_lastFrameTiming.nodeEditorNodesMs)},
            {"  Links:", formatMs(g_lastFrameTiming.nodeEditorLinksMs)},
            {"  Interaction:", formatMs(g_lastFrameTiming.nodeEditorInteractionMs)},
            {"  Positions:", formatMs(g_lastFrameTiming.nodeEditorPositionMs)},
            {"  Count:", std::format("{} nodes / {} links", g_lastFrameTiming.nodeCount, g_lastFrameTiming.linkCount)},
            {"Inspector:", formatMs(g_lastFrameTiming.inspectorMs)},
            {"Status Bar:", formatMs(g_lastFrameTiming.statusBarMs)},
            {"GPU Preview:", formatMs(g_lastFrameTiming.gpuPreviewMs)},
            {"GPU Preview Reason:", g_lastFrameTiming.gpuPreviewReason.empty() ? "-" : g_lastFrameTiming.gpuPreviewReason},
            {"ImGui Render:", formatMs(g_lastFrameTiming.imguiRenderMs)},
            {"RenderFrame:", formatMs(g_lastFrameTiming.renderFrameMs)},
            {"Present:", formatMs(g_lastFrameTiming.presentMs)},
            {"Frame Limit Sleep:", formatMs(g_lastFrameTiming.frameLimitSleepMs)},
            {"Background Sleep:", formatMs(g_lastFrameTiming.backgroundSleepMs)},
            {"Fence Wait:", formatMs(g_lastFrameTiming.fenceWaitMs)},
            {"FPS Limit:", g_lastFrameTiming.frameRateLimitFps > 0 ? std::format("{} FPS", g_lastFrameTiming.frameRateLimitFps) : "Unlimited"},
        };

        float labelWidth = 0.0f;
        float valueWidth = 0.0f;
        for (const OverlayStatLine& line : lines)
        {
            labelWidth = std::max(labelWidth, ImGui::CalcTextSize(line.label).x);
            valueWidth = std::max(valueWidth, ImGui::CalcTextSize(line.value.c_str()).x);
        }
        const float lineHeight = ImGui::GetTextLineHeight() + 2.0f;
        const ImVec2 padding(10.0f, 7.0f);
        const float gap = 14.0f;
        const float maxOverlayWidth = std::max(220.0f, (max.x - min.x) - 28.0f);
        const ImVec2 statsSize(std::min(maxOverlayWidth, labelWidth + gap + valueWidth + padding.x * 2.0f),
                               lineHeight * static_cast<float>(lines.size()) + padding.y * 2.0f);
        const ImVec2 statsMax(max.x - 14.0f, overlayTop + statsSize.y);
        const ImVec2 statsMin(statsMax.x - statsSize.x, overlayTop);
        drawList->AddRectFilled(statsMin, statsMax, overlayBgColor, 4.0f);
        drawList->AddRect(statsMin, statsMax, overlayBorderColor, 4.0f);
        const float valueX = statsMin.x + padding.x + labelWidth + gap;
        const float valueClipMaxX = statsMax.x - padding.x;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            const OverlayStatLine& line = lines[i];
            const float y = statsMin.y + padding.y + lineHeight * static_cast<float>(i);
            drawList->AddText(ImVec2(statsMin.x + padding.x, y), overlayLabelColor, line.label);
            drawList->PushClipRect(ImVec2(valueX, statsMin.y), ImVec2(valueClipMaxX, statsMax.y), true);
            drawList->AddText(ImVec2(valueX, y), overlayValueColor, line.value.c_str());
            drawList->PopClipRect();
        }
        overlayTop = statsMax.y + 6.0f;
    }
    if (g_ui.showDrawStats)
    {
        const PreviewRenderStats& stats = g_gpuMeshPreview.renderStats;
        const std::string statsText = std::format(
            "Draw Calls {}\nVerts {}  Tris {}\nRT {} x {}",
            stats.drawCalls,
            stats.submittedVertices,
            stats.submittedTriangles,
            stats.renderTargetWidth,
            stats.renderTargetHeight);
        const ImVec2 statsSize = ImGui::CalcTextSize(statsText.c_str());
        const ImVec2 statsPadding(9.0f, 6.0f);
        const ImVec2 statsMax(max.x - 14.0f, overlayTop + statsSize.y + statsPadding.y * 2.0f);
        const ImVec2 statsMin(statsMax.x - statsSize.x - statsPadding.x * 2.0f, overlayTop);
        drawList->AddRectFilled(statsMin, statsMax, overlayBgColor, 4.0f);
        drawList->AddRect(statsMin, statsMax, overlayBorderColor, 4.0f);
        drawList->AddText(ImVec2(statsMin.x + statsPadding.x, statsMin.y + statsPadding.y), overlayLabelColor, statsText.c_str());
        overlayTop = statsMax.y + 6.0f;
    }
    DrawViewportAxisGizmo(drawList, min, max);
}

ImU32 MapPreviewColor(float value, bool mask, rock::MaskShadingMode mode, int cellX, int cellZ)
{
    value = std::clamp(value, 0.0f, 1.0f);
    if (mask && mode == rock::MaskShadingMode::GrayOrange)
    {
        const int r = static_cast<int>(35.0f + value * 220.0f);
        const int g = static_cast<int>(42.0f + value * 122.0f);
        const int b = static_cast<int>(44.0f + value * 24.0f);
        return IM_COL32(r, g, b, 255);
    }

    if (mask && mode == rock::MaskShadingMode::GrayscaleHatched)
    {
        // 飽和域のみ均等な 3:1 対角ハッチで描画 (GeoGen 風)。
        // 背景は純白 / 純黒、4 セル中 1 つだけグレー (= 斜線) で
        // コントラストを抑える。
        //   value >= 0.99 → 白×3 + グレー×1
        //   value <= 0.01 → 黒×3 + グレー×1
        //   中間域       → 通常のグレースケールランプ
        if (value >= 0.99f || value <= 0.01f)
        {
            const int phase = ((cellX + cellZ) % 4 + 4) % 4;
            const bool isMinor = (phase == 3);
            const int majorVal = (value >= 0.99f) ? 255 : 0;
            const int stripeGray = 128;
            const int c = isMinor ? stripeGray : majorVal;
            return IM_COL32(c, c, c, 255);
        }
    }

    const int c = static_cast<int>(28.0f + value * 214.0f);
    return IM_COL32(c, c, c, 255);
}

ImVec2 PathWorldToMapScreen(float x, float z, const ImVec2& mapMin, const ImVec2& mapMax)
{
    const float terrainSize = std::max(1.0f, g_graph.Settings().preview.terrainSizeMeters);
    const float halfSize = terrainSize * 0.5f;
    const float u = std::clamp((x + halfSize) / terrainSize, 0.0f, 1.0f);
    const float v = std::clamp((halfSize - z) / terrainSize, 0.0f, 1.0f);
    return ImVec2(std::lerp(mapMin.x, mapMax.x, u), std::lerp(mapMin.y, mapMax.y, v));
}

bool MapScreenToPathWorld(const ImVec2& screen, const ImVec2& mapMin, const ImVec2& mapMax, float* outX, float* outZ)
{
    if (screen.x < mapMin.x || screen.x > mapMax.x || screen.y < mapMin.y || screen.y > mapMax.y)
    {
        return false;
    }
    const float terrainSize = std::max(1.0f, g_graph.Settings().preview.terrainSizeMeters);
    const float halfSize = terrainSize * 0.5f;
    const float u = (screen.x - mapMin.x) / std::max(1.0f, mapMax.x - mapMin.x);
    const float v = (screen.y - mapMin.y) / std::max(1.0f, mapMax.y - mapMin.y);
    *outX = u * terrainSize - halfSize;
    *outZ = halfSize - v * terrainSize;
    return true;
}

void DrawPathMapOverlay(ImDrawList* drawList, const ImVec2& mapMin, const ImVec2& mapMax)
{
    const rock::Node* node = SelectedPathNode();
    if (node == nullptr)
    {
        return;
    }

    const ImU32 edgeColor = IM_COL32(90, 182, 255, 235);
    const ImU32 selectedEdgeColor = IM_COL32(255, 210, 92, 255);
    const ImU32 pointColor = IM_COL32(250, 247, 224, 255);
    const ImU32 selectedPointColor = IM_COL32(255, 210, 92, 255);
    const ImU32 pointBorderColor = IM_COL32(31, 35, 38, 255);
    drawList->PushClipRect(mapMin, mapMax, true);
    for (const rock::PathEdge& edge : node->path.edges)
    {
        if (!edge.enabled)
        {
            continue;
        }
        Vec3 prev;
        if (!PathEdgePositionAt(node->path, edge, 0.0f, &prev))
        {
            continue;
        }
        const bool selected = g_pathSelectionKind == PathSelectionKind::Edge && g_pathSelectedElementId == edge.id;
        ImVec2 prevScreen = PathWorldToMapScreen(prev.x, prev.z, mapMin, mapMax);
        const int steps = PathEdgeDisplayStepCount(edge);
        for (int step = 1; step <= steps; ++step)
        {
            Vec3 next;
            if (!PathEdgePositionAt(node->path, edge, static_cast<float>(step) / static_cast<float>(steps), &next))
            {
                break;
            }
            const ImVec2 nextScreen = PathWorldToMapScreen(next.x, next.z, mapMin, mapMax);
            drawList->AddLine(prevScreen, nextScreen, selected ? selectedEdgeColor : edgeColor, selected ? 4.0f : 2.5f);
            prevScreen = nextScreen;
        }
    }
    for (const rock::PathPoint& point : node->path.points)
    {
        const ImVec2 p = PathWorldToMapScreen(point.x, point.z, mapMin, mapMax);
        const bool selected = g_pathSelectionKind == PathSelectionKind::Point && g_pathSelectedElementId == point.id;
        drawList->AddCircleFilled(p, selected ? 6.5f : 5.0f, selected ? selectedPointColor : pointColor, 16);
        drawList->AddCircle(p, selected ? 6.5f : 5.0f, pointBorderColor, 16, 1.5f);
    }
    drawList->PopClipRect();
    drawList->AddText(ImVec2(mapMin.x, mapMax.y + 8.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), "Path mode: click/select, Ctrl+edge inserts, Del deletes, Enter finishes");
}

PathHitResult HitTestPathMap(const rock::Node& node, const ImVec2& mapMin, const ImVec2& mapMax, const ImVec2& mouse)
{
    constexpr float kPointHitRadius = 8.0f;
    constexpr float kEdgeHitRadius = 7.0f;
    PathHitResult result;
    float bestPointDistanceSq = kPointHitRadius * kPointHitRadius;
    float bestEdgeDistanceSq = kEdgeHitRadius * kEdgeHitRadius;

    for (const rock::PathPoint& point : node.path.points)
    {
        const ImVec2 p = PathWorldToMapScreen(point.x, point.z, mapMin, mapMax);
        const float distanceSq = DistanceSquared(mouse, p);
        if (distanceSq <= bestPointDistanceSq)
        {
            bestPointDistanceSq = distanceSq;
            result.kind = PathSelectionKind::Point;
            result.elementId = point.id;
            result.t = 0.0f;
        }
    }
    if (result.kind == PathSelectionKind::Point)
    {
        return result;
    }

    for (const rock::PathEdge& edge : node.path.edges)
    {
        if (!edge.enabled)
        {
            continue;
        }
        Vec3 prev;
        if (!PathEdgePositionAt(node.path, edge, 0.0f, &prev))
        {
            continue;
        }
        ImVec2 prevScreen = PathWorldToMapScreen(prev.x, prev.z, mapMin, mapMax);
        const int steps = PathEdgeDisplayStepCount(edge);
        for (int step = 1; step <= steps; ++step)
        {
            const float edgeT0 = static_cast<float>(step - 1) / static_cast<float>(steps);
            const float edgeT1 = static_cast<float>(step) / static_cast<float>(steps);
            Vec3 next;
            if (!PathEdgePositionAt(node.path, edge, edgeT1, &next))
            {
                break;
            }
            const ImVec2 nextScreen = PathWorldToMapScreen(next.x, next.z, mapMin, mapMax);
            float segmentT = 0.0f;
            const float distanceSq = DistanceToSegmentSquared(mouse, prevScreen, nextScreen, &segmentT);
            if (distanceSq <= bestEdgeDistanceSq)
            {
                bestEdgeDistanceSq = distanceSq;
                result.kind = PathSelectionKind::Edge;
                result.elementId = edge.id;
                result.t = std::lerp(edgeT0, edgeT1, segmentT);
            }
            prevScreen = nextScreen;
        }
    }
    return result;
}

void HandlePathMapInput(const ImVec2& mapMin, const ImVec2& mapMax)
{
    const rock::Node* pathNode = SelectedPathNode();
    if (pathNode == nullptr)
    {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsMouseHoveringRect(mapMin, mapMax);
    if (hovered && !io.WantTextInput && FinishPathSegmentFromShortcut())
    {
        return;
    }
    if (hovered && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete))
    {
        DeleteSelectedPathElement();
        return;
    }
    if (io.WantTextInput || !ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.KeyShift || io.KeyAlt)
    {
        return;
    }
    if (hovered)
    {
        const PathHitResult hit = HitTestPathMap(*pathNode, mapMin, mapMax, io.MousePos);
        if (!io.KeyCtrl && hit.kind == PathSelectionKind::Point)
        {
            SelectPathPoint(hit.elementId);
            return;
        }
        if (hit.kind == PathSelectionKind::Edge)
        {
            if (io.KeyCtrl)
            {
                rock::Node* mutablePathNode = SelectedMutablePathNode();
                if (mutablePathNode != nullptr)
                {
                    InsertPathPointOnEdge(*mutablePathNode, hit.elementId, hit.t);
                }
                return;
            }
            SelectPathEdge(hit.elementId);
            return;
        }
    }

    if (io.KeyCtrl)
    {
        return;
    }

    float x = 0.0f;
    float z = 0.0f;
    if (MapScreenToPathWorld(io.MousePos, mapMin, mapMax, &x, &z))
    {
        AddPathPointFromViewport(x, z, 0.0f);
    }
}

void DrawHeightfieldMapPreview(const ImVec2& min, const ImVec2& max)
{
    constexpr int kMaxMapPreviewSamples = 1024;
    UpdateMapViewportInteraction(min, max);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const std::array<float, 3>& viewportBackground = g_graph.Settings().preview.viewportBackground;
    drawList->AddRectFilled(min, max, ColorToU32(ImVec4(viewportBackground[0], viewportBackground[1], viewportBackground[2], 1.0f)));

    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    const rock::HeightfieldGrid& grid = evaluation.previewHeightfield;
    const int gridResolution = grid.resolution;
    const size_t cellCount = static_cast<size_t>(gridResolution) * static_cast<size_t>(gridResolution);
    const bool canDrawMap = gridResolution >= 2 &&
        grid.heights.size() >= cellCount;
    const bool maskPreview = evaluation.previewShowsMask;
    const auto heightfieldFieldName = [](rock::HeightfieldPreviewField field) {
        switch (field)
        {
        case rock::HeightfieldPreviewField::Deposits:
            return "Deposits";
        case rock::HeightfieldPreviewField::Flows:
            return "Flows";
        case rock::HeightfieldPreviewField::Age:
            return "Age";
        case rock::HeightfieldPreviewField::Mask:
            return "Mask";
        case rock::HeightfieldPreviewField::Heightmap:
        default:
            return "Heightmap";
        }
    };
    const bool colorPreview = evaluation.previewIsColor;
    const std::string title = colorPreview
        ? "2D View: Color Texture"
        : (maskPreview
            ? "2D View: " + std::string(heightfieldFieldName(evaluation.previewField))
            : "2D View: Heightmap");
    drawList->AddText(ImVec2(min.x + 16.0f, min.y + 14.0f), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), title.c_str());

    // Color texture preview: RGBA 直接描画
    if (colorPreview)
    {
        const rock::ColorGrid& cg = evaluation.previewColorGrid;
        if (cg.resolution < 2 || static_cast<int>(cg.pixels.size()) < cg.resolution * cg.resolution * 4)
        {
            drawList->AddText(ImVec2(min.x + 16.0f, min.y + 42.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), Tr("Connect a Gradient Mask.", "Gradient Mask を接続してください。"));
            return;
        }
        const float availableWidth = std::max(1.0f, max.x - min.x - 32.0f);
        const float availableHeight = std::max(1.0f, max.y - min.y - 76.0f);
        const float mapSize = std::max(1.0f, std::min(availableWidth, availableHeight)) * std::clamp(g_mapViewport.zoom, 0.05f, 64.0f);
        const ImVec2 mapMin(
            min.x + 16.0f + (std::max(1.0f, std::min(availableWidth, availableHeight)) - mapSize) * 0.5f + g_mapViewport.pan.x,
            min.y + 52.0f + (std::max(1.0f, std::min(availableWidth, availableHeight)) - mapSize) * 0.5f + g_mapViewport.pan.y);
        const ImVec2 mapMax(mapMin.x + mapSize, mapMin.y + mapSize);
        drawList->PushClipRect(ImVec2(min.x + 1.0f, min.y + 42.0f), ImVec2(max.x - 1.0f, max.y - 1.0f), true);
        drawList->AddRectFilled(mapMin, mapMax, IM_COL32(18, 20, 20, 255));
        const int res = cg.resolution;
        const int maxVisibleSamples = std::clamp(static_cast<int>(std::ceil(mapSize)), 2, kMaxMapPreviewSamples);
        const int samples = std::clamp(std::min(res, maxVisibleSamples), 2, res);
        const float cellSize = mapSize / static_cast<float>(samples);
        for (int z = 0; z < samples; ++z)
        {
            const int srcZ = res - 1 - (samples > 1 ? static_cast<int>(std::lround(static_cast<float>(z) * static_cast<float>(res - 1) / static_cast<float>(samples - 1))) : 0);
            for (int x = 0; x < samples; ++x)
            {
                const int srcX = samples > 1 ? static_cast<int>(std::lround(static_cast<float>(x) * static_cast<float>(res - 1) / static_cast<float>(samples - 1))) : 0;
                const size_t idx = (static_cast<size_t>(srcZ) * static_cast<size_t>(res) + static_cast<size_t>(srcX)) * 4;
                const ImVec2 cellMin(mapMin.x + static_cast<float>(x) * cellSize, mapMin.y + static_cast<float>(z) * cellSize);
                const ImVec2 cellMax(mapMin.x + static_cast<float>(x + 1) * cellSize + 0.5f, mapMin.y + static_cast<float>(z + 1) * cellSize + 0.5f);
                drawList->AddRectFilled(cellMin, cellMax, IM_COL32(cg.pixels[idx], cg.pixels[idx+1], cg.pixels[idx+2], 255));
            }
        }
        drawList->AddRect(mapMin, mapMax, ThemeColor("border", ImVec4(0.20f, 0.23f, 0.22f, 0.85f)));
        drawList->PopClipRect();
        DrawPathMapOverlay(drawList, mapMin, mapMax);
        HandlePathMapInput(mapMin, mapMax);
        char info[128]{};
        const bool downsampled = samples != res;
        if (downsampled)
        {
            std::snprintf(info, sizeof(info), "%d x %d texture / %d x %d drawn / zoom %.2fx", res, res, samples, samples, g_mapViewport.zoom);
        }
        else
        {
            std::snprintf(info, sizeof(info), "%d x %d texture / zoom %.2fx", res, res, g_mapViewport.zoom);
        }
        drawList->AddText(ImVec2(min.x + 16.0f, max.y - 28.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), info);
        return;
    }

    if (!canDrawMap)
    {
        drawList->AddText(ImVec2(min.x + 16.0f, min.y + 42.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), "Select a heightmap or mask output to inspect it as a 2D map.");
        return;
    }

    const std::vector<float>& mapValues = maskPreview && grid.mask.size() >= cellCount
        ? grid.mask
        : grid.heights;
    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();
    for (const float height : grid.heights)
    {
        minHeight = std::min(minHeight, height);
        maxHeight = std::max(maxHeight, height);
    }
    const float heightRange = std::max(0.0001f, maxHeight - minHeight);

    const float availableWidth = std::max(1.0f, max.x - min.x - 32.0f);
    const float availableHeight = std::max(1.0f, max.y - min.y - 76.0f);
    const float mapSize = std::max(1.0f, std::min(availableWidth, availableHeight)) * std::clamp(g_mapViewport.zoom, 0.05f, 64.0f);
    const ImVec2 mapMin(
        min.x + 16.0f + (std::max(1.0f, std::min(availableWidth, availableHeight)) - mapSize) * 0.5f + g_mapViewport.pan.x,
        min.y + 52.0f + (std::max(1.0f, std::min(availableWidth, availableHeight)) - mapSize) * 0.5f + g_mapViewport.pan.y);
    const ImVec2 mapMax(mapMin.x + mapSize, mapMin.y + mapSize);
    drawList->PushClipRect(ImVec2(min.x + 1.0f, min.y + 42.0f), ImVec2(max.x - 1.0f, max.y - 1.0f), true);
    drawList->AddRectFilled(mapMin, mapMax, IM_COL32(18, 20, 20, 255));

    const int maxVisibleSamples = std::clamp(static_cast<int>(std::ceil(mapSize)), 2, kMaxMapPreviewSamples);
    const int samples = std::clamp(std::min(gridResolution, maxVisibleSamples), 2, gridResolution);
    const float cellSize = mapSize / static_cast<float>(samples);
    for (int z = 0; z < samples; ++z)
    {
        const int sampleZ = samples > 1 ? static_cast<int>(std::lround(static_cast<float>(z) * static_cast<float>(gridResolution - 1) / static_cast<float>(samples - 1))) : 0;
        const int srcZ = gridResolution - 1 - sampleZ;
        for (int x = 0; x < samples; ++x)
        {
            const int srcX = samples > 1 ? static_cast<int>(std::lround(static_cast<float>(x) * static_cast<float>(gridResolution - 1) / static_cast<float>(samples - 1))) : 0;
            const float sourceValue = mapValues[static_cast<size_t>(srcZ * gridResolution + srcX)];
            const float value = maskPreview ? sourceValue : (sourceValue - minHeight) / heightRange;
            const ImVec2 cellMin(mapMin.x + static_cast<float>(x) * cellSize, mapMin.y + static_cast<float>(z) * cellSize);
            const ImVec2 cellMax(mapMin.x + static_cast<float>(x + 1) * cellSize + 0.5f, mapMin.y + static_cast<float>(z + 1) * cellSize + 0.5f);
            drawList->AddRectFilled(cellMin, cellMax, MapPreviewColor(value, maskPreview, g_graph.Settings().preview.maskShading, x, z));
        }
    }
    drawList->AddRect(mapMin, mapMax, ThemeColor("border", ImVec4(0.20f, 0.23f, 0.22f, 0.85f)));
    drawList->PopClipRect();
    DrawPathMapOverlay(drawList, mapMin, mapMax);
    HandlePathMapInput(mapMin, mapMax);

    char info[192]{};
    const bool downsampled = samples != gridResolution;
    if (maskPreview)
    {
        if (downsampled)
        {
            std::snprintf(info, sizeof(info), "%d x %d simulation / %d x %d drawn / zoom %.2fx", gridResolution, gridResolution, samples, samples, g_mapViewport.zoom);
        }
        else
        {
            std::snprintf(info, sizeof(info), "%d x %d simulation / zoom %.2fx", gridResolution, gridResolution, g_mapViewport.zoom);
        }
    }
    else
    {
        if (downsampled)
        {
            std::snprintf(info, sizeof(info), "%d x %d simulation / %d x %d drawn / zoom %.2fx / height %.2f m to %.2f m", gridResolution, gridResolution, samples, samples, g_mapViewport.zoom, minHeight, maxHeight);
        }
        else
        {
            std::snprintf(info, sizeof(info), "%d x %d simulation / zoom %.2fx / height %.2f m to %.2f m", gridResolution, gridResolution, g_mapViewport.zoom, minHeight, maxHeight);
        }
    }
    drawList->AddText(ImVec2(min.x + 16.0f, max.y - 28.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), info);
}

ImVec4 NodeAccentColor(rock::NodeKind kind)
{
    const ImVec4 heightfieldGreen(0.42f, 0.70f, 0.50f, 1.0f);
    const ImVec4 maskOrange(0.92f, 0.56f, 0.24f, 1.0f);
    switch (rock::CategoryFor(kind))
    {
    case rock::NodeCategory::Heightfield:
        return heightfieldGreen;
    case rock::NodeCategory::Mask:
        return maskOrange;
    case rock::NodeCategory::Color:
        return ImVec4(0.44f, 0.50f, 0.96f, 1.0f); // 青紫 (カラー系)
    case rock::NodeCategory::Path:
        return ImVec4(0.42f, 0.78f, 0.92f, 1.0f);
    default:
        return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    }
}

ImVec2 InitialNodePosition(rock::NodeKind kind)
{
    switch (kind)
    {
    case rock::NodeKind::HeightmapLoad:
        return ImVec2(40.0f, 240.0f);
    case rock::NodeKind::Shape:
        return ImVec2(40.0f, 360.0f);
    case rock::NodeKind::HeightmapFromMask:
        return ImVec2(320.0f, 800.0f);
    case rock::NodeKind::HeightmapBlur:
        return ImVec2(600.0f, 240.0f);
    case rock::NodeKind::MultiScaleErosion:
        return ImVec2(320.0f, 380.0f);
    case rock::NodeKind::FluvialErosion:
        return ImVec2(320.0f, 480.0f);
    case rock::NodeKind::DropletErosion:
        return ImVec2(320.0f, 580.0f);
    case rock::NodeKind::MaskNoise:
        return ImVec2(40.0f, 520.0f);
    case rock::NodeKind::MaskBlend:
        return ImVec2(320.0f, 520.0f);
    case rock::NodeKind::MaskLevels:
        return ImVec2(600.0f, 520.0f);
    case rock::NodeKind::MaskHeight:
        return ImVec2(600.0f, 590.0f);
    case rock::NodeKind::MaskSlope:
        return ImVec2(600.0f, 660.0f);
    case rock::NodeKind::MaskCurvature:
        return ImVec2(600.0f, 800.0f);
    case rock::NodeKind::MaskFluvial:
        return ImVec2(880.0f, 240.0f);
    case rock::NodeKind::MaskPath:
        return ImVec2(320.0f, 720.0f);
    case rock::NodeKind::MaskBlur:
        return ImVec2(600.0f, 720.0f);
    case rock::NodeKind::Crumbling:
        return ImVec2(880.0f, 380.0f);
    case rock::NodeKind::Rock:
        return ImVec2(880.0f, 450.0f);
    case rock::NodeKind::Scatter:
        return ImVec2(880.0f, 590.0f);
    case rock::NodeKind::Sediment:
        return ImVec2(880.0f, 520.0f);
    case rock::NodeKind::Snow:
        return ImVec2(880.0f, 660.0f);
    case rock::NodeKind::Soil:
        return ImVec2(880.0f, 730.0f);
    case rock::NodeKind::Colorize:
        return ImVec2(1160.0f, 380.0f);
    case rock::NodeKind::Path:
        return ImVec2(40.0f, 720.0f);
    default:
        return ImVec2(40.0f, 64.0f);
    }
}

int ToGraphId(uintptr_t id)
{
    return static_cast<int>(id);
}

void EvaluateWhenParameterEditEnds()
{
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        EvaluateGraph();
    }
}

ImU32 ColorToU32(const ImVec4& color)
{
    return ImGui::ColorConvertFloat4ToU32(color);
}

ImU32 ThemeColor(const std::string& name, const ImVec4& fallback)
{
    return ColorToU32(g_themeManager.AppColor(name, fallback));
}

void ResetMapViewport()
{
    g_mapViewport.zoom = 1.0f;
    g_mapViewport.pan = ImVec2(0.0f, 0.0f);
}

void UpdateMapViewportInteraction(const ImVec2& min, const ImVec2& max)
{
    ImGuiIO& io = ImGui::GetIO();
    if (g_layoutSplitterActive)
    {
        return;
    }

    const bool hovered = ImGui::IsMouseHoveringRect(min, max);
    if (!hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        return;
    }

    const bool altNavigation = io.KeyAlt;
    if (altNavigation && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        ResetMapViewport();
        SaveAppSettingsSilently();
        return;
    }

    bool changed = false;
    if (altNavigation && hovered && io.MouseWheel != 0.0f)
    {
        const float oldZoom = g_mapViewport.zoom;
        const ImVec2 mouse = io.MousePos;
        const ImVec2 center((min.x + max.x) * 0.5f + g_mapViewport.pan.x, (min.y + max.y) * 0.5f + g_mapViewport.pan.y);
        g_mapViewport.zoom *= std::pow(1.12f, io.MouseWheel);
        g_mapViewport.zoom = std::clamp(g_mapViewport.zoom, 0.05f, 64.0f);
        const float zoomRatio = oldZoom > 0.0001f ? g_mapViewport.zoom / oldZoom : 1.0f;
        g_mapViewport.pan.x += (center.x - mouse.x) * (zoomRatio - 1.0f);
        g_mapViewport.pan.y += (center.y - mouse.y) * (zoomRatio - 1.0f);
        changed = true;
    }

    if (altNavigation && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) && hovered)
    {
        g_mapViewport.pan.x += io.MouseDelta.x;
        g_mapViewport.pan.y += io.MouseDelta.y;
        changed = true;
    }

    if (changed && (ImGui::IsMouseReleased(ImGuiMouseButton_Left) || ImGui::IsMouseReleased(ImGuiMouseButton_Right) || ImGui::IsMouseReleased(ImGuiMouseButton_Middle) || io.MouseWheel != 0.0f))
    {
        SaveAppSettingsSilently();
    }
}

ImVec4 LinkColor(const rock::Link& link)
{
    if (const rock::Pin* startPin = g_graph.FindPin(link.startPin))
    {
        return PinTypeColor(startPin->valueType);
    }
    if (const rock::Pin* endPin = g_graph.FindPin(link.endPin))
    {
        return PinTypeColor(endPin->valueType);
    }
    return ImVec4(0.52f, 0.70f, 0.59f, 1.0f);
}

ImVec4 LinkPreviewColor(rock::GraphId startPinId, rock::GraphId endPinId)
{
    if (const rock::Pin* startPin = g_graph.FindPin(startPinId))
    {
        return PinTypeColor(startPin->valueType);
    }
    if (const rock::Pin* endPin = g_graph.FindPin(endPinId))
    {
        return PinTypeColor(endPin->valueType);
    }
    return ImVec4(0.52f, 0.70f, 0.59f, 1.0f);
}

enum class NodeEvaluationVisualState
{
    None,
    Processing,
    Pending,
};

NodeEvaluationVisualState GetNodeEvaluationVisualState(const rock::Node& node)
{
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    const rock::GraphId currentlyEvaluating =
        rock::CurrentlyEvaluatingNodeId().load(std::memory_order_relaxed);

    // "Processing" follows the worker thread — it walks the upstream chain in
    // real time. Until the first kernel stores its id (or if every step
    // is a cache hit), fall back to the preview target so the user sees
    // *something* during in-flight evaluation. "Pending" only shows on
    // the preview target when an evaluation is queued behind another.
    const bool isCurrent = g_evaluationInFlight && currentlyEvaluating == node.id;
    const bool isPreviewFallback = g_evaluationInFlight
        && currentlyEvaluating == 0
        && evaluation.previewNodeId == node.id;
    const bool isQueued = g_evaluationPending && evaluation.previewNodeId == node.id;
    if (!isCurrent && !isPreviewFallback && !isQueued)
    {
        return NodeEvaluationVisualState::None;
    }

    return isQueued ? NodeEvaluationVisualState::Pending : NodeEvaluationVisualState::Processing;
}

void DrawNodeEvaluationPulse(const rock::Node& node, const ImVec2& clipMin, const ImVec2& clipMax)
{
    const NodeEvaluationVisualState state = GetNodeEvaluationVisualState(node);
    if (state == NodeEvaluationVisualState::None)
    {
        return;
    }

    const ImVec2 nodeSize = ed::GetNodeSize(ed::NodeId(node.id));
    if (nodeSize.x <= 1.0f || nodeSize.y <= 1.0f)
    {
        return;
    }

    const float t = static_cast<float>(ImGui::GetTime());
    const float pulse = state == NodeEvaluationVisualState::Processing
        ? 0.5f + 0.5f * std::sin(t * 5.2f)
        : 0.45f + 0.25f * std::sin(t * 2.7f);
    const ImVec4 color = state == NodeEvaluationVisualState::Processing
        ? ImVec4(0.72f, 0.98f, 0.26f, 0.38f + 0.34f * pulse)
        : ImVec4(0.58f, 0.76f, 0.36f, 0.24f + 0.20f * pulse);
    const ImVec4 glowColor(color.x, color.y, color.z, color.w * 0.32f);

    const ImVec2 nodePos = ed::GetNodePosition(ed::NodeId(node.id));
    const ImVec2 screenMin = ed::CanvasToScreen(nodePos);
    const ImVec2 screenMax = ed::CanvasToScreen(ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y));
    const ImVec2 screen0 = ed::CanvasToScreen(ImVec2(0.0f, 0.0f));
    const ImVec2 screen1 = ed::CanvasToScreen(ImVec2(1.0f, 0.0f));
    const float screenScale = std::clamp(std::abs(screen1.x - screen0.x), 0.25f, 2.0f);
    const float rounding = 8.0f * screenScale;
    const float outerOffset = 3.0f * screenScale;
    const float innerOffset = 1.0f * screenScale;
    const float glowThickness = 4.0f * screenScale;
    const float borderThickness = (state == NodeEvaluationVisualState::Processing ? 2.2f : 1.6f) * screenScale;

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->PushClipRect(clipMin, clipMax, true);
    drawList->AddRect(
        ImVec2(screenMin.x - outerOffset, screenMin.y - outerOffset),
        ImVec2(screenMax.x + outerOffset, screenMax.y + outerOffset),
        ColorToU32(glowColor),
        rounding + outerOffset,
        0,
        glowThickness);
    drawList->AddRect(
        ImVec2(screenMin.x - innerOffset, screenMin.y - innerOffset),
        ImVec2(screenMax.x + innerOffset, screenMax.y + innerOffset),
        ColorToU32(color),
        rounding + innerOffset,
        0,
        borderThickness);
    drawList->PopClipRect();
}

void DrawRockNodeShadows()
{
    ed::Suspend();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (const rock::Node& node : g_graph.Nodes())
    {
        const ImVec2 nodeSize = ed::GetNodeSize(ed::NodeId(node.id));
        if (nodeSize.x <= 1.0f || nodeSize.y <= 1.0f)
        {
            continue;
        }

        const ImVec2 nodePos = ed::GetNodePosition(ed::NodeId(node.id));
        const ImVec2 screenMin = ed::CanvasToScreen(nodePos);
        const ImVec2 screenMax = ed::CanvasToScreen(ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y));
        constexpr float rounding = 8.0f;
        constexpr std::array<std::pair<float, int>, 4> shadowLayers = {{
            {10.0f, 3},
            {7.0f, 5},
            {4.0f, 7},
            {2.0f, 10},
        }};
        for (const auto& [spread, alpha] : shadowLayers)
        {
            drawList->AddRectFilled(
                ImVec2(screenMin.x - spread, screenMin.y - spread),
                ImVec2(screenMax.x + spread, screenMax.y + spread),
                IM_COL32(0, 0, 0, alpha),
                rounding + spread);
        }
    }
    ed::Resume();
}

void DrawRockNode(const rock::Node& node, const ImVec2& editorScreenMin, const ImVec2& editorScreenMax)
{
    constexpr float nodeWidth = 250.0f;
    const ImVec4 accent = NodeAccentColor(node.kind);
    const ImVec4 nodeBorderColor(0.22f, 0.22f, 0.22f, 1.0f);
    const ImVec4 activeNodeBorderColor(0.24f, 0.72f, 0.92f, 1.0f);
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12.0f, 10.0f, 12.0f, 10.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 8.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.080f, 0.080f, 0.080f, 0.98f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, nodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, activeNodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, activeNodeBorderColor);

    ed::BeginNode(ed::NodeId(node.id));

    const ImVec2 headerCursor = ImGui::GetCursorScreenPos();
    DrawNodeIcon(headerCursor, accent);
    ImGui::Dummy(ImVec2(28.0f, 20.0f));
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.88f, 0.88f, 1.0f));
    ImGui::SetWindowFontScale(1.10f);
    const std::string_view displayTitle = node.kind == rock::NodeKind::MaskPath ? rock::ToString(node.kind) : std::string_view(node.title);
    ImGui::TextUnformatted(displayTitle.data(), displayTitle.data() + displayTitle.size());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(nodeWidth, 10.0f));
    const float rowStartX = ImGui::GetCursorPosX();
    const float rowY = ImGui::GetCursorPosY();
    bool suppressNodeHoverBorder = false;

    for (size_t inputIndex = 0; inputIndex < node.inputs.size(); ++inputIndex)
    {
        const rock::Pin& input = node.inputs[inputIndex];
        const float inputY = rowY + static_cast<float>(inputIndex) * 24.0f;
        ImGui::SetCursorPos(ImVec2(rowStartX, inputY));
        ed::BeginPin(ed::PinId(input.id), ed::PinKind::Input);
        DrawRoundPin(input);
        ed::EndPin();
        ImGui::SameLine();
        ImGui::SetCursorPosY(inputY + 2.0f);
        const ImVec2 textPos = ImGui::GetCursorScreenPos();
        const ImVec2 textSize = ImGui::CalcTextSize(input.label.c_str());
        const bool inputTextHovered = ImGui::IsMouseHoveringRect(
            textPos,
            ImVec2(textPos.x + textSize.x, textPos.y + textSize.y));
        suppressNodeHoverBorder = suppressNodeHoverBorder || inputTextHovered;
        ImGui::TextColored(PinLabelColor(input, inputTextHovered, false), "%s", input.label.c_str());
    }

    for (size_t outputIndex = 0; outputIndex < node.outputs.size(); ++outputIndex)
    {
        const rock::Pin& output = node.outputs[outputIndex];
        const bool outputSelected = g_graph.Evaluation().previewPinId == output.id;
        const float outputY = rowY + static_cast<float>(outputIndex) * 24.0f;
        const float labelWidth = ImGui::CalcTextSize(output.label.c_str()).x + (outputSelected ? 2.0f : 0.0f);
        ImGui::SetCursorPos(ImVec2(rowStartX + nodeWidth - labelWidth - 22.0f, outputY + 2.0f));
        const ImVec2 textPos = ImGui::GetCursorScreenPos();
        const ImVec2 textSize = ImGui::CalcTextSize(output.label.c_str());
        const bool outputTextHovered = ImGui::IsMouseHoveringRect(
            textPos,
            ImVec2(textPos.x + textSize.x, textPos.y + textSize.y));
        suppressNodeHoverBorder = suppressNodeHoverBorder || outputTextHovered;
        const ImVec4 outputTextColor = PinLabelColor(output, outputTextHovered, outputSelected);
        ImGui::TextColored(outputTextColor, "%s", output.label.c_str());
        if (outputSelected)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 textMin = ImGui::GetItemRectMin();
            drawList->AddText(ImVec2(textMin.x + 0.7f, textMin.y), ColorToU32(outputTextColor), output.label.c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (!g_pendingPreviewSelectionRestore)
            {
                g_pendingPreviewSelectionRestore = CurrentSelectedNodeIds();
            }
            g_pendingPreviewPinId = output.id;
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(outputY);
        ed::BeginPin(ed::PinId(output.id), ed::PinKind::Output);
        DrawRoundPin(output);
        ed::EndPin();
    }
    const size_t pinRowCount = std::max(node.inputs.size(), node.outputs.size());
    ImGui::Dummy(ImVec2(nodeWidth, std::max(4.0f, static_cast<float>(pinRowCount) * 24.0f - 20.0f)));

    if (suppressNodeHoverBorder)
    {
        ed::PushStyleColor(ed::StyleColor_HovNodeBorder, nodeBorderColor);
    }
    ed::EndNode();
    DrawNodeEvaluationPulse(node, editorScreenMin, editorScreenMax);
    ed::PopStyleColor(suppressNodeHoverBorder ? 5 : 4);
    ed::PopStyleVar(4);
}

void DrawNodeGraphDots(const ImVec2& screenMin, const ImVec2& screenMax)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMin = ed::ScreenToCanvas(screenMin);
    const ImVec2 canvasMax = ed::ScreenToCanvas(screenMax);
    constexpr float baseSpacing = 24.0f;
    const ImU32 backgroundColor = ThemeColor("nodeEditorBg", ImVec4(0.115f, 0.115f, 0.115f, 1.0f));
    const ImU32 dotColor = ThemeColor("nodeGridDot", ImVec4(0.255f, 0.255f, 0.255f, 0.46f));

    ed::Suspend();
    drawList->PushClipRect(screenMin, screenMax, true);
    drawList->AddRectFilled(screenMin, screenMax, backgroundColor);
    const ImVec2 screen0 = ed::CanvasToScreen(ImVec2(0.0f, 0.0f));
    const ImVec2 screenStep = ed::CanvasToScreen(ImVec2(baseSpacing, 0.0f));
    const float baseScreenSpacing = std::max(1.0f, std::abs(screenStep.x - screen0.x));
    const float spacingMultiplier = std::max(1.0f, std::ceil(12.0f / baseScreenSpacing));
    const float spacing = baseSpacing * spacingMultiplier;
    const float startX = std::floor(std::min(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float endX = std::ceil(std::max(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float startY = std::floor(std::min(canvasMin.y, canvasMax.y) / spacing) * spacing;
    const float endY = std::ceil(std::max(canvasMin.y, canvasMax.y) / spacing) * spacing;
    for (float y = startY; y <= endY; y += spacing)
    {
        for (float x = startX; x <= endX; x += spacing)
        {
            const ImVec2 screen = ed::CanvasToScreen(ImVec2(x, y));
            if (screen.x < screenMin.x || screen.x > screenMax.x || screen.y < screenMin.y || screen.y > screenMax.y)
            {
                continue;
            }
            drawList->AddRectFilled(ImVec2(screen.x - 1.0f, screen.y - 1.0f), ImVec2(screen.x + 1.0f, screen.y + 1.0f), dotColor);
        }
    }
    drawList->PopClipRect();
    ed::Resume();
}

ImVec2 CurrentNodeViewCenter(const ImVec2& screenMin, const ImVec2& screenMax)
{
    return ed::ScreenToCanvas(ImVec2(
        (screenMin.x + screenMax.x) * 0.5f,
        (screenMin.y + screenMax.y) * 0.5f));
}

void CopySelectedNodesToClipboard()
{
    std::vector<ed::NodeId> selectedNodes(g_graph.Nodes().size());
    const int selectedCount = ed::GetSelectedNodes(selectedNodes.data(), static_cast<int>(selectedNodes.size()));
    if (selectedCount <= 0)
    {
        return;
    }

    g_nodeClipboard = {};
    std::vector<rock::GraphId> copiedNodeIds;
    copiedNodeIds.reserve(static_cast<size_t>(selectedCount));
    for (int i = 0; i < selectedCount; ++i)
    {
        const rock::GraphId nodeId = ToGraphId(selectedNodes[static_cast<size_t>(i)].Get());
        if (const rock::Node* node = g_graph.FindNode(nodeId))
        {
            g_nodeClipboard.nodes.push_back({*node, ed::GetNodePosition(ed::NodeId(nodeId))});
            copiedNodeIds.push_back(nodeId);
        }
    }

    const auto containsCopiedNode = [&](rock::GraphId nodeId) {
        return std::ranges::find(copiedNodeIds, nodeId) != copiedNodeIds.end();
    };
    for (const rock::Link& link : g_graph.Links())
    {
        const rock::Pin* startPin = g_graph.FindPin(link.startPin);
        const rock::Pin* endPin = g_graph.FindPin(link.endPin);
        if (startPin != nullptr && endPin != nullptr && containsCopiedNode(endPin->nodeId))
        {
            g_nodeClipboard.links.push_back(link);
        }
    }
    SetProjectStatus(std::format("Copied {} node{}", g_nodeClipboard.nodes.size(), g_nodeClipboard.nodes.size() == 1 ? "" : "s"));
}

void PasteNodesFromClipboard(const ImVec2& pasteCenter)
{
    if (g_nodeClipboard.nodes.empty())
    {
        return;
    }

    PushUndoSnapshot();
    g_skipNodeMoveUndoThisFrame = true;
    ImVec2 minPosition = g_nodeClipboard.nodes.front().position;
    ImVec2 maxPosition = g_nodeClipboard.nodes.front().position;
    for (const ClipboardNode& clipboardNode : g_nodeClipboard.nodes)
    {
        minPosition.x = std::min(minPosition.x, clipboardNode.position.x);
        minPosition.y = std::min(minPosition.y, clipboardNode.position.y);
        maxPosition.x = std::max(maxPosition.x, clipboardNode.position.x);
        maxPosition.y = std::max(maxPosition.y, clipboardNode.position.y);
    }
    const ImVec2 sourceCenter((minPosition.x + maxPosition.x) * 0.5f, (minPosition.y + maxPosition.y) * 0.5f);

    std::vector<std::pair<rock::GraphId, rock::GraphId>> nodeMap;
    std::vector<std::pair<rock::GraphId, rock::GraphId>> pinMap;
    std::vector<rock::GraphId> pastedNodeIds;
    for (const ClipboardNode& clipboardNode : g_nodeClipboard.nodes)
    {
        const rock::GraphId newNodeId = g_graph.CreateNode(clipboardNode.node.kind);
        if (rock::Node* newMutableNode = g_graph.FindMutableNode(newNodeId))
        {
            newMutableNode->heightmap = clipboardNode.node.heightmap;
            newMutableNode->shape = clipboardNode.node.shape;
            newMutableNode->heightmapBlur = clipboardNode.node.heightmapBlur;
            newMutableNode->multiScaleErosion = clipboardNode.node.multiScaleErosion;
            newMutableNode->fluvialErosion = clipboardNode.node.fluvialErosion;
            newMutableNode->dropletErosion = clipboardNode.node.dropletErosion;
            newMutableNode->maskNoise = clipboardNode.node.maskNoise;
            newMutableNode->maskBlend = clipboardNode.node.maskBlend;
            newMutableNode->maskCurvature = clipboardNode.node.maskCurvature;
            newMutableNode->maskLevels = clipboardNode.node.maskLevels;
            newMutableNode->maskSlope = clipboardNode.node.maskSlope;
            newMutableNode->maskHeight = clipboardNode.node.maskHeight;
            newMutableNode->crumbling = clipboardNode.node.crumbling;
            newMutableNode->maskFluvial = clipboardNode.node.maskFluvial;
            newMutableNode->rock = clipboardNode.node.rock;
            newMutableNode->scatter = clipboardNode.node.scatter;
            newMutableNode->sediment = clipboardNode.node.sediment;
            newMutableNode->snow = clipboardNode.node.snow;
            newMutableNode->colorize = clipboardNode.node.colorize;
        }
        const rock::Node* newNode = g_graph.FindNode(newNodeId);
        if (newNode == nullptr)
        {
            continue;
        }

        nodeMap.push_back({clipboardNode.node.id, newNodeId});
        pastedNodeIds.push_back(newNodeId);
        for (size_t i = 0; i < clipboardNode.node.inputs.size() && i < newNode->inputs.size(); ++i)
        {
            pinMap.push_back({clipboardNode.node.inputs[i].id, newNode->inputs[i].id});
        }
        for (size_t i = 0; i < clipboardNode.node.outputs.size() && i < newNode->outputs.size(); ++i)
        {
            pinMap.push_back({clipboardNode.node.outputs[i].id, newNode->outputs[i].id});
        }

        const ImVec2 offset(clipboardNode.position.x - sourceCenter.x, clipboardNode.position.y - sourceCenter.y);
        g_pendingNodePositions.push_back({newNodeId, ImVec2(pasteCenter.x + offset.x, pasteCenter.y + offset.y)});
    }

    const auto mappedPin = [&](rock::GraphId oldPinId) {
        const auto it = std::ranges::find_if(pinMap, [oldPinId](const auto& entry) {
            return entry.first == oldPinId;
        });
        return it != pinMap.end() ? it->second : 0;
    };
    for (const rock::Link& clipboardLink : g_nodeClipboard.links)
    {
        const rock::GraphId mappedStartPin = mappedPin(clipboardLink.startPin);
        const rock::GraphId newStartPin = mappedStartPin != 0 ? mappedStartPin : clipboardLink.startPin;
        const rock::GraphId newEndPin = mappedPin(clipboardLink.endPin);
        if (newStartPin != 0 && newEndPin != 0)
        {
            g_graph.CreateLink(newStartPin, newEndPin);
        }
    }

    g_pendingSelectedNodeIds = pastedNodeIds;
    g_selectedNodeId = pastedNodeIds.empty() ? 0 : pastedNodeIds.front();
    SetProjectStatus(std::format("Pasted {} node{}", pastedNodeIds.size(), pastedNodeIds.size() == 1 ? "" : "s"));
    MarkProjectDirty();
    EvaluateGraph();
}

void DrawNodeGraph()
{
    static ImVec2 addNodePosition(0.0f, 0.0f);
    g_frameTiming.nodeCount = static_cast<int>(g_graph.Nodes().size());
    g_frameTiming.linkCount = static_cast<int>(g_graph.Links().size());
    NodeEditorContextScope editorScope(g_nodeEditor);
    g_nodeEditorFrameActive = true;
    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 canvasMax(canvasMin.x + ImGui::GetContentRegionAvail().x, canvasMin.y + ImGui::GetContentRegionAvail().y);
    const ImVec4 activeNodeBorderColor(0.24f, 0.72f, 0.92f, 1.0f);
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_HovNodeBorder, activeNodeBorderColor);
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, activeNodeBorderColor);
    ed::Begin("Rock Node Graph", ImGui::GetContentRegionAvail());
    const auto dotsStart = std::chrono::steady_clock::now();
    DrawNodeGraphDots(canvasMin, canvasMax);
    const auto dotsEnd = std::chrono::steady_clock::now();
    g_frameTiming.nodeEditorDotsMs = std::chrono::duration<double, std::milli>(dotsEnd - dotsStart).count();

    const bool hasPendingNodePositions = !g_pendingNodePositions.empty();
    if (hasPendingNodePositions || !g_nodePositionsInitialized)
    {
        for (const rock::Node& node : g_graph.Nodes())
        {
            if (hasPendingNodePositions)
            {
                const auto pending = std::ranges::find_if(g_pendingNodePositions, [&](const auto& entry) {
                    return entry.first == node.id;
                });
                if (pending != g_pendingNodePositions.end())
                {
                    ed::SetNodePosition(ed::NodeId(node.id), pending->second);
                    continue;
                }
            }
            if (!g_nodePositionsInitialized)
            {
                ed::SetNodePosition(ed::NodeId(node.id), InitialNodePosition(node.kind));
            }
        }
        if (hasPendingNodePositions)
        {
            g_pendingNodePositions.clear();
        }
        g_nodePositionsInitialized = true;
    }

    const auto shadowsStart = std::chrono::steady_clock::now();
    DrawRockNodeShadows();
    const auto shadowsEnd = std::chrono::steady_clock::now();
    g_frameTiming.nodeEditorShadowsMs = std::chrono::duration<double, std::milli>(shadowsEnd - shadowsStart).count();

    const auto nodesStart = std::chrono::steady_clock::now();
    for (const rock::Node& node : g_graph.Nodes())
    {
        DrawRockNode(node, canvasMin, canvasMax);
    }
    const auto nodesEnd = std::chrono::steady_clock::now();
    g_frameTiming.nodeEditorNodesMs = std::chrono::duration<double, std::milli>(nodesEnd - nodesStart).count();

    if (!g_nodeGraphNavigatedToContent)
    {
        ed::NavigateToContent(0.0f);
        g_nodeGraphNavigatedToContent = true;
    }

    const auto interactionStart = std::chrono::steady_clock::now();
    if (!g_pendingSelectedNodeIds.empty())
    {
        ApplyNodeSelection(g_pendingSelectedNodeIds);
        g_pendingSelectedNodeIds.clear();
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_C, false))
    {
        CopySelectedNodesToClipboard();
    }
    if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_V, false))
    {
        PasteNodesFromClipboard(CurrentNodeViewCenter(canvasMin, canvasMax));
    }

    const auto linksStart = std::chrono::steady_clock::now();
    for (const rock::Link& link : g_graph.Links())
    {
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPin), ed::PinId(link.endPin), LinkColor(link), 2.5f);
    }
    const auto linksEnd = std::chrono::steady_clock::now();
    g_frameTiming.nodeEditorLinksMs = std::chrono::duration<double, std::milli>(linksEnd - linksStart).count();

    if (ed::BeginCreate(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f))
    {
        ed::PinId startPinId;
        ed::PinId endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId))
        {
            int startPin = ToGraphId(startPinId.Get());
            int endPin = ToGraphId(endPinId.Get());
            if (g_graph.CanCreateLink(startPin, endPin))
            {
                if (ed::AcceptNewItem(LinkPreviewColor(startPin, endPin), 3.0f))
                {
                    PushUndoSnapshot();
                    if (g_graph.CreateLink(startPin, endPin))
                    {
                        MarkProjectDirty();
                        EvaluateGraph();
                    }
                }
            }
            else
            {
                ed::RejectNewItem(ImVec4(0.78f, 0.28f, 0.24f, 1.0f), 2.0f);
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete())
    {
        bool graphChanged = false;
        std::optional<GraphEditSnapshot> deleteUndoSnapshot;
        const auto captureDeleteUndo = [&]() {
            if (!deleteUndoSnapshot)
            {
                deleteUndoSnapshot = CaptureGraphEditSnapshot();
            }
        };
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId))
        {
            if (ed::AcceptDeletedItem())
            {
                const int linkId = ToGraphId(deletedLinkId.Get());
                captureDeleteUndo();
                if (g_graph.DeleteLink(linkId))
                {
                    graphChanged = true;
                }
            }
        }
        ed::NodeId deletedNodeId;
        while (ed::QueryDeletedNode(&deletedNodeId))
        {
            if (ed::AcceptDeletedItem())
            {
                const int nodeId = ToGraphId(deletedNodeId.Get());
                captureDeleteUndo();
                if (g_graph.DeleteNode(nodeId))
                {
                    graphChanged = true;
                    if (g_selectedNodeId == nodeId)
                    {
                        g_selectedNodeId = 0;
                    }
                    std::erase_if(g_nodePositionCache, [nodeId](const auto& entry) {
                        return entry.first == nodeId;
                    });
                }
            }
        }
        if (graphChanged)
        {
            if (deleteUndoSnapshot)
            {
                CommitUndoSnapshot(std::move(*deleteUndoSnapshot));
            }
            g_skipNodeMoveUndoThisFrame = true;
            MarkProjectDirty();
            EvaluateGraph();
        }
    }
    ed::EndDelete();

    if (ed::ShowBackgroundContextMenu())
    {
        addNodePosition = ImGui::GetMousePos();
        ed::Suspend();
        ImGui::OpenPopup("AddNodeContextMenu");
        ed::Resume();
    }

    ed::Suspend();
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
    if (ImGui::BeginPopup("AddNodeContextMenu"))
    {
        ImGui::TextDisabled("%s", Tr("Add Node", "ノードを追加"));
        ImGui::Separator();
        const auto addNodeMenuItem = [&](rock::NodeKind kind) {
            if (ImGui::MenuItem(rock::ToString(kind).data()))
            {
                PushUndoSnapshot();
                g_skipNodeMoveUndoThisFrame = true;
                const rock::GraphId nodeId = CreateNodeWithEnvironmentDefaults(kind);
                g_pendingNodePositions.push_back({nodeId, addNodePosition});
                g_pendingSelectedNodeIds = {nodeId};
                g_selectedNodeId = nodeId;
                SetProjectStatus("Added " + std::string(rock::ToString(kind)));
                MarkProjectDirty();
                EvaluateGraph();
            }
        };
        if (ImGui::BeginMenu(Tr("Heightfield", "ハイトフィールド")))
        {
            addNodeMenuItem(rock::NodeKind::HeightmapLoad);
            addNodeMenuItem(rock::NodeKind::Shape);
            addNodeMenuItem(rock::NodeKind::HeightmapFromMask);
            addNodeMenuItem(rock::NodeKind::HeightmapBlur);
            addNodeMenuItem(rock::NodeKind::MultiScaleErosion);
            addNodeMenuItem(rock::NodeKind::FluvialErosion);
            addNodeMenuItem(rock::NodeKind::DropletErosion);
            addNodeMenuItem(rock::NodeKind::Crumbling);
            addNodeMenuItem(rock::NodeKind::Rock);
            addNodeMenuItem(rock::NodeKind::Scatter);
            addNodeMenuItem(rock::NodeKind::Sediment);
            addNodeMenuItem(rock::NodeKind::Snow);
            addNodeMenuItem(rock::NodeKind::Soil);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr("Mask", "マスク")))
        {
            addNodeMenuItem(rock::NodeKind::MaskNoise);
            addNodeMenuItem(rock::NodeKind::MaskBlend);
            addNodeMenuItem(rock::NodeKind::MaskLevels);
            addNodeMenuItem(rock::NodeKind::MaskBlur);
            addNodeMenuItem(rock::NodeKind::MaskHeight);
            addNodeMenuItem(rock::NodeKind::MaskSlope);
            addNodeMenuItem(rock::NodeKind::MaskCurvature);
            addNodeMenuItem(rock::NodeKind::MaskFluvial);
            addNodeMenuItem(rock::NodeKind::MaskPath);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr("Color", "カラー")))
        {
            addNodeMenuItem(rock::NodeKind::Colorize);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr("Path", "パス")))
        {
            addNodeMenuItem(rock::NodeKind::Path);
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(4);
    ed::Resume();

    ed::NodeId selectedNodes[1];
    bool restoredPreviewSelection = false;
    if (g_pendingPreviewPinId != 0)
    {
        const rock::GraphId pinId = g_pendingPreviewPinId;
        g_pendingPreviewPinId = 0;
        if (g_graph.SetPreviewPin(pinId))
        {
            MarkProjectDirty();
            if (g_graph.Evaluation().dirty)
            {
                EvaluateGraph();
            }
        }
        if (g_pendingPreviewSelectionRestore)
        {
            ApplyNodeSelection(*g_pendingPreviewSelectionRestore);
            g_pendingPreviewSelectionRestore.reset();
            restoredPreviewSelection = true;
        }
    }
    if (!restoredPreviewSelection && ed::GetSelectedNodes(selectedNodes, 1) > 0)
    {
        const rock::GraphId selectedNodeId = ToGraphId(selectedNodes[0].Get());
        g_selectedNodeId = selectedNodeId;
    }
    else if (!restoredPreviewSelection)
    {
        g_selectedNodeId = 0;
    }
    const auto interactionEnd = std::chrono::steady_clock::now();
    g_frameTiming.nodeEditorInteractionMs = std::chrono::duration<double, std::milli>(interactionEnd - interactionStart).count();

    ed::End();
    const auto positionStart = std::chrono::steady_clock::now();
    std::vector<std::pair<rock::GraphId, ImVec2>> currentNodePositions;
    currentNodePositions.reserve(g_graph.Nodes().size());
    for (const rock::Node& node : g_graph.Nodes())
    {
        const ImVec2 position = ed::GetNodePosition(ed::NodeId(node.id));
        currentNodePositions.push_back({node.id, position});
    }
    if (!g_skipNodeMoveUndoThisFrame && g_nodePositionsInitialized && !g_nodePositionCache.empty() && NodePositionsChanged(currentNodePositions, g_nodePositionCache))
    {
        if (!g_pendingNodeMoveUndo)
        {
            g_pendingNodeMoveUndo = CaptureGraphEditSnapshotWithPositions(g_nodePositionCache);
        }
    }
    if (g_pendingNodeMoveUndo && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        if (NodePositionsChanged(currentNodePositions, g_pendingNodeMoveUndo->nodePositions))
        {
            CommitUndoSnapshot(std::move(*g_pendingNodeMoveUndo));
            SetProjectStatus("Node moved");
            MarkProjectDirty();
        }
        g_pendingNodeMoveUndo.reset();
    }
    g_nodePositionCache = std::move(currentNodePositions);
    g_skipNodeMoveUndoThisFrame = false;
    const auto positionEnd = std::chrono::steady_clock::now();
    g_frameTiming.nodeEditorPositionMs = std::chrono::duration<double, std::milli>(positionEnd - positionStart).count();
    ed::PopStyleColor(4);
    g_nodeEditorFrameActive = false;
}

void DrawPropertiesPanel()
{
    DrawNodePropertiesPanel(g_graph, g_selectedNodeId);
}

void DrawDisplaySettingsPanel()
{
    terrain::ui::DrawDisplaySettingsPanel({
        g_graph.Settings(),
        g_ui.meshPreview,
        g_viewport.orbitDistance,
        []() { return DefaultViewportOrbitDistance(); },
        []() { EvaluateGraph(); },
        [](const char* reason) { MarkGraphChanged(reason); },
        []() { SaveAppSettingsSilently(); },
    });
}
void DrawSkySettingsPanel()
{
    terrain::ui::DrawSkySettingsPanel({
        g_graph.Settings(),
        []() { SaveAppSettingsSilently(); },
    });
}

void DrawCloudSettingsPanel()
{
    terrain::ui::DrawCloudSettingsPanel({
        g_graph.Settings(),
        []() { SaveAppSettingsSilently(); },
    });
}

void DrawWaterSettingsPanel()
{
    terrain::ui::DrawWaterSettingsPanel({
        g_graph.Settings(),
        []() { SaveAppSettingsSilently(); },
    });
}

void DrawCameraPanel()
{
    rock::PreviewSettings& preview = g_graph.Settings().preview;
    terrain::ui::DrawCameraPanel({
        {
            g_viewport.yaw,
            g_viewport.pitch,
            g_viewport.fovDegrees,
            g_viewport.orbitDistance,
            g_viewport.autoOrbitEnabled,
            g_viewport.autoOrbitSpeedDegreesPerSecond,
        },
        preview,
        preview.gridCellCount,
        preview.gridCellSizeMeters,
        {
            kDefaultViewportYaw,
            kDefaultViewportPitch,
            kDefaultViewportFovDegrees,
            DefaultViewportOrbitDistance(),
            kMaxViewportOrbitDistance,
        },
        []() { ResetViewport(); },
        g_focusPickMode,
        []() { g_focusPickMode = true; },
        [](const char* reason) { MarkGraphChanged(reason); },
        []() { SaveAppSettingsSilently(); },
    });
}

void DrawDebugPanel()
{
    const PreviewRenderStats& renderStats = g_gpuMeshPreview.renderStats;
    const uint64_t displayedVertices = renderStats.gpuDisplacement && renderStats.displayMeshResolution > 0
        ? static_cast<uint64_t>(renderStats.displayMeshResolution) * static_cast<uint64_t>(renderStats.displayMeshResolution) * 2u +
            static_cast<uint64_t>(renderStats.displayMeshResolution) * 8u
        : static_cast<uint64_t>(g_gpuMeshPreview.vertexCount);
    const uint64_t displayedTriangles = renderStats.gpuDisplacement
        ? (renderStats.tessellation
            ? static_cast<uint64_t>(g_gpuMeshPreview.displacementPatchIndexCount / 4u) *
                static_cast<uint64_t>(std::ceil(std::max(renderStats.tessellationMaxFactor, 1.0f))) *
                static_cast<uint64_t>(std::ceil(std::max(renderStats.tessellationMaxFactor, 1.0f))) * 2u +
                static_cast<uint64_t>(g_gpuMeshPreview.displacementSectionIndexCount / 3u)
            : static_cast<uint64_t>((g_gpuMeshPreview.displacementTriIndexCount + g_gpuMeshPreview.displacementSectionIndexCount) / 3u))
        : static_cast<uint64_t>(g_gpuMeshPreview.triIndexCount / 3u);

    terrain::ui::DrawDebugPanel({
        g_graph.Settings(),
        g_graph.Evaluation(),
        g_lastEvaluationDuration,
        g_ui.showDrawStats,
        g_ui.showFrameStats,
        {
            renderStats.drawCalls,
            renderStats.indexedDrawCalls,
            renderStats.submittedVertices,
            renderStats.submittedTriangles,
            renderStats.submittedLines,
            renderStats.submittedPatches,
            renderStats.renderTargetWidth,
            renderStats.renderTargetHeight,
            renderStats.displayMeshResolution,
            renderStats.gpuDisplacement,
            renderStats.tessellation,
            renderStats.tessellationMaxFactor,
            renderStats.surfacePass,
            renderStats.wireframePass,
            renderStats.gridPass,
            renderStats.shadowPass,
            renderStats.skyPass,
            renderStats.cloudsPass,
            displayedVertices,
            displayedTriangles,
        },
        {
            static_cast<float>(g_lastFrameTiming.frameMs),
            static_cast<float>(g_lastFrameTiming.messagePumpMs),
            static_cast<float>(g_lastFrameTiming.newFrameMs),
            static_cast<float>(g_lastFrameTiming.mainThreadWorkMs),
            static_cast<float>(g_lastFrameTiming.drawUiMs),
            static_cast<float>(g_lastFrameTiming.viewportTabsMs),
            static_cast<float>(g_lastFrameTiming.nodeEditorMs),
            static_cast<float>(g_lastFrameTiming.nodeEditorDotsMs),
            static_cast<float>(g_lastFrameTiming.nodeEditorShadowsMs),
            static_cast<float>(g_lastFrameTiming.nodeEditorNodesMs),
            static_cast<float>(g_lastFrameTiming.nodeEditorLinksMs),
            static_cast<float>(g_lastFrameTiming.nodeEditorInteractionMs),
            static_cast<float>(g_lastFrameTiming.nodeEditorPositionMs),
            static_cast<float>(g_lastFrameTiming.inspectorMs),
            static_cast<float>(g_lastFrameTiming.statusBarMs),
            static_cast<float>(g_lastFrameTiming.gpuPreviewMs),
            static_cast<float>(g_lastFrameTiming.imguiRenderMs),
            static_cast<float>(g_lastFrameTiming.renderFrameMs),
            static_cast<float>(g_lastFrameTiming.presentMs),
            static_cast<float>(g_lastFrameTiming.frameLimitSleepMs),
            static_cast<float>(g_lastFrameTiming.backgroundSleepMs),
            static_cast<float>(g_lastFrameTiming.fenceWaitMs),
            g_lastFrameTiming.frameRateLimitFps,
            g_lastFrameTiming.windowActive,
            g_lastFrameTiming.windowForeground,
            g_lastFrameTiming.windowMinimized,
            g_lastFrameTiming.backgroundThrottled,
            g_lastFrameTiming.gpuPreviewReason,
            g_lastFrameTiming.nodeCount,
            g_lastFrameTiming.linkCount,
        },
        []() { SaveAppSettingsSilently(); },
    });
}

void DrawAssetExportPanel()
{
    terrain::ui::DrawAssetExportPanel({
        g_graph.Evaluation(),
        g_exportStatus,
        g_graph.Settings().preview.simulationResolution,
        [](const std::filesystem::path& path, int resolution, std::string* error) {
            return ExportCurrentPreviewTexture(path, resolution, error);
        },
        [](const std::filesystem::path& path) {
            OpenExportFolder(path);
        },
    });
}

void BeginInspectorTabContent()
{
    ImGui::Spacing();
    ImGui::Indent(10.0f);
}

void EndInspectorTabContent()
{
    ImGui::Unindent(10.0f);
}

void BeginScrollableInspectorTabContent(const char* id)
{
    const float rightInset = ImGui::GetStyle().WindowPadding.x;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x - rightInset);
    ImGui::BeginChild(id, ImVec2(width, 0.0f), false);
    BeginInspectorTabContent();
}

void EndScrollableInspectorTabContent()
{
    EndInspectorTabContent();
    ImGui::EndChild();
}

struct TabHeaderStyle
{
    ImVec2 framePadding = ImVec2(12.0f, 5.0f);
    ImVec2 itemInnerSpacing = ImVec2(5.0f, 5.0f);
    float fontScale = 1.08f;
};

void PushTabHeaderStyle(const TabHeaderStyle& style = {})
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, style.framePadding);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, style.itemInnerSpacing);
    ImGui::SetWindowFontScale(style.fontScale);
}

void PopTabHeaderStyle()
{
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleVar(2);
}

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

bool BeginStyledTabItem(const char* label, const char* stableId = nullptr)
{
    const std::string stableLabel = StableImGuiLabel(label, stableId);
    const bool open = ImGui::BeginTabItem(stableLabel.c_str());
    if (open)
    {
        PopTabHeaderStyle();
    }
    return open;
}

void EndStyledTabItem(const TabHeaderStyle& style = {})
{
    PushTabHeaderStyle(style);
    ImGui::EndTabItem();
}

bool DrawVerticalSplitter(const char* id, float* leftWidth, float totalWidth, float minLeftWidth, float minRightWidth, float height)
{
    constexpr float splitterWidth = 1.0f;
    constexpr float splitterHitWidth = 9.0f;
    const float maxLeftWidth = std::max(minLeftWidth, totalWidth - minRightWidth - splitterWidth);
    *leftWidth = std::clamp(*leftWidth, minLeftWidth, maxLeftWidth);

    ImGui::SameLine();
    ImGui::PushID(id);
    const ImGuiID splitterId = ImGui::GetID("##splitter");
    ImGui::InvisibleButton("##splitter", ImVec2(splitterWidth, height));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float hitPadding = (splitterHitWidth - splitterWidth) * 0.5f;
    const bool hovered = ImGui::IsMouseHoveringRect(
        ImVec2(min.x - hitPadding, min.y),
        ImVec2(max.x + hitPadding, max.y),
        true);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        g_activeLayoutSplitterId = splitterId;
    }
    bool active = g_activeLayoutSplitterId == splitterId && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool released = g_activeLayoutSplitterId == splitterId && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released)
    {
        g_activeLayoutSplitterId = 0;
        active = false;
    }
    if (active)
    {
        g_layoutSplitterActive = true;
        *leftWidth = std::clamp(*leftWidth + ImGui::GetIO().MouseDelta.x, minLeftWidth, maxLeftWidth);
    }
    if (hovered || active)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    const ImVec4 color = active
        ? g_themeManager.AppColor("accent", ImVec4(0.52f, 0.70f, 0.59f, 1.0f))
        : g_themeManager.AppColor("border", ImVec4(0.22f, 0.24f, 0.23f, 1.0f));
    const float lineX = std::floor((min.x + max.x) * 0.5f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(lineX, min.y),
        ImVec2(lineX, max.y),
        ColorToU32(color),
        active ? 2.0f : 1.0f);
    ImGui::PopID();
    ImGui::SameLine();
    return released;
}

bool DrawHorizontalSplitter(const char* id, float* topHeight, float totalHeight, float minTopHeight, float minBottomHeight)
{
    constexpr float splitterHeight = 1.0f;
    constexpr float splitterHitHeight = 9.0f;
    const float maxTopHeight = std::max(minTopHeight, totalHeight - minBottomHeight - splitterHeight);
    *topHeight = std::clamp(*topHeight, minTopHeight, maxTopHeight);

    ImGui::PushID(id);
    const ImGuiID splitterId = ImGui::GetID("##splitter");
    ImGui::InvisibleButton("##splitter", ImVec2(-1.0f, splitterHeight));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const float hitPadding = (splitterHitHeight - splitterHeight) * 0.5f;
    const bool hovered = ImGui::IsMouseHoveringRect(
        ImVec2(min.x, min.y - hitPadding),
        ImVec2(max.x, max.y + hitPadding),
        true);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        g_activeLayoutSplitterId = splitterId;
    }
    bool active = g_activeLayoutSplitterId == splitterId && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool released = g_activeLayoutSplitterId == splitterId && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released)
    {
        g_activeLayoutSplitterId = 0;
        active = false;
    }
    if (active)
    {
        g_layoutSplitterActive = true;
        *topHeight = std::clamp(*topHeight + ImGui::GetIO().MouseDelta.y, minTopHeight, maxTopHeight);
    }
    if (hovered || active)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    const ImVec4 color = active
        ? g_themeManager.AppColor("accent", ImVec4(0.52f, 0.70f, 0.59f, 1.0f))
        : g_themeManager.AppColor("border", ImVec4(0.22f, 0.24f, 0.23f, 1.0f));
    const float lineY = std::floor((min.y + max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(min.x, lineY),
        ImVec2(max.x, lineY),
        ColorToU32(color),
        active ? 2.0f : 1.0f);
    ImGui::PopID();
    return released;
}

void DrawViewportTabs(float previewWidth, float workHeight, float timeSeconds, ImGuiWindowFlags childFlags)
{
    const TabHeaderStyle defaultTabStyle;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("Preview Viewport", ImVec2(previewWidth, workHeight), false, childFlags);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    PushTabHeaderStyle(defaultTabStyle);
    if (ImGui::BeginTabBar("ViewportTabs"))
    {
        if (BeginStyledTabItem(Tr("3D View", "3Dビュー"), "Main3DView"))
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + ImGui::GetContentRegionAvail().y);
            DrawViewportCube(min, max, timeSeconds);
            ImGui::Dummy(ImGui::GetContentRegionAvail());
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("2D View", "2Dビュー"), "Main2DView"))
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + ImGui::GetContentRegionAvail().y);
            DrawHeightfieldMapPreview(min, max);
            ImGui::Dummy(ImGui::GetContentRegionAvail());
            EndStyledTabItem(defaultTabStyle);
        }
        ImGui::EndTabBar();
    }
    PopTabHeaderStyle();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DrawNodeNetworkTabs(float nodePaneHeight, ImGuiWindowFlags childFlags)
{
    const TabHeaderStyle defaultTabStyle;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 8.0f));
    ImGui::BeginChild("Node Network", ImVec2(0.0f, nodePaneHeight), false, childFlags);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    PushTabHeaderStyle(defaultTabStyle);
    if (ImGui::BeginTabBar("NodeNetworkTabs"))
    {
        if (BeginStyledTabItem(Tr("Node Network", "ノードネットワーク"), "MainNodeNetwork"))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
            DrawNodeGraph();
            ImGui::PopStyleVar();
            EndStyledTabItem(defaultTabStyle);
        }
        ImGui::EndTabBar();
    }
    PopTabHeaderStyle();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();
}

std::string DebugLogTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    char buffer[16]{};
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &localTime);
    return buffer;
}

void AppendDebugLog(std::string message)
{
    if (message.empty())
    {
        return;
    }

    constexpr size_t kMaxDebugLogEntries = 500;
    g_debugLogEntries.push_back({DebugLogTimestamp(), std::move(message)});
    if (g_debugLogEntries.size() > kMaxDebugLogEntries)
    {
        g_debugLogEntries.erase(g_debugLogEntries.begin(), g_debugLogEntries.begin() + static_cast<std::ptrdiff_t>(g_debugLogEntries.size() - kMaxDebugLogEntries));
    }
}

void AppendDebugLogIfChanged(const char* label, const std::string& value, std::string& previous)
{
    if (value == previous)
    {
        return;
    }

    previous = value;
    if (!value.empty())
    {
        AppendDebugLog(std::string(label) + ": " + value);
    }
}

const char* HResultName(HRESULT hr)
{
    switch (hr)
    {
    case S_OK: return "S_OK";
    case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    case DXGI_ERROR_WAS_STILL_DRAWING: return "DXGI_ERROR_WAS_STILL_DRAWING";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_FAIL: return "E_FAIL";
    default: return nullptr;
    }
}

std::string FormatHResult(HRESULT hr)
{
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(static_cast<uint32_t>(hr)));
    std::string result = buffer;
    if (const char* name = HResultName(hr))
    {
        result += " ";
        result += name;
    }
    return result;
}

std::string D3D12FailureMessage(const char* operation, HRESULT hr)
{
    std::string message = std::string(operation) + " (" + FormatHResult(hr) + ")";
    if (g_device)
    {
        const HRESULT reason = g_device->GetDeviceRemovedReason();
        if (reason != S_OK || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG)
        {
            message += "; device removed reason: ";
            message += FormatHResult(reason);
        }
    }
    return message;
}

void CaptureDebugStatusLogs()
{
    static std::string previousProjectStatus;
    static std::string previousExportStatus;
    static std::string previousEvaluationStatus;
    static std::string previousEvaluationDuration;
    static std::string previousSkyStatus;
    static std::string previousCloudStatus;
    static std::string previousDofStatus;
    static std::string previousTonemapStatus;
    static std::string previousMseStatus;
    static std::string previousMaskNoiseStatus;
    static std::string previousMaskUtilityStatus;
    static std::string previousSedimentStatus;
    static std::string previousRockStatus;
    static std::string previousScatterStatus;
    static std::string previousMaskFluvialStatus;
    static std::string previousSnowStatus;
    static std::string previousColorizeStatus;
    static std::string previousGpuPreviewReason;

    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    AppendDebugLogIfChanged("Project", g_projectStatus, previousProjectStatus);
    AppendDebugLogIfChanged("Export", g_exportStatus, previousExportStatus);
    AppendDebugLogIfChanged("Evaluation", evaluation.status, previousEvaluationStatus);
    AppendDebugLogIfChanged("Eval Time", g_lastEvaluationDuration, previousEvaluationDuration);
    AppendDebugLogIfChanged("Sky", g_skyPipelineStatus, previousSkyStatus);
    AppendDebugLogIfChanged("Cloud", g_cloudPipelineStatus, previousCloudStatus);
    AppendDebugLogIfChanged("Depth of Field", g_dofPipelineStatus, previousDofStatus);
    AppendDebugLogIfChanged("Tonemap", g_tonemapPipelineStatus, previousTonemapStatus);
    AppendDebugLogIfChanged("MSE GPU", MseComputeStatus(), previousMseStatus);
    AppendDebugLogIfChanged("Mask Noise GPU", MaskNoiseComputeStatus(), previousMaskNoiseStatus);
    AppendDebugLogIfChanged("Mask Utility GPU", MaskUtilityComputeStatus(), previousMaskUtilityStatus);
    AppendDebugLogIfChanged("Sediment GPU", SedimentComputeStatus(), previousSedimentStatus);
    AppendDebugLogIfChanged("Rock GPU", RockComputeStatus(), previousRockStatus);
    AppendDebugLogIfChanged("Scatter GPU", ScatterComputeStatus(), previousScatterStatus);
    AppendDebugLogIfChanged("Mask Fluvial GPU", MaskFluvialComputeStatus(), previousMaskFluvialStatus);
    AppendDebugLogIfChanged("Snow GPU", SnowComputeStatus(), previousSnowStatus);
    AppendDebugLogIfChanged("Colorize GPU", ColorizeComputeStatus(), previousColorizeStatus);
    AppendDebugLogIfChanged("GPU Preview", g_lastFrameTiming.gpuPreviewReason, previousGpuPreviewReason);
}

void DrawDebugLogWindow(float width, float height, ImGuiWindowFlags childFlags)
{
    const TabHeaderStyle defaultTabStyle;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("Debug Log Window", ImVec2(width, height), true, childFlags);

    PushTabHeaderStyle(defaultTabStyle);
    if (ImGui::BeginTabBar("DebugLogTabs"))
    {
        const float closeSize = ImGui::GetFrameHeight() - 4.0f;
        const float closeX = std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - closeSize - 8.0f);
        ImGui::SameLine(closeX);
        const bool closePressed = ImGui::Button("##CloseDebugArea", ImVec2(closeSize, closeSize));
        const char* closeText = "×";
        const ImVec2 closeMin = ImGui::GetItemRectMin();
        const ImVec2 closeMax = ImGui::GetItemRectMax();
        const ImVec2 closeTextSize = ImGui::CalcTextSize(closeText);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(
                closeMin.x + ((closeMax.x - closeMin.x) - closeTextSize.x) * 0.5f,
                closeMin.y + ((closeMax.y - closeMin.y) - closeTextSize.y) * 0.5f - 1.0f),
            ImGui::GetColorU32(ImGuiCol_Text),
            closeText);
        if (closePressed)
        {
            g_ui.debugLogVisible = false;
            SaveAppSettingsSilently();
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        {
            ImGui::SetTooltip("%s", Tr("Close debug area", "デバッグ領域を閉じる"));
        }
        const std::string logTabLabel = StableImGuiLabel(Tr("Log", "ログ"), "DebugLog");
        if (ImGui::BeginTabItem(logTabLabel.c_str()))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
            ImGui::BeginChild("DebugLogContent", ImVec2(0.0f, 0.0f), false);
            BeginInspectorTabContent();
            ImGui::Checkbox(Tr("Auto-scroll", "自動スクロール"), &g_debugLogAutoScroll);
            const char* clearText = Tr("Clear", "クリア");
            const float clearButtonWidth = ImGui::CalcTextSize(clearText).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - clearButtonWidth));
            if (ImGui::SmallButton(clearText))
            {
                g_debugLogEntries.clear();
            }

            ImGui::Separator();
            std::string logText;
            for (const DebugLogEntry& entry : g_debugLogEntries)
            {
                logText += entry.time;
                logText += "  ";
                logText += entry.message;
                logText += '\n';
            }
            std::vector<char> logTextBuffer(logText.begin(), logText.end());
            logTextBuffer.push_back('\0');
            const ImGuiID logTextId = ImGui::GetID("##DebugLogText");
            ImGuiWindow* debugLogContentWindow = ImGui::GetCurrentWindow();
            ImGui::InputTextMultiline(
                "##DebugLogText",
                logTextBuffer.data(),
                logTextBuffer.size(),
                ImGui::GetContentRegionAvail(),
                ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);
            if (g_debugLogAutoScroll && debugLogContentWindow)
            {
                for (ImGuiWindow* childWindow : debugLogContentWindow->DC.ChildWindows)
                {
                    if (childWindow && childWindow->ChildId == logTextId)
                    {
                        childWindow->Scroll.y = childWindow->ScrollMax.y;
                        childWindow->ScrollTarget.y = FLT_MAX;
                        break;
                    }
                }
            }
            EndInspectorTabContent();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::EndTabItem();
        }
        const std::string diagnosticsTabLabel = StableImGuiLabel(Tr("Diagnostics", "診断"), "DebugDiagnostics");
        if (ImGui::BeginTabItem(diagnosticsTabLabel.c_str()))
        {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
            ImGui::BeginChild("DebugPanelContent", ImVec2(0.0f, 0.0f), false);
            BeginInspectorTabContent();
            DrawDebugPanel();
            EndInspectorTabContent();
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    PopTabHeaderStyle();

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void DrawLanguageSettings()
{
    ImGui::SeparatorText(Tr("Language", "言語"));
    if (ImGui::BeginTable("LanguageSettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(Tr("Language", "言語"));
        ImGui::TableSetColumnIndex(1);

        const char* currentLabel = CurrentLanguage() == UiLanguage::Japanese ? "日本語" : "English";
        ImGui::SetNextItemWidth(std::min(180.0f, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##UiLanguage", currentLabel))
        {
            const bool englishSelected = CurrentLanguage() == UiLanguage::English;
            if (ImGui::Selectable("English", englishSelected))
            {
                SetUiLanguage(UiLanguage::English);
            }
            if (englishSelected)
            {
                ImGui::SetItemDefaultFocus();
            }

            const bool japaneseSelected = CurrentLanguage() == UiLanguage::Japanese;
            if (ImGui::Selectable("日本語", japaneseSelected))
            {
                SetUiLanguage(UiLanguage::Japanese);
            }
            if (japaneseSelected)
            {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
}

int FrameRateLimitIndex(int limitFps)
{
    switch (limitFps)
    {
    case 60:
        return 1;
    case 30:
        return 2;
    default:
        return 0;
    }
}

int FrameRateLimitFromIndex(int index)
{
    switch (index)
    {
    case 1:
        return 60;
    case 2:
        return 30;
    default:
        return 0;
    }
}

int ResolutionPresetIndex(int value)
{
    const int preset = NearestResolutionPreset(value);
    const auto it = std::ranges::find(kResolutionPresets, preset);
    return it != kResolutionPresets.end() ? static_cast<int>(std::distance(kResolutionPresets.begin(), it)) : 2;
}

int ShadowResolutionPresetIndex(int value)
{
    const int preset = NearestShadowResolutionPreset(value);
    const auto it = std::ranges::find(kShadowResolutionPresets, preset);
    return it != kShadowResolutionPresets.end() ? static_cast<int>(std::distance(kShadowResolutionPresets.begin(), it)) : 1;
}

void DrawEnvironmentSettingsPanel()
{
    DrawLanguageSettings();

    rock::GraphSettings& settings = g_graph.Settings();

    ImGui::SeparatorText(Tr("Performance", "パフォーマンス"));
    if (ImGui::BeginTable("EnvironmentPerformanceRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (DrawPropertyBoolRow("FPS", "EnvironmentFps", &g_ui.showFps, "FPS visibility changed", nullptr, true, true))
        {
            SaveAppSettingsSilently();
        }

        int frameRateLimitIndex = FrameRateLimitIndex(g_ui.frameRateLimitFps);
        if (DrawPropertyComboRow(
                "FPS Limit",
                "EnvironmentFrameRateLimit",
                &frameRateLimitIndex,
                Tr("Unlimited\0" "60 FPS\0" "30 FPS\0" "\0", "上限なし\0" "60 FPS\0" "30 FPS\0" "\0"),
                Tr("Frame-rate cap for the whole app, including the 3D viewport. Unlimited does not add an app-side wait.",
                    "3D ビューポートを含むアプリ全体の描画更新上限です。上限なしではアプリ側の待ちを入れません。"),
                0))
        {
            g_ui.frameRateLimitFps = FrameRateLimitFromIndex(frameRateLimitIndex);
            SaveAppSettingsSilently();
        }

        ImGui::EndTable();
    }

    ImGui::SeparatorText(Tr("Viewport", "ビューポート"));
    if (ImGui::BeginTable("EnvironmentViewportRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        int previewResolutionIndex = ResolutionPresetIndex(settings.preview.resolution);
        if (DrawPropertyComboRow(
                "Viewport Mesh",
                "EnvironmentPreviewResolution",
                &previewResolutionIndex,
                "128\0" "256\0" "512\0" "1024\0" "2048\0" "4096\0" "\0",
                Tr("Mesh density for the 3D preview. This changes only displayed subdivisions, not Simulation Resolution.",
                    "3D プレビュー用メッシュの細かさです。Simulation Resolution は変えず、表示の分割数だけを変更します。"),
                ResolutionPresetIndex(rock::PreviewSettings{}.resolution)))
        {
            settings.preview.resolution = kResolutionPresets[static_cast<size_t>(std::clamp(previewResolutionIndex, 0, static_cast<int>(kResolutionPresets.size()) - 1))];
            SaveAppSettingsSilently();
        }

        if (DrawPropertyIntRow("LOD", "EnvironmentPreviewLod", &settings.preview.lod, 0, 4, rock::PreviewSettings{}.lod, "Preview LOD changed", false))
        {
            settings.preview.lod = std::clamp(settings.preview.lod, 0, 4);
            SaveAppSettingsSilently();
        }

        int backendInt = static_cast<int>(settings.preview.meshBackend);
        if (DrawPropertyComboRow(
                "Mesh Backend",
                "EnvironmentMeshBackend",
                &backendInt,
                "CPU Mesh\0GPU Displacement\0\0",
                Tr("Rendering backend for the 3D preview. GPU Displacement uses a static UV grid and displaces it on the GPU.",
                    "プレビュー 3D ビューポートのレンダリング経路です。GPU Displacement は静的 UV グリッドを GPU 側で変位させます。"),
                static_cast<int>(rock::PreviewSettings{}.meshBackend)))
        {
            settings.preview.meshBackend = static_cast<rock::MeshPreviewBackend>(std::clamp(
                backendInt,
                static_cast<int>(rock::MeshPreviewBackend::CpuMesh),
                static_cast<int>(rock::MeshPreviewBackend::GpuDisplacement)));
            SaveAppSettingsSilently();
        }

        if (settings.preview.meshBackend == rock::MeshPreviewBackend::GpuDisplacement)
        {
            if (DrawPropertyBoolRow("Tessellation", "EnvironmentViewportTessellation", &settings.preview.viewportTessellation, "Viewport tessellation changed",
                    Tr("Subdivides only the GPU Displacement viewport rendering with hardware tessellation. Node evaluation and exported meshes are not affected.",
                        "GPU Displacement のビューポート描画だけをハードウェアテセレーションで細分化します。ノード評価やエクスポート用メッシュには影響しません。"),
                    rock::PreviewSettings{}.viewportTessellation, true))
            {
                SaveAppSettingsSilently();
            }

            if (settings.preview.viewportTessellation)
            {
                if (DrawPropertyFloatRow("Tess Min", "EnvironmentTessMin", &settings.preview.tessellationMinFactor, 1.0f, 16.0f, rock::PreviewSettings{}.tessellationMinFactor, "Tessellation min changed", false,
                        Tr("Minimum tessellation factor used in the distance.", "遠景で使う最小テセレーション係数です。")))
                {
                    settings.preview.tessellationMinFactor = std::clamp(settings.preview.tessellationMinFactor, 1.0f, 64.0f);
                    settings.preview.tessellationMaxFactor = std::max(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor);
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Tess Max", "EnvironmentTessMax", &settings.preview.tessellationMaxFactor, 1.0f, 32.0f, rock::PreviewSettings{}.tessellationMaxFactor, "Tessellation max changed", false,
                        Tr("Maximum tessellation factor used nearby. Higher values look smoother but cost more to render.", "近景で使う最大テセレーション係数です。高いほど滑らかになりますが描画負荷が増えます。")))
                {
                    settings.preview.tessellationMaxFactor = std::clamp(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor, 64.0f);
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Tess Near (m)", "EnvironmentTessNear", &settings.preview.tessellationNearDistance, 1.0f, 20000.0f, rock::PreviewSettings{}.tessellationNearDistance, "Tessellation near changed", false,
                        Tr("Uses the maximum tessellation factor up to this distance.", "この距離までは最大テセレーション係数を使います。"), "%.0f"))
                {
                    settings.preview.tessellationNearDistance = std::clamp(settings.preview.tessellationNearDistance, 1.0f, 100000.0f);
                    settings.preview.tessellationFarDistance = std::max(settings.preview.tessellationFarDistance, settings.preview.tessellationNearDistance + 1.0f);
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Tess Far (m)", "EnvironmentTessFar", &settings.preview.tessellationFarDistance, 1.0f, 50000.0f, rock::PreviewSettings{}.tessellationFarDistance, "Tessellation far changed", false,
                        Tr("Falls back to the minimum tessellation factor beyond this distance.", "この距離以遠では最小テセレーション係数へ落とします。"), "%.0f"))
                {
                    settings.preview.tessellationFarDistance = std::clamp(settings.preview.tessellationFarDistance, settings.preview.tessellationNearDistance + 1.0f, 200000.0f);
                    SaveAppSettingsSilently();
                }
            }
        }

        int shadowMapResolutionIndex = ShadowResolutionPresetIndex(settings.preview.shadowMapResolution);
        if (DrawPropertyComboRow(
                "Shadow Map",
                "EnvironmentShadowMapResolution",
                &shadowMapResolutionIndex,
                "512\0" "1024\0" "2048\0" "4096\0" "\0",
                Tr("Resolution of the terrain shadow depth map. Higher values create finer shadow edges but cost more to render.",
                    "地形シャドウ用深度マップの解像度です。高いほど影の輪郭が細かくなりますが描画負荷が増えます。"),
                ShadowResolutionPresetIndex(rock::PreviewSettings{}.shadowMapResolution)))
        {
            settings.preview.shadowMapResolution = kShadowResolutionPresets[static_cast<size_t>(std::clamp(shadowMapResolutionIndex, 0, static_cast<int>(kShadowResolutionPresets.size()) - 1))];
            SaveAppSettingsSilently();
        }

        ImGui::EndTable();
    }

    ImGui::SeparatorText(Tr("Cloud Quality", "雲の品質"));
    if (ImGui::BeginTable("EnvironmentCloudQualityRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (DrawPropertyIntRow("Quality", "EnvironmentCloudQuality", &settings.clouds.qualitySamples, 8, 96, rock::CloudSettings{}.qualitySamples, "Cloud quality changed", false,
                Tr("Raymarch samples per pixel. Higher values improve cloud detail but increase rendering cost.",
                    "1 ピクセルあたりのレイマーチサンプル数。大きいほど雲のディテールが上がりますが負荷も増えます。")))
        {
            settings.clouds.qualitySamples = std::clamp(settings.clouds.qualitySamples, 8, 128);
            SaveAppSettingsSilently();
        }

        int cloudShadowResolutionIndex = ShadowResolutionPresetIndex(settings.clouds.shadowResolution);
        if (DrawPropertyComboRow(
                "Shadow Res",
                "EnvironmentCloudShadowResolution",
                &cloudShadowResolutionIndex,
                "512\0" "1024\0" "2048\0" "4096\0" "\0",
                Tr("Resolution of the cloud shadow texture. Larger values create finer shadow edges but cost more to generate.",
                    "雲影テクスチャの解像度です。大きいほど影の輪郭が細かくなりますが生成負荷が増えます。"),
                ShadowResolutionPresetIndex(rock::CloudSettings{}.shadowResolution)))
        {
            settings.clouds.shadowResolution = kShadowResolutionPresets[static_cast<size_t>(std::clamp(cloudShadowResolutionIndex, 0, static_cast<int>(kShadowResolutionPresets.size()) - 1))];
            SaveAppSettingsSilently();
        }

        if (DrawPropertyIntRow("Shadow Samples", "EnvironmentCloudShadowSamples", &settings.clouds.shadowSamples, 4, 64, rock::CloudSettings{}.shadowSamples, "Cloud shadow samples changed", false,
                Tr("Samples shot along the sun direction when generating the cloud shadow texture.",
                    "雲影テクスチャ生成時に太陽方向へ撃つレイのサンプル数です。")))
        {
            settings.clouds.shadowSamples = std::clamp(settings.clouds.shadowSamples, 4, 64);
            SaveAppSettingsSilently();
        }

        if (DrawPropertyIntRow("Light Samples", "EnvironmentCloudLightSamples", &settings.clouds.lightSamples, 1, 16, rock::CloudSettings{}.lightSamples, "Cloud light samples changed", false,
                Tr("Sun-direction raymarch steps for cloud self-shadowing.",
                    "雲内自己遮蔽の太陽方向レイマーチ段数です。")))
        {
            settings.clouds.lightSamples = std::clamp(settings.clouds.lightSamples, 1, 16);
            SaveAppSettingsSilently();
        }

        ImGui::EndTable();
    }

    ImGui::SeparatorText(Tr("Node Defaults", "ノード既定値"));
    if (ImGui::BeginTable("EnvironmentNodeDefaultRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        int nodeBackendIndex = g_ui.newNodeBackendGpu ? 1 : 0;
        if (DrawPropertyComboRow(
                "Backend",
                "EnvironmentNodeBackendDefault",
                &nodeBackendIndex,
                "CPU\0GPU\0\0",
                Tr("Default backend for newly created nodes. Existing nodes keep their own saved backend.",
                    "新しく作成するノードの既定バックエンドです。既存ノードは個別に保存された Backend を維持します。"),
                1))
        {
            g_ui.newNodeBackendGpu = nodeBackendIndex == 1;
            SaveAppSettingsSilently();
        }

        ImGui::EndTable();
    }
}

void DrawUi()
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float timeSeconds = std::chrono::duration<float>(now - start).count();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
    {
        g_layoutSplitterActive = false;
    }
    constexpr ImGuiWindowFlags shellFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_MenuBar;
    constexpr ImGuiWindowFlags fixedPaneFlags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Terrain Editor Shell", nullptr, shellFlags);

    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        SaveCurrentProject();
    }
    if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    {
        if (io.KeyShift)
        {
            RedoGraphEdit();
        }
        else
        {
            UndoGraphEdit();
        }
    }
    if (io.KeyCtrl && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    {
        RedoGraphEdit();
    }
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A, false))
    {
        ResetViewport();
    }
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_O, false))
    {
        g_viewport.autoOrbitEnabled = !g_viewport.autoOrbitEnabled;
        SaveAppSettingsSilently();
    }
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_D, false))
    {
        rock::PreviewSettings& preview = g_graph.Settings().preview;
        preview.depthOfFieldEnabled = !preview.depthOfFieldEnabled;
        if (!preview.depthOfFieldEnabled)
        {
            g_focusPickMode = false;
        }
        MarkGraphChanged("Depth of Field toggled");
    }
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C, false))
    {
        rock::CloudSettings& clouds = g_graph.Settings().clouds;
        clouds.enabled = !clouds.enabled;
        MarkGraphChanged("Clouds toggled");
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F12, false))
    {
        std::filesystem::path screenshotPath;
        std::string error;
        if (terrain::CaptureWindowScreenshot(g_hwnd, ScreenshotDirectory(), &screenshotPath, &error))
        {
            SetProjectStatus("Screenshot saved " + PathToUtf8(screenshotPath));
        }
        else
        {
            SetProjectStatus("Screenshot failed: " + error);
        }
    }
    RefreshProjectStatus();
    CaptureDebugStatusLogs();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    if (ImGui::BeginMenuBar())
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
        if (ImGui::BeginMenu(Tr("File", "ファイル")))
        {
            if (ImGui::MenuItem(Tr("New", "新規"), "Ctrl+N"))
            {
                if (ConfirmSaveUnsavedChanges())
                {
                    NewProject();
                }
            }
            if (ImGui::MenuItem(Tr("Open", "開く"), "Ctrl+O"))
            {
                if (ConfirmSaveUnsavedChanges())
                {
                    if (const std::optional<std::filesystem::path> path = ShowProjectFileDialog(false))
                    {
                        std::string error;
                        if (!LoadProjectFromFile(*path, &error))
                        {
                            SetProjectStatus("Load failed: " + error);
                        }
                    }
                }
            }
            if (PruneMissingRecentProjectPaths())
            {
                SaveAppSettingsSilently();
            }
            if (ImGui::BeginMenu(Tr("Recent Files", "最近使ったファイル"), !g_recentProjectPaths.empty()))
            {
                for (size_t index = 0; index < g_recentProjectPaths.size(); ++index)
                {
                    const std::filesystem::path& recentPath = g_recentProjectPaths[index];
                    const std::string label =
                        std::to_string(index + 1) + ". " + PathToUtf8(recentPath.filename()) + "##RecentProject" + std::to_string(index);
                    if (ImGui::MenuItem(label.c_str()))
                    {
                        if (ConfirmSaveUnsavedChanges())
                        {
                            std::string error;
                            if (!LoadProjectFromFile(recentPath, &error))
                            {
                                SetProjectStatus("Load failed: " + error);
                            }
                        }
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", PathToUtf8(recentPath).c_str());
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(Tr("Clear History", "履歴をクリア")))
                {
                    g_recentProjectPaths.clear();
                    SaveAppSettingsSilently();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(Tr("Save", "保存"), "Ctrl+S"))
            {
                SaveCurrentProject();
            }
            if (ImGui::MenuItem(Tr("Save As", "名前を付けて保存")))
            {
                if (const std::optional<std::filesystem::path> path = ShowProjectFileDialog(true))
                {
                    std::string error;
                    if (!SaveProjectToFile(*path, &error))
                    {
                        SetProjectStatus("Save failed: " + error);
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Tr("Open Project Folder", "プロジェクトの保存場所を開く"), nullptr, false, !g_projectPath.empty()))
            {
                OpenFolderInExplorer(ProjectFolder());
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Tr("Exit", "終了")))
            {
                if (ConfirmSaveUnsavedChanges())
                {
                    DestroyWindow(g_hwnd);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr("Edit", "編集")))
        {
            if (ImGui::MenuItem(Tr("Undo", "元に戻す"), "Ctrl+Z", false, !g_undoStack.empty()))
            {
                UndoGraphEdit();
            }
            if (ImGui::MenuItem(Tr("Redo", "やり直し"), "Ctrl+Y", false, !g_redoStack.empty()))
            {
                RedoGraphEdit();
            }
            ImGui::Separator();
            ImGui::MenuItem(Tr("Copy", "コピー"), "Ctrl+C", false, false);
            ImGui::MenuItem(Tr("Paste", "貼り付け"), "Ctrl+V", false, false);
            ImGui::MenuItem(Tr("Delete", "削除"), "Delete", false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr("View", "表示")))
        {
            if (ImGui::MenuItem(Tr("Debug Log", "デバッグログ"), nullptr, g_ui.debugLogVisible))
            {
                g_ui.debugLogVisible = !g_ui.debugLogVisible;
                SaveAppSettingsSilently();
            }
            ImGui::Separator();
            if (ImGui::MenuItem(Tr("Reset Layout", "レイアウトを初期化")))
            {
                ResetLayoutToDefaults();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(Tr("Settings", "設定")))
        {
            if (ImGui::BeginMenu(Tr("Language", "言語")))
            {
                if (ImGui::MenuItem("English", nullptr, CurrentLanguage() == UiLanguage::English))
                {
                    SetUiLanguage(UiLanguage::English);
                }
                if (ImGui::MenuItem("日本語", nullptr, CurrentLanguage() == UiLanguage::Japanese))
                {
                    SetUiLanguage(UiLanguage::Japanese);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(Tr("UI Theme", "UIテーマ")))
            {
                for (const rock::UiThemeInfo& themeInfo : g_themeManager.ThemeInfos())
                {
                    const bool selected = themeInfo.id == g_themeManager.CurrentThemeId();
                    if (ImGui::MenuItem(themeInfo.name.c_str(), nullptr, selected))
                    {
                        g_themeManager.ApplyTheme(themeInfo.id);
                        SaveAppSettingsSilently();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::MenuItem(Tr("Preferences", "環境設定"), nullptr, false, false);
            ImGui::MenuItem(Tr("Shortcut Settings", "ショートカット設定"), nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::PopStyleVar(5);

    const ImVec2 content = ImGui::GetContentRegionAvail();
    const float statusBarHeight = ImGui::GetTextLineHeight() + 16.0f;
    const float workHeight = std::max(260.0f, content.y - statusBarHeight);
    constexpr float mainSplitterWidth = 1.0f;
    constexpr float paneMinWidth = 320.0f;
    const float minMainLayoutWidth = paneMinWidth * 2.0f + mainSplitterWidth;
    const bool mainLayoutCanFit = content.x >= minMainLayoutWidth;
    float rightPaneWidth = g_ui.rightPaneWidth;
    if (rightPaneWidth <= 0.0f)
    {
        rightPaneWidth = std::clamp(content.x * 0.42f, 480.0f, std::min(820.0f, std::max(paneMinWidth, content.x - paneMinWidth)));
    }
    const float maxRightWidth = std::max(paneMinWidth, content.x - paneMinWidth - mainSplitterWidth);
    rightPaneWidth = std::clamp(rightPaneWidth, paneMinWidth, maxRightWidth);
    if (mainLayoutCanFit)
    {
        g_ui.rightPaneWidth = rightPaneWidth;
    }
    float previewWidth = std::max(paneMinWidth, content.x - rightPaneWidth - mainSplitterWidth);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

    const TabHeaderStyle defaultTabStyle;
    const auto viewportTabsStart = std::chrono::steady_clock::now();
    {
        ImGui::BeginChild("Left Work Column", ImVec2(previewWidth, workHeight), false, fixedPaneFlags);
        const float leftColumnWidth = ImGui::GetContentRegionAvail().x;
        constexpr float debugLogSplitterHeight = 1.0f;
        const bool debugLogCanFit = workHeight >= 320.0f;
        if (g_ui.debugLogVisible && debugLogCanFit)
        {
            float debugLogHeight = std::clamp(g_ui.debugLogHeight, 100.0f, std::max(100.0f, workHeight - 180.0f - debugLogSplitterHeight));
            float viewportHeight = std::max(180.0f, workHeight - debugLogHeight - debugLogSplitterHeight);
            DrawViewportTabs(leftColumnWidth, viewportHeight, timeSeconds, fixedPaneFlags);
            const bool debugLogSplitterReleased = DrawHorizontalSplitter("ViewportDebugLogSplitter", &viewportHeight, workHeight, 180.0f, 100.0f);
            debugLogHeight = std::max(100.0f, workHeight - viewportHeight - debugLogSplitterHeight);
            g_ui.debugLogHeight = debugLogHeight;
            if (debugLogSplitterReleased)
            {
                SaveAppSettingsSilently();
            }
            DrawDebugLogWindow(leftColumnWidth, debugLogHeight, fixedPaneFlags);
        }
        else
        {
            DrawViewportTabs(leftColumnWidth, workHeight, timeSeconds, fixedPaneFlags);
        }
        ImGui::EndChild();
    }
    const auto viewportTabsEnd = std::chrono::steady_clock::now();
    g_frameTiming.viewportTabsMs = std::chrono::duration<double, std::milli>(viewportTabsEnd - viewportTabsStart).count();

    if (DrawVerticalSplitter("MainLayoutSplitter", &previewWidth, content.x, paneMinWidth, paneMinWidth, workHeight))
    {
        if (mainLayoutCanFit)
        {
            SaveAppSettingsSilently();
        }
    }
    rightPaneWidth = std::max(paneMinWidth, content.x - previewWidth - mainSplitterWidth);
    if (mainLayoutCanFit)
    {
        g_ui.rightPaneWidth = rightPaneWidth;
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild("Right Work Column", ImVec2(rightPaneWidth, workHeight), false, fixedPaneFlags);
    const float rightColumnHeight = ImGui::GetContentRegionAvail().y;
    constexpr float inspectorSplitterHeight = 1.0f;
    const bool inspectorLayoutCanFit = rightColumnHeight >= 160.0f * 2.0f + inspectorSplitterHeight;
    float nodePaneHeight = g_ui.nodePaneHeight;
    if (nodePaneHeight <= 0.0f)
    {
        nodePaneHeight = std::clamp(rightColumnHeight * 0.56f, 220.0f, std::max(220.0f, rightColumnHeight - 190.0f));
    }
    nodePaneHeight = std::clamp(nodePaneHeight, 160.0f, std::max(160.0f, rightColumnHeight - 160.0f - inspectorSplitterHeight));
    if (inspectorLayoutCanFit)
    {
        g_ui.nodePaneHeight = nodePaneHeight;
    }

    const auto nodeEditorStart = std::chrono::steady_clock::now();
    DrawNodeNetworkTabs(nodePaneHeight, fixedPaneFlags);
    const auto nodeEditorEnd = std::chrono::steady_clock::now();
    g_frameTiming.nodeEditorMs = std::chrono::duration<double, std::milli>(nodeEditorEnd - nodeEditorStart).count();

    const bool inspectorSplitterReleased = DrawHorizontalSplitter("InspectorLayoutSplitter", &nodePaneHeight, rightColumnHeight, 160.0f, 160.0f);
    if (inspectorLayoutCanFit)
    {
        g_ui.nodePaneHeight = nodePaneHeight;
    }
    if (inspectorSplitterReleased && inspectorLayoutCanFit)
    {
        SaveAppSettingsSilently();
    }

    const auto inspectorStart = std::chrono::steady_clock::now();
    ImGui::BeginChild("Inspector", ImVec2(0.0f, 0.0f), false, fixedPaneFlags);
    PushTabHeaderStyle(defaultTabStyle);
    if (ImGui::BeginTabBar("InspectorTabs"))
    {
        if (BeginStyledTabItem(Tr("Properties", "プロパティ"), "InspectorProperties"))
        {
            BeginScrollableInspectorTabContent("InspectorPropertiesContent");
            DrawPropertiesPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("Settings", "設定"), "InspectorSettings"))
        {
            BeginScrollableInspectorTabContent("InspectorSettingsContent");
            DrawDisplaySettingsPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("Sky", "天球"), "InspectorSky"))
        {
            BeginScrollableInspectorTabContent("InspectorSkyContent");
            DrawSkySettingsPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("Clouds", "雲"), "InspectorClouds"))
        {
            BeginScrollableInspectorTabContent("InspectorCloudsContent");
            DrawCloudSettingsPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("Water", "水面"), "InspectorWater"))
        {
            BeginScrollableInspectorTabContent("InspectorWaterContent");
            DrawWaterSettingsPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("Camera", "カメラ"), "InspectorCamera"))
        {
            BeginScrollableInspectorTabContent("InspectorCameraContent");
            DrawCameraPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("Export", "エクスポート"), "InspectorExport"))
        {
            BeginScrollableInspectorTabContent("InspectorExportContent");
            DrawAssetExportPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem(Tr("Environment", "環境設定"), "InspectorEnvironment"))
        {
            BeginScrollableInspectorTabContent("InspectorEnvironmentContent");
            DrawEnvironmentSettingsPanel();
            EndScrollableInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        ImGui::EndTabBar();
    }
    PopTabHeaderStyle();
    ImGui::EndChild();
    const auto inspectorEnd = std::chrono::steady_clock::now();
    g_frameTiming.inspectorMs = std::chrono::duration<double, std::milli>(inspectorEnd - inspectorStart).count();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    const auto statusBarStart = std::chrono::steady_clock::now();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::BeginChild("Status Bar", ImVec2(0.0f, statusBarHeight), true, fixedPaneFlags);
    const ImVec2 statusMin = ImGui::GetWindowPos();
    const ImVec2 statusMax(statusMin.x + ImGui::GetWindowWidth(), statusMin.y + ImGui::GetWindowHeight());
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    const char* evaluationState = g_evaluationInFlight
        ? (g_evaluationPending ? "Pending" : "Processing")
        : (evaluation.dirty ? "Dirty" : "Evaluated");
    const ImVec4 stateColor = g_evaluationInFlight
        ? ImVec4(0.90f, 0.72f, 0.34f, 1.0f)
        : (evaluation.dirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f));
    std::string statusDetail = std::format(
        " | {} | {} | {}",
        rock::ToString(evaluation.previewStage).data(),
        g_lastEvaluationDuration,
        g_projectStatus);
    if (!g_exportStatus.empty())
    {
        statusDetail += " | ";
        statusDetail += g_exportStatus;
    }
    const float textY = statusMin.y + 2.0f;
    const float textX = statusMin.x + 8.0f;
    ImDrawList* statusDrawList = ImGui::GetWindowDrawList();
    statusDrawList->AddLine(
        statusMin,
        ImVec2(statusMax.x, statusMin.y),
        ColorToU32(g_themeManager.AppColor("border", ImVec4(0.22f, 0.24f, 0.23f, 1.0f))),
        1.0f);
    statusDrawList->AddText(ImVec2(textX, textY), ImGui::GetColorU32(stateColor), evaluationState);
    statusDrawList->AddText(
        ImVec2(textX + ImGui::CalcTextSize(evaluationState).x + 4.0f, textY),
        ImGui::GetColorU32(ImGuiCol_Text),
        statusDetail.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
    const auto statusBarEnd = std::chrono::steady_clock::now();
    g_frameTiming.statusBarMs = std::chrono::duration<double, std::milli>(statusBarEnd - statusBarStart).count();

    ImGui::PopStyleVar(3);

    ImGui::End();
    ImGui::PopStyleVar();
}

void RenderFrame()
{
    const auto renderStart = std::chrono::steady_clock::now();
    FrameContext& frameContext = WaitForNextFrameResources();
    ThrowIfFailed(frameContext.commandAllocator->Reset(), "CommandAllocator reset failed");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    ThrowIfFailed(g_commandList->Reset(frameContext.commandAllocator.Get(), nullptr), "CommandList reset failed");
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;

    const float clearColor[4] = {0.10f, 0.11f, 0.12f, 1.0f};
    g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    g_commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);
    ThrowIfFailed(g_commandList->Close(), "CommandList close failed");

    ID3D12CommandList* commandLists[] = {g_commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, commandLists);
    const auto presentStart = std::chrono::steady_clock::now();
    const HRESULT presentHr = g_swapChain->Present(0, 0);
    if (FAILED(presentHr))
    {
        const std::string message = D3D12FailureMessage("Present failed", presentHr);
        AppendDebugLog(message);
        throw std::runtime_error(message);
    }
    const auto presentEnd = std::chrono::steady_clock::now();
    g_frameTiming.presentMs = std::chrono::duration<double, std::milli>(presentEnd - presentStart).count();

    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal failed");
    frameContext.fenceValue = fenceValue;
    const auto renderEnd = std::chrono::steady_clock::now();
    g_frameTiming.renderFrameMs = std::chrono::duration<double, std::milli>(renderEnd - renderStart).count();
}

void ApplyFrameRateLimit(std::chrono::steady_clock::time_point frameStart)
{
    using Clock = std::chrono::steady_clock;

    const int limitFps = ClampFrameRateLimitFps(g_ui.frameRateLimitFps);
    if (limitFps <= 0)
    {
        g_frameTiming.frameLimitSleepMs = 0.0;
        return;
    }

    const auto frameDuration = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / static_cast<double>(limitFps)));
    const Clock::time_point targetFrameEnd = frameStart + frameDuration;
    const Clock::time_point now = Clock::now();

    if (now < targetFrameEnd)
    {
        const auto sleepStart = Clock::now();
        while (true)
        {
            const Clock::time_point loopNow = Clock::now();
            if (loopNow >= targetFrameEnd)
            {
                break;
            }
            const auto remaining = targetFrameEnd - loopNow;
            if (remaining > std::chrono::milliseconds(2))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else
            {
                std::this_thread::yield();
            }
        }
        const auto sleepEnd = Clock::now();
        g_frameTiming.frameLimitSleepMs = std::chrono::duration<double, std::milli>(sleepEnd - sleepStart).count();
    }
    else
    {
        g_frameTiming.frameLimitSleepMs = 0.0;
    }
}

void ProcessMainThreadEvaluationWork()
{
    ProcessPendingMseGpuRequests();
    ProcessPendingMaskNoiseGpuRequests();
    ProcessPendingMaskUtilityGpuRequests();
    ProcessPendingSedimentGpuRequests();
    ProcessPendingRockGpuRequests();
    ProcessPendingScatterGpuRequests();
    ProcessPendingMaskFluvialGpuRequests();
    ProcessPendingFluvialErosionGpuRequests();
    ProcessPendingDropletErosionGpuRequests();
    ProcessPendingSnowGpuRequests();
    ProcessPendingColorizeGpuRequests();
    PollAsyncEvaluation();
}

float WrapUnitPhase(float value)
{
    value = std::fmod(value, 1.0f);
    if (value < 0.0f)
    {
        value += 1.0f;
    }
    return value;
}

void UpdateCloudLoopPhase(float deltaSeconds)
{
    rock::CloudSettings& clouds = g_graph.Settings().clouds;
    if (!clouds.enabled || !clouds.animate || clouds.windSpeedMetersPerSec <= 0.0f)
    {
        clouds.loopPhase = std::clamp(clouds.loopPhase, 0.0f, 1.0f);
        return;
    }

    const CloudLoopVector cloudLoop = ComputeCloudLoopVector(clouds);
    clouds.loopPhase = WrapUnitPhase(clouds.loopPhase + deltaSeconds * clouds.windSpeedMetersPerSec / cloudLoop.distanceMeters);
}

void AdvanceSunDateTime(rock::PreviewSettings& preview, float hours)
{
    if (!std::isfinite(hours) || hours == 0.0f)
    {
        preview.sunTimeHours = std::clamp(preview.sunTimeHours, 0.0f, 24.0f);
        preview.sunMonth = std::clamp(preview.sunMonth, 1, 12);
        preview.sunDay = std::clamp(preview.sunDay, 1, DaysInMonth(preview.sunMonth));
        return;
    }

    preview.sunMonth = std::clamp(preview.sunMonth, 1, 12);
    preview.sunDay = std::clamp(preview.sunDay, 1, DaysInMonth(preview.sunMonth));
    preview.sunTimeHours += hours;
    while (preview.sunTimeHours >= 24.0f)
    {
        preview.sunTimeHours -= 24.0f;
        ++preview.sunDay;
        if (preview.sunDay > DaysInMonth(preview.sunMonth))
        {
            preview.sunDay = 1;
            preview.sunMonth = preview.sunMonth == 12 ? 1 : preview.sunMonth + 1;
        }
    }
    while (preview.sunTimeHours < 0.0f)
    {
        preview.sunTimeHours += 24.0f;
        --preview.sunDay;
        if (preview.sunDay < 1)
        {
            preview.sunMonth = preview.sunMonth == 1 ? 12 : preview.sunMonth - 1;
            preview.sunDay = DaysInMonth(preview.sunMonth);
        }
    }
}

void SkipSunNightIfNeeded(rock::PreviewSettings& preview)
{
    constexpr float kNightSkipElevationDegrees = -5.0f;
    if (!preview.sunTimeSkipNight || ComputeDateTimeSunPosition(preview).elevation >= kNightSkipElevationDegrees)
    {
        return;
    }

    rock::PreviewSettings candidate = preview;
    constexpr float kSearchStepHours = 5.0f / 60.0f;
    constexpr int kMaxSearchSteps = static_cast<int>((48.0f / kSearchStepHours) + 0.5f);
    for (int i = 0; i < kMaxSearchSteps; ++i)
    {
        AdvanceSunDateTime(candidate, kSearchStepHours);
        if (ComputeDateTimeSunPosition(candidate).elevation >= kNightSkipElevationDegrees)
        {
            preview.sunMonth = candidate.sunMonth;
            preview.sunDay = candidate.sunDay;
            preview.sunTimeHours = candidate.sunTimeHours;
            return;
        }
    }
}

void UpdateSunTimeAnimation(float deltaSeconds)
{
    rock::PreviewSettings& preview = g_graph.Settings().preview;
    if (preview.sunDirectionMode != rock::SunDirectionMode::DateTime || !preview.sunTimeAnimate)
    {
        return;
    }

    preview.sunTimeDayLengthSeconds = std::clamp(preview.sunTimeDayLengthSeconds, 5.0f, 3600.0f);
    if (deltaSeconds <= 0.0f || !std::isfinite(deltaSeconds))
    {
        SkipSunNightIfNeeded(preview);
        return;
    }

    const float clampedDeltaSeconds = std::min(deltaSeconds, 1.0f);
    AdvanceSunDateTime(preview, clampedDeltaSeconds * 24.0f / preview.sunTimeDayLengthSeconds);
    SkipSunNightIfNeeded(preview);
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_ACTIVATEAPP:
        g_windowActive = (wParam != FALSE);
        break;
    case WM_ACTIVATE:
        g_windowActive = (LOWORD(wParam) != WA_INACTIVE);
        break;
    case WM_SIZE:
        g_windowMinimized = (wParam == SIZE_MINIMIZED);
        break;
    default:
        break;
    }

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return true;
    }

    switch (msg)
    {
    case WM_SETTEXT:
        if (!g_windowTitle.empty())
        {
            return DefWindowProcW(hwnd, WM_SETTEXT, wParam, reinterpret_cast<LPARAM>(g_windowTitle.c_str()));
        }
        break;
    case WM_SIZE:
        if (!g_windowMinimized)
        {
            ResizeSwapChain(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
        {
            return 0;
        }
        break;
    case WM_CLOSE:
        if (ConfirmSaveUnsavedChanges())
        {
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        g_mainThreadId = std::this_thread::get_id();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"TerrainEditorWindow";
        RegisterClassExW(&wc);

        LoadSavedWindowSize();
        RECT rect{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        const std::wstring windowTitle = L"Terrain Editor";
        g_hwnd = CreateWindowW(wc.lpszClassName, windowTitle.c_str(), WS_OVERLAPPEDWINDOW, 100, 100, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_hwnd)
        {
            throw std::runtime_error("CreateWindow failed");
        }
        InitD3D(g_hwnd);
        terrain::gpu::GpuComputeContext gpuComputeContext{
            g_device.Get(),
            g_commandQueue.Get(),
            g_fence.Get(),
            &g_fenceLastSignaledValue,
            g_mainThreadId,
            [](UINT64 value) { WaitForFenceValue(value); },
        };
        terrain::gpu::SetMaskNoiseComputeContext({
            gpuComputeContext,
            ShaderPath("mask_noise_compute.hlsl"),
        });
        terrain::gpu::SetMaskUtilityComputeContext({
            gpuComputeContext,
            ShaderPath("mask_utility_compute.hlsl"),
        });
        terrain::gpu::SetColorizeComputeContext({
            gpuComputeContext,
            ShaderPath("colorize_compute.hlsl"),
        });
        terrain::gpu::SetSedimentComputeContext({
            gpuComputeContext,
            ShaderPath("sediment_compute.hlsl"),
        });
        terrain::gpu::SetRockComputeContext({
            gpuComputeContext,
            ShaderPath("rock_compute.hlsl"),
        });
        terrain::gpu::SetScatterComputeContext({
            gpuComputeContext,
            ShaderPath("scatter_compute.hlsl"),
        });
        terrain::gpu::SetMaskFluvialComputeContext({
            gpuComputeContext,
            ShaderPath("mask_fluvial_compute.hlsl"),
        });
        terrain::gpu::SetFluvialErosionComputeContext({
            gpuComputeContext,
            ShaderPath("fluvial_erosion_compute.hlsl"),
        });
        terrain::gpu::SetDropletErosionComputeContext({
            gpuComputeContext,
            ShaderPath("droplet_erosion_compute.hlsl"),
        });
        terrain::gpu::SetSnowComputeContext({
            gpuComputeContext,
            ShaderPath("snow_compute.hlsl"),
        });
        terrain::gpu::SetMseComputeContext({
            gpuComputeContext,
            ShaderPath("multi_scale_erosion_compute.hlsl"),
        });
        rock::SetMultiScaleErosionGpuEvaluator(RunMseComputeGrid);
        rock::SetMaskNoiseGpuEvaluator(RunMaskNoiseCompute);
        rock::SetMaskPathGpuEvaluator(RunMaskPathCompute);
        rock::SetMaskBlurGpuEvaluator(RunMaskBlurCompute);
        rock::SetHeightmapFromMaskGpuEvaluator(RunHeightmapFromMaskCompute);
        rock::SetSedimentGpuEvaluator(RunSedimentCompute);
        rock::SetRockGpuEvaluator(RunRockCompute);
        rock::SetScatterGpuEvaluator(RunScatterCompute);
        rock::SetMaskFluvialGpuEvaluator(RunMaskFluvialCompute);
        rock::SetFluvialErosionGpuEvaluator(RunFluvialErosionCompute);
        rock::SetDropletErosionGpuEvaluator(RunDropletErosionCompute);
        rock::SetSnowGpuEvaluator(RunSnowCompute);
        rock::SetSoilGpuEvaluator(RunSoilCompute);
        rock::SetColorizeGpuEvaluator(RunColorizeCompute);
        rock::SetAssetPathResolver(ResolveProjectAssetPath);
        SetPropertyWidgetCallbacks({
            [](const char* reason) { MarkGraphChanged(reason); },
            []() { BeginPropertyUndoEdit(); },
            []() { CommitPropertyUndoEdit(); },
            []() { PushUndoSnapshot(); },
            [](const std::string& currentPath) { return ShowHeightmapFileDialog(currentPath); },
            [](const std::filesystem::path& path) { return PathToUtf8(path); },
            [](const std::string& value) { return MakeProjectAssetPathForJson(value); },
        });
        SetNodePropertyCallbacks({
            []() { EvaluateGraph(); },
            [](const char* reason) { MarkGraphChanged(reason); },
            []() { return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; },
            []() { return (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0; },
            [](float& r, float& g, float& b) { SampleScreenPixel(r, g, b); },
            []() { SetForegroundWindow(g_hwnd); },
            [](rock::GraphId nodeId) -> rock::GraphId {
                const rock::Node* node = g_graph.FindNode(nodeId);
                if (node != nullptr &&
                    nodeId == g_selectedNodeId &&
                    g_pathSelectionKind == PathSelectionKind::Point &&
                    PathContainsPoint(*node, g_pathSelectedElementId))
                {
                    return g_pathSelectedElementId;
                }
                return 0;
            },
            [](rock::GraphId nodeId) -> rock::GraphId {
                const rock::Node* node = g_graph.FindNode(nodeId);
                if (node != nullptr &&
                    nodeId == g_selectedNodeId &&
                    g_pathSelectionKind == PathSelectionKind::Edge)
                {
                    const auto it = std::ranges::find_if(node->path.edges, [](const rock::PathEdge& edge) {
                        return edge.id == g_pathSelectedElementId;
                    });
                    return it != node->path.edges.end() ? g_pathSelectedElementId : 0;
                }
                return 0;
            },
        });

        ShowWindow(g_hwnd, showCommand);
        UpdateWindow(g_hwnd);
        UpdateWindowTitle();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        LoadJapaneseFont(io);
        g_themeManager.LoadThemes(AssetDirectory() / "ui_themes");
        g_themeManager.ApplyTheme("road_editor_dark");
        std::string appSettingsError;
        if (!LoadAppSettings(&appSettingsError) && !appSettingsError.empty())
        {
            SetProjectStatus("App settings load failed: " + appSettingsError);
        }
        ApplyLowDedicatedMemoryViewportSafety();
        EvaluateGraph();

        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX12_InitInfo dx12InitInfo{};
        dx12InitInfo.Device = g_device.Get();
        dx12InitInfo.CommandQueue = g_commandQueue.Get();
        dx12InitInfo.NumFramesInFlight = kFrameCount;
        dx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        dx12InitInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        dx12InitInfo.SrvDescriptorHeap = g_srvHeap.Get();
        dx12InitInfo.SrvDescriptorAllocFn = AllocateSrvDescriptor;
        dx12InitInfo.SrvDescriptorFreeFn = FreeSrvDescriptor;
        ImGui_ImplDX12_Init(&dx12InitInfo);

        ed::Config nodeEditorConfig{};
        nodeEditorConfig.SettingsFile = nullptr;
        nodeEditorConfig.NavigateButtonIndex = 2;
        g_nodeEditor = ed::CreateEditor(&nodeEditorConfig);

        MSG msg{};
        while (g_running)
        {
            const auto frameStart = std::chrono::steady_clock::now();
            g_frameTiming = {};
            g_frameTiming.frameRateLimitFps = ClampFrameRateLimitFps(g_ui.frameRateLimitFps);

            const auto messagePumpStart = std::chrono::steady_clock::now();
            while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT)
                {
                    g_running = false;
                }
            }
            const auto messagePumpEnd = std::chrono::steady_clock::now();
            g_frameTiming.messagePumpMs = std::chrono::duration<double, std::milli>(messagePumpEnd - messagePumpStart).count();

            if (!g_running)
            {
                break;
            }

            const bool windowForeground = (GetForegroundWindow() == g_hwnd);
            const bool windowMinimized = g_windowMinimized || IsIconic(g_hwnd);
            g_frameTiming.windowActive = g_windowActive;
            g_frameTiming.windowForeground = windowForeground;
            g_frameTiming.windowMinimized = windowMinimized;
            if ((!g_windowActive && !windowForeground) || windowMinimized)
            {
                g_frameTiming.backgroundThrottled = true;
                ProcessMainThreadEvaluationWork();
                const auto sleepStart = std::chrono::steady_clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                const auto sleepEnd = std::chrono::steady_clock::now();
                g_frameTiming.backgroundSleepMs = std::chrono::duration<double, std::milli>(sleepEnd - sleepStart).count();
                g_frameTiming.frameMs = std::chrono::duration<double, std::milli>(sleepEnd - frameStart).count();
                g_lastFrameTiming = g_frameTiming;
                continue;
            }

            const auto newFrameStart = std::chrono::steady_clock::now();
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            const auto newFrameEnd = std::chrono::steady_clock::now();
            g_frameTiming.newFrameMs = std::chrono::duration<double, std::milli>(newFrameEnd - newFrameStart).count();

            UpdateCloudLoopPhase(ImGui::GetIO().DeltaTime);
            UpdateSunTimeAnimation(ImGui::GetIO().DeltaTime);
            UpdateCameraAutoOrbit(ImGui::GetIO().DeltaTime);
            UpdateColorizeScreenPick(g_graph);
            const auto mainThreadWorkStart = std::chrono::steady_clock::now();
            ProcessMainThreadEvaluationWork();
            const auto mainThreadWorkEnd = std::chrono::steady_clock::now();
            g_frameTiming.mainThreadWorkMs = std::chrono::duration<double, std::milli>(mainThreadWorkEnd - mainThreadWorkStart).count();

            const auto drawUiStart = std::chrono::steady_clock::now();
            DrawUi();
            const auto drawUiEnd = std::chrono::steady_clock::now();
            g_frameTiming.drawUiMs = std::chrono::duration<double, std::milli>(drawUiEnd - drawUiStart).count();

            const auto imguiRenderStart = std::chrono::steady_clock::now();
            ImGui::Render();
            const auto imguiRenderEnd = std::chrono::steady_clock::now();
            g_frameTiming.imguiRenderMs = std::chrono::duration<double, std::milli>(imguiRenderEnd - imguiRenderStart).count();

            RenderFrame();
            ApplyFrameRateLimit(frameStart);
            const auto frameEnd = std::chrono::steady_clock::now();
            g_frameTiming.frameMs = std::chrono::duration<double, std::milli>(frameEnd - frameStart).count();
            g_lastFrameTiming = g_frameTiming;
        }

        WaitForAsyncEvaluationForShutdown();
        WaitForLastSubmittedFrame();
        SaveAppSettingsSilently();
        ed::DestroyEditor(g_nodeEditor);
        g_nodeEditor = nullptr;
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        rock::SetMultiScaleErosionGpuEvaluator(nullptr);
        rock::SetMaskNoiseGpuEvaluator(nullptr);
        rock::SetMaskPathGpuEvaluator(nullptr);
        rock::SetMaskBlurGpuEvaluator(nullptr);
        rock::SetHeightmapFromMaskGpuEvaluator(nullptr);
        rock::SetSedimentGpuEvaluator(nullptr);
        rock::SetRockGpuEvaluator(nullptr);
        rock::SetScatterGpuEvaluator(nullptr);
        rock::SetMaskFluvialGpuEvaluator(nullptr);
        rock::SetFluvialErosionGpuEvaluator(nullptr);
        rock::SetSnowGpuEvaluator(nullptr);
        rock::SetSoilGpuEvaluator(nullptr);
        rock::SetColorizeGpuEvaluator(nullptr);
        rock::SetAssetPathResolver(nullptr);
        SetPropertyWidgetCallbacks({});
        SetNodePropertyCallbacks({});
        CleanupD3D();
        DestroyWindow(g_hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
    catch (const std::exception& ex)
    {
        MessageBoxA(nullptr, ex.what(), "Terrain Editor Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    return 0;
}
