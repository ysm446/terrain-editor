cbuffer Settings : register(b0)
{
    uint width;
    uint height;
    uint primitiveKind;
    uint sdfOperationCount;
    float preCameraPadding;
    float2 cameraPadding;
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float2 viewportMin;
    float2 viewportMax;
    float2 viewportCenter;
    float focalLength;
    float projectionScale;
    float2 padding;
};

RWTexture2D<float4> gOutput : register(u0);
ByteAddressBuffer gSdfOperations : register(t0);

float length3(float3 v)
{
    return sqrt(dot(v, v));
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

float evaluate_sdf(float3 p)
{
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
    return sdf;
}

bool intersect_bounds(float3 origin, float3 direction, out float nearT, out float farT)
{
    nearT = 0.0;
    farT = 1000.0;
    [unroll]
    for (uint axis = 0; axis < 3; ++axis)
    {
        float rayOrigin = origin[axis];
        float rayDirection = direction[axis];
        if (abs(rayDirection) < 0.00001)
        {
            if (rayOrigin < -1.05 || rayOrigin > 1.05)
                return false;
            continue;
        }

        float t0 = (-1.05 - rayOrigin) / rayDirection;
        float t1 = (1.05 - rayOrigin) / rayDirection;
        if (t0 > t1)
        {
            float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        nearT = max(nearT, t0);
        farT = min(farT, t1);
        if (nearT > farT)
            return false;
    }
    return farT > 0.0;
}

float3 estimate_normal(float3 p)
{
    const float e = 0.012;
    float dx = evaluate_sdf(p + float3(e, 0, 0)) - evaluate_sdf(p - float3(e, 0, 0));
    float dy = evaluate_sdf(p + float3(0, e, 0)) - evaluate_sdf(p - float3(0, e, 0));
    float dz = evaluate_sdf(p + float3(0, 0, e)) - evaluate_sdf(p - float3(0, 0, e));
    return normalize(float3(dx, dy, dz));
}

float4 shade_hit(float3 hitPoint, float3 normal, float3 viewDirection, uint steps)
{
    float3 lightDirection = normalize(float3(-0.45, 0.78, 0.42));
    float diffuse = saturate(dot(normal, lightDirection) * 0.5 + 0.5);
    float fresnel = pow(saturate(1.0 - abs(dot(normal, -viewDirection))), 2.0);
    float height = saturate(hitPoint.y * 0.5 + 0.5);
    float stepShade = saturate(1.0 - float(steps) / 64.0);
    float3 color = float3(
        95.0 + diffuse * 70.0 + fresnel * 36.0 + height * 14.0,
        104.0 + diffuse * 82.0 + fresnel * 28.0 + height * 12.0,
        94.0 + diffuse * 62.0 + fresnel * 20.0 + stepShade * 14.0) / 255.0;
    return float4(color, 0.92);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float2 screen = lerp(viewportMin, viewportMax, uv);
    float cameraXOverDepth = (screen.x - viewportCenter.x) / max(focalLength * projectionScale, 0.001);
    float cameraYOverDepth = -(screen.y - viewportCenter.y) / max(focalLength * projectionScale, 0.001);
    float3 direction = normalize(cameraForward.xyz + cameraRight.xyz * cameraXOverDepth + cameraUp.xyz * cameraYOverDepth);

    float nearT;
    float farT;
    if (!intersect_bounds(cameraPosition.xyz, direction, nearT, farT))
    {
        gOutput[id.xy] = float4(0, 0, 0, 0);
        return;
    }

    float t = max(nearT, 0.0);
    [loop]
    for (uint step = 0; step < 96 && t <= farT; ++step)
    {
        float3 p = cameraPosition.xyz + direction * t;
        float sdf = evaluate_sdf(p);
        if (abs(sdf) < 0.0045)
        {
            gOutput[id.xy] = shade_hit(p, estimate_normal(p), direction, step);
            return;
        }
        t += max(abs(sdf) * 0.82, 0.004);
    }

    gOutput[id.xy] = float4(0, 0, 0, 0);
}
