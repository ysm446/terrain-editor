#include "Rock.h"

#include "HeightfieldOps.h"
#include "MaskOps.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace rock
{
namespace
{
RockGpuEvaluator g_rockGpuEvaluator = nullptr;

template <typename Fn>
inline void ParallelForRows(int n, Fn&& fn)
{
    std::vector<int> rows(static_cast<size_t>(n));
    std::iota(rows.begin(), rows.end(), 0);
    std::for_each(std::execution::par, rows.begin(), rows.end(), std::forward<Fn>(fn));
}
namespace rock_node
{
inline uint32_t Hash2(int32_t x, int32_t y, int32_t seed)
{
    uint32_t h = static_cast<uint32_t>(x) * 0x27d4eb2du + static_cast<uint32_t>(y) * 0x9e3779b9u + static_cast<uint32_t>(seed) * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x21f0aaadu;
    h ^= h >> 15;
    h *= 0x735a2d97u;
    h ^= h >> 15;
    return h;
}

inline float HashFloat01(int32_t x, int32_t y, int32_t seed)
{
    return static_cast<float>(Hash2(x, y, seed) & 0xFFFFFFu) / static_cast<float>(0xFFFFFFu);
}

inline int32_t DeriveSeed(int32_t seed, uint32_t multiplier, uint32_t addend)
{
    const uint32_t value = static_cast<uint32_t>(seed) * multiplier + addend;
    return static_cast<int32_t>(value);
}

// One Voronoi pass: jittered grid where each integer cell holds a single
// site at its centre + a per-cell offset in [-0.45, 0.45]. Returns the two
// nearest distances (F1, F2) and the integer coordinates of the F1 cell —
// callers reuse those coordinates to fetch per-cell randomisation.
inline void VoronoiF1F2(float x, float z, int32_t seed,
                        float& f1, float& f2,
                        int32_t& f1cx, int32_t& f1cz)
{
    const int32_t cx = static_cast<int32_t>(std::floor(x));
    const int32_t cz = static_cast<int32_t>(std::floor(z));
    f1 = std::numeric_limits<float>::infinity();
    f2 = std::numeric_limits<float>::infinity();
    f1cx = cx;
    f1cz = cz;
    for (int dz = -1; dz <= 1; ++dz)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int32_t gx = cx + dx;
            const int32_t gz = cz + dz;
            const float jx = HashFloat01(gx, gz, seed) * 0.9f - 0.45f;
            const float jz = HashFloat01(gx, gz, seed + 73) * 0.9f - 0.45f;
            const float sx = static_cast<float>(gx) + 0.5f + jx;
            const float sz = static_cast<float>(gz) + 0.5f + jz;
            const float dxs = sx - x;
            const float dzs = sz - z;
            const float d = std::sqrt(dxs * dxs + dzs * dzs);
            if (d < f1)
            {
                f2 = f1;
                f1 = d;
                f1cx = gx;
                f1cz = gz;
            }
            else if (d < f2)
            {
                f2 = d;
            }
        }
    }
}

inline float Smoothstep01(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace rock_node
} // namespace

// Rock: tiles the terrain with a jittered Voronoi grid, raising each cell
// into a dome with sub-cell roughness and an optional crack at the cell
// boundary. Heights are added (peaks rise above the input terrain), and a
// 0..1 mask of "where the rock dome is significant" is written to grid.mask.
void ApplyRock(HeightfieldGrid& grid, const RockSettings& settings, const MaskGrid* placementMask)
{
    const int n = grid.resolution;
    const size_t cellCount = static_cast<size_t>(n) * static_cast<size_t>(n);
    if (n < 2 || grid.heights.size() < cellCount || settings.density <= 0.0f)
    {
        return;
    }

    const bool hasPlacementMask = placementMask != nullptr &&
        placementMask->resolution > 0 &&
        !placementMask->values.empty();
    const bool usesSmoothedGround = settings.groundDetailLevelM > 0.0f;
    if (!hasPlacementMask && !usesSmoothedGround &&
        settings.backend == RockBackend::GpuCompute && g_rockGpuEvaluator != nullptr)
    {
        std::string ignoredError;
        if (g_rockGpuEvaluator(grid, settings, &ignoredError))
        {
            return;
        }
        // Falls through to the CPU implementation on shader / dispatch failure.
    }

    grid.mask.assign(cellCount, 0.0f);
    grid.uniqueMask.assign(cellCount, 0.0f);

    const float density = std::max(settings.density, 0.1f);
    const float coverage = std::clamp(settings.coverage, 0.0f, 1.0f);
    // Sizes are specified in metres; convert to cell-pitch units (1 cell = density m).
    const float rockSizeMinM = std::clamp(settings.rockSizeMinM, 0.1f, 200.0f);
    const float rockSizeMaxM = std::clamp(std::max(settings.rockSizeMaxM, rockSizeMinM), 0.1f, 200.0f);
    const float rockSizeMinCells = rockSizeMinM / density;
    const float rockSizeMaxCells = rockSizeMaxM / density;
    const float rockHeight = std::max(settings.rockHeight, 0.0f);
    const float heightJitter = std::clamp(settings.heightJitter, 0.0f, 1.0f);
    const float rotationVar = std::clamp(settings.rotationVariation, 0.0f, 1.0f);
    const float aspectVar = std::clamp(settings.aspectVariation, 0.0f, 1.0f);
    const float edgeSharpness = std::clamp(settings.edgeSharpness, 0.0f, 1.0f);
    const float bumpiness = std::clamp(settings.bumpiness, 0.0f, 1.0f);
    const float facetSharpness = std::clamp(settings.facetSharpness, 0.0f, 1.0f);
    const float facetScale = std::clamp(settings.facetScale, 0.5f, 8.0f);
    const int layerCount = std::clamp(settings.layerCount, 1, 8);
    const int orientationRule = std::clamp(static_cast<int>(settings.orientationRule), 0, 2);
    const int rockStyle = std::clamp(static_cast<int>(settings.style), 0, 2);
    const bool polygonalStyle = rockStyle != static_cast<int>(RockStyle::Classic);
    const bool shardStyle = rockStyle == static_cast<int>(RockStyle::Shard);
    const float effectiveEdgeSharpness = polygonalStyle ? std::max(edgeSharpness, 0.65f) : edgeSharpness;
    const float effectiveFacetSharpness = polygonalStyle ? std::max(facetSharpness, 0.7f) : facetSharpness;
    const float styleAspectBoost = shardStyle ? 0.65f : 0.0f;
    const int32_t seed = settings.seed;
    const int32_t subSeedI = rock_node::DeriveSeed(seed, 7919u, 31337u);
    const int32_t facetSeedI = rock_node::DeriveSeed(seed, 2347u, 8675309u);
    const int32_t rotSeed = rock_node::DeriveSeed(seed, 4519u, 91173u);
    const int32_t sizeSeed = rock_node::DeriveSeed(seed, 1583u, 22441u);
    const int32_t aspectSeed = rock_node::DeriveSeed(seed, 2381u, 33797u);
    const int32_t aspectAxisSeed = rock_node::DeriveSeed(seed, 4093u, 51817u);
    const int32_t subOffsetSeedX = rock_node::DeriveSeed(seed, 643u, 5081u);
    const int32_t subOffsetSeedZ = rock_node::DeriveSeed(seed, 757u, 6151u);
    const int32_t polyCountSeed = rock_node::DeriveSeed(seed, 1009u, 13513u);
    const int32_t polyAngleSeed = rock_node::DeriveSeed(seed, 137u, 60013u);
    const int32_t polyRadiusSeed = rock_node::DeriveSeed(seed, 251u, 70003u);
    const int32_t apexSeedX = rock_node::DeriveSeed(seed, 1181u, 42043u);
    const int32_t apexSeedZ = rock_node::DeriveSeed(seed, 1871u, 52189u);
    const bool needPolyhedral = effectiveEdgeSharpness > 0.0f;

    const float terrainSize = std::max(grid.terrainSizeMeters, 1.0f);
    const float halfSize = terrainSize * 0.5f;
    const float invStep = (n > 1) ? 1.0f / static_cast<float>(n - 1) : 0.0f;
    const float cellSizeMeters = terrainSize / static_cast<float>(std::max(1, n - 1));
    const float invTwoCellMeters = 1.0f / (2.0f * cellSizeMeters);
    const float groundDetailM = std::clamp(settings.groundDetailLevelM, 0.0f, terrainSize * 0.5f);
    const int groundRadius = groundDetailM > 0.0f
        ? std::clamp(static_cast<int>(std::round(groundDetailM / cellSizeMeters)), 1, 128)
        : 0;
    const std::vector<float> groundHeights = groundRadius > 1 ? BoxBlurHeights(grid, groundRadius) : std::vector<float>();
    const std::vector<float> inputHeights = (orientationRule != static_cast<int>(RockOrientationRule::Flat))
        ? grid.heights
        : std::vector<float>();
    const auto samplePlacementMask = [&](float u, float v) {
        if (!hasPlacementMask)
        {
            return 1.0f;
        }
        return std::clamp(SampleMaskBilinear(*placementMask, u, v), 0.0f, 1.0f);
    };

    // Search radius covers the worst case: largest rock × max aspect stretch.
    // aspect uses pow(2, aspectVar) to give a symmetric multiplicative range
    // around 1.0 (e.g. aspectVar = 0.5 → aspect ∈ [√½, √2]).
    const float maxDomeRadius = rockSizeMaxCells * 0.5f;
    const float maxAspect = std::pow(2.0f, aspectVar + styleAspectBoost);
    const float maxReach = maxDomeRadius * maxAspect;
    const int searchRadius = std::max(1, static_cast<int>(std::ceil(maxReach - 0.05f)));
    // Apex sharpness: higher facetSharpness pinches the apex, but at full
    // edgeSharpness the dome is already a flat-faceted polyhedron so we
    // keep the falloff linear (exp = 1).
    const float domeExp = polygonalStyle ? 1.0f : (1.0f + facetSharpness * 1.5f * (1.0f - edgeSharpness));

    ParallelForRows(n, [&](int z) {
        const float worldZ = -halfSize + static_cast<float>(z) * invStep * terrainSize;
        const float cellZ = worldZ / density;
        for (int x = 0; x < n; ++x)
        {
            const float worldX = -halfSize + static_cast<float>(x) * invStep * terrainSize;
            const float cellX = worldX / density;
            const int32_t baseCx = static_cast<int32_t>(std::floor(cellX));
            const int32_t baseCz = static_cast<int32_t>(std::floor(cellZ));

            // Track the largest rock contribution at this pixel.
            float bestRockH = 0.0f;
            float bestDome = 0.0f;
            float bestUnique = 0.0f;
            float gradX = 0.0f;
            float gradZ = 0.0f;
            float slopeLen = 0.0f;
            float normalUp = 1.0f;
            if (!inputHeights.empty())
            {
                const int xm = std::max(0, x - 1);
                const int xp = std::min(n - 1, x + 1);
                const int zm = std::max(0, z - 1);
                const int zp = std::min(n - 1, z + 1);
                const size_t idxL = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(xm);
                const size_t idxR = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(xp);
                const size_t idxD = static_cast<size_t>(zm) * static_cast<size_t>(n) + static_cast<size_t>(x);
                const size_t idxU = static_cast<size_t>(zp) * static_cast<size_t>(n) + static_cast<size_t>(x);
                gradX = (inputHeights[idxR] - inputHeights[idxL]) * invTwoCellMeters;
                gradZ = (inputHeights[idxU] - inputHeights[idxD]) * invTwoCellMeters;
                slopeLen = std::sqrt(gradX * gradX + gradZ * gradZ);
                normalUp = 1.0f / std::sqrt(1.0f + slopeLen * slopeLen);
            }

            for (int layer = 0; layer < layerCount; ++layer)
            {
                const int32_t layerSeed = rock_node::DeriveSeed(seed + layer * 1009, 1667u, 104729u);
                for (int dz = -searchRadius; dz <= searchRadius; ++dz)
                {
                    for (int dx = -searchRadius; dx <= searchRadius; ++dx)
                    {
                    const int32_t gx = baseCx + dx;
                    const int32_t gz = baseCz + dz;
                    const float jx = rock_node::HashFloat01(gx, gz, layerSeed) * 0.9f - 0.45f;
                    const float jz = rock_node::HashFloat01(gx, gz, layerSeed + 73) * 0.9f - 0.45f;
                    const float sx = static_cast<float>(gx) + 0.5f + jx;
                    const float sz = static_cast<float>(gz) + 0.5f + jz;
                    const float siteWorldX = sx * density;
                    const float siteWorldZ = sz * density;
                    const float siteU = (siteWorldX + halfSize) / terrainSize;
                    const float siteV = (siteWorldZ + halfSize) / terrainSize;
                    const float siteMask = samplePlacementMask(siteU, siteV);
                    if (siteMask <= 0.0f)
                    {
                        continue;
                    }
                    const float ddx = cellX - sx;
                    const float ddz = cellZ - sz;
                    const float d_iso = std::sqrt(ddx * ddx + ddz * ddz);

                    if (d_iso >= maxReach)
                    {
                        continue;
                    }

                    // Per-seed coverage gate: this cell may not be a rock at all.
                    const float cellRandom = rock_node::HashFloat01(gx, gz, layerSeed + 17);
                    if (cellRandom > coverage * siteMask)
                    {
                        continue;
                    }

                    // Per-rock random size in [rockSizeMinCells, rockSizeMaxCells].
                    const float sizeRand = rock_node::HashFloat01(gx, gz, sizeSeed + layerSeed);
                    const float rockSizeCells = rockSizeMinCells + sizeRand * (rockSizeMaxCells - rockSizeMinCells);
                    const float domeRadius_per = rockSizeCells * 0.5f;

                    // Per-rock rotation. rotationVar = 1 → full 2π, 0 → no rotation.
                    const float rotRand = rock_node::HashFloat01(gx, gz, rotSeed + layerSeed);
                    const float randomTheta = (rotRand - 0.5f) * 2.0f * 3.14159265358979323846f * rotationVar;
                    const float slopeTheta = (slopeLen > 1e-4f) ? std::atan2(gradZ, gradX) : 0.0f;
                    const float theta = (orientationRule == static_cast<int>(RockOrientationRule::SlopeOriented) && slopeLen > 1e-4f)
                        ? (slopeTheta + randomTheta)
                        : randomTheta;
                    const float cosT = std::cos(theta);
                    const float sinT = std::sin(theta);

                    // Per-rock area-preserving aspect. aspect ∈ [pow(2,-aspectVar), pow(2,aspectVar)].
                    // aspectAxis ∈ {0, 1} chooses which axis (in the rock's local frame) is the long one.
                    const float aspectRand = rock_node::HashFloat01(gx, gz, aspectSeed + layerSeed);
                    const float aspectExp = aspectVar * (2.0f * aspectRand - 1.0f) + styleAspectBoost;
                    const float aspect = std::pow(2.0f, aspectExp);
                    const float axisRand = rock_node::HashFloat01(gx, gz, aspectAxisSeed + layerSeed);
                    const float aspect_x = (axisRand < 0.5f) ? aspect : (1.0f / aspect);
                    const float aspect_z = 1.0f / aspect_x;

                    // Local rock-frame coordinates: rotate into the rock's
                    // own frame, then divide by the per-axis aspect to get
                    // an elliptic distance metric.
                    const float rx_unrot = ddx * cosT + ddz * sinT;
                    const float rz_unrot = -ddx * sinT + ddz * cosT;
                    const float rx = rx_unrot / aspect_x;
                    const float rz = rz_unrot / aspect_z;
                    const float slopeAlong = (orientationRule != static_cast<int>(RockOrientationRule::Flat))
                        ? (gradX * ddx + gradZ * ddz)
                        : 0.0f;
                    const float d_local = std::sqrt(rx * rx + rz * rz + slopeAlong * slopeAlong);
                    if (d_local >= domeRadius_per)
                    {
                        continue;
                    }

                    // Per-rock height variation centred on rockHeight.
                    const float heightRand = rock_node::HashFloat01(gx, gz, layerSeed + 53);
                    const float orientationHeightScale = (orientationRule == static_cast<int>(RockOrientationRule::FollowGround)) ? normalUp : 1.0f;
                    const float cellHeight = rockHeight * orientationHeightScale * (1.0f - heightJitter + heightJitter * 2.0f * heightRand);

                    // Radial component: smooth circular dome (falls off linearly
                    // from centre to elliptic boundary).
                    const float radialT = std::clamp(1.0f - d_local / domeRadius_per, 0.0f, 1.0f);

                    // Polyhedral component: signed-distance field of an irregular
                    // 4–7 sided convex polygon inscribed in the elliptic dome.
                    // When edgeSharpness > 0 we hard-clip the rock to the polygon
                    // — pixels outside the polygon get no contribution at all,
                    // so there's no halo of soft radial dome leaking past the
                    // polygon edges. The blend by edgeSharpness only affects
                    // the *interior* dome height (radial vs flat-facet shape).
                    float polyhedralT = 0.0f;
                    float topPlaneMask = 0.0f;
                    if (needPolyhedral)
                    {
                        int facetCount = std::min(7, 4 + static_cast<int>(rock_node::HashFloat01(gx, gz, polyCountSeed + layerSeed) * 4.0f)); // 4..7
                        if (polygonalStyle)
                        {
                            const float styleCount = rock_node::HashFloat01(gx, gz, polyCountSeed + layerSeed + 97);
                            facetCount = shardStyle
                                ? std::min(6, 4 + static_cast<int>(styleCount * 3.0f))
                                : std::min(8, 5 + static_cast<int>(styleCount * 4.0f));
                        }
                        const float facetCountF = static_cast<float>(facetCount);
                        const float kPi = 3.14159265358979323846f;
                        // Polygon vertices touch the elliptic boundary; edges sit
                        // at the inradius. Per-edge jitter shrinks edges further
                        // in for irregular convex shapes.
                        const float baseInradius = domeRadius_per * std::cos(kPi / facetCountF);
                        const float edgeAngularSpan = (2.0f * kPi) / facetCountF;
                        const float apexRange = shardStyle ? 0.42f : 0.28f;
                        const float apexX = polygonalStyle ? ((rock_node::HashFloat01(gx, gz, apexSeedX + layerSeed) - 0.5f) * 2.0f * baseInradius * apexRange) : 0.0f;
                        const float apexZ = polygonalStyle ? ((rock_node::HashFloat01(gx, gz, apexSeedZ + layerSeed) - 0.5f) * 2.0f * baseInradius * apexRange) : 0.0f;
                        float polyDist = polygonalStyle ? 1.0f : std::numeric_limits<float>::max();
                        for (int i = 0; i < facetCount; ++i)
                        {
                            const float baseAngle = static_cast<float>(i) * edgeAngularSpan;
                            const float aJit = (rock_node::HashFloat01(gx, gz, polyAngleSeed + layerSeed + i * 17) - 0.5f) * (edgeAngularSpan * 0.5f);
                            const float theta_i = baseAngle + aJit;
                            const float n_x = std::cos(theta_i);
                            const float n_z = std::sin(theta_i);
                            const float rJit = rock_node::HashFloat01(gx, gz, polyRadiusSeed + layerSeed + i * 23);
                            const float radiusJitter = polygonalStyle ? 0.18f : 0.3f;
                            const float r_i = baseInradius * (1.0f - rJit * radiusJitter);
                            const float interiorDist = r_i - (rx * n_x + rz * n_z);
                            if (polygonalStyle)
                            {
                                const float apexDist = r_i - (apexX * n_x + apexZ * n_z);
                                const float normalizedDist = interiorDist / std::max(apexDist, 1e-4f);
                                if (normalizedDist < polyDist) polyDist = normalizedDist;
                            }
                            else if (interiorDist < polyDist)
                            {
                                polyDist = interiorDist;
                            }
                        }
                        // Hard polygon clip — outside polygon, this rock contributes nothing.
                        if (polyDist <= 0.0f) continue;
                        if (polygonalStyle)
                        {
                            const float topCut = shardStyle ? 0.92f : 0.64f;
                            polyhedralT = std::clamp(polyDist / topCut, 0.0f, 1.0f);
                            if (!shardStyle)
                            {
                                const float topT = (polyDist - topCut) / std::max(1.0f - topCut, 1e-4f);
                                topPlaneMask = rock_node::Smoothstep01(topT);
                            }
                        }
                        else
                        {
                            polyhedralT = std::clamp(polyDist / std::max(baseInradius, 1e-4f), 0.0f, 1.0f);
                        }
                    }

                    // Blend interior dome height by edgeSharpness.
                    // Outside polygon was already excluded by the hard clip above
                    // (when needPolyhedral); so radialT here is always meaningful too.
                    const float t = (1.0f - effectiveEdgeSharpness) * radialT + effectiveEdgeSharpness * polyhedralT;
                    if (t <= 0.0f) continue;
                    const float dome = std::pow(t, domeExp);

                    // Per-rock facet field, sampled in the rock's local
                    // (rotated, unsquashed) frame. Per-rock random offset
                    // gives every rock a unique facet pattern.
                    const float subOffX = rock_node::HashFloat01(gx, gz, subOffsetSeedX + layerSeed) * 1024.0f;
                    const float subOffZ = rock_node::HashFloat01(gx, gz, subOffsetSeedZ + layerSeed) * 1024.0f;
                    float sub_f1 = 0.0f, sub_f2 = 0.0f;
                    int32_t sub_cx = 0, sub_cz = 0;
                    rock_node::VoronoiF1F2(subOffX + rx * facetScale, subOffZ + rz * facetScale,
                                           subSeedI, sub_f1, sub_f2, sub_cx, sub_cz);
                    const float smoothBump = rock_node::Smoothstep01(1.0f - sub_f1 / 0.5f) - 0.5f;
                    const float facetH = rock_node::HashFloat01(sub_cx, sub_cz, facetSeedI) - 0.5f;
                    const float edgeT = std::clamp((sub_f2 - sub_f1) * 4.0f, 0.0f, 1.0f);
                    const float facetTerm = facetH * edgeT - (1.0f - edgeT) * 0.25f;
                    const float surfaceMod = ((1.0f - effectiveFacetSharpness) * smoothBump + effectiveFacetSharpness * facetTerm) * (1.0f - topPlaneMask);

                    const float rockH = cellHeight * dome * (1.0f + bumpiness * surfaceMod);
                    if (rockH > bestRockH)
                    {
                        bestRockH = rockH;
                        bestDome = dome;
                        bestUnique = rock_node::HashFloat01(gx, gz, layerSeed + 131);
                    }
                    }
                }
            }

            if (bestRockH <= 0.0f)
            {
                continue;
            }

            const size_t idx = static_cast<size_t>(z) * static_cast<size_t>(n) + static_cast<size_t>(x);
            const float pixelMask = samplePlacementMask(
                static_cast<float>(x) * invStep,
                static_cast<float>(z) * invStep);
            if (pixelMask <= 0.0f)
            {
                continue;
            }

            const float originalH = grid.heights[idx];
            const float groundH = groundHeights.empty() ? originalH : groundHeights[idx];
            const float rockTargetH = groundH + bestRockH * pixelMask;
            grid.heights[idx] = std::max(originalH, rockTargetH);
            grid.mask[idx] = bestDome * pixelMask;
            grid.uniqueMask[idx] = bestUnique;
        }
    });
}

void SetRockGpuEvaluator(RockGpuEvaluator evaluator)
{
    g_rockGpuEvaluator = evaluator;
}
} // namespace rock
