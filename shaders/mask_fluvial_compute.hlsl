// Mask Fluvial compute shader.
//
// GPU port of the MFD flow accumulation in src/node_graph.cpp
// (ApplyMaskFluvial). The CPU version is a sort + topological walk
// (inherently sequential). The GPU version uses an iterative Jacobi
// gather instead — all cells update in parallel each iteration, with
// information propagating one cell per iteration. This converges in
// O(longest_path) iterations to a result that is **visually equivalent**
// to the CPU output but not bit-identical (gather order differs, so
// floating-point accumulation order differs).
//
// Pipeline:
//
//   CSCopyInputHeights — copy raw heights into the analysis height buffer.
//   CSBlurHorizontal / CSBlurVertical — optional Largest Detail Level
//                       low-pass on analysis heights only.
//   CSPitFillJacobi  — Jacobi double-buffered pit fill, one dispatch per
//                       iteration (read from PrevHeights, write to NextHeights).
//   CSCommitHeights  — copy PitFill result back into Heights for the rest
//                       of the pipeline.
//   CSComputeWeights — MFD per-cell receiver weights into 8-float
//                       OutWeights (k order: NW, N, NE, W, E, SW, S, SE).
//   CSAccumIter      — Jacobi gather: for each cell c, scan its 8
//                       neighbours; if neighbour n's weight in the direction
//                       toward c is > 0, add accum_prev[n] * weight to c.
//                       Repeat ~2 * resolution times.
//   CSToMaskLog      — log(1+a) / log(1+max) ramp + gamma.
//   CSToMaskThreshold — smoothstep + power threshold.
//   CSToMaskLinear   — linear ramp + gamma.
//
// Buffer layout (all RWStructuredBuffer<float> over res*res):
//   u0 = Heights      (pit-filled heights, double-buffered with HeightsScratch)
//   u1 = HeightsScratch
//   u2 = Weights      (8 floats per cell: k=0..7 = NW,N,NE,W,E,SW,S,SE)
//   u3 = AccumA       (accumulator, double-buffered with AccumB)
//   u4 = AccumB
//   u5 = OutMask
//   u6 = MaxScratch   (single-element float for max reduction)
//   u7 = InputHeights (raw heights from CPU upload)

cbuffer MaskFluvialConstants : register(b0)
{
    uint  resolution;
    uint  algorithmIsMfd;   // Legacy padding slot. Mask Fluvial evaluates as MFD.
    float mfdExponent;
    uint  accumDirection;   // 0: read AccumA → write AccumB.  1: read AccumB → write AccumA.

    // Mask conversion params
    float thresholdCells;
    float gamma;
    float softness;
    float power;

    uint  outputCurve;      // 0=Log, 1=Threshold, 2=Linear
    float inertia;          // Legacy padding slot. Inertia is fixed to 0 in Mask Fluvial.
    uint  detailBlurRadius; // Largest Detail Level converted to cells.
    uint  pad0;
    uint  pad1;
    uint  pad2;
    uint  pad3;
    uint  pad4;
};

RWStructuredBuffer<float> Heights       : register(u0);
RWStructuredBuffer<float> HeightsScratch: register(u1);
RWStructuredBuffer<float> Weights       : register(u2); // 8 per cell
RWStructuredBuffer<float> AccumA        : register(u3);
RWStructuredBuffer<float> AccumB        : register(u4);
RWStructuredBuffer<float> OutMask       : register(u5);
// MaxScratch is a single-element atomic uint reinterpreted as float
// for reductions. We use uint atomics on a sortable bit pattern of
// positive floats: float bits compare like ints when both are positive,
// which all accum values are (>= 0).
RWStructuredBuffer<uint> MaxScratch     : register(u6);
RWStructuredBuffer<float> InputHeights  : register(u7);

// Direction encoding (matches CPU kDx/kDz tables exactly):
//   k=0: NW(-1,-1), 1:N(0,-1), 2:NE(1,-1)
//   k=3: W (-1, 0), 4:E(1, 0)
//   k=5: SW(-1, 1), 6:S(0, 1), 7:SE(1, 1)
static const int kDx[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
static const int kDz[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };
static const float kDist[8] = {
    1.41421356f, 1.0f, 1.41421356f,
    1.0f,              1.0f,
    1.41421356f, 1.0f, 1.41421356f
};
// opposite[k] = direction from neighbour back to me. So if I'm cell c
// and my neighbour at offset (kDx[k], kDz[k]) has weight > 0 in
// direction opposite[k], then n flows to me.
//   opposite[NW=0] = SE=7, opposite[N=1] = S=6, opposite[NE=2] = SW=5
//   opposite[W=3] = E=4,   opposite[E=4] = W=3
//   opposite[SW=5] = NE=2, opposite[S=6] = N=1, opposite[SE=7] = NW=0
static const uint kOpposite[8] = { 7, 6, 5, 4, 3, 2, 1, 0 };

[numthreads(8, 8, 1)]
void CSCopyInputHeights(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    Heights[i] = InputHeights[i];
}

[numthreads(8, 8, 1)]
void CSBlurHorizontal(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    int radius = clamp((int)detailBlurRadius, 1, 64);
    float sigma = max(1.0f, (float)radius * 0.5f);
    float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (int ox = -radius; ox <= radius; ++ox)
    {
        uint sx = (uint)clamp((int)x + ox, 0, (int)resolution - 1);
        float w = exp(-(float)(ox * ox) * invTwoSigma2);
        sum += Heights[z * resolution + sx] * w;
        weightSum += w;
    }
    HeightsScratch[z * resolution + x] = sum / max(1e-6f, weightSum);
}

[numthreads(8, 8, 1)]
void CSBlurVertical(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    int radius = clamp((int)detailBlurRadius, 1, 64);
    float sigma = max(1.0f, (float)radius * 0.5f);
    float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);
    float sum = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (int oz = -radius; oz <= radius; ++oz)
    {
        uint sz = (uint)clamp((int)z + oz, 0, (int)resolution - 1);
        float w = exp(-(float)(oz * oz) * invTwoSigma2);
        sum += HeightsScratch[sz * resolution + x] * w;
        weightSum += w;
    }
    Heights[z * resolution + x] = sum / max(1e-6f, weightSum);
}

