#include <windows.h>
#include <commdlg.h>

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
#include "sdf_preview.h"
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
rock::UiThemeManager g_themeManager;
rock::GraphId g_selectedNodeId = 0;
rock::GraphId g_lastFinalOutputNodeId = 0;
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
    rock::PreviewStage previewStage = rock::PreviewStage::Output;
};

std::vector<GraphEditSnapshot> g_undoStack;
std::vector<GraphEditSnapshot> g_redoStack;
std::optional<GraphEditSnapshot> g_pendingPropertyEditUndo;
std::optional<GraphEditSnapshot> g_pendingNodeMoveUndo;

struct UiState
{
    int primitive = 0;
    float noiseAmplitude = 0.35f;
    float noiseFrequency = 2.0f;
    int noiseOctaves = 4;
    float crackWidth = 0.035f;
    float crackDepth = 0.42f;
    float crackRoughness = 0.65f;
    bool meshPreview = true;
    float rightPaneWidth = 0.0f;
    float nodePaneHeight = 0.0f;
};

UiState g_ui;

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
    ComPtr<ID3D12Resource> colorTarget;
    ComPtr<ID3D12Resource> depthTarget;
    ComPtr<ID3D12Resource> shadowTarget;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> edgeIndexBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    bool srvAllocated = false;
    bool shadowSrvAllocated = false;
    UINT vertexCount = 0;
    UINT triIndexCount = 0;
    UINT edgeIndexCount = 0;
    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

ComPtr<ID3D12RootSignature> g_meshPreviewRootSignature;
ComPtr<ID3D12PipelineState> g_meshPreviewSurfacePso;
ComPtr<ID3D12PipelineState> g_meshPreviewWirePso;
ComPtr<ID3D12PipelineState> g_meshPreviewShadowPso;
ComPtr<ID3D12RootSignature> g_fluvialComputeRootSignature;
ComPtr<ID3D12PipelineState> g_fluvialGridErosionPso;
ComPtr<ID3D12PipelineState> g_fluvialFlowAccumulationPso;
ComPtr<ID3D12DescriptorHeap> g_meshPreviewRtvHeap;
ComPtr<ID3D12DescriptorHeap> g_meshPreviewDsvHeap;
GpuMeshPreview g_gpuMeshPreview;
std::string g_fluvialComputeStatus = "GPU Compute not initialized";
bool g_fluvialComputeSmokeTested = false;
std::mutex g_fluvialComputeMutex;
std::mutex g_fluvialGpuRequestMutex;
std::thread::id g_mainThreadId;

struct FluvialGpuRequestResult
{
    bool success = false;
    rock::HeightfieldGrid grid;
    std::string error;
};

struct FluvialGpuRequest
{
    rock::HeightfieldGrid grid;
    rock::FluvialErosionSettings settings;
    std::promise<FluvialGpuRequestResult> promise;
};

std::vector<std::shared_ptr<FluvialGpuRequest>> g_pendingFluvialGpuRequests;

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
    g_meshPreviewSurfacePso.Reset();
    g_meshPreviewWirePso.Reset();
    g_meshPreviewRootSignature.Reset();
    g_fluvialGridErosionPso.Reset();
    g_fluvialFlowAccumulationPso.Reset();
    g_fluvialComputeRootSignature.Reset();
    g_fluvialComputeSmokeTested = false;
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

std::filesystem::path FluvialComputeShaderPath()
{
    return ShaderPath("fluvial_erosion_compute.hlsl");
}

