cbuffer MaskUtilityConstants : register(b0)
{
    uint resolution;
    uint sourceResolution;
    uint pointCount;
    uint segmentCount;
    float terrainSizeMeters;
    float gamma;
    float invert;
    float radiusPixels;
    uint iterations;
    float strength;
    float heightMeters;
    float baseHeightMeters;
};

struct PathPointGpu
{
    float x;
    float z;
    float width;
    float feather;
    float intensity;
    float pad0;
    float pad1;
    float pad2;
};

struct PathSegmentGpu
{
    float ax;
    float az;
    float abX;
    float abZ;
    float lenSq;
    float widthA;
    float widthB;
    float featherA;
    float featherB;
    float intensityA;
    float intensityB;
    float pad0;
};

StructuredBuffer<float> InputMask : register(t0);
StructuredBuffer<PathPointGpu> Points : register(t1);
StructuredBuffer<PathSegmentGpu> Segments : register(t2);
StructuredBuffer<float> SecondMask : register(t3);

RWStructuredBuffer<float> OutputA : register(u0);
RWStructuredBuffer<float> OutputB : register(u1);
RWStructuredBuffer<float> OutputC : register(u2);

float MaskPathDistanceValue(float distance, float widthMeters, float featherMeters)
{
    float halfWidth = max(0.0f, widthMeters) * 0.5f;
    float feather = max(0.0f, featherMeters);
    if (distance <= halfWidth)
    {
        return 1.0f;
    }
    if (feather <= 0.0001f || distance >= halfWidth + feather)
    {
        return 0.0f;
    }
    float t = saturate((distance - halfWidth) / feather);
    float smooth = t * t * (3.0f - 2.0f * t);
    return 1.0f - smooth;
}

float SampleMaskBilinear(float x, float z)
{
    float maxCoord = max(0.0f, (float)sourceResolution - 1.0f);
    x = clamp(x, 0.0f, maxCoord);
    z = clamp(z, 0.0f, maxCoord);
    uint x0 = (uint)floor(x);
    uint z0 = (uint)floor(z);
    uint x1 = min(x0 + 1u, sourceResolution - 1u);
    uint z1 = min(z0 + 1u, sourceResolution - 1u);
    float tx = x - (float)x0;
    float tz = z - (float)z0;
    float h00 = InputMask[z0 * sourceResolution + x0];
    float h10 = InputMask[z0 * sourceResolution + x1];
    float h01 = InputMask[z1 * sourceResolution + x0];
    float h11 = InputMask[z1 * sourceResolution + x1];
    return lerp(lerp(h00, h10, tx), lerp(h01, h11, tx), tz);
}

[numthreads(8, 8, 1)]
void CSMaskPath(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution)
    {
        return;
    }

    float invStep = 1.0f / (float)max(1u, resolution - 1u);
    float halfSize = terrainSizeMeters * 0.5f;
    float worldX = -halfSize + (float)x * invStep * terrainSizeMeters;
    float worldZ = halfSize - (float)z * invStep * terrainSizeMeters;
    float value = 0.0f;

    [loop]
    for (uint i = 0; i < pointCount; ++i)
    {
        PathPointGpu p = Points[i];
        float dx = worldX - p.x;
        float dz = worldZ - p.z;
        value = max(value, MaskPathDistanceValue(sqrt(dx * dx + dz * dz), p.width, p.feather) * p.intensity);
    }

    [loop]
    for (uint i = 0; i < segmentCount; ++i)
    {
        PathSegmentGpu s = Segments[i];
        float t = saturate(((worldX - s.ax) * s.abX + (worldZ - s.az) * s.abZ) / s.lenSq);
        float closestX = s.ax + s.abX * t;
        float closestZ = s.az + s.abZ * t;
        float dx = worldX - closestX;
        float dz = worldZ - closestZ;
        float width = lerp(s.widthA, s.widthB, t);
        float feather = lerp(s.featherA, s.featherB, t);
        float intensityValue = lerp(s.intensityA, s.intensityB, t);
        value = max(value, MaskPathDistanceValue(sqrt(dx * dx + dz * dz), width, feather) * intensityValue);
    }

    value = pow(saturate(value), gamma);
    if (invert > 0.5f)
    {
        value = 1.0f - value;
    }
    OutputA[z * resolution + x] = value;
}

[numthreads(8, 8, 1)]
void CSMaskBlurHorizontal(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution)
    {
        return;
    }
    int radius = (int)radiusPixels;
    bool odd = (iterations & 1u) != 0u;
    float sum = 0.0f;
    int samples = 0;
    [loop]
    for (int dx = -radius; dx <= radius; ++dx)
    {
        uint sx = (uint)clamp((int)x + dx, 0, (int)resolution - 1);
        uint idx = z * resolution + sx;
        sum += odd ? SecondMask[idx] : InputMask[idx];
        ++samples;
    }
    OutputA[z * resolution + x] = sum / (float)samples;
}

[numthreads(8, 8, 1)]
void CSMaskBlurVertical(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution)
    {
        return;
    }
    int radius = (int)radiusPixels;
    bool odd = (iterations & 1u) != 0u;
    float sum = 0.0f;
    int samples = 0;
    [loop]
    for (int dz = -radius; dz <= radius; ++dz)
    {
        uint sz = (uint)clamp((int)z + dz, 0, (int)resolution - 1);
        sum += OutputA[sz * resolution + x];
        ++samples;
    }
    uint idx = z * resolution + x;
    float current = odd ? SecondMask[idx] : InputMask[idx];
    float blurred = saturate(sum / (float)samples);
    if (odd)
    {
        OutputC[idx] = saturate(lerp(current, blurred, strength));
    }
    else
    {
        OutputB[idx] = saturate(lerp(current, blurred, strength));
    }
}

[numthreads(8, 8, 1)]
void CSHeightmapFromMask(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution)
    {
        return;
    }

    float invTarget = 1.0f / (float)max(1u, resolution - 1u);
    float sourceMax = (float)max(0u, sourceResolution - 1u);
    float value = saturate(SampleMaskBilinear((float)x * invTarget * sourceMax, (float)z * invTarget * sourceMax));
    if (invert > 0.5f)
    {
        value = 1.0f - value;
    }
    value = pow(value, gamma);
    uint idx = z * resolution + x;
    OutputA[idx] = baseHeightMeters + value * heightMeters;
    OutputB[idx] = value;
}
