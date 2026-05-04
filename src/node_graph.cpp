#include "node_graph.h"

#include "sdf_preview.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace rock
{
namespace
{
using Microsoft::WRL::ComPtr;

std::string NoisePipelineSummary(const SdfPipeline& pipeline)
{
    if (pipeline.noiseLayers.empty())
    {
        return "";
    }
    if (pipeline.noiseLayers.size() == 1)
    {
        const NoiseSettings& noise = pipeline.noiseLayers.front();
        return std::format(" -> noise {:.2f}/{:.2f}/{} seed {}", noise.amplitude, noise.frequency, noise.octaves, noise.seed);
    }
    return std::format(" -> noise x{}", pipeline.noiseLayers.size());
}

struct HeightmapImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> values;
};

std::wstring Utf8ToWidePath(const std::string& value)
{
    if (value.empty())
    {
        return {};
    }
    const std::u8string utf8(value.begin(), value.end());
    return std::filesystem::path(utf8).wstring();
}

bool LoadHeightmapImage(const std::string& path, HeightmapImage& image, std::string* error)
{
    if (path.empty())
    {
        if (error != nullptr)
        {
            *error = "No heightmap file selected";
        }
        return false;
    }

    const HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE)
    {
        if (error != nullptr)
        {
            *error = std::format("COM initialization failed: 0x{:08X}", static_cast<unsigned int>(initHr));
        }
        return false;
    }

    const auto cleanup = [&]() {
        if (shouldUninitialize)
        {
            CoUninitialize();
        }
    };

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = std::format("WIC factory creation failed: 0x{:08X}", static_cast<unsigned int>(hr));
        }
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    const std::wstring widePath = Utf8ToWidePath(path);
    hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Failed to open heightmap image";
        }
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Failed to read heightmap image frame";
        }
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    frame->GetSize(&width, &height);
    if (width < 2 || height < 2)
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Heightmap image must be at least 2 x 2 pixels";
        }
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Failed to create heightmap image converter";
        }
        return false;
    }

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr))
    {
        cleanup();
        if (error != nullptr)
        {
            *error = "Failed to convert heightmap image";
        }
        return false;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    hr = converter->CopyPixels(nullptr, width * 4u, static_cast<UINT>(pixels.size()), pixels.data());
    cleanup();
    if (FAILED(hr))
    {
        if (error != nullptr)
        {
            *error = "Failed to copy heightmap pixels";
        }
        return false;
    }

    image.width = width;
    image.height = height;
    image.values.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
    for (size_t i = 0; i < image.values.size(); ++i)
    {
        const uint8_t r = pixels[i * 4u + 0u];
        const uint8_t g = pixels[i * 4u + 1u];
        const uint8_t b = pixels[i * 4u + 2u];
        image.values[i] = (0.2126f * static_cast<float>(r) + 0.7152f * static_cast<float>(g) + 0.0722f * static_cast<float>(b)) / 255.0f;
    }
    if (error != nullptr)
    {
        error->clear();
    }
    return true;
}

float SampleHeightmap(const HeightmapImage& image, float u, float v)
{
    const float x = std::clamp(u, 0.0f, 1.0f) * static_cast<float>(image.width - 1u);
    const float y = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(image.height - 1u);
    const uint32_t x0 = static_cast<uint32_t>(std::floor(x));
    const uint32_t y0 = static_cast<uint32_t>(std::floor(y));
    const uint32_t x1 = std::min(x0 + 1u, image.width - 1u);
    const uint32_t y1 = std::min(y0 + 1u, image.height - 1u);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto at = [&](uint32_t px, uint32_t py) {
        return image.values[static_cast<size_t>(py) * image.width + px];
    };
    const float a = std::lerp(at(x0, y0), at(x1, y0), tx);
    const float b = std::lerp(at(x0, y1), at(x1, y1), tx);
    return std::lerp(a, b, ty);
}

std::string OperationPipelineSummary(const SdfPipeline& pipeline)
{
    if (pipeline.operations.empty())
    {
        return "";
    }
    return std::format(" -> {} op{}", pipeline.operations.size(), pipeline.operations.size() == 1 ? "" : "s");
}

