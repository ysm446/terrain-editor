// Rock compute shader.
//
// GPU port of the rock-cover generator in src/node_graph.cpp ApplyRock.
// Algorithm overview (CPU and GPU produce identical results):
//
//   For each pixel:
//     1. Convert pixel world (x,z) → cell (cx,cz) where 1 cell = `density` m.
//     2. Scan a (2*searchRadius+1)² neighbourhood of jittered Voronoi cells.
//     3. For each cell:
//          - jitter the seed position (per-cell hash)
//          - coverage gate (per-cell hash > coverage → skip)
//          - per-rock random size / rotation / aspect
//          - rotate + aspect-divide → elliptic local coords
//          - if edgeSharpness > 0: 4..7-sided convex polygon SDF, hard clip
//          - radial / polyhedral interior height blended by edgeSharpness
//          - sub-Voronoi facet field in rock-local frame
//          - rockH = cellHeight * dome * (1 + bumpiness * surfaceMod)
//          - track max rockH (and corresponding dome value)
//     4. Output:
//          OutputHeights[i] = InputHeights[i] + bestRockH
//          OutputMask[i]    = bestDome
//          OutputUniqueMask[i] = per-rock random value for the winning rock
//
// Buffer layout (RWStructuredBuffer<float>, row-major float[res*res]):
//   u0 = InputHeights   (read-only in shader, but UAV so CopyBufferRegion
//                        can stage it from an upload buffer with no SRV
//                        descriptor — same convention as Sediment)
//   u1 = OutputHeights  (write)
//   u2 = OutputMask     (write)
//   u3 = OutputUniqueMask (write)

cbuffer RockConstants : register(b0)
{
    uint  resolution;
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
    int   needPolyhedral; // 0 / 1
    int   rockStyle;      // 0 = Classic, 1 = Polygonal, 2 = Shard

    int   orientationRule; // 0 = Flat, 1 = Follow Ground, 2 = Slope Oriented
    int   layerCount;
    int   pad2;
    int   pad3;
};

RWStructuredBuffer<float> InputHeights  : register(u0);
RWStructuredBuffer<float> OutputHeights : register(u1);
RWStructuredBuffer<float> OutputMask    : register(u2);
RWStructuredBuffer<float> OutputUniqueMask : register(u3);

// 32-bit integer mixing hash (Mulberry32-style finaliser). Mirrors
// rock_node::Hash2 in node_graph.cpp exactly so CPU and GPU produce
// identical pseudo-random streams. The `int` → `uint` reinterpretation
// uses asuint to preserve negative cell indices (which appear when the
// terrain centre is at world origin and cellX < 0).
uint Hash2(int x, int y, int s)
{
    uint h = asuint(x) * 0x27d4eb2du + asuint(y) * 0x9e3779b9u + asuint(s) * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x21f0aaadu;
    h ^= h >> 15;
    h *= 0x735a2d97u;
    h ^= h >> 15;
    return h;
}

float HashFloat01(int x, int y, int s)
{
    return (float)(Hash2(x, y, s) & 0xFFFFFFu) / (float)(0xFFFFFFu);
}