void EvaluateGraph();
void ProcessPendingFluvialGpuRequests();
void EnsureFinalMesh(rock::GraphId outputNodeId = 0);
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
            {"meshSurface", settings.preview.showSurface},
            {"meshWireframe", settings.preview.showWireframe},
            {"surfacePoints", settings.preview.showPoints},
            {"grid", settings.preview.showGrid},
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
        settings.preview.showSurface = visibilityJson.value("meshSurface", settings.preview.showSurface);
        settings.preview.showWireframe = visibilityJson.value("meshWireframe", settings.preview.showWireframe);
        settings.preview.showPoints = visibilityJson.value("surfacePoints", settings.preview.showPoints);
        settings.preview.showGrid = visibilityJson.value("grid", settings.preview.showGrid);
        settings.preview.resolution = std::clamp(visibilityJson.value("previewResolution", settings.preview.resolution), 16, 512);
        settings.preview.lod = std::clamp(visibilityJson.value("previewLod", settings.preview.lod), 0, 4);
        settings.preview.lightingMode = std::clamp(visibilityJson.value("lightingMode", settings.preview.lightingMode), 0, 2);
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
    else if (g_graph.FindNode(snapshot.selectedNodeId) != nullptr)
    {
        g_graph.SetPreviewNode(snapshot.selectedNodeId);
    }

    g_pendingNodePositions = snapshot.nodePositions;
    g_nodePositionCache = snapshot.nodePositions;
    g_pendingSelectedNodeIds = snapshot.selectedNodeIds;
    g_selectedNodeId = snapshot.selectedNodeId;
    g_nodePositionsInitialized = false;
    g_lastFinalOutputNodeId = 0;
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

        root["settings"] = nlohmann::json::object();

        root["nodeSettings"] = nlohmann::json::object();
        for (const rock::Node& node : g_graph.Nodes())
        {
            if (node.kind == rock::NodeKind::OutputMesh)
            {
                root["nodeSettings"][std::to_string(node.id)] = {
                    {"outputMesh", {
                        {"resolution", node.outputMesh.resolution},
                        {"lod", node.outputMesh.lod},
                        {"isoValue", node.outputMesh.isoValue},
                    }},
                };
            }
        }

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
                {"primitive", {{"kind", static_cast<int>(node.primitive.kind)}}},
                {"noise", {
                    {"amplitude", node.noise.amplitude},
                    {"frequency", node.noise.frequency},
                    {"octaves", node.noise.octaves},
                    {"seed", node.noise.seed},
                }},
                {"crack", {
                    {"width", node.crack.width},
                    {"depth", node.crack.depth},
                    {"roughness", node.crack.roughness},
                }},
                {"outputMesh", {
                    {"resolution", node.outputMesh.resolution},
                    {"lod", node.outputMesh.lod},
                    {"isoValue", node.outputMesh.isoValue},
                }},
                {"heightmap", {
                    {"path", node.heightmap.path},
                    {"scaleMeters", node.heightmap.scaleMeters},
                    {"relativeVerticalScalePercent", node.heightmap.relativeVerticalScalePercent},
                    {"verticalOffsetMeters", node.heightmap.verticalOffsetMeters},
                    {"simulationResolution", node.heightmap.simulationResolution},
                }},
                {"fluvialErosion", {
                    {"backend", static_cast<int>(node.fluvialErosion.backend)},
                    {"useAdvancedParameters", node.fluvialErosion.useAdvancedParameters},
                    {"featureSize", node.fluvialErosion.featureSize},
                    {"iterations", node.fluvialErosion.iterations},
                    {"channelLength", node.fluvialErosion.channelLength},
                    {"erosionStrength", node.fluvialErosion.erosionStrength},
                    {"channeling", node.fluvialErosion.channeling},
                    {"friction", node.fluvialErosion.friction},
                    {"wearAngleDegrees", node.fluvialErosion.wearAngleDegrees},
                    {"depositAngleDegrees", node.fluvialErosion.depositAngleDegrees},
                    {"maxErosionAngleDegrees", node.fluvialErosion.maxErosionAngleDegrees},
                    {"erosionGranularity", node.fluvialErosion.erosionGranularity},
                    {"sedimentVelocity", node.fluvialErosion.sedimentVelocity},
                    {"sedimentCapacity", node.fluvialErosion.sedimentCapacity},
                    {"depositionRate", node.fluvialErosion.depositionRate},
                    {"levelStrengths", node.fluvialErosion.levelStrengths},
                    {"seed", node.fluvialErosion.seed},
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
        const nlohmann::json primitiveJson = settingsJson.value("primitive", nlohmann::json::object());
        const nlohmann::json noiseJson = settingsJson.value("noise", nlohmann::json::object());
        const nlohmann::json crackJson = settingsJson.value("crack", nlohmann::json::object());
        const nlohmann::json outputMeshJson = settingsJson.value("outputMesh", nlohmann::json::object());
        const nlohmann::json nodesJson = root.value("nodes", nlohmann::json::array());
        if (nodesJson.is_array() && !nodesJson.empty())
        {
            std::vector<rock::Node> nodes;
            rock::GraphId syntheticPinId = 100000;
            for (const nlohmann::json& nodeJson : nodesJson)
            {
                rock::Node node;
                node.id = nodeJson.value("id", 0);
                node.kind = static_cast<rock::NodeKind>(std::clamp(nodeJson.value("kind", 0), 0, 5));
                if (!IsTerrainNodeKind(node.kind))
                {
                    continue;
                }
                node.title = nodeJson.value("title", std::string(rock::ToString(node.kind)));
                if (node.kind == rock::NodeKind::HeightmapLoad && node.title == "Load Heightmap")
                {
                    node.title = std::string(rock::ToString(node.kind));
                }
                const nlohmann::json nodePrimitiveJson = nodeJson.value("primitive", primitiveJson);
                const nlohmann::json nodeNoiseJson = nodeJson.value("noise", noiseJson);
                const nlohmann::json nodeCrackJson = nodeJson.value("crack", crackJson);
                const nlohmann::json nodeOutputMeshJson = nodeJson.value("outputMesh", nlohmann::json::object());
                const nlohmann::json nodeHeightmapJson = nodeJson.value("heightmap", nlohmann::json::object());
                const nlohmann::json nodeFluvialJson = nodeJson.value("fluvialErosion", nlohmann::json::object());
                node.primitive.kind = static_cast<rock::PrimitiveKind>(std::clamp(nodePrimitiveJson.value("kind", static_cast<int>(node.primitive.kind)), 0, 4));
                node.noise.amplitude = nodeNoiseJson.value("amplitude", node.noise.amplitude);
                node.noise.frequency = nodeNoiseJson.value("frequency", node.noise.frequency);
                node.noise.octaves = std::clamp(nodeNoiseJson.value("octaves", node.noise.octaves), 1, 8);
                node.noise.seed = std::clamp(nodeNoiseJson.value("seed", node.noise.seed), 0, 999999);
                node.crack.width = nodeCrackJson.value("width", node.crack.width);
                node.crack.depth = nodeCrackJson.value("depth", node.crack.depth);
                node.crack.roughness = nodeCrackJson.value("roughness", node.crack.roughness);
                node.outputMesh.resolution = std::clamp(nodeOutputMeshJson.value("resolution", node.outputMesh.resolution), 16, 512);
                node.outputMesh.lod = std::clamp(nodeOutputMeshJson.value("lod", node.outputMesh.lod), 0, 4);
                node.outputMesh.isoValue = std::clamp(nodeOutputMeshJson.value("isoValue", node.outputMesh.isoValue), -0.2f, 0.2f);
                node.heightmap.path = nodeHeightmapJson.value("path", node.heightmap.path);
                node.heightmap.scaleMeters = std::clamp(nodeHeightmapJson.value("scaleMeters", node.heightmap.scaleMeters), 1.0f, 1000000.0f);
                node.heightmap.relativeVerticalScalePercent = std::clamp(nodeHeightmapJson.value("relativeVerticalScalePercent", node.heightmap.relativeVerticalScalePercent), 0.0f, 10000.0f);
                node.heightmap.verticalOffsetMeters = std::clamp(nodeHeightmapJson.value("verticalOffsetMeters", node.heightmap.verticalOffsetMeters), -1000000.0f, 1000000.0f);
                node.heightmap.simulationResolution = std::clamp(nodeHeightmapJson.value("simulationResolution", node.heightmap.simulationResolution), 2, 2048);
                node.fluvialErosion.backend = static_cast<rock::FluvialBackend>(std::clamp(nodeFluvialJson.value("backend", static_cast<int>(node.fluvialErosion.backend)), 0, 1));
                node.fluvialErosion.useAdvancedParameters = nodeFluvialJson.value("useAdvancedParameters", node.fluvialErosion.useAdvancedParameters);
                node.fluvialErosion.featureSize = std::clamp(nodeFluvialJson.value("featureSize", node.fluvialErosion.featureSize), 1.0f, 64.0f);
                node.fluvialErosion.iterations = std::clamp(nodeFluvialJson.value("iterations", node.fluvialErosion.iterations), 0, 200);
                node.fluvialErosion.channelLength = std::clamp(nodeFluvialJson.value("channelLength", node.fluvialErosion.channelLength), 1.0f, 1024.0f);
                node.fluvialErosion.erosionStrength = std::clamp(nodeFluvialJson.value("erosionStrength", node.fluvialErosion.erosionStrength), 0.0f, 1.0f);
                node.fluvialErosion.channeling = std::clamp(nodeFluvialJson.value("channeling", node.fluvialErosion.channeling), 0.0f, 1.0f);
                node.fluvialErosion.friction = std::clamp(nodeFluvialJson.value("friction", node.fluvialErosion.friction), 0.0f, 1.0f);
                node.fluvialErosion.wearAngleDegrees = std::clamp(nodeFluvialJson.value("wearAngleDegrees", node.fluvialErosion.wearAngleDegrees), 0.0f, 90.0f);
                node.fluvialErosion.depositAngleDegrees = std::clamp(nodeFluvialJson.value("depositAngleDegrees", node.fluvialErosion.depositAngleDegrees), 0.0f, 90.0f);
                node.fluvialErosion.maxErosionAngleDegrees = std::clamp(nodeFluvialJson.value("maxErosionAngleDegrees", node.fluvialErosion.maxErosionAngleDegrees), 0.0f, 90.0f);
                node.fluvialErosion.erosionGranularity = std::clamp(nodeFluvialJson.value("erosionGranularity", node.fluvialErosion.erosionGranularity), 1.0f, 100.0f);
                node.fluvialErosion.sedimentVelocity = std::clamp(nodeFluvialJson.value("sedimentVelocity", node.fluvialErosion.sedimentVelocity), 0.0f, 2.0f);
                node.fluvialErosion.sedimentCapacity = std::clamp(nodeFluvialJson.value("sedimentCapacity", node.fluvialErosion.sedimentCapacity), 0.0f, 2.0f);
                node.fluvialErosion.depositionRate = std::clamp(nodeFluvialJson.value("depositionRate", node.fluvialErosion.depositionRate), 0.0f, 1.0f);
                if (nodeFluvialJson.contains("levelStrengths") && nodeFluvialJson["levelStrengths"].is_array())
                {
                    const nlohmann::json& levelStrengthsJson = nodeFluvialJson["levelStrengths"];
                    for (size_t i = 0; i < node.fluvialErosion.levelStrengths.size() && i < levelStrengthsJson.size(); ++i)
                    {
                        node.fluvialErosion.levelStrengths[i] = std::clamp(levelStrengthsJson[i].get<float>(), 0.0f, 2.0f);
                    }
                }
                node.fluvialErosion.seed = std::clamp(nodeFluvialJson.value("seed", node.fluvialErosion.seed), 0, 999999);

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
                        pin.valueType = static_cast<rock::ValueType>(std::clamp(pinJson.value("valueType", 0), 0, 3));
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
                if (node.kind == rock::NodeKind::FluvialErosion &&
                    std::ranges::none_of(node.outputs, [](const rock::Pin& pin) { return pin.valueType == rock::ValueType::Mask; }))
                {
                    node.outputs.push_back({syntheticPinId++, node.id, rock::PinKind::Output, rock::ValueType::Mask, "Fluvial Mask"});
                }
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
        rock::OutputMeshSettings legacyOutputMesh = g_graph.OutputMeshSettingsFor();
        rock::PrimitiveSettings legacyPrimitive;
        rock::NoiseSettings legacyNoise;
        rock::CrackSettings legacyCrack;
        legacyPrimitive.kind = static_cast<rock::PrimitiveKind>(std::clamp(primitiveJson.value("kind", static_cast<int>(legacyPrimitive.kind)), 0, 4));
        legacyNoise.amplitude = noiseJson.value("amplitude", legacyNoise.amplitude);
        legacyNoise.frequency = noiseJson.value("frequency", legacyNoise.frequency);
        legacyNoise.octaves = std::clamp(noiseJson.value("octaves", legacyNoise.octaves), 1, 8);
        legacyCrack.width = crackJson.value("width", legacyCrack.width);
        legacyCrack.depth = crackJson.value("depth", legacyCrack.depth);
        legacyCrack.roughness = crackJson.value("roughness", legacyCrack.roughness);
        legacyOutputMesh.resolution = std::clamp(outputMeshJson.value("resolution", legacyOutputMesh.resolution), 16, 512);
        legacyOutputMesh.lod = std::clamp(outputMeshJson.value("lod", legacyOutputMesh.lod), 0, 4);
        legacyOutputMesh.isoValue = std::clamp(outputMeshJson.value("isoValue", legacyOutputMesh.isoValue), -0.2f, 0.2f);
        for (const rock::Node& node : g_graph.Nodes())
        {
            rock::Node* mutableNode = g_graph.FindMutableNode(node.id);
            if (mutableNode == nullptr)
            {
                continue;
            }
            if (!nodesJson.is_array() || nodesJson.empty())
            {
                mutableNode->primitive = legacyPrimitive;
                mutableNode->noise = legacyNoise;
                mutableNode->crack = legacyCrack;
            }
            if (rock::OutputMeshSettings* nodeOutputMesh = g_graph.FindOutputMeshSettings(node.id))
            {
                *nodeOutputMesh = legacyOutputMesh;
            }
        }
        const nlohmann::json nodeSettingsJson = root.value("nodeSettings", nlohmann::json::object());
        for (const auto& [nodeIdText, nodeSettingsJsonValue] : nodeSettingsJson.items())
        {
            const int nodeId = std::stoi(nodeIdText);
            if (rock::OutputMeshSettings* nodeOutputMesh = g_graph.FindOutputMeshSettings(nodeId))
            {
                const nlohmann::json nodeOutputMeshJson = nodeSettingsJsonValue.value("outputMesh", nlohmann::json::object());
                nodeOutputMesh->resolution = std::clamp(nodeOutputMeshJson.value("resolution", nodeOutputMesh->resolution), 16, 512);
                nodeOutputMesh->lod = std::clamp(nodeOutputMeshJson.value("lod", nodeOutputMesh->lod), 0, 4);
                nodeOutputMesh->isoValue = std::clamp(nodeOutputMeshJson.value("isoValue", nodeOutputMesh->isoValue), -0.2f, 0.2f);
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
        g_graph.SetPreviewStage(static_cast<rock::PreviewStage>(std::clamp(root.value("previewStage", static_cast<int>(g_graph.Preview())), 0, 3)));
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

    D3D12_ROOT_PARAMETER rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[0].Constants.ShaderRegister = 0;
    rootParams[0].Constants.RegisterSpace = 0;
    rootParams[0].Constants.Num32BitValues = sizeof(MeshPreviewConstants) / sizeof(UINT);
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &shadowRange;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC shadowSampler{};
    shadowSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    shadowSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    shadowSampler.ShaderRegister = 0;
    shadowSampler.RegisterSpace = 0;
    shadowSampler.MaxLOD = D3D12_FLOAT32_MAX;
    shadowSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &shadowSampler;
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

bool RunFluvialComputeSmokeTest(std::string* error)
{
    if (g_fluvialComputeSmokeTested)
    {
        return true;
    }
    if (!g_device || !g_commandQueue || !g_fluvialComputeRootSignature || !g_fluvialGridErosionPso || !g_fluvialFlowAccumulationPso)
    {
        if (error) *error = "GPU Compute pipeline not initialized";
        return false;
    }

    constexpr UINT resolution = 8;
    constexpr UINT cellCount = resolution * resolution;
    constexpr UINT64 bufferSize = cellCount * sizeof(float);
    std::array<float, cellCount> inputHeights{};
    std::array<float, cellCount> zeroData{};
    for (UINT z = 0; z < resolution; ++z)
    {
        for (UINT x = 0; x < resolution; ++x)
        {
            inputHeights[z * resolution + x] = static_cast<float>(resolution - z) * 0.1f + static_cast<float>(x) * 0.01f;
        }
    }

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC gpuDesc = BufferResourceDesc(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC cpuDesc = BufferResourceDesc(bufferSize);

    ComPtr<ID3D12Resource> heightIn;
    ComPtr<ID3D12Resource> heightOut;
    ComPtr<ID3D12Resource> maskOut;
    ComPtr<ID3D12Resource> flowIn;
    ComPtr<ID3D12Resource> flowOut;
    ComPtr<ID3D12Resource> uploadHeights;
    ComPtr<ID3D12Resource> uploadZero;
    ComPtr<ID3D12Resource> uploadOne;
    ComPtr<ID3D12Resource> readbackHeights;
    ComPtr<ID3D12Resource> readbackMask;

    HRESULT hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&heightIn));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke height input failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&heightOut));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke height output failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&maskOut));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke mask output failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&flowIn));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke flow input failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&flowOut));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke flow output failed"; return false; }
    hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeights));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke height upload failed"; return false; }
    hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadZero));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke zero upload failed"; return false; }
    hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadOne));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke one upload failed"; return false; }
    hr = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackHeights));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke height readback failed"; return false; }
    hr = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackMask));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke mask readback failed"; return false; }

    void* mapped = nullptr;
    D3D12_RANGE readRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &readRange, &mapped), "Map fluvial smoke height upload failed");
    std::memcpy(mapped, inputHeights.data(), bufferSize);
    uploadHeights->Unmap(0, nullptr);
    ThrowIfFailed(uploadZero->Map(0, &readRange, &mapped), "Map fluvial smoke zero upload failed");
    std::memcpy(mapped, zeroData.data(), bufferSize);
    uploadZero->Unmap(0, nullptr);
    std::array<float, cellCount> oneData{};
    oneData.fill(1.0f);
    ThrowIfFailed(uploadOne->Map(0, &readRange, &mapped), "Map fluvial smoke one upload failed");
    std::memcpy(mapped, oneData.data(), bufferSize);
    uploadOne->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 5;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create fluvial smoke descriptor heap failed"; return false; }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = cellCount;
    uavDesc.Buffer.StructureByteStride = sizeof(float);
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateUnorderedAccessView(heightIn.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(heightOut.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(maskOut.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowIn.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowOut.Get(), nullptr, &uavDesc, descriptor);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create fluvial smoke command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create fluvial smoke command list failed");

    commandList->CopyBufferRegion(heightIn.Get(), 0, uploadHeights.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(heightOut.Get(), 0, uploadZero.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(maskOut.Get(), 0, uploadZero.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(flowIn.Get(), 0, uploadOne.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(flowOut.Get(), 0, uploadOne.Get(), 0, bufferSize);

    D3D12_RESOURCE_BARRIER toUav[5]{};
    ID3D12Resource* uavResources[5] = {heightIn.Get(), heightOut.Get(), maskOut.Get(), flowIn.Get(), flowOut.Get()};
    for (int i = 0; i < 5; ++i)
    {
        toUav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav[i].Transition.pResource = uavResources[i];
        toUav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toUav[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(5, toUav);

    struct FluvialGridConstants
    {
        UINT resolution;
        UINT cellCount;
        UINT iteration;
        float terrainSizeMeters;
        float erosionStrength;
        float sedimentCapacity;
        float depositionRate;
        float channeling;
        float cellSizeMeters;
        float wearSlope;
        float maxSlope;
        float strengthScale;
    } constants{resolution, cellCount, 0, 8.0f, 1.0f, 0.8f, 0.35f, 0.25f, 8.0f / static_cast<float>(resolution - 1), 0.05f, 10.0f, 0.68f};

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_fluvialComputeRootSignature.Get());
    commandList->SetPipelineState(g_fluvialGridErosionPso.Get());
    commandList->SetComputeRoot32BitConstants(0, 12, &constants, 0);
    commandList->SetComputeRootDescriptorTable(1, descriptorHeap->GetGPUDescriptorHandleForHeapStart());
    commandList->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr;
    commandList->ResourceBarrier(1, &uavBarrier);

    D3D12_RESOURCE_BARRIER toCopy[2]{};
    ID3D12Resource* copyResources[2] = {heightOut.Get(), maskOut.Get()};
    for (int i = 0; i < 2; ++i)
    {
        toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy[i].Transition.pResource = copyResources[i];
        toCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(2, toCopy);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, heightOut.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(readbackMask.Get(), 0, maskOut.Get(), 0, bufferSize);
    ThrowIfFailed(commandList->Close(), "Close fluvial smoke command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal fluvial smoke fence failed");
    WaitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedMask = nullptr;
    D3D12_RANGE outputReadRange{0, static_cast<SIZE_T>(bufferSize)};
    ThrowIfFailed(readbackHeights->Map(0, &outputReadRange, &mappedHeights), "Map fluvial smoke height readback failed");
    ThrowIfFailed(readbackMask->Map(0, &outputReadRange, &mappedMask), "Map fluvial smoke mask readback failed");
    const float* outputHeights = static_cast<const float*>(mappedHeights);
    const float* outputMask = static_cast<const float*>(mappedMask);

    bool changed = false;
    bool maskWritten = false;
    for (UINT i = 0; i < cellCount; ++i)
    {
        changed = changed || std::abs(outputHeights[i] - inputHeights[i]) > 0.000001f;
        maskWritten = maskWritten || outputMask[i] > 0.000001f;
    }

    D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackMask->Unmap(0, &emptyWriteRange);

    if (!changed || !maskWritten)
    {
        if (error) *error = "GPU Compute smoke dispatch produced no erosion output";
        return false;
    }

    g_fluvialComputeSmokeTested = true;
    return true;
}

bool EnsureFluvialComputePipeline(std::string* error)
{
    if (g_fluvialGridErosionPso && g_fluvialFlowAccumulationPso)
    {
        if (RunFluvialComputeSmokeTest(error))
        {
            g_fluvialComputeStatus = "GPU Compute dispatch ready";
        }
        else
        {
            g_fluvialComputeStatus = "GPU Compute dispatch failed";
            return false;
        }
        return true;
    }
    if (!g_device)
    {
        if (error) *error = "D3D12 device not initialized";
        g_fluvialComputeStatus = "GPU Compute unavailable";
        return false;
    }

    D3D12_DESCRIPTOR_RANGE uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 5;
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
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Serialize fluvial compute root sig failed";
        g_fluvialComputeStatus = "GPU Compute root signature failed";
        return false;
    }
    hr = g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_fluvialComputeRootSignature));
    if (FAILED(hr))
    {
        if (error) *error = "Create fluvial compute root sig failed";
        g_fluvialComputeStatus = "GPU Compute root signature failed";
        return false;
    }

    const std::filesystem::path shaderPath = FluvialComputeShaderPath();
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> flowBlob, erosionBlob;
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSFlowAccumulation", "cs_5_0", compileFlags, 0, &flowBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile fluvial flow CS failed";
        g_fluvialComputeStatus = "GPU Compute flow shader compile failed";
        return false;
    }
    errBlob.Reset();
    hr = D3DCompileFromFile(shaderPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "CSGridErosion", "cs_5_0", compileFlags, 0, &erosionBlob, &errBlob);
    if (FAILED(hr))
    {
        if (error) *error = errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "Compile fluvial erosion CS failed";
        g_fluvialComputeStatus = "GPU Compute shader compile failed";
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = g_fluvialComputeRootSignature.Get();
    psoDesc.CS = {flowBlob->GetBufferPointer(), flowBlob->GetBufferSize()};
    hr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_fluvialFlowAccumulationPso));
    if (FAILED(hr))
    {
        if (error) *error = "Create fluvial flow PSO failed";
        g_fluvialComputeStatus = "GPU Compute flow PSO failed";
        return false;
    }
    psoDesc.CS = {erosionBlob->GetBufferPointer(), erosionBlob->GetBufferSize()};
    hr = g_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&g_fluvialGridErosionPso));
    if (FAILED(hr))
    {
        if (error) *error = "Create fluvial compute PSO failed";
        g_fluvialComputeStatus = "GPU Compute PSO failed";
        return false;
    }

    if (!RunFluvialComputeSmokeTest(error))
    {
        g_fluvialComputeStatus = "GPU Compute dispatch failed";
        return false;
    }

    g_fluvialComputeStatus = "GPU Compute dispatch ready";
    return true;
}