template <typename Settings>
int EffectiveMeshResolution(const Settings& settings)
{
    const int divisor = 1 << std::clamp(settings.lod, 0, 4);
    return std::clamp(settings.resolution / divisor, 16, 96);
}

struct QuantizedVertex
{
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const QuantizedVertex& other) const
    {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct QuantizedVertexHash
{
    size_t operator()(const QuantizedVertex& value) const
    {
        size_t h = static_cast<size_t>(value.x) * 73856093u;
        h ^= static_cast<size_t>(value.y) * 19349663u;
        h ^= static_cast<size_t>(value.z) * 83492791u;
        return h;
    }
};

uint64_t EdgeKey(uint32_t a, uint32_t b)
{
    const uint32_t lo = std::min(a, b);
    const uint32_t hi = std::max(a, b);
    return (static_cast<uint64_t>(lo) << 32) | hi;
}

void AddEdge(MeshData& mesh, std::unordered_set<uint64_t>& edgeKeys, uint32_t a, uint32_t b)
{
    const uint64_t key = EdgeKey(a, b);
    if (edgeKeys.insert(key).second)
    {
        mesh.edges.push_back({std::min(a, b), std::max(a, b)});
    }
}

void AccumulateNormal(MeshVertex& vertex, float nx, float ny, float nz)
{
    vertex.nx += nx;
    vertex.ny += ny;
    vertex.nz += nz;
}

void ApplySdfGradientNormals(const GraphSettings& settings, const SdfPipeline& pipeline, const SdfPreviewStats& sdf, MeshData& mesh)
{
    const float e = std::max(sdf.voxelSize * 0.35f, 0.001f);
    for (MeshVertex& vertex : mesh.vertices)
    {
        const float dx = EvaluateSdfAt(settings, pipeline, vertex.x + e, vertex.y, vertex.z) -
            EvaluateSdfAt(settings, pipeline, vertex.x - e, vertex.y, vertex.z);
        const float dy = EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y + e, vertex.z) -
            EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y - e, vertex.z);
        const float dz = EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y, vertex.z + e) -
            EvaluateSdfAt(settings, pipeline, vertex.x, vertex.y, vertex.z - e);
        const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (length > 0.000001f)
        {
            vertex.nx = dx / length;
            vertex.ny = dy / length;
            vertex.nz = dz / length;
        }
    }
}

MeshData BuildMeshFromSdf(const GraphSettings& settings, const SdfPipeline& pipeline, const SdfPreviewStats& sdf)
{
    MeshData mesh;
    mesh.vertices.reserve(sdf.surfaceTriangles.size());
    mesh.triangles.reserve(sdf.surfaceTriangles.size());
    mesh.edges.reserve(sdf.surfaceTriangles.size() * 3);

    std::unordered_map<QuantizedVertex, uint32_t, QuantizedVertexHash> vertexMap;
    std::unordered_set<uint64_t> edgeKeys;
    constexpr float kQuantizeScale = 10000.0f;

    const auto vertexIndex = [&](float x, float y, float z) -> uint32_t {
        const QuantizedVertex key{
            static_cast<int>(std::lround(x * kQuantizeScale)),
            static_cast<int>(std::lround(y * kQuantizeScale)),
            static_cast<int>(std::lround(z * kQuantizeScale)),
        };
        if (const auto it = vertexMap.find(key); it != vertexMap.end())
        {
            return it->second;
        }

        const uint32_t index = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back({x, y, z, 0.0f, 0.0f, 0.0f});
        vertexMap.emplace(key, index);
        return index;
    };

    for (const SurfaceTriangle& triangle : sdf.surfaceTriangles)
    {
        const uint32_t a = vertexIndex(triangle.ax, triangle.ay, triangle.az);
        const uint32_t b = vertexIndex(triangle.bx, triangle.by, triangle.bz);
        const uint32_t c = vertexIndex(triangle.cx, triangle.cy, triangle.cz);
        if (a == b || b == c || c == a)
        {
            continue;
        }

        const float ux = triangle.bx - triangle.ax;
        const float uy = triangle.by - triangle.ay;
        const float uz = triangle.bz - triangle.az;
        const float vx = triangle.cx - triangle.ax;
        const float vy = triangle.cy - triangle.ay;
        const float vz = triangle.cz - triangle.az;
        const float nx = uy * vz - uz * vy;
        const float ny = uz * vx - ux * vz;
        const float nz = ux * vy - uy * vx;

        AccumulateNormal(mesh.vertices[a], nx, ny, nz);
        AccumulateNormal(mesh.vertices[b], nx, ny, nz);
        AccumulateNormal(mesh.vertices[c], nx, ny, nz);
        mesh.triangles.push_back({a, b, c});
        AddEdge(mesh, edgeKeys, a, b);
        AddEdge(mesh, edgeKeys, b, c);
        AddEdge(mesh, edgeKeys, c, a);
    }

    for (MeshVertex& vertex : mesh.vertices)
    {
        const float length = std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz);
        if (length > 0.000001f)
        {
            vertex.nx /= length;
            vertex.ny /= length;
            vertex.nz /= length;
        }
        else
        {
            vertex.nx = 0.0f;
            vertex.ny = 1.0f;
            vertex.nz = 0.0f;
        }
    }

    ApplySdfGradientNormals(settings, pipeline, sdf, mesh);

    return mesh;
}

