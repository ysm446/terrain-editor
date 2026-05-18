#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cfloat>
#include <cstdint>
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
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <nlohmann/json.hpp>

#include "node_graph.h"
#include "obj_exporter.h"
#include "resource.h"
#include "screenshot_capture.h"
#include "ui/UiTheme.h"
#include "Version.h"

using Microsoft::WRL::ComPtr;
namespace ed = ax::NodeEditor;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr int kFrameCount = 2;
constexpr int kSrvDescriptorCount = 128;
constexpr float kDegreesToRadians = 3.1415926535f / 180.0f;
constexpr float kFullFrameSensorHeightMm = 24.0f;
constexpr float kDefaultViewportYaw = 30.0f * kDegreesToRadians;
constexpr float kDefaultViewportPitch = 30.0f * kDegreesToRadians;
constexpr float kDefaultViewportFovDegrees = 45.0f;
constexpr float kDefaultViewportOrbitDistance = 2044.0f;
constexpr float kMaxViewportOrbitDistance = 100000.0f;
constexpr float kViewportFarPlane = 200000.0f;
constexpr std::array<int, 4> kTerrainSizePresets = {512, 1024, 2048, 4096};
constexpr std::array<int, 5> kResolutionPresets = {128, 256, 512, 1024, 2048};
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

HWND g_hwnd = nullptr;
UINT g_width = 1600;
UINT g_height = 900;
bool g_running = true;

// スクリーンカラーピッカー状態
// 2 モードを持つ:
//   SingleClick: 左クリック 1 回で選択中ストップの色を更新
//   Drag: ドラッグ中に収集した色列を間引き、線形にグラデーションへ投影
// どちらも SetCapture を使わないため他アプリ上の色も取得可能。
enum class ScreenPickMode
{
    Idle,
    DragArmed,     // Ctrl 押下待ち
    DragCollecting,// Ctrl 押しながらマウス移動でサンプリング中
};

struct ScreenColorPick
{
    ScreenPickMode mode = ScreenPickMode::Idle;
    rock::GraphId nodeId = 0;
    float previewR = 1.0f;
    float previewG = 1.0f;
    float previewB = 1.0f;
    bool prevCtrl = false;                        // Ctrl キーエッジ検出用
    std::vector<std::array<float, 3>> dragSamples;// Drag 用サンプル列 (RGB)
} g_screenPick;

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
ed::EditorContext* g_nodeEditor = nullptr;
bool g_nodeEditorFrameActive = false;
bool g_skipNodeMoveUndoThisFrame = false;
bool g_nodePositionsInitialized = false;
bool g_nodeGraphNavigatedToContent = false;
bool g_layoutSplitterActive = false;
rock::NodeGraph g_graph = rock::NodeGraph::CreateDefaultTerrainGraph();
std::string g_exportStatus = "No export yet";
std::string g_projectStatus = "No project file";
std::string g_lastEvaluationDuration = "Eval --";
bool g_projectSettingsHadSimulationResolution = false;
std::filesystem::path g_projectPath;
std::wstring g_windowTitle;
std::vector<std::filesystem::path> g_recentProjectPaths;
std::vector<std::pair<rock::GraphId, ImVec2>> g_pendingNodePositions;
std::vector<std::pair<rock::GraphId, ImVec2>> g_nodePositionCache;
std::vector<rock::GraphId> g_pendingSelectedNodeIds;
std::optional<std::vector<rock::GraphId>> g_pendingPreviewSelectionRestore;
rock::UiThemeManager g_themeManager;
rock::GraphId g_selectedNodeId = 0;
rock::GraphId g_pendingPreviewPinId = 0;

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
    bool showDrawStats = false;
    float rightPaneWidth = 0.0f;
    float nodePaneHeight = 0.0f;
};

UiState g_ui;

struct PreviewRenderStats
{
    uint32_t drawCalls = 0;
    uint32_t indexedDrawCalls = 0;
    uint64_t submittedVertices = 0;
    uint64_t submittedIndices = 0;
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
};

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
};

ViewportState g_viewport;

void NormalizeLoadedViewport(bool migrateCloseOrbitDistance)
{
    g_viewport.pitch = std::clamp(g_viewport.pitch, -1.25f, 1.25f);
    g_viewport.fovDegrees = std::clamp(g_viewport.fovDegrees, 15.0f, 90.0f);
    g_viewport.orbitDistance = std::clamp(g_viewport.orbitDistance, 1.0f, kMaxViewportOrbitDistance);
    if (migrateCloseOrbitDistance && g_viewport.orbitDistance <= 40.0f)
    {
        g_viewport.pitch = kDefaultViewportPitch;
        g_viewport.orbitDistance = kDefaultViewportOrbitDistance;
    }
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

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct CameraBasis
{
    Vec3 position;
    Vec3 right;
    Vec3 up;
    Vec3 forward;
};

struct ProjectedPoint
{
    ImVec2 screen;
    float depth = 0.0f;
};

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
    float padding2;
};

static_assert(offsetof(MeshPreviewConstants, lightRight) == 160);
static_assert(offsetof(MeshPreviewConstants, lightUp) == 176);
static_assert(offsetof(MeshPreviewConstants, lightForward) == 192);
static_assert(offsetof(MeshPreviewConstants, lightCenter) == 208);
static_assert(offsetof(MeshPreviewConstants, lightWorldRadius) == 224);
static_assert(offsetof(MeshPreviewConstants, padding2) == 236);
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
    float cloudShadowPadA;
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
    float pad0;
    float pad1;
};
static_assert(sizeof(CloudShadowMeshConstants) == 128);

struct GpuMeshPreview
{
    int width = 0;
    int height = 0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovDegrees = 0.0f;
    float orbitDistance = 0.0f;
    ImVec2 pan = ImVec2(0.0f, 0.0f);
    uint64_t graphVersion = UINT64_MAX;
    bool showSurface = false;
    bool showWireframe = false;
    bool showGrid = false;
    bool maskPreview = false;
    int maskShading = -1;
    int terrainBoundaryMode = -1;
    int lightingMode = 0;
    float sunAzimuthDegrees = 0.0f;
    float sunElevationDegrees = 0.0f;
    float sunIntensity = 0.0f;
    float ambientStrength = 0.0f;
    float shadowStrength = 0.0f;
    int shadowMapResolution = 0;
    float shadowBias = 0.0f;
    std::array<float, 3> pbrAlbedo = {};
    std::array<float, 3> gridColor = {};
    ComPtr<ID3D12Resource> colorTarget;
    ComPtr<ID3D12Resource> postTarget;
    ComPtr<ID3D12Resource> depthTarget;
    ComPtr<ID3D12Resource> shadowTarget;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> edgeIndexBuffer;
    ComPtr<ID3D12Resource> gridVertexBuffer;
    ComPtr<ID3D12Resource> terrainBoundaryLineVertexBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE postRtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE postSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE postSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE depthSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu{};
    bool srvAllocated = false;
    bool postSrvAllocated = false;
    bool shadowSrvAllocated = false;
    bool depthSrvAllocated = false;
    UINT vertexCount = 0;
    UINT triIndexCount = 0;
    UINT edgeIndexCount = 0;
    UINT gridVertexCount = 0;
    UINT terrainBoundaryLineVertexCount = 0;
    uint64_t terrainBoundaryLineUploadKey = UINT64_MAX;
    int gridCellCount = 0;
    float gridCellSizeMeters = 0.0f;
    int skyMode = -1;
    float skyAtmosphereDensity = 0.0f;
    float skyMieStrength = 0.0f;
    float skyMieEccentricity = 0.0f;
    std::array<float, 3> skyGroundAlbedo = {};
    float skySunSizeDegrees = 0.0f;
    float skySunGlowStrength = 0.0f;
    int cloudsEnabled = -1;
    int cloudSeed = INT_MIN;
    float cloudCoverage = 0.0f;
    float cloudDensityMultiplier = 0.0f;
    float cloudAltitudeMin = 0.0f;
    float cloudAltitudeMax = 0.0f;
    float cloudHorizontalScale = 0.0f;
    float cloudAbsorption = 0.0f;
    std::array<float, 3> cloudColor = {};
    int cloudAnimate = -1;
    float cloudWindDirectionDegrees = 0.0f;
    float cloudWindSpeed = 0.0f;
    int cloudQualitySamples = 0;
    float cloudShadowStrength = 0.0f;
    int cloudShadowResolution = 0;
    int cloudShadowSamples = 0;
    float cloudFieldRadius = 0.0f;
    float cloudFieldFalloff = 0.0f;
    int cloudLightSamples = 0;
    float cloudLightStepMeters = 0.0f;
    float cloudPhaseEccentricity = 0.0f;

    // GPU vertex displacement (Phase 2). Heightfield + mask are uploaded
    // to textures each evaluation, while the static UV grid mesh (just
    // index buffers, no vertex data — VS reads SV_VertexID) is built once
    // per displacementMeshResolution change.
    int meshBackend = -1;  // cached PreviewSettings::meshBackend
    ComPtr<ID3D12Resource> displacementHeightTexture;
    ComPtr<ID3D12Resource> displacementMaskTexture;
    int displacementTextureResolution = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE displacementHeightSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE displacementHeightSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE displacementMaskSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE displacementMaskSrvGpu{};
    bool displacementSrvAllocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE meshResourceTableCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE meshResourceTableGpu{};
    bool meshResourceTableAllocated = false;
    ComPtr<ID3D12Resource> colorizeTexture;
    int colorizeTextureResolution = 0;
    uint64_t colorizeTextureUploadKey = 0;
    ComPtr<ID3D12Resource> displacementTriIndexBuffer;
    ComPtr<ID3D12Resource> displacementPatchIndexBuffer;
    ComPtr<ID3D12Resource> displacementSectionIndexBuffer;
    ComPtr<ID3D12Resource> displacementEdgeIndexBuffer;
    int displacementMeshResolution = 0;
    UINT displacementTriIndexCount = 0;
    UINT displacementPatchIndexCount = 0;
    UINT displacementSectionIndexCount = 0;
    UINT displacementEdgeIndexCount = 0;
    uint64_t displacementHeightUploadKey = 0;
    bool viewportTessellation = false;
    float tessellationMinFactor = 0.0f;
    float tessellationMaxFactor = 0.0f;
    float tessellationNearDistance = 0.0f;
    float tessellationFarDistance = 0.0f;
    bool depthOfFieldEnabled = false;
    float dofFStop = 0.0f;
    float dofFocusDistanceMeters = 0.0f;
    float dofSensorHeightMm = 0.0f;
    float dofMaxBlurPixels = 0.0f;
    int dofApertureShape = -1;
    int dofApertureBlades = 0;
    float dofApertureRotationDegrees = 0.0f;
    float dofHighlightBoost = 0.0f;
    PreviewRenderStats renderStats;

    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES postState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
};

ComPtr<ID3D12RootSignature> g_meshPreviewRootSignature;
ComPtr<ID3D12PipelineState> g_meshPreviewSurfacePso;
ComPtr<ID3D12PipelineState> g_meshPreviewWirePso;
ComPtr<ID3D12RootSignature> g_meshPreviewDisplacementRootSignature;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementSurfacePso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementShadowPso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementWirePso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementSectionPso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementSectionShadowPso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementSectionWirePso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementTessSurfacePso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementTessShadowPso;
ComPtr<ID3D12PipelineState> g_meshPreviewDisplacementTessWirePso;
// Persistent CBV upload buffer for the displacement path's mesh
// constants (the regular CPU mesh path keeps its 32BitConstants binding
// — it's a separate root signature, less invasive).
ComPtr<ID3D12Resource> g_meshPreviewDisplacementCbv;
ComPtr<ID3D12PipelineState> g_meshPreviewGridPso;
ComPtr<ID3D12PipelineState> g_meshPreviewShadowPso;
ComPtr<ID3D12RootSignature> g_mseComputeRootSignature;
ComPtr<ID3D12PipelineState> g_mseStreamPowerPso;
ComPtr<ID3D12PipelineState> g_mseThermalPso;
ComPtr<ID3D12PipelineState> g_mseDepositionPso;
ComPtr<ID3D12RootSignature> g_maskNoiseComputeRootSignature;
ComPtr<ID3D12PipelineState> g_maskNoisePso;
ComPtr<ID3D12RootSignature> g_sedimentComputeRootSignature;
ComPtr<ID3D12PipelineState> g_sedimentSetupPso;
ComPtr<ID3D12PipelineState> g_sedimentEmitPso;
ComPtr<ID3D12PipelineState> g_sedimentSweep1Pso;
ComPtr<ID3D12PipelineState> g_sedimentSweep2Pso;
ComPtr<ID3D12RootSignature> g_rockComputeRootSignature;
ComPtr<ID3D12PipelineState> g_rockComputePso;
ComPtr<ID3D12RootSignature> g_maskFluvialComputeRootSignature;
ComPtr<ID3D12PipelineState> g_mfPitFillPso;
ComPtr<ID3D12PipelineState> g_mfCommitHeightsPso;
ComPtr<ID3D12PipelineState> g_mfCopyInputHeightsPso;
ComPtr<ID3D12PipelineState> g_mfBlurHorizontalPso;
ComPtr<ID3D12PipelineState> g_mfBlurVerticalPso;
ComPtr<ID3D12PipelineState> g_mfComputeWeightsPso;
ComPtr<ID3D12PipelineState> g_mfAccumInitPso;
ComPtr<ID3D12PipelineState> g_mfAccumIterPso;
ComPtr<ID3D12PipelineState> g_mfMaxReducePso;
ComPtr<ID3D12PipelineState> g_mfToMaskLogPso;
ComPtr<ID3D12PipelineState> g_mfToMaskLinearPso;
ComPtr<ID3D12PipelineState> g_mfToMaskThresholdPso;
ComPtr<ID3D12RootSignature> g_snowComputeRootSignature;
ComPtr<ID3D12PipelineState> g_snowCopyInputHeightsPso;
ComPtr<ID3D12PipelineState> g_snowComputeThicknessPso;
ComPtr<ID3D12PipelineState> g_snowEnvelopeSmoothingPso;
ComPtr<ID3D12PipelineState> g_snowApplyPso;
ComPtr<ID3D12RootSignature> g_colorizeComputeRootSignature;
ComPtr<ID3D12PipelineState> g_colorizeComputePso;
ComPtr<ID3D12RootSignature> g_skyRootSignature;
ComPtr<ID3D12PipelineState> g_skyPso;
bool g_skyPipelineReady = false;
std::string g_skyPipelineStatus = "Sky pipeline not initialized";
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
ComPtr<ID3D12DescriptorHeap> g_meshPreviewRtvHeap;
ComPtr<ID3D12DescriptorHeap> g_meshPreviewDsvHeap;
GpuMeshPreview g_gpuMeshPreview;
std::string g_mseComputeStatus = "MSE GPU Compute not initialized";
bool g_mseComputeReady = false;
std::mutex g_mseComputeMutex;
std::mutex g_mseGpuRequestMutex;
std::string g_maskNoiseComputeStatus = "Mask Noise GPU Compute not initialized";
bool g_maskNoiseComputeReady = false;
std::mutex g_maskNoiseComputeMutex;
std::mutex g_maskNoiseGpuRequestMutex;
std::string g_sedimentComputeStatus = "Sediment GPU Compute not initialized";
bool g_sedimentComputeReady = false;
std::mutex g_sedimentComputeMutex;
std::mutex g_sedimentGpuRequestMutex;
std::string g_rockComputeStatus = "Rock GPU Compute not initialized";
bool g_rockComputeReady = false;
std::mutex g_rockComputeMutex;
std::mutex g_rockGpuRequestMutex;
std::string g_maskFluvialComputeStatus = "Mask Fluvial GPU Compute not initialized";
bool g_maskFluvialComputeReady = false;
std::mutex g_maskFluvialComputeMutex;
std::mutex g_maskFluvialGpuRequestMutex;
std::string g_snowComputeStatus = "Snow GPU Compute not initialized";
bool g_snowComputeReady = false;
std::mutex g_snowComputeMutex;
std::mutex g_snowGpuRequestMutex;
std::string g_colorizeComputeStatus = "Colorize GPU Compute not initialized";
bool g_colorizeComputeReady = false;
std::mutex g_colorizeComputeMutex;
std::mutex g_colorizeGpuRequestMutex;
std::thread::id g_mainThreadId;

struct MseGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct MseGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::MultiScaleErosionSettings settings;
    std::promise<MseGpuRequestResult> promise;
};

std::vector<std::shared_ptr<MseGpuRequest>> g_pendingMseGpuRequests;

struct MaskNoiseGpuRequestResult
{
    bool success = false;
    rock::MaskGrid grid;
    std::string error;
};

struct SedimentGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct SedimentGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::SedimentSettings settings;
    std::promise<SedimentGpuRequestResult> promise;
};

std::vector<std::shared_ptr<SedimentGpuRequest>> g_pendingSedimentGpuRequests;

struct RockGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct RockGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::RockSettings settings;
    std::promise<RockGpuRequestResult> promise;
};

std::vector<std::shared_ptr<RockGpuRequest>> g_pendingRockGpuRequests;

struct MaskFluvialGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct MaskFluvialGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::MaskFluvialSettings settings;
    std::promise<MaskFluvialGpuRequestResult> promise;
};

std::vector<std::shared_ptr<MaskFluvialGpuRequest>> g_pendingMaskFluvialGpuRequests;

struct SnowGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct SnowGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::SnowSettings settings;
    std::promise<SnowGpuRequestResult> promise;
};

std::vector<std::shared_ptr<SnowGpuRequest>> g_pendingSnowGpuRequests;

struct ColorizeGpuRequestResult
{
    bool success = false;
    rock::ColorGrid grid;
    std::string error;
};

struct ColorizeGpuRequest
{
    rock::ColorizeSettings settings;
    rock::MaskGrid gradientMask;
    rock::MaskGrid mask;
    rock::ColorGrid baseColor;
    bool hasMask = false;
    bool hasBaseColor = false;
    std::promise<ColorizeGpuRequestResult> promise;
};

std::vector<std::shared_ptr<ColorizeGpuRequest>> g_pendingColorizeGpuRequests;

struct MaskNoiseGpuRequest
{
    rock::MaskNoiseSettings settings;
    int resolution = 0;
    std::promise<MaskNoiseGpuRequestResult> promise;
};

std::vector<std::shared_ptr<MaskNoiseGpuRequest>> g_pendingMaskNoiseGpuRequests;

std::string MakeWindowTitleText()
{
    std::string title = "Terrain Editor v" + std::string(TERRAIN_EDITOR_VERSION_STRING) + " ";
    title += g_projectPath.empty() ? "Untitled" : g_projectPath.filename().string();
    return title;
}

std::wstring MakeWindowTitle()
{
    std::wstring title = L"Terrain Editor v" +
        std::to_wstring(TERRAIN_EDITOR_VERSION_MAJOR) + L"." +
        std::to_wstring(TERRAIN_EDITOR_VERSION_MINOR) + L"." +
        std::to_wstring(TERRAIN_EDITOR_VERSION_PATCH) + L" ";
    title += g_projectPath.empty() ? L"Untitled" : g_projectPath.filename().wstring();
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

void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(message);
    }
}

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferResourceDesc(UINT64 byteSize, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = byteSize;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

D3D12_RESOURCE_DESC Texture2DResourceDesc(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
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

    ThrowIfFailed(g_fence->SetEventOnCompletion(value, g_fenceEvent), "SetEventOnCompletion failed");
    WaitForSingleObject(g_fenceEvent, INFINITE);
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
            break;
        }
    }

    if (!g_device)
    {
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice failed");
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

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kFrameCount;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)), "CreateDescriptorHeap RTV failed");
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = kSrvDescriptorCount;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
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
    g_gpuMeshPreview.colorTarget.Reset();
    g_gpuMeshPreview.postTarget.Reset();
    g_gpuMeshPreview.depthTarget.Reset();
    g_gpuMeshPreview.vertexBuffer.Reset();
    g_gpuMeshPreview.indexBuffer.Reset();
    g_gpuMeshPreview.edgeIndexBuffer.Reset();
    g_gpuMeshPreview.gridVertexBuffer.Reset();
    g_gpuMeshPreview.terrainBoundaryLineVertexBuffer.Reset();
    g_gpuMeshPreview.gridVertexCount = 0;
    g_gpuMeshPreview.terrainBoundaryLineVertexCount = 0;
    g_gpuMeshPreview.terrainBoundaryLineUploadKey = UINT64_MAX;
    g_meshPreviewSurfacePso.Reset();
    g_meshPreviewWirePso.Reset();
    g_meshPreviewGridPso.Reset();
    g_meshPreviewRootSignature.Reset();
    g_meshPreviewDisplacementSurfacePso.Reset();
    g_meshPreviewDisplacementShadowPso.Reset();
    g_meshPreviewDisplacementWirePso.Reset();
    g_meshPreviewDisplacementSectionPso.Reset();
    g_meshPreviewDisplacementSectionShadowPso.Reset();
    g_meshPreviewDisplacementSectionWirePso.Reset();
    g_meshPreviewDisplacementTessSurfacePso.Reset();
    g_meshPreviewDisplacementTessShadowPso.Reset();
    g_meshPreviewDisplacementTessWirePso.Reset();
    g_meshPreviewDisplacementRootSignature.Reset();
    g_meshPreviewDisplacementCbv.Reset();
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
    g_mseStreamPowerPso.Reset();
    g_mseThermalPso.Reset();
    g_mseDepositionPso.Reset();
    g_mseComputeRootSignature.Reset();
    g_mseComputeReady = false;
    g_maskNoisePso.Reset();
    g_maskNoiseComputeRootSignature.Reset();
    g_maskNoiseComputeReady = false;
    g_sedimentSetupPso.Reset();
    g_sedimentEmitPso.Reset();
    g_sedimentSweep1Pso.Reset();
    g_sedimentSweep2Pso.Reset();
    g_sedimentComputeRootSignature.Reset();
    g_sedimentComputeReady = false;
    g_rockComputePso.Reset();
    g_rockComputeRootSignature.Reset();
    g_rockComputeReady = false;
    g_mfPitFillPso.Reset();
    g_mfCommitHeightsPso.Reset();
    g_mfCopyInputHeightsPso.Reset();
    g_mfBlurHorizontalPso.Reset();
    g_mfBlurVerticalPso.Reset();
    g_mfComputeWeightsPso.Reset();
    g_mfAccumInitPso.Reset();
    g_mfAccumIterPso.Reset();
    g_mfMaxReducePso.Reset();
    g_mfToMaskLogPso.Reset();
    g_mfToMaskLinearPso.Reset();
    g_mfToMaskThresholdPso.Reset();
    g_maskFluvialComputeRootSignature.Reset();
    g_maskFluvialComputeReady = false;
    g_snowCopyInputHeightsPso.Reset();
    g_snowComputeThicknessPso.Reset();
    g_snowEnvelopeSmoothingPso.Reset();
    g_snowApplyPso.Reset();
    g_snowComputeRootSignature.Reset();
    g_snowComputeReady = false;
    g_colorizeComputePso.Reset();
    g_colorizeComputeRootSignature.Reset();
    g_colorizeComputeReady = false;
    g_skyPso.Reset();
    g_skyRootSignature.Reset();
    g_skyPipelineReady = false;
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
    g_dofPso.Reset();
    g_dofRootSignature.Reset();
    g_dofPipelineReady = false;
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
    g_meshPreviewRtvHeap.Reset();
    g_meshPreviewDsvHeap.Reset();
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

std::filesystem::path MseComputeShaderPath()
{
    return ShaderPath("multi_scale_erosion_compute.hlsl");
}

std::filesystem::path MaskNoiseShaderPath()
{
    return ShaderPath("mask_noise_compute.hlsl");
}

std::filesystem::path SedimentComputeShaderPath()
{
    return ShaderPath("sediment_compute.hlsl");
}

std::filesystem::path RockComputeShaderPath()
{
    return ShaderPath("rock_compute.hlsl");
}

std::filesystem::path MaskFluvialComputeShaderPath()
{
    return ShaderPath("mask_fluvial_compute.hlsl");
}

std::filesystem::path SnowComputeShaderPath()
{
    return ShaderPath("snow_compute.hlsl");
}

