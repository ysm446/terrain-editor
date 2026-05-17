cbuffer Constants : register(b0)
{
    uint resolution;
    uint cellCount;
    uint stopCount;
    uint hasMask;
};

StructuredBuffer<float> GradientMask : register(t0);
StructuredBuffer<float> MaskInput : register(t1);
StructuredBuffer<float4> Stops : register(t2); // x = position, yzw = rgb
RWStructuredBuffer<uint> Output : register(u0); // packed RGBA8

float3 SampleStops(float t)
{
    if (stopCount == 0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    float4 firstStop = Stops[0];
    if (t <= firstStop.x || stopCount == 1)
    {
        return firstStop.yzw;
    }

    [loop]
    for (uint i = 0; i + 1 < stopCount; ++i)
    {
        float4 a = Stops[i];
        float4 b = Stops[i + 1];
        if (t <= b.x)
        {
            float span = max(b.x - a.x, 1.0e-6);
            float alpha = saturate((t - a.x) / span);
            return lerp(a.yzw, b.yzw, alpha);
        }
    }

    return Stops[stopCount - 1].yzw;
}

uint PackRgba8(float4 c)
{
    uint4 v = (uint4)round(saturate(c) * 255.0);
    return (v.r & 255u) | ((v.g & 255u) << 8) | ((v.b & 255u) << 16) | ((v.a & 255u) << 24);
}

[numthreads(8, 8, 1)]
void CSColorize(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= resolution || id.y >= resolution)
    {
        return;
    }

    uint index = id.y * resolution + id.x;
    if (index >= cellCount)
    {
        return;
    }

    float t = saturate(GradientMask[index]);
    float3 rgb = SampleStops(t);
    float alpha = hasMask != 0 ? saturate(MaskInput[index]) : 1.0;
    Output[index] = PackRgba8(float4(rgb, alpha));
}
