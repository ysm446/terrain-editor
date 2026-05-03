cbuffer Settings : register(b0)
{
    uint resolution;
    uint primitiveKind;
    uint sdfOperationCount;
    uint padding0;
};

RWStructuredBuffer<float> gSdf : register(u0);
ByteAddressBuffer gSdfOperations : register(t0);

float length3(float3 v)
{
    return sqrt(dot(v, v));
}

uint grid_index(uint x, uint y, uint z)
{
    return (z * resolution + y) * resolution + x;
}

float hash3(float3 p)
{
    float n = sin(dot(p, float3(12.9898, 78.233, 37.719))) * 43758.5453;
    return frac(n);
}

float value_noise(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);

    float x00 = lerp(hash3(i + float3(0, 0, 0)), hash3(i + float3(1, 0, 0)), f.x);
    float x10 = lerp(hash3(i + float3(0, 1, 0)), hash3(i + float3(1, 1, 0)), f.x);
    float x01 = lerp(hash3(i + float3(0, 0, 1)), hash3(i + float3(1, 0, 1)), f.x);
    float x11 = lerp(hash3(i + float3(0, 1, 1)), hash3(i + float3(1, 1, 1)), f.x);
    return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z) * 2.0 - 1.0;
}

float fbm(float3 p, uint octaves)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    float total = 0.0;
    [loop]
    for (uint i = 0; i < min(max(octaves, 1), 8); ++i)
    {
        value += value_noise(p * frequency) * amplitude;
        total += amplitude;
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    return total > 0.0 ? value / total : value;
}

float box_sdf(float3 p, float3 halfExtents)
{
    float3 q = abs(p) - halfExtents;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float capsule_sdf(float3 p)
{
    const float halfHeight = 0.42;
    p.y -= clamp(p.y, -halfHeight, halfHeight);
    return length3(p) - 0.38;
}

float primitive_sdf(float3 p)
{
    if (primitiveKind == 0)
        return length3(p) - 0.62;
    if (primitiveKind == 1)
        return box_sdf(p, float3(0.48, 0.42, 0.55));
    if (primitiveKind == 2)
        return capsule_sdf(p);
    if (primitiveKind == 3)
        return (length3(float3(p.x / 0.72, p.y / 0.48, p.z / 0.56)) - 1.0) * 0.56;
    return length3(float3(p.x / 0.70, p.y / 0.55, p.z / 0.62)) * 0.58 - 0.58;
}

float apply_noise(float sdf, float3 p, float4 op, float4 extra)
{
    float3 seedOffset = extra.x * float3(12.9898, 78.233, 37.719);
    float n = fbm(p * op.z + seedOffset, (uint)op.w);
    return sdf + n * op.y * 0.12;
}

float apply_cracks(float sdf, float3 p, float4 op)
{
    float crackWidth = op.y;
    float crackDepth = op.z;
    float crackRoughness = op.w;
    const float3 normals[3] = {
        float3(0.92, 0.18, 0.34),
        float3(-0.25, 0.96, 0.11),
        float3(0.16, -0.38, 0.91),
    };
    const float offsets[3] = {-0.17, 0.11, 0.29};

    float crackSdf = -1.0;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        float rough = fbm(float3(p.x * 7.0 + i * 13.0, p.y * 7.0, p.z * 7.0), 3) * crackRoughness * 0.045;
        float plane = crackWidth - abs(dot(p, normals[i]) + offsets[i] + rough);
        crackSdf = max(crackSdf, plane * (0.6 + crackDepth));
    }
    return max(sdf, crackSdf);
}

[numthreads(8, 8, 8)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= resolution || id.y >= resolution || id.z >= resolution)
        return;

    float voxelSize = 2.0 / (float(resolution) - 1.0);
    float3 p = float3(
        -1.0 + float(id.x) * voxelSize,
        -1.0 + float(id.y) * voxelSize,
        -1.0 + float(id.z) * voxelSize);

    float sdf = primitive_sdf(p);
    [loop]
    for (uint opIndex = 0; opIndex < sdfOperationCount; ++opIndex)
    {
        uint byteOffset = opIndex * 32;
        float4 op = asfloat(gSdfOperations.Load4(byteOffset));
        float4 extra = asfloat(gSdfOperations.Load4(byteOffset + 16));
        uint opKind = (uint)(op.x + 0.5);
        if (opKind == 1)
            sdf = apply_noise(sdf, p, op, extra);
        else if (opKind == 2)
            sdf = apply_cracks(sdf, p, op);
        else if (opKind == 3)
            sdf -= op.y;
    }

    gSdf[grid_index(id.x, id.y, id.z)] = sdf;
}