std::filesystem::path ColorizeComputeShaderPath()
{
    return ShaderPath("colorize_compute.hlsl");
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

void EvaluateGraph();
void ProcessPendingMseGpuRequests();
void ProcessPendingMaskNoiseGpuRequests();
void ProcessPendingSedimentGpuRequests();
void ProcessPendingRockGpuRequests();
void ProcessPendingMaskFluvialGpuRequests();
void ProcessPendingSnowGpuRequests();
void ProcessPendingColorizeGpuRequests();
void EnsurePreviewMesh();
int CurrentPreviewMeshResolution();
bool IsTerrainNodeKind(rock::NodeKind kind);
void ResetViewport();
void UpdateMapViewportInteraction(const ImVec2& min, const ImVec2& max);
ImVec2 InitialNodePosition(rock::NodeKind kind);

std::optional<std::filesystem::path> ShowProjectFileDialog(bool save)
{
    wchar_t fileName[MAX_PATH]{};
    if (!g_projectPath.empty())
    {
        const std::wstring current = g_projectPath.wstring();
        wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Terrain Editor Project (*.terrainproj)\0*.terrainproj\0Legacy Rock Generator Project (*.rockproj)\0*.rockproj\0JSON (*.json)\0*.json\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"terrainproj";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (save)
    {
        ofn.Flags |= OFN_OVERWRITEPROMPT;
        if (!GetSaveFileNameW(&ofn))
        {
            return std::nullopt;
        }
    }
    else
    {
        ofn.Flags |= OFN_FILEMUSTEXIST;
        if (!GetOpenFileNameW(&ofn))
        {
            return std::nullopt;
        }
    }

    return std::filesystem::path(fileName);
}

std::optional<std::filesystem::path> ShowHeightmapFileDialog(const std::string& currentPath)
{
    wchar_t fileName[MAX_PATH]{};
    if (!currentPath.empty())
    {
        const std::wstring current = std::filesystem::path(currentPath).wstring();
        wcsncpy_s(fileName, current.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Heightmap Images (*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp)\0*.png;*.jpg;*.jpeg;*.tif;*.tiff;*.bmp\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
    {
        return std::nullopt;
    }

    return std::filesystem::path(fileName);
}

std::string PathToUtf8(const std::filesystem::path& path)
{
    const std::u8string value = path.u8string();
    return std::string(value.begin(), value.end());
}

std::filesystem::path PathFromUtf8(const std::string& value)
{
    const std::u8string utf8(value.begin(), value.end());
    return std::filesystem::path(utf8);
}

std::filesystem::path ScreenshotDirectory()
{
    if (!g_projectPath.empty())
    {
        const std::filesystem::path parent = g_projectPath.parent_path();
        if (!parent.empty())
        {
            return parent / "screenshots";
        }
    }
    return std::filesystem::current_path() / "screenshots";
}

void RevealFileInExplorer(const std::filesystem::path& path)
{
    if (path.empty())
    {
        return;
    }

    const std::wstring args = L"/select,\"" + std::filesystem::absolute(path).wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void OpenFolderInExplorer(const std::filesystem::path& folder)
{
    if (folder.empty())
    {
        return;
    }

    ShellExecuteW(nullptr, L"open", std::filesystem::absolute(folder).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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

std::filesystem::path NormalizedProjectPath(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
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
        root["previewVisibility"] = {
            {"mesh", g_ui.meshPreview},
            {"fps", g_ui.showFps},
            {"drawStats", g_ui.showDrawStats},
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
            {"sunAzimuthDegrees", settings.preview.sunAzimuthDegrees},
            {"sunElevationDegrees", settings.preview.sunElevationDegrees},
            {"sunIntensity", settings.preview.sunIntensity},
            {"ambientStrength", settings.preview.ambientStrength},
            {"shadowStrength", settings.preview.shadowStrength},
            {"shadowMapResolution", settings.preview.shadowMapResolution},
            {"shadowBias", settings.preview.shadowBias},
            {"sunDirectionMode", static_cast<int>(settings.preview.sunDirectionMode)},
            {"sunLatitudeDegrees", settings.preview.sunLatitudeDegrees},
            {"sunLongitudeDegrees", settings.preview.sunLongitudeDegrees},
            {"sunUtcOffsetHours", settings.preview.sunUtcOffsetHours},
            {"sunMonth", settings.preview.sunMonth},
            {"sunDay", settings.preview.sunDay},
            {"sunTimeHours", settings.preview.sunTimeHours},
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
        g_projectStatus = "App settings save failed: " + error;
    }
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

        rock::GraphSettings& settings = g_graph.Settings();

        const nlohmann::json visibilityJson = root.value("previewVisibility", nlohmann::json::object());
        g_ui.meshPreview = visibilityJson.value("mesh", g_ui.meshPreview);
        g_ui.showFps = visibilityJson.value("fps", g_ui.showFps);
        g_ui.showDrawStats = visibilityJson.value("drawStats", g_ui.showDrawStats);
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
        settings.preview.sunAzimuthDegrees = std::clamp(visibilityJson.value("sunAzimuthDegrees", settings.preview.sunAzimuthDegrees), 0.0f, 360.0f);
        settings.preview.sunElevationDegrees = std::clamp(visibilityJson.value("sunElevationDegrees", settings.preview.sunElevationDegrees), -10.0f, 89.0f);
        settings.preview.sunIntensity = std::clamp(visibilityJson.value("sunIntensity", settings.preview.sunIntensity), 0.0f, 5.0f);
        settings.preview.ambientStrength = std::clamp(visibilityJson.value("ambientStrength", settings.preview.ambientStrength), 0.0f, 2.0f);
        settings.preview.shadowStrength = std::clamp(visibilityJson.value("shadowStrength", settings.preview.shadowStrength), 0.0f, 1.0f);
        settings.preview.shadowMapResolution = NearestShadowResolutionPreset(visibilityJson.value("shadowMapResolution", settings.preview.shadowMapResolution));
        settings.preview.shadowBias = std::clamp(visibilityJson.value("shadowBias", settings.preview.shadowBias), 0.0f, 0.05f);
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

        g_projectStatus = "Loaded app settings " + PathToUtf8(path);
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
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
    g_projectStatus = "Undo";
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
    g_projectStatus = "Redo";
}

void NewProject()
{
    ClearUndoHistory();
    g_graph = rock::NodeGraph::CreateDefaultTerrainGraph();
    g_projectPath.clear();
    UpdateWindowTitle();
    g_projectStatus = "New project";
    g_exportStatus = "No export yet";
    ResetViewport();
    ResetNodeEditorViewToDefault();
    EvaluateGraph();
}

nlohmann::json MakeBasicHeightfieldSettingsJson(const rock::Node& node)
{
    return {
        {"heightmap", {
            {"path", node.heightmap.path},
            {"scaleMeters", node.heightmap.scaleMeters},
            {"relativeVerticalScalePercent", node.heightmap.relativeVerticalScalePercent},
            {"verticalOffsetMeters", node.heightmap.verticalOffsetMeters},
        }},
        {"shape", {
            {"kind", static_cast<int>(node.shape.kind)},
            {"scaleMeters", node.shape.scaleMeters},
            {"relativeHeightPercent", node.shape.relativeHeightPercent},
        }},
        {"heightmapBlur", {
            {"radius", node.heightmapBlur.radius},
            {"strength", node.heightmapBlur.strength},
            {"iterations", node.heightmapBlur.iterations},
        }},
    };
}

nlohmann::json MakeMultiScaleErosionSettingsJson(const rock::Node& node)
{
    return {
        {"multiScaleErosion", {
            {"iterations", node.multiScaleErosion.iterations},
            {"enableStreamPower", node.multiScaleErosion.enableStreamPower},
            {"enableThermal", node.multiScaleErosion.enableThermal},
            {"enableDeposition", node.multiScaleErosion.enableDeposition},
            {"speStrength", node.multiScaleErosion.speStrength},
            {"streamExponent", node.multiScaleErosion.streamExponent},
            {"slopeExponent", node.multiScaleErosion.slopeExponent},
            {"maxStreamPower", node.multiScaleErosion.maxStreamPower},
            {"flowExponent", node.multiScaleErosion.flowExponent},
            {"speTimeStep", node.multiScaleErosion.speTimeStep},
            {"thermalAngleDegrees", node.multiScaleErosion.thermalAngleDegrees},
            {"thermalStrength", node.multiScaleErosion.thermalStrength},
            {"thermalNoisifyAngle", node.multiScaleErosion.thermalNoisifyAngle},
            {"thermalNoiseMin", node.multiScaleErosion.thermalNoiseMin},
            {"thermalNoiseMax", node.multiScaleErosion.thermalNoiseMax},
            {"thermalNoiseWavelength", node.multiScaleErosion.thermalNoiseWavelength},
            {"depositionStrength", node.multiScaleErosion.depositionStrength},
            {"rain", node.multiScaleErosion.rain},
            {"useMultigrid", node.multiScaleErosion.useMultigrid},
            {"backend", static_cast<int>(node.multiScaleErosion.backend)},
        }},
    };
}

nlohmann::json MakeMaskSettingsJson(const rock::Node& node)
{
    return {
        {"maskNoise", {
            {"seed", node.maskNoise.seed},
            {"octaves", node.maskNoise.octaves},
            {"frequency", node.maskNoise.frequency},
            {"lacunarity", node.maskNoise.lacunarity},
            {"persistence", node.maskNoise.persistence},
            {"backend", static_cast<int>(node.maskNoise.backend)},
        }},
        {"maskFluvial", {
            {"simulationMode", static_cast<int>(node.maskFluvial.simulationMode)},
            {"algorithm", static_cast<int>(node.maskFluvial.algorithm)},
            {"outputCurve", static_cast<int>(node.maskFluvial.outputCurve)},
            {"accumulationThreshold", node.maskFluvial.accumulationThreshold},
            {"gamma", node.maskFluvial.gamma},
            {"softness", node.maskFluvial.softness},
            {"power", node.maskFluvial.power},
            {"largestDetailLevelM", node.maskFluvial.largestDetailLevelM},
            {"mfdExponent", node.maskFluvial.mfdExponent},
            {"particleCount", node.maskFluvial.particleCount},
            {"particleLifetime", node.maskFluvial.particleLifetime},
            {"particleInertia", node.maskFluvial.particleInertia},
            {"particleStepLengthM", node.maskFluvial.particleStepLengthM},
            {"particleSeed", node.maskFluvial.particleSeed},
            {"backend", static_cast<int>(node.maskFluvial.backend)},
        }},
        {"maskCurvature", {
            {"mode", static_cast<int>(node.maskCurvature.mode)},
            {"radius", node.maskCurvature.radius},
            {"sensitivityMeters", node.maskCurvature.sensitivityMeters},
            {"threshold", node.maskCurvature.threshold},
            {"gamma", node.maskCurvature.gamma},
        }},
        {"maskLevels", {
            {"blackPoint", node.maskLevels.blackPoint},
            {"whitePoint", node.maskLevels.whitePoint},
            {"gamma", node.maskLevels.gamma},
            {"invert", node.maskLevels.invert},
        }},
        {"maskSlope", {
            {"slopeMinDeg", node.maskSlope.slopeMinDeg},
            {"slopeMaxDeg", node.maskSlope.slopeMaxDeg},
            {"gamma", node.maskSlope.gamma},
            {"invert", node.maskSlope.invert},
        }},
        {"maskHeight", {
            {"useFullRange", node.maskHeight.useFullRange},
            {"heightMinMeters", node.maskHeight.heightMinMeters},
            {"heightMaxMeters", node.maskHeight.heightMaxMeters},
            {"featherMeters", node.maskHeight.featherMeters},
            {"gamma", node.maskHeight.gamma},
            {"invert", node.maskHeight.invert},
        }},
        {"maskBlend", {
            {"mode", static_cast<int>(node.maskBlend.mode)},
            {"intensity", node.maskBlend.intensity},
        }},
    };
}

nlohmann::json MakeSnowSettingsJson(const rock::Node& node)
{
    return {
        {"snow", {
            {"emissionAmount", node.snow.emissionAmount},
            {"slopeLimitMinDeg", node.snow.slopeLimitMinDeg},
            {"slopeLimitMaxDeg", node.snow.slopeLimitMaxDeg},
            {"maskMaxSnow", node.snow.maskMaxSnow},
            {"smoothingIterations", node.snow.smoothingIterations},
            {"largestDetailLevelM", node.snow.largestDetailLevelM},
            {"fillRadius", node.snow.fillRadius},
            {"backend", static_cast<int>(node.snow.backend)},
        }},
    };
}

nlohmann::json MakeColorizeSettingsJson(const rock::Node& node)
{
    nlohmann::json stopsArr = nlohmann::json::array();
    for (const rock::ColorStop& s : node.colorize.stops)
    {
        stopsArr.push_back({{"position", s.position}, {"r", s.r}, {"g", s.g}, {"b", s.b}});
    }
    return {{"colorize", {
        {"backend", static_cast<int>(node.colorize.backend)},
        {"stops", stopsArr},
    }}};
}

nlohmann::json MakeRockSettingsJson(const rock::Node& node)
{
    return {
        {"rock", {
            {"style", static_cast<int>(node.rock.style)},
            {"orientationRule", static_cast<int>(node.rock.orientationRule)},
            {"layerCount", node.rock.layerCount},
            {"seed", node.rock.seed},
            {"density", node.rock.density},
            {"coverage", node.rock.coverage},
            {"rockSizeMinM", node.rock.rockSizeMinM},
            {"rockSizeMaxM", node.rock.rockSizeMaxM},
            {"rockHeight", node.rock.rockHeight},
            {"heightJitter", node.rock.heightJitter},
            {"rotationVariation", node.rock.rotationVariation},
            {"aspectVariation", node.rock.aspectVariation},
            {"edgeSharpness", node.rock.edgeSharpness},
            {"bumpiness", node.rock.bumpiness},
            {"facetSharpness", node.rock.facetSharpness},
            {"facetScale", node.rock.facetScale},
            {"backend", static_cast<int>(node.rock.backend)},
        }},
    };
}

nlohmann::json MakeCrumblingSettingsJson(const rock::Node& node)
{
    return {
        {"crumbling", {
            {"physicsCount", node.crumbling.physicsCount},
            {"debrisAmount", node.crumbling.debrisAmount},
            {"debrisSizeMinM", node.crumbling.debrisSizeMinM},
            {"debrisSizeMaxM", node.crumbling.debrisSizeMaxM},
            {"style", static_cast<int>(node.crumbling.style)},
            {"gravity", node.crumbling.gravity},
            {"seed", node.crumbling.seed},
        }},
    };
}

nlohmann::json MakeSedimentSettingsJson(const rock::Node& node)
{
    return {
        {"sediment", {
            {"iterations", node.sediment.iterations},
            {"stabilizationIterations", node.sediment.stabilizationIterations},
            {"largestDetailLevelM", node.sediment.largestDetailLevelM},
            {"emissionAmountM", node.sediment.emissionAmountM},
            {"emissionTime", node.sediment.emissionTime},
            {"sedimentViscosity", node.sediment.sedimentViscosity},
            {"convertTerrainToSediment", node.sediment.convertTerrainToSediment},
            {"maskContrast", node.sediment.maskContrast},
            {"backend", static_cast<int>(node.sediment.backend)},
        }},
    };
}

nlohmann::json MakeNodeSettingsJson(const rock::Node& node)
{
    nlohmann::json nodeJson;
    nodeJson.update(MakeBasicHeightfieldSettingsJson(node));
    nodeJson.update(MakeMultiScaleErosionSettingsJson(node));
    nodeJson.update(MakeMaskSettingsJson(node));
    nodeJson.update(MakeCrumblingSettingsJson(node));
    nodeJson.update(MakeRockSettingsJson(node));
    nodeJson.update(MakeSedimentSettingsJson(node));
    nodeJson.update(MakeSnowSettingsJson(node));
    nodeJson.update(MakeColorizeSettingsJson(node));
    return nodeJson;
}

nlohmann::json MakeSerializedNodeJson(const rock::Node& node)
{
    nlohmann::json nodeJson = {
        {"id", node.id},
        {"kind", static_cast<int>(node.kind)},
        {"title", node.title},
        {"inputs", nlohmann::json::array()},
        {"outputs", nlohmann::json::array()},
    };
    nodeJson.update(MakeNodeSettingsJson(node));

    for (const rock::Pin& pin : node.inputs)
    {
        nodeJson["inputs"].push_back({
            {"id", pin.id},
            {"valueType", static_cast<int>(pin.valueType)},
            {"label", pin.label},
        });
    }
    for (const rock::Pin& pin : node.outputs)
    {
        nodeJson["outputs"].push_back({
            {"id", pin.id},
            {"valueType", static_cast<int>(pin.valueType)},
            {"label", pin.label},
        });
    }
    return nodeJson;
}

std::optional<rock::NodeKind> ReadSerializedNodeKind(const nlohmann::json& nodeJson)
{
    const int kindInt = nodeJson.value("kind", 0);
    const rock::NodeKind kind = static_cast<rock::NodeKind>(kindInt);
    if (!IsTerrainNodeKind(kind))
    {
        return std::nullopt;
    }
    return kind;
}

std::optional<rock::PreviewStage> ReadSerializedPreviewStage(const nlohmann::json& root)
{
    const int stageInt = root.value("previewStage", static_cast<int>(g_graph.Preview()));
    const rock::PreviewStage stage = static_cast<rock::PreviewStage>(stageInt);
    switch (stage)
    {
    case rock::PreviewStage::Graph:
    case rock::PreviewStage::HeightmapBlur:
    case rock::PreviewStage::Shape:
    case rock::PreviewStage::MultiScaleErosion:
    case rock::PreviewStage::MaskNoise:
    case rock::PreviewStage::MaskBlend:
    case rock::PreviewStage::MaskLevels:
    case rock::PreviewStage::MaskSlope:
    case rock::PreviewStage::MaskHeight:
    case rock::PreviewStage::Crumbling:
    case rock::PreviewStage::MaskCurvature:
    case rock::PreviewStage::MaskFluvial:
    case rock::PreviewStage::Rock:
    case rock::PreviewStage::Sediment:
    case rock::PreviewStage::Snow:
    case rock::PreviewStage::Colorize:
        return stage;
    default:
        return std::nullopt;
    }
}

void ReadBasicHeightfieldSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeHeightmapJson = nodeJson.value("heightmap", nlohmann::json::object());
    const nlohmann::json nodeShapeJson = nodeJson.value("shape", nlohmann::json::object());
    const nlohmann::json nodeBlurJson = nodeJson.value("heightmapBlur", nlohmann::json::object());
    node.heightmap.path = nodeHeightmapJson.value("path", node.heightmap.path);
    node.heightmap.scaleMeters = std::clamp(nodeHeightmapJson.value("scaleMeters", node.heightmap.scaleMeters), 1.0f, 1000000.0f);
    node.heightmap.relativeVerticalScalePercent = std::clamp(nodeHeightmapJson.value("relativeVerticalScalePercent", node.heightmap.relativeVerticalScalePercent), 0.0f, 10000.0f);
    node.heightmap.verticalOffsetMeters = std::clamp(nodeHeightmapJson.value("verticalOffsetMeters", node.heightmap.verticalOffsetMeters), -1000000.0f, 1000000.0f);
    node.heightmap.simulationResolution = NearestResolutionPreset(nodeHeightmapJson.value("simulationResolution", node.heightmap.simulationResolution));
    node.shape.kind = static_cast<rock::ShapeKind>(std::clamp(nodeShapeJson.value("kind", static_cast<int>(node.shape.kind)), 0, 1));
    node.shape.scaleMeters = std::clamp(nodeShapeJson.value("scaleMeters", node.shape.scaleMeters), 1.0f, 1000000.0f);
    node.shape.relativeHeightPercent = std::clamp(nodeShapeJson.value("relativeHeightPercent", node.shape.relativeHeightPercent), 0.0f, 10000.0f);
    node.shape.simulationResolution = NearestResolutionPreset(nodeShapeJson.value("simulationResolution", node.shape.simulationResolution));
    node.heightmapBlur.radius = std::clamp(nodeBlurJson.value("radius", node.heightmapBlur.radius), 0.0f, 128.0f);
    node.heightmapBlur.strength = std::clamp(nodeBlurJson.value("strength", node.heightmapBlur.strength), 0.0f, 1.0f);
    node.heightmapBlur.iterations = std::clamp(nodeBlurJson.value("iterations", node.heightmapBlur.iterations), 0, 64);
}

void ReadMultiScaleErosionSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeMultiScaleErosionJson = nodeJson.value("multiScaleErosion", nlohmann::json::object());

    node.multiScaleErosion.iterations = std::clamp(nodeMultiScaleErosionJson.value("iterations", node.multiScaleErosion.iterations), 0, 500);
    node.multiScaleErosion.enableStreamPower = nodeMultiScaleErosionJson.value("enableStreamPower", node.multiScaleErosion.enableStreamPower);
    node.multiScaleErosion.enableThermal = nodeMultiScaleErosionJson.value("enableThermal", node.multiScaleErosion.enableThermal);
    node.multiScaleErosion.enableDeposition = nodeMultiScaleErosionJson.value("enableDeposition", node.multiScaleErosion.enableDeposition);
    node.multiScaleErosion.speStrength = std::clamp(nodeMultiScaleErosionJson.value("speStrength", node.multiScaleErosion.speStrength), 0.0f, 0.01f);
    node.multiScaleErosion.streamExponent = std::clamp(nodeMultiScaleErosionJson.value("streamExponent", node.multiScaleErosion.streamExponent), 0.0f, 2.0f);
    node.multiScaleErosion.slopeExponent = std::clamp(nodeMultiScaleErosionJson.value("slopeExponent", node.multiScaleErosion.slopeExponent), 0.0f, 4.0f);
    node.multiScaleErosion.maxStreamPower = std::clamp(nodeMultiScaleErosionJson.value("maxStreamPower", node.multiScaleErosion.maxStreamPower), 1.0f, 1000000.0f);
    node.multiScaleErosion.flowExponent = std::clamp(nodeMultiScaleErosionJson.value("flowExponent", node.multiScaleErosion.flowExponent), 0.5f, 4.0f);
    node.multiScaleErosion.speTimeStep = std::clamp(nodeMultiScaleErosionJson.value("speTimeStep", node.multiScaleErosion.speTimeStep), 0.0f, 4.0f);
    node.multiScaleErosion.thermalAngleDegrees = std::clamp(nodeMultiScaleErosionJson.value("thermalAngleDegrees", node.multiScaleErosion.thermalAngleDegrees), 0.0f, 60.0f);
    node.multiScaleErosion.thermalStrength = std::clamp(nodeMultiScaleErosionJson.value("thermalStrength", node.multiScaleErosion.thermalStrength), 0.0f, 0.01f);
    node.multiScaleErosion.thermalNoisifyAngle = nodeMultiScaleErosionJson.value("thermalNoisifyAngle", node.multiScaleErosion.thermalNoisifyAngle);
    node.multiScaleErosion.thermalNoiseMin = std::clamp(nodeMultiScaleErosionJson.value("thermalNoiseMin", node.multiScaleErosion.thermalNoiseMin), 0.0f, 4.0f);
    node.multiScaleErosion.thermalNoiseMax = std::clamp(nodeMultiScaleErosionJson.value("thermalNoiseMax", node.multiScaleErosion.thermalNoiseMax), 0.0f, 4.0f);
    node.multiScaleErosion.thermalNoiseWavelength = std::clamp(nodeMultiScaleErosionJson.value("thermalNoiseWavelength", node.multiScaleErosion.thermalNoiseWavelength), 0.0f, 0.05f);
    node.multiScaleErosion.depositionStrength = std::clamp(nodeMultiScaleErosionJson.value("depositionStrength", node.multiScaleErosion.depositionStrength), 0.0f, 8.0f);
    node.multiScaleErosion.rain = std::clamp(nodeMultiScaleErosionJson.value("rain", node.multiScaleErosion.rain), 0.0f, 10.0f);
    node.multiScaleErosion.useMultigrid = nodeMultiScaleErosionJson.value("useMultigrid", node.multiScaleErosion.useMultigrid);
    {
        const int backendInt = std::clamp(nodeMultiScaleErosionJson.value("backend", static_cast<int>(node.multiScaleErosion.backend)),
                                           static_cast<int>(rock::MultiScaleErosionBackend::CpuReference),
                                           static_cast<int>(rock::MultiScaleErosionBackend::GpuCompute));
        node.multiScaleErosion.backend = static_cast<rock::MultiScaleErosionBackend>(backendInt);
    }
}

void ReadMaskSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeMaskNoiseJson = nodeJson.value("maskNoise", nlohmann::json::object());
    const nlohmann::json nodeMaskBlendJson = nodeJson.value("maskBlend", nlohmann::json::object());
    const nlohmann::json nodeMaskFluvialJson = nodeJson.value("maskFluvial", nlohmann::json::object());
    const nlohmann::json nodeMaskCurvatureJson = nodeJson.value("maskCurvature", nlohmann::json::object());
    const nlohmann::json nodeMaskLevelsJson = nodeJson.value("maskLevels", nlohmann::json::object());
    const nlohmann::json nodeMaskSlopeJson = nodeJson.value("maskSlope", nlohmann::json::object());
    const nlohmann::json nodeMaskHeightJson = nodeJson.value("maskHeight", nlohmann::json::object());

    node.maskNoise.seed = std::clamp(nodeMaskNoiseJson.value("seed", node.maskNoise.seed), 0, 999999);
    node.maskNoise.octaves = std::clamp(nodeMaskNoiseJson.value("octaves", node.maskNoise.octaves), 1, 12);
    node.maskNoise.frequency = std::clamp(nodeMaskNoiseJson.value("frequency", node.maskNoise.frequency), 0.0f, 256.0f);
    node.maskNoise.lacunarity = std::clamp(nodeMaskNoiseJson.value("lacunarity", node.maskNoise.lacunarity), 0.0f, 8.0f);
    node.maskNoise.persistence = std::clamp(nodeMaskNoiseJson.value("persistence", node.maskNoise.persistence), 0.0f, 1.0f);
    node.maskNoise.simulationResolution = NearestResolutionPreset(nodeMaskNoiseJson.value("simulationResolution", node.maskNoise.simulationResolution));
    {
        const int maskNoiseBackendInt = std::clamp(nodeMaskNoiseJson.value("backend", static_cast<int>(node.maskNoise.backend)),
                                                    static_cast<int>(rock::MaskNoiseBackend::CpuParallel),
                                                    static_cast<int>(rock::MaskNoiseBackend::GpuCompute));
        node.maskNoise.backend = static_cast<rock::MaskNoiseBackend>(maskNoiseBackendInt);
    }
    {
        const int modeInt = std::clamp(nodeMaskBlendJson.value("mode", static_cast<int>(node.maskBlend.mode)),
                                        static_cast<int>(rock::MaskBlendMode::Add),
                                        static_cast<int>(rock::MaskBlendMode::Max));
        node.maskBlend.mode = static_cast<rock::MaskBlendMode>(modeInt);
    }
    node.maskBlend.intensity = std::clamp(nodeMaskBlendJson.value("intensity", node.maskBlend.intensity), 0.0f, 1.0f);
    {
        const int modeInt = std::clamp(nodeMaskCurvatureJson.value("mode", static_cast<int>(node.maskCurvature.mode)),
                                        static_cast<int>(rock::MaskCurvatureMode::Ridges),
                                        static_cast<int>(rock::MaskCurvatureMode::Absolute));
        node.maskCurvature.mode = static_cast<rock::MaskCurvatureMode>(modeInt);
    }
    node.maskCurvature.radius = std::clamp(nodeMaskCurvatureJson.value("radius", node.maskCurvature.radius), 1, 64);
    node.maskCurvature.sensitivityMeters = std::clamp(nodeMaskCurvatureJson.value("sensitivityMeters", node.maskCurvature.sensitivityMeters), 0.001f, 1000.0f);
    node.maskCurvature.threshold = std::clamp(nodeMaskCurvatureJson.value("threshold", node.maskCurvature.threshold), 0.0f, 0.99f);
    node.maskCurvature.gamma = std::clamp(nodeMaskCurvatureJson.value("gamma", node.maskCurvature.gamma), 0.05f, 8.0f);
    node.maskLevels.blackPoint = std::clamp(nodeMaskLevelsJson.value("blackPoint", node.maskLevels.blackPoint), 0.0f, 1.0f);
    node.maskLevels.whitePoint = std::clamp(nodeMaskLevelsJson.value("whitePoint", node.maskLevels.whitePoint), 0.0f, 1.0f);
    node.maskLevels.gamma = std::clamp(nodeMaskLevelsJson.value("gamma", node.maskLevels.gamma), 0.05f, 8.0f);
    node.maskLevels.invert = nodeMaskLevelsJson.value("invert", node.maskLevels.invert);
    node.maskSlope.slopeMinDeg = std::clamp(nodeMaskSlopeJson.value("slopeMinDeg", node.maskSlope.slopeMinDeg), 0.0f, 89.9f);
    node.maskSlope.slopeMaxDeg = std::clamp(nodeMaskSlopeJson.value("slopeMaxDeg", node.maskSlope.slopeMaxDeg), 0.0f, 89.9f);
    if (node.maskSlope.slopeMaxDeg < node.maskSlope.slopeMinDeg)
    {
        std::swap(node.maskSlope.slopeMinDeg, node.maskSlope.slopeMaxDeg);
    }
    node.maskSlope.gamma = std::clamp(nodeMaskSlopeJson.value("gamma", node.maskSlope.gamma), 0.05f, 8.0f);
    node.maskSlope.invert = nodeMaskSlopeJson.value("invert", node.maskSlope.invert);
    node.maskHeight.useFullRange = nodeMaskHeightJson.value("useFullRange", node.maskHeight.useFullRange);
    node.maskHeight.heightMinMeters = std::clamp(nodeMaskHeightJson.value("heightMinMeters", node.maskHeight.heightMinMeters), -100000.0f, 100000.0f);
    node.maskHeight.heightMaxMeters = std::clamp(nodeMaskHeightJson.value("heightMaxMeters", node.maskHeight.heightMaxMeters), -100000.0f, 100000.0f);
    if (node.maskHeight.heightMaxMeters < node.maskHeight.heightMinMeters)
    {
        std::swap(node.maskHeight.heightMinMeters, node.maskHeight.heightMaxMeters);
    }
    node.maskHeight.featherMeters = std::clamp(nodeMaskHeightJson.value("featherMeters", node.maskHeight.featherMeters), 0.0f, 100000.0f);
    node.maskHeight.gamma = std::clamp(nodeMaskHeightJson.value("gamma", node.maskHeight.gamma), 0.05f, 8.0f);
    node.maskHeight.invert = nodeMaskHeightJson.value("invert", node.maskHeight.invert);
    {
        const int modeInt = std::clamp(nodeMaskFluvialJson.value("simulationMode", static_cast<int>(node.maskFluvial.simulationMode)),
                                       static_cast<int>(rock::MaskFluvialSimulationMode::FlowAccumulation),
                                       static_cast<int>(rock::MaskFluvialSimulationMode::Particles));
        node.maskFluvial.simulationMode = static_cast<rock::MaskFluvialSimulationMode>(modeInt);
    }
    {
        (void)nodeMaskFluvialJson.value("algorithm", static_cast<int>(node.maskFluvial.algorithm));
        node.maskFluvial.algorithm = rock::FlowAccumulationAlgorithm::MFD;
    }
    {
        const int curveInt = std::clamp(nodeMaskFluvialJson.value("outputCurve", static_cast<int>(node.maskFluvial.outputCurve)),
                                         static_cast<int>(rock::MaskFluvialOutputCurve::Log),
                                         static_cast<int>(rock::MaskFluvialOutputCurve::Linear));
        node.maskFluvial.outputCurve = static_cast<rock::MaskFluvialOutputCurve>(curveInt);
    }
    node.maskFluvial.accumulationThreshold = std::clamp(nodeMaskFluvialJson.value("accumulationThreshold", node.maskFluvial.accumulationThreshold), 0.0f, 1.0f);
    node.maskFluvial.gamma = std::clamp(nodeMaskFluvialJson.value("gamma", node.maskFluvial.gamma), 0.05f, 8.0f);
    node.maskFluvial.softness = std::clamp(nodeMaskFluvialJson.value("softness", node.maskFluvial.softness), 0.001f, 4.0f);
    node.maskFluvial.power = std::clamp(nodeMaskFluvialJson.value("power", node.maskFluvial.power), 0.1f, 8.0f);
    (void)nodeMaskFluvialJson.value("pitFillIterations", node.maskFluvial.pitFillIterations);
    node.maskFluvial.pitFillIterations = rock::MaskFluvialSettings{}.pitFillIterations;
    node.maskFluvial.largestDetailLevelM = std::clamp(nodeMaskFluvialJson.value("largestDetailLevelM", node.maskFluvial.largestDetailLevelM), 1.0f, 1024.0f);
    node.maskFluvial.mfdExponent = std::clamp(nodeMaskFluvialJson.value("mfdExponent", node.maskFluvial.mfdExponent), 0.1f, 16.0f);
    (void)nodeMaskFluvialJson.value("inertia", node.maskFluvial.inertia);
    node.maskFluvial.inertia = rock::MaskFluvialSettings{}.inertia;
    node.maskFluvial.particleCount = std::clamp(nodeMaskFluvialJson.value("particleCount", node.maskFluvial.particleCount), 1, 200000);
    node.maskFluvial.particleLifetime = std::clamp(nodeMaskFluvialJson.value("particleLifetime", node.maskFluvial.particleLifetime), 1, 2048);
    node.maskFluvial.particleInertia = std::clamp(nodeMaskFluvialJson.value("particleInertia", node.maskFluvial.particleInertia), 0.0f, 0.98f);
    node.maskFluvial.particleStepLengthM = std::clamp(nodeMaskFluvialJson.value("particleStepLengthM", node.maskFluvial.particleStepLengthM), 0.01f, 1024.0f);
    node.maskFluvial.particleSeed = std::clamp(nodeMaskFluvialJson.value("particleSeed", node.maskFluvial.particleSeed), 0, 999999);
    {
        const int backendInt = std::clamp(nodeMaskFluvialJson.value("backend", static_cast<int>(node.maskFluvial.backend)),
                                           static_cast<int>(rock::MaskFluvialBackend::CpuReference),
                                           static_cast<int>(rock::MaskFluvialBackend::GpuCompute));
        node.maskFluvial.backend = static_cast<rock::MaskFluvialBackend>(backendInt);
    }
}

void ReadRockSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeRockJson = nodeJson.value("rock", nlohmann::json::object());

    if (nodeRockJson.contains("style"))
    {
        const int styleInt = nodeRockJson.value("style", static_cast<int>(node.rock.style));
        node.rock.style = static_cast<rock::RockStyle>(std::clamp(styleInt,
            static_cast<int>(rock::RockStyle::Classic),
            static_cast<int>(rock::RockStyle::Shard)));
    }
    else
    {
        node.rock.style = rock::RockStyle::Classic;
    }
    {
        const int orientationInt = nodeRockJson.value("orientationRule", static_cast<int>(node.rock.orientationRule));
        node.rock.orientationRule = static_cast<rock::RockOrientationRule>(std::clamp(orientationInt,
            static_cast<int>(rock::RockOrientationRule::Flat),
            static_cast<int>(rock::RockOrientationRule::SlopeOriented)));
    }
    node.rock.layerCount = std::clamp(nodeRockJson.value("layerCount", node.rock.layerCount), 1, 8);
    node.rock.seed = std::clamp(nodeRockJson.value("seed", node.rock.seed), 0, 999999);
    node.rock.density = std::clamp(nodeRockJson.value("density", node.rock.density), 0.5f, 1000.0f);
    node.rock.coverage = std::clamp(nodeRockJson.value("coverage", node.rock.coverage), 0.0f, 1.0f);
    const float density = node.rock.density;
    const float legacyRockFill = nodeRockJson.value("rockFill", -1.0f);
    const float legacyRockSize = nodeRockJson.value("rockSize", -1.0f);
    const float legacyMinRatio = nodeRockJson.value("rockSizeMin", -1.0f);
    const float legacyMaxRatio = nodeRockJson.value("rockSizeMax", -1.0f);
    if (legacyRockFill > 0.0f)
    {
        node.rock.rockSizeMinM = legacyRockFill * density;
        node.rock.rockSizeMaxM = node.rock.rockSizeMinM;
    }
    else if (legacyRockSize > 0.0f)
    {
        node.rock.rockSizeMinM = legacyRockSize * density;
        node.rock.rockSizeMaxM = node.rock.rockSizeMinM;
    }
    else if (legacyMinRatio > 0.0f || legacyMaxRatio > 0.0f)
    {
        const float minR = (legacyMinRatio > 0.0f) ? legacyMinRatio : 0.7f;
        const float maxR = (legacyMaxRatio > 0.0f) ? legacyMaxRatio : 1.2f;
        node.rock.rockSizeMinM = minR * density;
        node.rock.rockSizeMaxM = maxR * density;
    }
    else
    {
        node.rock.rockSizeMinM = nodeRockJson.value("rockSizeMinM", node.rock.rockSizeMinM);
        node.rock.rockSizeMaxM = nodeRockJson.value("rockSizeMaxM", node.rock.rockSizeMaxM);
    }
    node.rock.rockSizeMinM = std::clamp(node.rock.rockSizeMinM, 0.1f, 200.0f);
    node.rock.rockSizeMaxM = std::clamp(std::max(node.rock.rockSizeMaxM, node.rock.rockSizeMinM), 0.1f, 200.0f);
    node.rock.rockHeight = std::clamp(nodeRockJson.value("rockHeight", node.rock.rockHeight), 0.0f, 100.0f);
    node.rock.heightJitter = std::clamp(nodeRockJson.value("heightJitter", node.rock.heightJitter), 0.0f, 1.0f);
    node.rock.rotationVariation = std::clamp(nodeRockJson.value("rotationVariation", node.rock.rotationVariation), 0.0f, 1.0f);
    node.rock.aspectVariation = std::clamp(nodeRockJson.value("aspectVariation", node.rock.aspectVariation), 0.0f, 1.0f);
    node.rock.edgeSharpness = std::clamp(nodeRockJson.value("edgeSharpness", node.rock.edgeSharpness), 0.0f, 1.0f);
    node.rock.bumpiness = std::clamp(nodeRockJson.value("bumpiness", node.rock.bumpiness), 0.0f, 1.0f);
    node.rock.facetSharpness = std::clamp(nodeRockJson.value("facetSharpness", node.rock.facetSharpness), 0.0f, 1.0f);
    node.rock.facetScale = std::clamp(nodeRockJson.value("facetScale", node.rock.facetScale), 0.5f, 8.0f);
    {
        const int backendInt = nodeRockJson.value("backend", static_cast<int>(node.rock.backend));
        node.rock.backend = static_cast<rock::RockBackend>(std::clamp(backendInt,
            static_cast<int>(rock::RockBackend::CpuReference),
            static_cast<int>(rock::RockBackend::GpuCompute)));
    }
}

void ReadCrumblingSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeCrumblingJson = nodeJson.value("crumbling", nlohmann::json::object());
    node.crumbling.physicsCount = std::clamp(nodeCrumblingJson.value("physicsCount", node.crumbling.physicsCount), 0, 512);
    node.crumbling.debrisAmount = std::clamp(nodeCrumblingJson.value("debrisAmount", node.crumbling.debrisAmount), 0.0f, 1.0f);
    node.crumbling.debrisSizeMinM = std::clamp(nodeCrumblingJson.value("debrisSizeMinM", node.crumbling.debrisSizeMinM), 0.1f, 1000.0f);
    node.crumbling.debrisSizeMaxM = std::clamp(nodeCrumblingJson.value("debrisSizeMaxM", node.crumbling.debrisSizeMaxM), 0.1f, 1000.0f);
    if (node.crumbling.debrisSizeMaxM < node.crumbling.debrisSizeMinM)
    {
        std::swap(node.crumbling.debrisSizeMinM, node.crumbling.debrisSizeMaxM);
    }
    {
        const int styleInt = std::clamp(nodeCrumblingJson.value("style", static_cast<int>(node.crumbling.style)),
            static_cast<int>(rock::RockStyle::Classic),
            static_cast<int>(rock::RockStyle::Shard));
        node.crumbling.style = static_cast<rock::RockStyle>(styleInt);
    }
    node.crumbling.gravity = std::clamp(nodeCrumblingJson.value("gravity", node.crumbling.gravity), 0.0f, 1.0f);
    node.crumbling.seed = std::clamp(nodeCrumblingJson.value("seed", node.crumbling.seed), 0, 999999);
}

void ReadSedimentSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeSedimentJson = nodeJson.value("sediment", nlohmann::json::object());

    node.sediment.iterations = std::clamp(nodeSedimentJson.value("iterations", node.sediment.iterations), 1, 1000);
    node.sediment.stabilizationIterations = std::clamp(nodeSedimentJson.value("stabilizationIterations", node.sediment.stabilizationIterations), 1, 32);
    node.sediment.largestDetailLevelM = std::clamp(nodeSedimentJson.value("largestDetailLevelM", node.sediment.largestDetailLevelM), 1.0f, 1024.0f);
    node.sediment.emissionAmountM = std::clamp(nodeSedimentJson.value("emissionAmountM", node.sediment.emissionAmountM), 0.0f, 1000.0f);
    node.sediment.emissionTime = std::clamp(nodeSedimentJson.value("emissionTime", node.sediment.emissionTime), 0.0f, 1.0f);
    node.sediment.sedimentViscosity = std::clamp(nodeSedimentJson.value("sedimentViscosity", node.sediment.sedimentViscosity), 0.0f, 1.0f);
    node.sediment.convertTerrainToSediment = nodeSedimentJson.value("convertTerrainToSediment", node.sediment.convertTerrainToSediment);
    node.sediment.maskContrast = std::clamp(nodeSedimentJson.value("maskContrast", node.sediment.maskContrast), 0.0f, 1.0f);
    {
        const int backendInt = nodeSedimentJson.value("backend", static_cast<int>(node.sediment.backend));
        node.sediment.backend = static_cast<rock::SedimentBackend>(std::clamp(backendInt,
            static_cast<int>(rock::SedimentBackend::CpuReference),
            static_cast<int>(rock::SedimentBackend::GpuCompute)));
    }
}

void ReadSnowSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json nodeSnowJson = nodeJson.value("snow", nlohmann::json::object());

    node.snow.emissionAmount = std::clamp(nodeSnowJson.value("emissionAmount", node.snow.emissionAmount), 0.0f, 100.0f);
    node.snow.slopeLimitMinDeg = std::clamp(nodeSnowJson.value("slopeLimitMinDeg", node.snow.slopeLimitMinDeg), 0.0f, 89.9f);
    node.snow.slopeLimitMaxDeg = std::clamp(std::max(nodeSnowJson.value("slopeLimitMaxDeg", node.snow.slopeLimitMaxDeg), node.snow.slopeLimitMinDeg), 0.0f, 89.9f);
    node.snow.maskMaxSnow = std::clamp(nodeSnowJson.value("maskMaxSnow", node.snow.maskMaxSnow), 0.001f, 1000.0f);
    node.snow.smoothingIterations = std::clamp(nodeSnowJson.value("smoothingIterations", node.snow.smoothingIterations), 0, 16);
    node.snow.largestDetailLevelM = std::clamp(nodeSnowJson.value("largestDetailLevelM", node.snow.largestDetailLevelM), 1.0f, 1024.0f);
    node.snow.fillRadius = std::clamp(nodeSnowJson.value("fillRadius", node.snow.fillRadius), 1, 8);
    {
        const int backendInt = std::clamp(nodeSnowJson.value("backend", static_cast<int>(node.snow.backend)),
                                           static_cast<int>(rock::SnowBackend::CpuReference),
                                           static_cast<int>(rock::SnowBackend::GpuCompute));
        node.snow.backend = static_cast<rock::SnowBackend>(backendInt);
    }
}

void ReadColorizeSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    const nlohmann::json colorizeJson = nodeJson.value("colorize", nlohmann::json::object());
    {
        const int backendInt = std::clamp(colorizeJson.value("backend", static_cast<int>(node.colorize.backend)),
                                          static_cast<int>(rock::ColorizeBackend::CpuParallel),
                                          static_cast<int>(rock::ColorizeBackend::GpuCompute));
        node.colorize.backend = static_cast<rock::ColorizeBackend>(backendInt);
    }
    if (!colorizeJson.contains("stops") || !colorizeJson["stops"].is_array())
    {
        return;
    }
    node.colorize.stops.clear();
    for (const auto& stopJson : colorizeJson["stops"])
    {
        rock::ColorStop s;
        s.position = std::clamp(stopJson.value("position", 0.0f), 0.0f, 1.0f);
        s.r = std::clamp(stopJson.value("r", 0.0f), 0.0f, 1.0f);
        s.g = std::clamp(stopJson.value("g", 0.0f), 0.0f, 1.0f);
        s.b = std::clamp(stopJson.value("b", 0.0f), 0.0f, 1.0f);
        node.colorize.stops.push_back(s);
    }
    // デフォルトに戻す (stops が空になった場合)
    if (node.colorize.stops.empty())
    {
        node.colorize.stops = {{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    }
}

void ReadNodeSettingsJson(const nlohmann::json& nodeJson, rock::Node& node)
{
    ReadBasicHeightfieldSettingsJson(nodeJson, node);
    ReadMultiScaleErosionSettingsJson(nodeJson, node);
    ReadMaskSettingsJson(nodeJson, node);
    ReadCrumblingSettingsJson(nodeJson, node);
    ReadRockSettingsJson(nodeJson, node);
    ReadSedimentSettingsJson(nodeJson, node);
    ReadSnowSettingsJson(nodeJson, node);
    ReadColorizeSettingsJson(nodeJson, node);
}

nlohmann::json MakeProjectSettingsJson()
{
    const rock::GraphSettings& graphSettings = g_graph.Settings();
    const rock::PreviewSettings& preview = graphSettings.preview;
    const rock::SkySettings& sky = graphSettings.sky;
    const rock::CloudSettings& clouds = graphSettings.clouds;
    const int displayMode = sky.mode == rock::SkyMode::Atmospheric
        ? 2
        : (preview.lightingMode >= 1 ? 1 : 0);

    return {
        {"display", {
            {"mode", displayMode},
            {"showFps", g_ui.showFps},
            {"cloudsEnabled", clouds.enabled},
        }},
        {"preview", {
            {"terrainSizeMeters", preview.terrainSizeMeters},
            {"simulationResolution", preview.simulationResolution},
            {"lightingMode", preview.lightingMode},
            {"meshBackend", static_cast<int>(preview.meshBackend)},
            {"terrainBoundaryMode", static_cast<int>(preview.terrainBoundaryMode)},
            {"viewportTessellation", preview.viewportTessellation},
            {"tessellationMinFactor", preview.tessellationMinFactor},
            {"tessellationMaxFactor", preview.tessellationMaxFactor},
            {"tessellationNearDistance", preview.tessellationNearDistance},
            {"tessellationFarDistance", preview.tessellationFarDistance},
            {"depthOfFieldEnabled", preview.depthOfFieldEnabled},
            {"dofFStop", preview.dofFStop},
            {"dofFocusDistanceMeters", preview.dofFocusDistanceMeters},
            {"dofSensorHeightMm", preview.dofSensorHeightMm},
            {"dofMaxBlurPixels", preview.dofMaxBlurPixels},
            {"dofApertureShape", preview.dofApertureShape},
            {"dofApertureBlades", preview.dofApertureBlades},
            {"dofApertureRotationDegrees", preview.dofApertureRotationDegrees},
            {"dofHighlightBoost", preview.dofHighlightBoost},
            {"sunAzimuthDegrees", preview.sunAzimuthDegrees},
            {"sunElevationDegrees", preview.sunElevationDegrees},
            {"sunIntensity", preview.sunIntensity},
            {"ambientStrength", preview.ambientStrength},
            {"shadowStrength", preview.shadowStrength},
            {"shadowMapResolution", preview.shadowMapResolution},
            {"shadowBias", preview.shadowBias},
            {"sunDirectionMode", static_cast<int>(preview.sunDirectionMode)},
            {"sunLatitudeDegrees", preview.sunLatitudeDegrees},
            {"sunLongitudeDegrees", preview.sunLongitudeDegrees},
            {"sunUtcOffsetHours", preview.sunUtcOffsetHours},
            {"sunMonth", preview.sunMonth},
            {"sunDay", preview.sunDay},
            {"sunTimeHours", preview.sunTimeHours},
            {"showGrid", preview.showGrid},
            {"gridCellCount", preview.gridCellCount},
            {"gridCellSizeMeters", preview.gridCellSizeMeters},
            {"gridColor", {
                preview.gridColor[0],
                preview.gridColor[1],
                preview.gridColor[2],
            }},
            {"maskPreviewUseNearestHeightmap", preview.maskPreviewUseNearestHeightmap},
        }},
        {"sky", {
            {"mode", static_cast<int>(sky.mode)},
            {"atmosphereDensity", sky.atmosphereDensity},
            {"mieStrength", sky.mieStrength},
            {"mieEccentricity", sky.mieEccentricity},
            {"groundAlbedo", {sky.groundAlbedo[0], sky.groundAlbedo[1], sky.groundAlbedo[2]}},
            {"sunSizeDegrees", sky.sunSizeDegrees},
            {"sunGlowStrength", sky.sunGlowStrength},
        }},
        {"clouds", {
            {"enabled", clouds.enabled},
            {"seed", clouds.seed},
            {"coverage", clouds.coverage},
            {"densityMultiplier", clouds.densityMultiplier},
            {"altitudeMin", clouds.altitudeMin},
            {"altitudeMax", clouds.altitudeMax},
            {"horizontalScale", clouds.horizontalScale},
            {"absorption", clouds.absorption},
            {"color", {clouds.color[0], clouds.color[1], clouds.color[2]}},
            {"animate", clouds.animate},
            {"windDirectionDegrees", clouds.windDirectionDegrees},
            {"windSpeedMetersPerSec", clouds.windSpeedMetersPerSec},
            {"qualitySamples", clouds.qualitySamples},
            {"shadowStrength", clouds.shadowStrength},
            {"shadowResolution", clouds.shadowResolution},
            {"shadowSamples", clouds.shadowSamples},
            {"fieldRadius", clouds.fieldRadius},
            {"fieldFalloff", clouds.fieldFalloff},
            {"lightSamples", clouds.lightSamples},
            {"lightStepMeters", clouds.lightStepMeters},
            {"phaseEccentricity", clouds.phaseEccentricity},
        }},
    };
}

nlohmann::json MakeViewportJson()
{
    return {
        {"yaw", g_viewport.yaw},
        {"pitch", g_viewport.pitch},
        {"fovDegrees", g_viewport.fovDegrees},
        {"orbitDistance", g_viewport.orbitDistance},
        {"pan", {g_viewport.pan.x, g_viewport.pan.y}},
    };
}

nlohmann::json MakeSerializedNodesJson()
{
    nlohmann::json nodesJson = nlohmann::json::array();
    for (const rock::Node& node : g_graph.Nodes())
    {
        nodesJson.push_back(MakeSerializedNodeJson(node));
    }
    return nodesJson;
}

nlohmann::json MakeSerializedLinksJson()
{
    nlohmann::json linksJson = nlohmann::json::array();
    for (const rock::Link& link : g_graph.Links())
    {
        linksJson.push_back({
            {"id", link.id},
            {"startPin", link.startPin},
            {"endPin", link.endPin},
        });
    }
    return linksJson;
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
        nlohmann::json root;
        root["format"] = "terrain_editor_project";
        root["formatVersion"] = 1;
        root["appVersion"] = TERRAIN_EDITOR_VERSION_STRING;
        WriteSelectedNodesJson(root);
        root["previewStage"] = static_cast<int>(g_graph.Preview());
        root["previewPinId"] = g_graph.Evaluation().previewPinId;
        root["settings"] = MakeProjectSettingsJson();

        root["nodeSettings"] = nlohmann::json::object();
        root["viewport"] = MakeViewportJson();
        root["nodes"] = MakeSerializedNodesJson();
        root["links"] = MakeSerializedLinksJson();
        root["nodePositions"] = MakeNodePositionsJson();

        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream stream(path);
        if (!stream)
        {
            if (error) *error = "Failed to open project for writing";
            return false;
        }
        stream << root.dump(2);
        g_projectPath = path;
        UpdateWindowTitle();
        AddRecentProjectPath(path);
        SaveAppSettingsSilently();
        g_projectStatus = "Saved " + PathToUtf8(path);
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

void SaveCurrentProject()
{
    const std::optional<std::filesystem::path> path =
        g_projectPath.empty() ? ShowProjectFileDialog(true) : std::optional<std::filesystem::path>(g_projectPath);
    if (!path)
    {
        return;
    }

    std::string error;
    if (!SaveProjectToFile(*path, &error))
    {
        g_projectStatus = "Save failed: " + error;
    }
}

void ReadColor3Json(const nlohmann::json& ownerJson, const char* key, std::array<float, 3>& target, float maxValue)
{
    if (ownerJson.contains(key) && ownerJson[key].is_array() && ownerJson[key].size() == 3)
    {
        target[0] = std::clamp(ownerJson[key][0].get<float>(), 0.0f, maxValue);
        target[1] = std::clamp(ownerJson[key][1].get<float>(), 0.0f, maxValue);
        target[2] = std::clamp(ownerJson[key][2].get<float>(), 0.0f, maxValue);
    }
}

void ReadSkySettingsJson(const nlohmann::json& settingsJson, rock::SkySettings& sky)
{
    const nlohmann::json skyJson = settingsJson.value("sky", nlohmann::json::object());
    sky = rock::SkySettings{};
    if (skyJson.empty())
    {
        return;
    }

    const int skyModeInt = std::clamp(skyJson.value("mode", static_cast<int>(sky.mode)),
                                      static_cast<int>(rock::SkyMode::SolidColor),
                                      static_cast<int>(rock::SkyMode::Atmospheric));
    sky.mode = static_cast<rock::SkyMode>(skyModeInt);
    sky.atmosphereDensity = std::clamp(skyJson.value("atmosphereDensity", sky.atmosphereDensity), 0.05f, 8.0f);
    sky.mieStrength = std::clamp(skyJson.value("mieStrength", sky.mieStrength), 0.0f, 8.0f);
    sky.mieEccentricity = std::clamp(skyJson.value("mieEccentricity", sky.mieEccentricity), -0.99f, 0.99f);
    ReadColor3Json(skyJson, "groundAlbedo", sky.groundAlbedo, 8.0f);
    sky.sunSizeDegrees = std::clamp(skyJson.value("sunSizeDegrees", sky.sunSizeDegrees), 0.1f, 30.0f);
    sky.sunGlowStrength = std::clamp(skyJson.value("sunGlowStrength", sky.sunGlowStrength), 0.0f, 4.0f);
}

void ReadCloudSettingsJson(const nlohmann::json& settingsJson, rock::CloudSettings& clouds)
{
    const nlohmann::json cloudsJson = settingsJson.value("clouds", nlohmann::json::object());
    clouds = rock::CloudSettings{};
    if (cloudsJson.empty())
    {
        return;
    }

    clouds.enabled = cloudsJson.value("enabled", clouds.enabled);
    clouds.seed = std::clamp(cloudsJson.value("seed", clouds.seed), 0, 999999);
    clouds.coverage = std::clamp(cloudsJson.value("coverage", clouds.coverage), 0.0f, 1.0f);
    clouds.densityMultiplier = std::clamp(cloudsJson.value("densityMultiplier", clouds.densityMultiplier), 0.0f, 8.0f);
    clouds.altitudeMin = std::clamp(cloudsJson.value("altitudeMin", clouds.altitudeMin), 0.0f, 30000.0f);
    clouds.altitudeMax = std::clamp(cloudsJson.value("altitudeMax", clouds.altitudeMax), 0.0f, 30000.0f);
    clouds.horizontalScale = std::clamp(cloudsJson.value("horizontalScale", clouds.horizontalScale), 50.0f, 100000.0f);
    clouds.absorption = std::clamp(cloudsJson.value("absorption", clouds.absorption), 0.0f, 2.0f);
    ReadColor3Json(cloudsJson, "color", clouds.color, 8.0f);
    const bool legacyAnimatedClouds = !cloudsJson.contains("animate") &&
        cloudsJson.value("windSpeedMetersPerSec", clouds.windSpeedMetersPerSec) > 0.0f;
    clouds.animate = cloudsJson.value("animate", legacyAnimatedClouds ? true : clouds.animate);
    clouds.windDirectionDegrees = std::clamp(cloudsJson.value("windDirectionDegrees", clouds.windDirectionDegrees), 0.0f, 360.0f);
    clouds.windSpeedMetersPerSec = std::clamp(cloudsJson.value("windSpeedMetersPerSec", clouds.windSpeedMetersPerSec), 0.0f, 500.0f);
    clouds.qualitySamples = std::clamp(cloudsJson.value("qualitySamples", clouds.qualitySamples), 8, 128);
    clouds.shadowStrength = std::clamp(cloudsJson.value("shadowStrength", clouds.shadowStrength), 0.0f, 1.0f);
    clouds.shadowResolution = NearestShadowResolutionPreset(cloudsJson.value("shadowResolution", clouds.shadowResolution));
    clouds.shadowSamples = std::clamp(cloudsJson.value("shadowSamples", clouds.shadowSamples), 4, 64);
    clouds.fieldRadius = std::clamp(cloudsJson.value("fieldRadius", clouds.fieldRadius), 100.0f, 200000.0f);
    clouds.fieldFalloff = std::clamp(cloudsJson.value("fieldFalloff", clouds.fieldFalloff), 1.0f, 50000.0f);
    clouds.lightSamples = std::clamp(cloudsJson.value("lightSamples", clouds.lightSamples), 0, 16);
    clouds.lightStepMeters = std::clamp(cloudsJson.value("lightStepMeters", clouds.lightStepMeters), 1.0f, 2000.0f);
    clouds.phaseEccentricity = std::clamp(cloudsJson.value("phaseEccentricity", clouds.phaseEccentricity), -0.99f, 0.99f);
}

void ReadPreviewSettingsJson(const nlohmann::json& settingsJson, rock::PreviewSettings& preview, const rock::SkySettings& sky)
{
    const nlohmann::json previewJson = settingsJson.value("preview", nlohmann::json::object());
    if (previewJson.empty())
    {
        if (sky.mode == rock::SkyMode::Atmospheric)
        {
            preview.lightingMode = 1;
        }
        return;
    }

    preview.terrainSizeMeters = static_cast<float>(NearestTerrainSizePreset(previewJson.value("terrainSizeMeters", preview.terrainSizeMeters)));
    preview.simulationResolution = NearestResolutionPreset(previewJson.value("simulationResolution", preview.simulationResolution));
    g_projectSettingsHadSimulationResolution = previewJson.contains("simulationResolution");
    preview.lightingMode = std::clamp(previewJson.value("lightingMode", preview.lightingMode), 0, 1);
    const int backendInt = std::clamp(previewJson.value("meshBackend", static_cast<int>(preview.meshBackend)),
                                      static_cast<int>(rock::MeshPreviewBackend::CpuMesh),
                                      static_cast<int>(rock::MeshPreviewBackend::GpuDisplacement));
    preview.meshBackend = static_cast<rock::MeshPreviewBackend>(backendInt);
    const int boundaryInt = std::clamp(previewJson.value("terrainBoundaryMode", static_cast<int>(preview.terrainBoundaryMode)),
                                      static_cast<int>(rock::TerrainBoundaryMode::None),
                                      static_cast<int>(rock::TerrainBoundaryMode::Lines));
    preview.terrainBoundaryMode = static_cast<rock::TerrainBoundaryMode>(boundaryInt);
    preview.viewportTessellation = previewJson.value("viewportTessellation", preview.viewportTessellation);
    preview.tessellationMinFactor = std::clamp(previewJson.value("tessellationMinFactor", preview.tessellationMinFactor), 1.0f, 64.0f);
    preview.tessellationMaxFactor = std::clamp(previewJson.value("tessellationMaxFactor", preview.tessellationMaxFactor), preview.tessellationMinFactor, 64.0f);
    preview.tessellationNearDistance = std::clamp(previewJson.value("tessellationNearDistance", preview.tessellationNearDistance), 1.0f, 100000.0f);
    preview.tessellationFarDistance = std::clamp(previewJson.value("tessellationFarDistance", preview.tessellationFarDistance), preview.tessellationNearDistance + 1.0f, 200000.0f);
    preview.depthOfFieldEnabled = previewJson.value("depthOfFieldEnabled", preview.depthOfFieldEnabled);
    preview.dofFStop = std::clamp(previewJson.value("dofFStop", preview.dofFStop), 0.7f, 32.0f);
    preview.dofFocusDistanceMeters = std::clamp(previewJson.value("dofFocusDistanceMeters", preview.dofFocusDistanceMeters), 0.1f, 20000.0f);
    preview.dofSensorHeightMm = std::clamp(previewJson.value("dofSensorHeightMm", preview.dofSensorHeightMm), 4.0f, 80.0f);
    preview.dofMaxBlurPixels = std::clamp(previewJson.value("dofMaxBlurPixels", preview.dofMaxBlurPixels), 0.0f, 64.0f);
    preview.dofApertureShape = std::clamp(previewJson.value("dofApertureShape", preview.dofApertureShape), 0, 4);
    preview.dofApertureBlades = std::clamp(previewJson.value("dofApertureBlades", preview.dofApertureBlades), 3, 12);
    preview.dofApertureRotationDegrees = std::clamp(previewJson.value("dofApertureRotationDegrees", preview.dofApertureRotationDegrees), -180.0f, 180.0f);
    preview.dofHighlightBoost = std::clamp(previewJson.value("dofHighlightBoost", preview.dofHighlightBoost), 0.0f, 4.0f);
    preview.sunAzimuthDegrees = std::clamp(previewJson.value("sunAzimuthDegrees", preview.sunAzimuthDegrees), 0.0f, 360.0f);
    preview.sunElevationDegrees = std::clamp(previewJson.value("sunElevationDegrees", preview.sunElevationDegrees), -10.0f, 89.0f);
    preview.sunIntensity = std::clamp(previewJson.value("sunIntensity", preview.sunIntensity), 0.0f, 5.0f);
    preview.ambientStrength = std::clamp(previewJson.value("ambientStrength", preview.ambientStrength), 0.0f, 2.0f);
    preview.shadowStrength = std::clamp(previewJson.value("shadowStrength", preview.shadowStrength), 0.0f, 1.0f);
    preview.shadowMapResolution = NearestShadowResolutionPreset(previewJson.value("shadowMapResolution", preview.shadowMapResolution));
    preview.shadowBias = std::clamp(previewJson.value("shadowBias", preview.shadowBias), 0.0f, 0.05f);
    {
        const int sunModeInt = std::clamp(previewJson.value("sunDirectionMode", static_cast<int>(preview.sunDirectionMode)),
            static_cast<int>(rock::SunDirectionMode::Manual),
            static_cast<int>(rock::SunDirectionMode::DateTime));
        preview.sunDirectionMode = static_cast<rock::SunDirectionMode>(sunModeInt);
    }
    preview.sunLatitudeDegrees = std::clamp(previewJson.value("sunLatitudeDegrees", preview.sunLatitudeDegrees), -90.0f, 90.0f);
    preview.sunLongitudeDegrees = std::clamp(previewJson.value("sunLongitudeDegrees", preview.sunLongitudeDegrees), -180.0f, 180.0f);
    preview.sunUtcOffsetHours = std::clamp(previewJson.value("sunUtcOffsetHours", preview.sunUtcOffsetHours), -12.0f, 14.0f);
    preview.sunMonth = std::clamp(previewJson.value("sunMonth", preview.sunMonth), 1, 12);
    preview.sunDay = std::clamp(previewJson.value("sunDay", preview.sunDay), 1, DaysInMonth(preview.sunMonth));
    preview.sunTimeHours = std::clamp(previewJson.value("sunTimeHours", preview.sunTimeHours), 0.0f, 24.0f);
    preview.showGrid = previewJson.value("showGrid", preview.showGrid);
    preview.gridCellCount = std::clamp(previewJson.value("gridCellCount", preview.gridCellCount), 1, 200);
    preview.gridCellSizeMeters = std::clamp(previewJson.value("gridCellSizeMeters", preview.gridCellSizeMeters), 1.0f, 10000.0f);
    ReadColor3Json(previewJson, "gridColor", preview.gridColor, 1.0f);
    preview.maskPreviewUseNearestHeightmap = previewJson.value("maskPreviewUseNearestHeightmap", preview.maskPreviewUseNearestHeightmap);
}

void ReadDisplaySettingsJson(const nlohmann::json& settingsJson,
                             rock::PreviewSettings& preview,
                             rock::SkySettings& sky,
                             rock::CloudSettings& clouds)
{
    const nlohmann::json displayJson = settingsJson.value("display", nlohmann::json::object());
    if (displayJson.empty())
    {
        return;
    }

    g_ui.showFps = displayJson.value("showFps", g_ui.showFps);
    clouds.enabled = displayJson.value("cloudsEnabled", clouds.enabled);
    const int displayMode = std::clamp(displayJson.value("mode", -1), -1, 2);
    if (displayMode == 0)
    {
        preview.lightingMode = 0;
        sky.mode = rock::SkyMode::SolidColor;
    }
    else if (displayMode == 1)
    {
        preview.lightingMode = 1;
        sky.mode = rock::SkyMode::SolidColor;
    }
    else if (displayMode == 2)
    {
        preview.lightingMode = 1;
        sky.mode = rock::SkyMode::Atmospheric;
    }
}

void ReadProjectSettingsJson(const nlohmann::json& root)
{
    const nlohmann::json settingsJson = root.value("settings", nlohmann::json::object());
    rock::GraphSettings& graphSettings = g_graph.Settings();
    rock::PreviewSettings& preview = graphSettings.preview;
    rock::SkySettings& sky = graphSettings.sky;
    rock::CloudSettings& clouds = graphSettings.clouds;

    g_projectSettingsHadSimulationResolution = false;
    ReadSkySettingsJson(settingsJson, sky);
    ReadCloudSettingsJson(settingsJson, clouds);
    ReadPreviewSettingsJson(settingsJson, preview, sky);
    ReadDisplaySettingsJson(settingsJson, preview, sky, clouds);
}

void ReadSerializedPinsJson(const nlohmann::json& pinsJson,
                            rock::GraphId nodeId,
                            rock::PinKind pinKind,
                            std::vector<rock::Pin>& pins)
{
    if (!pinsJson.is_array())
    {
        return;
    }

    for (const nlohmann::json& pinJson : pinsJson)
    {
        rock::Pin pin;
        pin.id = pinJson.value("id", 0);
        pin.nodeId = nodeId;
        pin.kind = pinKind;
        const int serializedValueType = pinJson.value("valueType", static_cast<int>(rock::ValueType::HeightField));
        if (serializedValueType == static_cast<int>(rock::ValueType::Mask))
            pin.valueType = rock::ValueType::Mask;
        else if (serializedValueType == static_cast<int>(rock::ValueType::ColorTexture))
            pin.valueType = rock::ValueType::ColorTexture;
        else
            pin.valueType = rock::ValueType::HeightField;
        pin.label = pinJson.value("label", std::string(rock::ToString(pin.valueType)));
        // 旧プロジェクトでは入力 / 出力どちらの heightfield ピンも `HeightField`
        // と保存されていた可能性があるが、現在は両方とも `Heightmap` に統一
        // しているのでマイグレーションする。
        if (pin.valueType == rock::ValueType::HeightField && pin.label == "HeightField")
        {
            pin.label = "Heightmap";
        }
        pins.push_back(std::move(pin));
    }
}

std::optional<rock::Node> ReadSerializedNodeJson(const nlohmann::json& nodeJson)
{
    rock::Node node;
    node.id = nodeJson.value("id", 0);
    const std::optional<rock::NodeKind> nodeKind = ReadSerializedNodeKind(nodeJson);
    if (!nodeKind || node.id == 0)
    {
        return std::nullopt;
    }

    node.kind = *nodeKind;
    node.title = nodeJson.value("title", std::string(rock::ToString(node.kind)));
    if (node.kind == rock::NodeKind::HeightmapLoad && node.title == "Load Heightmap")
    {
        node.title = std::string(rock::ToString(node.kind));
    }
    ReadNodeSettingsJson(nodeJson, node);
    ReadSerializedPinsJson(nodeJson.value("inputs", nlohmann::json::array()), node.id, rock::PinKind::Input, node.inputs);
    ReadSerializedPinsJson(nodeJson.value("outputs", nlohmann::json::array()), node.id, rock::PinKind::Output, node.outputs);
    return node;
}

void MigrateRockUniqueMaskPins(std::vector<rock::Node>& nodes)
{
    rock::GraphId nextId = 1;
    for (const rock::Node& node : nodes)
    {
        nextId = std::max(nextId, node.id + 1);
        for (const rock::Pin& pin : node.inputs)
        {
            nextId = std::max(nextId, pin.id + 1);
        }
        for (const rock::Pin& pin : node.outputs)
        {
            nextId = std::max(nextId, pin.id + 1);
        }
    }

    for (rock::Node& node : nodes)
    {
        if (node.kind != rock::NodeKind::Rock)
        {
            continue;
        }
        const bool hasUniqueMask = std::ranges::any_of(node.outputs, [](const rock::Pin& pin) {
            return pin.kind == rock::PinKind::Output &&
                   pin.valueType == rock::ValueType::Mask &&
                   pin.label == "Unique Mask";
        });
        if (hasUniqueMask)
        {
            continue;
        }

        rock::Pin pin;
        pin.id = nextId++;
        pin.nodeId = node.id;
        pin.kind = rock::PinKind::Output;
        pin.valueType = rock::ValueType::Mask;
        pin.label = "Unique Mask";
        node.outputs.push_back(std::move(pin));
    }
}

void ReadSerializedNodesJson(const nlohmann::json& root)
{
    const nlohmann::json nodesJson = root.value("nodes", nlohmann::json::array());
    if (!nodesJson.is_array() || nodesJson.empty())
    {
        return;
    }

    std::vector<rock::Node> nodes;
    for (const nlohmann::json& nodeJson : nodesJson)
    {
        std::optional<rock::Node> node = ReadSerializedNodeJson(nodeJson);
        if (node)
        {
            nodes.push_back(std::move(*node));
        }
    }
    if (!nodes.empty())
    {
        MigrateRockUniqueMaskPins(nodes);
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
    const nlohmann::json viewportJson = root.value("viewport", nlohmann::json::object());
    g_viewport.yaw = viewportJson.value("yaw", g_viewport.yaw);
    g_viewport.pitch = viewportJson.value("pitch", g_viewport.pitch);
    g_viewport.fovDegrees = viewportJson.value("fovDegrees", g_viewport.fovDegrees);
    g_viewport.orbitDistance = viewportJson.value("orbitDistance", g_viewport.orbitDistance);
    const std::string savedAppVersion = root.value("appVersion", std::string());
    NormalizeLoadedViewport(savedAppVersion != TERRAIN_EDITOR_VERSION_STRING);
    if (viewportJson.contains("pan") && viewportJson["pan"].is_array() && viewportJson["pan"].size() == 2)
    {
        g_viewport.pan = ImVec2(viewportJson["pan"][0].get<float>(), viewportJson["pan"][1].get<float>());
    }
}

void ReadSerializedLinksJson(const nlohmann::json& root)
{
    std::vector<rock::Link> links;
    if (root.contains("links") && root["links"].is_array())
    {
        for (const nlohmann::json& linkJson : root["links"])
        {
            rock::Link link;
            link.id = linkJson.value("id", 0);
            link.startPin = linkJson.value("startPin", 0);
            link.endPin = linkJson.value("endPin", 0);
            if (link.id > 0 && g_graph.CanCreateLink(link.startPin, link.endPin))
            {
                links.push_back(link);
            }
        }
    }
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
    g_graph.SetPreviewStage(ReadSerializedPreviewStage(root).value_or(rock::PreviewStage::Graph));
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

        ReadProjectSettingsJson(root);
        ReadSerializedNodesJson(root);
        MigrateLegacySimulationResolutionFromNodes();
        ReadViewportJson(root);
        ReadSerializedLinksJson(root);
        ReadSelectedNodesJson(root);
        ReadPreviewSelectionJson(root);
        ReadNodePositionsJson(root);

        g_projectPath = path;
        UpdateWindowTitle();
        AddRecentProjectPath(path);
        SaveAppSettingsSilently();
        ClearUndoHistory();
        g_projectStatus = "Loaded " + PathToUtf8(path);
        EvaluateGraph();
        return true;
    }
    catch (const std::exception& ex)
    {
        if (error) *error = ex.what();
        return false;
    }
}

bool EnsureMeshPreviewPipeline(std::string* error)
{
    if (g_meshPreviewSurfacePso) return true;
    if (!g_device) { if (error) *error = "D3D12 device not initialized"; return false; }

    D3D12_DESCRIPTOR_RANGE meshResourceRange{};
    meshResourceRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    meshResourceRange.NumDescriptors = 5; // t0 shadow, t1 cloud shadow, t2/t3 displacement, t4 Colorize
    meshResourceRange.BaseShaderRegister = 0;
    meshResourceRange.RegisterSpace = 0;
    meshResourceRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Root parameter budget: 60 (mesh constants) + 2 (cloud shadow CBV)
    // + 1 (mesh resource table) = 63 DWORDs.
    D3D12_ROOT_PARAMETER rootParams[3]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = sizeof(MeshPreviewConstants) / sizeof(UINT);
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &meshResourceRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].RegisterSpace = 0;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    samplers[1].RegisterSpace = 0;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 3;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize mesh root sig failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_meshPreviewRootSignature));
    if (FAILED(hr)) { if (error) *error = "Create mesh preview root sig failed"; return false; }

    const std::filesystem::path shaderPath = MeshPreviewShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vsBlob, psBlob, psEdgeBlob, vsShadowBlob;
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh VS failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSSurface", "ps_5_0", compileFlags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh PS failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSEdge", "ps_5_0", compileFlags, 0, &psEdgeBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh edge PS failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSShadow", "vs_5_0", compileFlags, 0, &vsShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile mesh shadow VS failed"; return false; }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] =
    {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_meshPreviewRootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.InputLayout = {inputLayout, 4};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewSurfacePso));
    if (FAILED(hr)) { if (error) *error = "Create mesh surface PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewWirePso));
    if (FAILED(hr)) { if (error) *error = "Create mesh wire PSO failed"; return false; }

    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewGridPso));
    if (FAILED(hr)) { if (error) *error = "Create mesh grid PSO failed"; return false; }

    psoDesc.VS = {vsShadowBlob->GetBufferPointer(), vsShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewShadowPso));
    if (FAILED(hr)) { if (error) *error = "Create mesh shadow PSO failed"; return false; }

    return true;
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

bool EnsureMeshPreviewDisplacementPipeline(std::string* error)
{
    if (g_meshPreviewDisplacementSurfacePso) return true;
    if (!g_device) { if (error) *error = "D3D12 device not initialized"; return false; }

    // Persistent CBV upload buffer for mesh constants. Aligned to 256 bytes
    // (CB requirement) and filled per-draw via memcpy. One instance is
    // sufficient since the GPU consumes it before the next draw of this
    // pass; no in-flight overlap to worry about.
    if (!g_meshPreviewDisplacementCbv)
    {
        const UINT64 cbSize = (sizeof(MeshPreviewConstants) + 255u) & ~255u;
        D3D12_HEAP_PROPERTIES uploadHeap{};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = cbSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        HRESULT hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE,
            &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&g_meshPreviewDisplacementCbv));
        if (FAILED(hr)) { if (error) *error = "Create mesh displacement CBV failed"; return false; }
    }

    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 0; // t0
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE cloudShadowRange{};
    cloudShadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cloudShadowRange.NumDescriptors = 1;
    cloudShadowRange.BaseShaderRegister = 1; // t1
    cloudShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE heightRange{};
    heightRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    heightRange.NumDescriptors = 1;
    heightRange.BaseShaderRegister = 2; // t2
    heightRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE maskRange{};
    maskRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    maskRange.NumDescriptors = 1;
    maskRange.BaseShaderRegister = 3; // t3
    maskRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE colorRange{};
    colorRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    colorRange.NumDescriptors = 1;
    colorRange.BaseShaderRegister = 4; // t4
    colorRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Budget: 2 (mesh CBV) + 2 (cloud shadow CBV) + 8 (displacement consts)
    // + 1*5 (5 SRV tables) = 17 DWORDs of 64.
    D3D12_ROOT_PARAMETER rootParams[8]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[2].Constants.ShaderRegister = 2;
    rootParams[2].Constants.Num32BitValues = sizeof(DisplacementShaderConstants) / sizeof(UINT);
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &shadowRange;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[4].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[4].DescriptorTable.pDescriptorRanges = &cloudShadowRange;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[5].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[5].DescriptorTable.pDescriptorRanges = &heightRange;
    rootParams[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[6].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[6].DescriptorTable.pDescriptorRanges = &maskRange;
    rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[7].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[7].DescriptorTable.pDescriptorRanges = &colorRange;
    rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize displacement root sig failed"; return false; }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_meshPreviewDisplacementRootSignature));
    if (FAILED(hr)) { if (error) *error = "Create displacement root sig failed"; return false; }

    const std::filesystem::path shaderPath = MeshPreviewShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> vsBlob, psBlob, psEdgeBlob, vsShadowBlob, vsSectionBlob, vsSectionShadowBlob, vsPatchBlob, hsPatchBlob, dsPatchBlob, dsPatchShadowBlob;
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacement", "vs_5_0", compileFlags, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacement failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSSurface", "ps_5_0", compileFlags, 0, &psBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile PSSurface (displacement) failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSEdge", "ps_5_0", compileFlags, 0, &psEdgeBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile PSEdge (displacement) failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementShadow", "vs_5_0", compileFlags, 0, &vsShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementShadow failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementSection", "vs_5_0", compileFlags, 0, &vsSectionBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementSection failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementSectionShadow", "vs_5_0", compileFlags, 0, &vsSectionShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementSectionShadow failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSDisplacementPatch", "vs_5_0", compileFlags, 0, &vsPatchBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile VSDisplacementPatch failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "HSDisplacement", "hs_5_0", compileFlags, 0, &hsPatchBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile HSDisplacement failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "DSDisplacement", "ds_5_0", compileFlags, 0, &dsPatchBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile DSDisplacement failed"; return false; }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "DSDisplacementShadow", "ds_5_0", compileFlags, 0, &dsPatchShadowBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile DSDisplacementShadow failed"; return false; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_meshPreviewDisplacementRootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    // No vertex buffer — VS reads SV_VertexID. Empty input layout.
    psoDesc.InputLayout = {nullptr, 0};
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementSurfacePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement surface PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.DepthBias = -64;
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.25f;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementWirePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement wire PSO failed"; return false; }

    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.VS = {vsSectionBlob->GetBufferPointer(), vsSectionBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.HS = {};
    psoDesc.DS = {};
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementSectionPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement section PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.DepthBias = -64;
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.25f;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementSectionWirePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement section wire PSO failed"; return false; }

    psoDesc.VS = {vsSectionShadowBlob->GetBufferPointer(), vsSectionShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementSectionShadowPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement section shadow PSO failed"; return false; }

    // Shadow PSO — same root sig (so the shader can read displacement
    // constants + height texture), but writes only depth.
    psoDesc.VS = {vsShadowBlob->GetBufferPointer(), vsShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementShadowPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement shadow PSO failed"; return false; }

    psoDesc.VS = {vsPatchBlob->GetBufferPointer(), vsPatchBlob->GetBufferSize()};
    psoDesc.HS = {hsPatchBlob->GetBufferPointer(), hsPatchBlob->GetBufferSize()};
    psoDesc.DS = {dsPatchBlob->GetBufferPointer(), dsPatchBlob->GetBufferSize()};
    psoDesc.PS = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementTessSurfacePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement tessellation surface PSO failed"; return false; }

    psoDesc.PS = {psEdgeBlob->GetBufferPointer(), psEdgeBlob->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    psoDesc.RasterizerState.DepthBias = -64;
    psoDesc.RasterizerState.SlopeScaledDepthBias = -0.25f;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementTessWirePso));
    if (FAILED(hr)) { if (error) *error = "Create displacement tessellation wire PSO failed"; return false; }

    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.DepthBias = 0;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    psoDesc.DS = {dsPatchShadowBlob->GetBufferPointer(), dsPatchShadowBlob->GetBufferSize()};
    psoDesc.PS = {};
    psoDesc.NumRenderTargets = 0;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
    psoDesc.RasterizerState.DepthBias = 1200;
    psoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_meshPreviewDisplacementTessShadowPso));
    if (FAILED(hr)) { if (error) *error = "Create displacement tessellation shadow PSO failed"; return false; }

    return true;
}

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
        AllocateSrvDescriptorRange(5, &g_gpuMeshPreview.meshResourceTableCpu, &g_gpuMeshPreview.meshResourceTableGpu);
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
        g_gpuClouds.dummyShadowSrvCpu,
    };
    for (int i = 0; i < 4; ++i)
    {
        g_device->CopyDescriptorsSimple(1, OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, i), src[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!g_gpuMeshPreview.colorizeTexture)
    {
        g_device->CopyDescriptorsSimple(1, OffsetCpuSrv(g_gpuMeshPreview.meshResourceTableCpu, 4), src[4], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
}


// =============================================================================
// Multi-Scale Erosion GPU compute path
// =============================================================================

// Root-signature-visible constants for shaders/multi_scale_erosion_compute.hlsl.
// Layout matches the cbuffer there exactly (HLSL scalar packing into 16-byte
// rows). We push 21 32-bit values via ID3D12GraphicsCommandList::SetComputeRoot32BitConstants.
struct MseShaderConstants
{
    UINT  resolution;
    float terrainSizeMeters;
    float cellSizeMeters;
    float cellDiag;
    float refCellArea;
    float speStrength;
    float streamExponent;
    float slopeExponent;
    float maxStreamPower;
    float flowExponent;
    float speTimeStep;
    float thermalTanAngle;
    float thermalStrength;
    UINT  thermalNoisifyAngle;
    float thermalNoiseMin;
    float thermalNoiseMax;
    float thermalNoiseWavelength;
    float depositionStrength;
    float rain;
    float pad0;
    float pad1;
};
static_assert(sizeof(MseShaderConstants) == 21 * sizeof(UINT), "MseShaderConstants must be 21 DWORDs");

bool EnsureMseComputePipeline(std::string* error)
{
    if (g_mseComputeReady && g_mseComputeRootSignature && g_mseStreamPowerPso && g_mseThermalPso && g_mseDepositionPso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_mseComputeStatus = "MSE GPU Compute unavailable";
        return false;
    }

    // 6 UAVs (HeightIn/Out, StreamIn/Out, SedIn/Out) bound as a single
    // descriptor table. The table base shifts between dispatches so we don't
    // have to rewrite descriptors mid-command-list.
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 6;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 21;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize MSE root sig failed";
        g_mseComputeStatus = "MSE GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_mseComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create MSE root sig failed";
        g_mseComputeStatus = "MSE GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = MseComputeShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile MSE shader failed";
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> spBlob, thBlob, depBlob;
    if (!compileEntry("CSStreamPower", spBlob)) { g_mseComputeStatus = "MSE SPE shader compile failed"; return false; }
    if (!compileEntry("CSThermal",     thBlob)) { g_mseComputeStatus = "MSE thermal shader compile failed"; return false; }
    if (!compileEntry("CSDeposition",  depBlob)) { g_mseComputeStatus = "MSE deposition shader compile failed"; return false; }

    auto buildPso = [&](ID3DBlob* csBlob, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_mseComputeRootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        const HRESULT psoHr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(psoHr))
        {
            if (error) *error = "Create MSE PSO failed";
            return false;
        }
        return true;
    };

    if (!buildPso(spBlob.Get(),  g_mseStreamPowerPso)) { g_mseComputeStatus = "MSE SPE PSO failed"; return false; }
    if (!buildPso(thBlob.Get(),  g_mseThermalPso))     { g_mseComputeStatus = "MSE thermal PSO failed"; return false; }
    if (!buildPso(depBlob.Get(), g_mseDepositionPso))  { g_mseComputeStatus = "MSE deposition PSO failed"; return false; }

    g_mseComputeReady = true;
    g_mseComputeStatus = "MSE GPU Compute dispatch ready";
    return true;
}

bool RunMseComputeGridImmediate(rock::HeightfieldGrid& grid, const rock::MultiScaleErosionSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_mseComputeMutex);
    if (!EnsureMseComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    if (resolution < 3 || grid.heights.size() < cellCount)
    {
        if (error) *error = "Invalid heightfield for MSE GPU Compute";
        return false;
    }

    const UINT64 bufferSize = cellCount * sizeof(float);
    const std::vector<float> zeroData(static_cast<size_t>(cellCount), 0.0f);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC gpuDesc = BufferResourceDesc(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC cpuDesc = BufferResourceDesc(bufferSize);

    // Six ping-pong UAV buffers (Heights/Stream/Sed × A/B).
    ComPtr<ID3D12Resource> heightA, heightB, streamA, streamB, sedA, sedB;
    ComPtr<ID3D12Resource> uploadHeights, uploadZero;
    ComPtr<ID3D12Resource> readbackHeights, readbackStream, readbackSed;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(heightA, "MSE heightA")) return false;
    if (!createDefault(heightB, "MSE heightB")) return false;
    if (!createDefault(streamA, "MSE streamA")) return false;
    if (!createDefault(streamB, "MSE streamB")) return false;
    if (!createDefault(sedA,    "MSE sedA"))    return false;
    if (!createDefault(sedB,    "MSE sedB"))    return false;
    if (!createUpload(uploadHeights, "MSE upload heights")) return false;
    if (!createUpload(uploadZero,    "MSE upload zero"))    return false;
    if (!createReadback(readbackHeights, "MSE readback heights")) return false;
    if (!createReadback(readbackStream,  "MSE readback stream"))  return false;
    if (!createReadback(readbackSed,     "MSE readback sed"))     return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map MSE height upload failed");
    std::memcpy(mapped, grid.heights.data(), bufferSize);
    uploadHeights->Unmap(0, nullptr);
    ThrowIfFailed(uploadZero->Map(0, &emptyReadRange, &mapped), "Map MSE zero upload failed");
    std::memcpy(mapped, zeroData.data(), bufferSize);
    uploadZero->Unmap(0, nullptr);

    // Descriptor heap layout: one fixed UAV per buffer (6 descriptors). The
    // descriptor table base advances per-pass to bind the right In/Out pairs.
    // For the 8 possible (heightCur, streamCur, sedCur) states we need 8
    // contiguous 6-descriptor blocks = 48 descriptors total.
    constexpr UINT kStateCount = 8;
    constexpr UINT kDescriptorsPerState = 6;
    constexpr UINT kHeapDescriptors = kStateCount * kDescriptorsPerState;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kHeapDescriptors;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create MSE descriptor heap failed"; return false; }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = static_cast<UINT>(cellCount);
    uavDesc.Buffer.StructureByteStride = sizeof(float);

    // For each (hCur, sCur, dCur) ∈ {0,1}³ build a 6-UAV slot in the heap.
    // u0 = HeightIn (cur), u1 = HeightOut (other), u2/u3 = stream, u4/u5 = sed.
    auto resolveBuffer = [&](int which, ID3D12Resource* a, ID3D12Resource* b) {
        return which == 0 ? a : b;
    };
    D3D12_CPU_DESCRIPTOR_HANDLE descBase = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT state = 0; state < kStateCount; ++state)
    {
        const int hCur = static_cast<int>((state >> 0) & 1u);
        const int sCur = static_cast<int>((state >> 1) & 1u);
        const int dCur = static_cast<int>((state >> 2) & 1u);
        ID3D12Resource* uavs[6] = {
            resolveBuffer(hCur, heightA.Get(), heightB.Get()),
            resolveBuffer(1 - hCur, heightA.Get(), heightB.Get()),
            resolveBuffer(sCur, streamA.Get(), streamB.Get()),
            resolveBuffer(1 - sCur, streamA.Get(), streamB.Get()),
            resolveBuffer(dCur, sedA.Get(), sedB.Get()),
            resolveBuffer(1 - dCur, sedA.Get(), sedB.Get()),
        };
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descBase;
        handle.ptr += static_cast<SIZE_T>(state) * static_cast<SIZE_T>(kDescriptorsPerState) * g_srvDescriptorSize;
        for (UINT i = 0; i < 6; ++i)
        {
            g_device->CreateUnorderedAccessView(uavs[i], nullptr, &uavDesc, handle);
            handle.ptr += g_srvDescriptorSize;
        }
    }

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create MSE command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create MSE command list failed");

    // Initial state: HeightA/B = input heights, all stream / sed = 0.
    commandList->CopyBufferRegion(heightA.Get(), 0, uploadHeights.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(heightB.Get(), 0, uploadHeights.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(streamA.Get(), 0, uploadZero.Get(),    0, bufferSize);
    commandList->CopyBufferRegion(streamB.Get(), 0, uploadZero.Get(),    0, bufferSize);
    commandList->CopyBufferRegion(sedA.Get(),    0, uploadZero.Get(),    0, bufferSize);
    commandList->CopyBufferRegion(sedB.Get(),    0, uploadZero.Get(),    0, bufferSize);

    D3D12_RESOURCE_BARRIER toUav[6]{};
    ID3D12Resource* uavResources[6] = {heightA.Get(), heightB.Get(), streamA.Get(), streamB.Get(), sedA.Get(), sedB.Get()};
    for (int i = 0; i < 6; ++i)
    {
        toUav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav[i].Transition.pResource = uavResources[i];
        toUav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toUav[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(6, toUav);

    const float cellSizeMeters = grid.terrainSizeMeters / static_cast<float>(std::max<UINT>(1, resolution - 1));
    MseShaderConstants constants{};
    constants.resolution = resolution;
    constants.terrainSizeMeters = grid.terrainSizeMeters;
    constants.cellSizeMeters = cellSizeMeters;
    constants.cellDiag = cellSizeMeters * std::sqrt(2.0f);
    constants.refCellArea = 16.0f;  // matches mse::kRefCellArea on the CPU side
    constants.speStrength = settings.speStrength;
    constants.streamExponent = settings.streamExponent;
    constants.slopeExponent = settings.slopeExponent;
    constants.maxStreamPower = settings.maxStreamPower;
    constants.flowExponent = settings.flowExponent;
    constants.speTimeStep = settings.speTimeStep;
    constants.thermalTanAngle = std::tan(settings.thermalAngleDegrees * 3.14159265358979323846f / 180.0f);
    constants.thermalStrength = settings.thermalStrength;
    constants.thermalNoisifyAngle = settings.thermalNoisifyAngle ? 1u : 0u;
    constants.thermalNoiseMin = settings.thermalNoiseMin;
    constants.thermalNoiseMax = settings.thermalNoiseMax;
    constants.thermalNoiseWavelength = settings.thermalNoiseWavelength;
    constants.depositionStrength = settings.depositionStrength;
    constants.rain = settings.rain;

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_mseComputeRootSignature.Get());

    const UINT groupCount = (resolution + 7u) / 8u;
    const int iterations = std::clamp(settings.iterations, 1, 500);

    auto stateIndex = [](int hCur, int sCur, int dCur) -> UINT {
        return static_cast<UINT>(hCur | (sCur << 1) | (dCur << 2));
    };
    auto bindStateAndDispatch = [&](int hCur, int sCur, int dCur, ID3D12PipelineState* pso) {
        commandList->SetPipelineState(pso);
        commandList->SetComputeRoot32BitConstants(0, 21, &constants, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE table = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += static_cast<UINT64>(stateIndex(hCur, sCur, dCur)) * kDescriptorsPerState * g_srvDescriptorSize;
        commandList->SetComputeRootDescriptorTable(1, table);
        commandList->Dispatch(groupCount, groupCount, 1);

        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &uavBarrier);
    };

    int hCur = 0;
    int sCur = 0;
    int dCur = 0;
    for (int it = 0; it < iterations; ++it)
    {
        if (settings.enableStreamPower)
        {
            bindStateAndDispatch(hCur, sCur, dCur, g_mseStreamPowerPso.Get());
            hCur ^= 1;
            sCur ^= 1;
        }
        if (settings.enableThermal)
        {
            bindStateAndDispatch(hCur, sCur, dCur, g_mseThermalPso.Get());
            hCur ^= 1;
        }
        if (settings.enableDeposition)
        {
            bindStateAndDispatch(hCur, sCur, dCur, g_mseDepositionPso.Get());
            hCur ^= 1;
            sCur ^= 1;
            dCur ^= 1;
        }
    }

    // Read back the buffers that hold the *current* state for each field.
    ID3D12Resource* finalHeight = (hCur == 0) ? heightA.Get() : heightB.Get();
    ID3D12Resource* finalStream = (sCur == 0) ? streamA.Get() : streamB.Get();
    ID3D12Resource* finalSed    = (dCur == 0) ? sedA.Get()    : sedB.Get();

    D3D12_RESOURCE_BARRIER toCopy[3]{};
    ID3D12Resource* copyResources[3] = {finalHeight, finalStream, finalSed};
    for (int i = 0; i < 3; ++i)
    {
        toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy[i].Transition.pResource = copyResources[i];
        toCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(3, toCopy);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, finalHeight, 0, bufferSize);
    commandList->CopyBufferRegion(readbackStream.Get(),  0, finalStream, 0, bufferSize);
    commandList->CopyBufferRegion(readbackSed.Get(),     0, finalSed,    0, bufferSize);
    ThrowIfFailed(commandList->Close(), "Close MSE command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal MSE fence failed");
    WaitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedStream = nullptr;
    void* mappedSed = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bufferSize)};
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map MSE height readback failed");
    ThrowIfFailed(readbackStream->Map(0, &readRange, &mappedStream),   "Map MSE stream readback failed");
    ThrowIfFailed(readbackSed->Map(0, &readRange, &mappedSed),         "Map MSE sed readback failed");
    const float* heightValues = static_cast<const float*>(mappedHeights);
    const float* streamValues = static_cast<const float*>(mappedStream);
    const float* sedValues    = static_cast<const float*>(mappedSed);
    grid.heights.assign(heightValues, heightValues + cellCount);
    grid.flows.assign(streamValues, streamValues + cellCount);
    grid.deposits.assign(sedValues, sedValues + cellCount);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    grid.age.assign(static_cast<size_t>(cellCount), 0.0f);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackStream->Unmap(0, &emptyWriteRange);
    readbackSed->Unmap(0, &emptyWriteRange);

    // Match the CPU path's final NormalizeHeightfieldFields: scale flows /
    // deposits to [0, 1] by their max so the downstream visualization sees the
    // same range regardless of backend.
    auto normalize = [](std::vector<float>& field) {
        float maxValue = 0.0f;
        for (float v : field) { maxValue = std::max(maxValue, v); }
        if (maxValue > 1e-6f)
        {
            for (float& v : field) { v = std::clamp(v / maxValue, 0.0f, 1.0f); }
        }
    };
    normalize(grid.flows);
    normalize(grid.deposits);

    g_mseComputeStatus = "MSE GPU Compute evaluated heightfield";
    return true;
}

bool RunMseComputeGrid(rock::HeightfieldGrid& grid, const rock::MultiScaleErosionSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunMseComputeGridImmediate(grid, settings, error);
    }

    auto request = std::make_shared<MseGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<MseGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_mseGpuRequestMutex);
        g_pendingMseGpuRequests.push_back(request);
    }
    g_mseComputeStatus = "MSE GPU Compute queued on main thread";

    MseGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingMseGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<MseGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_mseGpuRequestMutex);
        requests.swap(g_pendingMseGpuRequests);
    }

    for (const std::shared_ptr<MseGpuRequest>& request : requests)
    {
        MseGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunMseComputeGridImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

struct MaskNoiseShaderConstants
{
    UINT  resolution;
    UINT  octaves;
    INT   seed;
    float frequency;
    float lacunarity;
    float persistence;
};
static_assert(sizeof(MaskNoiseShaderConstants) == 6 * sizeof(UINT), "MaskNoiseShaderConstants must be 6 DWORDs");

bool EnsureMaskNoiseComputePipeline(std::string* error)
{
    if (g_maskNoiseComputeReady && g_maskNoiseComputeRootSignature && g_maskNoisePso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_maskNoiseComputeStatus = "Mask Noise GPU Compute unavailable";
        return false;
    }

    // One UAV (output buffer) bound through a descriptor table, plus 6
    // 32-bit constants for shader parameters.
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 6;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize Mask Noise root sig failed";
        g_maskNoiseComputeStatus = "Mask Noise GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_maskNoiseComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create Mask Noise root sig failed";
        g_maskNoiseComputeStatus = "Mask Noise GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = MaskNoiseShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                            "CSGenerate", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Mask Noise shader failed";
        g_maskNoiseComputeStatus = "Mask Noise shader compile failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_maskNoiseComputeRootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    HRESULT psoHr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_maskNoisePso));
    if (FAILED(psoHr))
    {
        if (error) *error = "Create Mask Noise PSO failed";
        g_maskNoiseComputeStatus = "Mask Noise PSO failed";
        return false;
    }

    g_maskNoiseComputeReady = true;
    g_maskNoiseComputeStatus = "Mask Noise GPU Compute dispatch ready";
    return true;
}

bool RunMaskNoiseComputeImmediate(rock::MaskGrid& grid, const rock::MaskNoiseSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_maskNoiseComputeMutex);
    if (!EnsureMaskNoiseComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    if (resolution < 2)
    {
        if (error) *error = "Invalid resolution for Mask Noise GPU Compute";
        return false;
    }
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    const UINT64 bufferSize = cellCount * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC gpuDesc = BufferResourceDesc(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC cpuDesc = BufferResourceDesc(bufferSize);

    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> readback;
    HRESULT hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output));
    if (FAILED(hr)) { if (error) *error = "Create Mask Noise output buffer failed"; return false; }
    hr = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) { if (error) *error = "Create Mask Noise readback buffer failed"; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Mask Noise descriptor heap failed"; return false; }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = static_cast<UINT>(cellCount);
    uavDesc.Buffer.StructureByteStride = sizeof(float);
    g_device->CreateUnorderedAccessView(output.Get(), nullptr, &uavDesc, descriptorHeap->GetCPUDescriptorHandleForHeapStart());

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Mask Noise command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Mask Noise command list failed");

    MaskNoiseShaderConstants constants{};
    constants.resolution = resolution;
    constants.octaves = static_cast<UINT>(std::clamp(settings.octaves, 1, 12));
    constants.seed = settings.seed;
    constants.frequency = std::max(settings.frequency, 0.0f);
    constants.lacunarity = std::max(settings.lacunarity, 0.0f);
    constants.persistence = std::clamp(settings.persistence, 0.0f, 1.0f);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_maskNoiseComputeRootSignature.Get());
    commandList->SetPipelineState(g_maskNoisePso.Get());
    commandList->SetComputeRoot32BitConstants(0, 6, &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    const UINT groupCount = (resolution + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = output.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopy);
    commandList->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, bufferSize);
    ThrowIfFailed(commandList->Close(), "Close Mask Noise command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal Mask Noise fence failed");
    WaitForFenceValue(fenceValue);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(bufferSize)};
    ThrowIfFailed(readback->Map(0, &readRange, &mapped), "Map Mask Noise readback failed");
    const float* values = static_cast<const float*>(mapped);
    grid.values.assign(values, values + cellCount);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readback->Unmap(0, &emptyWriteRange);

    g_maskNoiseComputeStatus = "Mask Noise GPU Compute evaluated";
    return true;
}