MeshData BuildMeshFromHeightmap(const HeightmapLoadSettings& settings, int resolution, std::string* message)
{
    MeshData mesh;
    HeightmapImage image;
    std::string error;
    if (!LoadHeightmapImage(settings.path, image, &error))
    {
        if (message != nullptr)
        {
            *message = error;
        }
        return mesh;
    }

    const int gridResolution = std::clamp(resolution, 2, 256);
    const float terrainSize = std::max(1.0f, settings.scaleMeters);
    const float verticalRange = terrainSize * std::max(0.0f, settings.relativeVerticalScalePercent) / 100.0f;
    const float verticalOffset = settings.verticalOffsetMeters;
    const float halfSize = terrainSize * 0.5f;

    mesh.vertices.reserve(static_cast<size_t>(gridResolution) * static_cast<size_t>(gridResolution));
    mesh.triangles.reserve(static_cast<size_t>(gridResolution - 1) * static_cast<size_t>(gridResolution - 1) * 2u);
    mesh.edges.reserve(mesh.triangles.capacity() * 3u);

    for (int z = 0; z < gridResolution; ++z)
    {
        const float v = gridResolution > 1 ? static_cast<float>(z) / static_cast<float>(gridResolution - 1) : 0.0f;
        for (int x = 0; x < gridResolution; ++x)
        {
            const float u = gridResolution > 1 ? static_cast<float>(x) / static_cast<float>(gridResolution - 1) : 0.0f;
            const float height = SampleHeightmap(image, u, v);
            mesh.vertices.push_back({
                std::lerp(-halfSize, halfSize, u),
                verticalOffset + height * verticalRange,
                std::lerp(halfSize, -halfSize, v),
                0.0f,
                0.0f,
                0.0f,
            });
        }
    }

    std::unordered_set<uint64_t> edgeKeys;
    const auto indexAt = [gridResolution](int x, int z) {
        return static_cast<uint32_t>(z * gridResolution + x);
    };
    const auto addTriangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        const MeshVertex& va = mesh.vertices[a];
        const MeshVertex& vb = mesh.vertices[b];
        const MeshVertex& vc = mesh.vertices[c];
        const float ux = vb.x - va.x;
        const float uy = vb.y - va.y;
        const float uz = vb.z - va.z;
        const float vx = vc.x - va.x;
        const float vy = vc.y - va.y;
        const float vz = vc.z - va.z;
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        if (ny < 0.0f)
        {
            std::swap(b, c);
            nx = -nx;
            ny = -ny;
            nz = -nz;
        }
        AccumulateNormal(mesh.vertices[a], nx, ny, nz);
        AccumulateNormal(mesh.vertices[b], nx, ny, nz);
        AccumulateNormal(mesh.vertices[c], nx, ny, nz);
        mesh.triangles.push_back({a, b, c});
        AddEdge(mesh, edgeKeys, a, b);
        AddEdge(mesh, edgeKeys, b, c);
        AddEdge(mesh, edgeKeys, c, a);
    };

    for (int z = 0; z < gridResolution - 1; ++z)
    {
        for (int x = 0; x < gridResolution - 1; ++x)
        {
            const uint32_t a = indexAt(x, z);
            const uint32_t b = indexAt(x + 1, z);
            const uint32_t c = indexAt(x + 1, z + 1);
            const uint32_t d = indexAt(x, z + 1);
            addTriangle(a, b, c);
            addTriangle(a, c, d);
        }
    }

    for (MeshVertex& vertex : mesh.vertices)
    {
        const float length = std::sqrt(vertex.nx * vertex.nx + vertex.ny * vertex.ny + vertex.nz * vertex.nz);
        if (length > 0.000001f)
        {
            vertex.nx /= length;
            vertex.ny /= length;
            vertex.nz /= length;
        }
        else
        {
            vertex.nx = 0.0f;
            vertex.ny = 1.0f;
            vertex.nz = 0.0f;
        }
    }

    if (message != nullptr)
    {
        *message = std::format(
            "heightmap {}x{} -> terrain {}x{} ({:.1f} m)",
            image.width,
            image.height,
            gridResolution,
            gridResolution,
            settings.scaleMeters);
    }
    return mesh;
}
} // namespace

