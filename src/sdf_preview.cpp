#include "sdf_preview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace rock
{
namespace
{
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

float Length(Vec3 v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

float Dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Abs(Vec3 v)
{
    return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)};
}

Vec3 Sub(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Max(Vec3 v, float s)
{
    return {std::max(v.x, s), std::max(v.y, s), std::max(v.z, s)};
}

float MaxComponent(Vec3 v)
{
    return std::max(v.x, std::max(v.y, v.z));
}

Vec3 GridPoint(int x, int y, int z, float voxelSize)
{
    return {
        -1.0f + static_cast<float>(x) * voxelSize,
        -1.0f + static_cast<float>(y) * voxelSize,
        -1.0f + static_cast<float>(z) * voxelSize,
    };
}

size_t GridIndex(int x, int y, int z, int resolution)
{
    return static_cast<size_t>((z * resolution + y) * resolution + x);
}

float Hash(Vec3 p)
{
    const float n = std::sin(Dot(p, {12.9898f, 78.233f, 37.719f})) * 43758.5453f;
    return n - std::floor(n);
}

float ValueNoise(Vec3 p)
{
    Vec3 i{std::floor(p.x), std::floor(p.y), std::floor(p.z)};
    Vec3 f{p.x - i.x, p.y - i.y, p.z - i.z};
    f = {
        f.x * f.x * (3.0f - 2.0f * f.x),
        f.y * f.y * (3.0f - 2.0f * f.y),
        f.z * f.z * (3.0f - 2.0f * f.z),
    };

    auto sample = [&](float x, float y, float z) {
        return Hash({i.x + x, i.y + y, i.z + z});
    };

    const float x00 = std::lerp(sample(0, 0, 0), sample(1, 0, 0), f.x);
    const float x10 = std::lerp(sample(0, 1, 0), sample(1, 1, 0), f.x);
    const float x01 = std::lerp(sample(0, 0, 1), sample(1, 0, 1), f.x);
    const float x11 = std::lerp(sample(0, 1, 1), sample(1, 1, 1), f.x);
    const float y0 = std::lerp(x00, x10, f.y);
    const float y1 = std::lerp(x01, x11, f.y);
    return std::lerp(y0, y1, f.z) * 2.0f - 1.0f;
}

float Fbm(Vec3 p, int octaves)
{
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 1.0f;
    float total = 0.0f;
    for (int i = 0; i < std::clamp(octaves, 1, 8); ++i)
    {
        value += ValueNoise({p.x * frequency, p.y * frequency, p.z * frequency}) * amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }
    return total > 0.0f ? value / total : value;
}

float BoxSdf(Vec3 p, Vec3 halfExtents)
{
    Vec3 q = Sub(Abs(p), halfExtents);
    return Length(Max(q, 0.0f)) + std::min(MaxComponent(q), 0.0f);
}

float CapsuleSdf(Vec3 p)
{
    const float halfHeight = 0.42f;
    p.y -= std::clamp(p.y, -halfHeight, halfHeight);
    return Length(p) - 0.38f;
}

float PrimitiveSdf(Vec3 p, PrimitiveKind kind)
{
    switch (kind)
    {
    case PrimitiveKind::Sphere:
        return Length(p) - 0.62f;
    case PrimitiveKind::Box:
        return BoxSdf(p, {0.48f, 0.42f, 0.55f});
    case PrimitiveKind::Capsule:
        return CapsuleSdf(p);
    case PrimitiveKind::Ellipsoid:
        return (Length({p.x / 0.72f, p.y / 0.48f, p.z / 0.56f}) - 1.0f) * 0.56f;
    case PrimitiveKind::RockBlob:
    default:
        return Length({p.x / 0.70f, p.y / 0.55f, p.z / 0.62f}) * 0.58f - 0.58f;
    }
}

float ApplyNoise(float sdf, Vec3 p, const NoiseSettings& noise)
{
    const float seed = static_cast<float>(std::clamp(noise.seed, 0, 999999));
    const Vec3 offset{
        seed * 12.9898f,
        seed * 78.233f,
        seed * 37.719f,
    };
    const float n = Fbm({
        p.x * noise.frequency + offset.x,
        p.y * noise.frequency + offset.y,
        p.z * noise.frequency + offset.z,
    }, noise.octaves);
    return sdf + n * noise.amplitude * 0.12f;
}

float ApplyCracks(float sdf, Vec3 p, const CrackSettings& crack)
{
    const Vec3 normals[] = {
        {0.92f, 0.18f, 0.34f},
        {-0.25f, 0.96f, 0.11f},
        {0.16f, -0.38f, 0.91f},
    };
    const float offsets[] = {-0.17f, 0.11f, 0.29f};

    float crackSdf = -1.0f;
    for (int i = 0; i < 3; ++i)
    {
        const float rough = Fbm({p.x * 7.0f + i * 13.0f, p.y * 7.0f, p.z * 7.0f}, 3) * crack.roughness * 0.045f;
        const float plane = crack.width - std::fabs(Dot(p, normals[i]) + offsets[i] + rough);
        crackSdf = std::max(crackSdf, plane * (0.6f + crack.depth));
    }

    return std::max(sdf, crackSdf);
}

float EvaluatePipelineSdf(Vec3 p, const GraphSettings& settings, const SdfPipeline& pipeline)
{
    (void)settings;
    float sdf = PrimitiveSdf(p, pipeline.primitiveKind);

    if (!pipeline.operations.empty())
    {
        for (const SdfPipeline::Operation& operation : pipeline.operations)
        {
            switch (operation.kind)
            {
            case SdfPipeline::OperationKind::NoiseWarp:
                sdf = ApplyNoise(sdf, p, operation.noise);
                break;
            case SdfPipeline::OperationKind::CrackField:
                sdf = ApplyCracks(sdf, p, operation.crack);
                break;
            case SdfPipeline::OperationKind::OutputIso:
                sdf -= operation.isoValue;
                break;
            }
        }
    }
    else if (!pipeline.noiseLayers.empty())
    {
        for (const NoiseSettings& noise : pipeline.noiseLayers)
        {
            sdf = ApplyNoise(sdf, p, noise);
        }
    }
    else if (pipeline.useNoise)
    {
        sdf = ApplyNoise(sdf, p, pipeline.noise);
    }

    if (pipeline.operations.empty() && pipeline.useCrack)
    {
        sdf = ApplyCracks(sdf, p, pipeline.crack);
    }

    if (pipeline.operations.empty() && pipeline.applyOutputIso)
    {
        sdf -= pipeline.outputIsoValue;
    }
    return sdf;
}

Vec3 Add(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 Scale(Vec3 v, float s)
{
    return {v.x * s, v.y * s, v.z * s};
}

Vec3 Lerp(Vec3 a, Vec3 b, float t)
{
    return Add(a, Scale(Sub(b, a), t));
}

size_t CellIndex(int x, int y, int z, int cellResolution)
{
    return static_cast<size_t>((z * cellResolution + y) * cellResolution + x);
}

void BuildSurfaceNetGeometry(SdfPreviewStats& stats, const std::vector<float>& sdfValues)
{
    const int cellResolution = stats.resolution - 1;
    if (cellResolution <= 0)
    {
        return;
    }

    constexpr size_t kMaxSurfaceSegments = 60000;
    constexpr size_t kMaxSurfaceTriangles = 60000;
    constexpr std::array<std::array<int, 2>, 12> edges{{
        {{0, 1}}, {{1, 3}}, {{3, 2}}, {{2, 0}},
        {{4, 5}}, {{5, 7}}, {{7, 6}}, {{6, 4}},
        {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
    }};
    constexpr std::array<std::array<int, 3>, 8> cornerOffsets{{
        {{0, 0, 0}}, {{1, 0, 0}}, {{0, 1, 0}}, {{1, 1, 0}},
        {{0, 0, 1}}, {{1, 0, 1}}, {{0, 1, 1}}, {{1, 1, 1}},
    }};

    std::vector<int> cellVertices(static_cast<size_t>(cellResolution * cellResolution * cellResolution), -1);
    std::vector<Vec3> vertices;
    vertices.reserve(static_cast<size_t>(cellResolution * cellResolution));

    const auto cellVertex = [&](int x, int y, int z) -> int& {
        return cellVertices[CellIndex(x, y, z, cellResolution)];
    };

    for (int z = 0; z < cellResolution; ++z)
    {
        for (int y = 0; y < cellResolution; ++y)
        {
            for (int x = 0; x < cellResolution; ++x)
            {
                std::array<Vec3, 8> positions{};
                std::array<float, 8> values{};
                bool hasInside = false;
                bool hasOutside = false;
                for (int i = 0; i < 8; ++i)
                {
                    const int gx = x + cornerOffsets[i][0];
                    const int gy = y + cornerOffsets[i][1];
                    const int gz = z + cornerOffsets[i][2];
                    positions[i] = GridPoint(gx, gy, gz, stats.voxelSize);
                    values[i] = sdfValues[GridIndex(gx, gy, gz, stats.resolution)];
                    hasInside = hasInside || values[i] < 0.0f;
                    hasOutside = hasOutside || values[i] >= 0.0f;
                }

                if (!hasInside || !hasOutside)
                {
                    continue;
                }

                Vec3 average{};
                int crossingCount = 0;
                for (const auto& edge : edges)
                {
                    const int a = edge[0];
                    const int b = edge[1];
                    if ((values[a] < 0.0f) == (values[b] < 0.0f))
                    {
                        continue;
                    }

                    const float denom = values[a] - values[b];
                    const float t = std::fabs(denom) > 0.000001f ? std::clamp(values[a] / denom, 0.0f, 1.0f) : 0.5f;
                    average = Add(average, Lerp(positions[a], positions[b], t));
                    ++crossingCount;
                }

                if (crossingCount > 0)
                {
                    cellVertex(x, y, z) = static_cast<int>(vertices.size());
                    vertices.push_back(Scale(average, 1.0f / static_cast<float>(crossingCount)));
                }
            }
        }
    }

    const auto validCellVertex = [&](int x, int y, int z) -> int {
        if (x < 0 || y < 0 || z < 0 || x >= cellResolution || y >= cellResolution || z >= cellResolution)
        {
            return -1;
        }
        return cellVertices[CellIndex(x, y, z, cellResolution)];
    };

    const auto addSegment = [&](Vec3 a, Vec3 b) {
        if (stats.surfaceSegments.size() < kMaxSurfaceSegments)
        {
            stats.surfaceSegments.push_back({a.x, a.y, a.z, b.x, b.y, b.z});
        }
    };

    const auto addQuad = [&](int ia, int ib, int ic, int id) {
        if (ia < 0 || ib < 0 || ic < 0 || id < 0 || stats.surfaceTriangles.size() + 2 > kMaxSurfaceTriangles)
        {
            return;
        }

        const Vec3 a = vertices[static_cast<size_t>(ia)];
        const Vec3 b = vertices[static_cast<size_t>(ib)];
        const Vec3 c = vertices[static_cast<size_t>(ic)];
        const Vec3 d = vertices[static_cast<size_t>(id)];
        stats.surfaceTriangles.push_back({a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
        stats.surfaceTriangles.push_back({a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z});
        addSegment(a, b);
        addSegment(b, c);
        addSegment(c, d);
        addSegment(d, a);
    };

    for (int z = 0; z < stats.resolution; ++z)
    {
        for (int y = 0; y < stats.resolution; ++y)
        {
            for (int x = 0; x < stats.resolution; ++x)
            {
                if (x < stats.resolution - 1)
                {
                    const float a = sdfValues[GridIndex(x, y, z, stats.resolution)];
                    const float b = sdfValues[GridIndex(x + 1, y, z, stats.resolution)];
                    if ((a < 0.0f) != (b < 0.0f))
                    {
                        addQuad(
                            validCellVertex(x, y - 1, z - 1),
                            validCellVertex(x, y, z - 1),
                            validCellVertex(x, y, z),
                            validCellVertex(x, y - 1, z));
                    }
                }
                if (y < stats.resolution - 1)
                {
                    const float a = sdfValues[GridIndex(x, y, z, stats.resolution)];
                    const float b = sdfValues[GridIndex(x, y + 1, z, stats.resolution)];
                    if ((a < 0.0f) != (b < 0.0f))
                    {
                        addQuad(
                            validCellVertex(x - 1, y, z - 1),
                            validCellVertex(x, y, z - 1),
                            validCellVertex(x, y, z),
                            validCellVertex(x - 1, y, z));
                    }
                }
                if (z < stats.resolution - 1)
                {
                    const float a = sdfValues[GridIndex(x, y, z, stats.resolution)];
                    const float b = sdfValues[GridIndex(x, y, z + 1, stats.resolution)];
                    if ((a < 0.0f) != (b < 0.0f))
                    {
                        addQuad(
                            validCellVertex(x - 1, y - 1, z),
                            validCellVertex(x, y - 1, z),
                            validCellVertex(x, y, z),
                            validCellVertex(x - 1, y, z));
                    }
                }
            }
        }
    }
}

void BuildVoxelFaceGeometry(SdfPreviewStats& stats, const std::vector<float>& sdfValues)
{
    constexpr size_t kMaxSurfaceSegments = 60000;
    constexpr size_t kMaxSurfaceTriangles = 60000;

    const auto addSegment = [&](Vec3 a, Vec3 b) {
        if (stats.surfaceSegments.size() < kMaxSurfaceSegments)
        {
            stats.surfaceSegments.push_back({a.x, a.y, a.z, b.x, b.y, b.z});
        }
    };

    const auto addQuad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        if (stats.surfaceTriangles.size() + 2 > kMaxSurfaceTriangles)
        {
            return;
        }
        stats.surfaceTriangles.push_back({a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
        stats.surfaceTriangles.push_back({a.x, a.y, a.z, c.x, c.y, c.z, d.x, d.y, d.z});
    };

    for (int z = 0; z < stats.resolution - 1; ++z)
    {
        for (int y = 0; y < stats.resolution - 1; ++y)
        {
            for (int x = 0; x < stats.resolution - 1; ++x)
            {
                if (((x + y + z) % 2) != 0)
                {
                    continue;
                }

                const float center = sdfValues[GridIndex(x, y, z, stats.resolution)];
                const Vec3 p = GridPoint(x, y, z, stats.voxelSize);
                const float half = stats.voxelSize * 0.42f;
                const float sx = sdfValues[GridIndex(x + 1, y, z, stats.resolution)];
                const float sy = sdfValues[GridIndex(x, y + 1, z, stats.resolution)];
                const float sz = sdfValues[GridIndex(x, y, z + 1, stats.resolution)];

                if ((center < 0.0f) != (sx < 0.0f))
                {
                    addSegment({p.x + half, p.y - half, p.z}, {p.x + half, p.y + half, p.z});
                    addSegment({p.x + half, p.y, p.z - half}, {p.x + half, p.y, p.z + half});
                    addQuad(
                        {p.x + half, p.y - half, p.z - half},
                        {p.x + half, p.y + half, p.z - half},
                        {p.x + half, p.y + half, p.z + half},
                        {p.x + half, p.y - half, p.z + half});
                }
                if ((center < 0.0f) != (sy < 0.0f))
                {
                    addSegment({p.x - half, p.y + half, p.z}, {p.x + half, p.y + half, p.z});
                    addSegment({p.x, p.y + half, p.z - half}, {p.x, p.y + half, p.z + half});
                    addQuad(
                        {p.x - half, p.y + half, p.z - half},
                        {p.x + half, p.y + half, p.z - half},
                        {p.x + half, p.y + half, p.z + half},
                        {p.x - half, p.y + half, p.z + half});
                }
                if ((center < 0.0f) != (sz < 0.0f))
                {
                    addSegment({p.x - half, p.y, p.z + half}, {p.x + half, p.y, p.z + half});
                    addSegment({p.x, p.y - half, p.z + half}, {p.x, p.y + half, p.z + half});
                    addQuad(
                        {p.x - half, p.y - half, p.z + half},
                        {p.x + half, p.y - half, p.z + half},
                        {p.x + half, p.y + half, p.z + half},
                        {p.x - half, p.y + half, p.z + half});
                }
            }
        }
    }
}

void BuildPreviewGeometry(SdfPreviewStats& stats, const std::vector<float>& sdfValues, const GraphSettings& settings)
{
    if (settings.preview.displayMode == MeshDisplayMode::Voxels)
    {
        BuildVoxelFaceGeometry(stats, sdfValues);
    }
    else
    {
        BuildSurfaceNetGeometry(stats, sdfValues);
    }
}
} // namespace

float EvaluateSdfAt(const GraphSettings& settings, const SdfPipeline& pipeline, float x, float y, float z)
{
    return EvaluatePipelineSdf({x, y, z}, settings, pipeline);
}

SdfPreviewStats BuildDenseSdfPreview(const GraphSettings& settings, const SdfPipeline& pipeline, int resolution)
{
    SdfPreviewStats stats;
    stats.resolution = std::max(8, resolution);
    stats.sliceResolution = stats.resolution;
    stats.totalVoxels = stats.resolution * stats.resolution * stats.resolution;
    stats.voxelSize = 2.0f / static_cast<float>(stats.resolution - 1);
    stats.minSdf = std::numeric_limits<float>::max();
    stats.maxSdf = std::numeric_limits<float>::lowest();
    stats.centerSlice.assign(static_cast<size_t>(stats.sliceResolution * stats.sliceResolution), 0.0f);
    stats.surfacePoints.reserve(2200);
    stats.surfaceSegments.reserve(4200);
    stats.surfaceTriangles.reserve(3600);
    std::vector<float> sdfValues(static_cast<size_t>(stats.totalVoxels), 0.0f);
    const int sliceZ = stats.resolution / 2;
    const float surfaceBand = stats.voxelSize * 0.82f;
    constexpr size_t kMaxSurfacePoints = 2200;

    for (int z = 0; z < stats.resolution; ++z)
    {
        for (int y = 0; y < stats.resolution; ++y)
        {
            for (int x = 0; x < stats.resolution; ++x)
            {
                Vec3 p = GridPoint(x, y, z, stats.voxelSize);

                float sdf = EvaluatePipelineSdf(p, settings, pipeline);
                sdfValues[GridIndex(x, y, z, stats.resolution)] = sdf;

                stats.minSdf = std::min(stats.minSdf, sdf);
                stats.maxSdf = std::max(stats.maxSdf, sdf);
                if (z == sliceZ)
                {
                    stats.centerSlice[static_cast<size_t>(y * stats.sliceResolution + x)] = sdf;
                }
                if (sdf < 0.0f)
                {
                    ++stats.insideVoxels;
                }
                if (std::fabs(sdf) <= surfaceBand && stats.surfacePoints.size() < kMaxSurfacePoints && ((x + y * 3 + z * 5) % 3 == 0))
                {
                    stats.surfacePoints.push_back({p.x, p.y, p.z, sdf});
                }
            }
        }
    }

    BuildPreviewGeometry(stats, sdfValues, settings);

    stats.fillRatio = stats.totalVoxels > 0 ? static_cast<float>(stats.insideVoxels) / static_cast<float>(stats.totalVoxels) : 0.0f;
    stats.estimatedVolume = static_cast<float>(stats.insideVoxels) * stats.voxelSize * stats.voxelSize * stats.voxelSize;
    return stats;
}

SdfPreviewStats BuildDenseSdfPreviewFromValues(const GraphSettings& settings, int resolution, const std::vector<float>& sdfValues)
{
    SdfPreviewStats stats;
    stats.resolution = std::max(8, resolution);
    stats.sliceResolution = stats.resolution;
    stats.totalVoxels = stats.resolution * stats.resolution * stats.resolution;
    stats.voxelSize = 2.0f / static_cast<float>(stats.resolution - 1);
    stats.minSdf = std::numeric_limits<float>::max();
    stats.maxSdf = std::numeric_limits<float>::lowest();
    stats.centerSlice.assign(static_cast<size_t>(stats.sliceResolution * stats.sliceResolution), 0.0f);
    stats.surfacePoints.reserve(2200);
    stats.surfaceSegments.reserve(4200);
    stats.surfaceTriangles.reserve(3600);
    const int sliceZ = stats.resolution / 2;
    const float surfaceBand = stats.voxelSize * 0.82f;
    constexpr size_t kMaxSurfacePoints = 2200;

    if (sdfValues.size() < static_cast<size_t>(stats.totalVoxels))
    {
        return stats;
    }

    for (int z = 0; z < stats.resolution; ++z)
    {
        for (int y = 0; y < stats.resolution; ++y)
        {
            for (int x = 0; x < stats.resolution; ++x)
            {
                const Vec3 p = GridPoint(x, y, z, stats.voxelSize);
                const float sdf = sdfValues[GridIndex(x, y, z, stats.resolution)];
                stats.minSdf = std::min(stats.minSdf, sdf);
                stats.maxSdf = std::max(stats.maxSdf, sdf);
                if (z == sliceZ)
                {
                    stats.centerSlice[static_cast<size_t>(y * stats.sliceResolution + x)] = sdf;
                }
                if (sdf < 0.0f)
                {
                    ++stats.insideVoxels;
                }
                if (std::fabs(sdf) <= surfaceBand && stats.surfacePoints.size() < kMaxSurfacePoints && ((x + y * 3 + z * 5) % 3 == 0))
                {
                    stats.surfacePoints.push_back({p.x, p.y, p.z, sdf});
                }
            }
        }
    }

    BuildPreviewGeometry(stats, sdfValues, settings);

    stats.fillRatio = stats.totalVoxels > 0 ? static_cast<float>(stats.insideVoxels) / static_cast<float>(stats.totalVoxels) : 0.0f;
    stats.estimatedVolume = static_cast<float>(stats.insideVoxels) * stats.voxelSize * stats.voxelSize * stats.voxelSize;
    return stats;
}
} // namespace rock
