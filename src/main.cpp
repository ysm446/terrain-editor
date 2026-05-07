#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
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
constexpr int kSrvDescriptorCount = 64;
constexpr float kFullFrameSensorHeightMm = 24.0f;
constexpr int kMaxSerializedNodeKind = static_cast<int>(rock::NodeKind::MaskBlend);
constexpr int kMaxSerializedPreviewStage = static_cast<int>(rock::PreviewStage::MaskBlend);
constexpr std::array<int, 5> kResolutionPresets = {128, 256, 512, 1024, 2048};

int NearestResolutionPreset(int value)
{
    const auto nearest = std::ranges::min_element(kResolutionPresets, [value](int lhs, int rhs) {
        return std::abs(lhs - value) < std::abs(rhs - value);
    });
    return nearest != kResolutionPresets.end() ? *nearest : 512;
}

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    UINT64 fenceValue = 0;
};

HWND g_hwnd = nullptr;
UINT g_width = 1600;
UINT g_height = 900;
bool g_running = true;

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
    float rightPaneWidth = 0.0f;
    float nodePaneHeight = 0.0f;
};

UiState g_ui;

enum class ViewportDisplayMode
{
    Simple,
    Pbr,
    Sky,
};

struct ViewportState
{
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovDegrees = 45.0f;
    float orbitDistance = 8.0f;
    float zoom = 1.0f;
    ImVec2 pan = ImVec2(0.0f, 0.0f);
};

ViewportState g_viewport;

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
    float padding;
    float padding0;
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
    float atmosphereDensity;
    float aerialPerspectiveStrength;
    float pad0;
    float pad1;
};
static_assert(sizeof(CloudShadowMeshConstants) == 112);

struct GpuMeshPreview
{
    int width = 0;
    int height = 0;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float fovDegrees = 0.0f;
    float orbitDistance = 0.0f;
    float zoom = 0.0f;
    ImVec2 pan = ImVec2(0.0f, 0.0f);
    uint64_t graphVersion = UINT64_MAX;
    bool showSurface = false;
    bool showWireframe = false;
    bool showGrid = false;
    bool maskPreview = false;
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
    ComPtr<ID3D12Resource> depthTarget;
    ComPtr<ID3D12Resource> shadowTarget;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> edgeIndexBuffer;
    ComPtr<ID3D12Resource> gridVertexBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE depthSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu{};
    bool srvAllocated = false;
    bool shadowSrvAllocated = false;
    bool depthSrvAllocated = false;
    UINT vertexCount = 0;
    UINT triIndexCount = 0;
    UINT edgeIndexCount = 0;
    UINT gridVertexCount = 0;
    int gridCellCount = 0;
    float gridCellSizeMeters = 0.0f;
    int skyMode = -1;
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
    float cloudWindDirectionDegrees = 0.0f;
    float cloudWindSpeed = 0.0f;
    int cloudQualitySamples = 0;
    float cloudShadowStrength = 0.0f;
    int cloudShadowResolution = 0;
    int cloudShadowSamples = 0;
    float cloudFieldRadius = 0.0f;
    float cloudFieldFalloff = 0.0f;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
};

ComPtr<ID3D12RootSignature> g_meshPreviewRootSignature;
ComPtr<ID3D12PipelineState> g_meshPreviewSurfacePso;
ComPtr<ID3D12PipelineState> g_meshPreviewWirePso;
ComPtr<ID3D12PipelineState> g_meshPreviewGridPso;
ComPtr<ID3D12PipelineState> g_meshPreviewShadowPso;
ComPtr<ID3D12RootSignature> g_mseComputeRootSignature;
ComPtr<ID3D12PipelineState> g_mseStreamPowerPso;
ComPtr<ID3D12PipelineState> g_mseThermalPso;
ComPtr<ID3D12PipelineState> g_mseDepositionPso;
ComPtr<ID3D12RootSignature> g_maskNoiseComputeRootSignature;
ComPtr<ID3D12PipelineState> g_maskNoisePso;
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
    g_gpuMeshPreview.colorTarget.Reset();
    g_gpuMeshPreview.depthTarget.Reset();
    g_gpuMeshPreview.vertexBuffer.Reset();
    g_gpuMeshPreview.indexBuffer.Reset();
    g_gpuMeshPreview.edgeIndexBuffer.Reset();
    g_gpuMeshPreview.gridVertexBuffer.Reset();
    g_gpuMeshPreview.gridVertexCount = 0;
    g_meshPreviewSurfacePso.Reset();
    g_meshPreviewWirePso.Reset();
    g_meshPreviewGridPso.Reset();
    g_meshPreviewRootSignature.Reset();
    g_mseStreamPowerPso.Reset();
    g_mseThermalPso.Reset();
    g_mseDepositionPso.Reset();
    g_mseComputeRootSignature.Reset();
    g_mseComputeReady = false;
    g_maskNoisePso.Reset();
    g_maskNoiseComputeRootSignature.Reset();
    g_maskNoiseComputeReady = false;
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

void EvaluateGraph();
void ProcessPendingMseGpuRequests();
void ProcessPendingMaskNoiseGpuRequests();
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
            return parent;
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
            {"meshSurface", settings.preview.showSurface},
            {"meshWireframe", settings.preview.showWireframe},
            {"grid", settings.preview.showGrid},
            {"gridCellCount", settings.preview.gridCellCount},
            {"gridCellSizeMeters", settings.preview.gridCellSizeMeters},
            {"previewResolution", settings.preview.resolution},
            {"previewLod", settings.preview.lod},
            {"lightingMode", settings.preview.lightingMode},
            {"sunAzimuthDegrees", settings.preview.sunAzimuthDegrees},
            {"sunElevationDegrees", settings.preview.sunElevationDegrees},
            {"sunIntensity", settings.preview.sunIntensity},
            {"ambientStrength", settings.preview.ambientStrength},
            {"shadowStrength", settings.preview.shadowStrength},
            {"shadowMapResolution", settings.preview.shadowMapResolution},
            {"shadowBias", settings.preview.shadowBias},
            {"pbrAlbedo", {
                settings.preview.pbrAlbedo[0],
                settings.preview.pbrAlbedo[1],
                settings.preview.pbrAlbedo[2],
            }},
            {"gridColor", {
                settings.preview.gridColor[0],
                settings.preview.gridColor[1],
                settings.preview.gridColor[2],
            }},
            {"viewportBackground", {
                settings.preview.viewportBackground[0],
                settings.preview.viewportBackground[1],
                settings.preview.viewportBackground[2],
            }},
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
            {"zoom", g_viewport.zoom},
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
        settings.preview.showSurface = visibilityJson.value("meshSurface", settings.preview.showSurface);
        settings.preview.showWireframe = visibilityJson.value("meshWireframe", settings.preview.showWireframe);
        settings.preview.showGrid = visibilityJson.value("grid", settings.preview.showGrid);
        settings.preview.gridCellCount = std::clamp(visibilityJson.value("gridCellCount", settings.preview.gridCellCount), 1, 200);
        settings.preview.gridCellSizeMeters = std::clamp(visibilityJson.value("gridCellSizeMeters", settings.preview.gridCellSizeMeters), 1.0f, 10000.0f);
        settings.preview.resolution = NearestResolutionPreset(visibilityJson.value("previewResolution", settings.preview.resolution));
        settings.preview.lod = std::clamp(visibilityJson.value("previewLod", settings.preview.lod), 0, 4);
        settings.preview.lightingMode = std::clamp(visibilityJson.value("lightingMode", settings.preview.lightingMode), 0, 1);
        settings.preview.sunAzimuthDegrees = std::clamp(visibilityJson.value("sunAzimuthDegrees", settings.preview.sunAzimuthDegrees), 0.0f, 360.0f);
        settings.preview.sunElevationDegrees = std::clamp(visibilityJson.value("sunElevationDegrees", settings.preview.sunElevationDegrees), 1.0f, 89.0f);
        settings.preview.sunIntensity = std::clamp(visibilityJson.value("sunIntensity", settings.preview.sunIntensity), 0.0f, 5.0f);
        settings.preview.ambientStrength = std::clamp(visibilityJson.value("ambientStrength", settings.preview.ambientStrength), 0.0f, 2.0f);
        settings.preview.shadowStrength = std::clamp(visibilityJson.value("shadowStrength", settings.preview.shadowStrength), 0.0f, 1.0f);
        settings.preview.shadowMapResolution = std::clamp(visibilityJson.value("shadowMapResolution", settings.preview.shadowMapResolution), 512, 4096);
        settings.preview.shadowBias = std::clamp(visibilityJson.value("shadowBias", settings.preview.shadowBias), 0.0f, 0.05f);
        if (visibilityJson.contains("pbrAlbedo") && visibilityJson["pbrAlbedo"].is_array() && visibilityJson["pbrAlbedo"].size() == 3)
        {
            settings.preview.pbrAlbedo[0] = std::clamp(visibilityJson["pbrAlbedo"][0].get<float>(), 0.0f, 1.0f);
            settings.preview.pbrAlbedo[1] = std::clamp(visibilityJson["pbrAlbedo"][1].get<float>(), 0.0f, 1.0f);
            settings.preview.pbrAlbedo[2] = std::clamp(visibilityJson["pbrAlbedo"][2].get<float>(), 0.0f, 1.0f);
        }
        if (visibilityJson.contains("gridColor") && visibilityJson["gridColor"].is_array() && visibilityJson["gridColor"].size() == 3)
        {
            settings.preview.gridColor[0] = std::clamp(visibilityJson["gridColor"][0].get<float>(), 0.0f, 1.0f);
            settings.preview.gridColor[1] = std::clamp(visibilityJson["gridColor"][1].get<float>(), 0.0f, 1.0f);
            settings.preview.gridColor[2] = std::clamp(visibilityJson["gridColor"][2].get<float>(), 0.0f, 1.0f);
        }
        if (visibilityJson.contains("viewportBackground") && visibilityJson["viewportBackground"].is_array() && visibilityJson["viewportBackground"].size() == 3)
        {
            settings.preview.viewportBackground[0] = std::clamp(visibilityJson["viewportBackground"][0].get<float>(), 0.0f, 1.0f);
            settings.preview.viewportBackground[1] = std::clamp(visibilityJson["viewportBackground"][1].get<float>(), 0.0f, 1.0f);
            settings.preview.viewportBackground[2] = std::clamp(visibilityJson["viewportBackground"][2].get<float>(), 0.0f, 1.0f);
        }

        const nlohmann::json layoutJson = root.value("layout", nlohmann::json::object());
        g_ui.rightPaneWidth = std::max(0.0f, layoutJson.value("rightPaneWidth", g_ui.rightPaneWidth));
        g_ui.nodePaneHeight = std::max(0.0f, layoutJson.value("nodePaneHeight", g_ui.nodePaneHeight));

        const nlohmann::json windowJson = root.value("window", nlohmann::json::object());
        g_width = static_cast<UINT>(std::clamp(windowJson.value("width", static_cast<int>(g_width)), 640, 7680));
        g_height = static_cast<UINT>(std::clamp(windowJson.value("height", static_cast<int>(g_height)), 480, 4320));

        g_recentProjectPaths.clear();
        if (root.contains("recentProjects") && root["recentProjects"].is_array())
        {
            for (const nlohmann::json& recentJson : root["recentProjects"])
            {
                if (!recentJson.is_string())
                {
                    continue;
                }
                AddRecentProjectPath(PathFromUtf8(recentJson.get<std::string>()));
            }
        }

        const nlohmann::json viewportJson = root.value("viewport", nlohmann::json::object());
        g_viewport.yaw = viewportJson.value("yaw", g_viewport.yaw);
        g_viewport.pitch = std::clamp(viewportJson.value("pitch", g_viewport.pitch), -1.25f, 1.25f);
        g_viewport.fovDegrees = std::clamp(viewportJson.value("fovDegrees", g_viewport.fovDegrees), 15.0f, 90.0f);
        g_viewport.orbitDistance = std::clamp(viewportJson.value("orbitDistance", g_viewport.orbitDistance), 1.0f, 10000.0f);
        g_viewport.zoom = std::clamp(viewportJson.value("zoom", g_viewport.zoom), 0.05f, 20.0f);
        const std::string savedAppVersion = root.value("appVersion", std::string());
        if ((savedAppVersion.empty() || savedAppVersion.rfind("0.1.", 0) == 0 || savedAppVersion.rfind("0.2.", 0) == 0) &&
            g_viewport.orbitDistance <= 40.0f)
        {
            g_viewport.pitch = 0.72f;
            g_viewport.orbitDistance = 1800.0f;
            g_viewport.zoom = 1.0f;
        }
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
        ed::SetCurrentEditor(g_nodeEditor);
        ed::ClearSelection();
        for (const rock::Node& node : g_graph.Nodes())
        {
            const ImVec2 position = InitialNodePosition(node.kind);
            ed::SetNodePosition(ed::NodeId(node.id), position);
            g_nodePositionCache.push_back({node.id, position});
        }
        ed::NavigateToContent(0.0f);
        ed::SetCurrentEditor(nullptr);
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
        ed::SetCurrentEditor(g_nodeEditor);
    }
    std::vector<ed::NodeId> selectedNodes(g_graph.Nodes().size());
    const int selectedCount = ed::GetSelectedNodes(selectedNodes.data(), static_cast<int>(selectedNodes.size()));
    if (!g_nodeEditorFrameActive)
    {
        ed::SetCurrentEditor(nullptr);
    }

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