bool RunMaskNoiseCompute(rock::MaskGrid& grid, const rock::MaskNoiseSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunMaskNoiseComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<MaskNoiseGpuRequest>();
    request->settings = settings;
    request->resolution = grid.resolution;
    std::future<MaskNoiseGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_maskNoiseGpuRequestMutex);
        g_pendingMaskNoiseGpuRequests.push_back(request);
    }
    g_maskNoiseComputeStatus = "Mask Noise GPU Compute queued on main thread";

    MaskNoiseGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingMaskNoiseGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<MaskNoiseGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_maskNoiseGpuRequestMutex);
        requests.swap(g_pendingMaskNoiseGpuRequests);
    }

    for (const std::shared_ptr<MaskNoiseGpuRequest>& request : requests)
    {
        MaskNoiseGpuRequestResult result;
        result.grid.resolution = request->resolution;
        result.grid.values.assign(static_cast<size_t>(request->resolution) * static_cast<size_t>(request->resolution), 0.0f);
        result.success = RunMaskNoiseComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

struct ColorizeShaderConstants
{
    UINT resolution;
    UINT cellCount;
    UINT stopCount;
    UINT hasMask;
    UINT hasBaseColor;
};
static_assert(sizeof(ColorizeShaderConstants) == 5 * sizeof(UINT), "ColorizeShaderConstants must be 5 DWORDs");

bool EnsureColorizeComputePipeline(std::string* error)
{
    if (g_colorizeComputeReady && g_colorizeComputeRootSignature && g_colorizeComputePso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_colorizeComputeStatus = "Colorize GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE srvRange{};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 4;
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
    rootParams[0].Constants.Num32BitValues = 5;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize Colorize root sig failed";
        g_colorizeComputeStatus = "Colorize GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_colorizeComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create Colorize root sig failed";
        g_colorizeComputeStatus = "Colorize GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = ColorizeComputeShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                 "CSColorize", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Colorize shader failed";
        g_colorizeComputeStatus = "Colorize shader compile failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_colorizeComputeRootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    hr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_colorizeComputePso));
    if (FAILED(hr))
    {
        if (error) *error = "Create Colorize PSO failed";
        g_colorizeComputeStatus = "Colorize PSO failed";
        return false;
    }

    g_colorizeComputeReady = true;
    g_colorizeComputeStatus = "Colorize GPU Compute dispatch ready";
    return true;
}

bool RunColorizeComputeImmediate(rock::ColorGrid& grid, const rock::ColorizeSettings& settings, const rock::MaskGrid& gradientMask, const rock::MaskGrid* mask, const rock::ColorGrid* baseColor, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_colorizeComputeMutex);
    if (!EnsureColorizeComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(gradientMask.resolution, 0, 4096));
    if (resolution < 2 || gradientMask.values.size() < static_cast<size_t>(resolution) * static_cast<size_t>(resolution))
    {
        if (error) *error = "Invalid Gradient Mask for Colorize GPU Compute";
        return false;
    }
    const bool hasMask = mask != nullptr &&
        mask->resolution == static_cast<int>(resolution) &&
        mask->values.size() >= static_cast<size_t>(resolution) * static_cast<size_t>(resolution);
    const bool hasBaseColor = baseColor != nullptr &&
        baseColor->resolution > 0 &&
        baseColor->pixels.size() >= static_cast<size_t>(baseColor->resolution) * static_cast<size_t>(baseColor->resolution) * 4u;
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    const UINT64 maskByteSize = cellCount * sizeof(float);
    std::vector<rock::ColorStop> sourceStops = settings.stops;
    if (sourceStops.empty())
    {
        sourceStops = {
            {0.0f, 0.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 1.0f, 1.0f},
        };
    }
    const UINT stopCount = static_cast<UINT>(std::min<size_t>(sourceStops.size(), 256));
    struct GpuStop { float position; float r; float g; float b; };
    std::vector<GpuStop> stops;
    stops.reserve(stopCount);
    for (UINT i = 0; i < stopCount; ++i)
    {
        const rock::ColorStop& s = sourceStops[i];
        stops.push_back({std::clamp(s.position, 0.0f, 1.0f), s.r, s.g, s.b});
    }

    std::vector<uint32_t> basePixels(static_cast<size_t>(cellCount), 0xff000000u);
    if (hasBaseColor)
    {
        for (UINT y = 0; y < resolution; ++y)
        {
            const float v = resolution > 1 ? static_cast<float>(y) / static_cast<float>(resolution - 1u) : 0.0f;
            const int baseY = std::clamp(static_cast<int>(std::round(v * static_cast<float>(baseColor->resolution - 1))), 0, baseColor->resolution - 1);
            for (UINT x = 0; x < resolution; ++x)
            {
                const float u = resolution > 1 ? static_cast<float>(x) / static_cast<float>(resolution - 1u) : 0.0f;
                const int baseX = std::clamp(static_cast<int>(std::round(u * static_cast<float>(baseColor->resolution - 1))), 0, baseColor->resolution - 1);
                const size_t src = (static_cast<size_t>(baseY) * static_cast<size_t>(baseColor->resolution) + static_cast<size_t>(baseX)) * 4u;
                const uint32_t r = baseColor->pixels[src + 0u];
                const uint32_t g = baseColor->pixels[src + 1u];
                const uint32_t b = baseColor->pixels[src + 2u];
                const uint32_t a = baseColor->pixels[src + 3u];
                basePixels[static_cast<size_t>(y) * static_cast<size_t>(resolution) + static_cast<size_t>(x)] =
                    (r & 255u) | ((g & 255u) << 8) | ((b & 255u) << 16) | ((a & 255u) << 24);
            }
        }
    }

    const UINT64 stopByteSize = std::max<UINT64>(static_cast<UINT64>(stops.size()) * sizeof(GpuStop), sizeof(GpuStop));
    const UINT64 outputByteSize = cellCount * sizeof(uint32_t);
    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC outputGpuDesc = BufferResourceDesc(outputByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC outputCpuDesc = BufferResourceDesc(outputByteSize);

    ComPtr<ID3D12Resource> gradientUpload = CreateUploadBuffer(gradientMask.values.data(), maskByteSize, "Create Colorize gradient upload failed");
    ComPtr<ID3D12Resource> maskUpload = CreateUploadBuffer(hasMask ? mask->values.data() : gradientMask.values.data(), maskByteSize, "Create Colorize mask upload failed");
    ComPtr<ID3D12Resource> stopsUpload = CreateUploadBuffer(stops.data(), stopByteSize, "Create Colorize stops upload failed");
    ComPtr<ID3D12Resource> baseUpload = CreateUploadBuffer(basePixels.data(), outputByteSize, "Create Colorize base upload failed");
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12Resource> readback;
    HRESULT hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &outputGpuDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output));
    if (FAILED(hr)) { if (error) *error = "Create Colorize output buffer failed"; return false; }
    hr = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &outputCpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) { if (error) *error = "Create Colorize readback buffer failed"; return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 5;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Colorize descriptor heap failed"; return false; }
    const UINT descriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpuHandle = [&](UINT index) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize;
        return handle;
    };
    auto gpuHandle = [&](UINT index) {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize;
        return handle;
    };

    auto createFloatSrv = [&](ID3D12Resource* resource, UINT64 elementCount, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.NumElements = static_cast<UINT>(elementCount);
        srvDesc.Buffer.StructureByteStride = sizeof(float);
        g_device->CreateShaderResourceView(resource, &srvDesc, dst);
    };
    createFloatSrv(gradientUpload.Get(), cellCount, cpuHandle(0));
    createFloatSrv(maskUpload.Get(), cellCount, cpuHandle(1));
    D3D12_SHADER_RESOURCE_VIEW_DESC stopsSrv{};
    stopsSrv.Format = DXGI_FORMAT_UNKNOWN;
    stopsSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    stopsSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    stopsSrv.Buffer.NumElements = stopCount;
    stopsSrv.Buffer.StructureByteStride = sizeof(GpuStop);
    g_device->CreateShaderResourceView(stopsUpload.Get(), &stopsSrv, cpuHandle(2));
    D3D12_SHADER_RESOURCE_VIEW_DESC baseSrv{};
    baseSrv.Format = DXGI_FORMAT_UNKNOWN;
    baseSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    baseSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    baseSrv.Buffer.NumElements = static_cast<UINT>(cellCount);
    baseSrv.Buffer.StructureByteStride = sizeof(uint32_t);
    g_device->CreateShaderResourceView(baseUpload.Get(), &baseSrv, cpuHandle(3));
    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUav{};
    outputUav.Format = DXGI_FORMAT_UNKNOWN;
    outputUav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    outputUav.Buffer.NumElements = static_cast<UINT>(cellCount);
    outputUav.Buffer.StructureByteStride = sizeof(uint32_t);
    g_device->CreateUnorderedAccessView(output.Get(), nullptr, &outputUav, cpuHandle(4));

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Colorize command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Colorize command list failed");

    ColorizeShaderConstants constants{};
    constants.resolution = resolution;
    constants.cellCount = static_cast<UINT>(cellCount);
    constants.stopCount = stopCount;
    constants.hasMask = hasMask ? 1u : 0u;
    constants.hasBaseColor = hasBaseColor ? 1u : 0u;

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_colorizeComputeRootSignature.Get());
    commandList->SetPipelineState(g_colorizeComputePso.Get());
    commandList->SetComputeRoot32BitConstants(0, 5, &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, gpuHandle(0));
    commandList->SetComputeRootDescriptorTable(2, gpuHandle(4));
    const UINT groupCount = (resolution + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = output.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopy);
    commandList->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, outputByteSize);
    ThrowIfFailed(commandList->Close(), "Close Colorize command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal Colorize fence failed");
    WaitForFenceValue(fenceValue);

    void* mapped = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(outputByteSize)};
    ThrowIfFailed(readback->Map(0, &readRange, &mapped), "Map Colorize readback failed");
    const uint32_t* packed = static_cast<const uint32_t*>(mapped);
    grid.resolution = static_cast<int>(resolution);
    grid.pixels.resize(static_cast<size_t>(cellCount) * 4u);
    for (UINT64 i = 0; i < cellCount; ++i)
    {
        const uint32_t p = packed[i];
        grid.pixels[i * 4u + 0u] = static_cast<uint8_t>(p & 0xffu);
        grid.pixels[i * 4u + 1u] = static_cast<uint8_t>((p >> 8) & 0xffu);
        grid.pixels[i * 4u + 2u] = static_cast<uint8_t>((p >> 16) & 0xffu);
        grid.pixels[i * 4u + 3u] = static_cast<uint8_t>((p >> 24) & 0xffu);
    }
    const D3D12_RANGE emptyWriteRange{0, 0};
    readback->Unmap(0, &emptyWriteRange);

    g_colorizeComputeStatus = "Colorize GPU Compute evaluated";
    return true;
}

bool RunColorizeCompute(rock::ColorGrid& grid, const rock::ColorizeSettings& settings, const rock::MaskGrid& gradientMask, const rock::MaskGrid* mask, const rock::ColorGrid* baseColor, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunColorizeComputeImmediate(grid, settings, gradientMask, mask, baseColor, error);
    }

    auto request = std::make_shared<ColorizeGpuRequest>();
    request->settings = settings;
    request->gradientMask = gradientMask;
    request->hasMask = mask != nullptr;
    if (mask != nullptr)
    {
        request->mask = *mask;
    }
    request->hasBaseColor = baseColor != nullptr;
    if (baseColor != nullptr)
    {
        request->baseColor = *baseColor;
    }
    std::future<ColorizeGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_colorizeGpuRequestMutex);
        g_pendingColorizeGpuRequests.push_back(request);
    }
    g_colorizeComputeStatus = "Colorize GPU Compute queued on main thread";

    ColorizeGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingColorizeGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<ColorizeGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_colorizeGpuRequestMutex);
        requests.swap(g_pendingColorizeGpuRequests);
    }

    for (const std::shared_ptr<ColorizeGpuRequest>& request : requests)
    {
        ColorizeGpuRequestResult result;
        result.success = RunColorizeComputeImmediate(
            result.grid,
            request->settings,
            request->gradientMask,
            request->hasMask ? &request->mask : nullptr,
            request->hasBaseColor ? &request->baseColor : nullptr,
            &result.error);
        request->promise.set_value(std::move(result));
    }
}

// Mirrors the cbuffer in shaders/sediment_compute.hlsl. Re-bound for
// every dispatch (the emit / sweep / setup passes share the same root
// signature and only change `talusH` / `emissionPerIter` / `convertTerrainToSediment`).
struct SedimentShaderConstants
{
    UINT  resolution;
    float talusH;
    float emissionPerIter;
    UINT  convertTerrainToSediment;
};
static_assert(sizeof(SedimentShaderConstants) == 4 * sizeof(UINT), "SedimentShaderConstants must be 4 DWORDs");

bool EnsureSedimentComputePipeline(std::string* error)
{
    if (g_sedimentComputeReady && g_sedimentComputeRootSignature
        && g_sedimentSetupPso && g_sedimentEmitPso
        && g_sedimentSweep1Pso && g_sedimentSweep2Pso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_sedimentComputeStatus = "Sediment GPU Compute unavailable";
        return false;
    }

    // 4 UAVs (bedrock, sediment, outgoing, inputHeights) bound as one
    // descriptor table at u0..u3.
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 4;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 4;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize Sediment root sig failed";
        g_sedimentComputeStatus = "Sediment GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_sedimentComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create Sediment root sig failed";
        g_sedimentComputeStatus = "Sediment GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = SedimentComputeShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Sediment shader failed";
            return false;
        }
        return true;
    };

    ComPtr<ID3DBlob> setupBlob, emitBlob, sw1Blob, sw2Blob;
    if (!compileEntry("CSSetup",       setupBlob)) { g_sedimentComputeStatus = "Sediment Setup shader compile failed"; return false; }
    if (!compileEntry("CSEmit",        emitBlob))  { g_sedimentComputeStatus = "Sediment Emit shader compile failed"; return false; }
    if (!compileEntry("CSSlideSweep1", sw1Blob))   { g_sedimentComputeStatus = "Sediment Sweep1 shader compile failed"; return false; }
    if (!compileEntry("CSSlideSweep2", sw2Blob))   { g_sedimentComputeStatus = "Sediment Sweep2 shader compile failed"; return false; }

    auto buildPso = [&](ID3DBlob* csBlob, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_sedimentComputeRootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        const HRESULT psoHr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(psoHr))
        {
            if (error) *error = "Create Sediment PSO failed";
            return false;
        }
        return true;
    };
    if (!buildPso(setupBlob.Get(), g_sedimentSetupPso))  { g_sedimentComputeStatus = "Sediment Setup PSO failed"; return false; }
    if (!buildPso(emitBlob.Get(),  g_sedimentEmitPso))   { g_sedimentComputeStatus = "Sediment Emit PSO failed"; return false; }
    if (!buildPso(sw1Blob.Get(),   g_sedimentSweep1Pso)) { g_sedimentComputeStatus = "Sediment Sweep1 PSO failed"; return false; }
    if (!buildPso(sw2Blob.Get(),   g_sedimentSweep2Pso)) { g_sedimentComputeStatus = "Sediment Sweep2 PSO failed"; return false; }

    g_sedimentComputeReady = true;
    g_sedimentComputeStatus = "Sediment GPU Compute dispatch ready";
    return true;
}