[numthreads(8, 8, 1)]
void CSPitFillJacobi(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;

    // Boundary cells act as outlets — pass through unchanged.
    if (x == 0u || x == resolution - 1u || z == 0u || z == resolution - 1u)
    {
        HeightsScratch[i] = Heights[i];
        return;
    }

    float h = Heights[i];
    float minN = 1e30f;
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        int nx = (int)x + kDx[k];
        int nz = (int)z + kDz[k];
        float nh = Heights[(uint)nz * resolution + (uint)nx];
        if (nh < minN) minN = nh;
    }
    const float kPitEpsilon = 1e-4f;
    HeightsScratch[i] = (h <= minN) ? (minN + kPitEpsilon) : h;
}

[numthreads(8, 8, 1)]
void CSCommitHeights(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    Heights[i] = HeightsScratch[i];
}

[numthreads(8, 8, 1)]
void CSComputeWeights(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    float h = Heights[i];

    // MFD — per-direction slope^p, normalised so all positive weights sum to 1.
    float w[8];
    float sum = 0.0f;
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        int nx = (int)x + kDx[k];
        int nz = (int)z + kDz[k];
        float wk = 0.0f;
        if (!(nx < 0 || nx >= (int)resolution || nz < 0 || nz >= (int)resolution))
        {
            float nh = Heights[(uint)nz * resolution + (uint)nx];
            float slope = (h - nh) / kDist[k];
            if (slope > 0.0f) wk = pow(slope, mfdExponent);
        }
        w[k] = wk;
        sum += wk;
    }
    float inv = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
    [unroll]
    for (int k3 = 0; k3 < 8; ++k3)
    {
        Weights[i * 8u + (uint)k3] = w[k3] * inv;
    }
}

[numthreads(8, 8, 1)]
void CSAccumInit(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    AccumA[i] = 1.0f;
}

// Jacobi gather: gather flow from upstream donors. CB.accumDirection
// alternates each iteration so we don't need to re-bind UAV slots —
// dir=0 reads AccumA writes AccumB, dir=1 reverses.
[numthreads(8, 8, 1)]
void CSAccumIter(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;

    // Total = self contribution (1.0) + Σ over donors n of accum[n] * weight[n→c]
    float total = 1.0f;
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        int nx = (int)x + kDx[k];
        int nz = (int)z + kDz[k];
        if (nx < 0 || nx >= (int)resolution || nz < 0 || nz >= (int)resolution) continue;
        uint nIdx = (uint)nz * resolution + (uint)nx;
        // Neighbour n's weight in the direction back toward me (opposite).
        float w = Weights[nIdx * 8u + kOpposite[k]];
        if (w > 0.0f)
        {
            float a = (accumDirection == 0u) ? AccumA[nIdx] : AccumB[nIdx];
            total += a * w;
        }
    }
    uint outIdx = z * resolution + x;
    if (accumDirection == 0u) AccumB[outIdx] = total;
    else                       AccumA[outIdx] = total;
}

[numthreads(8, 8, 1)]
void CSMaxReduce(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    float adjusted = max(0.0f, AccumA[i] - thresholdCells);
    // Atomic max on the bit pattern of a non-negative float (ordering
    // matches int ordering for IEEE-754 positive floats).
    InterlockedMax(MaxScratch[0], asuint(adjusted));
}

[numthreads(8, 8, 1)]
void CSToMaskLog(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    float a = max(0.0f, AccumA[i] - thresholdCells);
    // Read max from MaxScratch (computed by CSMaxReduce). Bit-cast uint→float.
    float maxAdjusted = asfloat(MaxScratch[0]);
    float invLogMax = 1.0f / max(log(1.0f + maxAdjusted), 1e-3f);
    float t = log(1.0f + a) * invLogMax;
    OutMask[i] = pow(saturate(t), gamma);
}

[numthreads(8, 8, 1)]
void CSToMaskLinear(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    float a = max(0.0f, AccumA[i] - thresholdCells);
    float maxAdjusted = asfloat(MaxScratch[0]);
    float invMax = 1.0f / max(maxAdjusted, 1e-3f);
    float t = a * invMax;
    OutMask[i] = pow(saturate(t), gamma);
}

[numthreads(8, 8, 1)]
void CSToMaskThreshold(uint3 dt : SV_DispatchThreadID)
{
    uint x = dt.x;
    uint z = dt.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    float a = AccumA[i];
    float thresholdLow = max(1.0f, thresholdCells);
    float softnessClamped = clamp(softness, 0.001f, 4.0f);
    float thresholdHigh = thresholdLow * (1.0f + 4.0f * softnessClamped);
    float invRange = 1.0f / max(thresholdHigh - thresholdLow, 1e-3f);
    float t = saturate((a - thresholdLow) * invRange);
    float smoothT = t * t * (3.0f - 2.0f * t);
    OutMask[i] = pow(smoothT, power);
}
