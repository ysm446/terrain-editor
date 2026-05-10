// Snow compute shader.
//
// GPU port of ApplySnow (src/node_graph.cpp). Algorithm matches CPU exactly
// (modulo float ordering of the 3x3 box blur).
//
// Pipeline:
//   1. CSCopyInputHeights: InputHeights → BaseHeights (UAV).
//   2. CSComputeThickness: per-cell slope from BaseHeights, smoothstep
//      between min/max, write thickness into Thickness. Also write
//      initial SurfA = BaseHeights + Thickness for the smoothing pass.
//   3. CSEnvelopeSmoothing × smoothingIterations: 3x3 box blur of the
//      surface, then max(self, blurred) — preserves peaks, lifts grooves.
//      Direction flag (smoothDirection) ping-pongs SurfA / SurfB.
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
    uint  smoothDirection; // 0: SurfA → SurfB, 1: SurfB → SurfA
    float pad0;
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

// 3x3 box blur of SurfA, then max(self, blurred). smoothDirection toggles
// which buffer is read / written so we don't need to re-bind UAV slots.
[numthreads(8, 8, 1)]
void CSEnvelopeSmoothing(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    int xm = max(0, (int)x - 1);
    int xp = min((int)resolution - 1, (int)x + 1);
    int zm = max(0, (int)z - 1);
    int zp = min((int)resolution - 1, (int)z + 1);

    float s00, s01, s02, s10, s11, s12, s20, s21, s22;
    if (smoothDirection == 0u)
    {
        s00 = SurfA[(uint)zm * resolution + (uint)xm];
        s01 = SurfA[(uint)zm * resolution + (uint)x];
        s02 = SurfA[(uint)zm * resolution + (uint)xp];
        s10 = SurfA[(uint)z  * resolution + (uint)xm];
        s11 = SurfA[(uint)z  * resolution + (uint)x];
        s12 = SurfA[(uint)z  * resolution + (uint)xp];
        s20 = SurfA[(uint)zp * resolution + (uint)xm];
        s21 = SurfA[(uint)zp * resolution + (uint)x];
        s22 = SurfA[(uint)zp * resolution + (uint)xp];
    }
    else
    {
        s00 = SurfB[(uint)zm * resolution + (uint)xm];
        s01 = SurfB[(uint)zm * resolution + (uint)x];
        s02 = SurfB[(uint)zm * resolution + (uint)xp];
        s10 = SurfB[(uint)z  * resolution + (uint)xm];
        s11 = SurfB[(uint)z  * resolution + (uint)x];
        s12 = SurfB[(uint)z  * resolution + (uint)xp];
        s20 = SurfB[(uint)zp * resolution + (uint)xm];
        s21 = SurfB[(uint)zp * resolution + (uint)x];
        s22 = SurfB[(uint)zp * resolution + (uint)xp];
    }
    float blurred = (s00 + s01 + s02 + s10 + s11 + s12 + s20 + s21 + s22) * (1.0f / 9.0f);
    float result = max(s11, blurred);

    uint i = z * resolution + x;
    if (smoothDirection == 0u) SurfB[i] = result;
    else                       SurfA[i] = result;
}

// Read final surface from SurfA (CPU side ensures even smoothingIterations
// so the result lands in SurfA), compute thickness, write outputs.
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