NodeGraph NodeGraph::CreateDefaultTerrainGraph()
{
    NodeGraph graph;
    graph.Evaluate();
    return graph;
}

const std::vector<Node>& NodeGraph::Nodes() const
{
    return nodes_;
}

const std::vector<Link>& NodeGraph::Links() const
{
    return links_;
}

GraphSettings& NodeGraph::Settings()
{
    return settings_;
}

const GraphSettings& NodeGraph::Settings() const
{
    return settings_;
}

const EvaluationSummary& NodeGraph::Evaluation() const
{
    return evaluation_;
}

OutputMeshSettings* NodeGraph::FindOutputMeshSettings(GraphId nodeId)
{
    Node* node = FindMutableNode(nodeId);
    return node != nullptr && node->kind == NodeKind::OutputMesh ? &node->outputMesh : nullptr;
}

const OutputMeshSettings* NodeGraph::FindOutputMeshSettings(GraphId nodeId) const
{
    const Node* node = FindNode(nodeId);
    return node != nullptr && node->kind == NodeKind::OutputMesh ? &node->outputMesh : nullptr;
}

const OutputMeshSettings& NodeGraph::OutputMeshSettingsFor(GraphId nodeId) const
{
    if (const OutputMeshSettings* settings = FindOutputMeshSettings(nodeId))
    {
        return *settings;
    }
    if (const Node* outputNode = FindFirstNode(NodeKind::OutputMesh))
    {
        return outputNode->outputMesh;
    }

    static constexpr OutputMeshSettings fallback{};
    return fallback;
}

const Pin* NodeGraph::FindPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        const auto input = std::ranges::find_if(node.inputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (input != node.inputs.end())
        {
            return &*input;
        }

        const auto output = std::ranges::find_if(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; });
        if (output != node.outputs.end())
        {
            return &*output;
        }
    }

    return nullptr;
}

const Node* NodeGraph::FindNode(GraphId nodeId) const
{
    const auto it = std::ranges::find_if(nodes_, [nodeId](const Node& node) { return node.id == nodeId; });
    return it == nodes_.end() ? nullptr : &*it;
}

bool NodeGraph::IsInputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Input;
}

bool NodeGraph::IsOutputPin(GraphId pinId) const
{
    const Pin* pin = FindPin(pinId);
    return pin != nullptr && pin->kind == PinKind::Output;
}

bool NodeGraph::PinHasLink(GraphId pinId) const
{
    return std::ranges::any_of(links_, [pinId](const Link& link) {
        return link.startPin == pinId || link.endPin == pinId;
    });
}

bool NodeGraph::CanCreateLink(GraphId startPin, GraphId endPin) const
{
    if (startPin == 0 || endPin == 0 || startPin == endPin)
    {
        return false;
    }

    const Pin* start = FindPin(startPin);
    const Pin* end = FindPin(endPin);
    if (start == nullptr || end == nullptr || start->nodeId == end->nodeId || start->valueType != end->valueType)
    {
        return false;
    }

    return (start->kind == PinKind::Output && end->kind == PinKind::Input) ||
           (start->kind == PinKind::Input && end->kind == PinKind::Output);
}