bool SaveProjectToFile(const std::filesystem::path& path, std::string* error)
{
    try
    {
        nlohmann::json root;
        root["format"] = "terrain_editor_project";
        root["formatVersion"] = 1;
        root["appVersion"] = TERRAIN_EDITOR_VERSION_STRING;
        root["selectedNodeId"] = g_selectedNodeId;
        root["selectedNodeIds"] = nlohmann::json::array();
        root["previewStage"] = static_cast<int>(g_graph.Preview());
        root["previewPinId"] = g_graph.Evaluation().previewPinId;

        const rock::GraphSettings& graphSettings = g_graph.Settings();
        const rock::PreviewSettings& preview = graphSettings.preview;
        const rock::SkySettings& sky = graphSettings.sky;
        const rock::CloudSettings& clouds = graphSettings.clouds;
        const int displayMode = sky.mode == rock::SkyMode::Atmospheric
            ? 2
            : (preview.lightingMode >= 1 ? 1 : 0);
        root["settings"] = {
            {"display", {
                {"mode", displayMode},
                {"showFps", g_ui.showFps},
            }},
            {"preview", {
                {"lightingMode", preview.lightingMode},
            }},
            {"sky", {
                {"mode", static_cast<int>(sky.mode)},
                {"atmosphereDensity", sky.atmosphereDensity},
                {"mieStrength", sky.mieStrength},
                {"mieEccentricity", sky.mieEccentricity},
                {"aerialPerspectiveStrength", sky.aerialPerspectiveStrength},
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
                {"windDirectionDegrees", clouds.windDirectionDegrees},
                {"windSpeedMetersPerSec", clouds.windSpeedMetersPerSec},
                {"qualitySamples", clouds.qualitySamples},
                {"shadowStrength", clouds.shadowStrength},
                {"shadowResolution", clouds.shadowResolution},
                {"shadowSamples", clouds.shadowSamples},
                {"fieldRadius", clouds.fieldRadius},
                {"fieldFalloff", clouds.fieldFalloff},
            }},
        };

        root["nodeSettings"] = nlohmann::json::object();

        root["viewport"] = {
            {"yaw", g_viewport.yaw},
            {"pitch", g_viewport.pitch},
            {"fovDegrees", g_viewport.fovDegrees},
            {"orbitDistance", g_viewport.orbitDistance},
            {"zoom", g_viewport.zoom},
            {"pan", {g_viewport.pan.x, g_viewport.pan.y}},
        };

        root["nodes"] = nlohmann::json::array();
        for (const rock::Node& node : g_graph.Nodes())
        {
            nlohmann::json nodeJson = {
                {"id", node.id},
                {"kind", static_cast<int>(node.kind)},
                {"title", node.title},
                {"inputs", nlohmann::json::array()},
                {"outputs", nlohmann::json::array()},
                {"heightmap", {
                    {"path", node.heightmap.path},
                    {"scaleMeters", node.heightmap.scaleMeters},
                    {"relativeVerticalScalePercent", node.heightmap.relativeVerticalScalePercent},
                    {"verticalOffsetMeters", node.heightmap.verticalOffsetMeters},
                    {"simulationResolution", node.heightmap.simulationResolution},
                }},
                {"shape", {
                    {"kind", static_cast<int>(node.shape.kind)},
                    {"scaleMeters", node.shape.scaleMeters},
                    {"relativeHeightPercent", node.shape.relativeHeightPercent},
                    {"simulationResolution", node.shape.simulationResolution},
                }},
                {"heightmapBlur", {
                    {"radius", node.heightmapBlur.radius},
                    {"strength", node.heightmapBlur.strength},
                    {"iterations", node.heightmapBlur.iterations},
                }},
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
                {"erosionNoise", {
                    {"frequency", node.erosionNoise.frequency},
                    {"octaves", node.erosionNoise.octaves},
                    {"erosionStrength", node.erosionNoise.erosionStrength},
                    {"directionInfluence", node.erosionNoise.directionInfluence},
                    {"valleyLow", node.erosionNoise.valleyLow},
                    {"valleyHigh", node.erosionNoise.valleyHigh},
                    {"seed", node.erosionNoise.seed},
                }},
                {"maskNoise", {
                    {"seed", node.maskNoise.seed},
                    {"octaves", node.maskNoise.octaves},
                    {"frequency", node.maskNoise.frequency},
                    {"lacunarity", node.maskNoise.lacunarity},
                    {"persistence", node.maskNoise.persistence},
                    {"simulationResolution", node.maskNoise.simulationResolution},
                    {"backend", static_cast<int>(node.maskNoise.backend)},
                }},
                {"maskBlend", {
                    {"mode", static_cast<int>(node.maskBlend.mode)},
                    {"intensity", node.maskBlend.intensity},
                }},
            };
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
            root["nodes"].push_back(std::move(nodeJson));
        }

        root["links"] = nlohmann::json::array();
        for (const rock::Link& link : g_graph.Links())
        {
            root["links"].push_back({
                {"id", link.id},
                {"startPin", link.startPin},
                {"endPin", link.endPin},
            });
        }

        root["nodePositions"] = nlohmann::json::object();
        if (g_nodeEditor != nullptr)
        {
            ed::SetCurrentEditor(g_nodeEditor);
            std::vector<ed::NodeId> selectedNodes(g_graph.Nodes().size());
            const int selectedCount = ed::GetSelectedNodes(selectedNodes.data(), static_cast<int>(selectedNodes.size()));
            g_selectedNodeId = selectedCount > 0 ? static_cast<rock::GraphId>(selectedNodes.front().Get()) : 0;
            root["selectedNodeId"] = g_selectedNodeId;
            for (int i = 0; i < selectedCount; ++i)
            {
                root["selectedNodeIds"].push_back(static_cast<rock::GraphId>(selectedNodes[static_cast<size_t>(i)].Get()));
            }
            ed::SetCurrentEditor(nullptr);
        }
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
                ed::SetCurrentEditor(g_nodeEditor);
                position = ed::GetNodePosition(ed::NodeId(node.id));
                ed::SetCurrentEditor(nullptr);
            }
            root["nodePositions"][std::to_string(node.id)] = {position.x, position.y};
        }

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

        const nlohmann::json settingsJson = root.value("settings", nlohmann::json::object());
        rock::GraphSettings& graphSettings = g_graph.Settings();
        rock::PreviewSettings& preview = graphSettings.preview;
        const nlohmann::json skyJson = settingsJson.value("sky", nlohmann::json::object());
        rock::SkySettings& sky = graphSettings.sky;
        sky = rock::SkySettings{};
        if (!skyJson.empty())
        {
            const int skyModeInt = std::clamp(skyJson.value("mode", static_cast<int>(sky.mode)),
                                              static_cast<int>(rock::SkyMode::SolidColor),
                                              static_cast<int>(rock::SkyMode::Atmospheric));
            sky.mode = static_cast<rock::SkyMode>(skyModeInt);
            const auto readColor = [&](const char* key, std::array<float, 3>& target) {
                if (skyJson.contains(key) && skyJson[key].is_array() && skyJson[key].size() == 3)
                {
                    target[0] = std::clamp(skyJson[key][0].get<float>(), 0.0f, 8.0f);
                    target[1] = std::clamp(skyJson[key][1].get<float>(), 0.0f, 8.0f);
                    target[2] = std::clamp(skyJson[key][2].get<float>(), 0.0f, 8.0f);
                }
            };
            sky.atmosphereDensity = std::clamp(skyJson.value("atmosphereDensity", sky.atmosphereDensity), 0.05f, 8.0f);
            sky.mieStrength = std::clamp(skyJson.value("mieStrength", sky.mieStrength), 0.0f, 8.0f);
            sky.mieEccentricity = std::clamp(skyJson.value("mieEccentricity", sky.mieEccentricity), -0.99f, 0.99f);
            sky.aerialPerspectiveStrength = std::clamp(skyJson.value("aerialPerspectiveStrength", sky.aerialPerspectiveStrength), 0.0f, 8.0f);
            readColor("groundAlbedo", sky.groundAlbedo);
            sky.sunSizeDegrees = std::clamp(skyJson.value("sunSizeDegrees", sky.sunSizeDegrees), 0.1f, 30.0f);
            sky.sunGlowStrength = std::clamp(skyJson.value("sunGlowStrength", sky.sunGlowStrength), 0.0f, 4.0f);
        }

        const nlohmann::json cloudsJson = settingsJson.value("clouds", nlohmann::json::object());
        rock::CloudSettings& clouds = graphSettings.clouds;
        clouds = rock::CloudSettings{};
        if (!cloudsJson.empty())
        {
            clouds.enabled = cloudsJson.value("enabled", clouds.enabled);
            clouds.seed = std::clamp(cloudsJson.value("seed", clouds.seed), 0, 999999);
            clouds.coverage = std::clamp(cloudsJson.value("coverage", clouds.coverage), 0.0f, 1.0f);
            clouds.densityMultiplier = std::clamp(cloudsJson.value("densityMultiplier", clouds.densityMultiplier), 0.0f, 8.0f);
            clouds.altitudeMin = std::clamp(cloudsJson.value("altitudeMin", clouds.altitudeMin), 0.0f, 30000.0f);
            clouds.altitudeMax = std::clamp(cloudsJson.value("altitudeMax", clouds.altitudeMax), 0.0f, 30000.0f);
            clouds.horizontalScale = std::clamp(cloudsJson.value("horizontalScale", clouds.horizontalScale), 50.0f, 100000.0f);
            clouds.absorption = std::clamp(cloudsJson.value("absorption", clouds.absorption), 0.0f, 2.0f);
            if (cloudsJson.contains("color") && cloudsJson["color"].is_array() && cloudsJson["color"].size() == 3)
            {
                clouds.color[0] = std::clamp(cloudsJson["color"][0].get<float>(), 0.0f, 8.0f);
                clouds.color[1] = std::clamp(cloudsJson["color"][1].get<float>(), 0.0f, 8.0f);
                clouds.color[2] = std::clamp(cloudsJson["color"][2].get<float>(), 0.0f, 8.0f);
            }
            clouds.windDirectionDegrees = std::clamp(cloudsJson.value("windDirectionDegrees", clouds.windDirectionDegrees), 0.0f, 360.0f);
            clouds.windSpeedMetersPerSec = std::clamp(cloudsJson.value("windSpeedMetersPerSec", clouds.windSpeedMetersPerSec), 0.0f, 500.0f);
            clouds.qualitySamples = std::clamp(cloudsJson.value("qualitySamples", clouds.qualitySamples), 8, 128);
            clouds.shadowStrength = std::clamp(cloudsJson.value("shadowStrength", clouds.shadowStrength), 0.0f, 1.0f);
            clouds.shadowResolution = std::clamp(cloudsJson.value("shadowResolution", clouds.shadowResolution), 256, 4096);
            clouds.shadowSamples = std::clamp(cloudsJson.value("shadowSamples", clouds.shadowSamples), 4, 64);
            clouds.fieldRadius = std::clamp(cloudsJson.value("fieldRadius", clouds.fieldRadius), 100.0f, 200000.0f);
            clouds.fieldFalloff = std::clamp(cloudsJson.value("fieldFalloff", clouds.fieldFalloff), 1.0f, 50000.0f);
        }
        const nlohmann::json previewJson = settingsJson.value("preview", nlohmann::json::object());
        if (!previewJson.empty())
        {
            preview.lightingMode = std::clamp(previewJson.value("lightingMode", preview.lightingMode), 0, 1);
        }
        else if (sky.mode == rock::SkyMode::Atmospheric)
        {
            preview.lightingMode = 1;
        }
        const nlohmann::json displayJson = settingsJson.value("display", nlohmann::json::object());
        if (!displayJson.empty())
        {
            g_ui.showFps = displayJson.value("showFps", g_ui.showFps);
            const int displayMode = std::clamp(displayJson.value("mode", -1), -1, 2);
            if (displayMode == 0)
            {
                preview.lightingMode = 0;
                sky.mode = rock::SkyMode::SolidColor;
                clouds.enabled = false;
            }
            else if (displayMode == 1)
            {
                preview.lightingMode = 1;
                sky.mode = rock::SkyMode::SolidColor;
                clouds.enabled = false;
            }
            else if (displayMode == 2)
            {
                preview.lightingMode = 1;
                sky.mode = rock::SkyMode::Atmospheric;
            }
        }

        const nlohmann::json nodesJson = root.value("nodes", nlohmann::json::array());
        if (nodesJson.is_array() && !nodesJson.empty())
        {
            std::vector<rock::Node> nodes;
            for (const nlohmann::json& nodeJson : nodesJson)
            {
                rock::Node node;
                node.id = nodeJson.value("id", 0);
                node.kind = static_cast<rock::NodeKind>(std::clamp(nodeJson.value("kind", 0), 0, kMaxSerializedNodeKind));
                if (!IsTerrainNodeKind(node.kind))
                {
                    continue;
                }
                node.title = nodeJson.value("title", std::string(rock::ToString(node.kind)));
                if (node.kind == rock::NodeKind::HeightmapLoad && node.title == "Load Heightmap")
                {
                    node.title = std::string(rock::ToString(node.kind));
                }
                const nlohmann::json nodeHeightmapJson = nodeJson.value("heightmap", nlohmann::json::object());
                const nlohmann::json nodeShapeJson = nodeJson.value("shape", nlohmann::json::object());
                const nlohmann::json nodeBlurJson = nodeJson.value("heightmapBlur", nlohmann::json::object());
                const nlohmann::json nodeErosionNoiseJson = nodeJson.value("erosionNoise", nlohmann::json::object());
                const nlohmann::json nodeMultiScaleErosionJson = nodeJson.value("multiScaleErosion", nlohmann::json::object());
                const nlohmann::json nodeMaskNoiseJson = nodeJson.value("maskNoise", nlohmann::json::object());
                const nlohmann::json nodeMaskBlendJson = nodeJson.value("maskBlend", nlohmann::json::object());
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
                node.erosionNoise.frequency = std::clamp(nodeErosionNoiseJson.value("frequency", node.erosionNoise.frequency), 0.0f, 256.0f);
                node.erosionNoise.octaves = std::clamp(nodeErosionNoiseJson.value("octaves", node.erosionNoise.octaves), 0, 8);
                node.erosionNoise.erosionStrength = std::clamp(nodeErosionNoiseJson.value("erosionStrength", node.erosionNoise.erosionStrength), 0.0f, 1.0f);
                node.erosionNoise.directionInfluence = std::clamp(nodeErosionNoiseJson.value("directionInfluence", node.erosionNoise.directionInfluence), 0.0f, 8.0f);
                node.erosionNoise.valleyLow = std::clamp(nodeErosionNoiseJson.value("valleyLow", node.erosionNoise.valleyLow), 0.0f, 1.0f);
                node.erosionNoise.valleyHigh = std::clamp(nodeErosionNoiseJson.value("valleyHigh", node.erosionNoise.valleyHigh), 0.0f, 1.0f);
                node.erosionNoise.seed = std::clamp(nodeErosionNoiseJson.value("seed", node.erosionNoise.seed), 0, 999999);
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

                const auto readPins = [&](const nlohmann::json& pinsJson, rock::PinKind pinKind, std::vector<rock::Pin>& pins) {
                    if (!pinsJson.is_array())
                    {
                        return;
                    }
                    for (const nlohmann::json& pinJson : pinsJson)
                    {
                        rock::Pin pin;
                        pin.id = pinJson.value("id", 0);
                        pin.nodeId = node.id;
                        pin.kind = pinKind;
                        const int serializedValueType = std::clamp(pinJson.value("valueType", static_cast<int>(rock::ValueType::HeightField)), 0, 3);
                        pin.valueType = serializedValueType == static_cast<int>(rock::ValueType::Mask)
                            ? rock::ValueType::Mask
                            : rock::ValueType::HeightField;
                        pin.label = pinJson.value("label", std::string(rock::ToString(pin.valueType)));
                        if (pin.valueType == rock::ValueType::HeightField && pin.kind == rock::PinKind::Output)
                        {
                            pin.label = "Heightmap";
                        }
                        pins.push_back(std::move(pin));
                    }
                };
                readPins(nodeJson.value("inputs", nlohmann::json::array()), rock::PinKind::Input, node.inputs);
                readPins(nodeJson.value("outputs", nlohmann::json::array()), rock::PinKind::Output, node.outputs);
                if (node.id != 0)
                {
                    nodes.push_back(std::move(node));
                }
            }
            if (!nodes.empty())
            {
                g_graph.ReplaceNodes(std::move(nodes));
            }
        }
        const nlohmann::json viewportJson = root.value("viewport", nlohmann::json::object());
        g_viewport.yaw = viewportJson.value("yaw", g_viewport.yaw);
        g_viewport.pitch = viewportJson.value("pitch", g_viewport.pitch);
        g_viewport.fovDegrees = viewportJson.value("fovDegrees", g_viewport.fovDegrees);
        g_viewport.orbitDistance = viewportJson.value("orbitDistance", g_viewport.orbitDistance);
        g_viewport.zoom = viewportJson.value("zoom", g_viewport.zoom);
        if (viewportJson.contains("pan") && viewportJson["pan"].is_array() && viewportJson["pan"].size() == 2)
        {
            g_viewport.pan = ImVec2(viewportJson["pan"][0].get<float>(), viewportJson["pan"][1].get<float>());
        }

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
        const int serializedPreviewStage = std::clamp(root.value("previewStage", static_cast<int>(g_graph.Preview())), 0, kMaxSerializedPreviewStage);
        const rock::PreviewStage previewStage = serializedPreviewStage >= static_cast<int>(rock::PreviewStage::Graph)
            ? static_cast<rock::PreviewStage>(serializedPreviewStage)
            : rock::PreviewStage::Graph;
        g_graph.SetPreviewStage(previewStage);
        const rock::GraphId previewPinId = root.value("previewPinId", 0);
        if (previewPinId != 0 && g_graph.FindPin(previewPinId) != nullptr)
        {
            g_graph.SetPreviewPin(previewPinId);
        }

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

    D3D12_DESCRIPTOR_RANGE shadowRange{};
    shadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shadowRange.NumDescriptors = 1;
    shadowRange.BaseShaderRegister = 0;
    shadowRange.RegisterSpace = 0;
    shadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE cloudShadowRange{};
    cloudShadowRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cloudShadowRange.NumDescriptors = 1;
    cloudShadowRange.BaseShaderRegister = 1;
    cloudShadowRange.RegisterSpace = 0;
    cloudShadowRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // Root parameter budget: 60 (mesh constants) + 2 (cloud shadow CBV)
    // + 1 (shadow table) + 1 (cloud shadow table) = 64 DWORDs (the limit).
    D3D12_ROOT_PARAMETER rootParams[4]{};
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
    rootParams[2].DescriptorTable.pDescriptorRanges = &shadowRange;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &cloudShadowRange;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    rsDesc.NumParameters = 4;
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
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_meshPreviewRootSignature.Get();
    psoDesc.VS = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.InputLayout = {inputLayout, 3};
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
void RenderSkyPass(ID3D12GraphicsCommandList* commandList, const rock::SkySettings& sky, const SkyShaderConstants& base)
{
    if (sky.mode != rock::SkyMode::Atmospheric)
    {
        return;
    }
    std::string ignoredError;
    if (!EnsureSkyPipeline(&ignoredError))
    {
        return;
    }

    const float density = std::clamp(sky.atmosphereDensity, 0.05f, 8.0f);
    const float mieS = std::clamp(sky.mieStrength, 0.0f, 8.0f);
    const float mieG = std::clamp(sky.mieEccentricity, -0.99f, 0.99f);
    std::string lutError;
    const bool lutReady = EnsureAtmosphereMultiScatterLut(density, mieS, mieG, &lutError);
    if (!lutReady || !g_atmosphereMultiScatterSrvAllocated)
    {
        return;
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
};
static_assert(sizeof(CloudRenderShaderConstants) == 52 * sizeof(UINT), "CloudRenderShaderConstants must be 52 DWORDs");

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
        rootParams[0].Constants.Num32BitValues = 52;
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
void RenderCloudPass(ID3D12GraphicsCommandList* commandList,
                    const rock::CloudSettings& clouds,
                    const CloudRenderShaderConstants& base,
                    float windOffsetX,
                    float windOffsetZ,
                    float fieldCenterX,
                    float fieldCenterZ,
                    D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu)
{
    if (!clouds.enabled) return;
    if (!g_gpuClouds.volumeReady || !g_gpuClouds.volumeTexture) return;
    if (!g_cloudPipelinesReady) return;

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

    commandList->SetGraphicsRootSignature(g_cloudRenderRootSignature.Get());
    commandList->SetPipelineState(g_cloudRenderPso.Get());
    commandList->SetGraphicsRoot32BitConstants(0, sizeof(c) / 4, &c, 0);
    commandList->SetGraphicsRootDescriptorTable(1, g_gpuClouds.volumeSrvGpu);
    commandList->SetGraphicsRootDescriptorTable(2, depthSrvGpu);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->DrawInstanced(3, 1, 0, 0);
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
        kind == rock::NodeKind::ErosionNoise ||
        kind == rock::NodeKind::MultiScaleErosion ||
        kind == rock::NodeKind::MaskNoise ||
        kind == rock::NodeKind::MaskBlend;
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

void ResetViewport()
{
    g_viewport = {};
    g_viewport.yaw = 0.0f;
    g_viewport.pitch = 0.72f;
    g_viewport.fovDegrees = 45.0f;
    g_viewport.orbitDistance = 1800.0f;
    g_viewport.zoom = 1.0f;
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
        g_viewport.orbitDistance = std::clamp(g_viewport.orbitDistance, 1.0f, 10000.0f);
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
    const float distance = std::clamp(g_viewport.orbitDistance, 1.0f, 10000.0f);
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
        g_gpuMeshPreview.depthTarget.Reset();
        g_gpuMeshPreview.shadowTarget.Reset();
        g_gpuMeshPreview.width = width;
        g_gpuMeshPreview.height = height;
        g_gpuMeshPreview.shadowMapResolution = shadowResolution;
        g_gpuMeshPreview.colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_gpuMeshPreview.shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        if (!g_meshPreviewRtvHeap)
        {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = 1;
            ThrowIfFailed(g_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_meshPreviewRtvHeap)), "Create mesh RTV heap failed");
            g_gpuMeshPreview.rtvCpu = g_meshPreviewRtvHeap->GetCPUDescriptorHandleForHeapStart();
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
    const float scale = viewportSize * 1.20f * g_viewport.zoom;

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
    const float scale = viewportSize * 1.20f * g_viewport.zoom;

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
    if (!showSurface && !showWireframe && !showGrid) return true;
    if (!EnsureMeshPreviewPipeline(error)) return false;

    const float viewportWidth = std::max(1.0f, max.x - min.x);
    const float viewportHeight = std::max(1.0f, max.y - min.y);
    const int targetWidth = std::clamp(static_cast<int>(viewportWidth), 160, 960);
    const int targetHeight = std::clamp(static_cast<int>(viewportHeight), 120, 720);
    if (!EnsureMeshPreviewRenderTarget(targetWidth, targetHeight, error)) return false;

    const rock::MeshData& mesh = g_graph.Evaluation().previewMesh;
    const uint64_t currentVersion = g_graph.Evaluation().version;
    const bool meshHasVertices = !mesh.vertices.empty();
    const bool meshDirty = (g_gpuMeshPreview.graphVersion != currentVersion || (meshHasVertices && !g_gpuMeshPreview.vertexBuffer));
    const bool viewportDirty =
        g_gpuMeshPreview.yaw != g_viewport.yaw ||
        g_gpuMeshPreview.pitch != g_viewport.pitch ||
        g_gpuMeshPreview.fovDegrees != g_viewport.fovDegrees ||
        g_gpuMeshPreview.orbitDistance != g_viewport.orbitDistance ||
        g_gpuMeshPreview.zoom != g_viewport.zoom ||
        g_gpuMeshPreview.pan.x != g_viewport.pan.x ||
        g_gpuMeshPreview.pan.y != g_viewport.pan.y ||
        g_gpuMeshPreview.showSurface != showSurface ||
        g_gpuMeshPreview.showWireframe != showWireframe ||
        g_gpuMeshPreview.showGrid != showGrid ||
        g_gpuMeshPreview.maskPreview != g_graph.Evaluation().previewShowsMask ||
        g_gpuMeshPreview.lightingMode != g_graph.Settings().preview.lightingMode ||
        g_gpuMeshPreview.sunAzimuthDegrees != g_graph.Settings().preview.sunAzimuthDegrees ||
        g_gpuMeshPreview.sunElevationDegrees != g_graph.Settings().preview.sunElevationDegrees ||
        g_gpuMeshPreview.sunIntensity != g_graph.Settings().preview.sunIntensity ||
        g_gpuMeshPreview.ambientStrength != g_graph.Settings().preview.ambientStrength ||
        g_gpuMeshPreview.shadowStrength != g_graph.Settings().preview.shadowStrength ||
        g_gpuMeshPreview.shadowBias != g_graph.Settings().preview.shadowBias ||
        g_gpuMeshPreview.pbrAlbedo != g_graph.Settings().preview.pbrAlbedo ||
        g_gpuMeshPreview.gridColor != g_graph.Settings().preview.gridColor ||
        g_gpuMeshPreview.gridCellCount != std::clamp(g_graph.Settings().preview.gridCellCount, 1, 200) ||
        g_gpuMeshPreview.gridCellSizeMeters != std::clamp(g_graph.Settings().preview.gridCellSizeMeters, 1.0f, 10000.0f) ||
        g_gpuMeshPreview.skyMode != static_cast<int>(g_graph.Settings().sky.mode) ||
        g_gpuMeshPreview.skyMieStrength != g_graph.Settings().sky.mieStrength ||
        g_gpuMeshPreview.skyMieEccentricity != g_graph.Settings().sky.mieEccentricity ||
        g_gpuMeshPreview.skyGroundAlbedo != g_graph.Settings().sky.groundAlbedo ||
        g_gpuMeshPreview.skySunSizeDegrees != g_graph.Settings().sky.sunSizeDegrees ||
        g_gpuMeshPreview.skySunGlowStrength != g_graph.Settings().sky.sunGlowStrength ||
        g_gpuMeshPreview.cloudsEnabled != (g_graph.Settings().clouds.enabled ? 1 : 0) ||
        g_gpuMeshPreview.cloudSeed != g_graph.Settings().clouds.seed ||
        g_gpuMeshPreview.cloudCoverage != g_graph.Settings().clouds.coverage ||
        g_gpuMeshPreview.cloudDensityMultiplier != g_graph.Settings().clouds.densityMultiplier ||
        g_gpuMeshPreview.cloudAltitudeMin != g_graph.Settings().clouds.altitudeMin ||
        g_gpuMeshPreview.cloudAltitudeMax != g_graph.Settings().clouds.altitudeMax ||
        g_gpuMeshPreview.cloudHorizontalScale != g_graph.Settings().clouds.horizontalScale ||
        g_gpuMeshPreview.cloudAbsorption != g_graph.Settings().clouds.absorption ||
        g_gpuMeshPreview.cloudColor != g_graph.Settings().clouds.color ||
        g_gpuMeshPreview.cloudWindDirectionDegrees != g_graph.Settings().clouds.windDirectionDegrees ||
        g_gpuMeshPreview.cloudWindSpeed != g_graph.Settings().clouds.windSpeedMetersPerSec ||
        g_gpuMeshPreview.cloudQualitySamples != g_graph.Settings().clouds.qualitySamples ||
        g_gpuMeshPreview.cloudShadowStrength != g_graph.Settings().clouds.shadowStrength ||
        g_gpuMeshPreview.cloudShadowResolution != g_graph.Settings().clouds.shadowResolution ||
        g_gpuMeshPreview.cloudShadowSamples != g_graph.Settings().clouds.shadowSamples ||
        g_gpuMeshPreview.cloudFieldRadius != g_graph.Settings().clouds.fieldRadius ||
        g_gpuMeshPreview.cloudFieldFalloff != g_graph.Settings().clouds.fieldFalloff ||
        (g_graph.Settings().clouds.enabled && g_graph.Settings().clouds.windSpeedMetersPerSec > 0.0f) ||
        (showGrid && !g_gpuMeshPreview.gridVertexBuffer) ||
        g_gpuMeshPreview.colorState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!meshDirty && !viewportDirty) return true;

    try
    {
        if (meshDirty)
        {
            UpdateMeshPreviewBuffers(mesh);
            g_gpuMeshPreview.graphVersion = currentVersion;
        }
        if (showGrid)
        {
            EnsureGridPreviewBuffer();
        }
        const bool hasMeshVertices = g_gpuMeshPreview.vertexCount > 0 && g_gpuMeshPreview.vertexBuffer;
        if (!hasMeshVertices && (!showGrid || g_gpuMeshPreview.gridVertexCount == 0))
        {
            return false;
        }

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Mesh preview allocator failed");
        ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Mesh preview CL failed");

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

        const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
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
        const float scale = viewportSize * 1.20f * g_viewport.zoom;

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
        constants.farPlane   = 20000.0f;
        constants.maskPreview = g_graph.Evaluation().previewShowsMask ? 1.0f : 0.0f;
        constants.lightingMode = static_cast<float>(g_graph.Settings().preview.lightingMode);
        const float azimuth = g_graph.Settings().preview.sunAzimuthDegrees * 3.1415926535f / 180.0f;
        const float elevation = g_graph.Settings().preview.sunElevationDegrees * 3.1415926535f / 180.0f;
        const float cosElevation = std::cos(elevation);
        constants.sunDirection[0] = std::sin(azimuth) * cosElevation;
        constants.sunDirection[1] = std::sin(elevation);
        constants.sunDirection[2] = std::cos(azimuth) * cosElevation;
        constants.sunDirection[3] = 0.0f;
        constants.albedoColor[0] = g_graph.Settings().preview.pbrAlbedo[0];
        constants.albedoColor[1] = g_graph.Settings().preview.pbrAlbedo[1];
        constants.albedoColor[2] = g_graph.Settings().preview.pbrAlbedo[2];
        constants.albedoColor[3] = 1.0f;
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

        if (hasMeshVertices && constants.shadowEnabled > 0.5f && showSurface && g_gpuMeshPreview.triIndexCount > 0)
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
            D3D12_INDEX_BUFFER_VIEW shadowIbv{g_gpuMeshPreview.indexBuffer->GetGPUVirtualAddress(), g_gpuMeshPreview.triIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&shadowIbv);
            commandList->SetPipelineState(g_meshPreviewShadowPso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(g_gpuMeshPreview.triIndexCount, 1, 0, 0, 0);

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
        RenderSkyPass(commandList.Get(), g_graph.Settings().sky, skyBase);

        // Cloud shadow texture: regenerated each frame from the same cloud
        // volume the cloud render pass uses. Before the mesh draw so the
        // surface PS can sample it. CloudShadowMeshConstants in the upload
        // CB tells the shader where the texture lives in world XZ.
        const rock::CloudSettings& cloudSettingsForShadow = g_graph.Settings().clouds;
        const float windRad = cloudSettingsForShadow.windDirectionDegrees * 3.14159265358979323846f / 180.0f;
        const float seconds = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        const float windOffsetX = std::cos(windRad) * cloudSettingsForShadow.windSpeedMetersPerSec * seconds;
        const float windOffsetZ = std::sin(windRad) * cloudSettingsForShadow.windSpeedMetersPerSec * seconds;

        // Expand the shadow footprint a bit beyond the mesh so projected
        // shadows don't get clamped to the mesh edges.
        const float shadowMargin = std::max(boundsDiagonal * 0.4f, 1024.0f);
        const float shadowMinX = boundsMin.x - shadowMargin;
        const float shadowMinZ = boundsMin.z - shadowMargin;
        const float shadowSizeX = (boundsMax.x - boundsMin.x) + shadowMargin * 2.0f;
        const float shadowSizeZ = (boundsMax.z - boundsMin.z) + shadowMargin * 2.0f;

        bool cloudShadowReady = false;
        if (cloudSettingsForShadow.enabled && cloudSettingsForShadow.shadowStrength > 0.001f)
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
        cloudShadowCb.atmosphereDensity = std::clamp(sky.atmosphereDensity, 0.05f, 8.0f);
        cloudShadowCb.aerialPerspectiveStrength =
            (sky.mode == rock::SkyMode::Atmospheric) ? std::clamp(sky.aerialPerspectiveStrength, 0.0f, 8.0f) : 0.0f;
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
        commandList->SetGraphicsRootDescriptorTable(2, g_gpuMeshPreview.shadowSrvGpu);
        D3D12_GPU_DESCRIPTOR_HANDLE cloudShadowGpu = cloudShadowReady && g_gpuClouds.shadowSrvAllocated
            ? g_gpuClouds.shadowSrvGpu
            : g_gpuClouds.dummyShadowSrvGpu;
        commandList->SetGraphicsRootDescriptorTable(3, cloudShadowGpu);

        if (showSurface && g_gpuMeshPreview.triIndexCount > 0)
        {
            D3D12_INDEX_BUFFER_VIEW ibv{g_gpuMeshPreview.indexBuffer->GetGPUVirtualAddress(), g_gpuMeshPreview.triIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            commandList->SetPipelineState(g_meshPreviewSurfacePso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(g_gpuMeshPreview.triIndexCount, 1, 0, 0, 0);
        }
        if (showGrid && g_gpuMeshPreview.gridVertexCount > 0)
        {
            constants.albedoColor[0] = g_graph.Settings().preview.gridColor[0];
            constants.albedoColor[1] = g_graph.Settings().preview.gridColor[1];
            constants.albedoColor[2] = g_graph.Settings().preview.gridColor[2];
            constants.albedoColor[3] = 1.0f;
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            D3D12_VERTEX_BUFFER_VIEW gridVbv{};
            gridVbv.BufferLocation = g_gpuMeshPreview.gridVertexBuffer->GetGPUVirtualAddress();
            gridVbv.SizeInBytes = g_gpuMeshPreview.gridVertexCount * static_cast<UINT>(sizeof(rock::MeshVertex));
            gridVbv.StrideInBytes = static_cast<UINT>(sizeof(rock::MeshVertex));
            commandList->IASetVertexBuffers(0, 1, &gridVbv);
            commandList->SetPipelineState(g_meshPreviewGridPso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            commandList->DrawInstanced(g_gpuMeshPreview.gridVertexCount, 1, 0, 0);
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
            commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);
            D3D12_INDEX_BUFFER_VIEW ibv{g_gpuMeshPreview.edgeIndexBuffer->GetGPUVirtualAddress(), g_gpuMeshPreview.edgeIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            commandList->SetPipelineState(g_meshPreviewWirePso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            commandList->DrawIndexedInstanced(g_gpuMeshPreview.edgeIndexCount, 1, 0, 0, 0);
        }

        // Cloud pass: now that terrain has written depth, transition depth to
        // SRV and ray-march cloud over the existing color. Each pixel reads
        // depth to clamp tExit so cloud renders correctly in front of distant
        // terrain and is occluded by closer terrain. Alpha-blended over the
        // already-rendered scene with SRC_ALPHA / INV_SRC_ALPHA.
        const rock::CloudSettings& cloudSettings = g_graph.Settings().clouds;
        if (cloudSettings.enabled)
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
                RenderCloudPass(commandList.Get(), cloudSettings, cloudBase,
                                windOffsetX, windOffsetZ,
                                boundsCenter.x, boundsCenter.z,
                                g_gpuMeshPreview.depthSrvGpu);
            }
        }

        D3D12_RESOURCE_BARRIER toSrv{};
        toSrv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrv.Transition.pResource = g_gpuMeshPreview.colorTarget.Get();
        toSrv.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        toSrv.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toSrv.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        commandList->ResourceBarrier(1, &toSrv);

        ThrowIfFailed(commandList->Close(), "Close mesh preview CL failed");
        ID3D12CommandList* cls[] = {commandList.Get()};
        g_commandQueue->ExecuteCommandLists(1, cls);
        const UINT64 fenceVal = ++g_fenceLastSignaledValue;
        ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceVal), "Signal mesh preview failed");
        WaitForFenceValue(fenceVal);

        g_gpuMeshPreview.yaw           = g_viewport.yaw;
        g_gpuMeshPreview.pitch         = g_viewport.pitch;
        g_gpuMeshPreview.fovDegrees    = g_viewport.fovDegrees;
        g_gpuMeshPreview.orbitDistance = g_viewport.orbitDistance;
        g_gpuMeshPreview.zoom          = g_viewport.zoom;
        g_gpuMeshPreview.pan           = g_viewport.pan;
        g_gpuMeshPreview.showSurface   = showSurface;
        g_gpuMeshPreview.showWireframe = showWireframe;
        g_gpuMeshPreview.showGrid      = showGrid;
        g_gpuMeshPreview.maskPreview   = g_graph.Evaluation().previewShowsMask;
        g_gpuMeshPreview.lightingMode  = g_graph.Settings().preview.lightingMode;
        g_gpuMeshPreview.sunAzimuthDegrees = g_graph.Settings().preview.sunAzimuthDegrees;
        g_gpuMeshPreview.sunElevationDegrees = g_graph.Settings().preview.sunElevationDegrees;
        g_gpuMeshPreview.sunIntensity = g_graph.Settings().preview.sunIntensity;
        g_gpuMeshPreview.ambientStrength = g_graph.Settings().preview.ambientStrength;
        g_gpuMeshPreview.shadowStrength = g_graph.Settings().preview.shadowStrength;
        g_gpuMeshPreview.shadowBias = g_graph.Settings().preview.shadowBias;
        g_gpuMeshPreview.pbrAlbedo = g_graph.Settings().preview.pbrAlbedo;
        g_gpuMeshPreview.gridColor = g_graph.Settings().preview.gridColor;
        g_gpuMeshPreview.skyMode = static_cast<int>(g_graph.Settings().sky.mode);
        g_gpuMeshPreview.skyMieStrength = g_graph.Settings().sky.mieStrength;
        g_gpuMeshPreview.skyMieEccentricity = g_graph.Settings().sky.mieEccentricity;
        g_gpuMeshPreview.skyGroundAlbedo = g_graph.Settings().sky.groundAlbedo;
        g_gpuMeshPreview.skySunSizeDegrees = g_graph.Settings().sky.sunSizeDegrees;
        g_gpuMeshPreview.skySunGlowStrength = g_graph.Settings().sky.sunGlowStrength;
        g_gpuMeshPreview.cloudsEnabled = g_graph.Settings().clouds.enabled ? 1 : 0;
        g_gpuMeshPreview.cloudSeed = g_graph.Settings().clouds.seed;
        g_gpuMeshPreview.cloudCoverage = g_graph.Settings().clouds.coverage;
        g_gpuMeshPreview.cloudDensityMultiplier = g_graph.Settings().clouds.densityMultiplier;
        g_gpuMeshPreview.cloudAltitudeMin = g_graph.Settings().clouds.altitudeMin;
        g_gpuMeshPreview.cloudAltitudeMax = g_graph.Settings().clouds.altitudeMax;
        g_gpuMeshPreview.cloudHorizontalScale = g_graph.Settings().clouds.horizontalScale;
        g_gpuMeshPreview.cloudAbsorption = g_graph.Settings().clouds.absorption;
        g_gpuMeshPreview.cloudColor = g_graph.Settings().clouds.color;
        g_gpuMeshPreview.cloudWindDirectionDegrees = g_graph.Settings().clouds.windDirectionDegrees;
        g_gpuMeshPreview.cloudWindSpeed = g_graph.Settings().clouds.windSpeedMetersPerSec;
        g_gpuMeshPreview.cloudQualitySamples = g_graph.Settings().clouds.qualitySamples;
        g_gpuMeshPreview.cloudShadowStrength = g_graph.Settings().clouds.shadowStrength;
        g_gpuMeshPreview.cloudShadowResolution = g_graph.Settings().clouds.shadowResolution;
        g_gpuMeshPreview.cloudShadowSamples = g_graph.Settings().clouds.shadowSamples;
        g_gpuMeshPreview.cloudFieldRadius = g_graph.Settings().clouds.fieldRadius;
        g_gpuMeshPreview.cloudFieldFalloff = g_graph.Settings().clouds.fieldFalloff;
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
    if (g_gpuMeshPreview.srvAllocated && g_gpuMeshPreview.colorState == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
    {
        drawList->PushClipRect(min, max, true);
        drawList->AddImage(static_cast<ImTextureID>(g_gpuMeshPreview.srvGpu.ptr), min, max);
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
        settings.clouds.enabled = false;
        break;
    case ViewportDisplayMode::Pbr:
        settings.preview.lightingMode = 1;
        settings.sky.mode = rock::SkyMode::SolidColor;
        settings.clouds.enabled = false;
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
            if (ImGui::Checkbox("雲を描画", &settings.clouds.enabled))
            {
                SaveAppSettingsSilently();
            }
        }
        ImGui::Separator();
        if (ImGui::Checkbox("FPSを表示", &g_ui.showFps))
        {
            SaveAppSettingsSilently();
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
    if (g_ui.showFps)
    {
        char fpsText[32]{};
        std::snprintf(fpsText, sizeof(fpsText), "FPS %.1f", ImGui::GetIO().Framerate);
        const ImVec2 fpsSize = ImGui::CalcTextSize(fpsText);
        const ImVec2 fpsPadding(9.0f, 5.0f);
        const ImVec2 fpsMax(max.x - 14.0f, min.y + 14.0f + fpsSize.y + fpsPadding.y * 2.0f);
        const ImVec2 fpsMin(fpsMax.x - fpsSize.x - fpsPadding.x * 2.0f, min.y + 14.0f);
        drawList->AddRectFilled(fpsMin, fpsMax, IM_COL32(8, 10, 10, 168), 4.0f);
        drawList->AddRect(fpsMin, fpsMax, ThemeColor("border", ImVec4(0.20f, 0.23f, 0.22f, 0.70f)), 4.0f);
        drawList->AddText(ImVec2(fpsMin.x + fpsPadding.x, fpsMin.y + fpsPadding.y), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), fpsText);
    }
    DrawViewportAxisGizmo(drawList, min, max);
}

ImU32 MapPreviewColor(float value, bool mask)
{
    value = std::clamp(value, 0.0f, 1.0f);
    if (mask)
    {
        const int r = static_cast<int>(35.0f + value * 220.0f);
        const int g = static_cast<int>(42.0f + value * 122.0f);
        const int b = static_cast<int>(44.0f + value * 24.0f);
        return IM_COL32(r, g, b, 255);
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
    const std::string title = maskPreview
        ? "2D View: " + std::string(heightfieldFieldName(evaluation.previewField))
        : "2D View: Heightmap";
    drawList->AddText(ImVec2(min.x + 16.0f, min.y + 14.0f), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), title.c_str());

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
        const int srcZ = samples > 1 ? static_cast<int>(std::lround(static_cast<float>(z) * static_cast<float>(gridResolution - 1) / static_cast<float>(samples - 1))) : 0;
        for (int x = 0; x < samples; ++x)
        {
            const int srcX = samples > 1 ? static_cast<int>(std::lround(static_cast<float>(x) * static_cast<float>(gridResolution - 1) / static_cast<float>(samples - 1))) : 0;
            const float sourceValue = mapValues[static_cast<size_t>(srcZ * gridResolution + srcX)];
            const float value = maskPreview ? sourceValue : (sourceValue - minHeight) / heightRange;
            const ImVec2 cellMin(mapMin.x + static_cast<float>(x) * cellSize, mapMin.y + static_cast<float>(z) * cellSize);
            const ImVec2 cellMax(mapMin.x + static_cast<float>(x + 1) * cellSize + 0.5f, mapMin.y + static_cast<float>(z + 1) * cellSize + 0.5f);
            drawList->AddRectFilled(cellMin, cellMax, MapPreviewColor(value, maskPreview));
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
    switch (kind)
    {
    case rock::NodeKind::HeightmapLoad:
        return ImVec4(0.38f, 0.62f, 0.53f, 1.0f);
    case rock::NodeKind::Shape:
        return ImVec4(0.50f, 0.68f, 0.48f, 1.0f);
    case rock::NodeKind::HeightmapBlur:
        return ImVec4(0.58f, 0.61f, 0.44f, 1.0f);
    case rock::NodeKind::ErosionNoise:
        return ImVec4(0.66f, 0.55f, 0.42f, 1.0f);
    case rock::NodeKind::MultiScaleErosion:
        return ImVec4(0.42f, 0.66f, 0.74f, 1.0f);
    case rock::NodeKind::MaskNoise:
        return ImVec4(0.62f, 0.46f, 0.74f, 1.0f);
    case rock::NodeKind::MaskBlend:
        return ImVec4(0.50f, 0.42f, 0.78f, 1.0f);
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
    case rock::NodeKind::ErosionNoise:
        return ImVec2(600.0f, 380.0f);
    case rock::NodeKind::MultiScaleErosion:
        return ImVec2(320.0f, 380.0f);
    case rock::NodeKind::MaskNoise:
        return ImVec2(40.0f, 520.0f);
    case rock::NodeKind::MaskBlend:
        return ImVec2(320.0f, 520.0f);
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
    if (!g_evaluationInFlight || evaluation.previewNodeId != node.id)
    {
        return;
    }

    const char* label = g_evaluationPending ? "計算待ち" : "計算中";
    const int dotCount = g_evaluationPending ? 0 : (static_cast<int>(ImGui::GetTime() * 3.0) % 4);
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
        if (startPin != nullptr && endPin != nullptr && containsCopiedNode(startPin->nodeId) && containsCopiedNode(endPin->nodeId))
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
            newMutableNode->erosionNoise = clipboardNode.node.erosionNoise;
            newMutableNode->multiScaleErosion = clipboardNode.node.multiScaleErosion;
            newMutableNode->maskNoise = clipboardNode.node.maskNoise;
            newMutableNode->maskBlend = clipboardNode.node.maskBlend;
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
        const rock::GraphId newStartPin = mappedPin(clipboardLink.startPin);
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
    ed::SetCurrentEditor(g_nodeEditor);
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
        addNodeMenuItem(rock::NodeKind::HeightmapLoad);
        addNodeMenuItem(rock::NodeKind::Shape);
        addNodeMenuItem(rock::NodeKind::HeightmapBlur);
        addNodeMenuItem(rock::NodeKind::ErosionNoise);
        addNodeMenuItem(rock::NodeKind::MultiScaleErosion);
        addNodeMenuItem(rock::NodeKind::MaskNoise);
        addNodeMenuItem(rock::NodeKind::MaskBlend);
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
    ed::SetCurrentEditor(nullptr);
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

bool DrawPropertyFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr, const char* format = "%.3f", ImGuiSliderFlags sliderFlags = 0)
{
    bool editEnded = false;
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
    if (ImGui::InputFloat("##number", value, 0.0f, 0.0f, format))
    {
        *value = std::clamp(*value, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();
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
    const float nextValue = std::clamp(percentValue / 100.0f, minValue, maxValue);
    const bool changed = nextValue != *value;
    if (nextValue != *value)
    {
        *value = nextValue;
    }
    return editEnded || changed;
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
    if (ImGui::InputInt("##number", value, 0, 0))
    {
        *value = std::clamp(*value, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();
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

bool DrawResolutionPresetRow(const char* label, const char* id, int* value, int defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr)
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    const int normalizedValue = NearestResolutionPreset(*value);
    if (*value != normalizedValue)
    {
        *value = normalizedValue;
    }
    const int normalizedDefault = NearestResolutionPreset(defaultValue);
    DrawPropertyLabel(label, tooltip, *value != normalizedDefault);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    constexpr float comboWidth = 110.0f;
    const std::string previewValue = std::to_string(*value);
    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##preset", previewValue.c_str()))
    {
        for (int preset : kResolutionPresets)
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

bool DrawCameraFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* format = "%.2f")
{
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
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
    if (ImGui::InputFloat("##number", value, 0.0f, 0.0f, format))
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
    if (selectedNode->kind == rock::NodeKind::HeightmapLoad && ImGui::BeginTable("HeightmapPropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 184.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        editableNode->heightmap.scaleMeters = std::clamp(editableNode->heightmap.scaleMeters, 1.0f, 8096.0f);
        editableNode->heightmap.relativeVerticalScalePercent = std::clamp(editableNode->heightmap.relativeVerticalScalePercent, 0.0f, 100.0f);
        editableNode->heightmap.verticalOffsetMeters = std::clamp(editableNode->heightmap.verticalOffsetMeters, -4096.0f, 4096.0f);
        editableNode->heightmap.simulationResolution = NearestResolutionPreset(editableNode->heightmap.simulationResolution);

        if (DrawPropertyPathRow("File", "HeightmapFile", &editableNode->heightmap.path, "Heightmap file changed", "読み込むハイトマップ画像です。明るいピクセルほど高い地形として扱います。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Scale (m)", "HeightmapScaleMeters", &editableNode->heightmap.scaleMeters, 1.0f, 8096.0f, rock::HeightmapLoadSettings{}.scaleMeters, "Heightmap scale changed", true, "地形の横幅と奥行きです。1 unit = 1 m として描画します。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Relative Vertical (%)", "HeightmapRelativeVerticalScale", &editableNode->heightmap.relativeVerticalScalePercent, 0.0f, 100.0f, rock::HeightmapLoadSettings{}.relativeVerticalScalePercent, "Heightmap vertical scale changed", true, "高さ方向の相対倍率です。実際の高さ範囲は Scale (m) x この値 / 100 になります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Offset (m)", "HeightmapVerticalOffset", &editableNode->heightmap.verticalOffsetMeters, -4096.0f, 4096.0f, rock::HeightmapLoadSettings{}.verticalOffsetMeters, "Heightmap vertical offset changed", true, "地形全体を上下に移動する高さオフセットです。"))
        {
            EvaluateGraph();
        }
        if (DrawResolutionPresetRow("Simulation Resolution", "HeightmapSimulationResolution", &editableNode->heightmap.simulationResolution, rock::HeightmapLoadSettings{}.simulationResolution, "Heightmap simulation resolution changed", true, "侵食や地形処理に使う内部ハイトフィールド解像度です。表示設定の Resolution はメッシュ表示の細かさだけを変更します。"))
        {
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::Shape && ImGui::BeginTable("ShapePropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 184.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        rock::ShapeSettings& shape = editableNode->shape;
        shape.scaleMeters = std::clamp(shape.scaleMeters, 1.0f, 8096.0f);
        shape.relativeHeightPercent = std::clamp(shape.relativeHeightPercent, 0.0f, 100.0f);
        shape.simulationResolution = NearestResolutionPreset(shape.simulationResolution);

        int shapeKind = static_cast<int>(shape.kind);
        if (DrawPropertyComboRow("Shape Type", "ShapeType", &shapeKind, "Hemisphere\0Pyramid\0", "デバッグ用の基本ハイトフィールド形状です。", static_cast<int>(rock::ShapeSettings{}.kind)))
        {
            shape.kind = static_cast<rock::ShapeKind>(std::clamp(shapeKind, 0, 1));
            g_graph.MarkDirty("Shape type changed");
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Scale (m)", "ShapeScaleMeters", &shape.scaleMeters, 1.0f, 8096.0f, rock::ShapeSettings{}.scaleMeters, "Shape scale changed", true, "シェープの横幅と奥行きです。1 unit = 1 m として描画します。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Relative Height (%)", "ShapeRelativeHeight", &shape.relativeHeightPercent, 0.0f, 100.0f, rock::ShapeSettings{}.relativeHeightPercent, "Shape height changed", true, "最大高さです。実際の高さは Scale (m) x この値 / 100 になります。"))
        {
            EvaluateGraph();
        }
        if (DrawResolutionPresetRow("Simulation Resolution", "ShapeSimulationResolution", &shape.simulationResolution, rock::ShapeSettings{}.simulationResolution, "Shape simulation resolution changed", true, "侵食や地形処理に使う内部ハイトフィールド解像度です。表示設定の Resolution はメッシュ表示の細かさだけを変更します。"))
        {
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::HeightmapBlur && ImGui::BeginTable("HeightmapBlurRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 184.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        rock::HeightmapBlurSettings& blur = editableNode->heightmapBlur;
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
        return;
    }

    if (selectedNode->kind == rock::NodeKind::ErosionNoise && ImGui::BeginTable("ErosionNoiseRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        rock::ErosionNoiseSettings& en = editableNode->erosionNoise;
        en.frequency = std::clamp(en.frequency, 0.0f, 256.0f);
        en.octaves = std::clamp(en.octaves, 0, 8);
        en.erosionStrength = std::clamp(en.erosionStrength, 0.0f, 1.0f);
        en.directionInfluence = std::clamp(en.directionInfluence, 0.0f, 8.0f);
        en.valleyLow = std::clamp(en.valleyLow, 0.0f, 1.0f);
        en.valleyHigh = std::clamp(en.valleyHigh, 0.0f, 1.0f);
        en.seed = std::clamp(en.seed, 0, 999999);

        if (DrawPropertyFloatRow("Frequency", "ErosionNoiseFrequency", &en.frequency, 0.0f, 256.0f, rock::ErosionNoiseSettings{}.frequency, "Erosion noise frequency changed", true, "地形範囲に対するノイズの周波数です。大きいほど細かい谷筋になります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyIntRow("Octaves", "ErosionNoiseOctaves", &en.octaves, 0, 8, rock::ErosionNoiseSettings{}.octaves, "Erosion noise octaves changed", true, "重ねる方向性ノイズのオクターブ数です。多いほど階層的なディテールが増えますが計算時間も増えます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyPercentRow("Strength (%)", "ErosionNoiseStrength", &en.erosionStrength, 0.0f, 1.0f, rock::ErosionNoiseSettings{}.erosionStrength, "Erosion noise strength changed", "ノイズで足し合わせる高さ寄与です。元地形の高さレンジに対する割合として扱います。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Direction Influence", "ErosionNoiseDirectionInfluence", &en.directionInfluence, 0.0f, 8.0f, rock::ErosionNoiseSettings{}.directionInfluence, "Erosion noise direction influence changed", true, "入力ハイトフィールドの勾配が谷筋方向に効く強さです。0 でランダム方向、大きいほど等高線に沿います。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Valley Low", "ErosionNoiseValleyLow", &en.valleyLow, 0.0f, 1.0f, rock::ErosionNoiseSettings{}.valleyLow, "Erosion noise valley low changed", true, "谷を滑らかに保つ smoothstep の下端しきい値です(正規化高度)。これより低い場所はノイズの寄与が抑えられます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Valley High", "ErosionNoiseValleyHigh", &en.valleyHigh, 0.0f, 1.0f, rock::ErosionNoiseSettings{}.valleyHigh, "Erosion noise valley high changed", true, "谷を滑らかに保つ smoothstep の上端しきい値です(正規化高度)。これより高い場所でノイズが完全に効きます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyIntRow("Seed", "ErosionNoiseSeed", &en.seed, 0, 999999, rock::ErosionNoiseSettings{}.seed, "Erosion noise seed changed", true, "ハッシュのオフセットです。同じパラメータでも異なるパターンを得るために使います。"))
        {
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MultiScaleErosion && ImGui::BeginTable("MultiScaleErosionRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        rock::MultiScaleErosionSettings& mse = editableNode->multiScaleErosion;
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
            if (DrawPropertyComboRow("Backend", "MseBackend", &backendInt, "CPU Reference\0GPU Compute\0\0", "CPU 並列実装と GPU compute (D3D12 + HLSL) を切り替えます。GPU は反復回数が多いほど速くなりますが、結果が CPU と微小にずれることがあります (浮動小数の累積順序)。\nGPU が初期化に失敗したり実行時エラーになると自動的に CPU 版にフォールバックします。", static_cast<int>(rock::MultiScaleErosionSettings{}.backend)))
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
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskNoise && ImGui::BeginTable("MaskNoiseRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        rock::MaskNoiseSettings& mn = editableNode->maskNoise;
        mn.seed = std::clamp(mn.seed, 0, 999999);
        mn.octaves = std::clamp(mn.octaves, 1, 12);
        mn.frequency = std::clamp(mn.frequency, 0.0f, 256.0f);
        mn.lacunarity = std::clamp(mn.lacunarity, 0.0f, 8.0f);
        mn.persistence = std::clamp(mn.persistence, 0.0f, 1.0f);
        mn.simulationResolution = NearestResolutionPreset(mn.simulationResolution);

        {
            int backendInt = static_cast<int>(mn.backend);
            if (DrawPropertyComboRow("Backend", "MaskNoiseBackend", &backendInt, "CPU Parallel\0GPU Compute\0\0", "CPU 並列実装と GPU compute (D3D12 + HLSL) を切り替えます。GPU は解像度が高いほど速くなります (1024² 以上で顕著)。\nGPU が初期化に失敗したり実行時エラーになると自動的に CPU 版にフォールバックします。", static_cast<int>(rock::MaskNoiseSettings{}.backend)))
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
        if (DrawResolutionPresetRow("Simulation Resolution", "MaskNoiseSimulationResolution", &mn.simulationResolution, rock::MaskNoiseSettings{}.simulationResolution, "Mask noise simulation resolution changed", true, "Mask の評価解像度です。高いほど細かい模様を解像できます。"))
        {
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::MaskBlend && ImGui::BeginTable("MaskBlendRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        rock::MaskBlendSettings& mb = editableNode->maskBlend;
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
        return;
    }

}

void DrawDisplaySettingsPanel()
{
    rock::GraphSettings& settings = g_graph.Settings();
    const float headerRightPadding = 10.0f;
    const float sectionWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - headerRightPadding);
    ImGui::BeginChild("PreviewDisplaySection", ImVec2(sectionWidth, 0.0f), false);
    if (!ImGui::CollapsingHeader("プレビュー画面", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::EndChild();
        return;
    }

    if (ImGui::BeginTable("PreviewDisplaySettingsRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (DrawPropertyBoolRow("Mesh Preview", "DisplayMeshPreview", &g_ui.meshPreview, "Mesh preview visibility changed", nullptr, UiState{}.meshPreview, true))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyBoolRow("FPS", "DisplayFps", &g_ui.showFps, "FPS visibility changed", nullptr, UiState{}.showFps, true))
        {
            SaveAppSettingsSilently();
        }
        if (DrawResolutionPresetRow("Resolution", "DisplayPreviewResolution", &settings.preview.resolution, rock::PreviewSettings{}.resolution, "Preview resolution changed", false))
        {
            EvaluateGraph();
            SaveAppSettingsSilently();
        }
        if (DrawPropertyIntRow("LOD", "DisplayPreviewLod", &settings.preview.lod, 0, 4, rock::PreviewSettings{}.lod, "Preview LOD changed", false))
        {
            EvaluateGraph();
            SaveAppSettingsSilently();
        }

        if (DrawPropertyBoolRow("Surface", "DisplaySurface", &settings.preview.showSurface, "Surface visibility changed", nullptr, rock::PreviewSettings{}.showSurface, true))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyBoolRow("Wireframe", "DisplayWireframe", &settings.preview.showWireframe, "Wireframe visibility changed", nullptr, rock::PreviewSettings{}.showWireframe, true))
        {
            SaveAppSettingsSilently();
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
            ImGui::SeparatorText("太陽");
            if (DrawPropertyFloatRow("Sun Azimuth (deg)", "DisplaySunAzimuth", &settings.preview.sunAzimuthDegrees, 0.0f, 360.0f, rock::PreviewSettings{}.sunAzimuthDegrees, "Sun azimuth changed", false, "太陽の水平角度です。地形の溝が読みやすい方向へ回せます。"))
            {
                SaveAppSettingsSilently();
            }
            if (DrawPropertyFloatRow("Sun Elevation (deg)", "DisplaySunElevation", &settings.preview.sunElevationDegrees, 1.0f, 89.0f, rock::PreviewSettings{}.sunElevationDegrees, "Sun elevation changed", false, "太陽の高さです。低いほど影が長く、凹凸が強調されます。"))
            {
                SaveAppSettingsSilently();
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
            if (DrawPropertyIntRow("Shadow Map Resolution", "DisplayShadowMapResolution", &settings.preview.shadowMapResolution, 512, 4096, rock::PreviewSettings{}.shadowMapResolution, "Shadow map resolution changed", false, "太陽方向から見た深度マップの解像度です。高いほど影の輪郭が細かくなりますが描画負荷が増えます。"))
            {
                settings.preview.shadowMapResolution = std::clamp(settings.preview.shadowMapResolution, 512, 4096);
                SaveAppSettingsSilently();
            }
            if (DrawPropertyFloatRow("Shadow Bias", "DisplayShadowBias", &settings.preview.shadowBias, 0.0f, 0.05f, rock::PreviewSettings{}.shadowBias, "Shadow bias changed", false, "影のにじみや縞を抑えるための深度オフセットです。大きすぎると影が浮いて見えます。"))
            {
                SaveAppSettingsSilently();
            }
        }

        rock::SkySettings& sky = settings.sky;
        if (displayMode == ViewportDisplayMode::Sky)
        {
            ImGui::SeparatorText("天球 (大気散乱)");
            DrawPropertyFloatRow("大気厚み (密度)", "SkyAtmosphereDensity", &sky.atmosphereDensity, 0.05f, 5.0f, rock::SkySettings{}.atmosphereDensity, "Sky atmosphere density changed", false, "Rayleigh 散乱係数 β_R の倍率。1.0 が地球標準、0.5 で薄い大気 (火星っぽい)、2-3 で濃い大気 (空が深く青く、夕焼けが赤く)。遠景フォグの強度も比例して上がります。");
            DrawPropertyFloatRow("ヘイズ (Mie 強度)", "SkyMieStrength", &sky.mieStrength, 0.0f, 8.0f, rock::SkySettings{}.mieStrength, "Sky mie strength changed", false, "Mie 散乱の強さ。大きいほど大気が霞んで見え、太陽周辺のグローも強くなります。0 で純 Rayleigh (透明な青空)、1 で標準的な大気。");
            DrawPropertyFloatRow("Mie 偏向 (g)", "SkyMieG", &sky.mieEccentricity, -0.95f, 0.95f, rock::SkySettings{}.mieEccentricity, "Sky mie g changed", false, "Henyey-Greenstein g 値。0 で等方散乱、正で前方 (太陽方向) 散乱が強くなりグローが太陽周りに集中。0.7-0.85 が現実的。");
            DrawPropertyFloatRow("遠景フォグ強度", "SkyAerialPerspective", &sky.aerialPerspectiveStrength, 0.0f, 4.0f, rock::SkySettings{}.aerialPerspectiveStrength, "Sky aerial perspective changed", false, "地形の遠景に大気の霞をかぶせる強さ。0 で霧無し (くっきり遠景)、1 で物理どおり、2 以上で強めの霧。大気密度を上げると物理的にも自動で霧が強くなりますが、これは見た目だけ独立に強める/弱めるためのスケール。");
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
                DrawPropertyFloatRow("Wind Direction (deg)", "CloudWindDir", &clouds.windDirectionDegrees, 0.0f, 360.0f, rock::CloudSettings{}.windDirectionDegrees, "Cloud wind direction changed", false, "風の向き(度、北=0、東=90)。Wind Speed > 0 のときに雲が流れる方向。", "%.0f");
                DrawPropertyFloatRow("Wind Speed (m/s)", "CloudWindSpeed", &clouds.windSpeedMetersPerSec, 0.0f, 200.0f, rock::CloudSettings{}.windSpeedMetersPerSec, "Cloud wind speed changed", false, "雲が流れる速度 (m/s)。0 で静止。動かすとフレーム毎にビューポートが再描画され負荷が増えます。");
                DrawPropertyIntRow("Quality (samples)", "CloudQuality", &clouds.qualitySamples, 8, 96, rock::CloudSettings{}.qualitySamples, "Cloud quality changed", false, "1 ピクセルあたりのレイマーチサンプル数。大きいほど雲のディテールが上がりますが負荷も増えます。32 が標準、低スペックなら 16、高品質なら 64。");
                DrawPropertyFloatRow("Shadow Strength", "CloudShadowStrength", &clouds.shadowStrength, 0.0f, 1.0f, rock::CloudSettings{}.shadowStrength, "Cloud shadow strength changed", false, "雲が地形に落とす影の強さ。0 で影無し、1 で完全に暗くなります。太陽方向に projection した雲の透過率を地形シェーダーで乗算します。");
                DrawPropertyIntRow("Shadow Resolution", "CloudShadowResolution", &clouds.shadowResolution, 256, 4096, rock::CloudSettings{}.shadowResolution, "Cloud shadow resolution changed", false, "雲影テクスチャの解像度 (片辺ピクセル数)。1024 で約 1MB。大きいほど影の輪郭が細かくなりますが生成負荷が増えます。");
                DrawPropertyIntRow("Shadow Samples", "CloudShadowSamples", &clouds.shadowSamples, 4, 64, rock::CloudSettings{}.shadowSamples, "Cloud shadow samples changed", false, "雲影テクスチャ生成時に太陽方向へ撃つレイのサンプル数。大きいほど厚い雲の影が正確になりますが生成時間も増えます。16 が標準。");
            }
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

    ImGui::Spacing();
    if (ImGui::BeginTable("CameraRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        DrawCameraFloatRow("FOV", "FovDegrees", &g_viewport.fovDegrees, 15.0f, 90.0f, 45.0f, "%.1f");
        float focalLengthMm = CameraFocalLengthMmFromFovYDegrees(g_viewport.fovDegrees);
        if (DrawCameraFloatRow("焦点距離 (mm)", "FocalLengthMm", &focalLengthMm, 1.0f, 200.0f, CameraFocalLengthMmFromFovYDegrees(45.0f), "%.1f"))
        {
            g_viewport.fovDegrees = CameraFovYDegreesFromFocalLengthMm(focalLengthMm);
        }
        DrawCameraFloatRow("Distance", "OrbitDistance", &g_viewport.orbitDistance, 1.0f, 10000.0f, 1800.0f, "%.1f");
        DrawCameraFloatRow("Zoom", "ViewportZoom", &g_viewport.zoom, 0.05f, 20.0f, 1.0f, "%.2f");
        DrawCameraFloatRow("Yaw", "ViewportYaw", &g_viewport.yaw, -3.14159f, 3.14159f, 0.0f, "%.3f");
        DrawCameraFloatRow("Pitch", "ViewportPitch", &g_viewport.pitch, -1.25f, 1.25f, 0.0f, "%.3f");

        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Right-handed / Y-up");
    ImGui::TextDisabled("Grid: %d x %d, %.0f m cells",
        g_graph.Settings().preview.gridCellCount,
        g_graph.Settings().preview.gridCellCount,
        g_graph.Settings().preview.gridCellSizeMeters);
}

void DrawStatsPanel()
{
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    ImGui::Text("Graph Version: %llu", static_cast<unsigned long long>(evaluation.version));
    ImGui::Text("%s", g_lastEvaluationDuration.c_str());
    ImGui::TextColored(evaluation.dirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f), "%s", evaluation.dirty ? "Dirty" : "Evaluated");
    ImGui::TextWrapped("%s", evaluation.status.c_str());

    ImGui::SeparatorText("Preview");
    ImGui::Text("Stage: %s", rock::ToString(evaluation.previewStage).data());
    ImGui::SeparatorText("Mesh Topology");
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
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F12, false))
    {
        std::filesystem::path screenshotPath;
        std::string error;
        if (terrain::CaptureWindowScreenshot(g_hwnd, ScreenshotDirectory(), &screenshotPath, &error))
        {
            g_projectStatus = "Screenshot saved " + PathToUtf8(screenshotPath);
            RevealFileInExplorer(screenshotPath);
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
            rock::GraphSettings& settings = g_graph.Settings();
            if (ImGui::MenuItem("Mesh", nullptr, g_ui.meshPreview))
            {
                g_ui.meshPreview = !g_ui.meshPreview;
                SaveAppSettingsSilently();
            }
            if (ImGui::MenuItem("Wireframe", nullptr, &settings.preview.showWireframe))
            {
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
        if (ImGui::BeginMenu("エクスポート"))
        {
            if (ImGui::MenuItem("OBJ"))
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
            ImGui::MenuItem("glTF", nullptr, false, false);
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

    if (DrawHorizontalSplitter("InspectorLayoutSplitter", &nodePaneHeight, rightColumnHeight, 160.0f, 160.0f))
    {
        if (inspectorLayoutCanFit)
        {
            g_ui.nodePaneHeight = nodePaneHeight;
            SaveAppSettingsSilently();
        }
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
        if (BeginStyledTabItem("表示設定"))
        {
            BeginInspectorTabContent();
            DrawDisplaySettingsPanel();
            EndInspectorTabContent();
            EndStyledTabItem(defaultTabStyle);
        }
        if (BeginStyledTabItem("統計"))
        {
            BeginInspectorTabContent();
            DrawStatsPanel();
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
            ProcessPendingMseGpuRequests();
            ProcessPendingMaskNoiseGpuRequests();
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
