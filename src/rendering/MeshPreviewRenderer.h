#pragma once

#include <array>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <string>

#include <d3d12.h>
#include <imgui.h>
#include <wrl/client.h>

namespace terrain::rendering
{

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
    bool hdrViewportEnabled = false;
    DXGI_FORMAT colorFormat = DXGI_FORMAT_UNKNOWN;
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
    Microsoft::WRL::ComPtr<ID3D12Resource> colorTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> postTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> outputTarget;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> exposureTargets;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> sceneDepthTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> shadowTarget;
    Microsoft::WRL::ComPtr<ID3D12Resource> vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> edgeIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> gridVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> terrainBoundaryLineVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> waterVertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> waterIndexBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE postRtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE outputRtvCpu{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> exposureRtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowDsvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE postSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE postSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE outputSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE outputSrvGpu{};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 2> exposureSrvCpu{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 2> exposureSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE shadowSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE depthSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE depthSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE sceneDepthSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrvGpu{};
    bool srvAllocated = false;
    bool postSrvAllocated = false;
    bool outputSrvAllocated = false;
    bool exposureSrvAllocated = false;
    bool exposureInitialized = false;
    int exposureHistoryIndex = 0;
    bool shadowSrvAllocated = false;
    bool depthSrvAllocated = false;
    bool sceneDepthSrvAllocated = false;
    UINT vertexCount = 0;
    UINT triIndexCount = 0;
    UINT edgeIndexCount = 0;
    UINT gridVertexCount = 0;
    UINT terrainBoundaryLineVertexCount = 0;
    UINT waterIndexCount = 0;
    UINT waterVertexCount = 0;
    uint64_t waterHeightfieldVersion = UINT64_MAX;
    uint64_t terrainBoundaryLineUploadKey = UINT64_MAX;
    int gridCellCount = 0;
    float gridCellSizeMeters = 0.0f;
    bool waterEnabled = false;
    float waterLevelMeters = 0.0f;
    float waterOpacity = 0.0f;
    std::array<float, 3> waterColor = {};
    float waterTerrainSizeMeters = 0.0f;
    float waterWavesScale = 24.0f;
    float waterRefractiveIndex = 1.33f;
    float waterFresnelPower = 5.0f;
    float waterRefractionStrength = 0.25f;
    bool waterAnimationEnabled = true;
    float waterReflectionStrength = 1.0f;
    bool waterSsrEnabled = false;
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
    float cloudLoopPhase = 0.0f;
    float cloudWindDirectionDegrees = 0.0f;
    float cloudWindSpeed = 0.0f;
    int cloudQualitySamples = 0;
    float cloudShadowStrength = 0.0f;
    int cloudShadowResolution = 0;
    int cloudShadowSamples = 0;
    float cloudFieldRadius = 0.0f;
    float cloudFieldFalloff = 0.0f;
    int cloudSelfShadowEnabled = -1;
    int cloudLightSamples = 0;
    float cloudLightStepMeters = 0.0f;
    float cloudPhaseEccentricity = 0.0f;
    float cloudShadowAmbientStrength = 0.0f;

    int meshBackend = -1;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacementHeightTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacementMaskTexture;
    int displacementTextureResolution = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE displacementHeightSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE displacementHeightSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE displacementMaskSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE displacementMaskSrvGpu{};
    bool displacementSrvAllocated = false;
    D3D12_CPU_DESCRIPTOR_HANDLE meshResourceTableCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE meshResourceTableGpu{};
    bool meshResourceTableAllocated = false;
    Microsoft::WRL::ComPtr<ID3D12Resource> colorizeTexture;
    int colorizeTextureResolution = 0;
    uint64_t colorizeTextureUploadKey = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacementTriIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacementPatchIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacementSectionIndexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacementEdgeIndexBuffer;
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
    int exposureMode = -1;
    float exposureEv = 0.0f;
    float autoExposureBiasEv = 0.0f;
    float autoExposureMinEv = 0.0f;
    float autoExposureMaxEv = 0.0f;
    float autoExposureSpeed = 0.0f;
    float colorTemperatureKelvin = 0.0f;
    float dofFStop = 0.0f;
    float dofFocusDistanceMeters = 0.0f;
    float dofSensorHeightMm = 0.0f;
    float dofMaxBlurPixels = 0.0f;
    int dofApertureShape = -1;
    int dofApertureBlades = 0;
    float dofApertureRotationDegrees = 0.0f;
    float dofHighlightBoost = 0.0f;
    bool dofMiniatureEnabled = false;
    float dofMiniatureScale = 0.0f;

    Microsoft::WRL::ComPtr<ID3D12Resource> aoTexture;
    int aoTextureResolution = 0;
    D3D12_RESOURCE_STATES aoTextureState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_CPU_DESCRIPTOR_HANDLE aoSrvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE aoSrvGpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE aoUavCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE aoUavGpu{};
    bool aoSrvAllocated = false;
    bool aoUavAllocated = false;
    uint64_t aoUploadKey = 0;
    float aoCachedRadius = -1.0f;

    PreviewRenderStats renderStats;

    D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES postState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES outputState = D3D12_RESOURCE_STATE_COMMON;
    std::array<D3D12_RESOURCE_STATES, 2> exposureStates = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON};
    D3D12_RESOURCE_STATES shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES sceneDepthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
};

struct MeshPreviewPipelineResources
{
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> surfacePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> waterPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> wirePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> gridPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> shadowPso;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> displacementRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementSurfacePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementShadowPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementWirePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementSectionPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementSectionShadowPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementSectionWirePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementTessSurfacePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementTessShadowPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> displacementTessWirePso;
    Microsoft::WRL::ComPtr<ID3D12Resource> displacementCbv;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap;
    DXGI_FORMAT renderTargetFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT displacementRenderTargetFormat = DXGI_FORMAT_UNKNOWN;
};

struct MeshPreviewPipelineContext
{
    ID3D12Device* device = nullptr;
    std::filesystem::path shaderPath;
    DXGI_FORMAT renderTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT depthStencilFormat = DXGI_FORMAT_D32_FLOAT;
    UINT rootConstantDwordCount = 0;
    UINT displacementConstantDwordCount = 0;
    UINT64 displacementCbvByteSize = 0;
};

bool EnsureMeshPreviewPipeline(MeshPreviewPipelineResources& resources,
                               const MeshPreviewPipelineContext& context,
                               std::string* error);
bool EnsureMeshPreviewDisplacementPipeline(MeshPreviewPipelineResources& resources,
                                           const MeshPreviewPipelineContext& context,
                                           std::string* error);
void ResetMeshPreviewPipelineResources(MeshPreviewPipelineResources& resources);

} // namespace terrain::rendering
