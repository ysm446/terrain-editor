#pragma once

#include <array>
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
    OutputMesh = 3,
    HeightmapLoad = 4,
    HeightmapBlur = 5,
    Shape = 6,
    ErosionNoise = 7,
    MultiScaleErosion = 8,
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

enum class ShapeKind
{
    Hemisphere,
    Pyramid,
};

enum class PreviewStage
{
    Output = 3,
    HeightmapBlur = 4,
    Shape = 5,
    ErosionNoise = 6,
    MultiScaleErosion = 7,
};

enum class HeightfieldPreviewField
{
    Heightmap,
    Deposits,
    Flows,
    Age,
};

struct Pin
{
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::HeightField;
    std::string label;
};

struct OutputMeshSettings
{
    int resolution = 512;
    int lod = 0;
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

struct ErosionNoiseSettings
{
    float frequency = 20.0f;
    int octaves = 2;
    float erosionStrength = 0.02f;
    float directionInfluence = 0.5f;
    float valleyLow = 0.1f;
    float valleyHigh = 0.4f;
    int seed = 0;
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
    float speStrength = 0.0005f;        // k
    float streamExponent = 0.8f;        // p_sa
    float slopeExponent = 2.0f;         // p_sl
    float maxStreamPower = 10000.0f;    // max_spe
    float flowExponent = 1.3f;          // flow_p
    float speTimeStep = 1.0f;           // dt

    // Thermal (thermal.glsl)
    float thermalAngleDegrees = 30.0f;  // tanThresholdAngle (in degrees)
    float thermalStrength = 0.00005f;   // eps
    bool thermalNoisifyAngle = true;
    float thermalNoiseMin = 0.9f;
    float thermalNoiseMax = 1.4f;
    float thermalNoiseWavelength = 0.0023f;

    // Deposition (deposition.glsl)
    float depositionStrength = 1.0f;
    float rain = 2.6f;

    MultiScaleErosionBackend backend = MultiScaleErosionBackend::CpuReference;
};

struct Node
{
    GraphId id = 0;
    NodeKind kind = NodeKind::OutputMesh;
    std::string title;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    OutputMeshSettings outputMesh;
    HeightmapLoadSettings heightmap;
    ShapeSettings shape;
    HeightmapBlurSettings heightmapBlur;
    ErosionNoiseSettings erosionNoise;
    MultiScaleErosionSettings multiScaleErosion;
};

struct Link
{
    GraphId id = 0;
    GraphId startPin = 0;
    GraphId endPin = 0;
};

struct PreviewSettings
{
    int resolution = 48;
    int lod = 0;
    bool showSurface = true;
    bool showWireframe = true;
    bool showGrid = true;
    int lightingMode = 0;
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
};

struct GraphSettings
{
    PreviewSettings preview;
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

using MultiScaleErosionGpuEvaluator = bool (*)(HeightfieldGrid& grid, const MultiScaleErosionSettings& settings, std::string* error);

struct HeightfieldPipeline
{
    struct HeightfieldOperation
    {
        enum class Kind
        {
            HeightmapBlur,
            ErosionNoise,
            MultiScaleErosion,
        };

        Kind kind = Kind::HeightmapBlur;
        GraphId nodeId = 0;
        HeightmapBlurSettings heightmapBlur;
        ErosionNoiseSettings erosionNoise;
        MultiScaleErosionSettings multiScaleErosion;
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
    uint64_t finalVersion = 0;
    bool dirty = true;
    bool finalDirty = true;
    std::string status = "Graph has not been evaluated";
    PreviewStage previewStage = PreviewStage::Output;
    GraphId previewNodeId = 0;
    GraphId previewPinId = 0;
    bool previewShowsMask = false;
    HeightfieldPreviewField previewField = HeightfieldPreviewField::Heightmap;
    std::string previewMessage;
    HeightfieldGrid previewHeightfield;
    MeshData previewMesh;
    MeshData finalMesh;
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
    OutputMeshSettings* FindOutputMeshSettings(GraphId nodeId);
    const OutputMeshSettings* FindOutputMeshSettings(GraphId nodeId) const;
    const OutputMeshSettings& OutputMeshSettingsFor(GraphId nodeId = 0) const;

    const Pin* FindPin(GraphId pinId) const;
    const Node* FindNode(GraphId nodeId) const;
    Node* FindMutableNode(GraphId nodeId);
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
    HeightfieldPipeline FinalPipeline() const;
    void MarkDirty(std::string_view reason);
    void SetEvaluationPending(std::string_view status);
    void ApplyEvaluationResultFrom(const NodeGraph& evaluatedGraph);
    void Evaluate(int previewMeshResolution = 0);
    void EvaluateFinal(GraphId outputNodeId = 0);

private:
    GraphId AddNode(NodeKind kind, std::string title);
    GraphId AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label);
    void AddInitialLink(GraphId startPin, GraphId endPin);
    const Node* FindFirstNode(NodeKind kind) const;
    const Node* FindNodeByOutputPin(GraphId pinId) const;
    const Node* FindUpstreamNode(const Node& node) const;
    HeightfieldPipeline PipelineTo(NodeKind targetKind) const;
    HeightfieldPipeline PipelineToNode(const Node& targetNode) const;
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

    std::vector<Node> nodes_;
    std::vector<Link> links_;
    GraphSettings settings_;
    std::unordered_map<GraphId, HeightfieldNodeCache> heightfieldCache_;
    EvaluationSummary evaluation_;
    GraphId nextNodeId_ = 1;
    GraphId nextPinId_ = 11;
    GraphId nextLinkId_ = 101;
};

std::string_view ToString(ShapeKind kind);
std::string_view ToString(NodeKind kind);
std::string_view ToString(PreviewStage stage);
std::string_view ToString(ValueType type);
PreviewStage PreviewStageFor(NodeKind kind);
void SetMultiScaleErosionGpuEvaluator(MultiScaleErosionGpuEvaluator evaluator);

} // namespace rock