bool RunSedimentComputeImmediate(rock::HeightfieldGrid& grid, const rock::SedimentSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_sedimentComputeMutex);
    if (!EnsureSedimentComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    if (resolution < 2 || grid.heights.size() < cellCount)
    {
        if (error) *error = "Invalid heightfield for Sediment GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize = cellCount * sizeof(float);
    const UINT64 outgoingByteSize = cellCount * 4ull * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc = BufferResourceDesc(fieldByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC outgoingGpuDesc = BufferResourceDesc(outgoingByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc = BufferResourceDesc(fieldByteSize);

    ComPtr<ID3D12Resource> bedrockBuf, sedimentBuf, outgoingBuf, inputHeightsBuf;
    ComPtr<ID3D12Resource> uploadHeights;
    ComPtr<ID3D12Resource> readbackSediment, readbackBedrock;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(bedrockBuf,      fieldGpuDesc,    "Sediment bedrock buffer"))      return false;
    if (!createDefault(sedimentBuf,     fieldGpuDesc,    "Sediment sediment buffer"))     return false;
    if (!createDefault(outgoingBuf,     outgoingGpuDesc, "Sediment outgoing buffer"))     return false;
    if (!createDefault(inputHeightsBuf, fieldGpuDesc,    "Sediment input-heights buffer"))return false;
    if (!createUpload(uploadHeights,    "Sediment upload heights"))                       return false;
    if (!createReadback(readbackSediment, "Sediment readback sediment"))                  return false;
    if (!createReadback(readbackBedrock,  "Sediment readback bedrock"))                   return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map Sediment heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    // Descriptor heap: 4 UAVs in one block (bedrock, sediment, outgoing, inputHeights).
    constexpr UINT kDescriptorCount = 4;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kDescriptorCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Sediment descriptor heap failed"; return false; }

    auto createUav = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * g_srvDescriptorSize;
        g_device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    createUav(bedrockBuf.Get(),      static_cast<UINT>(cellCount),       0);
    createUav(sedimentBuf.Get(),     static_cast<UINT>(cellCount),       1);
    createUav(outgoingBuf.Get(),     static_cast<UINT>(cellCount * 4u),  2);
    createUav(inputHeightsBuf.Get(), static_cast<UINT>(cellCount),       3);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Sediment command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Sediment command list failed");

    // Stage input heights into the GPU buffer through one upload.
    D3D12_RESOURCE_BARRIER toCopyDest{};
    toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopyDest.Transition.pResource = inputHeightsBuf.Get();
    toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopyDest);
    commandList->CopyBufferRegion(inputHeightsBuf.Get(), 0, uploadHeights.Get(), 0, fieldByteSize);
    D3D12_RESOURCE_BARRIER toUav = toCopyDest;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &toUav);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_sedimentComputeRootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    const float terrainSizeMeters = std::max(grid.terrainSizeMeters, 1.0f);
    const float cellSizeMeters = terrainSizeMeters / static_cast<float>(std::max<UINT>(1, resolution - 1));

    // Talus angle from viscosity (matches CPU: viscosity² × 80°).
    const float viscosity = std::clamp(settings.sedimentViscosity, 0.0f, 1.0f);
    const float talusAngleDeg = viscosity * viscosity * 80.0f;
    const float talusH = std::tan(talusAngleDeg * 3.14159265358979323846f / 180.0f) * cellSizeMeters;

    const float largestM = std::clamp(settings.largestDetailLevelM, cellSizeMeters, terrainSizeMeters * 0.5f);
    const int macroPasses = std::max(1, static_cast<int>(std::ceil(largestM / cellSizeMeters)));

    const int iterations = std::max(1, settings.iterations);
    const int stabIter = std::max(1, settings.stabilizationIterations);
    const float emissionAmount = std::max(0.0f, settings.emissionAmountM);
    const float emissionTime = std::clamp(settings.emissionTime, 0.0f, 1.0f);
    const int emissionEnd = std::max(1,
        static_cast<int>(std::ceil(static_cast<float>(iterations) * emissionTime)));
    const float emissionPerIter = emissionAmount / static_cast<float>(emissionEnd);

    const UINT groupCount = (resolution + 7u) / 8u;

    auto setConstants = [&](float talusHValue, float emitValue, UINT convertFlag) {
        SedimentShaderConstants k{};
        k.resolution = resolution;
        k.talusH = talusHValue;
        k.emissionPerIter = emitValue;
        k.convertTerrainToSediment = convertFlag;
        commandList->SetComputeRoot32BitConstants(0, 4, &k, 0);
    };
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &barrier);
    };

    // 1. Setup: write bedrock and sediment from input heights.
    setConstants(talusH, 0.0f, settings.convertTerrainToSediment ? 1u : 0u);
    commandList->SetPipelineState(g_sedimentSetupPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    for (int iter = 0; iter < iterations; ++iter)
    {
        if (iter < emissionEnd && emissionPerIter > 0.0f)
        {
            setConstants(talusH, emissionPerIter, 0u);
            commandList->SetPipelineState(g_sedimentEmitPso.Get());
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();
        }

        const int passes = macroPasses * stabIter;
        for (int p = 0; p < passes; ++p)
        {
            // Sweep 1: compute outgoing shares.
            setConstants(talusH, 0.0f, 0u);
            commandList->SetPipelineState(g_sedimentSweep1Pso.Get());
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();
            // Sweep 2: apply self-out − incoming-from-neighbours.
            commandList->SetPipelineState(g_sedimentSweep2Pso.Get());
            commandList->Dispatch(groupCount, groupCount, 1);
            uavBarrier();
        }
    }

    // Read sediment + bedrock back to CPU. Heights and mask are
    // assembled host-side (need max sediment for mask normalisation).
    D3D12_RESOURCE_BARRIER toCopySrc[2]{};
    ID3D12Resource* copyResources[2] = {sedimentBuf.Get(), bedrockBuf.Get()};
    for (int i = 0; i < 2; ++i)
    {
        toCopySrc[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySrc[i].Transition.pResource = copyResources[i];
        toCopySrc[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopySrc[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySrc[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(2, toCopySrc);
    commandList->CopyBufferRegion(readbackSediment.Get(), 0, sedimentBuf.Get(), 0, fieldByteSize);
    commandList->CopyBufferRegion(readbackBedrock.Get(),  0, bedrockBuf.Get(),  0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close Sediment command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal Sediment fence failed");
    WaitForFenceValue(fenceValue);

    void* mappedSed = nullptr;
    void* mappedRock = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackSediment->Map(0, &readRange, &mappedSed), "Map Sediment readback (sediment) failed");
    ThrowIfFailed(readbackBedrock->Map(0, &readRange, &mappedRock), "Map Sediment readback (bedrock) failed");
    const float* sedimentValues = static_cast<const float*>(mappedSed);
    const float* bedrockValues  = static_cast<const float*>(mappedRock);

    grid.heights.assign(static_cast<size_t>(cellCount), 0.0f);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    // 95th percentile normalisation (matches the CPU path). Avoids the
    // single-deepest-pile compressing the rest of the map to near-zero.
    std::vector<float> sortedSediment(sedimentValues, sedimentValues + cellCount);
    const size_t pIndex = std::min(static_cast<size_t>(cellCount - 1), (static_cast<size_t>(cellCount) * 95u) / 100u);
    std::nth_element(sortedSediment.begin(), sortedSediment.begin() + pIndex, sortedSediment.end());
    const float maskNorm = std::max(sortedSediment[pIndex], 1e-4f);
    const float halfBand = std::max((1.0f - std::clamp(settings.maskContrast, 0.0f, 1.0f)) * 0.5f, 0.005f);
    const float maskLo = 0.5f - halfBand;
    const float maskHi = 0.5f + halfBand;
    for (size_t i = 0; i < cellCount; ++i)
    {
        grid.heights[i] = bedrockValues[i] + sedimentValues[i];
        const float t = std::clamp((sedimentValues[i] / maskNorm - maskLo) / (maskHi - maskLo), 0.0f, 1.0f);
        grid.mask[i] = t * t * (3.0f - 2.0f * t);
    }
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackSediment->Unmap(0, &emptyWriteRange);
    readbackBedrock->Unmap(0, &emptyWriteRange);

    g_sedimentComputeStatus = "Sediment GPU Compute evaluated";
    return true;
}

bool RunSedimentCompute(rock::HeightfieldGrid& grid, const rock::SedimentSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunSedimentComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<SedimentGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<SedimentGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_sedimentGpuRequestMutex);
        g_pendingSedimentGpuRequests.push_back(request);
    }
    g_sedimentComputeStatus = "Sediment GPU Compute queued on main thread";

    SedimentGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingSedimentGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<SedimentGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_sedimentGpuRequestMutex);
        requests.swap(g_pendingSedimentGpuRequests);
    }

    for (const std::shared_ptr<SedimentGpuRequest>& request : requests)
    {
        SedimentGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunSedimentComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

// Mirrors the cbuffer in shaders/rock_compute.hlsl.
struct RockShaderConstants
{
    UINT  resolution;
    int   seed;
    float terrainSizeMeters;
    float density;

    float coverage;
    float rockSizeMinCells;
    float rockSizeMaxCells;
    float rockHeight;

    float heightJitter;
    float rotationVar;
    float aspectVar;
    float edgeSharpness;

    float bumpiness;
    float facetSharpness;
    float facetScale;
    int   searchRadius;

    float maxReach;
    float domeExp;
    int   needPolyhedral;
    int   rockStyle;

    int   orientationRule;
    int   layerCount;
    int   pad2;
    int   pad3;
};
static_assert(sizeof(RockShaderConstants) == 24 * sizeof(UINT), "RockShaderConstants must be 24 DWORDs");

bool EnsureRockComputePipeline(std::string* error)
{
    if (g_rockComputeReady && g_rockComputeRootSignature && g_rockComputePso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_rockComputeStatus = "Rock GPU Compute unavailable";
        return false;
    }

    // 4 UAVs (inputHeights, outputHeights, outputMask, outputUniqueMask)
    // bound as one descriptor table at u0..u3.
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 4;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 24;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize Rock root sig failed";
        g_rockComputeStatus = "Rock GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rockComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create Rock root sig failed";
        g_rockComputeStatus = "Rock GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = RockComputeShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> csBlob;
    errBlob.Reset();
    const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                 "CSRock", "cs_5_0", compileFlags, 0, &csBlob, &errBlob);
    if (FAILED(compileHr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Rock shader failed";
        g_rockComputeStatus = "Rock shader compile failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_rockComputeRootSignature.Get();
    psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
    const HRESULT psoHr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_rockComputePso));
    if (FAILED(psoHr))
    {
        if (error) *error = "Create Rock PSO failed";
        g_rockComputeStatus = "Rock PSO failed";
        return false;
    }

    g_rockComputeReady = true;
    g_rockComputeStatus = "Rock GPU Compute dispatch ready";
    return true;
}

bool RunRockComputeImmediate(rock::HeightfieldGrid& grid, const rock::RockSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_rockComputeMutex);
    if (!EnsureRockComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    if (resolution < 2 || grid.heights.size() < cellCount || settings.density <= 0.0f)
    {
        if (error) *error = "Invalid heightfield for Rock GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize = cellCount * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc = BufferResourceDesc(fieldByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc = BufferResourceDesc(fieldByteSize);

    ComPtr<ID3D12Resource> inputHeightsBuf, outputHeightsBuf, outputMaskBuf, outputUniqueMaskBuf;
    ComPtr<ID3D12Resource> uploadHeights;
    ComPtr<ID3D12Resource> readbackHeights, readbackMask, readbackUniqueMask;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(inputHeightsBuf,  fieldGpuDesc, "Rock input-heights buffer"))  return false;
    if (!createDefault(outputHeightsBuf, fieldGpuDesc, "Rock output-heights buffer")) return false;
    if (!createDefault(outputMaskBuf,    fieldGpuDesc, "Rock output-mask buffer"))    return false;
    if (!createDefault(outputUniqueMaskBuf, fieldGpuDesc, "Rock output-unique-mask buffer")) return false;
    if (!createUpload(uploadHeights,     "Rock upload heights"))                      return false;
    if (!createReadback(readbackHeights, "Rock readback heights"))                    return false;
    if (!createReadback(readbackMask,    "Rock readback mask"))                       return false;
    if (!createReadback(readbackUniqueMask, "Rock readback unique mask"))             return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map Rock heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 4;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kDescriptorCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Rock descriptor heap failed"; return false; }

    auto createUav = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * g_srvDescriptorSize;
        g_device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    createUav(inputHeightsBuf.Get(),  static_cast<UINT>(cellCount), 0);
    createUav(outputHeightsBuf.Get(), static_cast<UINT>(cellCount), 1);
    createUav(outputMaskBuf.Get(),    static_cast<UINT>(cellCount), 2);
    createUav(outputUniqueMaskBuf.Get(), static_cast<UINT>(cellCount), 3);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Rock command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Rock command list failed");

    // Stage input heights: UAV → COPY_DEST → CopyBufferRegion → UAV.
    D3D12_RESOURCE_BARRIER toCopyDest{};
    toCopyDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopyDest.Transition.pResource = inputHeightsBuf.Get();
    toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toCopyDest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &toCopyDest);
    commandList->CopyBufferRegion(inputHeightsBuf.Get(), 0, uploadHeights.Get(), 0, fieldByteSize);
    D3D12_RESOURCE_BARRIER toUav = toCopyDest;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    commandList->ResourceBarrier(1, &toUav);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_rockComputeRootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    // Constants — mirror CPU-side derivations (ApplyRock).
    const float density = std::max(settings.density, 0.1f);
    const float rockSizeMinM = std::clamp(settings.rockSizeMinM, 0.1f, 200.0f);
    const float rockSizeMaxM = std::clamp(std::max(settings.rockSizeMaxM, rockSizeMinM), 0.1f, 200.0f);
    const float rockSizeMinCells = rockSizeMinM / density;
    const float rockSizeMaxCells = rockSizeMaxM / density;
    const float aspectVar = std::clamp(settings.aspectVariation, 0.0f, 1.0f);
    const float maxDomeRadius = rockSizeMaxCells * 0.5f;
    const float edgeSharpness = std::clamp(settings.edgeSharpness, 0.0f, 1.0f);
    const float facetSharpness = std::clamp(settings.facetSharpness, 0.0f, 1.0f);
    const int rockStyle = std::clamp(static_cast<int>(settings.style), 0, 2);
    const bool polygonalStyle = rockStyle != static_cast<int>(rock::RockStyle::Classic);
    const bool shardStyle = rockStyle == static_cast<int>(rock::RockStyle::Shard);
    const float styleAspectBoost = shardStyle ? 0.65f : 0.0f;
    const float maxAspect = std::pow(2.0f, aspectVar + styleAspectBoost);
    const float maxReach = maxDomeRadius * maxAspect;
    const int searchRadius = std::max(1, static_cast<int>(std::ceil(maxReach - 0.05f)));
    const float effectiveEdgeSharpness = polygonalStyle ? std::max(edgeSharpness, 0.65f) : edgeSharpness;
    const float domeExp = polygonalStyle ? 1.0f : (1.0f + facetSharpness * 1.5f * (1.0f - edgeSharpness));
    const int orientationRule = std::clamp(static_cast<int>(settings.orientationRule), 0, 2);
    const int layerCount = std::clamp(settings.layerCount, 1, 8);

    RockShaderConstants k{};
    k.resolution        = resolution;
    k.seed              = settings.seed;
    k.terrainSizeMeters = std::max(grid.terrainSizeMeters, 1.0f);
    k.density           = density;
    k.coverage          = std::clamp(settings.coverage, 0.0f, 1.0f);
    k.rockSizeMinCells  = rockSizeMinCells;
    k.rockSizeMaxCells  = rockSizeMaxCells;
    k.rockHeight        = std::max(settings.rockHeight, 0.0f);
    k.heightJitter      = std::clamp(settings.heightJitter, 0.0f, 1.0f);
    k.rotationVar       = std::clamp(settings.rotationVariation, 0.0f, 1.0f);
    k.aspectVar         = aspectVar;
    k.edgeSharpness     = edgeSharpness;
    k.bumpiness         = std::clamp(settings.bumpiness, 0.0f, 1.0f);
    k.facetSharpness    = facetSharpness;
    k.facetScale        = std::clamp(settings.facetScale, 0.5f, 8.0f);
    k.searchRadius      = searchRadius;
    k.maxReach          = maxReach;
    k.domeExp           = domeExp;
    k.needPolyhedral    = (effectiveEdgeSharpness > 0.0f) ? 1 : 0;
    k.rockStyle         = rockStyle;
    k.orientationRule   = orientationRule;
    k.layerCount        = layerCount;
    k.pad2              = 0;
    k.pad3              = 0;
    commandList->SetComputeRoot32BitConstants(0, 24, &k, 0);

    commandList->SetPipelineState(g_rockComputePso.Get());
    const UINT groupCount = (resolution + 7u) / 8u;
    commandList->Dispatch(groupCount, groupCount, 1);

    D3D12_RESOURCE_BARRIER uavBar{};
    uavBar.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBar.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &uavBar);

    // Read back outputHeights + outputMask + outputUniqueMask.
    D3D12_RESOURCE_BARRIER toCopySrc[3]{};
    ID3D12Resource* copyResources[3] = {outputHeightsBuf.Get(), outputMaskBuf.Get(), outputUniqueMaskBuf.Get()};
    for (int i = 0; i < 3; ++i)
    {
        toCopySrc[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySrc[i].Transition.pResource = copyResources[i];
        toCopySrc[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopySrc[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySrc[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(3, toCopySrc);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, outputHeightsBuf.Get(), 0, fieldByteSize);
    commandList->CopyBufferRegion(readbackMask.Get(),    0, outputMaskBuf.Get(),    0, fieldByteSize);
    commandList->CopyBufferRegion(readbackUniqueMask.Get(), 0, outputUniqueMaskBuf.Get(), 0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close Rock command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal Rock fence failed");
    WaitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedMask = nullptr;
    void* mappedUniqueMask = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map Rock readback (heights) failed");
    ThrowIfFailed(readbackMask->Map(0, &readRange, &mappedMask),       "Map Rock readback (mask) failed");
    ThrowIfFailed(readbackUniqueMask->Map(0, &readRange, &mappedUniqueMask), "Map Rock readback (unique mask) failed");
    std::memcpy(grid.heights.data(), mappedHeights, fieldByteSize);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.mask.data(), mappedMask, fieldByteSize);
    grid.uniqueMask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.uniqueMask.data(), mappedUniqueMask, fieldByteSize);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackMask->Unmap(0, &emptyWriteRange);
    readbackUniqueMask->Unmap(0, &emptyWriteRange);

    g_rockComputeStatus = "Rock GPU Compute evaluated";
    return true;
}

bool RunRockCompute(rock::HeightfieldGrid& grid, const rock::RockSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunRockComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<RockGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<RockGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_rockGpuRequestMutex);
        g_pendingRockGpuRequests.push_back(request);
    }
    g_rockComputeStatus = "Rock GPU Compute queued on main thread";

    RockGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingRockGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<RockGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_rockGpuRequestMutex);
        requests.swap(g_pendingRockGpuRequests);
    }

    for (const std::shared_ptr<RockGpuRequest>& request : requests)
    {
        RockGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunRockComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

// Mirrors the cbuffer in shaders/mask_fluvial_compute.hlsl.
struct MaskFluvialShaderConstants
{
    UINT  resolution;
    UINT  algorithmIsMfd;
    float mfdExponent;
    UINT  accumDirection;

    float thresholdCells;
    float gamma;
    float softness;
    float power;

    UINT  outputCurve;
    float inertia;
    UINT  detailBlurRadius;
    UINT  pad0;
    UINT  pad1;
    UINT  pad2;
    UINT  pad3;
    UINT  pad4;
};
static_assert(sizeof(MaskFluvialShaderConstants) == 16 * sizeof(UINT), "MaskFluvialShaderConstants must be 16 DWORDs");

bool EnsureMaskFluvialComputePipeline(std::string* error)
{
    if (g_maskFluvialComputeReady && g_maskFluvialComputeRootSignature
        && g_mfPitFillPso && g_mfCommitHeightsPso && g_mfCopyInputHeightsPso
        && g_mfBlurHorizontalPso && g_mfBlurVerticalPso
        && g_mfComputeWeightsPso && g_mfAccumInitPso && g_mfAccumIterPso
        && g_mfMaxReducePso && g_mfToMaskLogPso && g_mfToMaskLinearPso && g_mfToMaskThresholdPso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_maskFluvialComputeStatus = "Mask Fluvial GPU Compute unavailable";
        return false;
    }

    // 8 UAVs (Heights, HeightsScratch, Weights, AccumA, AccumB, OutMask, MaxScratch, InputHeights).
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 8;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 16;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize Mask Fluvial root sig failed";
        g_maskFluvialComputeStatus = "Mask Fluvial GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_maskFluvialComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create Mask Fluvial root sig failed";
        g_maskFluvialComputeStatus = "Mask Fluvial GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = MaskFluvialComputeShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Mask Fluvial shader failed";
            return false;
        }
        return true;
    };

    auto buildPso = [&](ID3DBlob* csBlob, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_maskFluvialComputeRootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        const HRESULT psoHr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(psoHr))
        {
            if (error) *error = "Create Mask Fluvial PSO failed";
            return false;
        }
        return true;
    };

    struct Entry { const char* name; ComPtr<ID3D12PipelineState>* pso; };
    Entry entries[] = {
        {"CSCopyInputHeights", &g_mfCopyInputHeightsPso},
        {"CSBlurHorizontal",    &g_mfBlurHorizontalPso},
        {"CSBlurVertical",      &g_mfBlurVerticalPso},
        {"CSPitFillJacobi",    &g_mfPitFillPso},
        {"CSCommitHeights",    &g_mfCommitHeightsPso},
        {"CSComputeWeights",   &g_mfComputeWeightsPso},
        {"CSAccumInit",        &g_mfAccumInitPso},
        {"CSAccumIter",        &g_mfAccumIterPso},
        {"CSMaxReduce",        &g_mfMaxReducePso},
        {"CSToMaskLog",        &g_mfToMaskLogPso},
        {"CSToMaskLinear",     &g_mfToMaskLinearPso},
        {"CSToMaskThreshold",  &g_mfToMaskThresholdPso},
    };
    for (const Entry& e : entries)
    {
        ComPtr<ID3DBlob> blob;
        if (!compileEntry(e.name, blob))
        {
            g_maskFluvialComputeStatus = std::string("Mask Fluvial ") + e.name + " compile failed";
            return false;
        }
        if (!buildPso(blob.Get(), *e.pso))
        {
            g_maskFluvialComputeStatus = std::string("Mask Fluvial ") + e.name + " PSO failed";
            return false;
        }
    }

    g_maskFluvialComputeReady = true;
    g_maskFluvialComputeStatus = "Mask Fluvial GPU Compute dispatch ready";
    return true;
}

bool RunMaskFluvialComputeImmediate(rock::HeightfieldGrid& grid, const rock::MaskFluvialSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_maskFluvialComputeMutex);
    if (!EnsureMaskFluvialComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    if (resolution < 3 || grid.heights.size() < cellCount)
    {
        if (error) *error = "Invalid heightfield for Mask Fluvial GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize    = cellCount * sizeof(float);
    const UINT64 weightsByteSize  = cellCount * 8ull * sizeof(float);
    const UINT64 maxScratchBytes  = sizeof(UINT);

    const D3D12_HEAP_PROPERTIES defaultHeap  = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap   = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc   = BufferResourceDesc(fieldByteSize,    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC weightsGpuDesc = BufferResourceDesc(weightsByteSize,  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC scratchGpuDesc = BufferResourceDesc(maxScratchBytes,  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc   = BufferResourceDesc(fieldByteSize);
    const D3D12_RESOURCE_DESC scratchCpuDesc = BufferResourceDesc(maxScratchBytes);

    ComPtr<ID3D12Resource> heightsBuf, heightsScratchBuf, weightsBuf, accumABuf, accumBBuf, outMaskBuf, maxScratchBuf, inputHeightsBuf;
    ComPtr<ID3D12Resource> uploadHeights, uploadMaxScratch, readbackMask;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(heightsBuf,        fieldGpuDesc,   "MF heights"))         return false;
    if (!createDefault(heightsScratchBuf, fieldGpuDesc,   "MF heights scratch")) return false;
    if (!createDefault(weightsBuf,        weightsGpuDesc, "MF weights"))         return false;
    if (!createDefault(accumABuf,         fieldGpuDesc,   "MF accum A"))         return false;
    if (!createDefault(accumBBuf,         fieldGpuDesc,   "MF accum B"))         return false;
    if (!createDefault(outMaskBuf,        fieldGpuDesc,   "MF out mask"))        return false;
    if (!createDefault(maxScratchBuf,     scratchGpuDesc, "MF max scratch"))     return false;
    if (!createDefault(inputHeightsBuf,   fieldGpuDesc,   "MF input heights"))   return false;
    if (!createUpload(uploadHeights,      fieldCpuDesc,   "MF upload heights"))  return false;
    if (!createUpload(uploadMaxScratch,   scratchCpuDesc, "MF upload max"))      return false;
    if (!createReadback(readbackMask,     fieldCpuDesc,   "MF readback mask"))   return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map MF heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    // Upload zero into MaxScratch initial value (atomic max baseline).
    ThrowIfFailed(uploadMaxScratch->Map(0, &emptyReadRange, &mapped), "Map MF max scratch upload failed");
    *static_cast<UINT*>(mapped) = 0u;
    uploadMaxScratch->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 8;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kDescriptorCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create MF descriptor heap failed"; return false; }

    auto createUavFloat = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * g_srvDescriptorSize;
        g_device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    auto createUavUint = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(UINT);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * g_srvDescriptorSize;
        g_device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    createUavFloat(heightsBuf.Get(),         static_cast<UINT>(cellCount),     0);
    createUavFloat(heightsScratchBuf.Get(),  static_cast<UINT>(cellCount),     1);
    createUavFloat(weightsBuf.Get(),         static_cast<UINT>(cellCount * 8u), 2);
    createUavFloat(accumABuf.Get(),          static_cast<UINT>(cellCount),     3);
    createUavFloat(accumBBuf.Get(),          static_cast<UINT>(cellCount),     4);
    createUavFloat(outMaskBuf.Get(),         static_cast<UINT>(cellCount),     5);
    createUavUint (maxScratchBuf.Get(),      1u,                               6);
    createUavFloat(inputHeightsBuf.Get(),    static_cast<UINT>(cellCount),     7);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create MF command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create MF command list failed");

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &b);
    };
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &b);
    };

    // Stage input heights.
    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(inputHeightsBuf.Get(), 0, uploadHeights.Get(), 0, fieldByteSize);
    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Initialise MaxScratch to 0 (so atomic max accumulates from baseline).
    transition(maxScratchBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(maxScratchBuf.Get(), 0, uploadMaxScratch.Get(), 0, maxScratchBytes);
    transition(maxScratchBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_maskFluvialComputeRootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    const UINT groupCount = (resolution + 7u) / 8u;
    const float thresholdCells = std::clamp(settings.accumulationThreshold, 0.0f, 1.0f) * static_cast<float>(cellCount);

    MaskFluvialShaderConstants k{};
    k.resolution      = resolution;
    k.algorithmIsMfd  = 1u;
    k.mfdExponent     = std::clamp(settings.mfdExponent, 0.1f, 16.0f);
    k.accumDirection  = 0u;
    k.thresholdCells  = thresholdCells;
    k.gamma           = std::clamp(settings.gamma, 0.05f, 8.0f);
    k.softness        = std::clamp(settings.softness, 0.001f, 4.0f);
    k.power           = std::clamp(settings.power, 0.1f, 8.0f);
    k.outputCurve     = static_cast<UINT>(settings.outputCurve);
    k.inertia         = 0.0f;
    const float cellSizeMeters = std::max(grid.terrainSizeMeters, 1.0f) / static_cast<float>(std::max(1u, resolution - 1u));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSizeMeters, std::max(grid.terrainSizeMeters, 1.0f) * 0.5f);
    k.detailBlurRadius = static_cast<UINT>(std::clamp(static_cast<int>(std::round(largestDetailM / cellSizeMeters)), 1, 64));
    auto setConstants = [&]() {
        commandList->SetComputeRoot32BitConstants(0, 16, &k, 0);
    };

    // 1. Copy input heights into the working Heights buffer.
    setConstants();
    commandList->SetPipelineState(g_mfCopyInputHeightsPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    // 2. Low-pass the analysis heights according to Largest Detail Level.
    if (k.detailBlurRadius > 1u)
    {
        commandList->SetPipelineState(g_mfBlurHorizontalPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
        commandList->SetPipelineState(g_mfBlurVerticalPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }

    // 3. Pit fill iterations (Jacobi double-buffer + commit).
    const int pitIters = rock::MaskFluvialSettings{}.pitFillIterations;
    for (int i = 0; i < pitIters; ++i)
    {
        commandList->SetPipelineState(g_mfPitFillPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
        commandList->SetPipelineState(g_mfCommitHeightsPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }

    // 4. Compute receivers / weights from final Heights.
    commandList->SetPipelineState(g_mfComputeWeightsPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    // 5. Initialise AccumA = 1.0.
    commandList->SetPipelineState(g_mfAccumInitPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    // 6. Iterative Jacobi gather. K = 2 * resolution iterations, even so
    //    the final result lands in AccumA. Direction alternates each iter.
    const int accumIters = static_cast<int>(resolution) * 2;
    for (int i = 0; i < accumIters; ++i)
    {
        k.accumDirection = static_cast<UINT>(i & 1);
        setConstants();
        commandList->SetPipelineState(g_mfAccumIterPso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }
    // Reset accumDirection so subsequent dispatches behave deterministically.
    k.accumDirection = 0u;
    setConstants();

    // 7. For Log/Linear: reduce max(adjusted) into MaxScratch[0].
    if (settings.outputCurve != rock::MaskFluvialOutputCurve::Threshold)
    {
        commandList->SetPipelineState(g_mfMaxReducePso.Get());
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }

    // 8. Mask conversion.
    ID3D12PipelineState* maskPso = g_mfToMaskLogPso.Get();
    if (settings.outputCurve == rock::MaskFluvialOutputCurve::Threshold) maskPso = g_mfToMaskThresholdPso.Get();
    else if (settings.outputCurve == rock::MaskFluvialOutputCurve::Linear) maskPso = g_mfToMaskLinearPso.Get();
    commandList->SetPipelineState(maskPso);
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    // 9. Read back OutMask.
    transition(outMaskBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->CopyBufferRegion(readbackMask.Get(), 0, outMaskBuf.Get(), 0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close MF command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal MF fence failed");
    WaitForFenceValue(fenceValue);

    void* mappedMask = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackMask->Map(0, &readRange, &mappedMask), "Map MF readback mask failed");
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.mask.data(), mappedMask, fieldByteSize);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackMask->Unmap(0, &emptyWriteRange);

    // Mask Fluvial does not modify heights — heights pass through unchanged.

    g_maskFluvialComputeStatus = "Mask Fluvial GPU Compute evaluated";
    return true;
}

bool RunMaskFluvialCompute(rock::HeightfieldGrid& grid, const rock::MaskFluvialSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunMaskFluvialComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<MaskFluvialGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<MaskFluvialGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_maskFluvialGpuRequestMutex);
        g_pendingMaskFluvialGpuRequests.push_back(request);
    }
    g_maskFluvialComputeStatus = "Mask Fluvial GPU Compute queued on main thread";

    MaskFluvialGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingMaskFluvialGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<MaskFluvialGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_maskFluvialGpuRequestMutex);
        requests.swap(g_pendingMaskFluvialGpuRequests);
    }

    for (const std::shared_ptr<MaskFluvialGpuRequest>& request : requests)
    {
        MaskFluvialGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunMaskFluvialComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

// Mirrors the cbuffer in shaders/snow_compute.hlsl.
struct SnowShaderConstants
{
    UINT  resolution;
    float terrainSizeMeters;
    float emissionAmount;
    float minTan;

    float invRange;
    float maskMaxSnow;
    UINT  smoothDirection;
    UINT  fillRadius;

    UINT  pad0;
    UINT  pad1;
    UINT  pad2;
    UINT  pad3;
};
static_assert(sizeof(SnowShaderConstants) == 12 * sizeof(UINT), "SnowShaderConstants must be 12 DWORDs");

bool EnsureSnowComputePipeline(std::string* error)
{
    if (g_snowComputeReady && g_snowComputeRootSignature
        && g_snowCopyInputHeightsPso && g_snowComputeThicknessPso
        && g_snowEnvelopeSmoothingPso && g_snowApplyPso)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_snowComputeStatus = "Snow GPU Compute unavailable";
        return false;
    }

    // 7 UAVs (InputHeights, BaseHeights, Thickness, SurfA, SurfB, OutHeights, OutMask)
    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 7;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = 12;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize Snow root sig failed";
        g_snowComputeStatus = "Snow GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_snowComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create Snow root sig failed";
        g_snowComputeStatus = "Snow GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = SnowComputeShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    auto compileEntry = [&](const char* entryPoint, ComPtr<ID3DBlob>& outBlob) -> bool {
        errBlob.Reset();
        const HRESULT compileHr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                                     entryPoint, "cs_5_0", compileFlags, 0, &outBlob, &errBlob);
        if (FAILED(compileHr))
        {
            if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile Snow shader failed";
            return false;
        }
        return true;
    };

    auto buildPso = [&](ID3DBlob* csBlob, ComPtr<ID3D12PipelineState>& outPso) -> bool {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = g_snowComputeRootSignature.Get();
        psoDesc.CS = {csBlob->GetBufferPointer(), csBlob->GetBufferSize()};
        const HRESULT psoHr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&outPso));
        if (FAILED(psoHr))
        {
            if (error) *error = "Create Snow PSO failed";
            return false;
        }
        return true;
    };

    struct Entry { const char* name; ComPtr<ID3D12PipelineState>* pso; };
    Entry entries[] = {
        {"CSCopyInputHeights",   &g_snowCopyInputHeightsPso},
        {"CSComputeThickness",   &g_snowComputeThicknessPso},
        {"CSEnvelopeSmoothing",  &g_snowEnvelopeSmoothingPso},
        {"CSApply",              &g_snowApplyPso},
    };
    for (const Entry& e : entries)
    {
        ComPtr<ID3DBlob> blob;
        if (!compileEntry(e.name, blob))
        {
            g_snowComputeStatus = std::string("Snow ") + e.name + " compile failed";
            return false;
        }
        if (!buildPso(blob.Get(), *e.pso))
        {
            g_snowComputeStatus = std::string("Snow ") + e.name + " PSO failed";
            return false;
        }
    }

    g_snowComputeReady = true;
    g_snowComputeStatus = "Snow GPU Compute dispatch ready";
    return true;
}

bool RunSnowComputeImmediate(rock::HeightfieldGrid& grid, const rock::SnowSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_snowComputeMutex);
    if (!EnsureSnowComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    if (resolution < 2 || grid.heights.size() < cellCount)
    {
        if (error) *error = "Invalid heightfield for Snow GPU Compute";
        return false;
    }

    const UINT64 fieldByteSize = cellCount * sizeof(float);

    const D3D12_HEAP_PROPERTIES defaultHeap  = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap   = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC fieldGpuDesc   = BufferResourceDesc(fieldByteSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC fieldCpuDesc   = BufferResourceDesc(fieldByteSize);

    ComPtr<ID3D12Resource> inputHeightsBuf, baseHeightsBuf, thicknessBuf, surfABuf, surfBBuf, outHeightsBuf, outMaskBuf;
    ComPtr<ID3D12Resource> uploadHeights, readbackHeights, readbackMask;

    auto createDefault = [&](ComPtr<ID3D12Resource>& out, const D3D12_RESOURCE_DESC& desc, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createUpload = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };
    auto createReadback = [&](ComPtr<ID3D12Resource>& out, const char* name) -> bool {
        const HRESULT hrLocal = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &fieldCpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hrLocal)) { if (error) *error = std::string("Create ") + name + " failed"; return false; }
        return true;
    };

    if (!createDefault(inputHeightsBuf, fieldGpuDesc, "Snow input heights"))  return false;
    if (!createDefault(baseHeightsBuf,  fieldGpuDesc, "Snow base heights"))   return false;
    if (!createDefault(thicknessBuf,    fieldGpuDesc, "Snow thickness"))      return false;
    if (!createDefault(surfABuf,        fieldGpuDesc, "Snow surfA"))          return false;
    if (!createDefault(surfBBuf,        fieldGpuDesc, "Snow surfB"))          return false;
    if (!createDefault(outHeightsBuf,   fieldGpuDesc, "Snow out heights"))    return false;
    if (!createDefault(outMaskBuf,      fieldGpuDesc, "Snow out mask"))       return false;
    if (!createUpload(uploadHeights,    "Snow upload heights"))               return false;
    if (!createReadback(readbackHeights,"Snow readback heights"))             return false;
    if (!createReadback(readbackMask,   "Snow readback mask"))                return false;

    void* mapped = nullptr;
    const D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map Snow heights upload failed");
    std::memcpy(mapped, grid.heights.data(), fieldByteSize);
    uploadHeights->Unmap(0, nullptr);

    constexpr UINT kDescriptorCount = 7;
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = kDescriptorCount;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create Snow descriptor heap failed"; return false; }

    auto createUav = [&](ID3D12Resource* res, UINT numElements, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = numElements;
        uavDesc.Buffer.StructureByteStride = sizeof(float);
        D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * g_srvDescriptorSize;
        g_device->CreateUnorderedAccessView(res, nullptr, &uavDesc, handle);
    };
    createUav(inputHeightsBuf.Get(), static_cast<UINT>(cellCount), 0);
    createUav(baseHeightsBuf.Get(),  static_cast<UINT>(cellCount), 1);
    createUav(thicknessBuf.Get(),    static_cast<UINT>(cellCount), 2);
    createUav(surfABuf.Get(),        static_cast<UINT>(cellCount), 3);
    createUav(surfBBuf.Get(),        static_cast<UINT>(cellCount), 4);
    createUav(outHeightsBuf.Get(),   static_cast<UINT>(cellCount), 5);
    createUav(outMaskBuf.Get(),      static_cast<UINT>(cellCount), 6);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create Snow command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create Snow command list failed");

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &b);
    };
    auto uavBarrier = [&]() {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &b);
    };

    // Stage input heights
    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyBufferRegion(inputHeightsBuf.Get(), 0, uploadHeights.Get(), 0, fieldByteSize);
    transition(inputHeightsBuf.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_snowComputeRootSignature.Get());
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());

    const float kPi = 3.14159265358979323846f;
    const float minDeg = std::clamp(settings.slopeLimitMinDeg, 0.0f, 89.9f);
    const float maxDeg = std::clamp(std::max(settings.slopeLimitMaxDeg, settings.slopeLimitMinDeg), 0.0f, 89.9f);
    const float minTan = std::tan(minDeg * (kPi / 180.0f));
    const float maxTan = std::tan(maxDeg * (kPi / 180.0f));

    SnowShaderConstants k{};
    k.resolution        = resolution;
    k.terrainSizeMeters = std::max(grid.terrainSizeMeters, 1.0f);
    k.emissionAmount    = std::max(0.0f, settings.emissionAmount);
    k.minTan            = minTan;
    k.invRange          = 1.0f / std::max(maxTan - minTan, 1e-6f);
    k.maskMaxSnow       = std::max(1e-4f, settings.maskMaxSnow);
    k.smoothDirection   = 0u;
    const float cellSizeMeters = k.terrainSizeMeters / static_cast<float>(std::max(1u, resolution - 1u));
    const float largestDetailM = std::clamp(settings.largestDetailLevelM, cellSizeMeters, k.terrainSizeMeters * 0.5f);
    k.fillRadius        = static_cast<UINT>(std::clamp(static_cast<int>(std::round(largestDetailM / cellSizeMeters)), 1, 64));
    k.pad0              = 0u;
    k.pad1              = 0u;
    k.pad2              = 0u;
    k.pad3              = 0u;
    auto setConstants = [&]() {
        commandList->SetComputeRoot32BitConstants(0, 12, &k, 0);
    };
    setConstants();

    const UINT groupCount = (resolution + 7u) / 8u;

    // 1. Copy InputHeights → BaseHeights
    commandList->SetPipelineState(g_snowCopyInputHeightsPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    // 2. Compute initial thickness + initial SurfA = base + thickness
    commandList->SetPipelineState(g_snowComputeThicknessPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    // 3. Envelope smoothing iterations. Each iteration builds a separable
    //    gaussian blur, then applies max(original surface, blurred) once.
    int rawIters = std::clamp(settings.smoothingIterations, 0, 16);
    int smoothIters = rawIters;
    commandList->SetPipelineState(g_snowEnvelopeSmoothingPso.Get());
    for (int i = 0; i < smoothIters; ++i)
    {
        k.smoothDirection = 0u;
        setConstants();
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
        k.smoothDirection = 1u;
        setConstants();
        commandList->Dispatch(groupCount, groupCount, 1);
        uavBarrier();
    }
    k.smoothDirection = 0u;
    setConstants();

    // 4. Apply: thickness = SurfA - BaseHeights, write OutHeights + OutMask
    commandList->SetPipelineState(g_snowApplyPso.Get());
    commandList->Dispatch(groupCount, groupCount, 1);
    uavBarrier();

    // Read back outputs
    D3D12_RESOURCE_BARRIER toCopySrc[2]{};
    ID3D12Resource* copyResources[2] = {outHeightsBuf.Get(), outMaskBuf.Get()};
    for (int i = 0; i < 2; ++i)
    {
        toCopySrc[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopySrc[i].Transition.pResource = copyResources[i];
        toCopySrc[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopySrc[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopySrc[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(2, toCopySrc);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, outHeightsBuf.Get(), 0, fieldByteSize);
    commandList->CopyBufferRegion(readbackMask.Get(),    0, outMaskBuf.Get(),    0, fieldByteSize);
    ThrowIfFailed(commandList->Close(), "Close Snow command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal Snow fence failed");
    WaitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedMask = nullptr;
    const D3D12_RANGE readRange{0, static_cast<SIZE_T>(fieldByteSize)};
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map Snow readback heights failed");
    ThrowIfFailed(readbackMask->Map(0, &readRange, &mappedMask),       "Map Snow readback mask failed");
    std::memcpy(grid.heights.data(), mappedHeights, fieldByteSize);
    grid.mask.assign(static_cast<size_t>(cellCount), 0.0f);
    std::memcpy(grid.mask.data(), mappedMask, fieldByteSize);
    const D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackMask->Unmap(0, &emptyWriteRange);

    g_snowComputeStatus = "Snow GPU Compute evaluated";
    return true;
}

bool RunSnowCompute(rock::HeightfieldGrid& grid, const rock::SnowSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunSnowComputeImmediate(grid, settings, error);
    }

    auto request = std::make_shared<SnowGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<SnowGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_snowGpuRequestMutex);
        g_pendingSnowGpuRequests.push_back(request);
    }
    g_snowComputeStatus = "Snow GPU Compute queued on main thread";

    SnowGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }
    grid = std::move(result.grid);
    return true;
}

void ProcessPendingSnowGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<SnowGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_snowGpuRequestMutex);
        requests.swap(g_pendingSnowGpuRequests);
    }

    for (const std::shared_ptr<SnowGpuRequest>& request : requests)
    {
        SnowGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunSnowComputeImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
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
    float pad0;
};
static_assert(sizeof(DepthOfFieldShaderConstants) == 12 * sizeof(UINT));

bool EnsureDepthOfFieldPipeline(std::string* error)
{
    if (g_dofPipelineReady && g_dofRootSignature && g_dofPso)
    {
        return true;
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

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize DOF root signature failed";
        g_dofPipelineStatus = "Depth of Field root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_dofRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create DOF root signature failed";
        g_dofPipelineStatus = "Depth of Field root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = DepthOfFieldShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    return true;
}

bool EnsureSkyPipeline(std::string* error)
{
    if (g_skyPipelineReady && g_skyRootSignature && g_skyPso)
    {
        return true;
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

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize sky root sig failed";
        g_skyPipelineStatus = "Sky root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_skyRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create sky root signature failed";
        g_skyPipelineStatus = "Sky root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = SkyShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
    if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize multi-scatter root sig failed"; return false; }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_atmosphereMultiScatterRootSignature));
    if (FAILED(hr)) { if (error) *error = "Create multi-scatter root signature failed"; return false; }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

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

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
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
    float pad1;
};
static_assert(sizeof(CloudRenderShaderConstants) == 56 * sizeof(UINT), "CloudRenderShaderConstants must be 56 DWORDs");

bool EnsureCloudPipelines(std::string* error)
{
    if (g_cloudPipelinesReady && g_cloudVolumePso && g_cloudVolumeRootSignature && g_cloudRenderPso && g_cloudRenderRootSignature)
    {
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device is not available";
        g_cloudPipelineStatus = "Cloud pipelines unavailable";
        return false;
    }

    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

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

        ComPtr<ID3DBlob> sigBlob, errBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
        if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize cloud volume root sig failed"; g_cloudPipelineStatus = "Cloud volume root signature failed"; return false; }
        hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_cloudVolumeRootSignature));
        if (FAILED(hr)) { if (error) *error = "Create cloud volume root sig failed"; g_cloudPipelineStatus = "Cloud volume root signature failed"; return false; }

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

        ComPtr<ID3DBlob> sigBlob, errBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
        if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize cloud render root sig failed"; g_cloudPipelineStatus = "Cloud render root signature failed"; return false; }
        hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_cloudRenderRootSignature));
        if (FAILED(hr)) { if (error) *error = "Create cloud render root sig failed"; g_cloudPipelineStatus = "Cloud render root signature failed"; return false; }

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
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
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

        ComPtr<ID3DBlob> sigBlob, errBlob;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob);
        if (FAILED(hr)) { if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize cloud shadow root sig failed"; g_cloudPipelineStatus = "Cloud shadow root signature failed"; return false; }
        hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_cloudShadowRootSignature));
        if (FAILED(hr)) { if (error) *error = "Create cloud shadow root sig failed"; g_cloudPipelineStatus = "Cloud shadow root signature failed"; return false; }

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
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
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
    c.lightSamples = std::clamp(clouds.lightSamples, 0, 16);
    c.lightStepMeters = std::clamp(clouds.lightStepMeters, 1.0f, 2000.0f);
    c.phaseEccentricity = std::clamp(clouds.phaseEccentricity, -0.99f, 0.99f);
    c.pad1 = 0.0f;

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

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 2;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
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
        kind == rock::NodeKind::HeightmapBlur ||
        kind == rock::NodeKind::MultiScaleErosion ||
        kind == rock::NodeKind::MaskNoise ||
        kind == rock::NodeKind::MaskBlend ||
        kind == rock::NodeKind::MaskLevels ||
        kind == rock::NodeKind::MaskSlope ||
        kind == rock::NodeKind::MaskHeight ||
        kind == rock::NodeKind::Crumbling ||
        kind == rock::NodeKind::MaskCurvature ||
        kind == rock::NodeKind::MaskFluvial ||
        kind == rock::NodeKind::Rock ||
        kind == rock::NodeKind::Sediment ||
        kind == rock::NodeKind::Snow ||
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

