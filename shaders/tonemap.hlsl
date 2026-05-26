cbuffer TonemapConstants : register(b0)
{
    float exposureMode;
    float exposureEv;
    float autoExposureBiasEv;
    float autoExposureMinEv;
    float autoExposureMaxEv;
    float adaptationRate;
    float deltaTimeSeconds;
    float colorTemperatureKelvin;
};

Texture2D<float4> HdrColor : register(t0);
Texture2D<float> ExposureHistory : register(t1);
SamplerState LinearSampler : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut TonemapVS(uint vid : SV_VertexID)
{
    VsOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(p.x, 1.0 - p.y);
    return o;
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 AcesFitted(float3 color)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((color * (a * color + b)) / (color * (c * color + d) + e));
}

float3 ColorTemperatureRgb(float kelvin)
{
    float temp = clamp(kelvin, 1000.0, 40000.0) / 100.0;
    float warmDelta = max(temp - 60.0, 1.0e-3);
    float r = temp <= 66.0 ? 1.0 : saturate(1.292936186 * pow(warmDelta, -0.1332047592));
    float g = temp <= 66.0
        ? saturate(0.3900815788 * log(temp) - 0.6318414438)
        : saturate(1.129890861 * pow(warmDelta, -0.0755148492));
    float b = temp >= 66.0 ? 1.0 : (temp <= 19.0 ? 0.0 : saturate(0.5432067891 * log(temp - 10.0) - 1.1962540891));
    return float3(r, g, b);
}

float AutoExposureEv()
{
    float logLum = 0.0;
    [unroll]
    for (int y = 0; y < 4; ++y)
    {
        [unroll]
        for (int x = 0; x < 4; ++x)
        {
            float2 uv = (float2(x, y) + 0.5) * 0.25;
            float3 color = max(HdrColor.SampleLevel(LinearSampler, uv, 0).rgb, 0.0);
            logLum += log2(max(Luminance(color), 1.0e-4));
        }
    }
    float avgLum = exp2(logLum / 16.0);
    float targetLum = 0.18;
    float ev = log2(targetLum / max(avgLum, 1.0e-4)) + autoExposureBiasEv;
    return clamp(ev, autoExposureMinEv, autoExposureMaxEv);
}

float4 ExposurePS(VsOut input) : SV_Target
{
    float targetEv = exposureMode > 0.5 ? AutoExposureEv() : exposureEv;
    float previousEv = ExposureHistory.SampleLevel(LinearSampler, float2(0.5, 0.5), 0);
    float rate = max(adaptationRate, 0.01);
    float alpha = 1.0 - exp2(-rate * max(deltaTimeSeconds, 0.0));
    float adaptedEv = lerp(previousEv, targetEv, saturate(alpha));
    return float4(adaptedEv, 0.0, 0.0, 1.0);
}

float4 TonemapPS(VsOut input) : SV_Target
{
    float3 hdr = max(HdrColor.SampleLevel(LinearSampler, input.uv, 0).rgb, 0.0);
    float ev = exposureMode > 0.5 ? ExposureHistory.SampleLevel(LinearSampler, float2(0.5, 0.5), 0) : exposureEv;
    float3 exposed = hdr * exp2(ev);
    float3 whiteBalance = ColorTemperatureRgb(colorTemperatureKelvin) / max(ColorTemperatureRgb(6500.0), float3(1.0e-3, 1.0e-3, 1.0e-3));
    exposed *= whiteBalance;
    float3 mapped = AcesFitted(exposed);
    return float4(mapped, 1.0);
}

float4 ColorGradePS(VsOut input) : SV_Target
{
    float4 color = HdrColor.SampleLevel(LinearSampler, input.uv, 0);
    float3 whiteBalance = ColorTemperatureRgb(colorTemperatureKelvin) / max(ColorTemperatureRgb(6500.0), float3(1.0e-3, 1.0e-3, 1.0e-3));
    color.rgb = saturate(max(color.rgb, 0.0) * whiteBalance);
    return float4(color.rgb, color.a);
}
