// Snow compute shader.
//
// GPU port of ApplySnow (src/node_graph.cpp). Algorithm matches CPU exactly
// (modulo float ordering of the radius-controlled gaussian blur).
//
// Pipeline:
//   1. CSCopyInputHeights: InputHeights → BaseHeights (UAV).
//   2. CSComputeThickness: per-cell slope from BaseHeights, smoothstep
//      between min/max, write thickness into Thickness. Also write
//      initial SurfA = BaseHeights + Thickness for the smoothing pass.
//   3. CSEnvelopeSmoothing x smoothingIterations: radius-controlled separable
//      gaussian blur of the surface, then max(self, blurred) - preserves
//      peaks, lifts grooves, and avoids directional streaks.
//   4. CSApply: thickness = surfFinal - BaseHeights, write OutHeights and
//      OutMask = clamp(thickness / maskMaxSnow).
//
// Buffer layout (RWStructuredBuffer<float>, row-major float[res*res]):
//   u0 = InputHeights   (CPU upload destination)
//   u1 = BaseHeights    (= InputHeights, used as the "before snow" reference)
//   u2 = Thickness      (per-cell snow thickness; reused as scratch)
//   u3 = SurfA          (snow surface; ping-pong with SurfB)
//   u4 = SurfB
//   u5 = OutHeights     (BaseHeights + Thickness)
//   u6 = OutMask        (Thickness / maskMaxSnow, clamped)

cbuffer SnowConstants : register(b0)
{
    uint  resolution;
    float terrainSizeMeters;
    float emissionAmount;
    float minTan;

    float invRange;        // 1 / (maxTan - minTan)
    float maskMaxSnow;
    uint  smoothDirection; // 0: horizontal gaussian A->B, 1: vertical gaussian B->A + max(A, blurred)
    uint  fillRadius;
    uint  pad0;
    uint  pad1;
    uint  pad2;
    uint  pad3;
};

RWStructuredBuffer<float> InputHeights : register(u0);
RWStructuredBuffer<float> BaseHeights  : register(u1);
RWStructuredBuffer<float> Thickness    : register(u2);
RWStructuredBuffer<float> SurfA        : register(u3);
RWStructuredBuffer<float> SurfB        : register(u4);
RWStructuredBuffer<float> OutHeights   : register(u5);
RWStructuredBuffer<float> OutMask      : register(u6);

[numthreads(8, 8, 1)]
void CSCopyInputHeights(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    BaseHeights[i] = InputHeights[i];
}

[numthreads(8, 8, 1)]
void CSComputeThickness(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    int xm = max(0, (int)x - 1);
    int xp = min((int)resolution - 1, (int)x + 1);
    int zm = max(0, (int)z - 1);
    int zp = min((int)resolution - 1, (int)z + 1);

    float h_xm = BaseHeights[(uint)z * resolution + (uint)xm];
    float h_xp = BaseHeights[(uint)z * resolution + (uint)xp];
    float h_zm = BaseHeights[(uint)zm * resolution + (uint)x];
    float h_zp = BaseHeights[(uint)zp * resolution + (uint)x];

    float cellSize = max(terrainSizeMeters, 1.0f) / max(1.0f, (float)resolution - 1.0f);
    float invTwoCell = 1.0f / (2.0f * cellSize);
    float dhdx = (h_xp - h_xm) * invTwoCell;
    float dhdz = (h_zp - h_zm) * invTwoCell;
    float slopeTan = sqrt(dhdx * dhdx + dhdz * dhdz);

    float t = saturate((slopeTan - minTan) * invRange);
    float smoothT = t * t * (3.0f - 2.0f * t);
    float snowFraction = 1.0f - smoothT;
    float thickness = max(0.0f, emissionAmount * snowFraction);

    uint i = z * resolution + x;
    Thickness[i] = thickness;
    SurfA[i] = BaseHeights[i] + thickness;
}

// Separable gaussian blur. Pass 0 writes horizontal blur to SurfB; pass 1
// vertically blurs SurfB and applies max(original SurfA, blurred) once.
[numthreads(8, 8, 1)]
void CSEnvelopeSmoothing(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    int radius = clamp((int)fillRadius, 1, 64);
    float sigma = max(1.0f, (float)radius * 0.5f);
    float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    float weightSum = 0.0f;
    if (smoothDirection == 0u)
    {
        [loop]
        for (int ox = -radius; ox <= radius; ++ox)
        {
            uint sx = (uint)clamp((int)x + ox, 0, (int)resolution - 1);
            float w = exp(-(float)(ox * ox) * invTwoSigma2);
            sum += SurfA[z * resolution + sx] * w;
            weightSum += w;
        }
    }
    else
    {
        [loop]
        for (int oz = -radius; oz <= radius; ++oz)
        {
            uint sz = (uint)clamp((int)z + oz, 0, (int)resolution - 1);
            float w = exp(-(float)(oz * oz) * invTwoSigma2);
            sum += SurfB[sz * resolution + x] * w;
            weightSum += w;
        }
    }
    float blurred = sum / max(1e-6f, weightSum);

    uint i = z * resolution + x;
    if (smoothDirection == 0u)
    {
        SurfB[i] = blurred;
    }
    else
    {
        SurfA[i] = max(SurfA[i], blurred);
    }
}

// Read final surface from SurfA. Each smoothing iteration writes the completed
// max(original, blurred) surface back to SurfA, so CSApply always reads SurfA.
[numthreads(8, 8, 1)]
void CSApply(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    uint i = z * resolution + x;
    float baseH = BaseHeights[i];
    float surf = SurfA[i];
    float thickness = max(0.0f, surf - baseH);
    OutHeights[i] = baseH + thickness;
    OutMask[i] = saturate(thickness / max(1e-4f, maskMaxSnow));
}