// ドラッグサンプル列を間引き、グラデーションストップとして Colorize ノードに投影する。
// 隣接サンプル間の色差が colorThreshold 以下の点を除去し、
// 色変化の大きい点を優先して最大ストップ数に収め、元サンプル位置を保って配置する。
static void ProcessDragSamples(rock::GraphId nodeId, const std::vector<std::array<float, 3>>& samples)
{
    if (samples.empty()) return;

    rock::Node* node = g_graph.FindMutableNode(nodeId);
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

    g_graph.MarkDirty("Drag color sampled");
    EvaluateGraph();
    SetForegroundWindow(g_hwnd);
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

// スクリーンカラーピッカーのフレーム更新。毎フレーム ImGui::NewFrame 直後に呼ぶ。
// Ctrl を押しながらマウスを移動すると色を収集し、Ctrl を離した瞬間にグラデーションへ投影。
void UpdateScreenColorPick()
{
    if (g_screenPick.mode == ScreenPickMode::Idle) return;

    SampleScreenPixel(g_screenPick.previewR, g_screenPick.previewG, g_screenPick.previewB);

    const bool ctrlDown    = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool ctrlRising  = ctrlDown  && !g_screenPick.prevCtrl;
    const bool ctrlFalling = !ctrlDown &&  g_screenPick.prevCtrl;
    g_screenPick.prevCtrl = ctrlDown;

    // Escape でどのモードからもキャンセル
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
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
            ProcessDragSamples(g_screenPick.nodeId, g_screenPick.dragSamples);
            g_screenPick.dragSamples.clear();
            g_screenPick.mode = ScreenPickMode::Idle;
        }
        break;

    default:
        break;
    }
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
        ProcessPendingSedimentGpuRequests();
        ProcessPendingRockGpuRequests();
        ProcessPendingMaskFluvialGpuRequests();
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

void EnsurePreviewMesh()
{
    if (g_graph.Evaluation().dirty)
    {
        EvaluateGraphSync();
    }
}

float DefaultViewportOrbitDistance()
{
    const float terrainSize = std::max(1.0f, g_graph.Settings().preview.terrainSizeMeters);
    const float fovRadians = std::clamp(kDefaultViewportFovDegrees, 15.0f, 90.0f) * kDegreesToRadians;
    const float horizontalBoundingRadius = terrainSize * 0.70710678f;
    const float distance = horizontalBoundingRadius / std::sin(fovRadians * 0.5f);
    return std::clamp(distance * 1.08f, 1.0f, kMaxViewportOrbitDistance);
}

void ResetViewport()
{
    g_viewport = {};
    g_viewport.yaw = kDefaultViewportYaw;
    g_viewport.pitch = kDefaultViewportPitch;
    g_viewport.fovDegrees = kDefaultViewportFovDegrees;
    g_viewport.orbitDistance = DefaultViewportOrbitDistance();
}

float CameraFocalLengthMmFromFovYDegrees(float fovYDegrees)
{
    const float clampedFovYDegrees = std::clamp(fovYDegrees, 15.0f, 90.0f);
    const float fovRadians = clampedFovYDegrees * 3.1415926535f / 180.0f;
    return kFullFrameSensorHeightMm / (2.0f * std::tan(fovRadians * 0.5f));
}

float CameraFovYDegreesFromFocalLengthMm(float focalLengthMm)
{
    const float defaultFocalLengthMm = CameraFocalLengthMmFromFovYDegrees(45.0f);
    const float clampedFocalLengthMm = std::max(0.1f, std::isfinite(focalLengthMm) ? focalLengthMm : defaultFocalLengthMm);
    const float fovRadians = 2.0f * std::atan(kFullFrameSensorHeightMm / (2.0f * clampedFocalLengthMm));
    return std::clamp(fovRadians * 180.0f / 3.1415926535f, 15.0f, 90.0f);
}

void UpdateViewportInteraction(const ImVec2& min, const ImVec2& max)
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

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        ResetViewport();
        return;
    }

    if (hovered && io.MouseWheel != 0.0f)
    {
        g_viewport.orbitDistance *= std::pow(1.12f, -io.MouseWheel);
        g_viewport.orbitDistance = std::clamp(g_viewport.orbitDistance, 1.0f, kMaxViewportOrbitDistance);
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && hovered)
    {
        g_viewport.yaw -= io.MouseDelta.x * 0.010f;
        g_viewport.pitch += io.MouseDelta.y * 0.010f;
        g_viewport.pitch = std::clamp(g_viewport.pitch, -1.25f, 1.25f);
    }

    if ((ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) && hovered)
    {
        g_viewport.pan.x += io.MouseDelta.x;
        g_viewport.pan.y += io.MouseDelta.y;
    }
}

Vec3 Subtract(Vec3 a, Vec3 b)
{
    return Vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

Vec3 Add(Vec3 a, Vec3 b)
{
    return Vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

Vec3 Scale(Vec3 value, float scalar)
{
    return Vec3(value.x * scalar, value.y * scalar, value.z * scalar);
}

float Dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float Length(Vec3 value)
{
    return std::sqrt(Dot(value, value));
}

Vec3 Cross(Vec3 a, Vec3 b)
{
    return Vec3(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x);
}

Vec3 Normalize(Vec3 value, Vec3 fallback)
{
    const float lengthSq = Dot(value, value);
    if (lengthSq <= 0.000001f)
    {
        return fallback;
    }
    return Scale(value, 1.0f / std::sqrt(lengthSq));
}

CameraBasis BuildCameraBasis()
{
    const float distance = std::clamp(g_viewport.orbitDistance, 1.0f, kMaxViewportOrbitDistance);
    const float cosPitch = std::cos(g_viewport.pitch);
    const float sinPitch = std::sin(g_viewport.pitch);
    const float cosYaw = std::cos(g_viewport.yaw);
    const float sinYaw = std::sin(g_viewport.yaw);
    const Vec3 worldUp(0.0f, 1.0f, 0.0f);

    CameraBasis basis;
    basis.position = Vec3(sinYaw * cosPitch * distance, sinPitch * distance, cosYaw * cosPitch * distance);
    basis.forward = Normalize(Scale(basis.position, -1.0f), Vec3(0.0f, 0.0f, -1.0f));
    basis.right = Normalize(Cross(basis.forward, worldUp), Vec3(1.0f, 0.0f, 0.0f));
    basis.up = Normalize(Cross(basis.right, basis.forward), worldUp);
    return basis;
}

ImVec2 ProjectWorldNormalized(float x, float y, float z)
{
    const CameraBasis basis = BuildCameraBasis();
    const Vec3 world(x, y, z);
    const Vec3 view = Subtract(world, basis.position);
    const float cameraX = Dot(view, basis.right);
    const float cameraY = Dot(view, basis.up);
    const float depth = std::max(0.05f, Dot(view, basis.forward));
    const float fovRadians = std::clamp(g_viewport.fovDegrees, 15.0f, 90.0f) * 3.1415926535f / 180.0f;
    const float focalLength = 1.0f / std::tan(fovRadians * 0.5f);
    const float perspective = focalLength / depth;
    return ImVec2(cameraX * perspective, -cameraY * perspective);
}

ProjectedPoint ProjectWorldToScreen(float x, float y, float z, const ImVec2& center, float scale)
{
    const CameraBasis basis = BuildCameraBasis();
    const Vec3 world(x, y, z);
    const Vec3 view = Subtract(world, basis.position);
    const float cameraX = Dot(view, basis.right);
    const float cameraY = Dot(view, basis.up);
    const float depth = std::max(0.05f, Dot(view, basis.forward));
    const float fovRadians = std::clamp(g_viewport.fovDegrees, 15.0f, 90.0f) * 3.1415926535f / 180.0f;
    const float focalLength = 1.0f / std::tan(fovRadians * 0.5f);
    const float perspective = focalLength / depth;
    return ProjectedPoint(ImVec2(center.x + cameraX * perspective * scale, center.y - cameraY * perspective * scale), depth);
}

ImVec2 RotatePoint(float x, float y, float z, float, float)
{
    return ProjectWorldNormalized(x, y, z);
}

ImU32 ColorToU32(const ImVec4& color);
ImU32 ThemeColor(const std::string& name, const ImVec4& fallback);
ImVec4 PinColor(const rock::Pin& pin);
ImVec4 PinLabelColor(const rock::Pin& pin, bool hovered, bool selected);

bool EnsureMeshPreviewRenderTarget(int width, int height, std::string* error)
{
    const int shadowResolution = std::clamp(g_graph.Settings().preview.shadowMapResolution, 512, 4096);
    if (g_gpuMeshPreview.colorTarget && g_gpuMeshPreview.shadowTarget &&
        g_gpuMeshPreview.width == width && g_gpuMeshPreview.height == height &&
        g_gpuMeshPreview.shadowMapResolution == shadowResolution)
    {
        return true;
    }

    try
    {
        WaitForLastSubmittedFrame();
        g_gpuMeshPreview.colorTarget.Reset();
        g_gpuMeshPreview.postTarget.Reset();
        g_gpuMeshPreview.depthTarget.Reset();
        g_gpuMeshPreview.shadowTarget.Reset();
        g_gpuMeshPreview.width = width;
        g_gpuMeshPreview.height = height;
        g_gpuMeshPreview.shadowMapResolution = shadowResolution;
        g_gpuMeshPreview.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_gpuMeshPreview.postState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_gpuMeshPreview.shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!g_meshPreviewRtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = 2;
            ThrowIfFailed(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_meshPreviewRtvHeap)), "Create mesh RTV heap failed");
            g_gpuMeshPreview.rtvCpu = g_meshPreviewRtvHeap->GetCPUDescriptorHandleForHeapStart();
            g_gpuMeshPreview.postRtvCpu = g_gpuMeshPreview.rtvCpu;
            g_gpuMeshPreview.postRtvCpu.ptr += g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        }
        if (!g_meshPreviewDsvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            desc.NumDescriptors = 2;
            ThrowIfFailed(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_meshPreviewDsvHeap)), "Create mesh DSV heap failed");
            g_gpuMeshPreview.dsvCpu = g_meshPreviewDsvHeap->GetCPUDescriptorHandleForHeapStart();
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
        if (!g_gpuMeshPreview.shadowSrvAllocated)
        {
            AllocateSrvDescriptor(nullptr, &g_gpuMeshPreview.shadowSrvCpu, &g_gpuMeshPreview.shadowSrvGpu);
            g_gpuMeshPreview.shadowSrvAllocated = true;
        }

        const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);

        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.colorTarget)),
                "Create mesh color RT failed");
            g_device->CreateRenderTargetView(g_gpuMeshPreview.colorTarget.Get(), nullptr, g_gpuMeshPreview.rtvCpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.colorTarget.Get(), &srvDesc, g_gpuMeshPreview.srvCpu);
        }
        {
            D3D12_CLEAR_VALUE clearVal{};
            clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.postTarget)),
                "Create mesh post RT failed");
            g_device->CreateRenderTargetView(g_gpuMeshPreview.postTarget.Get(), nullptr, g_gpuMeshPreview.postRtvCpu);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_device->CreateShaderResourceView(g_gpuMeshPreview.postTarget.Get(), &srvDesc, g_gpuMeshPreview.postSrvCpu);
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

bool RenderGpuMeshPreview(const ImVec2& min, const ImVec2& max, bool showSurface, bool showWireframe, std::string* error)
{
    const bool showGrid = g_graph.Settings().preview.showGrid;
    if (!showSurface && !showWireframe && !showGrid)
    {
        g_gpuMeshPreview.renderStats = {};
        return true;
    }
    if (!EnsureMeshPreviewPipeline(error)) return false;

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
        g_gpuMeshPreview.maskPreview != g_graph.Evaluation().previewShowsMask ||
        g_gpuMeshPreview.maskShading != static_cast<int>(g_graph.Settings().preview.maskShading) ||
        g_gpuMeshPreview.terrainBoundaryMode != static_cast<int>(terrainBoundaryMode) ||
        g_gpuMeshPreview.lightingMode != g_graph.Settings().preview.lightingMode ||
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
        g_gpuMeshPreview.cloudWindDirectionDegrees != g_graph.Settings().clouds.windDirectionDegrees ||
        g_gpuMeshPreview.cloudWindSpeed != g_graph.Settings().clouds.windSpeedMetersPerSec ||
        g_gpuMeshPreview.cloudQualitySamples != g_graph.Settings().clouds.qualitySamples ||
        g_gpuMeshPreview.cloudShadowStrength != g_graph.Settings().clouds.shadowStrength ||
        g_gpuMeshPreview.cloudShadowResolution != g_graph.Settings().clouds.shadowResolution ||
        g_gpuMeshPreview.cloudShadowSamples != g_graph.Settings().clouds.shadowSamples ||
        g_gpuMeshPreview.cloudFieldRadius != g_graph.Settings().clouds.fieldRadius ||
        g_gpuMeshPreview.cloudFieldFalloff != g_graph.Settings().clouds.fieldFalloff ||
        g_gpuMeshPreview.cloudLightSamples != g_graph.Settings().clouds.lightSamples ||
        g_gpuMeshPreview.cloudLightStepMeters != g_graph.Settings().clouds.lightStepMeters ||
        g_gpuMeshPreview.cloudPhaseEccentricity != g_graph.Settings().clouds.phaseEccentricity ||
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
        (g_graph.Settings().sky.mode == rock::SkyMode::Atmospheric && g_graph.Settings().clouds.enabled && g_graph.Settings().clouds.animate && g_graph.Settings().clouds.windSpeedMetersPerSec > 0.0f) ||
        (showGrid && !g_gpuMeshPreview.gridVertexBuffer) ||
        (showTerrainBoundaryLines && !g_gpuMeshPreview.terrainBoundaryLineVertexBuffer) ||
        g_gpuMeshPreview.colorState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE ||
        (useDepthOfField && g_gpuMeshPreview.postState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
                EnsureMeshPreviewDisplacementPipeline(&ignoredErr) &&
                EnsureDisplacementGridIndexBuffers(previewMeshResolution, &ignoredErr) &&
                EnsureCloudShadowMeshCb(&ignoredErr) &&
                EnsureDummyCloudShadowTexture(&ignoredErr))
            {
                displacementReady = true;
            }
        }

        const bool hasMeshVertices = g_gpuMeshPreview.vertexCount > 0 && g_gpuMeshPreview.vertexBuffer;
        if (!hasMeshVertices && !displacementReady && (!showGrid || g_gpuMeshPreview.gridVertexCount == 0))
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
        constants.padding2 = lightDepthMin;

        D3D12_VERTEX_BUFFER_VIEW vbv{};
        if (hasMeshVertices)
        {
            vbv.BufferLocation = g_gpuMeshPreview.vertexBuffer->GetGPUVirtualAddress();
            vbv.SizeInBytes    = g_gpuMeshPreview.vertexCount * static_cast<UINT>(sizeof(rock::MeshVertex));
            vbv.StrideInBytes  = static_cast<UINT>(sizeof(rock::MeshVertex));
            commandList->IASetVertexBuffers(0, 1, &vbv);
        }
        commandList->SetGraphicsRootSignature(g_meshPreviewRootSignature.Get());
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
                g_meshPreviewDisplacementCbv->Map(0, &readRange, &mappedCbv);
                std::memcpy(mappedCbv, &constants, sizeof(constants));
                g_meshPreviewDisplacementCbv->Unmap(0, nullptr);

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

                commandList->SetGraphicsRootSignature(g_meshPreviewDisplacementRootSignature.Get());
                commandList->SetGraphicsRootConstantBufferView(0, g_meshPreviewDisplacementCbv->GetGPUVirtualAddress());
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
                    ? g_meshPreviewDisplacementTessShadowPso.Get()
                    : g_meshPreviewDisplacementShadowPso.Get());
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
                    commandList->SetPipelineState(g_meshPreviewDisplacementSectionShadowPso.Get());
                    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    commandList->DrawIndexedInstanced(g_gpuMeshPreview.displacementSectionIndexCount, 1, 0, 0, 0);
                    recordIndexedDraw(g_gpuMeshPreview.displacementSectionIndexCount, true);
                }
                renderStats.shadowPass = true;

                // Restore CPU root sig + bindings for the surface / grid
                // draws below (they assume the CPU root sig is current).
                commandList->SetGraphicsRootSignature(g_meshPreviewRootSignature.Get());
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
                commandList->SetPipelineState(g_meshPreviewShadowPso.Get());
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
        const float windRad = cloudSettingsForShadow.windDirectionDegrees * 3.14159265358979323846f / 180.0f;
        const float seconds = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const float cloudWindSpeed = cloudSettingsForShadow.animate ? cloudSettingsForShadow.windSpeedMetersPerSec : 0.0f;
        const float windOffsetX = std::cos(windRad) * cloudWindSpeed * seconds;
        const float windOffsetZ = std::sin(windRad) * cloudWindSpeed * seconds;

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
        cloudShadowCb.cloudShadowPadA = 0.0f;
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
        const std::array<float, 3> sectionColor = colorTextureReady
            ? EstimateSectionColor(g_graph.Evaluation().previewColorGrid, g_graph.Settings().preview.pbrAlbedo)
            : EstimateSectionColor(rock::ColorGrid{}, g_graph.Settings().preview.pbrAlbedo);
        fillColor4(cloudShadowCb.sectionColor, sectionColor);
        cloudShadowCb.atmosphereDensity =
            (sky.mode == rock::SkyMode::Atmospheric) ? std::clamp(sky.atmosphereDensity, 0.05f, 8.0f) : 0.0f;
        cloudShadowCb.atmosphereMieStrength =
            (sky.mode == rock::SkyMode::Atmospheric) ? std::clamp(sky.mieStrength, 0.0f, 8.0f) : 0.0f;
        cloudShadowCb.pad0 = 0.0f;
        cloudShadowCb.pad1 = 0.0f;

        if (g_gpuClouds.meshCbMapped)
        {
            std::memcpy(g_gpuClouds.meshCbMapped, &cloudShadowCb, sizeof(cloudShadowCb));
        }

        // Restore mesh state for the surface draws below. Cloud pass moves to
        // after the mesh draws so it can sample depth and limit ray-march.
        ID3D12DescriptorHeap* descriptorHeaps[] = {g_srvHeap.Get()};
        commandList->SetDescriptorHeaps(1, descriptorHeaps);
        commandList->SetGraphicsRootSignature(g_meshPreviewRootSignature.Get());
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
            g_meshPreviewDisplacementCbv->Map(0, &readRange, &mappedCbv);
            std::memcpy(mappedCbv, &constants, sizeof(constants));
            g_meshPreviewDisplacementCbv->Unmap(0, nullptr);

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

            commandList->SetGraphicsRootSignature(g_meshPreviewDisplacementRootSignature.Get());
            commandList->SetGraphicsRootConstantBufferView(0, g_meshPreviewDisplacementCbv->GetGPUVirtualAddress());
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
                ? g_meshPreviewDisplacementTessSurfacePso.Get()
                : g_meshPreviewDisplacementSurfacePso.Get());
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
                commandList->SetPipelineState(g_meshPreviewDisplacementSectionPso.Get());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawIndexedInstanced(g_gpuMeshPreview.displacementSectionIndexCount, 1, 0, 0, 0);
                recordIndexedDraw(g_gpuMeshPreview.displacementSectionIndexCount, true);
            }
            renderStats.surfacePass = true;

            // Restore the CPU root sig + constants for wireframe / grid
            // draws below (those still expect the CPU mesh root sig).
            commandList->SetGraphicsRootSignature(g_meshPreviewRootSignature.Get());
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
            commandList->SetPipelineState(g_meshPreviewSurfacePso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(surfaceIndexCount, 1, 0, 0, 0);
            recordIndexedDraw(surfaceIndexCount, true);
            renderStats.surfacePass = true;
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
            g_meshPreviewDisplacementCbv->Map(0, &readRange, &mappedCbv);
            std::memcpy(mappedCbv, &wireConstants, sizeof(wireConstants));
            g_meshPreviewDisplacementCbv->Unmap(0, nullptr);

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

            commandList->SetGraphicsRootSignature(g_meshPreviewDisplacementRootSignature.Get());
            commandList->SetGraphicsRootConstantBufferView(0, g_meshPreviewDisplacementCbv->GetGPUVirtualAddress());
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
                ? g_meshPreviewDisplacementTessWirePso.Get()
                : g_meshPreviewDisplacementWirePso.Get());
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
                commandList->SetPipelineState(g_meshPreviewDisplacementSectionWirePso.Get());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                commandList->DrawIndexedInstanced(g_gpuMeshPreview.displacementSectionIndexCount, 1, 0, 0, 0);
                recordIndexedDraw(g_gpuMeshPreview.displacementSectionIndexCount, true);
            }
            renderStats.wireframePass = true;

            commandList->SetGraphicsRootSignature(g_meshPreviewRootSignature.Get());
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
            commandList->SetPipelineState(g_meshPreviewGridPso.Get());
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
            commandList->SetPipelineState(g_meshPreviewGridPso.Get());
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
            commandList->SetPipelineState(g_meshPreviewWirePso.Get());
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
                atmSkyColor = atm.zenith;
            }
            cloudBase.atmosphereSunColor[0] = atmSunColor[0];
            cloudBase.atmosphereSunColor[1] = atmSunColor[1];
            cloudBase.atmosphereSunColor[2] = atmSunColor[2];
            cloudBase.atmosphereSunColor[3] = 1.0f;
            cloudBase.atmosphereSkyColor[0] = atmSkyColor[0];
            cloudBase.atmosphereSkyColor[1] = atmSkyColor[1];
            cloudBase.atmosphereSkyColor[2] = atmSkyColor[2];
            cloudBase.atmosphereSkyColor[3] = 1.0f;

            // windRad / seconds / windOffsetX / windOffsetZ are already
            // computed earlier for the cloud-shadow generation pass.
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
        g_gpuMeshPreview.maskPreview   = g_graph.Evaluation().previewShowsMask;
        g_gpuMeshPreview.maskShading   = static_cast<int>(g_graph.Settings().preview.maskShading);
        g_gpuMeshPreview.terrainBoundaryMode = static_cast<int>(terrainBoundaryMode);
        g_gpuMeshPreview.lightingMode  = g_graph.Settings().preview.lightingMode;
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
        g_gpuMeshPreview.cloudWindDirectionDegrees = g_graph.Settings().clouds.windDirectionDegrees;
        g_gpuMeshPreview.cloudWindSpeed = g_graph.Settings().clouds.windSpeedMetersPerSec;
        g_gpuMeshPreview.cloudQualitySamples = g_graph.Settings().clouds.qualitySamples;
        g_gpuMeshPreview.cloudShadowStrength = g_graph.Settings().clouds.shadowStrength;
        g_gpuMeshPreview.cloudShadowResolution = g_graph.Settings().clouds.shadowResolution;
        g_gpuMeshPreview.cloudShadowSamples = g_graph.Settings().clouds.shadowSamples;
        g_gpuMeshPreview.cloudFieldRadius = g_graph.Settings().clouds.fieldRadius;
        g_gpuMeshPreview.cloudFieldFalloff = g_graph.Settings().clouds.fieldFalloff;
        g_gpuMeshPreview.cloudLightSamples = g_graph.Settings().clouds.lightSamples;
        g_gpuMeshPreview.cloudLightStepMeters = g_graph.Settings().clouds.lightStepMeters;
        g_gpuMeshPreview.cloudPhaseEccentricity = g_graph.Settings().clouds.phaseEccentricity;
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
    std::string error;
    if (!RenderGpuMeshPreview(min, max, showSurface, showWireframe, &error))
    {
        DrawMeshPreview(drawList, min, max, mesh, showSurface, showWireframe);
        return;
    }
    const bool usePostImage =
        g_gpuMeshPreview.depthOfFieldEnabled &&
        g_dofPipelineReady &&
        g_gpuMeshPreview.postSrvAllocated &&
        g_gpuMeshPreview.postState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_GPU_DESCRIPTOR_HANDLE imageSrv = usePostImage ? g_gpuMeshPreview.postSrvGpu : g_gpuMeshPreview.srvGpu;
    if (g_gpuMeshPreview.srvAllocated && g_gpuMeshPreview.colorState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
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
    if (ImGui::Button("表示", buttonSize))
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
            const bool pressed = ImGui::Selectable("##toggle_row", false, 0, ImVec2(rowWidth, rowHeight));
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
            drawList->AddRectFilled(boxMin, boxMax, boxFill, 3.0f);
            drawList->AddRect(boxMin, boxMax, boxBorder, 3.0f);
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

        if (drawSmallToggle("ViewportFpsToggle", "FPSを表示", &g_ui.showFps))
        {
            SaveAppSettingsSilently();
        }
        if (drawSmallToggle("ViewportGridToggle", "グリッドを表示", &settings.preview.showGrid))
        {
            SaveAppSettingsSilently();
        }
        ImGui::Separator();
        ImGui::TextUnformatted("表示モード");
        ImGui::Separator();
        const auto drawModeItem = [&](const char* label, ViewportDisplayMode mode) {
            const bool selected = displayMode == mode;
            if (ImGui::Selectable(label, selected))
            {
                displayMode = mode;
                ApplyViewportDisplayMode(settings, mode);
                SaveAppSettingsSilently();
            }
        };
        drawModeItem("シンプル", ViewportDisplayMode::Simple);
        drawModeItem("PBR", ViewportDisplayMode::Pbr);
        drawModeItem("天球", ViewportDisplayMode::Sky);

        if (displayMode == ViewportDisplayMode::Sky)
        {
            ImGui::Spacing();
            if (drawSmallToggle("ViewportCloudToggle", "雲を描画", &settings.clouds.enabled))
            {
                SaveAppSettingsSilently();
            }
            if (settings.clouds.enabled && drawSmallToggle("ViewportCloudAnimateToggle", "雲を動かす", &settings.clouds.animate))
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
    const bool drawGpuViewport = drawMeshSurface || preview.showGrid;
    if (drawGpuViewport)
    {
        DrawGpuMeshPreview(drawList, min, max, g_graph.Evaluation().previewMesh,
                           drawMeshSurface && preview.showSurface,
                           drawMeshSurface && preview.showWireframe);
    }
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

    const std::string title = g_graph.Evaluation().previewShowsMask
        ? std::string(heightfieldFieldName(g_graph.Evaluation().previewField)) + " Preview"
        : "Heightmap Preview";
    drawList->AddText(ImVec2(min.x + 82.0f, min.y + 14.0f), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), title.c_str());
    const std::string gridInfo = std::format(
        "Right-handed, Y-up, {} x {}, {:.0f} m cells",
        preview.gridCellCount,
        preview.gridCellCount,
        preview.gridCellSizeMeters);
    drawList->AddText(ImVec2(min.x + 82.0f, min.y + 36.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), gridInfo.c_str());
    DrawViewportDisplayMenu(min);
    float overlayTop = min.y + 14.0f;
    if (g_ui.showFps)
    {
        char fpsText[32]{};
        std::snprintf(fpsText, sizeof(fpsText), "FPS %.1f", ImGui::GetIO().Framerate);
        const ImVec2 fpsSize = ImGui::CalcTextSize(fpsText);
        const ImVec2 fpsPadding(9.0f, 5.0f);
        const ImVec2 fpsMax(max.x - 14.0f, overlayTop + fpsSize.y + fpsPadding.y * 2.0f);
        const ImVec2 fpsMin(fpsMax.x - fpsSize.x - fpsPadding.x * 2.0f, overlayTop);
        drawList->AddRectFilled(fpsMin, fpsMax, IM_COL32(8, 10, 10, 168), 4.0f);
        drawList->AddRect(fpsMin, fpsMax, ThemeColor("border", ImVec4(0.20f, 0.23f, 0.22f, 0.70f)), 4.0f);
        drawList->AddText(ImVec2(fpsMin.x + fpsPadding.x, fpsMin.y + fpsPadding.y), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), fpsText);
        overlayTop = fpsMax.y + 6.0f;
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
        drawList->AddRectFilled(statsMin, statsMax, IM_COL32(8, 10, 10, 168), 4.0f);
        drawList->AddRect(statsMin, statsMax, ThemeColor("border", ImVec4(0.20f, 0.23f, 0.22f, 0.70f)), 4.0f);
        drawList->AddText(ImVec2(statsMin.x + statsPadding.x, statsMin.y + statsPadding.y), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), statsText.c_str());
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

void DrawHeightfieldMapPreview(const ImVec2& min, const ImVec2& max)
{
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
            drawList->AddText(ImVec2(min.x + 16.0f, min.y + 42.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), "Gradient Mask を接続してください。");
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
        const int maxVisibleSamples = std::clamp(static_cast<int>(std::ceil(mapSize)), 2, 1024);
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
        char info[128]{};
        std::snprintf(info, sizeof(info), "%d x %d / zoom %.2fx", res, res, g_mapViewport.zoom);
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

    const int maxVisibleSamples = std::clamp(static_cast<int>(std::ceil(mapSize)), 2, 1024);
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
    switch (kind)
    {
    case rock::NodeKind::HeightmapLoad:
    case rock::NodeKind::Shape:
    case rock::NodeKind::HeightmapBlur:
    case rock::NodeKind::MultiScaleErosion:
    case rock::NodeKind::Crumbling:
    case rock::NodeKind::Rock:
    case rock::NodeKind::Sediment:
    case rock::NodeKind::Snow:
        return heightfieldGreen;
    case rock::NodeKind::MaskNoise:
    case rock::NodeKind::MaskBlend:
    case rock::NodeKind::MaskLevels:
    case rock::NodeKind::MaskSlope:
    case rock::NodeKind::MaskHeight:
    case rock::NodeKind::MaskCurvature:
    case rock::NodeKind::MaskFluvial:
        return maskOrange;
    case rock::NodeKind::Colorize:
        return ImVec4(0.44f, 0.50f, 0.96f, 1.0f); // 青紫 (カラー系)
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
    case rock::NodeKind::HeightmapBlur:
        return ImVec2(600.0f, 240.0f);
    case rock::NodeKind::MultiScaleErosion:
        return ImVec2(320.0f, 380.0f);
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
    case rock::NodeKind::Crumbling:
        return ImVec2(880.0f, 380.0f);
    case rock::NodeKind::Rock:
        return ImVec2(880.0f, 450.0f);
    case rock::NodeKind::Sediment:
        return ImVec2(880.0f, 520.0f);
    case rock::NodeKind::Snow:
        return ImVec2(880.0f, 660.0f);
    case rock::NodeKind::Colorize:
        return ImVec2(1160.0f, 380.0f);
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

void LoadJapaneseFont(ImGuiIO& io)
{
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
    };

    for (const char* fontPath : fontPaths)
    {
        if (!std::filesystem::exists(fontPath))
        {
            continue;
        }

        ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
        if (font != nullptr)
        {
            return;
        }
    }

    io.Fonts->AddFontDefault();
}

ImU32 ColorToU32(const ImVec4& color)
{
    return ImGui::ColorConvertFloat4ToU32(color);
}

ImU32 ThemeColor(const std::string& name, const ImVec4& fallback)
{
    return ColorToU32(g_themeManager.AppColor(name, fallback));
}

ImVec4 PinLabelColor(const rock::Pin& pin, bool hovered, bool selected)
{
    if (selected)
    {
        return PinColor(pin);
    }
    if (hovered)
    {
        return ImVec4(0.94f, 0.94f, 0.92f, 1.0f);
    }
    return ImVec4(0.62f, 0.64f, 0.62f, 1.0f);
}

ImVec4 PinTypeColor(rock::ValueType valueType)
{
    switch (valueType)
    {
    case rock::ValueType::HeightField:
        return ImVec4(0.70f, 0.93f, 0.78f, 1.0f);
    case rock::ValueType::Mask:
        return ImVec4(0.82f, 0.64f, 0.36f, 1.0f);
    case rock::ValueType::ColorTexture:
        return ImVec4(0.54f, 0.60f, 1.0f, 1.0f);
    case rock::ValueType::Mesh:
    default:
        return ImVec4(0.52f, 0.58f, 0.56f, 1.0f);
    }
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

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        ResetMapViewport();
        SaveAppSettingsSilently();
        return;
    }

    bool changed = false;
    if (hovered && io.MouseWheel != 0.0f)
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

    if ((ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) && hovered)
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

ImVec4 PinColor(const rock::Pin& pin)
{
    return PinTypeColor(pin.valueType);
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

void DrawNodeIcon(const ImVec2& origin, const ImVec4& color)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 iconColor = ColorToU32(color);
    drawList->AddTriangleFilled(
        ImVec2(origin.x + 2.0f, origin.y + 17.0f),
        ImVec2(origin.x + 7.0f, origin.y + 4.0f),
        ImVec2(origin.x + 11.0f, origin.y + 17.0f),
        iconColor);
    drawList->AddRectFilled(ImVec2(origin.x + 9.0f, origin.y + 9.0f), ImVec2(origin.x + 15.0f, origin.y + 18.0f), iconColor, 2.0f);
    drawList->AddTriangleFilled(
        ImVec2(origin.x + 14.0f, origin.y + 18.0f),
        ImVec2(origin.x + 20.0f, origin.y + 7.0f),
        ImVec2(origin.x + 24.0f, origin.y + 18.0f),
        iconColor);
}

void DrawRoundPin(const rock::Pin& pin)
{
    const ImVec2 size(14.0f, 20.0f);
    ImGui::Dummy(size);
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    const ImVec2 pivotMin(center.x - 6.0f, center.y - 6.0f);
    const ImVec2 pivotMax(center.x + 6.0f, center.y + 6.0f);
    ed::PinRect(min, max);
    ed::PinPivotRect(pivotMin, pivotMax);
    const ImVec4 color = PinColor(pin);
    ImGui::GetWindowDrawList()->AddCircle(center, 4.3f, ColorToU32(color), 16, 1.6f);
}

void DrawNodeEvaluationBadge(const rock::Node& node, float nodeWidth, const ImVec2& headerCursor)
{
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    const rock::GraphId currentlyEvaluating =
        rock::CurrentlyEvaluatingNodeId().load(std::memory_order_relaxed);

    // "計算中" follows the worker thread — it walks the upstream chain in
    // real time. Until the first kernel stores its id (or if every step
    // is a cache hit), fall back to the preview target so the user sees
    // *something* during in-flight evaluation. "計算待ち" only shows on
    // the preview target when an evaluation is queued behind another.
    const bool isCurrent = g_evaluationInFlight && currentlyEvaluating == node.id;
    const bool isPreviewFallback = g_evaluationInFlight
        && currentlyEvaluating == 0
        && evaluation.previewNodeId == node.id;
    const bool isQueued = g_evaluationPending && evaluation.previewNodeId == node.id;
    if (!isCurrent && !isPreviewFallback && !isQueued)
    {
        return;
    }

    const char* label = isQueued ? "計算待ち" : "計算中";
    const int dotCount = isQueued ? 0 : (static_cast<int>(ImGui::GetTime() * 3.0) % 4);
    char text[32]{};
    std::snprintf(text, sizeof(text), "%s%.*s", label, dotCount, "...");

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(text);
    const ImVec2 padding(8.0f, 3.0f);
    const ImVec2 badgeMax(headerCursor.x + nodeWidth - 4.0f, headerCursor.y + 20.0f);
    const ImVec2 badgeMin(badgeMax.x - textSize.x - padding.x * 2.0f, headerCursor.y - 1.0f);
    drawList->AddRectFilled(badgeMin, badgeMax, ColorToU32(ImVec4(0.18f, 0.14f, 0.07f, 0.96f)), 5.0f);
    drawList->AddRect(badgeMin, badgeMax, ColorToU32(ImVec4(0.90f, 0.70f, 0.28f, 0.78f)), 5.0f, 0, 1.0f);
    drawList->AddText(ImVec2(badgeMin.x + padding.x, badgeMin.y + padding.y - 1.0f), ColorToU32(ImVec4(0.96f, 0.80f, 0.38f, 1.0f)), text);
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

void DrawRockNode(const rock::Node& node)
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
    ImGui::TextUnformatted(node.title.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    DrawNodeEvaluationBadge(node, nodeWidth, headerCursor);

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
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            if (!g_pendingPreviewSelectionRestore)
            {
                g_pendingPreviewSelectionRestore = CurrentSelectedNodeIds();
            }
            g_pendingPreviewPinId = output.id;
        }
        ed::EndPin();
    }
    const size_t pinRowCount = std::max(node.inputs.size(), node.outputs.size());
    ImGui::Dummy(ImVec2(nodeWidth, std::max(4.0f, static_cast<float>(pinRowCount) * 24.0f - 20.0f)));

    if (suppressNodeHoverBorder)
    {
        ed::PushStyleColor(ed::StyleColor_HovNodeBorder, nodeBorderColor);
    }
    ed::EndNode();
    ed::PopStyleColor(suppressNodeHoverBorder ? 5 : 4);
    ed::PopStyleVar(4);
}