bool NodeGraph::CreateLink(GraphId startPin, GraphId endPin)
{
    if (!CanCreateLink(startPin, endPin))
    {
        return false;
    }

    if (IsInputPin(startPin))
    {
        std::swap(startPin, endPin);
    }

    std::erase_if(links_, [endPin](const Link& link) {
        return link.endPin == endPin;
    });
    links_.push_back({nextLinkId_++, startPin, endPin});
    MarkDirty("Link changed");
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId)
{
    const auto oldSize = links_.size();
    std::erase_if(links_, [linkId](const Link& link) { return link.id == linkId; });
    if (links_.size() == oldSize)
    {
        return false;
    }

    MarkDirty("Link deleted");
    return true;
}

bool NodeGraph::DeleteNode(GraphId nodeId)
{
    const Node* node = FindNode(nodeId);
    if (node == nullptr)
    {
        return false;
    }

    std::vector<GraphId> pinIds;
    pinIds.reserve(node->inputs.size() + node->outputs.size());
    for (const Pin& pin : node->inputs)
    {
        pinIds.push_back(pin.id);
    }
    for (const Pin& pin : node->outputs)
    {
        pinIds.push_back(pin.id);
    }

    std::erase_if(links_, [&](const Link& link) {
        return std::ranges::find(pinIds, link.startPin) != pinIds.end() ||
               std::ranges::find(pinIds, link.endPin) != pinIds.end();
    });
    std::erase_if(nodes_, [nodeId](const Node& candidate) {
        return candidate.id == nodeId;
    });
    if (evaluation_.previewNodeId == nodeId)
    {
        evaluation_.previewNodeId = 0;
    }
    MarkDirty("Node deleted");
    return true;
}

GraphId NodeGraph::CreateNode(NodeKind kind)
{
    const GraphId nodeId = AddNode(kind, std::string(ToString(kind)));
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        AddPin(nodeId, PinKind::Output, ValueType::SdfGrid, "SDFGrid");
        break;
    case NodeKind::NoiseWarp:
    case NodeKind::CrackField:
        AddPin(nodeId, PinKind::Input, ValueType::SdfGrid, "SDFGrid");
        AddPin(nodeId, PinKind::Output, ValueType::SdfGrid, "SDFGrid");
        break;
    case NodeKind::OutputMesh:
        AddPin(nodeId, PinKind::Input, ValueType::SdfGrid, "SDFGrid");
        break;
    case NodeKind::HeightmapLoad:
        AddPin(nodeId, PinKind::Output, ValueType::HeightField, "HeightField");
        break;
    default:
        break;
    }
    MarkDirty("Node added");
    return nodeId;
}

void NodeGraph::ReplaceNodes(std::vector<Node> nodes)
{
    nodes_ = std::move(nodes);
    nextNodeId_ = 1;
    nextPinId_ = 11;
    for (const Node& node : nodes_)
    {
        nextNodeId_ = std::max(nextNodeId_, node.id + 1);
        for (const Pin& pin : node.inputs)
        {
            nextPinId_ = std::max(nextPinId_, pin.id + 1);
        }
        for (const Pin& pin : node.outputs)
        {
            nextPinId_ = std::max(nextPinId_, pin.id + 1);
        }
    }
    MarkDirty("Project nodes loaded");
}

void NodeGraph::ReplaceLinks(std::vector<Link> links)
{
    links_ = std::move(links);
    nextLinkId_ = 101;
    for (const Link& link : links_)
    {
        nextLinkId_ = std::max(nextLinkId_, link.id + 1);
    }
    MarkDirty("Project links loaded");
}

bool NodeGraph::SetPreviewStage(PreviewStage stage)
{
    if (evaluation_.previewStage == stage)
    {
        return false;
    }

    evaluation_.previewStage = stage;
    evaluation_.dirty = true;
    evaluation_.status = std::format("Preview stage changed to {}", ToString(stage));
    return true;
}

