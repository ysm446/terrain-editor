#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rock
{
using GraphId = int;

enum class NodeKind
{
    HeightmapLoad = 4,
    HeightmapBlur = 5,
    Shape = 6,
    MultiScaleErosion = 8,
    MaskNoise = 9,
    MaskBlend = 10,
    MaskFluvial = 11,
    Rock = 12,
    Sediment = 13,
};

enum class PinKind
{
    Input,
    Output,
};

enum class ValueType
{
    Mesh = 1,
    HeightField = 2,
    Mask = 3,
};

enum class MultiScaleErosionBackend
{
    CpuReference,
    GpuCompute,
};

enum class MaskNoiseBackend
{
    CpuParallel,
    GpuCompute,
};

enum class ShapeKind
{
    Hemisphere,
    Pyramid,
};

enum class MaskBlendMode
{
    Add,
    Multiply,
    Min,
    Max,
};

enum class FlowAccumulationAlgorithm
{
    D8,
    MFD,
};

// How the 3D viewport renders the heightfield. CPU mesh builds an explicit
// vertex / triangle / edge mesh on the host and uploads it; GPU
// displacement uploads the heightfield as a texture and lets a vertex
// shader sample it on a static UV grid. The latter avoids re-uploading a
// large vertex buffer per parameter change at the cost of one extra
// texture upload (~few ms vs tens of ms).
enum class MeshPreviewBackend
{
    CpuMesh,
    GpuDisplacement,
};

// マスクプレビューのシェーディング方式。
// Grayscale: mask=0 を黒、mask=1 を白とする純粋な白黒ランプ (既定)。
// GrayOrange: ライティング付きのグレー×オレンジのトーン。
enum class MaskShadingMode
{
    Grayscale,
    GrayOrange,
};

enum class MaskFluvialOutputCurve
{
    // log(1 + accum) / log(1 + maxAdjusted), then pow(gamma). Continuous
    // dendritic visualization — the standard GIS "log flow accumulation"
    // look. Threshold acts as a noise floor (subtracted before normalize).
    Log,
    // smoothstep around accumulationThreshold, then pow(power). Sharp
    // binary river / non-river output for layering masks downstream.
    Threshold,
    // (accum - threshold) / max, then pow(gamma). Linear continuous,
    // preserves the long tail without log compression. Tends to overweight
    // the main trunks at the cost of fine branches.
    Linear,
};

enum class PreviewStage
{
    Graph = 3,
    HeightmapBlur = 4,
    Shape = 5,
    MultiScaleErosion = 7,
    MaskNoise = 8,
    MaskBlend = 9,
    MaskFluvial = 10,
    Rock = 11,
    Sediment = 12,
};

enum class HeightfieldPreviewField
{
    Heightmap,
    Deposits,
    Flows,
    Age,
    Mask,
};

struct Pin
{
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::HeightField;
    std::string label;
};

struct HeightmapLoadSettings
{
    std::string path;
    float scaleMeters = 1024.0f;
    float relativeVerticalScalePercent = 100.0f;
    float verticalOffsetMeters = 0.0f;
    int simulationResolution = 512;
};

struct ShapeSettings
{
    ShapeKind kind = ShapeKind::Hemisphere;
    float scaleMeters = 1024.0f;
    float relativeHeightPercent = 50.0f;
    int simulationResolution = 512;
};

struct HeightmapBlurSettings
{
    float radius = 3.0f;
    float strength = 1.0f;
    int iterations = 1;
};

struct MaskNoiseSettings
{
    int seed = 0;
    int octaves = 4;
    float frequency = 4.0f;
    float lacunarity = 2.0f;
    float persistence = 0.5f;
    int simulationResolution = 512;
    MaskNoiseBackend backend = MaskNoiseBackend::GpuCompute;
};

struct MaskBlendSettings
{
    MaskBlendMode mode = MaskBlendMode::Add;
    float intensity = 1.0f;
};

// Heightfield + mask. Scatters rocks on a jittered Voronoi grid (used
// purely as a deterministic Poisson-like scatter pattern) and lifts each
// scatter point into a rotated, possibly elongated dome with per-rock
// facet detail. Rock size is specified directly in metres and is
// independent of scatter spacing, so rocks freely overlap and pixels
// take the max rock contribution. The natural max-blend already
// produces creases where rocks meet — there is no separate crack
// carving step.
struct RockSettings
{
    int seed = 0;
    float density = 8.0f;            // Scatter pitch (m). Spacing between rock centres.
    float coverage = 1.0f;           // 0..1, fraction of scatter points that become a rock.
    float rockSizeMinM = 5.0f;       // Min rock diameter (m). Each rock samples uniformly from [min, max].
    float rockSizeMaxM = 10.0f;      // Max rock diameter (m). May be larger or smaller than density — overlap is handled by max-blend.
    float rockHeight = 1.5f;         // m, max bump height per rock.
    float heightJitter = 0.5f;       // 0..1, per-rock height variation (0 = uniform).
    float rotationVariation = 1.0f;  // 0..1, fraction of full 2π rotation each rock can take. 0 = aligned, 1 = full random.
    float aspectVariation = 0.3f;    // 0..1, per-rock aspect ratio variation. 0 = circular, 1 = up to 2:1 along a random axis.
    float edgeSharpness = 1.0f;      // 0 = circular silhouette (smooth dome). > 0 = polyhedral silhouette hard-clipped by 4–7 sided polygon; the value blends interior height between radial and polyhedral. 1 = pure flat-faceted polyhedron.
    float bumpiness = 0.6f;          // 0..1, surface detail amplitude (smooth or faceted, see facetSharpness).
    float facetSharpness = 0.5f;     // 0 = smooth dome with rounded bumps, 1 = polyhedral flat facets with sharp creases.
    float facetScale = 2.5f;         // Sub-cell Voronoi frequency in the rock-local frame (higher = more, smaller facets per rock).
};

// Heightfield + mask. Lays a uniform sediment layer of `initialSedimentM`
// on the input bedrock and redistributes it with a stochastic
// hydraulic-erosion particle simulation. Output heights =
// bedrock + remaining sediment; output mask = sediment thickness scaled
// around the initial layer depth, with an optional contrast curve.
struct SedimentSettings
{
    int iterations = 30;             // Number of waves of particles (sediment is updated between waves).
    float initialSedimentM = 2.0f;   // Initial sediment thickness on every cell (m). Determines how much there is to redistribute and the mask scale.
    float maskContrast = 0.7f;       // 0 = linear gradient (smooth grey transitions), 1 = hard binary at 50% of (initial × 2). Higher → more GeoGen-like crisp dendritic mask.

    int particleCount = 5000;        // Particles per wave (× iterations = total particles).
    int particleLifetime = 128;      // Max steps per particle.
    float particleGradientRadiusM = 8.0f; // Metres. Distance at which the wider central-difference gradient is sampled. Resolution-independent: 8m on a 1024m / 1024² grid is 8 cells, on a 512² grid is 4 cells. Larger → smoother flow that follows large-scale topology (matches GeoGen's "Largest Detail Level" setting). 16m+ may produce noticeable spikes at convergence points.
    float particleInertia = 0.40f;   // 0..1, blend with previous direction (carries particles past tiny pits/noise so they don't get stuck mid-slope).
    float particleFriction = 0.05f;  // 0..1, fraction of velocity lost per step (low enough to feel gravity-driven, high enough to bound velocity).
    float particleCapacity = 4.0f;   // Multiplier on slope × |v| × water for carrying capacity.
    float particleErosion = 0.3f;    // 0..1, fraction of (capacity - carried) eroded per step.
    float particleDeposition = 0.3f; // 0..1, fraction of (carried - capacity) deposited per step (lower → particles travel further before dumping, more streak-like).
    float particleEvaporation = 0.02f; // 0..1, water lost per step.
    float particleEmissionTime = 0.0f; // 0..1, fraction of iterations that emit new particles. 0 = first wave only, 1 = every wave.
    int particleSeed = 0;
};

// Heightfield -> mask. Performs D8 (or MFD) flow accumulation on the
// input heights and emits a mask. The default Log curve produces the
// classic continuous dendritic drainage tree (every cell visible, fine
// branches dim, main trunks bright). Switch to Threshold for sharp
// binary river extraction or Linear for a non-log continuous map.
struct MaskFluvialSettings
{
    FlowAccumulationAlgorithm algorithm = FlowAccumulationAlgorithm::D8;
    MaskFluvialOutputCurve outputCurve = MaskFluvialOutputCurve::Log;
    // In Log/Linear modes: noise floor — cells with fewer upstream cells
    // than this fraction of the grid get clipped to 0. Default 0 shows
    // the full tree.
    // In Threshold mode: the actual river threshold (0.005 ≈ 0.5%
    // gives a clean main-channel mask).
    float accumulationThreshold = 0.0f;
    float gamma = 0.5f;           // Log/Linear curve exponent (lower = brighter leaves)
    float softness = 0.15f;       // Threshold mode: smoothstep transition width
    float power = 1.6f;           // Threshold mode: pow(mask, power) for edge taper
    int pitFillIterations = 8;    // 0 keeps lakes, ~8 removes most local pits
    float mfdExponent = 4.0f;     // MFD slope exponent (only when MFD selected)
};

struct MultiScaleErosionSettings
{
    int iterations = 50;
    bool enableStreamPower = true;
    bool enableThermal = true;
    bool enableDeposition = true;
    // Multi-grid pyramid: run progressively from a coarse grid up to the
    // target resolution, with bilinear upsampling between levels. Drainage
    // networks form quickly at coarse scales (path_length / cellSize is
    // small) and finer levels only refine, giving near-resolution-invariant
    // results. Disable to fall back to a single-resolution simulation.
    bool useMultigrid = true;

    // Stream Power Erosion (erosion.glsl)
    float speStrength = 0.004f;         // k
    float streamExponent = 0.9f;        // p_sa
    float slopeExponent = 2.0f;         // p_sl
    float maxStreamPower = 10000.0f;    // max_spe
    float flowExponent = 1.3f;          // flow_p
    float speTimeStep = 1.0f;           // dt

    // Thermal (thermal.glsl)
    float thermalAngleDegrees = 30.0f;  // tanThresholdAngle (in degrees)
    float thermalStrength = 0.005f;     // eps
    bool thermalNoisifyAngle = true;
    float thermalNoiseMin = 0.9f;
    float thermalNoiseMax = 1.4f;
    float thermalNoiseWavelength = 0.0023f;

    // Deposition (deposition.glsl)
    float depositionStrength = 0.2f;
    float rain = 2.6f;

    MultiScaleErosionBackend backend = MultiScaleErosionBackend::CpuReference;
};

struct Node
{
    GraphId id = 0;
    NodeKind kind = NodeKind::HeightmapLoad;
    std::string title;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    HeightmapLoadSettings heightmap;
    ShapeSettings shape;
    HeightmapBlurSettings heightmapBlur;
    MultiScaleErosionSettings multiScaleErosion;
    MaskNoiseSettings maskNoise;
    MaskBlendSettings maskBlend;
    MaskFluvialSettings maskFluvial;
    RockSettings rock;
    SedimentSettings sediment;
};

struct Link
{
    GraphId id = 0;
    GraphId startPin = 0;
    GraphId endPin = 0;
};

struct PreviewSettings
{
    int resolution = 512;
    int lod = 0;
    bool showSurface = true;
    bool showWireframe = false;
    bool showGrid = true;
    int lightingMode = 0;
    MeshPreviewBackend meshBackend = MeshPreviewBackend::CpuMesh;
    float sunAzimuthDegrees = 315.0f;
    float sunElevationDegrees = 38.0f;
    float sunIntensity = 1.05f;
    float ambientStrength = 0.38f;
    float shadowStrength = 0.36f;
    int shadowMapResolution = 2048;
    float shadowBias = 0.0035f;
    std::array<float, 3> pbrAlbedo = {0.74f, 0.76f, 0.70f};
    int gridCellCount = 10;
    float gridCellSizeMeters = 100.0f;
    std::array<float, 3> gridColor = {0.2f, 0.2f, 0.2f};
    std::array<float, 3> viewportBackground = {0.268f, 0.268f, 0.268f};
    MaskShadingMode maskShading = MaskShadingMode::Grayscale;
};

enum class SkyMode
{
    SolidColor,
    Atmospheric,
};

// Atmospheric (Nishita single-scatter Rayleigh + Mie). The sky / sun /
// terrain ambient / cloud lighting are all derived from the same model
// driven only by sun direction so the whole scene transitions through
// day -> sunset -> night coherently when sun elevation is animated.
struct SkySettings
{
    SkyMode mode = SkyMode::SolidColor;
    float atmosphereDensity = 1.0f;            // multiplies the Rayleigh β coefficients — overall atmosphere "thickness"
    float mieStrength = 0.2f;                  // turbidity / haze (multiplies Mie scattering coefficient)
    float mieEccentricity = 0.76f;             // Henyey-Greenstein g — sun-glow tightness, 0 = isotropic, 0.9 = sharp
    std::array<float, 3> groundAlbedo = {0.30f, 0.30f, 0.30f};
    float sunSizeDegrees = 2.5f;
    float sunGlowStrength = 0.3f;
};

struct CloudSettings
{
    bool enabled = false;
    int seed = 1;
    float coverage = 0.55f;
    float densityMultiplier = 1.0f;
    float altitudeMin = 1500.0f;
    float altitudeMax = 3500.0f;
    float horizontalScale = 4000.0f;
    float absorption = 0.06f;
    std::array<float, 3> color = {1.0f, 1.0f, 1.0f};
    float windDirectionDegrees = 45.0f;
    float windSpeedMetersPerSec = 0.0f;
    int qualitySamples = 32;
    float shadowStrength = 0.7f;
    int shadowResolution = 1024;
    int shadowSamples = 16;
    float fieldRadius = 6000.0f;
    float fieldFalloff = 2000.0f;
    // Volumetric self-shadowing: each view sample marches a few steps
    // toward the sun and accumulates density to compute a Beer-Lambert
    // light transmittance. 0 disables (back to pure top-down ramp).
    int lightSamples = 6;
    float lightStepMeters = 80.0f;
    // Henyey-Greenstein eccentricity for the cloud phase function. 0.4 gives
    // a gentle silver lining around the sun direction; 0.0 is isotropic.
    float phaseEccentricity = 0.4f;
};

struct GraphSettings
{
    PreviewSettings preview;
    SkySettings sky;
    CloudSettings clouds;
};

struct MeshVertex
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float nx = 0.0f;
    float ny = 1.0f;
    float nz = 0.0f;
    float mask = 0.0f;
};