bool RunFluvialComputeGridImmediate(rock::HeightfieldGrid& grid, const rock::FluvialErosionSettings& settings, std::string* error)
{
    std::lock_guard<std::mutex> lock(g_fluvialComputeMutex);
    if (!EnsureFluvialComputePipeline(error))
    {
        return false;
    }

    const UINT resolution = static_cast<UINT>(std::clamp(grid.resolution, 0, 4096));
    const UINT64 cellCount = static_cast<UINT64>(resolution) * static_cast<UINT64>(resolution);
    if (resolution < 3 || grid.heights.size() < cellCount)
    {
        if (error) *error = "Invalid heightfield for GPU Compute";
        return false;
    }

    const UINT64 bufferSize = cellCount * sizeof(float);
    std::vector<float> zeroData(static_cast<size_t>(cellCount), 0.0f);

    const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
    const D3D12_HEAP_PROPERTIES readbackHeap = HeapProperties(D3D12_HEAP_TYPE_READBACK);
    const D3D12_RESOURCE_DESC gpuDesc = BufferResourceDesc(bufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const D3D12_RESOURCE_DESC cpuDesc = BufferResourceDesc(bufferSize);

    ComPtr<ID3D12Resource> heightA;
    ComPtr<ID3D12Resource> heightB;
    ComPtr<ID3D12Resource> maskOut;
    ComPtr<ID3D12Resource> flowA;
    ComPtr<ID3D12Resource> flowB;
    ComPtr<ID3D12Resource> uploadHeights;
    ComPtr<ID3D12Resource> uploadZero;
    ComPtr<ID3D12Resource> uploadOne;
    ComPtr<ID3D12Resource> readbackHeights;
    ComPtr<ID3D12Resource> readbackMask;

    HRESULT hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&heightA));
    if (FAILED(hr)) { if (error) *error = "Create GPU height A failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&heightB));
    if (FAILED(hr)) { if (error) *error = "Create GPU height B failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&maskOut));
    if (FAILED(hr)) { if (error) *error = "Create GPU mask failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&flowA));
    if (FAILED(hr)) { if (error) *error = "Create GPU flow A failed"; return false; }
    hr = g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &gpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&flowB));
    if (FAILED(hr)) { if (error) *error = "Create GPU flow B failed"; return false; }
    hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadHeights));
    if (FAILED(hr)) { if (error) *error = "Create GPU height upload failed"; return false; }
    hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadZero));
    if (FAILED(hr)) { if (error) *error = "Create GPU zero upload failed"; return false; }
    hr = g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadOne));
    if (FAILED(hr)) { if (error) *error = "Create GPU one upload failed"; return false; }
    hr = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackHeights));
    if (FAILED(hr)) { if (error) *error = "Create GPU height readback failed"; return false; }
    hr = g_device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &cpuDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackMask));
    if (FAILED(hr)) { if (error) *error = "Create GPU mask readback failed"; return false; }

    void* mapped = nullptr;
    D3D12_RANGE emptyReadRange{0, 0};
    ThrowIfFailed(uploadHeights->Map(0, &emptyReadRange, &mapped), "Map GPU height upload failed");
    std::memcpy(mapped, grid.heights.data(), bufferSize);
    uploadHeights->Unmap(0, nullptr);
    ThrowIfFailed(uploadZero->Map(0, &emptyReadRange, &mapped), "Map GPU zero upload failed");
    std::memcpy(mapped, zeroData.data(), bufferSize);
    uploadZero->Unmap(0, nullptr);
    std::vector<float> oneData(static_cast<size_t>(cellCount), 1.0f);
    ThrowIfFailed(uploadOne->Map(0, &emptyReadRange, &mapped), "Map GPU one upload failed");
    std::memcpy(mapped, oneData.data(), bufferSize);
    uploadOne->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 15;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    hr = g_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap));
    if (FAILED(hr)) { if (error) *error = "Create GPU descriptor heap failed"; return false; }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = static_cast<UINT>(cellCount);
    uavDesc.Buffer.StructureByteStride = sizeof(float);

    D3D12_CPU_DESCRIPTOR_HANDLE descriptor = descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateUnorderedAccessView(heightA.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(heightB.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(maskOut.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowA.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowB.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(heightA.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(heightB.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(maskOut.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowB.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowA.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(heightB.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(heightA.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(maskOut.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowA.Get(), nullptr, &uavDesc, descriptor);
    descriptor.ptr += g_srvDescriptorSize;
    g_device->CreateUnorderedAccessView(flowB.Get(), nullptr, &uavDesc, descriptor);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create GPU fluvial command allocator failed");
    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create GPU fluvial command list failed");

    commandList->CopyBufferRegion(heightA.Get(), 0, uploadHeights.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(heightB.Get(), 0, uploadHeights.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(maskOut.Get(), 0, uploadZero.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(flowA.Get(), 0, uploadOne.Get(), 0, bufferSize);
    commandList->CopyBufferRegion(flowB.Get(), 0, uploadOne.Get(), 0, bufferSize);

    D3D12_RESOURCE_BARRIER toUav[5]{};
    ID3D12Resource* uavResources[5] = {heightA.Get(), heightB.Get(), maskOut.Get(), flowA.Get(), flowB.Get()};
    for (int i = 0; i < 5; ++i)
    {
        toUav[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav[i].Transition.pResource = uavResources[i];
        toUav[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        toUav[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(5, toUav);

    struct FluvialGridConstants
    {
        UINT resolution;
        UINT cellCount;
        UINT iteration;
        float terrainSizeMeters;
        float erosionStrength;
        float sedimentCapacity;
        float depositionRate;
        float channeling;
        float cellSizeMeters;
        float wearSlope;
        float maxSlope;
        float strengthScale;
    } constants{
        resolution,
        static_cast<UINT>(cellCount),
        0,
        grid.terrainSizeMeters,
        std::clamp(settings.erosionStrength, 0.0f, 2.0f),
        std::clamp(settings.sedimentCapacity, 0.0f, 2.0f),
        std::clamp(settings.depositionRate, 0.0f, 1.0f),
        std::clamp(settings.channeling, 0.0f, 1.0f),
        grid.terrainSizeMeters / static_cast<float>(std::max<UINT>(1, resolution - 1)),
        std::tan(std::clamp(settings.wearAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f),
        std::tan(std::clamp(settings.maxErosionAngleDegrees, 0.0f, 89.0f) * 3.14159265f / 180.0f),
        0.68f
    };

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap.Get()};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(g_fluvialComputeRootSignature.Get());

    const UINT groupCount = (resolution + 7u) / 8u;
    int flowPassCount = std::clamp(static_cast<int>(resolution / 16u), 4, 48);
    if ((flowPassCount % 2) != 0)
    {
        ++flowPassCount;
    }
    commandList->SetPipelineState(g_fluvialFlowAccumulationPso.Get());
    for (int pass = 0; pass < flowPassCount; ++pass)
    {
        constants.iteration = static_cast<UINT>(pass);
        commandList->SetComputeRoot32BitConstants(0, 12, &constants, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE table = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        if ((pass % 2) != 0)
        {
            table.ptr += static_cast<UINT64>(5) * g_srvDescriptorSize;
        }
        commandList->SetComputeRootDescriptorTable(1, table);
        commandList->Dispatch(groupCount, groupCount, 1);

        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &uavBarrier);
    }

    commandList->SetPipelineState(g_fluvialGridErosionPso.Get());
    const int iterationCount = std::clamp(settings.iterations, 1, 200);
    for (int iteration = 0; iteration < iterationCount; ++iteration)
    {
        constants.iteration = static_cast<UINT>(iteration);
        commandList->SetComputeRoot32BitConstants(0, 12, &constants, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE table = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        if ((iteration % 2) != 0)
        {
            table.ptr += static_cast<UINT64>(10) * g_srvDescriptorSize;
        }
        commandList->SetComputeRootDescriptorTable(1, table);
        commandList->Dispatch(groupCount, groupCount, 1);

        D3D12_RESOURCE_BARRIER uavBarrier{};
        uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource = nullptr;
        commandList->ResourceBarrier(1, &uavBarrier);
    }

    ID3D12Resource* finalHeight = (iterationCount % 2) == 0 ? heightA.Get() : heightB.Get();
    D3D12_RESOURCE_BARRIER toCopy[2]{};
    ID3D12Resource* copyResources[2] = {finalHeight, maskOut.Get()};
    for (int i = 0; i < 2; ++i)
    {
        toCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy[i].Transition.pResource = copyResources[i];
        toCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    commandList->ResourceBarrier(2, toCopy);
    commandList->CopyBufferRegion(readbackHeights.Get(), 0, finalHeight, 0, bufferSize);
    commandList->CopyBufferRegion(readbackMask.Get(), 0, maskOut.Get(), 0, bufferSize);
    ThrowIfFailed(commandList->Close(), "Close GPU fluvial command list failed");

    ID3D12CommandList* lists[] = {commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, lists);
    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal GPU fluvial fence failed");
    WaitForFenceValue(fenceValue);

    void* mappedHeights = nullptr;
    void* mappedMask = nullptr;
    D3D12_RANGE readRange{0, static_cast<SIZE_T>(bufferSize)};
    ThrowIfFailed(readbackHeights->Map(0, &readRange, &mappedHeights), "Map GPU height readback failed");
    ThrowIfFailed(readbackMask->Map(0, &readRange, &mappedMask), "Map GPU mask readback failed");
    const float* heightValues = static_cast<const float*>(mappedHeights);
    const float* maskValues = static_cast<const float*>(mappedMask);
    grid.heights.assign(heightValues, heightValues + cellCount);
    grid.mask.assign(maskValues, maskValues + cellCount);
    D3D12_RANGE emptyWriteRange{0, 0};
    readbackHeights->Unmap(0, &emptyWriteRange);
    readbackMask->Unmap(0, &emptyWriteRange);

    g_fluvialComputeStatus = "GPU Compute evaluated heightfield";
    return true;
}

bool RunFluvialComputeGrid(rock::HeightfieldGrid& grid, const rock::FluvialErosionSettings& settings, std::string* error)
{
    if (std::this_thread::get_id() == g_mainThreadId)
    {
        return RunFluvialComputeGridImmediate(grid, settings, error);
    }

    auto request = std::make_shared<FluvialGpuRequest>();
    request->grid = grid;
    request->settings = settings;
    std::future<FluvialGpuRequestResult> future = request->promise.get_future();
    {
        std::lock_guard<std::mutex> lock(g_fluvialGpuRequestMutex);
        g_pendingFluvialGpuRequests.push_back(request);
    }
    g_fluvialComputeStatus = "GPU Compute queued on main thread";

    FluvialGpuRequestResult result = future.get();
    if (!result.success)
    {
        if (error) *error = result.error;
        return false;
    }

    grid = std::move(result.grid);
    return true;
}

void ProcessPendingFluvialGpuRequests()
{
    if (std::this_thread::get_id() != g_mainThreadId)
    {
        return;
    }

    std::vector<std::shared_ptr<FluvialGpuRequest>> requests;
    {
        std::lock_guard<std::mutex> lock(g_fluvialGpuRequestMutex);
        requests.swap(g_pendingFluvialGpuRequests);
    }

    for (const std::shared_ptr<FluvialGpuRequest>& request : requests)
    {
        FluvialGpuRequestResult result;
        result.grid = std::move(request->grid);
        result.success = RunFluvialComputeGridImmediate(result.grid, request->settings, &result.error);
        request->promise.set_value(std::move(result));
    }
}

int EffectiveMeshResolution(int resolution, int lod)
{
    return std::clamp(resolution / (1 << std::clamp(lod, 0, 4)), 16, 512);
}

bool IsTerrainNodeKind(rock::NodeKind kind)
{
    return kind == rock::NodeKind::HeightmapLoad || kind == rock::NodeKind::FluvialErosion;
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
        ProcessPendingFluvialGpuRequests();
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

void EnsureFinalMesh(rock::GraphId outputNodeId)
{
    if (g_graph.Evaluation().dirty)
    {
        EvaluateGraphSync();
    }
    if (g_graph.Evaluation().finalDirty)
    {
        g_graph.EvaluateFinal(outputNodeId);
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
            const D3D12_RESOURCE_DESC desc = Texture2DResourceDesc(
                static_cast<UINT>(width), static_cast<UINT>(height),
                DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            ThrowIfFailed(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearVal, IID_PPV_ARGS(&g_gpuMeshPreview.depthTarget)),
                "Create mesh depth buffer failed");
            D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
            dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
            dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            g_device->CreateDepthStencilView(g_gpuMeshPreview.depthTarget.Get(), &dsvDesc, g_gpuMeshPreview.dsvCpu);
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

void DrawSurfacePointPreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::SdfPreviewStats& sdf)
{
    if (sdf.surfacePoints.empty())
    {
        return;
    }

    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f * g_viewport.zoom;

    for (const rock::SurfacePoint& point : sdf.surfacePoints)
    {
        ImVec2 p = RotatePoint(point.x, point.y, point.z, g_viewport.yaw, g_viewport.pitch);
        const ImVec2 screen(center.x + p.x * scale, center.y + p.y * scale);
        if (screen.x < min.x + 8.0f || screen.x > max.x - 8.0f || screen.y < min.y + 8.0f || screen.y > max.y - 8.0f)
        {
            continue;
        }

        const float nearSurface = std::clamp(1.0f - std::fabs(point.sdf) / std::max(sdf.voxelSize, 0.0001f), 0.0f, 1.0f);
        const ImVec4 base = g_themeManager.AppColor("surfacePoint", ImVec4(0.78f, 0.84f, 0.72f, 0.82f));
        drawList->AddCircleFilled(screen, 1.35f, ColorToU32(ImVec4(base.x, base.y, base.z, std::clamp(base.w + nearSurface * 0.12f, 0.0f, 1.0f))));
    }
}

void DrawSurfaceWirePreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::SdfPreviewStats& sdf)
{
    if (sdf.surfaceSegments.empty())
    {
        return;
    }

    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f * g_viewport.zoom;

    for (const rock::SurfaceSegment& segment : sdf.surfaceSegments)
    {
        ImVec2 a = RotatePoint(segment.ax, segment.ay, segment.az, g_viewport.yaw, g_viewport.pitch);
        ImVec2 b = RotatePoint(segment.bx, segment.by, segment.bz, g_viewport.yaw, g_viewport.pitch);
        a = ImVec2(center.x + a.x * scale, center.y + a.y * scale);
        b = ImVec2(center.x + b.x * scale, center.y + b.y * scale);

        if ((a.x < min.x && b.x < min.x) || (a.x > max.x && b.x > max.x) || (a.y < min.y && b.y < min.y) || (a.y > max.y && b.y > max.y))
        {
            continue;
        }

        drawList->AddLine(a, b, ThemeColor("surfaceWire", ImVec4(0.34f, 0.34f, 0.34f, 0.70f)), 1.0f);
    }
}

ImVec2 ProjectPreviewPoint(float x, float y, float z, const ImVec2& center, float scale)
{
    ImVec2 p = RotatePoint(x, y, z, g_viewport.yaw, g_viewport.pitch);
    return ImVec2(center.x + p.x * scale, center.y + p.y * scale);
}

void DrawViewportGrid3D(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const ImVec2& center, float scale)
{
    const ImU32 minorColor = ThemeColor("viewportGrid", ImVec4(0.24f, 0.27f, 0.25f, 0.35f));
    const ImU32 axisXColor = IM_COL32(210, 76, 76, 210);
    const ImU32 axisZColor = IM_COL32(76, 130, 220, 210);
    constexpr int halfCellCount = 10;
    constexpr float cellSizeMeters = 100.0f;

    drawList->PushClipRect(min, max, true);
    for (int i = -halfCellCount; i <= halfCellCount; ++i)
    {
        const float offset = static_cast<float>(i) * cellSizeMeters;
        const ImVec2 xLineA = ProjectPreviewPoint(-halfCellCount * cellSizeMeters, 0.0f, offset, center, scale);
        const ImVec2 xLineB = ProjectPreviewPoint(halfCellCount * cellSizeMeters, 0.0f, offset, center, scale);
        const ImVec2 zLineA = ProjectPreviewPoint(offset, 0.0f, -halfCellCount * cellSizeMeters, center, scale);
        const ImVec2 zLineB = ProjectPreviewPoint(offset, 0.0f, halfCellCount * cellSizeMeters, center, scale);
        drawList->AddLine(xLineA, xLineB, i == 0 ? axisXColor : minorColor, i == 0 ? 1.8f : 1.0f);
        drawList->AddLine(zLineA, zLineB, i == 0 ? axisZColor : minorColor, i == 0 ? 1.8f : 1.0f);
    }
    drawList->PopClipRect();
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

    auto projectDirection = [](float x, float y, float z) {
        const CameraBasis basis = BuildCameraBasis();
        const Vec3 axis(x, y, z);
        ImVec2 dir(Dot(axis, basis.right), -Dot(axis, basis.up));
        const float length = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (length > 0.0001f)
        {
            dir.x /= length;
            dir.y /= length;
        }
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
        return a.depth < b.depth;
    });

    drawList->PushClipRect(min, max, true);
    drawList->AddCircleFilled(center, 4.0f, IM_COL32(235, 235, 235, 220), 16);
    for (const AxisLine& axis : axes)
    {
        const ImVec2 end(center.x + axis.dir.x * axisLength, center.y + axis.dir.y * axisLength);
        const float thickness = axis.depth >= 0.0f ? 2.6f : 1.8f;
        drawList->AddLine(center, end, axis.color, thickness);
        drawList->AddText(ImVec2(end.x + 8.0f, end.y - 8.0f), axis.color, axis.label);
    }
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
    if (!showSurface && !showWireframe) return true;
    if (!EnsureMeshPreviewPipeline(error)) return false;

    const float viewportWidth = std::max(1.0f, max.x - min.x);
    const float viewportHeight = std::max(1.0f, max.y - min.y);
    const int targetWidth = std::clamp(static_cast<int>(viewportWidth), 160, 960);
    const int targetHeight = std::clamp(static_cast<int>(viewportHeight), 120, 720);
    if (!EnsureMeshPreviewRenderTarget(targetWidth, targetHeight, error)) return false;

    const rock::MeshData& mesh = g_graph.Evaluation().previewMesh;
    const uint64_t currentVersion = g_graph.Evaluation().version;
    const bool meshDirty = (g_gpuMeshPreview.graphVersion != currentVersion || !g_gpuMeshPreview.vertexBuffer);
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
        g_gpuMeshPreview.maskPreview != g_graph.Evaluation().previewShowsMask ||
        g_gpuMeshPreview.lightingMode != g_graph.Settings().preview.lightingMode ||
        g_gpuMeshPreview.sunAzimuthDegrees != g_graph.Settings().preview.sunAzimuthDegrees ||
        g_gpuMeshPreview.sunElevationDegrees != g_graph.Settings().preview.sunElevationDegrees ||
        g_gpuMeshPreview.sunIntensity != g_graph.Settings().preview.sunIntensity ||
        g_gpuMeshPreview.ambientStrength != g_graph.Settings().preview.ambientStrength ||
        g_gpuMeshPreview.shadowStrength != g_graph.Settings().preview.shadowStrength ||
        g_gpuMeshPreview.shadowBias != g_graph.Settings().preview.shadowBias ||
        g_gpuMeshPreview.pbrAlbedo != g_graph.Settings().preview.pbrAlbedo ||
        g_gpuMeshPreview.colorState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (!meshDirty && !viewportDirty) return true;

    try
    {
        if (meshDirty)
        {
            UpdateMeshPreviewBuffers(mesh);
            g_gpuMeshPreview.graphVersion = currentVersion;
        }
        if (g_gpuMeshPreview.vertexCount == 0)
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
        for (const rock::MeshVertex& vertex : mesh.vertices)
        {
            boundsMin.x = std::min(boundsMin.x, vertex.x);
            boundsMin.y = std::min(boundsMin.y, vertex.y);
            boundsMin.z = std::min(boundsMin.z, vertex.z);
            boundsMax.x = std::max(boundsMax.x, vertex.x);
            boundsMax.y = std::max(boundsMax.y, vertex.y);
            boundsMax.z = std::max(boundsMax.z, vertex.z);
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
        vbv.BufferLocation = g_gpuMeshPreview.vertexBuffer->GetGPUVirtualAddress();
        vbv.SizeInBytes    = g_gpuMeshPreview.vertexCount * static_cast<UINT>(sizeof(rock::MeshVertex));
        vbv.StrideInBytes  = static_cast<UINT>(sizeof(rock::MeshVertex));
        commandList->IASetVertexBuffers(0, 1, &vbv);
        commandList->SetGraphicsRootSignature(g_meshPreviewRootSignature.Get());
        commandList->SetGraphicsRoot32BitConstants(0, sizeof(constants) / 4, &constants, 0);

        if (constants.shadowEnabled > 0.5f && showSurface && g_gpuMeshPreview.triIndexCount > 0)
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
        ID3D12DescriptorHeap* descriptorHeaps[] = {g_srvHeap.Get()};
        commandList->SetDescriptorHeaps(1, descriptorHeaps);
        commandList->SetGraphicsRootDescriptorTable(1, g_gpuMeshPreview.shadowSrvGpu);

        if (showSurface && g_gpuMeshPreview.triIndexCount > 0)
        {
            D3D12_INDEX_BUFFER_VIEW ibv{g_gpuMeshPreview.indexBuffer->GetGPUVirtualAddress(), g_gpuMeshPreview.triIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            commandList->SetPipelineState(g_meshPreviewSurfacePso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            commandList->DrawIndexedInstanced(g_gpuMeshPreview.triIndexCount, 1, 0, 0, 0);
        }
        if (showWireframe && g_gpuMeshPreview.edgeIndexCount > 0)
        {
            D3D12_INDEX_BUFFER_VIEW ibv{g_gpuMeshPreview.edgeIndexBuffer->GetGPUVirtualAddress(), g_gpuMeshPreview.edgeIndexCount * sizeof(UINT), DXGI_FORMAT_R32_UINT};
            commandList->IASetIndexBuffer(&ibv);
            commandList->SetPipelineState(g_meshPreviewWirePso.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
            commandList->DrawIndexedInstanced(g_gpuMeshPreview.edgeIndexCount, 1, 0, 0, 0);
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
        g_gpuMeshPreview.maskPreview   = g_graph.Evaluation().previewShowsMask;
        g_gpuMeshPreview.lightingMode  = g_graph.Settings().preview.lightingMode;
        g_gpuMeshPreview.sunAzimuthDegrees = g_graph.Settings().preview.sunAzimuthDegrees;
        g_gpuMeshPreview.sunElevationDegrees = g_graph.Settings().preview.sunElevationDegrees;
        g_gpuMeshPreview.sunIntensity = g_graph.Settings().preview.sunIntensity;
        g_gpuMeshPreview.ambientStrength = g_graph.Settings().preview.ambientStrength;
        g_gpuMeshPreview.shadowStrength = g_graph.Settings().preview.shadowStrength;
        g_gpuMeshPreview.shadowBias = g_graph.Settings().preview.shadowBias;
        g_gpuMeshPreview.pbrAlbedo = g_graph.Settings().preview.pbrAlbedo;
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

void DrawViewportCube(const ImVec2& min, const ImVec2& max, float timeSeconds)
{
    (void)timeSeconds;
    UpdateViewportInteraction(min, max);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.20f * g_viewport.zoom;

    const std::array<float, 3>& viewportBackground = g_graph.Settings().preview.viewportBackground;
    drawList->AddRectFilled(min, max, ColorToU32(ImVec4(viewportBackground[0], viewportBackground[1], viewportBackground[2], 1.0f)));
    if (g_graph.Settings().preview.showGrid)
    {
        DrawViewportGrid3D(drawList, min, max, center, scale);
    }

    if (g_ui.meshPreview)
    {
        const rock::PreviewSettings& preview = g_graph.Settings().preview;
        DrawGpuMeshPreview(drawList, min, max, g_graph.Evaluation().previewMesh,
                           preview.showSurface, preview.showWireframe);
        if (preview.showPoints && !g_graph.Evaluation().previewIsHeightmap)
        {
            DrawSurfacePointPreview(drawList, min, max, g_graph.Evaluation().previewSdf);
        }
    }

    const std::string title = g_graph.Evaluation().previewShowsMask
        ? "Fluvial Mask Preview"
        : (g_graph.Evaluation().previewIsHeightmap
            ? "Heightmap Preview"
            : "SDF Preview: " + std::string(rock::ToString(g_graph.Preview())));
    drawList->AddText(ImVec2(min.x + 16.0f, min.y + 14.0f), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), title.c_str());
    drawList->AddText(ImVec2(min.x + 16.0f, min.y + 36.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), "Right-handed, Y-up, 100 m cells");
    char fpsText[32]{};
    std::snprintf(fpsText, sizeof(fpsText), "FPS %.1f", ImGui::GetIO().Framerate);
    const ImVec2 fpsSize = ImGui::CalcTextSize(fpsText);
    const ImVec2 fpsPadding(9.0f, 5.0f);
    const ImVec2 fpsMax(max.x - 14.0f, min.y + 14.0f + fpsSize.y + fpsPadding.y * 2.0f);
    const ImVec2 fpsMin(fpsMax.x - fpsSize.x - fpsPadding.x * 2.0f, min.y + 14.0f);
    drawList->AddRectFilled(fpsMin, fpsMax, IM_COL32(8, 10, 10, 168), 4.0f);
    drawList->AddRect(fpsMin, fpsMax, ThemeColor("border", ImVec4(0.20f, 0.23f, 0.22f, 0.70f)), 4.0f);
    drawList->AddText(ImVec2(fpsMin.x + fpsPadding.x, fpsMin.y + fpsPadding.y), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), fpsText);
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
    const rock::MeshData& mesh = evaluation.previewMesh;
    const int gridResolution = static_cast<int>(std::lround(std::sqrt(static_cast<double>(mesh.vertices.size()))));
    const bool canDrawMap = evaluation.previewIsHeightmap &&
        gridResolution >= 2 &&
        static_cast<size_t>(gridResolution * gridResolution) == mesh.vertices.size();
    const bool maskPreview = evaluation.previewShowsMask;
    const std::string title = maskPreview ? "2D View: Fluvial Mask" : "2D View: Heightmap";
    drawList->AddText(ImVec2(min.x + 16.0f, min.y + 14.0f), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), title.c_str());

    if (!canDrawMap)
    {
        drawList->AddText(ImVec2(min.x + 16.0f, min.y + 42.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), "Select a heightmap or mask output to inspect it as a 2D map.");
        return;
    }

    float minHeight = std::numeric_limits<float>::max();
    float maxHeight = std::numeric_limits<float>::lowest();
    for (const rock::MeshVertex& vertex : mesh.vertices)
    {
        minHeight = std::min(minHeight, vertex.y);
        maxHeight = std::max(maxHeight, vertex.y);
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

    const int samples = std::clamp(gridResolution, 2, 256);
    const float cellSize = mapSize / static_cast<float>(samples);
    for (int z = 0; z < samples; ++z)
    {
        const int srcZ = samples > 1 ? static_cast<int>(std::lround(static_cast<float>(z) * static_cast<float>(gridResolution - 1) / static_cast<float>(samples - 1))) : 0;
        for (int x = 0; x < samples; ++x)
        {
            const int srcX = samples > 1 ? static_cast<int>(std::lround(static_cast<float>(x) * static_cast<float>(gridResolution - 1) / static_cast<float>(samples - 1))) : 0;
            const rock::MeshVertex& vertex = mesh.vertices[static_cast<size_t>(srcZ * gridResolution + srcX)];
            const float value = maskPreview ? vertex.mask : (vertex.y - minHeight) / heightRange;
            const ImVec2 cellMin(mapMin.x + static_cast<float>(x) * cellSize, mapMin.y + static_cast<float>(z) * cellSize);
            const ImVec2 cellMax(mapMin.x + static_cast<float>(x + 1) * cellSize + 0.5f, mapMin.y + static_cast<float>(z + 1) * cellSize + 0.5f);
            drawList->AddRectFilled(cellMin, cellMax, MapPreviewColor(value, maskPreview));
        }
    }
    drawList->AddRect(mapMin, mapMax, ThemeColor("border", ImVec4(0.20f, 0.23f, 0.22f, 0.85f)));
    drawList->PopClipRect();

    char info[128]{};
    if (maskPreview)
    {
        std::snprintf(info, sizeof(info), "%d x %d samples / zoom %.2fx", samples, samples, g_mapViewport.zoom);
    }
    else
    {
        std::snprintf(info, sizeof(info), "%d x %d samples / zoom %.2fx / height %.2f m to %.2f m", samples, samples, g_mapViewport.zoom, minHeight, maxHeight);
    }
    drawList->AddText(ImVec2(min.x + 16.0f, max.y - 28.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), info);
}

ImVec4 NodeAccentColor(rock::NodeKind kind)
{
    switch (kind)
    {
    case rock::NodeKind::PrimitiveSdf:
        return ImVec4(0.53f, 0.71f, 0.61f, 1.0f);
    case rock::NodeKind::HeightmapLoad:
        return ImVec4(0.38f, 0.62f, 0.53f, 1.0f);
    case rock::NodeKind::FluvialErosion:
        return ImVec4(0.36f, 0.58f, 0.78f, 1.0f);
    case rock::NodeKind::NoiseWarp:
        return ImVec4(0.46f, 0.65f, 0.76f, 1.0f);
    case rock::NodeKind::CrackField:
        return ImVec4(0.77f, 0.61f, 0.43f, 1.0f);
    case rock::NodeKind::OutputMesh:
        return ImVec4(0.70f, 0.52f, 0.62f, 1.0f);
    default:
        return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    }
}

ImVec2 InitialNodePosition(rock::NodeKind kind)
{
    switch (kind)
    {
    case rock::NodeKind::PrimitiveSdf:
        return ImVec2(40.0f, 64.0f);
    case rock::NodeKind::HeightmapLoad:
        return ImVec2(40.0f, 240.0f);
    case rock::NodeKind::FluvialErosion:
        return ImVec2(320.0f, 240.0f);
    case rock::NodeKind::NoiseWarp:
        return ImVec2(320.0f, 64.0f);
    case rock::NodeKind::CrackField:
        return ImVec2(600.0f, 64.0f);
    case rock::NodeKind::OutputMesh:
        return ImVec2(880.0f, 64.0f);
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

ImVec4 PinTypeColor(rock::ValueType valueType)
{
    switch (valueType)
    {
    case rock::ValueType::HeightField:
        return ImVec4(0.70f, 0.93f, 0.78f, 1.0f);
    case rock::ValueType::Mask:
        return ImVec4(0.82f, 0.64f, 0.36f, 1.0f);
    case rock::ValueType::SdfGrid:
        return ImVec4(0.58f, 0.72f, 0.86f, 1.0f);
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
    ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(12.0f, 10.0f, 12.0f, 10.0f));
    ed::PushStyleVar(ed::StyleVar_NodeRounding, 8.0f);
    ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, 1.0f);
    ed::PushStyleVar(ed::StyleVar_SelectedNodeBorderWidth, 1.8f);
    ed::PushStyleColor(ed::StyleColor_NodeBg, ImVec4(0.080f, 0.080f, 0.080f, 0.98f));
    ed::PushStyleColor(ed::StyleColor_NodeBorder, ImVec4(0.22f, 0.22f, 0.22f, 1.0f));
    ed::PushStyleColor(ed::StyleColor_SelNodeBorder, accent);

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

    if (!node.inputs.empty())
    {
        ImGui::SetCursorPos(ImVec2(rowStartX, rowY));
        ed::BeginPin(ed::PinId(node.inputs.front().id), ed::PinKind::Input);
        DrawRoundPin(node.inputs.front());
        ed::EndPin();
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowY + 2.0f);
        ImGui::TextColored(PinColor(node.inputs.front()), "%s", node.inputs.front().label.c_str());
    }
    else
    {
        ImGui::SetCursorPos(ImVec2(rowStartX, rowY));
        ImGui::Dummy(ImVec2(14.0f, 20.0f));
    }

    for (size_t outputIndex = 0; outputIndex < node.outputs.size(); ++outputIndex)
    {
        const rock::Pin& output = node.outputs[outputIndex];
        const bool outputSelected = g_graph.Evaluation().previewPinId == output.id;
        const float outputY = rowY + static_cast<float>(outputIndex) * 24.0f;
        const float labelWidth = ImGui::CalcTextSize(output.label.c_str()).x + (outputSelected ? 2.0f : 0.0f);
        ImGui::SetCursorPos(ImVec2(rowStartX + nodeWidth - labelWidth - 22.0f, outputY + 2.0f));
        const ImVec4 outputColor = PinColor(output);
        ImGui::TextColored(outputColor, "%s", output.label.c_str());
        if (outputSelected)
        {
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 textMin = ImGui::GetItemRectMin();
            drawList->AddText(ImVec2(textMin.x + 0.7f, textMin.y), ColorToU32(outputColor), output.label.c_str());
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            g_pendingPreviewPinId = output.id;
        }
        ImGui::SameLine();
        ImGui::SetCursorPosY(outputY);
        ed::BeginPin(ed::PinId(output.id), ed::PinKind::Output);
        DrawRoundPin(output);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            g_pendingPreviewPinId = output.id;
        }
        ed::EndPin();
    }
    ImGui::Dummy(ImVec2(nodeWidth, std::max(4.0f, static_cast<float>(node.outputs.size()) * 24.0f - 20.0f)));

    ed::EndNode();
    ed::PopStyleColor(3);
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
            newMutableNode->primitive = clipboardNode.node.primitive;
            newMutableNode->noise = clipboardNode.node.noise;
            newMutableNode->crack = clipboardNode.node.crack;
            newMutableNode->outputMesh = clipboardNode.node.outputMesh;
            newMutableNode->heightmap = clipboardNode.node.heightmap;
            newMutableNode->fluvialErosion = clipboardNode.node.fluvialErosion;
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
    g_lastFinalOutputNodeId = 0;
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
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
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
            else
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
        ed::ClearSelection();
        bool append = false;
        g_selectedNodeId = 0;
        for (const rock::GraphId nodeId : g_pendingSelectedNodeIds)
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
                    if (g_lastFinalOutputNodeId == nodeId)
                    {
                        g_lastFinalOutputNodeId = 0;
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
                g_lastFinalOutputNodeId = 0;
                g_projectStatus = "Added " + std::string(rock::ToString(kind));
                EvaluateGraph();
            }
        };
        addNodeMenuItem(rock::NodeKind::HeightmapLoad);
        addNodeMenuItem(rock::NodeKind::FluvialErosion);
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::NodeId selectedNodes[1];
    bool handledPreviewPinClick = false;
    if (g_pendingPreviewPinId != 0)
    {
        const rock::GraphId pinId = g_pendingPreviewPinId;
        g_pendingPreviewPinId = 0;
        handledPreviewPinClick = true;
        if (g_graph.SetPreviewPin(pinId))
        {
            if (const rock::Pin* pin = g_graph.FindPin(pinId))
            {
                g_selectedNodeId = pin->nodeId;
                g_pendingSelectedNodeIds = {pin->nodeId};
            }
            if (g_graph.Evaluation().dirty)
            {
                EvaluateGraph();
            }
        }
    }
    if (!handledPreviewPinClick && ed::GetSelectedNodes(selectedNodes, 1) > 0)
    {
        const rock::GraphId selectedNodeId = ToGraphId(selectedNodes[0].Get());
        g_selectedNodeId = selectedNodeId;
        if (const rock::Node* selectedNode = g_graph.FindNode(selectedNodeId))
        {
            if (g_graph.SetPreviewNode(selectedNode->id))
            {
                EvaluateGraph();
            }
            g_lastFinalOutputNodeId = 0;
        }
    }
    else
    {
        g_selectedNodeId = 0;
        g_lastFinalOutputNodeId = 0;
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
    ed::PopStyleColor(2);
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

void DrawPropertyLabel(const char* label, const char* tooltip = nullptr, bool modified = false)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (modified)
    {
        const ImVec2 textMin = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(textMin.x + 0.7f, textMin.y), ImGui::GetColorU32(ImGuiCol_Text), label);
    }
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

bool DrawResetToDefaultButton(const char* id)
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
    ImGui::GetWindowDrawList()->AddText(font, iconFontSize, iconPos, ImGui::GetColorU32(ImGuiCol_Text), resetIcon);
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("既定値に戻す");
    }
    ImGui::PopID();
    return pressed;
}

bool DrawPropertyFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, float defaultValue, const char* dirtyReason, bool recordUndo = true, const char* tooltip = nullptr)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, FloatDiffersFromDefault(*value, defaultValue));
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
    if (ImGui::SliderFloat("##slider", value, minValue, maxValue, "%.3f"))
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
    if (ImGui::InputFloat("##number", value, 0.0f, 0.0f, "%.3f"))
    {
        *value = std::clamp(*value, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
    }
    if (recordUndo && ImGui::IsItemActivated())
    {
        BeginPropertyUndoEdit();
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();
    if (DrawResetToDefaultButton("reset"))
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
    DrawPropertyLabel(label, tooltip, *value != defaultValue);
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
    if (DrawResetToDefaultButton("reset"))
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

bool DrawPropertyBoolRow(const char* label, const char* id, bool* value, const char* dirtyReason, const char* tooltip = nullptr, bool defaultValue = false)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    DrawPropertyLabel(label, tooltip, *value != defaultValue);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const bool changed = ImGui::Checkbox("##value", value);
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
    DrawPropertyLabel(label, nullptr, ColorDiffersFromDefault(value, defaultValue));
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
    if (DrawResetToDefaultButton("reset"))
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
    if (DrawResetToDefaultButton("reset"))
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
        editableNode->heightmap.simulationResolution = std::clamp(editableNode->heightmap.simulationResolution, 2, 2048);

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
        if (DrawPropertyIntRow("Simulation Resolution", "HeightmapSimulationResolution", &editableNode->heightmap.simulationResolution, 2, 2048, rock::HeightmapLoadSettings{}.simulationResolution, "Heightmap simulation resolution changed", true, "侵食や地形処理に使う内部ハイトフィールド解像度です。表示設定の Resolution はメッシュ表示の細かさだけを変更します。"))
        {
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::FluvialErosion && ImGui::BeginTable("FluvialErosionRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 210.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        rock::FluvialErosionSettings& erosion = editableNode->fluvialErosion;
        const auto scaleAverage = [&](size_t first, size_t second) {
            return (erosion.levelStrengths[first] + erosion.levelStrengths[second]) * 0.5f;
        };
        const auto setScalePair = [&](size_t first, size_t second, float value) {
            erosion.levelStrengths[first] = value;
            erosion.levelStrengths[second] = value;
        };
        erosion.featureSize = std::clamp(erosion.featureSize, 1.0f, 64.0f);
        erosion.iterations = std::clamp(erosion.iterations, 0, 200);
        erosion.channelLength = std::clamp(erosion.channelLength, 1.0f, 1024.0f);
        erosion.erosionStrength = std::clamp(erosion.erosionStrength, 0.0f, 1.0f);
        erosion.channeling = std::clamp(erosion.channeling, 0.0f, 1.0f);
        erosion.friction = std::clamp(erosion.friction, 0.0f, 1.0f);
        erosion.wearAngleDegrees = std::clamp(erosion.wearAngleDegrees, 0.0f, 90.0f);
        erosion.depositAngleDegrees = std::clamp(erosion.depositAngleDegrees, 0.0f, 90.0f);
        erosion.maxErosionAngleDegrees = std::clamp(erosion.maxErosionAngleDegrees, 0.0f, 90.0f);
        erosion.erosionGranularity = std::clamp(erosion.erosionGranularity, 1.0f, 100.0f);
        erosion.sedimentVelocity = std::clamp(erosion.sedimentVelocity, 0.0f, 2.0f);
        erosion.sedimentCapacity = std::clamp(erosion.sedimentCapacity, 0.0f, 2.0f);
        erosion.depositionRate = std::clamp(erosion.depositionRate, 0.0f, 1.0f);
        for (float& levelStrength : erosion.levelStrengths)
        {
            levelStrength = std::clamp(levelStrength, 0.0f, 2.0f);
        }
        erosion.seed = std::clamp(erosion.seed, 0, 999999);

        int backend = static_cast<int>(erosion.backend);
        if (DrawPropertyComboRow("Backend", "FluvialBackend", &backend, "CPU Reference\0GPU Compute (planned)\0", "浸食計算の実行バックエンドです。現時点では GPU Compute は準備中で、CPU Reference へフォールバックします。", static_cast<int>(rock::FluvialErosionSettings{}.backend)))
        {
            erosion.backend = static_cast<rock::FluvialBackend>(std::clamp(backend, 0, 1));
            g_graph.MarkDirty("Fluvial backend changed");
            EvaluateGraph();
        }
        if (erosion.backend == rock::FluvialBackend::GpuCompute)
        {
            std::string computeError;
            const bool computeReady = EnsureFluvialComputePipeline(&computeError);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(computeReady ? ImVec4(0.50f, 0.78f, 0.50f, 1.0f) : ImVec4(0.90f, 0.45f, 0.36f, 1.0f), "%s", g_fluvialComputeStatus.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.36f, 1.0f), "実行接続は準備中です。現在は CPU Reference で計算します。");
            if (!computeReady && !computeError.empty())
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", computeError.c_str());
            }
        }
        if (DrawPropertyIntRow("Iterations", "FluvialIterations", &erosion.iterations, 0, 200, rock::FluvialErosionSettings{}.iterations, "Fluvial iterations changed", true, "侵食シミュレーションの反復回数です。増やすほど効果が強くなりますが計算時間も増えます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Channel Length (m)", "FluvialChannelLength", &erosion.channelLength, 1.0f, 1024.0f, rock::FluvialErosionSettings{}.channelLength, "Fluvial channel length changed", true, "水の流れが影響する距離です。大きいほど長い流路が形成されやすくなります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyPercentRow("Erosion Strength (%)", "FluvialErosionStrength", &erosion.erosionStrength, 0.0f, 1.0f, rock::FluvialErosionSettings{}.erosionStrength, "Fluvial erosion strength changed", "地形を削る強さです。値を上げると谷や溝が深くなります。"))
        {
            EvaluateGraph();
        }
        float largeScaleStrength = scaleAverage(0, 1);
        if (DrawPropertyPercentRow("Large Scale (%)", "FluvialLargeScale", &largeScaleStrength, 0.0f, 2.0f, (rock::FluvialErosionSettings{}.levelStrengths[0] + rock::FluvialErosionSettings{}.levelStrengths[1]) * 0.5f, "Fluvial large scale strength changed", "大きな谷筋や流域に効く低解像度レベルの強度です。"))
        {
            setScalePair(0, 1, largeScaleStrength);
            EvaluateGraph();
        }
        float mediumScaleStrength = scaleAverage(2, 3);
        if (DrawPropertyPercentRow("Medium Scale (%)", "FluvialMediumScale", &mediumScaleStrength, 0.0f, 2.0f, (rock::FluvialErosionSettings{}.levelStrengths[2] + rock::FluvialErosionSettings{}.levelStrengths[3]) * 0.5f, "Fluvial medium scale strength changed", "中規模の支流や斜面のまとまりに効くレベルの強度です。"))
        {
            setScalePair(2, 3, mediumScaleStrength);
            EvaluateGraph();
        }
        float detailStrength = scaleAverage(4, 5);
        if (DrawPropertyPercentRow("Detail Scale (%)", "FluvialDetailScale", &detailStrength, 0.0f, 2.0f, (rock::FluvialErosionSettings{}.levelStrengths[4] + rock::FluvialErosionSettings{}.levelStrengths[5]) * 0.5f, "Fluvial detail scale strength changed", "細かいリルや表面ディテールに効く高解像度レベルの強度です。"))
        {
            setScalePair(4, 5, detailStrength);
            EvaluateGraph();
        }
        if (DrawPropertyPercentRow("Sediment Capacity (%)", "FluvialSedimentCapacity", &erosion.sedimentCapacity, 0.0f, 2.0f, rock::FluvialErosionSettings{}.sedimentCapacity, "Fluvial sediment capacity changed", "粒子が保持できる土砂量です。高いほど下流まで削った土砂を運びやすくなります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyPercentRow("Deposition Rate (%)", "FluvialDepositionRate", &erosion.depositionRate, 0.0f, 1.0f, rock::FluvialErosionSettings{}.depositionRate, "Fluvial deposition rate changed", "土砂を堆積させる速さです。高いほど谷底や緩斜面に土砂が残りやすくなります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyIntRow("Seed", "FluvialSeed", &erosion.seed, 0, 999999, rock::FluvialErosionSettings{}.seed, "Fluvial seed changed", true, "侵食パターンの乱数シードです。同じ値なら同じ結果を再現できます。"))
        {
            EvaluateGraph();
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("Advanced");
        ImGui::TableSetColumnIndex(1);
        ImGui::SeparatorText("Advanced");
        if (DrawPropertyBoolRow("Use Advanced", "FluvialUseAdvanced", &erosion.useAdvancedParameters, "Fluvial advanced mode changed", "有効にすると下の詳細パラメータを計算に使います。無効時は安定した内部デフォルトを使います。", rock::FluvialErosionSettings{}.useAdvancedParameters))
        {
            EvaluateGraph();
        }
        ImGui::BeginDisabled(!erosion.useAdvancedParameters);
        if (DrawPropertyFloatRow("Feature Size (m)", "FluvialFeatureSize", &erosion.featureSize, 1.0f, 64.0f, rock::FluvialErosionSettings{}.featureSize, "Fluvial feature size changed", true, "侵食で扱う地形特徴の大きさです。大きいほど広い起伏をなだらかに処理します。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyPercentRow("Channeling (%)", "FluvialChanneling", &erosion.channeling, 0.0f, 1.0f, rock::FluvialErosionSettings{}.channeling, "Fluvial channeling changed", "流れを細い水路へ集中させる度合いです。高いほど筋状の侵食が出やすくなります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyPercentRow("Friction (%)", "FluvialFriction", &erosion.friction, 0.0f, 1.0f, rock::FluvialErosionSettings{}.friction, "Fluvial friction changed", "水流の勢いを抑える度合いです。高いほど侵食が落ち着き、短い流れになります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Wear Angle (deg)", "FluvialWearAngle", &erosion.wearAngleDegrees, 0.0f, 90.0f, rock::FluvialErosionSettings{}.wearAngleDegrees, "Fluvial wear angle changed", true, "削れ始める斜面角度の目安です。低いほど緩い斜面にも侵食が入りやすくなります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Deposit Angle (deg)", "FluvialDepositAngle", &erosion.depositAngleDegrees, 0.0f, 90.0f, rock::FluvialErosionSettings{}.depositAngleDegrees, "Fluvial deposit angle changed", true, "土砂が堆積しやすくなる斜面角度の目安です。低いほど平坦部に土砂が残りやすくなります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Max Erosion Angle (deg)", "FluvialMaxErosionAngle", &erosion.maxErosionAngleDegrees, 0.0f, 90.0f, rock::FluvialErosionSettings{}.maxErosionAngleDegrees, "Fluvial max erosion angle changed", true, "侵食を許可する最大斜面角度です。急すぎる斜面への影響を抑えるときに使います。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Granularity (%)", "FluvialGranularity", &erosion.erosionGranularity, 1.0f, 100.0f, rock::FluvialErosionSettings{}.erosionGranularity, "Fluvial granularity changed", true, "侵食パターンの細かさです。高いほど細かい溝やノイズ感が出やすくなります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Sediment Velocity (x)", "FluvialSedimentVelocity", &erosion.sedimentVelocity, 0.0f, 2.0f, rock::FluvialErosionSettings{}.sedimentVelocity, "Fluvial sediment velocity changed", true, "削られた土砂が下流へ運ばれる強さです。高いほど堆積位置が流れ方向へ伸びます。"))
        {
            EvaluateGraph();
        }
        for (size_t level = 0; level < erosion.levelStrengths.size(); ++level)
        {
            const std::string label = std::format("Level {} Strength (%)", level + 1);
            const std::string id = std::format("FluvialLevelStrength{}", level + 1);
            const std::string tooltip = std::format(
                "スケール別の侵食強度です。Level {} は{}に効きます。",
                level + 1,
                level < 2 ? "大きな谷筋や流域" : (level < 4 ? "中規模の支流" : "細かいリルや表面ディテール"));
            if (DrawPropertyPercentRow(label.c_str(), id.c_str(), &erosion.levelStrengths[level], 0.0f, 2.0f, rock::FluvialErosionSettings{}.levelStrengths[level], "Fluvial level strength changed", tooltip.c_str()))
            {
                EvaluateGraph();
            }
        }
        ImGui::EndDisabled();

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::PrimitiveSdf && ImGui::BeginTable("PrimitivePropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        int primitive = static_cast<int>(editableNode->primitive.kind);
        if (DrawPropertyComboRow("Primitive", "Primitive", &primitive, "Sphere\0Box\0Capsule\0Ellipsoid\0Rock Blob\0", "生成する基本形状です。現在は古い SDF 系ノードとの互換用です。", static_cast<int>(rock::PrimitiveSettings{}.kind)))
        {
            PushUndoSnapshot();
            editableNode->primitive.kind = static_cast<rock::PrimitiveKind>(primitive);
            g_graph.MarkDirty("Primitive changed");
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::NoiseWarp && ImGui::BeginTable("NoisePropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (DrawPropertyFloatRow("Amplitude", "NoiseAmplitude", &editableNode->noise.amplitude, 0.0f, 2.0f, rock::NoiseSettings{}.amplitude, "Noise amplitude changed", true, "ノイズ変形の強さです。値を上げるほど形状が大きく歪みます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Frequency", "NoiseFrequency", &editableNode->noise.frequency, 0.1f, 12.0f, rock::NoiseSettings{}.frequency, "Noise frequency changed", true, "ノイズの細かさです。値を上げるほど細かい変化になります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyIntRow("Octaves", "NoiseOctaves", &editableNode->noise.octaves, 1, 8, rock::NoiseSettings{}.octaves, "Noise octaves changed", true, "重ねるノイズ階層の数です。増やすとディテールが増えます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyIntRow("Seed", "NoiseSeed", &editableNode->noise.seed, 0, 999999, rock::NoiseSettings{}.seed, "Noise seed changed", true, "ノイズパターンの乱数シードです。同じ値なら同じ結果を再現できます。"))
        {
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::CrackField && ImGui::BeginTable("CrackPropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (DrawPropertyFloatRow("Width", "CrackWidth", &editableNode->crack.width, 0.0f, 0.2f, rock::CrackSettings{}.width, "Crack width changed", true, "亀裂の太さです。値を上げるほど広い割れ目になります。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Depth", "CrackDepth", &editableNode->crack.depth, 0.0f, 1.0f, rock::CrackSettings{}.depth, "Crack depth changed", true, "亀裂の深さです。値を上げるほど強く彫り込まれます。"))
        {
            EvaluateGraph();
        }
        if (DrawPropertyFloatRow("Roughness", "CrackRoughness", &editableNode->crack.roughness, 0.0f, 1.0f, rock::CrackSettings{}.roughness, "Crack roughness changed", true, "亀裂境界の荒さです。高いほど不規則な割れ目になります。"))
        {
            EvaluateGraph();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::OutputMesh)
    {
        rock::OutputMeshSettings* outputMesh = g_graph.FindOutputMeshSettings(selectedNode->id);
        if (outputMesh != nullptr && ImGui::BeginTable("OutputMeshRows", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

            if (DrawPropertyIntRow("Resolution", "OutputMeshResolution", &outputMesh->resolution, 16, 512, rock::OutputMeshSettings{}.resolution, "Output mesh resolution changed", true, "出力メッシュの分割数です。高いほど細かくなりますが処理負荷も増えます。"))
            {
                EvaluateGraph();
            }
            if (DrawPropertyIntRow("LOD", "OutputMeshLod", &outputMesh->lod, 0, 4, rock::OutputMeshSettings{}.lod, "Output mesh LOD changed", true, "表示や出力時の簡略化レベルです。値を上げるほど軽くなりますがディテールは減ります。"))
            {
                EvaluateGraph();
            }
            if (DrawPropertyFloatRow("Iso Value", "OutputMeshIsoValue", &outputMesh->isoValue, -0.2f, 0.2f, rock::OutputMeshSettings{}.isoValue, "Output mesh iso value changed", true, "SDF 互換用の等値面しきい値です。ハイトフィールドの通常出力ではほぼ使いません。"))
            {
                EvaluateGraph();
            }

            ImGui::EndTable();
        }
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

        if (DrawPropertyBoolRow("Mesh Preview", "DisplayMeshPreview", &g_ui.meshPreview, "Mesh preview visibility changed", nullptr, UiState{}.meshPreview))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyIntRow("Resolution", "DisplayPreviewResolution", &settings.preview.resolution, 16, 512, rock::PreviewSettings{}.resolution, "Preview resolution changed", false))
        {
            EvaluateGraph();
            SaveAppSettingsSilently();
        }
        if (DrawPropertyIntRow("LOD", "DisplayPreviewLod", &settings.preview.lod, 0, 4, rock::PreviewSettings{}.lod, "Preview LOD changed", false))
        {
            EvaluateGraph();
            SaveAppSettingsSilently();
        }

        if (DrawPropertyBoolRow("Surface", "DisplaySurface", &settings.preview.showSurface, "Surface visibility changed", nullptr, rock::PreviewSettings{}.showSurface))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyBoolRow("Wireframe", "DisplayWireframe", &settings.preview.showWireframe, "Wireframe visibility changed", nullptr, rock::PreviewSettings{}.showWireframe))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyBoolRow("Points", "DisplayPoints", &settings.preview.showPoints, "Surface points visibility changed", nullptr, rock::PreviewSettings{}.showPoints))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyBoolRow("Grid", "DisplayGrid", &settings.preview.showGrid, "Grid visibility changed", nullptr, rock::PreviewSettings{}.showGrid))
        {
            SaveAppSettingsSilently();
        }
        if (DrawPropertyComboRow("Lighting Mode", "DisplayLightingMode", &settings.preview.lightingMode, "Simple\0PBR Preview\0Shadow Debug\0", "3Dビューのライティングモードです。Simple はマスク確認向け、PBR Preview は地形の陰影確認向け、Shadow Debug はシャドウ判定の確認用です。", rock::PreviewSettings{}.lightingMode))
        {
            settings.preview.lightingMode = std::clamp(settings.preview.lightingMode, 0, 2);
            SaveAppSettingsSilently();
        }
        if (settings.preview.lightingMode >= 1)
        {
            ImGui::SeparatorText("PBR Preview");
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
            if (DrawColorRgbRow("Albedo", "DisplayPbrAlbedo", settings.preview.pbrAlbedo, rock::PreviewSettings{}.pbrAlbedo))
            {
                SaveAppSettingsSilently();
            }
        }
        if (DrawColorRgbRow("ビューポート背景色", "ViewportBackgroundColor", settings.preview.viewportBackground, rock::PreviewSettings{}.viewportBackground))
        {
            SaveAppSettingsSilently();
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
    ImGui::TextDisabled("Grid: 20 x 20, 100 m cells");
}

void DrawStatsPanel()
{
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    ImGui::Text("Graph Version: %llu", static_cast<unsigned long long>(evaluation.version));
    ImGui::Text("%s", g_lastEvaluationDuration.c_str());
    ImGui::TextColored(evaluation.dirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f), "%s", evaluation.dirty ? "Dirty" : "Evaluated");
    ImGui::TextColored(evaluation.finalDirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f), "%s", evaluation.finalDirty ? "Terrain Mesh: pending" : "Terrain Mesh: ready");
    ImGui::TextWrapped("%s", evaluation.status.c_str());

    ImGui::SeparatorText("Preview");
    ImGui::Text("Stage: %s", rock::ToString(evaluation.previewStage).data());
    const rock::SdfPreviewStats& previewSdf = evaluation.previewSdf;
    if (!evaluation.previewIsHeightmap && previewSdf.totalVoxels > 0)
    {
        ImGui::Text("Dense SDF: %d^3", previewSdf.resolution);
        ImGui::Text("SDF Range: %.3f / %.3f", previewSdf.minSdf, previewSdf.maxSdf);
        ImGui::Text("Fill: %.1f%%", previewSdf.fillRatio * 100.0f);
        ImGui::Text("Volume: %.3f", previewSdf.estimatedVolume);
        ImGui::Text("Surface Points: %zu", previewSdf.surfacePoints.size());
        ImGui::Text("Surface Lines: %zu", previewSdf.surfaceSegments.size());
        ImGui::Text("Surface Triangles: %zu", previewSdf.surfaceTriangles.size());
    }
    ImGui::SeparatorText("Mesh Topology");
    ImGui::Text("Vertices: %zu", evaluation.previewMesh.vertices.size());
    ImGui::Text("Edges: %zu", evaluation.previewMesh.edges.size());
    ImGui::Text("Triangles: %zu", evaluation.previewMesh.triangles.size());
}

void DrawAssetExportPanel()
{
    const rock::OutputMeshSettings& outputMesh = g_graph.OutputMeshSettingsFor(g_selectedNodeId);
    const int effectiveResolution = std::clamp(outputMesh.resolution / (1 << std::clamp(outputMesh.lod, 0, 4)), 16, 512);
    ImGui::Columns(4, nullptr, false);
    ImGui::TextUnformatted("High mesh");
    ImGui::Text("%s", g_graph.Evaluation().finalDirty ? "needs build" : "ready");
    ImGui::NextColumn();
    ImGui::TextUnformatted("LOD");
    ImGui::Text("%d / output %d^3", outputMesh.lod, effectiveResolution);
    ImGui::NextColumn();
    ImGui::TextUnformatted("Textures");
    ImGui::TextUnformatted("normal / AO later");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Export");
    if (ImGui::Button("Build Mesh"))
    {
        EnsureFinalMesh(g_selectedNodeId);
    }
    ImGui::SameLine();
    if (ImGui::Button("Export OBJ"))
    {
        EnsureFinalMesh(g_selectedNodeId);

        std::string error;
        const std::filesystem::path exportPath = std::filesystem::path("exports") / "terrain_mesh.obj";
        if (rock::ExportMeshObj(g_graph.Evaluation().finalMesh, exportPath, &error))
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
        if (ImGui::BeginMenu("ビルド"))
        {
            if (ImGui::MenuItem("グラフを評価"))
            {
                EvaluateGraph();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("エクスポート"))
        {
            if (ImGui::MenuItem("OBJ"))
            {
                EnsureFinalMesh(g_selectedNodeId);

                std::string error;
                const std::filesystem::path exportPath = std::filesystem::path("exports") / "terrain_mesh.obj";
                if (rock::ExportMeshObj(g_graph.Evaluation().finalMesh, exportPath, &error))
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
    if (g_ui.rightPaneWidth <= 0.0f)
    {
        g_ui.rightPaneWidth = std::clamp(content.x * 0.42f, 480.0f, std::min(820.0f, std::max(paneMinWidth, content.x - paneMinWidth)));
    }
    const float maxRightWidth = std::max(paneMinWidth, content.x - paneMinWidth - mainSplitterWidth);
    g_ui.rightPaneWidth = std::clamp(g_ui.rightPaneWidth, paneMinWidth, maxRightWidth);
    float previewWidth = std::max(paneMinWidth, content.x - g_ui.rightPaneWidth - mainSplitterWidth);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

    const TabHeaderStyle defaultTabStyle;
    DrawViewportTabs(previewWidth, workHeight, timeSeconds, fixedPaneFlags);

    if (DrawVerticalSplitter("MainLayoutSplitter", &previewWidth, content.x, paneMinWidth, paneMinWidth, workHeight))
    {
        SaveAppSettingsSilently();
    }
    g_ui.rightPaneWidth = std::max(paneMinWidth, content.x - previewWidth - mainSplitterWidth);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild("Right Work Column", ImVec2(g_ui.rightPaneWidth, workHeight), false, fixedPaneFlags);
    const float rightColumnHeight = ImGui::GetContentRegionAvail().y;
    constexpr float inspectorSplitterHeight = 7.0f;
    if (g_ui.nodePaneHeight <= 0.0f)
    {
        g_ui.nodePaneHeight = std::clamp(rightColumnHeight * 0.56f, 220.0f, std::max(220.0f, rightColumnHeight - 190.0f));
    }
    g_ui.nodePaneHeight = std::clamp(g_ui.nodePaneHeight, 160.0f, std::max(160.0f, rightColumnHeight - 160.0f - inspectorSplitterHeight));

    DrawNodeNetworkTabs(g_ui.nodePaneHeight, fixedPaneFlags);

    if (DrawHorizontalSplitter("InspectorLayoutSplitter", &g_ui.nodePaneHeight, rightColumnHeight, 160.0f, 160.0f))
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
        rock::SetFluvialGpuEvaluator(RunFluvialComputeGrid);

        ShowWindow(g_hwnd, showCommand);
        UpdateWindow(g_hwnd);
        UpdateWindowTitle();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        LoadJapaneseFont(io);
        g_themeManager.LoadThemes(DataDirectory() / "ui_themes");
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
            ProcessPendingFluvialGpuRequests();
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
        rock::SetFluvialGpuEvaluator(nullptr);
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