void VoronoiF1F2(float x, float z, int s,
                 out float f1, out float f2,
                 out int f1cx, out int f1cz)
{
    int cx = (int)floor(x);
    int cz = (int)floor(z);
    f1 = 1e30f;
    f2 = 1e30f;
    f1cx = cx;
    f1cz = cz;
    [unroll]
    for (int dz = -1; dz <= 1; ++dz)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int gx = cx + dx;
            int gz = cz + dz;
            float jx = HashFloat01(gx, gz, s) * 0.9f - 0.45f;
            float jz = HashFloat01(gx, gz, s + 73) * 0.9f - 0.45f;
            float sx = (float)gx + 0.5f + jx;
            float sz = (float)gz + 0.5f + jz;
            float dxs = sx - x;
            float dzs = sz - z;
            float d = sqrt(dxs * dxs + dzs * dzs);
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

float Smoothstep01(float t)
{
    t = saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

[numthreads(8, 8, 1)]
void CSRock(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint x = dispatchThreadID.x;
    uint z = dispatchThreadID.y;
    if (x >= resolution || z >= resolution) return;

    uint i = z * resolution + x;
    float inputH = InputHeights[i];

    float halfSize = max(terrainSizeMeters, 1.0f) * 0.5f;
    float invStep = (resolution > 1u) ? (1.0f / (float)(resolution - 1u)) : 0.0f;
    float worldX = -halfSize + (float)x * invStep * max(terrainSizeMeters, 1.0f);
    float worldZ = -halfSize + (float)z * invStep * max(terrainSizeMeters, 1.0f);
    float cellX = worldX / max(density, 0.1f);
    float cellZ = worldZ / max(density, 0.1f);
    int baseCx = (int)floor(cellX);
    int baseCz = (int)floor(cellZ);

    // Per-rock seed offsets — keep the multiplier set in sync with the
    // CPU side (rock_node ApplyRock) so both paths produce the same values.
    int subSeedI       = seed * 7919  + 31337;
    int facetSeedI     = seed * 2347  + 8675309;
    int rotSeed        = seed * 4519  + 91173;
    int sizeSeed       = seed * 1583  + 22441;
    int aspectSeed     = seed * 2381  + 33797;
    int aspectAxisSeed = seed * 4093  + 51817;
    int subOffsetSeedX = seed * 643   + 5081;
    int subOffsetSeedZ = seed * 757   + 6151;
    int polyCountSeed  = seed * 1009  + 13513;
    int polyAngleSeed  = seed * 137   + 60013;
    int polyRadiusSeed = seed * 251   + 70003;
    int apexSeedX      = seed * 1181  + 42043;
    int apexSeedZ      = seed * 1871  + 52189;

    bool polygonalStyle = rockStyle != 0;
    bool shardStyle = rockStyle == 2;
    float effectiveEdgeSharpness = polygonalStyle ? max(edgeSharpness, 0.65f) : edgeSharpness;
    float effectiveFacetSharpness = polygonalStyle ? max(facetSharpness, 0.7f) : facetSharpness;
    float styleAspectBoost = shardStyle ? 0.65f : 0.0f;
    int lc = clamp(layerCount, 1, 8);

    float gradX = 0.0f;
    float gradZ = 0.0f;
    float slopeLen = 0.0f;
    float normalUp = 1.0f;
    if (orientationRule != 0)
    {
        uint xm = (x > 0u) ? (x - 1u) : 0u;
        uint xp = min(resolution - 1u, x + 1u);
        uint zm = (z > 0u) ? (z - 1u) : 0u;
        uint zp = min(resolution - 1u, z + 1u);
        float cellSizeMeters = max(terrainSizeMeters, 1.0f) / max((float)(resolution - 1u), 1.0f);
        float invTwoCellMeters = 1.0f / (2.0f * cellSizeMeters);
        gradX = (InputHeights[z * resolution + xp] - InputHeights[z * resolution + xm]) * invTwoCellMeters;
        gradZ = (InputHeights[zp * resolution + x] - InputHeights[zm * resolution + x]) * invTwoCellMeters;
        slopeLen = sqrt(gradX * gradX + gradZ * gradZ);
        normalUp = 1.0f / sqrt(1.0f + slopeLen * slopeLen);
    }

    float bestRockH = 0.0f;
    float bestDome = 0.0f;
    float bestUnique = 0.0f;

    int sr = searchRadius;
    for (int layer = 0; layer < lc; ++layer)
    {
        int layerSeed = (seed + layer * 1009) * 1667 + 104729;
        for (int dz = -sr; dz <= sr; ++dz)
        {
            for (int dx = -sr; dx <= sr; ++dx)
            {
            int gx = baseCx + dx;
            int gz = baseCz + dz;
            float jx = HashFloat01(gx, gz, layerSeed) * 0.9f - 0.45f;
            float jz = HashFloat01(gx, gz, layerSeed + 73) * 0.9f - 0.45f;
            float sx = (float)gx + 0.5f + jx;
            float sz = (float)gz + 0.5f + jz;
            float ddx = cellX - sx;
            float ddz = cellZ - sz;
            float d_iso = sqrt(ddx * ddx + ddz * ddz);
            if (d_iso >= maxReach) continue;

            float cellRandom = HashFloat01(gx, gz, layerSeed + 17);
            if (cellRandom > coverage) continue;

            float sizeRand = HashFloat01(gx, gz, sizeSeed + layerSeed);
            float rockSizeCells = rockSizeMinCells + sizeRand * (rockSizeMaxCells - rockSizeMinCells);
            float domeRadius_per = rockSizeCells * 0.5f;

            float rotRand = HashFloat01(gx, gz, rotSeed + layerSeed);
            float randomTheta = (rotRand - 0.5f) * 2.0f * 3.14159265358979323846f * rotationVar;
            float slopeTheta = (slopeLen > 1e-4f) ? atan2(gradZ, gradX) : 0.0f;
            float theta = (orientationRule == 2 && slopeLen > 1e-4f) ? (slopeTheta + randomTheta) : randomTheta;
            float cosT = cos(theta);
            float sinT = sin(theta);

            float aspectRand = HashFloat01(gx, gz, aspectSeed + layerSeed);
            float aspectExp = aspectVar * (2.0f * aspectRand - 1.0f) + styleAspectBoost;
            float aspect = pow(2.0f, aspectExp);
            float axisRand = HashFloat01(gx, gz, aspectAxisSeed + layerSeed);
            float aspect_x = (axisRand < 0.5f) ? aspect : (1.0f / aspect);
            float aspect_z = 1.0f / aspect_x;

            float rx_unrot = ddx * cosT + ddz * sinT;
            float rz_unrot = -ddx * sinT + ddz * cosT;
            float rx = rx_unrot / aspect_x;
            float rz = rz_unrot / aspect_z;
            float slopeAlong = (orientationRule != 0) ? (gradX * ddx + gradZ * ddz) : 0.0f;
            float d_local = sqrt(rx * rx + rz * rz + slopeAlong * slopeAlong);
            if (d_local >= domeRadius_per) continue;

            float heightRand = HashFloat01(gx, gz, layerSeed + 53);
            float orientationHeightScale = (orientationRule == 1) ? normalUp : 1.0f;
            float cellHeight = rockHeight * orientationHeightScale * (1.0f - heightJitter + heightJitter * 2.0f * heightRand);

            float radialT = saturate(1.0f - d_local / max(domeRadius_per, 1e-6f));

            float polyhedralT = 0.0f;
            float topPlaneMask = 0.0f;
            if (needPolyhedral != 0)
            {
                int facetCount = min(7, 4 + (int)(HashFloat01(gx, gz, polyCountSeed + layerSeed) * 4.0f)); // 4..7
                if (polygonalStyle)
                {
                    float styleCount = HashFloat01(gx, gz, polyCountSeed + layerSeed + 97);
                    facetCount = shardStyle
                        ? min(6, 4 + (int)(styleCount * 3.0f))
                        : min(8, 5 + (int)(styleCount * 4.0f));
                }
                float facetCountF = (float)facetCount;
                float kPi = 3.14159265358979323846f;
                float baseInradius = domeRadius_per * cos(kPi / facetCountF);
                float edgeAngularSpan = (2.0f * kPi) / facetCountF;
                float apexRange = shardStyle ? 0.42f : 0.28f;
                float apexX = polygonalStyle ? ((HashFloat01(gx, gz, apexSeedX + layerSeed) - 0.5f) * 2.0f * baseInradius * apexRange) : 0.0f;
                float apexZ = polygonalStyle ? ((HashFloat01(gx, gz, apexSeedZ + layerSeed) - 0.5f) * 2.0f * baseInradius * apexRange) : 0.0f;
                float polyDist = polygonalStyle ? 1.0f : 1e30f;
                // facetCount is dynamic (4..8) so we cap at 8 with an early-out branch.
                [loop]
                for (int fi = 0; fi < 8; ++fi)
                {
                    if (fi >= facetCount) break;
                    float baseAngle = (float)fi * edgeAngularSpan;
                    float aJit = (HashFloat01(gx, gz, polyAngleSeed + layerSeed + fi * 17) - 0.5f) * (edgeAngularSpan * 0.5f);
                    float theta_i = baseAngle + aJit;
                    float n_x = cos(theta_i);
                    float n_z = sin(theta_i);
                    float rJit = HashFloat01(gx, gz, polyRadiusSeed + layerSeed + fi * 23);
                    float radiusJitter = polygonalStyle ? 0.18f : 0.3f;
                    float r_i = baseInradius * (1.0f - rJit * radiusJitter);
                    float interiorDist = r_i - (rx * n_x + rz * n_z);
                    if (polygonalStyle)
                    {
                        float apexDist = r_i - (apexX * n_x + apexZ * n_z);
                        float normalizedDist = interiorDist / max(apexDist, 1e-4f);
                        polyDist = min(polyDist, normalizedDist);
                    }
                    else
                    {
                        polyDist = min(polyDist, interiorDist);
                    }
                }
                if (polyDist <= 0.0f) continue; // outside polygon — hard clip
                if (polygonalStyle)
                {
                    float topCut = shardStyle ? 0.92f : 0.64f;
                    polyhedralT = saturate(polyDist / topCut);
                    if (!shardStyle)
                    {
                        float topT = (polyDist - topCut) / max(1.0f - topCut, 1e-4f);
                        topPlaneMask = Smoothstep01(topT);
                    }
                }
                else
                {
                    polyhedralT = saturate(polyDist / max(baseInradius, 1e-4f));
                }
            }

            float t = (1.0f - effectiveEdgeSharpness) * radialT + effectiveEdgeSharpness * polyhedralT;
            if (t <= 0.0f) continue;
            float dome = pow(t, domeExp);

            float subOffX = HashFloat01(gx, gz, subOffsetSeedX + layerSeed) * 1024.0f;
            float subOffZ = HashFloat01(gx, gz, subOffsetSeedZ + layerSeed) * 1024.0f;
            float sub_f1 = 0.0f, sub_f2 = 0.0f;
            int sub_cx = 0, sub_cz = 0;
            VoronoiF1F2(subOffX + rx * facetScale, subOffZ + rz * facetScale,
                        subSeedI, sub_f1, sub_f2, sub_cx, sub_cz);
            float smoothBump = Smoothstep01(1.0f - sub_f1 / 0.5f) - 0.5f;
            float facetH = HashFloat01(sub_cx, sub_cz, facetSeedI) - 0.5f;
            float edgeT = saturate((sub_f2 - sub_f1) * 4.0f);
            float facetTerm = facetH * edgeT - (1.0f - edgeT) * 0.25f;
            float surfaceMod = ((1.0f - effectiveFacetSharpness) * smoothBump + effectiveFacetSharpness * facetTerm) * (1.0f - topPlaneMask);

            float rockH = cellHeight * dome * (1.0f + bumpiness * surfaceMod);
            if (rockH > bestRockH)
            {
                bestRockH = rockH;
                bestDome = dome;
                bestUnique = HashFloat01(gx, gz, layerSeed + 131);
            }
        }
        }
    }

    OutputHeights[i] = inputH + bestRockH;
    OutputMask[i]    = bestDome;
    OutputUniqueMask[i] = bestUnique;
}