void DrawNodeGraphDots(const ImVec2& screenMin, const ImVec2& screenMax)
{
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 canvasMin = ed::ScreenToCanvas(screenMin);
    const ImVec2 canvasMax = ed::ScreenToCanvas(screenMax);
    constexpr float spacing = 24.0f;

    const float startX = std::floor(std::min(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float endX = std::ceil(std::max(canvasMin.x, canvasMax.x) / spacing) * spacing;
    const float startY = std::floor(std::min(canvasMin.y, canvasMax.y) / spacing) * spacing;
    const float endY = std::ceil(std::max(canvasMin.y, canvasMax.y) / spacing) * spacing;
    const ImU32 backgroundColor = ThemeColor("nodeEditorBg", ImVec4(0.115f, 0.115f, 0.115f, 1.0f));
    const ImU32 dotColor = ThemeColor("nodeGridDot", ImVec4(0.255f, 0.255f, 0.255f, 0.46f));

    ed::Suspend();
    drawList->PushClipRect(screenMin, screenMax, true);
    drawList->AddRectFilled(screenMin, screenMax, backgroundColor);
    for (float y = startY; y <= endY; y += spacing)
    {
        for (float x = startX; x <= endX; x += spacing)
        {
            const ImVec2 screen = ed::CanvasToScreen(ImVec2(x, y));
            if (screen.x < screenMin.x || screen.x > screenMax.x || screen.y < screenMin.y || screen.y > screenMax.y)
            {
                continue;
            }
            drawList->AddCircleFilled(screen, 1.15f, dotColor, 8);
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
    g_projectStatus = std::format("Copied {} node{}", g_nodeClipboard.nodes.size(), g_nodeClipboard.nodes.size() == 1 ? "" : "s");
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
            newMutableNode->maskNoise = clipboardNode.node.maskNoise;
            newMutableNode->maskBlend = clipboardNode.node.maskBlend;
            newMutableNode->maskCurvature = clipboardNode.node.maskCurvature;
            newMutableNode->maskLevels = clipboardNode.node.maskLevels;
            newMutableNode->maskSlope = clipboardNode.node.maskSlope;
            newMutableNode->maskHeight = clipboardNode.node.maskHeight;
            newMutableNode->crumbling = clipboardNode.node.crumbling;
            newMutableNode->maskFluvial = clipboardNode.node.maskFluvial;
            newMutableNode->rock = clipboardNode.node.rock;
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
    g_projectStatus = std::format("Pasted {} node{}", pastedNodeIds.size(), pastedNodeIds.size() == 1 ? "" : "s");
    EvaluateGraph();
}

void DrawNodeGraph()
{
    static ImVec2 addNodePosition(0.0f, 0.0f);
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
    DrawNodeGraphDots(canvasMin, canvasMax);
    DrawRockNodeShadows();

    const bool hasPendingNodePositions = !g_pendingNodePositions.empty();
    for (const rock::Node& node : g_graph.Nodes())
    {
        DrawRockNode(node);
        if (hasPendingNodePositions)
        {
            const auto pending = std::ranges::find_if(g_pendingNodePositions, [&](const auto& entry) {
                return entry.first == node.id;
            });
            if (pending != g_pendingNodePositions.end())
            {
                ed::SetNodePosition(ed::NodeId(node.id), pending->second);
            }
            else if (!g_nodePositionsInitialized)
            {
                ed::SetNodePosition(ed::NodeId(node.id), InitialNodePosition(node.kind));
            }
        }
        else if (!g_nodePositionsInitialized)
        {
            ed::SetNodePosition(ed::NodeId(node.id), InitialNodePosition(node.kind));
        }
    }
    if (hasPendingNodePositions)
    {
        g_pendingNodePositions.clear();
    }
    g_nodePositionsInitialized = true;

    if (!g_nodeGraphNavigatedToContent)
    {
        ed::NavigateToContent(0.0f);
        g_nodeGraphNavigatedToContent = true;
    }

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

    for (const rock::Link& link : g_graph.Links())
    {
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPin), ed::PinId(link.endPin), LinkColor(link), 2.5f);
    }

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
            EvaluateGraph();
        }
    }
    ed::EndDelete();

    if (ed::ShowBackgroundContextMenu())
    {
        addNodePosition = CurrentNodeViewCenter(canvasMin, canvasMax);
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
        ImGui::TextDisabled("ノードを追加");
        ImGui::Separator();
        const auto addNodeMenuItem = [&](rock::NodeKind kind) {
            if (ImGui::MenuItem(rock::ToString(kind).data()))
            {
                PushUndoSnapshot();
                g_skipNodeMoveUndoThisFrame = true;
                const rock::GraphId nodeId = g_graph.CreateNode(kind);
                g_pendingNodePositions.push_back({nodeId, addNodePosition});
                g_pendingSelectedNodeIds = {nodeId};
                g_selectedNodeId = nodeId;
                g_projectStatus = "Added " + std::string(rock::ToString(kind));
                EvaluateGraph();
            }
        };
        if (ImGui::BeginMenu("ハイトフィールド"))
        {
            addNodeMenuItem(rock::NodeKind::HeightmapLoad);
            addNodeMenuItem(rock::NodeKind::Shape);
            addNodeMenuItem(rock::NodeKind::HeightmapBlur);
            addNodeMenuItem(rock::NodeKind::MultiScaleErosion);
            addNodeMenuItem(rock::NodeKind::Crumbling);
            addNodeMenuItem(rock::NodeKind::Rock);
            addNodeMenuItem(rock::NodeKind::Sediment);
            addNodeMenuItem(rock::NodeKind::Snow);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("マスク"))
        {
            addNodeMenuItem(rock::NodeKind::MaskNoise);
            addNodeMenuItem(rock::NodeKind::MaskBlend);
            addNodeMenuItem(rock::NodeKind::MaskLevels);
            addNodeMenuItem(rock::NodeKind::MaskHeight);
            addNodeMenuItem(rock::NodeKind::MaskSlope);
            addNodeMenuItem(rock::NodeKind::MaskCurvature);
            addNodeMenuItem(rock::NodeKind::MaskFluvial);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("カラー"))
        {
            addNodeMenuItem(rock::NodeKind::Colorize);
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

    ed::End();
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
            g_projectStatus = "Node moved";
        }
        g_pendingNodeMoveUndo.reset();
    }
    g_nodePositionCache = std::move(currentNodePositions);
    g_skipNodeMoveUndoThisFrame = false;
    ed::PopStyleColor(4);
    g_nodeEditorFrameActive = false;
}

bool FloatDiffersFromDefault(float value, float defaultValue)
{
    return std::fabs(value - defaultValue) > 0.0001f;
}

bool ColorDiffersFromDefault(const std::array<float, 3>& value, const std::array<float, 3>& defaultValue)
{
    return FloatDiffersFromDefault(value[0], defaultValue[0]) ||
           FloatDiffersFromDefault(value[1], defaultValue[1]) ||
           FloatDiffersFromDefault(value[2], defaultValue[2]);
}

std::string FormatDefaultFloat(float value, const char* format)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), format != nullptr ? format : "%.3f", value);
    return buffer;
}

void DrawPropertyLabel(const char* label, const char* tooltip = nullptr, bool = false)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (tooltip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.22f, 0.22f, 0.22f, 0.97f));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
}

bool DrawPropertyComboRow(const char* label, const char* id, int* value, const char* items, const char* tooltip = nullptr, int defaultValue = 0)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, *value != defaultValue);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(id);
    const float comboWidth = std::min(220.0f, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(comboWidth);
    const bool changed = ImGui::Combo("##value", value, items);
    ImGui::PopID();
    return changed;
}

void DrawReadOnlyFloatRow(const char* label, float value, const char* format = "%.2f", const char* tooltip = nullptr)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, false);
    ImGui::TableSetColumnIndex(1);
    ImGui::Text(format, value);
}

bool DrawResetToDefaultButton(const char* id, bool isDefaultValue, const char* defaultValueText = nullptr)
{
    ImGui::SameLine();
    ImGui::PushID(id);
    const float buttonSize = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button("##resetDefault", ImVec2(buttonSize, buttonSize));
    const char* resetIcon = "↺";
    ImFont* font = ImGui::GetFont();
    const float iconFontSize = ImGui::GetFontSize() * 1.25f;
    const ImVec2 iconSize = font->CalcTextSizeA(iconFontSize, FLT_MAX, 0.0f, resetIcon);
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    const ImVec2 iconPos = {
        buttonMin.x + ((buttonMax.x - buttonMin.x) - iconSize.x) * 0.5f,
        buttonMin.y + ((buttonMax.y - buttonMin.y) - iconSize.y) * 0.5f,
    };
    const ImU32 iconColor = ImGui::GetColorU32(isDefaultValue ? ImGuiCol_TextDisabled : ImGuiCol_Text);
    ImGui::GetWindowDrawList()->AddText(font, iconFontSize, iconPos, iconColor, resetIcon);
    if (ImGui::IsItemHovered())
    {
        if (defaultValueText != nullptr && defaultValueText[0] != '\0')
        {
            ImGui::SetTooltip("既定値に戻す\n既定値: %s", defaultValueText);
        }
        else
        {
            ImGui::SetTooltip("既定値に戻す");
        }
    }
    ImGui::PopID();
    return pressed;
}

std::string FormatTimeHours(float hours)
{
    int totalMinutes = static_cast<int>(std::round(std::clamp(hours, 0.0f, 24.0f) * 60.0f));
    totalMinutes = std::clamp(totalMinutes, 0, 24 * 60);
    const int hour = totalMinutes / 60;
    const int minute = totalMinutes % 60;
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hour, minute);
    return buffer;
}

bool DrawTimeOfDayRow(const char* label, const char* id, float* value, float defaultValue, const char* dirtyReason, const char* tooltip = nullptr)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, FloatDiffersFromDefault(*value, defaultValue));
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float valueWidth = 48.0f;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - valueWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x * 2.0f,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderFloat("##slider", value, 0.0f, 24.0f, ""))
    {
        *value = std::clamp(*value, 0.0f, 24.0f);
        g_graph.MarkDirty(dirtyReason);
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    const std::string timeText = FormatTimeHours(*value);
    ImGui::TextUnformatted(timeText.c_str());

    const std::string defaultText = FormatTimeHours(defaultValue);
    if (DrawResetToDefaultButton("reset", !FloatDiffersFromDefault(*value, defaultValue), defaultText.c_str()))
    {
        *value = std::clamp(defaultValue, 0.0f, 24.0f);
        g_graph.MarkDirty(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    return editEnded;
}

struct NumericTextInputState
{
    std::string text;
    bool active = false;
};

std::unordered_map<ImGuiID, NumericTextInputState> g_numericTextInputs;

std::string FormatFloatInputText(float value, const char* format)
{
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), format ? format : "%.3f", value);
    return buffer;
}

bool ParseFloatInputText(const char* text, float* outValue)
{
    if (!text || !outValue)
    {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text)
    {
        return false;
    }
    while (*end == ' ' || *end == '\t')
    {
        ++end;
    }
    if (*end != '\0')
    {
        return false;
    }
    *outValue = parsed;
    return true;
}