struct MeshTriangle
{
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
};

struct MeshEdge
{
    uint32_t a = 0;
    uint32_t b = 0;
};

struct MeshData
{
    std::vector<MeshVertex> vertices;
    std::vector<MeshTriangle> triangles;
    std::vector<MeshEdge> edges;
};

struct HeightfieldGrid
{
    int resolution = 0;
    float terrainSizeMeters = 1.0f;
    std::vector<float> heights;
    std::vector<float> mask;
    std::vector<float> deposits;
    std::vector<float> flows;
    std::vector<float> age;
};

struct MaskGrid
{
    int resolution = 0;
    std::vector<float> values;
};

using MultiScaleErosionGpuEvaluator = bool (*)(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings, std::string* error);
using MaskNoiseGpuEvaluator = bool (*)(MaskGrid& grid, const MaskNoiseSettings& settings, std::string* error);

struct HeightfieldPipeline
{
    struct HeightfieldOperation
    {
        enum class Kind
        {
            HeightmapBlur,
            MultiScaleErosion,
            MaskFluvial,
            Rock,
            Sediment,
        };

        Kind kind = Kind::HeightmapBlur;
        GraphId nodeId = 0;
        HeightmapBlurSettings heightmapBlur;
        MultiScaleErosionSettings multiScaleErosion;
        MaskFluvialSettings maskFluvial;
        RockSettings rock;
        SedimentSettings sediment;
    };

