// Scatter compute shader.
//
// GPU port of src/node_graph.cpp ApplyScatter for the default fast path:
// no placement Mask input and Ground Detail Level = Max. Masked or smoothed
// ground cases use the CPU reference path.

cbuffer ScatterConstants : register(b0)
{
    uint  resolution;
    int   seed;
    float terrainSizeMeters;
    float density;

    float coverage;
    float sizeMinCells;
    float sizeMaxCells;
    float height;

    float heightJitter;
    float rotationVar;
    float aspectVar;
    int   searchRadius;

    float maxReach;
    int   shapeType; // 0 = Hemisphere, 1 = Cone
    int   orientationRule; // 0 = Flat, 1 = Follow Ground, 2 = Slope Oriented
    int   pad1;
};

RWStructuredBuffer<float> InputHeights     : register(u0);
RWStructuredBuffer<float> OutputHeights    : register(u1);
RWStructuredBuffer<float> OutputMask       : register(u2);
RWStructuredBuffer<float> OutputUniqueMask : register(u3);

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

[numthreads(8, 8, 1)]
void CSScatter(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint x = dispatchThreadID.x;
    uint z = dispatchThreadID.y;
    if (x >= resolution || z >= resolution) return;

    uint i = z * resolution + x;
    float inputH = InputHeights[i];
    float terrainSize = max(terrainSizeMeters, 1.0f);
    float halfSize = terrainSize * 0.5f;
    float invStep = (resolution > 1u) ? (1.0f / (float)(resolution - 1u)) : 0.0f;
    float worldX = -halfSize + (float)x * invStep * terrainSize;
    float worldZ = -halfSize + (float)z * invStep * terrainSize;
    float cellX = worldX / max(density, 0.1f);
    float cellZ = worldZ / max(density, 0.1f);
    int baseCx = (int)floor(cellX);
    int baseCz = (int)floor(cellZ);
    float cellSizeMeters = terrainSize / (float)max(1u, resolution - 1u);
    float invTwoCellMeters = 1.0f / (2.0f * cellSizeMeters);
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
        gradX = (InputHeights[z * resolution + xp] - InputHeights[z * resolution + xm]) * invTwoCellMeters;
        gradZ = (InputHeights[zp * resolution + x] - InputHeights[zm * resolution + x]) * invTwoCellMeters;
        slopeLen = sqrt(gradX * gradX + gradZ * gradZ);
        normalUp = 1.0f / sqrt(1.0f + slopeLen * slopeLen);
    }

    int sizeSeed       = seed * 1583 + 22441;
    int heightSeed     = seed * 2017 + 39019;
    int rotSeed        = seed * 4519 + 91173;
    int aspectSeed     = seed * 2381 + 33797;
    int aspectAxisSeed = seed * 4093 + 51817;
    int uniqueSeed     = seed * 1877 + 73009;

    float bestShape = 0.0f;
    float bestHeight = 0.0f;
    float bestUnique = 0.0f;

    int sr = searchRadius;
    for (int dz = -sr; dz <= sr; ++dz)
    {
        for (int dx = -sr; dx <= sr; ++dx)
        {
            int gx = baseCx + dx;
            int gz = baseCz + dz;
            float jx = HashFloat01(gx, gz, seed) * 0.9f - 0.45f;
            float jz = HashFloat01(gx, gz, seed + 73) * 0.9f - 0.45f;
            float sx = (float)gx + 0.5f + jx;
            float sz = (float)gz + 0.5f + jz;
            if (HashFloat01(gx, gz, seed + 17) > coverage)
            {
                continue;
            }

            float ddx = cellX - sx;
            float ddz = cellZ - sz;
            if (sqrt(ddx * ddx + ddz * ddz) >= maxReach)
            {
                continue;
            }

            float sizeRand = HashFloat01(gx, gz, sizeSeed);
            float sizeCells = sizeMinCells + sizeRand * (sizeMaxCells - sizeMinCells);
            float radiusCells = max(sizeCells * 0.5f, 1e-4f);
            float randomTheta = (HashFloat01(gx, gz, rotSeed) - 0.5f) * 6.28318530718f * rotationVar;
            float slopeTheta = (slopeLen > 1e-4f) ? atan2(gradZ, gradX) : 0.0f;
            float theta = (orientationRule == 2 && slopeLen > 1e-4f) ? (slopeTheta + randomTheta) : randomTheta;
            float cosT = cos(theta);
            float sinT = sin(theta);
            float aspectRand = HashFloat01(gx, gz, aspectSeed);
            float aspectExp = aspectVar * (2.0f * aspectRand - 1.0f);
            float aspect = pow(2.0f, aspectExp);
            bool longX = HashFloat01(gx, gz, aspectAxisSeed) < 0.5f;
            float aspectX = longX ? aspect : (1.0f / aspect);
            float aspectZ = 1.0f / aspectX;
            float rxUnrot = ddx * cosT + ddz * sinT;
            float rzUnrot = -ddx * sinT + ddz * cosT;
            float rx = rxUnrot / aspectX;
            float rz = rzUnrot / aspectZ;
            float slopeAlong = (orientationRule != 0) ? (gradX * ddx + gradZ * ddz) : 0.0f;
            float normalizedDistance = sqrt(rx * rx + rz * rz + slopeAlong * slopeAlong) / radiusCells;
            if (normalizedDistance >= 1.0f)
            {
                continue;
            }

            float shape = (shapeType == 1)
                ? saturate(1.0f - normalizedDistance)
                : sqrt(max(0.0f, 1.0f - normalizedDistance * normalizedDistance));
            float heightRand = HashFloat01(gx, gz, heightSeed);
            float orientationHeightScale = (orientationRule == 1) ? normalUp : 1.0f;
            float cellHeight = height * orientationHeightScale * (1.0f - heightJitter + heightJitter * 2.0f * heightRand);
            float contribution = cellHeight * shape;
            if (shape > bestShape)
            {
                bestShape = shape;
                bestHeight = contribution;
                bestUnique = HashFloat01(gx, gz, uniqueSeed);
            }
        }
    }

    OutputHeights[i] = (bestHeight > 0.0f) ? max(inputH, inputH + bestHeight) : inputH;
    OutputMask[i] = bestShape;
    OutputUniqueMask[i] = bestUnique;
}
