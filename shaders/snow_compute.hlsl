// Snow / Soil compute shader.
//
// GPU implementation of the shared granular transport model (Snow and Soil
// nodes). Each settling pass is a gather step: a cell subtracts its own
// outgoing material and adds material from source neighbours that selected
// this cell as their steepest lower target. This avoids unordered float
// atomics when multiple cells flow into the same cell.
// slopeDependentEmission > 0 (Soil) scales the injection by the base-terrain
// slope so ridges and cliffs receive less material.

cbuffer SnowConstants : register(b0)
{
    uint  resolution;
    float terrainSizeMeters;
    float emissionAmount;
    float motionLimitTan;

    float transportRate;
    float maskThresholdM;
    uint  settleStride; // 0: copy SurfA -> Thickness, >0: transport stride
    uint  smoothDirection; // 0: horizontal Thickness->SurfA, 1: vertical SurfA->Thickness

    float maskFeatherM;
    float surfaceSmoothing;
    uint  smoothRadius;
    float slopeDependentEmission; // 0 = uniform injection (Snow), 0..1 = slope-scaled (Soil)
};

RWStructuredBuffer<float> InputHeights : register(u0);
RWStructuredBuffer<float> BaseHeights  : register(u1);
RWStructuredBuffer<float> Thickness    : register(u2);
RWStructuredBuffer<float> SurfA        : register(u3); // next thickness
RWStructuredBuffer<float> SurfB        : register(u4); // unused legacy scratch
RWStructuredBuffer<float> OutHeights   : register(u5);
RWStructuredBuffer<float> OutMask      : register(u6);

uint CellIndex(uint x, uint z)
{
    return z * resolution + x;
}

float CellDistance(int dx, int dz, uint stride, float cellSize)
{
    return cellSize * (float)stride * ((dx != 0 && dz != 0) ? 1.41421356237f : 1.0f);
}

float SnowCoverage(float snow)
{
    if (maskFeatherM <= 0.0f)
    {
        return snow >= maskThresholdM ? 1.0f : 0.0f;
    }
    float lo = max(0.0f, maskThresholdM - maskFeatherM);
    float hi = maskThresholdM + maskFeatherM;
    float t = saturate((snow - lo) / max(hi - lo, 1e-6f));
    return t * t * (3.0f - 2.0f * t);
}

void ComputeOutflow(uint x, uint z, out uint targetIndex, out float amount)
{
    uint idx = CellIndex(x, z);
    targetIndex = idx;
    amount = 0.0f;

    float snow = max(0.0f, Thickness[idx]);
    if (snow <= 0.0f || transportRate <= 0.0f)
    {
        return;
    }

    uint stride = max(1u, settleStride);
    float cellSize = max(terrainSizeMeters, 1.0f) / max(1.0f, (float)resolution - 1.0f);
    float surface = BaseHeights[idx] + snow;
    float bestSlope = motionLimitTan;
    float bestDistance = cellSize;
    uint bestIndex = idx;

    [unroll]
    for (int dz = -1; dz <= 1; ++dz)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dz == 0)
            {
                continue;
            }

            int nxI = clamp((int)x + dx * (int)stride, 0, (int)resolution - 1);
            int nzI = clamp((int)z + dz * (int)stride, 0, (int)resolution - 1);
            if (nxI == (int)x && nzI == (int)z)
            {
                continue;
            }

            uint nidx = CellIndex((uint)nxI, (uint)nzI);
            float distance = CellDistance(dx, dz, stride, cellSize);
            float neighbourSurface = BaseHeights[nidx] + Thickness[nidx];
            float slope = (surface - neighbourSurface) / max(distance, 1e-6f);
            if (slope > bestSlope)
            {
                bestSlope = slope;
                bestDistance = distance;
                bestIndex = nidx;
            }
        }
    }

    if (bestIndex == idx)
    {
        return;
    }

    float stableDrop = motionLimitTan * bestDistance;
    float neighbourSurfaceBest = BaseHeights[bestIndex] + Thickness[bestIndex];
    float excess = max(0.0f, surface - neighbourSurfaceBest - stableDrop);
    float slopeFactor = saturate((bestSlope - motionLimitTan) / max(bestSlope, 1e-6f));
    amount = min(snow, min(excess * 0.5f, snow * saturate(transportRate) * slopeFactor));
    targetIndex = amount > 0.0f ? bestIndex : idx;
}

[numthreads(8, 8, 1)]
void CSCopyInputHeights(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = CellIndex(x, z);
    BaseHeights[i] = InputHeights[i];
    Thickness[i] = 0.0f;
    SurfA[i] = 0.0f;
}