bool NodeGraph::SetPreviewNode(GraphId nodeId)
{
    const Node* node = FindNode(nodeId);
    if (node == nullptr)
    {
        return false;
    }

    const PreviewStage stage = PreviewStageFor(node->kind);
    if (evaluation_.previewNodeId == nodeId && evaluation_.previewStage == stage)
    {
        return false;
    }

    evaluation_.previewNodeId = nodeId;
    evaluation_.previewStage = stage;
    evaluation_.dirty = true;
    evaluation_.status = std::format("Preview node changed to {}", node->title);
    return true;
}

PreviewStage NodeGraph::Preview() const
{
    return evaluation_.previewStage;
}

SdfPipeline NodeGraph::PipelineFor(PreviewStage stage) const
{
    switch (stage)
    {
    case PreviewStage::Primitive:
        return PipelineTo(NodeKind::PrimitiveSdf);
    case PreviewStage::Noise:
        return PipelineTo(NodeKind::NoiseWarp);
    case PreviewStage::Crack:
        return PipelineTo(NodeKind::CrackField);
    case PreviewStage::Output:
    default:
        return PipelineTo(NodeKind::OutputMesh);
    }
}

SdfPipeline NodeGraph::PreviewPipeline() const
{
    if (const Node* previewNode = FindNode(evaluation_.previewNodeId))
    {
        return PipelineToNode(*previewNode);
    }
    return PipelineFor(evaluation_.previewStage);
}

SdfPipeline NodeGraph::FinalPipeline() const
{
    return PipelineFor(PreviewStage::Output);
}

void NodeGraph::MarkDirty(std::string_view reason)
{
    evaluation_.dirty = true;
    evaluation_.finalDirty = true;
    evaluation_.status = std::string(reason);
}

const Node* NodeGraph::FindFirstNode(NodeKind kind) const
{
    const auto it = std::ranges::find_if(nodes_, [kind](const Node& node) {
        return node.kind == kind;
    });
    return it != nodes_.end() ? &*it : nullptr;
}

Node* NodeGraph::FindMutableNode(GraphId nodeId)
{
    const auto it = std::ranges::find_if(nodes_, [nodeId](const Node& node) {
        return node.id == nodeId;
    });
    return it != nodes_.end() ? &*it : nullptr;
}

const Node* NodeGraph::FindNodeByOutputPin(GraphId pinId) const
{
    for (const Node& node : nodes_)
    {
        if (std::ranges::any_of(node.outputs, [pinId](const Pin& pin) { return pin.id == pinId; }))
        {
            return &node;
        }
    }
    return nullptr;
}

const Node* NodeGraph::FindUpstreamNode(const Node& node) const
{
    if (node.inputs.empty())
    {
        return nullptr;
    }

    const GraphId inputPin = node.inputs.front().id;
    const auto linkIt = std::find_if(links_.rbegin(), links_.rend(), [inputPin](const Link& link) {
        return link.endPin == inputPin;
    });
    if (linkIt == links_.rend())
    {
        return nullptr;
    }

    return FindNodeByOutputPin(linkIt->startPin);
}

SdfPipeline NodeGraph::PipelineTo(NodeKind targetKind) const
{
    const Node* node = FindFirstNode(targetKind);
    return node != nullptr ? PipelineToNode(*node) : SdfPipeline{};
}

SdfPipeline NodeGraph::PipelineToNode(const Node& targetNode) const
{
    SdfPipeline pipeline;
    const Node* node = &targetNode;
    int guard = 0;
    while (node != nullptr && guard++ < 16)
    {
        if (node->kind == NodeKind::NoiseWarp)
        {
            pipeline.noiseLayers.push_back(node->noise);
            pipeline.operations.push_back({
                SdfPipeline::OperationKind::NoiseWarp,
                node->id,
                node->noise,
                {},
                0.0f,
            });
        }
        else if (node->kind == NodeKind::CrackField)
        {
            pipeline.useCrack = true;
            pipeline.crack = node->crack;
            pipeline.operations.push_back({
                SdfPipeline::OperationKind::CrackField,
                node->id,
                {},
                node->crack,
                0.0f,
            });
        }
        else if (node->kind == NodeKind::OutputMesh)
        {
            pipeline.applyOutputIso = true;
            pipeline.outputIsoValue = node->outputMesh.isoValue;
            pipeline.operations.push_back({
                SdfPipeline::OperationKind::OutputIso,
                node->id,
                {},
                {},
                node->outputMesh.isoValue,
            });
        }
        else if (node->kind == NodeKind::PrimitiveSdf)
        {
            pipeline.hasSource = true;
            pipeline.primitiveKind = node->primitive.kind;
            break;
        }
        else if (node->kind == NodeKind::HeightmapLoad)
        {
            pipeline.hasSource = true;
            pipeline.useHeightmap = true;
            pipeline.heightmap = node->heightmap;
            break;
        }

        node = FindUpstreamNode(*node);
    }
    std::ranges::reverse(pipeline.noiseLayers);
    std::ranges::reverse(pipeline.operations);
    pipeline.useNoise = !pipeline.noiseLayers.empty();
    if (pipeline.useNoise)
    {
        pipeline.noise = pipeline.noiseLayers.back();
    }
    return pipeline;
}

