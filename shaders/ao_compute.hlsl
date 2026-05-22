// ハイトフィールドベースのホライゾン AO コンピュートシェーダー。
// 各テクセルから 8 方向へ指定半径内をサンプリングし、地平線仰角の最大値から
// 天空可視率を計算してアンビエントオクルージョンを生成する。

cbuffer AOConstants : register(b0)
{
    uint  resolution;           // テクスチャの幅 = 高さ
    float worldDX;              // セルサイズ (m) = terrainSize / (resolution - 1)
    float maxDistanceMeters;    // サンプル半径 (m)
    float pad0;
};

Texture2D<float>   inputHeight : register(t0);
RWTexture2D<float> outputAO    : register(u0);
SamplerState       linearSampler : register(s0);

static const uint kNumDirections = 8;
static const uint kNumSamples    = 24;

[numthreads(8, 8, 1)]
void CSAmbientOcclusion(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= resolution || id.y >= resolution)
        return;

    float2 uv         = (float2(id.xy) + 0.5f) / float(resolution);
    float  hCenter    = inputHeight.SampleLevel(linearSampler, uv, 0);
    float  invRes     = 1.0f / float(resolution);
    float  safeWorldDX = max(worldDX, 0.0001f);

    float totalOcclusion = 0.0f;
    for (uint d = 0; d < kNumDirections; ++d)
    {
        float  angle      = (float(d) / float(kNumDirections)) * 6.28318530718f;
        float2 dir        = float2(cos(angle), sin(angle));
        float  maxHorizon = 0.0f;

        for (uint s = 1; s <= kNumSamples; ++s)
        {
            float sampleT = float(s) / float(kNumSamples);
            float worldDist = max(0.001f, sampleT * sampleT * maxDistanceMeters);
            float texelDist = worldDist / safeWorldDX;

            float2 sampleUv = uv + dir * (texelDist * invRes);
            if (any(sampleUv < 0.0f) || any(sampleUv > 1.0f))
                break;
            float hSample   = inputHeight.SampleLevel(linearSampler, sampleUv, 0);
            float horizon   = atan2(hSample - hCenter, worldDist);
            maxHorizon      = max(maxHorizon, horizon);
        }

        totalOcclusion += sin(max(0.0f, maxHorizon));
    }

    outputAO[id.xy] = saturate(1.0f - totalOcclusion / float(kNumDirections));
}
