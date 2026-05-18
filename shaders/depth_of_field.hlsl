cbuffer DofConstants : register(b0)
{
    float focusDistance;
    float focalLengthMm;
    float fStop;
    float sensorHeightMm;
    float maxBlurPixels;
    float nearPlane;
    float farPlane;
    float apertureShape;
    float apertureBlades;
    float apertureRotationRadians;
    float highlightBoost;
    float miniatureScale;
};

Texture2D<float4> ColorBuffer : register(t0);
Texture2D<float> DepthBuffer : register(t1);
SamplerState LinearSampler : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut DofVS(uint vid : SV_VertexID)
{
    VsOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

float LinearizeDepth(float z)
{
    return z * (farPlane - nearPlane) + nearPlane;
}

float SignedCircleOfConfusionPixels(float viewDistance, float imageHeightPixels)
{
    float focalLengthMeters = max(focalLengthMm, 1.0) * 0.001;
    float apertureMeters = focalLengthMeters / max(fStop, 0.7);
    float sceneDistanceScale = 0.001;
    float lensDistance = max(viewDistance * sceneDistanceScale, focalLengthMeters + 0.01);
    float focusMeters = max(focusDistance * sceneDistanceScale, focalLengthMeters + 0.01);
    float sensorHeightMeters = max(sensorHeightMm, 1.0) * 0.001;
    float cocMeters = apertureMeters * focalLengthMeters * (lensDistance - focusMeters) /
                      max(lensDistance * (focusMeters - focalLengthMeters), 1e-4);
    float scale = max(miniatureScale, 1.0);
    return clamp(cocMeters / sensorHeightMeters * imageHeightPixels * scale, -maxBlurPixels, maxBlurPixels);
}

float ApertureBladeCount()
{
    if (apertureShape < 0.5)
    {
        return 0.0;
    }
    if (apertureShape < 1.5)
    {
        return 3.0;
    }
    if (apertureShape < 2.5)
    {
        return 6.0;
    }
    if (apertureShape < 3.5)
    {
        return 8.0;
    }
    return clamp(round(apertureBlades), 3.0, 12.0);
}

float PolygonRadiusScale(float angle, float blades)
{
    const float pi = 3.14159265359;
    float sector = 2.0 * pi / max(blades, 3.0);
    float local = fmod(angle + sector * 0.5 + pi * 4.0, sector) - sector * 0.5;
    return saturate(cos(sector * 0.5) / max(cos(local), 1e-3));
}

float2 ApertureSample(int index, int sampleCount, float blades)
{
    const float pi = 3.14159265359;
    float t = (float(index) + 0.5) / float(sampleCount);
    float angle = float(index) * 2.39996323 + apertureRotationRadians;
    float ring = sqrt(t);

    if (blades >= 3.0)
    {
        float shapeScale = PolygonRadiusScale(angle, blades);
        float edgeSample = step(2.5, fmod(float(index), 4.0));
        ring = lerp(ring * 0.92, 0.985, edgeSample);
        return float2(cos(angle), sin(angle)) * ring * shapeScale;
    }

    return float2(cos(angle), sin(angle)) * ring;
}

float DepthToViewDistance(float depth)
{
    return depth >= 0.9999 ? farPlane : LinearizeDepth(depth);
}

float LoadDepthNearest(float2 uv)
{
    uint width = 1;
    uint height = 1;
    DepthBuffer.GetDimensions(width, height);
    int2 pixel = int2(clamp(uv, 0.0, 1.0) * float2(width, height));
    pixel = clamp(pixel, int2(0, 0), int2(int(width) - 1, int(height) - 1));
    return DepthBuffer.Load(int3(pixel, 0));
}

float4 DofPS(VsOut input) : SV_Target
{
    uint width = 1;
    uint height = 1;
    ColorBuffer.GetDimensions(width, height);
    float2 texel = 1.0 / max(float2(width, height), 1.0);
    float imageHeightPixels = max(float(height), 1.0);

    float depth = LoadDepthNearest(input.uv);
    float viewDistance = DepthToViewDistance(depth);
    float signedRadius = SignedCircleOfConfusionPixels(viewDistance, imageHeightPixels);
    float radius = abs(signedRadius);
    float3 sharp = ColorBuffer.SampleLevel(LinearSampler, input.uv, 0).rgb;

    if (maxBlurPixels < 0.35)
    {
        return float4(sharp, 1.0);
    }

    float3 sum = 0.0;
    float weightSum = 0.0;
    float foregroundWeightSum = 0.0;
    float blades = ApertureBladeCount();
    const int sampleCount = 48;
    [unroll]
    for (int i = 0; i < sampleCount; ++i)
    {
        float2 apertureOffset = ApertureSample(i, sampleCount, blades);
        if (radius >= 0.35)
        {
            float2 uv = input.uv + apertureOffset * texel * radius;
            float sampleDepth = LoadDepthNearest(uv);
            float sampleDistance = DepthToViewDistance(sampleDepth);
            float sampleRadius = abs(SignedCircleOfConfusionPixels(sampleDistance, imageHeightPixels));
            float w = max(saturate(sampleRadius / max(radius, 1e-3)), 0.25);
            float3 sampleColor = ColorBuffer.SampleLevel(LinearSampler, uv, 0).rgb;
            float luminance = dot(sampleColor, float3(0.2126, 0.7152, 0.0722));
            w *= 1.0 + smoothstep(0.55, 1.0, luminance) * highlightBoost;
            sum += sampleColor * w;
            weightSum += w;
        }

        float2 foregroundOffsetPixels = apertureOffset * maxBlurPixels;
        float foregroundDistancePixels = length(foregroundOffsetPixels);
        float2 foregroundUv = input.uv + foregroundOffsetPixels * texel;
        float foregroundDepth = LoadDepthNearest(foregroundUv);
        float foregroundDistance = DepthToViewDistance(foregroundDepth);
        float foregroundSignedRadius = SignedCircleOfConfusionPixels(foregroundDistance, imageHeightPixels);
        float foregroundRadius = abs(foregroundSignedRadius);
        float foregroundCover = saturate((foregroundRadius - foregroundDistancePixels + 1.0) * 0.5);
        float foregroundMask = foregroundSignedRadius < -0.35 && foregroundDistance < viewDistance - 0.05 ? 1.0 : 0.0;
        foregroundCover *= foregroundMask;
        if (foregroundCover > 0.0)
        {
            float3 foregroundColor = ColorBuffer.SampleLevel(LinearSampler, foregroundUv, 0).rgb;
            float foregroundLuminance = dot(foregroundColor, float3(0.2126, 0.7152, 0.0722));
            float foregroundWeight = foregroundCover * (1.0 + smoothstep(0.55, 1.0, foregroundLuminance) * highlightBoost);
            sum += foregroundColor * foregroundWeight;
            weightSum += foregroundWeight;
            foregroundWeightSum += foregroundCover;
        }
    }

    if (weightSum < 1e-4)
    {
        return float4(sharp, 1.0);
    }

    float3 blurred = sum / max(weightSum, 1e-4);
    float centerBlend = smoothstep(0.25, max(maxBlurPixels, 0.5), radius);
    float foregroundBlend = saturate(foregroundWeightSum / 8.0);
    float blend = max(centerBlend, foregroundBlend);
    return float4(lerp(sharp, blurred, blend), 1.0);
}