    bool hasSource = false;
    GraphId heightmapNodeId = 0;
    HeightmapLoadSettings heightmap;
    bool useShape = false;
    GraphId shapeNodeId = 0;
    ShapeSettings shape;
    std::vector<HeightfieldOperation> heightfieldOperations;
};

struct EvaluationSummary
{
    uint64_t version = 0;
    bool dirty = true;
    std::string status = "Graph has not been evaluated";
    PreviewStage previewStage = PreviewStage::Graph;
    GraphId previewNodeId = 0;
    GraphId previewPinId = 0;
    bool previewShowsMask = false;
    HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap;
    std::string previewMessage;
    HeightfieldGrid previewHeightfield;
    MeshData previewMesh;
};

class NodeGraph
{
public:
    static NodeGraph CreateDefaultTerrainGraph();

    const std::vector<Node>& Nodes() const;
    const std::vector<Link>& Links() const;
    GraphSettings& Settings();
    const GraphSettings& Settings() const;
    const EvaluationSummary& Evaluation() const;

    const Pin* FindPin(GraphId pinId) const;
    const Node* FindNode(GraphId nodeId) const;
    Node* FindMutableNode(GraphId nodeId);
    const Node* FindUpstreamForPin(GraphId pinId) const;
    bool IsInputPin(GraphId pinId) const;
    bool IsOutputPin(GraphId pinId) const;
    bool PinHasLink(GraphId pinId) const;
    bool CanCreateLink(GraphId startPin, GraphId endPin) const;