// 基盤傾斜が安息角に近いほど注入を減らすスケール。CPU 実装
// (GranularSettle.cpp) と同じ、クランプ付き中心差分の式を使う。
float EmissionScale(uint x, uint z)
{
    if (slopeDependentEmission <= 0.0f)
    {
        return 1.0f;
    }
    float cellSize = max(terrainSizeMeters, 1.0f) / max(1.0f, (float)resolution - 1.0f);
    int xm = max((int)x - 1, 0);
    int xp = min((int)x + 1, (int)resolution - 1);
    int zm = max((int)z - 1, 0);
    int zp = min((int)z + 1, (int)resolution - 1);
    float dx = (BaseHeights[CellIndex((uint)xp, z)] - BaseHeights[CellIndex((uint)xm, z)]) /
        (cellSize * (float)(xp - xm));
    float dz = (BaseHeights[CellIndex(x, (uint)zp)] - BaseHeights[CellIndex(x, (uint)zm)]) /
        (cellSize * (float)(zp - zm));
    float slopeTan = sqrt(dx * dx + dz * dz);
    float flatFactor = max(0.0f, 1.0f - slopeTan / max(motionLimitTan, 1e-4f));
    return lerp(1.0f, flatFactor, saturate(slopeDependentEmission));
}

[numthreads(8, 8, 1)]
void CSComputeThickness(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = CellIndex(x, z);

    if (settleStride == 0u)
    {
        Thickness[i] = max(0.0f, SurfA[i]);
        return;
    }

    Thickness[i] += max(0.0f, emissionAmount) * EmissionScale(x, z);
}

[numthreads(8, 8, 1)]
void CSEnvelopeSmoothing(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    uint idx = CellIndex(x, z);
    uint ownTarget = idx;
    float ownOut = 0.0f;
    ComputeOutflow(x, z, ownTarget, ownOut);

    float nextSnow = max(0.0f, Thickness[idx] - ownOut);
    uint stride = max(1u, settleStride);

    [unroll]
    for (int dz = -1; dz <= 1; ++dz)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dz == 0)
            {
                continue;
            }

            int sxI = clamp((int)x - dx * (int)stride, 0, (int)resolution - 1);
            int szI = clamp((int)z - dz * (int)stride, 0, (int)resolution - 1);
            if (sxI == (int)x && szI == (int)z)
            {
                continue;
            }

            uint sourceTarget = CellIndex((uint)sxI, (uint)szI);
            float sourceOut = 0.0f;
            ComputeOutflow((uint)sxI, (uint)szI, sourceTarget, sourceOut);
            if (sourceTarget == idx)
            {
                nextSnow += sourceOut;
            }
        }
    }

    SurfA[idx] = max(0.0f, nextSnow);
}

[numthreads(8, 8, 1)]
void CSSmoothSnowSurface(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    uint idx = CellIndex(x, z);
    int radius = clamp((int)smoothRadius, 1, 32);
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
            uint sidx = CellIndex(sx, z);
            float snow = max(0.0f, Thickness[sidx]);
            float w = exp(-(float)(ox * ox) * invTwoSigma2) * SnowCoverage(snow);
            sum += (BaseHeights[sidx] + snow) * w;
            weightSum += w;
        }
        SurfA[idx] = weightSum > 1e-6f ? sum / weightSum : (BaseHeights[idx] + max(0.0f, Thickness[idx]));
    }
    else
    {
        [loop]
        for (int oz = -radius; oz <= radius; ++oz)
        {
            uint sz = (uint)clamp((int)z + oz, 0, (int)resolution - 1);
            uint sidx = CellIndex(x, sz);
            float snow = max(0.0f, Thickness[sidx]);
            float w = exp(-(float)(oz * oz) * invTwoSigma2) * SnowCoverage(snow);
            sum += SurfA[sidx] * w;
            weightSum += w;
        }

        float original = max(0.0f, Thickness[idx]);
        float blurredSurface = weightSum > 1e-6f ? sum / weightSum : (BaseHeights[idx] + original);
        float targetThickness = max(0.0f, blurredSurface - BaseHeights[idx]);
        float blend = saturate(surfaceSmoothing) * SnowCoverage(original);
        Thickness[idx] = max(0.0f, lerp(original, targetThickness, blend));
    }
}

[numthreads(8, 8, 1)]
void CSApply(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    uint i = CellIndex(x, z);
    float snow = max(0.0f, Thickness[i]);
    OutHeights[i] = BaseHeights[i] + snow;

    OutMask[i] = SnowCoverage(snow);
}
