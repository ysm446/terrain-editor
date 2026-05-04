#pragma once

#include <array>
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
    PrimitiveSdf,
    NoiseWarp,
    CrackField,
    OutputMesh,
    HeightmapLoad,
    FluvialErosion,
};

enum class PinKind
{
    Input,
    Output,
};

enum class ValueType
{
    SdfGrid,
    Mesh,
    HeightField,
    Mask,
};

enum class PrimitiveKind
{
    Sphere,
    Box,
    Capsule,
    Ellipsoid,
    RockBlob,
};

enum class PreviewStage
{
    Primitive,
    Noise,
    Crack,
    Fluvial,
    Output,
};

struct Pin
{
    GraphId id = 0;
    GraphId nodeId = 0;
    PinKind kind = PinKind::Input;
    ValueType valueType = ValueType::SdfGrid;
    std::string label;
};

struct PrimitiveSettings
{
    PrimitiveKind kind = PrimitiveKind::RockBlob;
};

struct NoiseSettings
{
    float amplitude = 0.35f;
    float frequency = 2.0f;
    int octaves = 4;
    int seed = 0;
};

struct CrackSettings
{
    float width = 0.035f;
    float depth = 0.42f;
    float roughness = 0.65f;
};

struct OutputMeshSettings
{
    int resolution = 512;
    int lod = 0;
    float isoValue = 0.0f;
};

struct HeightmapLoadSettings
{
    std::string path;
    float scaleMeters = 1024.0f;
    float relativeVerticalScalePercent = 100.0f;
    float verticalOffsetMeters = 0.0f;
    int simulationResolution = 512;
};

struct FluvialErosionSettings
{
    float featureSize = 8.0f;
    int iterations = 25;
    float channelLength = 128.0f;
    float erosionStrength = 1.0f;
    float channeling = 0.25f;
    float friction = 0.1f;
    float wearAngleDegrees = 15.0f;
    float depositAngleDegrees = 0.0f;
    float maxErosionAngleDegrees = 30.0f;
    float erosionGranularity = 10.0f;
    float sedimentVelocity = 1.0f;
    float sedimentCapacity = 0.6f;
    float depositionRate = 0.35f;
    int seed = 1;
};

struct Node
{
    GraphId id = 0;
    NodeKind kind = NodeKind::PrimitiveSdf;
    std::string title;
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    PrimitiveSettings primitive;
    NoiseSettings noise;
    CrackSettings crack;
    OutputMeshSettings outputMesh;
    HeightmapLoadSettings heightmap;
    FluvialErosionSettings fluvialErosion;
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
    bool showPoints = false;
    bool showGrid = true;
    int lightingMode = 0;
    float sunAzimuthDegrees = 315.0f;
    float sunElevationDegrees = 38.0f;
    float sunIntensity = 1.35f;
    float ambientStrength = 0.22f;
    float shadowStrength = 0.55f;
    int shadowMapResolution = 2048;
    float shadowBias = 0.0035f;
    std::array<float, 3> pbrAlbedo = {0.78f, 0.80f, 0.76f};
    std::array<float, 3> viewportBackground = {0.268f, 0.268f, 0.268f};
};

struct GraphSettings
{
    PreviewSettings preview;
};

struct SurfacePoint
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float sdf = 0.0f;
};

struct SurfaceSegment
{
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    float bz = 0.0f;
};

struct SurfaceTriangle
{
    float ax = 0.0f;
    float ay = 0.0f;
    float az = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    float bz = 0.0f;
    float cx = 0.0f;
    float cy = 0.0f;
    float cz = 0.0f;
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
};

struct SdfPreviewStats
{
    int resolution = 0;
    int sliceResolution = 0;
    int totalVoxels = 0;
    int insideVoxels = 0;
    float voxelSize = 0.0f;
    float minSdf = 0.0f;
    float maxSdf = 0.0f;
    float fillRatio = 0.0f;
    float estimatedVolume = 0.0f;
    std::vector<float> centerSlice;
    std::vector<SurfacePoint> surfacePoints;
    std::vector<SurfaceSegment> surfaceSegments;
    std::vector<SurfaceTriangle> surfaceTriangles;
};

struct SdfPipeline
{
    enum class OperationKind
    {
        NoiseWarp = 1,
        CrackField = 2,
        OutputIso = 3,
    };

    struct Operation
    {
        OperationKind kind = OperationKind::NoiseWarp;
        GraphId nodeId = 0;
        NoiseSettings noise;
        CrackSettings crack;
        float isoValue = 0.0f;
    };

    struct HeightfieldOperation
    {
        enum class Kind
        {
            FluvialErosion,
        };

        Kind kind = Kind::FluvialErosion;
        GraphId nodeId = 0;
        FluvialErosionSettings fluvialErosion;
    };

    PrimitiveKind primitiveKind = PrimitiveKind::RockBlob;
    std::vector<Operation> operations;
    bool useNoise = false;
    NoiseSettings noise;
    std::vector<NoiseSettings> noiseLayers;
    bool useCrack = false;
    CrackSettings crack;
    bool applyOutputIso = false;
    float outputIsoValue = 0.0f;
    bool hasSource = false;
    bool useHeightmap = false;
    GraphId heightmapNodeId = 0;
    HeightmapLoadSettings heightmap;
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
    bool previewIsHeightmap = false;
    bool previewShowsMask = false;
    std::string previewMessage;
    SdfPreviewStats previewSdf;
    SdfPreviewStats finalSdf;
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
    SdfPipeline PipelineFor(PreviewStage stage) const;
    SdfPipeline PreviewPipeline() const;
    SdfPipeline FinalPipeline() const;
    void MarkDirty(std::string_view reason);
    void Evaluate(int previewMeshResolution = 0);
    void EvaluateFinal(GraphId outputNodeId = 0);

private:
    GraphId AddNode(NodeKind kind, std::string title);
    GraphId AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label);
    void AddInitialLink(GraphId startPin, GraphId endPin);
    const Node* FindFirstNode(NodeKind kind) const;
    const Node* FindNodeByOutputPin(GraphId pinId) const;
    const Node* FindUpstreamNode(const Node& node) const;
    SdfPipeline PipelineTo(NodeKind targetKind) const;
    SdfPipeline PipelineToNode(const Node& targetNode) const;
    MeshData BuildMeshFromHeightPipelineCached(const SdfPipeline& pipeline, int resolution, std::string* message);

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

std::string_view ToString(PrimitiveKind kind);
std::string_view ToString(NodeKind kind);
std::string_view ToString(PreviewStage stage);
std::string_view ToString(ValueType type);
PreviewStage PreviewStageFor(NodeKind kind);

} // namespace rock