    bool CreateLink(GraphId startPin, GraphId endPin);
    bool DeleteLink(GraphId linkId);
    GraphId CreateNode(NodeKind kind);
    bool DeleteNode(GraphId nodeId);
    void ReplaceNodes(std::vector<Node> nodes);
    void ReplaceLinks(std::vector<Link> links);
    bool SetPreviewStage(PreviewStage stage);
    bool SetPreviewNode(GraphId nodeId);
    bool SetPreviewPin(GraphId pinId);
    PreviewStage Preview() const;
    HeightfieldPipeline PipelineFor(PreviewStage stage) const;
    HeightfieldPipeline PreviewPipeline() const;
    void MarkDirty(std::string_view reason);
    void SetEvaluationPending(std::string_view status);
    void ApplyEvaluationResultFrom(const NodeGraph& evaluatedGraph);
    void Evaluate(int previewMeshResolution = 0);

private:
    GraphId AllocateGraphId();
    void RebuildNextGraphId();
    GraphId AddNode(NodeKind kind, std::string title);
    GraphId AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label);
    void AddInitialLink(GraphId startPin, GraphId endPin);
    const Node* FindFirstNode(NodeKind kind) const;
    const Node* FindNodeByOutputPin(GraphId pinId) const;
    const Node* FindUpstreamNode(const Node& node) const;
    HeightfieldPipeline PipelineTo(NodeKind targetKind) const;
    HeightfieldPipeline PipelineToNode(const Node& targetNode) const;
    HeightfieldGrid EvaluateMaskAsHeightfield(const Node& node, std::string* message);
    MaskGrid EvaluateMaskGridForNodeCached(const Node& node, int depth, uint64_t* outputHash);
    HeightfieldGrid EvaluateHeightPipelineCached(const HeightfieldPipeline& pipeline, std::string* message, HeightfieldPreviewField previewField, uint64_t* outputHash);
    MeshData BuildMeshFromHeightPipelineCached(const HeightfieldPipeline& pipeline, int resolution, std::string* message, HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap, HeightfieldGrid* previewGrid = nullptr);

    struct HeightfieldNodeCache
    {
        bool valid = false;
        int resolution = 0;
        uint64_t inputHash = 0;
        uint64_t parameterHash = 0;
        uint64_t outputHash = 0;
        HeightfieldGrid grid;
        std::string message;
    };

    struct MaskNodeCache
    {
        bool valid = false;
        uint64_t inputHash = 0;
        uint64_t parameterHash = 0;
        uint64_t outputHash = 0;
        MaskGrid grid;
    };

    std::vector<Node> nodes_;
    std::vector<Link> links_;
    GraphSettings settings_;
    std::unordered_map<GraphId, HeightfieldNodeCache> heightfieldCache_;
    std::unordered_map<GraphId, MaskNodeCache> maskCache_;
    EvaluationSummary evaluation_;
    GraphId nextGraphId_ = 1;
};

std::string_view ToString(ShapeKind kind);
std::string_view ToString(MaskBlendMode mode);
std::string_view ToString(NodeKind kind);
std::string_view ToString(PreviewStage stage);
std::string_view ToString(ValueType type);
PreviewStage PreviewStageFor(NodeKind kind);
bool IsMaskOnlyNodeKind(NodeKind kind);
void SetMultiScaleErosionGpuEvaluator(MultiScaleErosionGpuEvaluator evaluator);
void SetMaskNoiseGpuEvaluator(MaskNoiseGpuEvaluator evaluator);

// Thread-safe progress signal: holds the GraphId of the node whose
// evaluation kernel is currently running on a worker thread, or 0 when
// no kernel is active. Updated only on cache misses (cache hits are
// instantaneous so the badge wouldn't be visible anyway). The UI thread
// reads this to draw a "計算中" badge that walks the upstream chain in
// real time as the pipeline progresses.
std::atomic<GraphId>& CurrentlyEvaluatingNodeId();

} // namespace rock