void NodeGraph::Evaluate(int previewMeshResolution)
{
    if (previewMeshResolution <= 0)
    {
        previewMeshResolution = EffectiveMeshResolution(settings_.preview);
    }
    const SdfPipeline previewPipeline = PreviewPipeline();
    const SdfPipeline finalPipeline = FinalPipeline();
    evaluation_.previewIsHeightmap = previewPipeline.useHeightmap;
    evaluation_.previewMessage.clear();
    if (!previewPipeline.hasSource)
    {
        evaluation_.previewSdf = {};
        evaluation_.previewMesh = {};
        evaluation_.previewMessage = "No source node";
    }
    else if (previewPipeline.useHeightmap)
    {
        evaluation_.previewSdf = {};
        evaluation_.previewMesh = BuildMeshFromHeightmap(previewPipeline.heightmap, previewMeshResolution, &evaluation_.previewMessage);
    }
    else
    {
        evaluation_.previewSdf = BuildDenseSdfPreview(settings_, previewPipeline, previewMeshResolution);
        evaluation_.previewMesh = BuildMeshFromSdf(settings_, previewPipeline, evaluation_.previewSdf);
    }
    ++evaluation_.version;
    evaluation_.dirty = false;
    if (!previewPipeline.hasSource)
    {
        evaluation_.status = "No source node";
    }
    else if (previewPipeline.useHeightmap)
    {
        evaluation_.status = std::format(
            "Heightmap preview [{}] -> {} verts / {} tris{}{}",
            evaluation_.previewMessage,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size(),
            evaluation_.finalDirty ? " / output mesh pending" : "",
            evaluation_.previewMesh.vertices.empty() ? " / no mesh" : "");
    }
    else
    {
        evaluation_.status = std::format(
            "{} preview -> {}{}{} -> preview LOD {} / output LOD {} / iso {:.3f} -> {} verts / {} tris{}",
            ToString(evaluation_.previewStage),
            ToString(previewPipeline.primitiveKind),
            OperationPipelineSummary(finalPipeline),
            "",
            settings_.preview.lod,
            OutputMeshSettingsFor().lod,
            OutputMeshSettingsFor().isoValue,
            evaluation_.previewMesh.vertices.size(),
            evaluation_.previewMesh.triangles.size(),
            evaluation_.finalDirty ? " / output mesh pending" : "");
    }
}