bool ParseIntInputText(const char* text, int* outValue)
{
    if (!text || !outValue)
    {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (end == text)
    {
        return false;
    }
    while (*end == ' ' || *end == '\t')
    {
        ++end;
    }
    if (*end != '\0')
    {
        return false;
    }
    *outValue = static_cast<int>(parsed);
    return true;
}

bool DrawEnterCommitFloatInput(const char* id, float* value, const char* format)
{
    const ImGuiID inputId = ImGui::GetID(id);
    NumericTextInputState& state = g_numericTextInputs[inputId];
    if (!state.active)
    {
        state.text = FormatFloatInputText(*value, format);
    }

    char buffer[64]{};
    strncpy_s(buffer, state.text.c_str(), _TRUNCATE);
    const bool enterPressed = ImGui::InputText(id, buffer, IM_ARRAYSIZE(buffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsScientific);
    state.text = buffer;
    state.active = ImGui::IsItemActive();

    if (!enterPressed)
    {
        return false;
    }
    float parsed = *value;
    if (!ParseFloatInputText(buffer, &parsed))
    {
        return false;
    }
    *value = parsed;
    state.text = FormatFloatInputText(*value, format);
    return true;
}

bool DrawEnterCommitIntInput(const char* id, int* value)
{
    const ImGuiID inputId = ImGui::GetID(id);
    NumericTextInputState& state = g_numericTextInputs[inputId];
    if (!state.active)
    {
        state.text = std::to_string(*value);
    }

    char buffer[32]{};
    strncpy_s(buffer, state.text.c_str(), _TRUNCATE);
    const bool enterPressed = ImGui::InputText(id, buffer, IM_ARRAYSIZE(buffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);
    state.text = buffer;
    state.active = ImGui::IsItemActive();

    if (!enterPressed)
    {
        return false;
    }
    int parsed = *value;
    if (!ParseIntInputText(buffer, &parsed))
    {
        return false;
    }
    *value = parsed;
    state.text = std::to_string(*value);
    return true;
}

bool DrawPropertyFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr, const char* format = "%.3f", ImGuiSliderFlags sliderFlags = 0, float inputMinValue = std::numeric_limits<float>::quiet_NaN(), float inputMaxValue = std::numeric_limits<float>::quiet_NaN())
{
    bool editEnded = false;
    const float inputMin = std::isnan(inputMinValue) ? minValue : inputMinValue;
    const float inputMax = std::isnan(inputMaxValue) ? maxValue : inputMaxValue;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    DrawPropertyLabel(label, tooltip, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 76.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - inputWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderFloat("##slider", value, minValue, maxValue, format, sliderFlags))
    {
        g_graph.MarkDirty(dirtyReason);
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitFloatInput("##number", value, format))
    {
        *value = std::clamp(*value, inputMin, inputMax);
        g_graph.MarkDirty(dirtyReason);
        editEnded = true;
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    const std::string defaultValueText = FormatDefaultFloat(defaultValue, format);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = std::clamp(defaultValue, inputMin, inputMax);
        g_graph.MarkDirty(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    if (recordUndo && editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

bool DrawPropertyFloatInputRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr, const char* format = "%.3f")
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    DrawPropertyLabel(label, tooltip, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float inputWidth = std::max(80.0f, availableWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitFloatInput("##number", value, format))
    {
        *value = std::clamp(*value, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
        editEnded = true;
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }

    ImGui::SameLine();
    differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    const std::string defaultValueText = FormatDefaultFloat(defaultValue, format);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = std::clamp(defaultValue, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    if (recordUndo && editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

bool DrawPropertyPercentRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, const char* tooltip = nullptr)
{
    float percentValue = *value * 100.0f;
    const float minPercent = minValue * 100.0f;
    const float maxPercent = maxValue * 100.0f;
    const float defaultPercent = defaultValue * 100.0f;
    const bool editEnded = DrawPropertyFloatRow(label, id, &percentValue, minPercent, maxPercent, defaultPercent, dirtyReason, true, tooltip);
    // Text input commits on Enter through DrawPropertyFloatRow. Slider edits
    // still update directly so the on-screen control stays responsive.
    const float nextValue = std::clamp(percentValue / 100.0f, minValue, maxValue);
    if (nextValue != *value)
    {
        *value = nextValue;
    }
    return editEnded;
}

bool DrawPropertyIntRow(const char* label, const char* id, int* value, int minValue, int maxValue, int defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = *value != defaultValue;
    DrawPropertyLabel(label, tooltip, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 58.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - inputWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderInt("##slider", value, minValue, maxValue))
    {
        g_graph.MarkDirty(dirtyReason);
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitIntInput("##number", value))
    {
        *value = std::clamp(*value, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
        editEnded = true;
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    differsFromDefault = *value != defaultValue;
    const std::string defaultValueText = std::to_string(defaultValue);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = std::clamp(defaultValue, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
        editEnded = true;
    }
    ImGui::PopID();
    if (recordUndo && editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

template <size_t N>
bool DrawPresetIntRow(const char* label,
                      const char* id,
                      int* value,
                      int defaultValue,
                      const std::array<int, N>& presets,
                      int fallback,
                      const char* dirtyReason,
                      bool recordUndo = true,
                      const char* tooltip = nullptr)
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const int normalizedValue = NearestPreset(*value, presets, fallback);
    if (*value != normalizedValue)
    {
        *value = normalizedValue;
    }
    const int normalizedDefault = NearestPreset(defaultValue, presets, fallback);
    DrawPropertyLabel(label, tooltip, *value != normalizedDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    constexpr float comboWidth = 110.0f;
    const std::string previewValue = std::to_string(*value);
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##preset", previewValue.c_str()))
    {
        for (int preset : presets)
        {
            const bool selected = *value == preset;
            const std::string presetText = std::to_string(preset);
            if (ImGui::Selectable(presetText.c_str(), selected))
            {
                if (*value != preset)
                {
                    if (recordUndo)
                    {
                        PushUndoSnapshot();
                    }
                    *value = preset;
                    g_graph.MarkDirty(dirtyReason);
                    changed = true;
                }
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const std::string defaultValueText = std::to_string(normalizedDefault);
    if (DrawResetToDefaultButton("reset", *value == normalizedDefault, defaultValueText.c_str()))
    {
        if (recordUndo)
        {
            PushUndoSnapshot();
        }
        *value = normalizedDefault;
        g_graph.MarkDirty(dirtyReason);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

bool DrawShadowResolutionPresetRow(const char* label, const char* id, int* value, int defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr)
{
    return DrawPresetIntRow(label, id, value, defaultValue, kShadowResolutionPresets, 1024, dirtyReason, recordUndo, tooltip);
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
    if (changed)
    {
        *value = static_cast<float>(intValue);
    }
    else
    {
        *value = static_cast<float>(intValue);
    }
    return changed;
}

bool DrawPropertyBoolRow(const char* label, const char* id, bool* value, const char* dirtyReason, const char* tooltip = nullptr, bool defaultValue = false, bool compact = false)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, *value != defaultValue);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    if (compact)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
    }
    const bool changed = ImGui::Checkbox("##value", value);
    if (compact)
    {
        ImGui::PopStyleVar();
    }
    if (changed)
    {
        g_graph.MarkDirty(dirtyReason);
    }
    ImGui::PopID();
    return changed;
}

bool DrawPropertyPathRow(const char* label, const char* id, std::string* value, const char* dirtyReason, const char* tooltip = nullptr)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, !value->empty());
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    char buffer[MAX_PATH]{};
    strncpy_s(buffer, value->c_str(), _TRUNCATE);
    const float buttonWidth = 82.0f;
    const float inputWidth = std::clamp(
        ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        120.0f,
        260.0f);
    ImGui::SetNextItemWidth(inputWidth);
    if (ImGui::InputText("##path", buffer, IM_ARRAYSIZE(buffer)))
    {
        *value = buffer;
        g_graph.MarkDirty(dirtyReason);
    }
    if (ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    if (ImGui::Button("参照", ImVec2(buttonWidth, 0.0f)))
    {
        if (const std::optional<std::filesystem::path> path = ShowHeightmapFileDialog(*value))
        {
            PushUndoSnapshot();
            *value = PathToUtf8(*path);
            g_graph.MarkDirty(dirtyReason);
            editEnded = true;
        }
    }
    ImGui::PopID();
    if (editEnded)
    {
        CommitPropertyUndoEdit();
    }
    return editEnded;
}

bool DrawColorRgbRow(const char* label, const char* id, std::array<float, 3>& value, const std::array<float, 3>& defaultValue)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    bool differsFromDefault = ColorDiffersFromDefault(value, defaultValue);
    DrawPropertyLabel(label, nullptr, differsFromDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float colorWidth = 356.0f;
    ImGui::SetNextItemWidth(colorWidth);
    bool changed = ImGui::ColorEdit3(
        "##rgb",
        value.data(),
        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoAlpha);
    value[0] = std::clamp(value[0], 0.0f, 1.0f);
    value[1] = std::clamp(value[1], 0.0f, 1.0f);
    value[2] = std::clamp(value[2], 0.0f, 1.0f);
    differsFromDefault = ColorDiffersFromDefault(value, defaultValue);
    const std::string defaultValueText = std::format("{:.3f}, {:.3f}, {:.3f}", defaultValue[0], defaultValue[1], defaultValue[2]);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        value = defaultValue;
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

bool DrawCameraFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* format = "%.2f", const char* tooltip = nullptr)
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const bool differsFromDefaultBeforeEdit = FloatDiffersFromDefault(*value, defaultValue);
    DrawPropertyLabel(label, tooltip, differsFromDefaultBeforeEdit);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 76.0f;
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float resetWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
    const float sliderWidth = std::clamp(
        availableWidth - inputWidth - resetWidth - ImGui::GetStyle().ItemInnerSpacing.x,
        80.0f,
        180.0f);
    ImGui::SetNextItemWidth(sliderWidth);
    changed = ImGui::SliderFloat("##slider", value, minValue, maxValue, format) || changed;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (DrawEnterCommitFloatInput("##number", value, format))
    {
        *value = std::clamp(*value, minValue, maxValue);
        changed = true;
    }
    const bool differsFromDefault = FloatDiffersFromDefault(*value, defaultValue);
    const std::string defaultValueText = FormatDefaultFloat(defaultValue, format);
    if (DrawResetToDefaultButton("reset", !differsFromDefault, defaultValueText.c_str()))
    {
        *value = std::clamp(defaultValue, minValue, maxValue);
        changed = true;
    }
    ImGui::PopID();
    return changed;
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
        g_graph.MarkDirty("Shape type changed");
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
    ms.slopeMinDeg = std::clamp(ms.slopeMinDeg, 0.0f, 89.9f);
    ms.slopeMaxDeg = std::clamp(ms.slopeMaxDeg, 0.0f, 89.9f);
    if (ms.slopeMaxDeg < ms.slopeMinDeg)
    {
        std::swap(ms.slopeMinDeg, ms.slopeMaxDeg);
    }
    ms.gamma = std::clamp(ms.gamma, 0.05f, 8.0f);

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
    if (DrawPropertyIntRow("Radius", "MaskCurvatureRadius", &mc.radius, 1, 64, rock::MaskCurvatureSettings{}.radius, "Mask curvature radius changed", true, "周囲平均との差分を見る半径です。小さいほど細かい凹凸、大きいほど広い尾根や谷を拾います。"))
    {
        EvaluateGraph();
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
                g_screenPick.prevCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
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
        constexpr std::array<float, 5> kFluvialDetailLevels = {4.0f, 8.0f, 16.0f, 32.0f, 64.0f};
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
        if (DrawPropertyComboRow("Largest Detail Level (m)", "MaskFluvialLargestDetailLevel", &detailIndex, "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "\0", "流向を計算する前の解析用ハイトをならす最大スケールです。4m は細かい支流や小さな窪みを拾いやすく、64m は小さな凹凸を無視して大きな谷筋を優先します。入力地形そのものは変更しません。", 1))
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
    if (DrawPropertyFloatRow("Largest Detail Level (m)", "SedimentLargestDetailLevel", &sd.largestDetailLevelM, 1.0f, 256.0f, rock::SedimentSettings{}.largestDetailLevelM, "Sediment largest detail level changed", true, "マルチグリッド緩和の最も粗いスケール (m)。この間隔の近傍へのスライドから始め、1 セルまで段階的に半分にしていきます。大きいほど大規模盆地が早く埋まり、小さいほど細部優先。", "%.1f"))
    {
        EvaluateGraph();
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
    sn.smoothingIterations = std::clamp(sn.smoothingIterations, 0, 16);
    sn.largestDetailLevelM = std::clamp(sn.largestDetailLevelM, 1.0f, 1024.0f);
    sn.fillRadius = std::clamp(sn.fillRadius, 1, 8);

    {
        int backendInt = static_cast<int>(sn.backend);
        if (DrawPropertyComboRow("Backend", "SnowBackend", &backendInt, "CPU\0GPU\0\0", "実行バックエンド。GPU (D3D12 compute) は CPU 比で 5-15 倍程度高速。シェーダーコンパイル/ディスパッチ失敗時は CPU に自動フォールバック。CPU は決定論的でデバッグ向き。", static_cast<int>(rock::SnowSettings{}.backend)))
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
    if (DrawPropertyFloatRow("Slope Limit Min (deg)", "SnowSlopeLimitMin", &sn.slopeLimitMinDeg, 0.0f, 89.9f, rock::SnowSettings{}.slopeLimitMinDeg, "Snow slope limit min changed", true, "この角度以下では雪が満杯まで積もります (Emission Amount まるごと)。例: 50° なら緩やかな尾根や谷底にしっかり雪が乗る。下げるほど雪が積もる範囲が狭くなります (=平地でも雪が薄い)。", "%.1f"))
    {
        if (sn.slopeLimitMaxDeg < sn.slopeLimitMinDeg) sn.slopeLimitMaxDeg = sn.slopeLimitMinDeg;
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Slope Limit Max (deg)", "SnowSlopeLimitMax", &sn.slopeLimitMaxDeg, 0.0f, 89.9f, rock::SnowSettings{}.slopeLimitMaxDeg, "Snow slope limit max changed", true, "この角度以上では雪はまったく積もらない (剥き出しの岩肌)。Min と Max の間は smoothstep で滑らかに遷移します。Min と Max の差を広げるほど積雪境界がぼやけます。", "%.1f"))
    {
        if (sn.slopeLimitMaxDeg < sn.slopeLimitMinDeg) sn.slopeLimitMinDeg = sn.slopeLimitMaxDeg;
        EvaluateGraph();
    }
    if (DrawPropertyIntRow("Smoothing Iterations", "SnowSmoothingIterations", &sn.smoothingIterations, 0, 16, rock::SnowSettings{}.smoothingIterations, "Snow smoothing iterations changed", true, "雪の表面を反復的に「平滑化 + 溝埋め」する回数。各反復で snowSurface = heights + thickness の近傍 blur を取り、max(snowSurface, blurred) でセルを更新します。これによりスロープ遷移域の per-cell な厚み揺らぎが消え、また周囲より低いセル (= 溝の底) は雪が増えて埋まります。0 で平滑化なし、6-8 で積雪面が出やすくなります。"))
    {
        EvaluateGraph();
    }
    if (DrawPropertyFloatRow("Mask Max Snow (m)", "SnowMaskMaxSnow", &sn.maskMaxSnow, 0.001f, 50.0f, rock::SnowSettings{}.maskMaxSnow, "Snow mask max snow changed", true, "Snow mask 出力の正規化基準 (m)。`雪厚 / Mask Max Snow` を [0, 1] にクランプして mask に書きます。Emission Amount と同じ値にすれば満雪域が真っ白に出ます。下げるとうっすらした雪も明るく見えるようになります。", "%.2f"))
    {
        EvaluateGraph();
    }

    {
        constexpr std::array<float, 5> kSnowDetailLevels = {4.0f, 8.0f, 16.0f, 32.0f, 64.0f};
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
        if (DrawPropertyComboRow("Largest Detail Level (m)", "SnowLargestDetailLevel", &detailIndex, "4 m\0" "8 m\0" "16 m\0" "32 m\0" "64 m\0" "\0", "GeoGen Snow の Largest detail level 相当です。雪面をならして隙間を埋める最大スケールをメートル単位で選びます。4m は細い隙間まで追いやすく、64m は大きなスケールの積雪面を作ります。", 1))
        {
            detailIndex = std::clamp(detailIndex, 0, static_cast<int>(kSnowDetailLevels.size()) - 1);
            sn.largestDetailLevelM = kSnowDetailLevels[static_cast<size_t>(detailIndex)];
            EvaluateGraph();
        }
    }

    ImGui::EndTable();
    return true;
}

void DrawPropertiesPanel()
{
    const rock::Node* selectedNode = g_graph.FindNode(g_selectedNodeId);
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

    rock::Node* editableNode = g_graph.FindMutableNode(selectedNode->id);
    if (editableNode == nullptr)
    {
        return;
    }
    if (selectedNode->kind == rock::NodeKind::HeightmapLoad && DrawHeightmapLoadProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::Shape && DrawShapeProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::HeightmapBlur && DrawHeightmapBlurProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MultiScaleErosion && DrawMultiScaleErosionProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskNoise && DrawMaskNoiseProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskBlend && DrawMaskBlendProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskLevels && DrawMaskLevelsProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskHeight && DrawMaskHeightProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskSlope && DrawMaskSlopeProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskCurvature && DrawMaskCurvatureProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskFluvial && DrawMaskFluvialProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::Crumbling && DrawCrumblingProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::Rock && DrawRockProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::Sediment && DrawSedimentProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::Snow && DrawSnowProperties(*editableNode))
    {
        return;
    }

    if (selectedNode->kind == rock::NodeKind::Colorize && DrawColorizeProperties(*editableNode))
    {
        return;
    }

}

void DrawDisplaySettingsPanel()
{
    rock::GraphSettings& settings = g_graph.Settings();
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
        const float defaultDistanceBeforeTerrainSizeEdit = DefaultViewportOrbitDistance();
        if (DrawTerrainSizePresetRow("Terrain Size (m)", "GlobalTerrainSizeMeters", &settings.preview.terrainSizeMeters, rock::PreviewSettings{}.terrainSizeMeters, "Terrain size changed", false, "ノードグラフ全体の地形キャンバスの縦横サイズです。Import Heightmap の Scale はこの中で画像が占める実サイズとして扱い、大きければクロップ、小さければ外側を高さ 0 にします。"))
        {
            if (!FloatDiffersFromDefault(g_viewport.orbitDistance, defaultDistanceBeforeTerrainSizeEdit))
            {
                g_viewport.orbitDistance = DefaultViewportOrbitDistance();
            }
            g_graph.MarkDirty("Terrain size changed");
            EvaluateGraph();
        }
        if (DrawResolutionPresetRow("Simulation Resolution", "GlobalSimulationResolution", &settings.preview.simulationResolution, rock::PreviewSettings{}.simulationResolution, "Simulation resolution changed", false, "ノードグラフ全体の地形・マスク評価解像度です。高いほど細かく計算できますが、評価時間とメモリ使用量が増えます。"))
        {
            g_graph.MarkDirty("Simulation resolution changed");
            EvaluateGraph();
        }
        if (DrawResolutionPresetRow("Viewport Mesh Resolution", "DisplayPreviewResolution", &settings.preview.resolution, rock::PreviewSettings{}.resolution, "Preview mesh resolution changed", false, "3D プレビュー用メッシュの細かさです。Simulation Resolution は変えず、表示の分割数だけを変更します。"))
        {
            EvaluateGraph();
            SaveAppSettingsSilently();
        }
        if (DrawPropertyIntRow("LOD", "DisplayPreviewLod", &settings.preview.lod, 0, 4, rock::PreviewSettings{}.lod, "Preview LOD changed", false))
        {
            EvaluateGraph();
            SaveAppSettingsSilently();
        }

        ImGui::SeparatorText("プレビュー画面");
        if (DrawPropertyBoolRow("Mesh Preview", "DisplayMeshPreview", &g_ui.meshPreview, "Mesh preview visibility changed", nullptr, UiState{}.meshPreview, true))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyBoolRow("FPS", "DisplayFps", &g_ui.showFps, "FPS visibility changed", nullptr, UiState{}.showFps, true))
        {
            SaveAppSettingsSilently();
        }
        {
            int backendInt = static_cast<int>(settings.preview.meshBackend);
            if (DrawPropertyComboRow("Mesh Backend", "DisplayMeshBackend", &backendInt, "CPU Mesh\0GPU Displacement\0\0", "プレビュー 3D ビューポートのレンダリング経路。CPU Mesh は CPU 側でメッシュを生成・アップロード(従来動作)、GPU Displacement は静的 UV グリッド + ハイトテクスチャを頂点シェーダーで displace します。GPU 側はテクスチャアップロード(~数 ms)だけで済むため、パラメータ変更時の応答性が上がります(現状はサーフェス描画のみ。シャドウ・ワイヤフレームは CPU パスを併走させます)。", static_cast<int>(rock::PreviewSettings{}.meshBackend)))
            {
                settings.preview.meshBackend = static_cast<rock::MeshPreviewBackend>(std::clamp(backendInt,
                    static_cast<int>(rock::MeshPreviewBackend::CpuMesh),
                    static_cast<int>(rock::MeshPreviewBackend::GpuDisplacement)));
                SaveAppSettingsSilently();
            }
        }
        if (settings.preview.meshBackend == rock::MeshPreviewBackend::GpuDisplacement)
        {
            if (DrawPropertyBoolRow("Tessellation", "DisplayViewportTessellation", &settings.preview.viewportTessellation, "Viewport tessellation changed", "GPU Displacement のビューポート描画だけをハードウェアテセレーションで細分化します。ノード評価やエクスポート用メッシュには影響しません。", rock::PreviewSettings{}.viewportTessellation, true))
            {
                SaveAppSettingsSilently();
            }
            if (settings.preview.viewportTessellation)
            {
                if (DrawPropertyFloatRow("Tess Min", "DisplayTessMin", &settings.preview.tessellationMinFactor, 1.0f, 16.0f, rock::PreviewSettings{}.tessellationMinFactor, "Tessellation min changed", false, "遠景で使う最小テセレーション係数です。"))
                {
                    settings.preview.tessellationMinFactor = std::clamp(settings.preview.tessellationMinFactor, 1.0f, 64.0f);
                    settings.preview.tessellationMaxFactor = std::max(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor);
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Tess Max", "DisplayTessMax", &settings.preview.tessellationMaxFactor, 1.0f, 32.0f, rock::PreviewSettings{}.tessellationMaxFactor, "Tessellation max changed", false, "近景で使う最大テセレーション係数です。高いほど滑らかになりますが描画負荷が増えます。"))
                {
                    settings.preview.tessellationMaxFactor = std::clamp(settings.preview.tessellationMaxFactor, settings.preview.tessellationMinFactor, 64.0f);
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Tess Near (m)", "DisplayTessNear", &settings.preview.tessellationNearDistance, 1.0f, 20000.0f, rock::PreviewSettings{}.tessellationNearDistance, "Tessellation near changed", false, "この距離までは最大テセレーション係数を使います。", "%.0f"))
                {
                    settings.preview.tessellationNearDistance = std::clamp(settings.preview.tessellationNearDistance, 1.0f, 100000.0f);
                    settings.preview.tessellationFarDistance = std::max(settings.preview.tessellationFarDistance, settings.preview.tessellationNearDistance + 1.0f);
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Tess Far (m)", "DisplayTessFar", &settings.preview.tessellationFarDistance, 1.0f, 50000.0f, rock::PreviewSettings{}.tessellationFarDistance, "Tessellation far changed", false, "この距離以遠では最小テセレーション係数へ落とします。", "%.0f"))
                {
                    settings.preview.tessellationFarDistance = std::clamp(settings.preview.tessellationFarDistance, settings.preview.tessellationNearDistance + 1.0f, 200000.0f);
                    SaveAppSettingsSilently();
                }
            }
        }

        if (DrawPropertyBoolRow("Surface", "DisplaySurface", &settings.preview.showSurface, "Surface visibility changed", nullptr, rock::PreviewSettings{}.showSurface, true))
        {
            SaveAppSettingsSilently();
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
                SaveAppSettingsSilently();
            }
        }
        if (DrawPropertyBoolRow("Grid", "DisplayGrid", &settings.preview.showGrid, "Grid visibility changed", nullptr, rock::PreviewSettings{}.showGrid, true))
        {
            SaveAppSettingsSilently();
        }
        if (settings.preview.showGrid)
        {
            if (DrawPropertyIntRow("Grid Cells", "DisplayGridCells", &settings.preview.gridCellCount, 1, 200, rock::PreviewSettings{}.gridCellCount, "Grid cell count changed", false, "グリッド全体の1辺あたりのマス数です。10なら10 x 10です。"))
            {
                settings.preview.gridCellCount = std::clamp(settings.preview.gridCellCount, 1, 200);
                SaveAppSettingsSilently();
            }
            if (DrawPropertyFloatRow("Grid Cell Size (m)", "DisplayGridCellSize", &settings.preview.gridCellSizeMeters, 1.0f, 10000.0f, rock::PreviewSettings{}.gridCellSizeMeters, "Grid cell size changed", false, "グリッド1マスの長さです。"))
            {
                settings.preview.gridCellSizeMeters = std::clamp(settings.preview.gridCellSizeMeters, 1.0f, 10000.0f);
                SaveAppSettingsSilently();
            }
            if (DrawColorRgbRow("Grid Color", "DisplayGridColor", settings.preview.gridColor, rock::PreviewSettings{}.gridColor))
            {
                SaveAppSettingsSilently();
            }
        }
        int displayModeInt = ToDisplayModeIndex(CurrentViewportDisplayMode(settings));
        if (DrawPropertyComboRow("表示モード", "ViewportDisplayMode", &displayModeInt, "シンプル\0PBR\0天球\0\0", "シンプル: フラットで軽い表示。PBR: 単色背景でリアル寄りのライティング。天球: 天球背景とリアル寄りのライティングです。", ToDisplayModeIndex(ViewportDisplayMode::Simple)))
        {
            ApplyViewportDisplayMode(settings, DisplayModeFromIndex(std::clamp(displayModeInt, 0, 2)));
            SaveAppSettingsSilently();
        }
        const ViewportDisplayMode displayMode = CurrentViewportDisplayMode(settings);
        ImGui::SeparatorText("地表");
        if (displayMode != ViewportDisplayMode::Sky)
        {
            if (DrawColorRgbRow("ビューポート背景色", "ViewportBackgroundColor", settings.preview.viewportBackground, rock::PreviewSettings{}.viewportBackground))
            {
                SaveAppSettingsSilently();
            }
        }
        if (displayMode != ViewportDisplayMode::Simple)
        {
            if (DrawColorRgbRow("Albedo", "DisplayPbrAlbedo", settings.preview.pbrAlbedo, rock::PreviewSettings{}.pbrAlbedo))
            {
                SaveAppSettingsSilently();
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
                SaveAppSettingsSilently();
            }
            if (DrawPropertyBoolRow("近い地形でマスク表示", "DisplayMaskUseNearestHeightmap", &settings.preview.maskPreviewUseNearestHeightmap, "Mask preview nearest heightmap toggled", "Mask Noise / Mask Blend / Mask Levels など、ハイトマップ参照を直接持たないマスクノードをプレビューするとき、入力側をたどって見つかった一番近い Heightmap を表示用の地形に使います。見つからない場合は従来どおり平面表示します。", rock::PreviewSettings{}.maskPreviewUseNearestHeightmap, true))
            {
                EvaluateGraph();
                SaveAppSettingsSilently();
            }
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void DrawSkySettingsPanel()
{
    rock::GraphSettings& settings = g_graph.Settings();
    const float headerRightPadding = 10.0f;
    const float sectionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - headerRightPadding);
    ImGui::BeginChild("SkySettingsSection", ImVec2(sectionWidth, 0.0f), false);

    if (ImGui::CollapsingHeader("太陽と影", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("SunShadowSettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            ImGui::SeparatorText("太陽");
            int sunModeInt = static_cast<int>(settings.preview.sunDirectionMode);
            if (DrawPropertyComboRow("Sun Mode", "DisplaySunMode", &sunModeInt, "Manual\0Date Time\0\0", "Manual は方位角と高度を直接指定します。Date Time は緯度、経度、月日、時刻、UTC Offset からそれらしい太陽位置を計算します。", static_cast<int>(rock::PreviewSettings{}.sunDirectionMode)))
            {
                settings.preview.sunDirectionMode = static_cast<rock::SunDirectionMode>(std::clamp(sunModeInt,
                    static_cast<int>(rock::SunDirectionMode::Manual),
                    static_cast<int>(rock::SunDirectionMode::DateTime)));
                SaveAppSettingsSilently();
            }
            if (settings.preview.sunDirectionMode == rock::SunDirectionMode::DateTime)
            {
                if (DrawPropertyFloatRow("Latitude", "SunLatitude", &settings.preview.sunLatitudeDegrees, -90.0f, 90.0f, rock::PreviewSettings{}.sunLatitudeDegrees, "Sun latitude changed", false, "太陽位置計算に使う緯度です。北緯を正、南緯を負で指定します。"))
                {
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Longitude", "SunLongitude", &settings.preview.sunLongitudeDegrees, -180.0f, 180.0f, rock::PreviewSettings{}.sunLongitudeDegrees, "Sun longitude changed", false, "太陽位置計算に使う経度です。東経を正、西経を負で指定します。"))
                {
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("UTC Offset", "SunUtcOffset", &settings.preview.sunUtcOffsetHours, -12.0f, 14.0f, rock::PreviewSettings{}.sunUtcOffsetHours, "Sun UTC offset changed", false, "日時の解釈に使う UTC からの時差です。夏時間やタイムゾーンDBは使わず、ここで指定した値をそのまま使います。", "%.1f"))
                {
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyIntRow("Month", "SunMonth", &settings.preview.sunMonth, 1, 12, rock::PreviewSettings{}.sunMonth, "Sun month changed", false, "太陽位置計算に使う月です。年は固定の非うるう年として扱います。"))
                {
                    settings.preview.sunDay = std::clamp(settings.preview.sunDay, 1, DaysInMonth(settings.preview.sunMonth));
                    SaveAppSettingsSilently();
                }
                const int maxDay = DaysInMonth(settings.preview.sunMonth);
                if (DrawPropertyIntRow("Day", "SunDay", &settings.preview.sunDay, 1, maxDay, std::clamp(rock::PreviewSettings{}.sunDay, 1, maxDay), "Sun day changed", false, "太陽位置計算に使う日です。月に応じて最大日数を制限します。"))
                {
                    settings.preview.sunDay = std::clamp(settings.preview.sunDay, 1, maxDay);
                    SaveAppSettingsSilently();
                }
                if (DrawTimeOfDayRow("Time", "SunTime", &settings.preview.sunTimeHours, rock::PreviewSettings{}.sunTimeHours, "Sun time changed", "ローカル時刻です。0:00 から 24:00 までをスライダーで指定します。"))
                {
                    SaveAppSettingsSilently();
                }

                const SunPositionDegrees computedSun = EffectiveSunPosition(settings.preview);
                DrawReadOnlyFloatRow("Computed Azimuth", computedSun.azimuth, "%.2f", "計算されたアプリ内方位角です。0° が南(Z+)、90° が東(X+)です。");
                DrawReadOnlyFloatRow("Computed Elevation", computedSun.elevation, "%.2f", "計算された太陽高度です。");
            }
            else
            {
                if (DrawPropertyFloatRow("Sun Azimuth (deg)", "DisplaySunAzimuth", &settings.preview.sunAzimuthDegrees, 0.0f, 360.0f, rock::PreviewSettings{}.sunAzimuthDegrees, "Sun azimuth changed", false, "太陽の水平角度です。0° が南(Z+)、90° が東(X+)です。地形の溝が読みやすい方向へ回せます。"))
                {
                    SaveAppSettingsSilently();
                }
                if (DrawPropertyFloatRow("Sun Elevation (deg)", "DisplaySunElevation", &settings.preview.sunElevationDegrees, -10.0f, 89.0f, rock::PreviewSettings{}.sunElevationDegrees, "Sun elevation changed", false, "太陽の高さです。低いほど影が長く、凹凸が強調されます。0° は地平線、負値は地平より下 (夜遷移の確認用)。"))
                {
                    SaveAppSettingsSilently();
                }
            }
            if (DrawPropertyFloatRow("Sun Intensity", "DisplaySunIntensity", &settings.preview.sunIntensity, 0.0f, 5.0f, rock::PreviewSettings{}.sunIntensity, "Sun intensity changed", false, "直射光の強さです。"))
            {
                SaveAppSettingsSilently();
            }
            if (DrawPropertyFloatRow("Ambient", "DisplayAmbientStrength", &settings.preview.ambientStrength, 0.0f, 2.0f, rock::PreviewSettings{}.ambientStrength, "Ambient strength changed", false, "影側を持ち上げる環境光の強さです。"))
            {
                SaveAppSettingsSilently();
            }
            ImGui::SeparatorText("影");
            if (DrawPropertyFloatRow("Shadow Strength", "DisplayShadowStrength", &settings.preview.shadowStrength, 0.0f, 1.0f, rock::PreviewSettings{}.shadowStrength, "Shadow strength changed", false, "シャドウマップで落ちる影の濃さです。"))
            {
                SaveAppSettingsSilently();
            }
            if (DrawShadowResolutionPresetRow("Shadow Map Resolution", "DisplayShadowMapResolution", &settings.preview.shadowMapResolution, rock::PreviewSettings{}.shadowMapResolution, "Shadow map resolution changed", false, "太陽方向から見た深度マップの解像度です。高いほど影の輪郭が細かくなりますが描画負荷が増えます。"))
            {
                SaveAppSettingsSilently();
            }
            if (DrawPropertyFloatRow("Shadow Bias", "DisplayShadowBias", &settings.preview.shadowBias, 0.0f, 0.05f, rock::PreviewSettings{}.shadowBias, "Shadow bias changed", false, "影のにじみや縞を抑えるための深度オフセットです。大きすぎると影が浮いて見えます。"))
            {
                SaveAppSettingsSilently();
            }

            ImGui::EndTable();
        }
    }

    if (!ImGui::CollapsingHeader("天球と雲", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("SkySettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        rock::SkySettings& sky = settings.sky;
            ImGui::SeparatorText("天球 (大気散乱)");
            DrawPropertyFloatRow("大気厚み (密度)", "SkyAtmosphereDensity", &sky.atmosphereDensity, 0.05f, 5.0f, rock::SkySettings{}.atmosphereDensity, "Sky atmosphere density changed", false, "Rayleigh 散乱係数 β_R と地平ヘイズの倍率。1.0 が地球標準、0.5 で薄い大気、2-3 で濃い大気。地形の遠景フォグもこの値から自動で決まります。");
            DrawPropertyFloatRow("ヘイズ (Mie 強度)", "SkyMieStrength", &sky.mieStrength, 0.0f, 8.0f, rock::SkySettings{}.mieStrength, "Sky mie strength changed", false, "Mie 散乱の強さ。0.2 前後が編集ビュー向けの標準です。大きいほど太陽方向の霞とグローが強くなりますが、地平の暖色帯も出やすくなります。0 で純 Rayleigh。");
            DrawPropertyFloatRow("Mie 偏向 (g)", "SkyMieG", &sky.mieEccentricity, -0.95f, 0.95f, rock::SkySettings{}.mieEccentricity, "Sky mie g changed", false, "Henyey-Greenstein g 値。0 で等方散乱、正で前方 (太陽方向) 散乱が強くなりグローが太陽周りに集中。0.7-0.85 が現実的。");
            DrawColorRgbRow("地面アルベド", "SkyGroundAlbedo", sky.groundAlbedo, rock::SkySettings{}.groundAlbedo);
            DrawPropertyFloatRow("太陽サイズ (deg)", "SkySunSize", &sky.sunSizeDegrees, 0.1f, 20.0f, rock::SkySettings{}.sunSizeDegrees, "Sky sun size changed", false, "太陽ディスクの直径(度)。実際の太陽は約 0.5 度ですが、視認性のためデフォルトはやや大きめです。");
            DrawPropertyFloatRow("太陽グロー", "SkySunGlow", &sky.sunGlowStrength, 0.0f, 2.0f, rock::SkySettings{}.sunGlowStrength, "Sky sun glow changed", false, "太陽周辺の柔らかい光の強さ。0 でグロー無し。");

            ImGui::SeparatorText("ボリューム雲");
            rock::CloudSettings& clouds = settings.clouds;
            DrawPropertyBoolRow("有効", "CloudEnabled", &clouds.enabled, "Clouds enabled toggled", "ボリューム雲のレイマーチ描画を有効化します。3D 密度テクスチャ (128³ R8 = 2MB) を生成し、雲帯 [Altitude Min, Max] とのレイ交差をフルスクリーンパスで毎フレーム積分します。", rock::CloudSettings{}.enabled, true);
            if (clouds.enabled)
            {
                DrawPropertyIntRow("Cloud Seed", "CloudSeed", &clouds.seed, 0, 999999, rock::CloudSettings{}.seed, "Cloud seed changed", false, "3D 密度ノイズのシード。変更すると雲のパターンが変わります(テクスチャを再生成)。");
                DrawPropertyFloatRow("Coverage", "CloudCoverage", &clouds.coverage, 0.0f, 1.0f, rock::CloudSettings{}.coverage, "Cloud coverage changed", false, "空に占める雲の割合。0 で雲無し、1 で空一面が雲。");
                DrawPropertyFloatRow("Density", "CloudDensity", &clouds.densityMultiplier, 0.0f, 4.0f, rock::CloudSettings{}.densityMultiplier, "Cloud density changed", false, "雲の濃さ倍率。大きいほど雲が不透明になります。");
                DrawPropertyFloatRow("Altitude Min (m)", "CloudAltMin", &clouds.altitudeMin, 0.0f, 8000.0f, rock::CloudSettings{}.altitudeMin, "Cloud altitude min changed", false, "雲帯の下限高度 (m)。地形をすっぽり包むには地形最高点より低い値、上に浮かべるなら高い値を指定。", "%.0f");
                DrawPropertyFloatRow("Altitude Max (m)", "CloudAltMax", &clouds.altitudeMax, 0.0f, 12000.0f, rock::CloudSettings{}.altitudeMax, "Cloud altitude max changed", false, "雲帯の上限高度 (m)。Max - Min が雲層の厚さです。", "%.0f");
                DrawPropertyFloatRow("Horizontal Scale (m)", "CloudHorizScale", &clouds.horizontalScale, 200.0f, 30000.0f, rock::CloudSettings{}.horizontalScale, "Cloud scale changed", false, "雲の水平スケール。大きいほど雲塊が大きく、小さいほど細かい雲になります。", "%.0f");
                DrawPropertyFloatRow("Field Radius (m)", "CloudFieldRadius", &clouds.fieldRadius, 200.0f, 50000.0f, rock::CloudSettings{}.fieldRadius, "Cloud field radius changed", false, "地形の中心を原点にした、雲が存在する円形フィールドの半径。大きくするとより遠くまで雲が広がります。地形と同程度にすると地形の周りだけに雲が出ます。", "%.0f");
                DrawPropertyFloatRow("Field Falloff (m)", "CloudFieldFalloff", &clouds.fieldFalloff, 50.0f, 20000.0f, rock::CloudSettings{}.fieldFalloff, "Cloud field falloff changed", false, "フィールド端のフェードアウト幅。大きいほど雲がじわっと消え、小さいと境界がくっきりします。", "%.0f");
                DrawPropertyFloatRow("Absorption", "CloudAbsorption", &clouds.absorption, 0.0f, 0.5f, rock::CloudSettings{}.absorption, "Cloud absorption changed", false, "Beer-Lambert の吸収係数。大きいほど雲がはっきり不透明になります。", "%.4f");
                DrawColorRgbRow("Cloud Color", "CloudColor", clouds.color, rock::CloudSettings{}.color);
                DrawPropertyBoolRow("雲を動かす", "CloudAnimate", &clouds.animate, "Cloud animation toggled", "ON のときだけ風向きと速度を使って雲を流します。OFF では速度の設定値を保持したまま静止表示します。", rock::CloudSettings{}.animate, true);
                if (clouds.animate)
                {
                    DrawPropertyFloatRow("Wind Speed (m/s)", "CloudWindSpeed", &clouds.windSpeedMetersPerSec, 0.0f, 200.0f, rock::CloudSettings{}.windSpeedMetersPerSec, "Cloud wind speed changed", false, "雲が流れる速度 (m/s)。動かすとフレーム毎にビューポートが再描画され負荷が増えます。");
                    DrawPropertyFloatRow("Wind Direction (deg)", "CloudWindDir", &clouds.windDirectionDegrees, 0.0f, 360.0f, rock::CloudSettings{}.windDirectionDegrees, "Cloud wind direction changed", false, "雲が流れる向きです。度数で指定します。北=0、東=90。", "%.0f");
                }
                DrawPropertyIntRow("Quality (samples)", "CloudQuality", &clouds.qualitySamples, 8, 96, rock::CloudSettings{}.qualitySamples, "Cloud quality changed", false, "1 ピクセルあたりのレイマーチサンプル数。大きいほど雲のディテールが上がりますが負荷も増えます。32 が標準、低スペックなら 16、高品質なら 64。");
                DrawPropertyFloatRow("Shadow Strength", "CloudShadowStrength", &clouds.shadowStrength, 0.0f, 1.0f, rock::CloudSettings{}.shadowStrength, "Cloud shadow strength changed", false, "雲が地形に落とす影の強さ。0 で影無し、1 で完全に暗くなります。太陽方向に projection した雲の透過率を地形シェーダーで乗算します。");
                if (DrawShadowResolutionPresetRow("Shadow Resolution", "CloudShadowResolution", &clouds.shadowResolution, rock::CloudSettings{}.shadowResolution, "Cloud shadow resolution changed", false, "雲影テクスチャの解像度 (片辺ピクセル数)。1024 で約 1MB。大きいほど影の輪郭が細かくなりますが生成負荷が増えます。"))
                {
                    SaveAppSettingsSilently();
                }
                DrawPropertyIntRow("Shadow Samples", "CloudShadowSamples", &clouds.shadowSamples, 4, 64, rock::CloudSettings{}.shadowSamples, "Cloud shadow samples changed", false, "雲影テクスチャ生成時に太陽方向へ撃つレイのサンプル数。大きいほど厚い雲の影が正確になりますが生成時間も増えます。16 が標準。");
                DrawPropertyIntRow("Light Samples", "CloudLightSamples", &clouds.lightSamples, 0, 16, rock::CloudSettings{}.lightSamples, "Cloud light samples changed", false, "雲内自己遮蔽の太陽方向レイマーチ段数。0 で無効化(従来の上下ランプのみ)、6 が標準。大きいほど雲塊の陰影がはっきりしますが負荷も増えます。");
                DrawPropertyFloatRow("Light Step (m)", "CloudLightStep", &clouds.lightStepMeters, 1.0f, 1000.0f, rock::CloudSettings{}.lightStepMeters, "Cloud light step changed", false, "自己遮蔽レイマーチの 1 ステップあたりの距離 (m)。Light Samples × Light Step が太陽方向への投光距離になります。雲スケールに対して短すぎると深い雲の中まで届かず、長すぎるとサンプルが粗くなります。", "%.0f");
                DrawPropertyFloatRow("Phase Eccentricity", "CloudPhaseG", &clouds.phaseEccentricity, -0.99f, 0.99f, rock::CloudSettings{}.phaseEccentricity, "Cloud phase eccentricity changed", false, "Henyey-Greenstein 位相関数の g 値。0 で等方散乱、正値で前方散乱(逆光時に太陽周りが明るくなるシルバーライニング)、負値で後方散乱。0.4 前後が雲らしい見た目。");
            }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void DrawCameraPanel()
{
    if (ImGui::Button("Reset View"))
    {
        ResetViewport();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
    {
        ImGui::SetTooltip("カメラの向き、距離、パンを既定値に戻します。ショートカット: F");
    }

    ImGui::Spacing();
    if (ImGui::BeginTable("CameraRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        DrawCameraFloatRow("FOV", "FovDegrees", &g_viewport.fovDegrees, 15.0f, 90.0f, kDefaultViewportFovDegrees, "%.1f",
            "垂直画角です。小さいほど望遠、大きいほど広角になります。焦点距離 (mm) と連動します。");
        float focalLengthMm = CameraFocalLengthMmFromFovYDegrees(g_viewport.fovDegrees);
        if (DrawCameraFloatRow("焦点距離 (mm)", "FocalLengthMm", &focalLengthMm, 1.0f, 200.0f, CameraFocalLengthMmFromFovYDegrees(kDefaultViewportFovDegrees), "%.1f",
            "35mm フルサイズ相当のレンズ焦点距離です。画角と DOF のぼけ量の両方に反映されます。"))
        {
            g_viewport.fovDegrees = CameraFovYDegreesFromFocalLengthMm(focalLengthMm);
        }
        DrawCameraFloatRow("Distance", "OrbitDistance", &g_viewport.orbitDistance, 1.0f, kMaxViewportOrbitDistance, DefaultViewportOrbitDistance(), "%.1f",
            "注視点からカメラまでの距離です。マウスホイールのオービット距離と同じ値です。");
        DrawCameraFloatRow("Yaw", "ViewportYaw", &g_viewport.yaw, -3.14159f, 3.14159f, kDefaultViewportYaw, "%.3f",
            "カメラの水平回転です。地形を左右から見る向きを調整します。単位はラジアンです。");
        DrawCameraFloatRow("Pitch", "ViewportPitch", &g_viewport.pitch, -1.25f, 1.25f, kDefaultViewportPitch, "%.3f",
            "カメラの上下角です。高い視点や低い視点から地形を見る角度を調整します。単位はラジアンです。");

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Depth of Field");
    rock::PreviewSettings& preview = g_graph.Settings().preview;
    if (ImGui::BeginTable("CameraDofRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        DrawPropertyBoolRow("有効", "DofEnabled", &preview.depthOfFieldEnabled, "Depth of Field toggled",
            "ビューポート表示だけにかかる被写界深度です。地形データや OBJ エクスポートには影響しません。",
            rock::PreviewSettings{}.depthOfFieldEnabled, true);
        if (preview.depthOfFieldEnabled)
        {
            DrawPropertyFloatRow("F 値", "DofFStop", &preview.dofFStop, 0.7f, 32.0f, rock::PreviewSettings{}.dofFStop, "Depth of Field f-stop changed", false,
                "絞り値です。小さいほどぼけが強く、大きいほど深くピントが合います。", "%.1f");
            DrawPropertyFloatRow("フォーカス距離 (m)", "DofFocusDistance", &preview.dofFocusDistanceMeters, 0.1f, 20000.0f, rock::PreviewSettings{}.dofFocusDistanceMeters, "Depth of Field focus distance changed", false,
                "カメラからピント面までの距離です。Orbit Distance と近い値にすると注視点付近にピントが合います。", "%.1f", ImGuiSliderFlags_Logarithmic);
            DrawPropertyFloatRow("センサー高さ (mm)", "DofSensorHeight", &preview.dofSensorHeightMm, 4.0f, 80.0f, rock::PreviewSettings{}.dofSensorHeightMm, "Depth of Field sensor height changed", false,
                "Circle of Confusion の換算に使うセンサー高さです。フルサイズ横位置なら 24mm が標準です。", "%.1f");
            DrawPropertyFloatRow("最大ぼけ (px)", "DofMaxBlur", &preview.dofMaxBlurPixels, 0.0f, 64.0f, rock::PreviewSettings{}.dofMaxBlurPixels, "Depth of Field max blur changed", false,
                "表示上の最大ぼけ半径です。現実値ベースの操作感を保ちながら、重くなりすぎるぼけを抑えます。", "%.1f");
            int apertureShape = std::clamp(preview.dofApertureShape, 0, 4);
            if (DrawPropertyComboRow("絞り形状", "DofApertureShape", &apertureShape, "丸\0三角形\0六角形\0八角形\0カスタム\0\0",
                "ぼけのサンプル形状です。多角形にすると絞り羽根由来の角ばったボケになります。", rock::PreviewSettings{}.dofApertureShape))
            {
                preview.dofApertureShape = std::clamp(apertureShape, 0, 4);
                g_graph.MarkDirty("Depth of Field aperture shape changed");
            }
            if (preview.dofApertureShape == 4)
            {
                DrawPropertyIntRow("絞り羽根", "DofApertureBlades", &preview.dofApertureBlades, 3, 12, rock::PreviewSettings{}.dofApertureBlades, "Depth of Field aperture blades changed", false,
                    "カスタム多角形ボケの羽根数です。");
            }
            DrawPropertyFloatRow("絞り回転 (deg)", "DofApertureRotation", &preview.dofApertureRotationDegrees, -180.0f, 180.0f, rock::PreviewSettings{}.dofApertureRotationDegrees, "Depth of Field aperture rotation changed", false,
                "多角形ボケの角度です。丸ボケでは見た目にほぼ影響しません。", "%.1f");
            DrawPropertyFloatRow("ハイライト強調", "DofHighlightBoost", &preview.dofHighlightBoost, 0.0f, 4.0f, rock::PreviewSettings{}.dofHighlightBoost, "Depth of Field highlight boost changed", false,
                "明るいサンプルを少し強め、点光源や明るい稜線のボケを目立たせます。", "%.2f");
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Right-handed / Y-up");
    ImGui::TextDisabled("Grid: %d x %d, %.0f m cells",
        g_graph.Settings().preview.gridCellCount,
        g_graph.Settings().preview.gridCellCount,
        g_graph.Settings().preview.gridCellSizeMeters);
}

void DrawDebugPanel()
{
    rock::GraphSettings& settings = g_graph.Settings();
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    const PreviewRenderStats& renderStats = g_gpuMeshPreview.renderStats;
    ImGui::SeparatorText("Viewport Debug");
    if (ImGui::BeginTable("DebugViewportRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        if (DrawPropertyBoolRow("Draw Calls", "DebugDrawCalls", &g_ui.showDrawStats, "Draw stats visibility changed",
                "Shows the latest preview draw-call count in the viewport overlay.",
                UiState{}.showDrawStats, true))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyBoolRow("Wireframe", "DebugWireframe", &settings.preview.showWireframe, "Wireframe visibility changed",
                "Shows mesh edges for topology debugging. High viewport resolutions can make this expensive.",
                rock::PreviewSettings{}.showWireframe, true))
        {
            SaveAppSettingsSilently();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Text("Graph Version: %llu", static_cast<unsigned long long>(evaluation.version));
    ImGui::Text("%s", g_lastEvaluationDuration.c_str());
    ImGui::TextColored(evaluation.dirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f), "%s", evaluation.dirty ? "Dirty" : "Evaluated");
    ImGui::TextWrapped("%s", evaluation.status.c_str());

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

    ImGui::SeparatorText("Preview");
    ImGui::Text("Stage: %s", rock::ToString(evaluation.previewStage).data());
    ImGui::Text("Backend: %s", renderStats.gpuDisplacement ? (renderStats.tessellation ? "GPU Displacement + Tessellation" : "GPU Displacement") : "CPU Mesh");
    ImGui::Text("Render Target: %d x %d", renderStats.renderTargetWidth, renderStats.renderTargetHeight);
    ImGui::Text("Draw Calls: %u (%u indexed)", renderStats.drawCalls, renderStats.indexedDrawCalls);
    ImGui::Text("Submitted: %llu verts / %llu tris / %llu lines",
        static_cast<unsigned long long>(renderStats.submittedVertices),
        static_cast<unsigned long long>(renderStats.submittedTriangles),
        static_cast<unsigned long long>(renderStats.submittedLines));
    if (renderStats.tessellation)
    {
        ImGui::Text("Patches: %u, Tess Max: %.1f", renderStats.submittedPatches, renderStats.tessellationMaxFactor);
    }
    ImGui::Text("Passes: %s%s%s%s%s%s",
        renderStats.shadowPass ? "Shadow " : "",
        renderStats.skyPass ? "Sky " : "",
        renderStats.surfacePass ? "Surface " : "",
        renderStats.gridPass ? "Grid " : "",
        renderStats.wireframePass ? "Wireframe " : "",
        renderStats.cloudsPass ? "Clouds " : "");
    ImGui::SeparatorText("Displayed Mesh");
    ImGui::Text("Mesh Resolution: %d", renderStats.displayMeshResolution);
    ImGui::Text("Vertices: %llu", static_cast<unsigned long long>(displayedVertices));
    ImGui::Text("Triangles: %llu", static_cast<unsigned long long>(displayedTriangles));
    ImGui::SeparatorText("Evaluated Mesh");
    ImGui::Text("Vertices: %zu", evaluation.previewMesh.vertices.size());
    ImGui::Text("Edges: %zu", evaluation.previewMesh.edges.size());
    ImGui::Text("Triangles: %zu", evaluation.previewMesh.triangles.size());
}

void DrawAssetExportPanel()
{
    ImGui::Columns(3, nullptr, false);
    ImGui::TextUnformatted("Preview mesh");
    ImGui::Text("%s", g_graph.Evaluation().dirty ? "needs evaluation" : "ready");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Topology");
    ImGui::Text("%zu verts / %zu tris",
                g_graph.Evaluation().previewMesh.vertices.size(),
                g_graph.Evaluation().previewMesh.triangles.size());
    ImGui::NextColumn();
    ImGui::TextUnformatted("Export");
    if (ImGui::Button("Export OBJ"))
    {
        EnsurePreviewMesh();

        std::string error;
        const std::filesystem::path exportPath = std::filesystem::path("exports") / "terrain_mesh.obj";
        if (rock::ExportMeshObj(g_graph.Evaluation().previewMesh, exportPath, &error))
        {
            g_exportStatus = "Exported " + exportPath.string();
        }
        else
        {
            g_exportStatus = "Export failed: " + error;
        }
    }
    ImGui::TextWrapped("%s", g_exportStatus.c_str());
    ImGui::Columns(1);
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

bool BeginStyledTabItem(const char* label)
{
    const bool open = ImGui::BeginTabItem(label);
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
    constexpr float splitterWidth = 7.0f;
    const float maxLeftWidth = std::max(minLeftWidth, totalWidth - minRightWidth - splitterWidth);
    *leftWidth = std::clamp(*leftWidth, minLeftWidth, maxLeftWidth);

    ImGui::SameLine();
    ImGui::PushID(id);
    ImGui::InvisibleButton("##splitter", ImVec2(splitterWidth, height));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (active)
    {
        g_layoutSplitterActive = true;
    }
    if (active)
    {
        *leftWidth = std::clamp(*leftWidth + ImGui::GetIO().MouseDelta.x, minLeftWidth, maxLeftWidth);
    }
    if (hovered || active)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec4 color = active
        ? g_themeManager.AppColor("accent", ImVec4(0.52f, 0.70f, 0.59f, 1.0f))
        : g_themeManager.AppColor("border", ImVec4(0.22f, 0.24f, 0.23f, 1.0f));
    const float lineX = std::floor((min.x + max.x) * 0.5f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(lineX, min.y),
        ImVec2(lineX, max.y),
        ColorToU32(color),
        active ? 2.0f : 1.0f);
    const bool released = ImGui::IsItemDeactivated();
    ImGui::PopID();
    ImGui::SameLine();
    return released;
}

bool DrawHorizontalSplitter(const char* id, float* topHeight, float totalHeight, float minTopHeight, float minBottomHeight)
{
    constexpr float splitterHeight = 7.0f;
    const float maxTopHeight = std::max(minTopHeight, totalHeight - minBottomHeight - splitterHeight);
    *topHeight = std::clamp(*topHeight, minTopHeight, maxTopHeight);

    ImGui::PushID(id);
    ImGui::InvisibleButton("##splitter", ImVec2(-1.0f, splitterHeight));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (active)
    {
        g_layoutSplitterActive = true;
    }
    if (active)
    {
        *topHeight = std::clamp(*topHeight + ImGui::GetIO().MouseDelta.y, minTopHeight, maxTopHeight);
    }
    if (hovered || active)
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }

    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec4 color = active
        ? g_themeManager.AppColor("accent", ImVec4(0.52f, 0.70f, 0.59f, 1.0f))
        : g_themeManager.AppColor("border", ImVec4(0.22f, 0.24f, 0.23f, 1.0f));
    const float lineY = std::floor((min.y + max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(min.x, lineY),
        ImVec2(max.x, lineY),
        ColorToU32(color),
        active ? 2.0f : 1.0f);
    const bool released = ImGui::IsItemDeactivated();
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
        if (BeginStyledTabItem("3Dビュー"))
        {
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + ImGui::GetContentRegionAvail().y);
            DrawViewportCube(min, max, timeSeconds);
            ImGui::Dummy(ImGui::GetContentRegionAvail());
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem("2Dビュー"))
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
        if (BeginStyledTabItem("ノードネットワーク"))
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
    if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        ResetViewport();
    }
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F12, false))
    {
        std::filesystem::path screenshotPath;
        std::string error;
        if (terrain::CaptureWindowScreenshot(g_hwnd, ScreenshotDirectory(), &screenshotPath, &error))
        {
            g_projectStatus = "Screenshot saved " + PathToUtf8(screenshotPath);
        }
        else
        {
            g_projectStatus = "Screenshot failed: " + error;
        }
    }

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 9.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(10.0f, 7.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    if (ImGui::BeginMenuBar())
    {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5.0f);
        if (ImGui::BeginMenu("ファイル"))
        {
            if (ImGui::MenuItem("新規", "Ctrl+N"))
            {
                NewProject();
            }
            if (ImGui::MenuItem("開く", "Ctrl+O"))
            {
                if (const std::optional<std::filesystem::path> path = ShowProjectFileDialog(false))
                {
                    std::string error;
                    if (!LoadProjectFromFile(*path, &error))
                    {
                        g_projectStatus = "Load failed: " + error;
                    }
                }
            }
            if (PruneMissingRecentProjectPaths())
            {
                SaveAppSettingsSilently();
            }
            if (ImGui::BeginMenu("最近使ったファイル", !g_recentProjectPaths.empty()))
            {
                for (size_t index = 0; index < g_recentProjectPaths.size(); ++index)
                {
                    const std::filesystem::path& recentPath = g_recentProjectPaths[index];
                    const std::string label =
                        std::to_string(index + 1) + ". " + PathToUtf8(recentPath.filename()) + "##RecentProject" + std::to_string(index);
                    if (ImGui::MenuItem(label.c_str()))
                    {
                        std::string error;
                        if (!LoadProjectFromFile(recentPath, &error))
                        {
                            g_projectStatus = "Load failed: " + error;
                        }
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s", PathToUtf8(recentPath).c_str());
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("履歴をクリア"))
                {
                    g_recentProjectPaths.clear();
                    SaveAppSettingsSilently();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("保存", "Ctrl+S"))
            {
                SaveCurrentProject();
            }
            if (ImGui::MenuItem("名前を付けて保存"))
            {
                if (const std::optional<std::filesystem::path> path = ShowProjectFileDialog(true))
                {
                    std::string error;
                    if (!SaveProjectToFile(*path, &error))
                    {
                        g_projectStatus = "Save failed: " + error;
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("プロジェクトの保存場所を開く", nullptr, false, !g_projectPath.empty()))
            {
                OpenFolderInExplorer(ProjectFolder());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("終了"))
            {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("編集"))
        {
            if (ImGui::MenuItem("元に戻す", "Ctrl+Z", false, !g_undoStack.empty()))
            {
                UndoGraphEdit();
            }
            if (ImGui::MenuItem("やり直し", "Ctrl+Y", false, !g_redoStack.empty()))
            {
                RedoGraphEdit();
            }
            ImGui::Separator();
            ImGui::MenuItem("コピー", "Ctrl+C", false, false);
            ImGui::MenuItem("貼り付け", "Ctrl+V", false, false);
            ImGui::MenuItem("削除", "Delete", false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("表示"))
        {
            if (ImGui::MenuItem("Mesh", nullptr, g_ui.meshPreview))
            {
                g_ui.meshPreview = !g_ui.meshPreview;
                SaveAppSettingsSilently();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("設定"))
        {
            if (ImGui::BeginMenu("UIテーマ"))
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
            ImGui::MenuItem("環境設定", nullptr, false, false);
            ImGui::MenuItem("ショートカット設定", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    ImGui::PopStyleVar(5);

    const ImVec2 content = ImGui::GetContentRegionAvail();
    const float statusBarHeight = ImGui::GetTextLineHeight() + 16.0f;
    const float workHeight = std::max(260.0f, content.y - statusBarHeight);
    constexpr float mainSplitterWidth = 7.0f;
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
    DrawViewportTabs(previewWidth, workHeight, timeSeconds, fixedPaneFlags);

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
    constexpr float inspectorSplitterHeight = 7.0f;
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

    DrawNodeNetworkTabs(nodePaneHeight, fixedPaneFlags);

    const bool inspectorSplitterReleased = DrawHorizontalSplitter("InspectorLayoutSplitter", &nodePaneHeight, rightColumnHeight, 160.0f, 160.0f);
    if (inspectorLayoutCanFit)
    {
        g_ui.nodePaneHeight = nodePaneHeight;
    }
    if (inspectorSplitterReleased && inspectorLayoutCanFit)
    {
        SaveAppSettingsSilently();
    }

    ImGui::BeginChild("Inspector", ImVec2(0.0f, 0.0f), false);
    PushTabHeaderStyle(defaultTabStyle);
    if (ImGui::BeginTabBar("InspectorTabs"))
    {
        if (BeginStyledTabItem("プロパティ"))
        {
            BeginInspectorTabContent();
            DrawPropertiesPanel();
            EndInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem("設定"))
        {
            BeginInspectorTabContent();
            DrawDisplaySettingsPanel();
            EndInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem("天球"))
        {
            BeginInspectorTabContent();
            DrawSkySettingsPanel();
            EndInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem("カメラ"))
        {
            BeginInspectorTabContent();
            DrawCameraPanel();
            EndInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem("Debug"))
        {
            BeginInspectorTabContent();
            DrawDebugPanel();
            EndInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem("エクスポート"))
        {
            BeginInspectorTabContent();
            DrawAssetExportPanel();
            EndInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        ImGui::EndTabBar();
    }
    PopTabHeaderStyle();
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::BeginChild("Status Bar", ImVec2(0.0f, statusBarHeight), true, fixedPaneFlags);
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    const char* evaluationState = g_evaluationInFlight
        ? (g_evaluationPending ? "計算待ち" : "計算中")
        : (evaluation.dirty ? "Dirty" : "Evaluated");
    const ImVec4 stateColor = g_evaluationInFlight
        ? ImVec4(0.90f, 0.72f, 0.34f, 1.0f)
        : (evaluation.dirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f));
    ImGui::TextColored(stateColor, "%s", evaluationState);
    ImGui::SameLine();
    ImGui::Text("| %s | %s | %s | %s", rock::ToString(evaluation.previewStage).data(), g_lastEvaluationDuration.c_str(), g_projectStatus.c_str(), g_exportStatus.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::PopStyleVar(3);

    ImGui::End();
    ImGui::PopStyleVar();
}

void RenderFrame()
{
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
    ThrowIfFailed(g_swapChain->Present(1, 0), "Present failed");

    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal failed");
    frameContext.fenceValue = fenceValue;
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
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
        if (wParam != SIZE_MINIMIZED)
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
        rock::SetMultiScaleErosionGpuEvaluator(RunMseComputeGrid);
        rock::SetMaskNoiseGpuEvaluator(RunMaskNoiseCompute);
        rock::SetSedimentGpuEvaluator(RunSedimentCompute);
        rock::SetRockGpuEvaluator(RunRockCompute);
        rock::SetMaskFluvialGpuEvaluator(RunMaskFluvialCompute);
        rock::SetSnowGpuEvaluator(RunSnowCompute);
        rock::SetColorizeGpuEvaluator(RunColorizeCompute);

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
            g_projectStatus = "App settings load failed: " + appSettingsError;
        }
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
            while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT)
                {
                    g_running = false;
                }
            }

            if (!g_running)
            {
                break;
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            UpdateScreenColorPick();
            ProcessPendingMseGpuRequests();
            ProcessPendingMaskNoiseGpuRequests();
            ProcessPendingSedimentGpuRequests();
            ProcessPendingRockGpuRequests();
            ProcessPendingMaskFluvialGpuRequests();
            ProcessPendingSnowGpuRequests();
            ProcessPendingColorizeGpuRequests();
            PollAsyncEvaluation();
            DrawUi();
            ImGui::Render();
            RenderFrame();
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
        rock::SetSedimentGpuEvaluator(nullptr);
        rock::SetRockGpuEvaluator(nullptr);
        rock::SetMaskFluvialGpuEvaluator(nullptr);
        rock::SetSnowGpuEvaluator(nullptr);
        rock::SetColorizeGpuEvaluator(nullptr);
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