void NodeGraph::EvaluateFinal(GraphId outputNodeId)
{
    const OutputMeshSettings& outputMesh = OutputMeshSettingsFor(outputNodeId);
    const int outputMeshResolution = EffectiveMeshResolution(outputMesh);
    const Node* outputNode = FindNode(outputNodeId);
    SdfPipeline finalPipeline = outputNode != nullptr && outputNode->kind == NodeKind::OutputMesh
        ? PipelineToNode(*outputNode)
        : FinalPipeline();
    if (finalPipeline.applyOutputIso)
    {
        finalPipeline.outputIsoValue = outputMesh.isoValue;
        for (SdfPipeline::Operation& operation : finalPipeline.operations)
        {
            if (operation.kind == SdfPipeline::OperationKind::OutputIso)
            {
                operation.isoValue = outputMesh.isoValue;
            }
        }
    }
    GraphSettings finalSettings = settings_;
    if (!finalPipeline.hasSource)
    {
        evaluation_.finalSdf = {};
        evaluation_.finalMesh = {};
    }
    else if (finalPipeline.useHeightmap)
    {
        evaluation_.finalSdf = {};
        evaluation_.finalMesh = BuildMeshFromHeightmap(finalPipeline.heightmap, outputMeshResolution, nullptr);
    }
    else
    {
        evaluation_.finalSdf = BuildDenseSdfPreview(finalSettings, finalPipeline, outputMeshResolution);
        evaluation_.finalMesh = BuildMeshFromSdf(finalSettings, finalPipeline, evaluation_.finalSdf);
    }
    ++evaluation_.finalVersion;
    evaluation_.finalDirty = false;
    evaluation_.status = std::format(
        "{} preview / output mesh {}^3 LOD {} iso {:.3f} -> {} verts / {} tris",
        ToString(evaluation_.previewStage),
        evaluation_.finalSdf.resolution,
        outputMesh.lod,
        outputMesh.isoValue,
        evaluation_.finalMesh.vertices.size(),
        evaluation_.finalMesh.triangles.size());
}

GraphId NodeGraph::AddNode(NodeKind kind, std::string title)
{
    const GraphId id = nextNodeId_++;
    nodes_.push_back({id, kind, std::move(title), {}, {}});
    return id;
}

GraphId NodeGraph::AddPin(GraphId nodeId, PinKind kind, ValueType valueType, std::string label)
{
    Node* node = nullptr;
    for (Node& candidate : nodes_)
    {
        if (candidate.id == nodeId)
        {
            node = &candidate;
            break;
        }
    }

    if (node == nullptr)
    {
        return 0;
    }

    const GraphId id = nextPinId_++;
    Pin pin{id, nodeId, kind, valueType, std::move(label)};
    if (kind == PinKind::Input)
    {
        node->inputs.push_back(std::move(pin));
    }
    else
    {
        node->outputs.push_back(std::move(pin));
    }
    return id;
}

void NodeGraph::AddInitialLink(GraphId startPin, GraphId endPin)
{
    links_.push_back({nextLinkId_++, startPin, endPin});
}

std::string_view ToString(PrimitiveKind kind)
{
    switch (kind)
    {
    case PrimitiveKind::Sphere:
        return "Sphere";
    case PrimitiveKind::Box:
        return "Box";
    case PrimitiveKind::Capsule:
        return "Capsule";
    case PrimitiveKind::Ellipsoid:
        return "Ellipsoid";
    case PrimitiveKind::RockBlob:
        return "Rock Blob";
    default:
        return "Unknown";
    }
}

std::string_view ToString(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        return "Primitive SDF";
    case NodeKind::NoiseWarp:
        return "Noise Warp";
    case NodeKind::CrackField:
        return "Crack Field";
    case NodeKind::OutputMesh:
        return "Output Mesh";
    case NodeKind::HeightmapLoad:
        return "Load Heightmap";
    default:
        return "Unknown";
    }
}

std::string_view ToString(PreviewStage stage)
{
    switch (stage)
    {
    case PreviewStage::Primitive:
        return "Primitive";
    case PreviewStage::Noise:
        return "Noise Warp";
    case PreviewStage::Crack:
        return "Crack Field";
    case PreviewStage::Output:
        return "Output Mesh";
    default:
        return "Unknown";
    }
}

std::string_view ToString(ValueType type)
{
    switch (type)
    {
    case ValueType::SdfGrid:
        return "SDFGrid";
    case ValueType::Mesh:
        return "Mesh";
    case ValueType::HeightField:
        return "HeightField";
    default:
        return "Unknown";
    }
}

PreviewStage PreviewStageFor(NodeKind kind)
{
    switch (kind)
    {
    case NodeKind::PrimitiveSdf:
        return PreviewStage::Primitive;
    case NodeKind::NoiseWarp:
        return PreviewStage::Noise;
    case NodeKind::CrackField:
        return PreviewStage::Crack;
    case NodeKind::HeightmapLoad:
        return PreviewStage::Primitive;
    case NodeKind::OutputMesh:
    default:
        return PreviewStage::Output;
    }
}

} // namespace rock
