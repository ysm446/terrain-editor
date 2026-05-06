// Multi-Scale Erosion compute shaders
//
// Direct HLSL port of the GLSL compute shaders from
//   https://github.com/H-Schott/MultiScaleErosion (MIT)
// Reference: Schott et al., "Terrain Amplification using Multi-scale Erosion",
//   ACM Transactions on Graphics, SIGGRAPH 2024.
//
// Three entry points map to the three CPU step functions in node_graph.cpp
// (mse::StepStreamPower / mse::StepThermal / mse::StepDeposition):
//   * CSStreamPower  — D8 weighted-flow accumulation + stream power erosion
//   * CSThermal      — talus / angle-of-repose redistribution (3x3 wraparound)
//   * CSDeposition   — sediment transport with rain-driven flow
//
// Each pass uses ping-pong buffers (HeightIn/Out, StreamIn/Out, SedIn/Out).
// The CPU evaluator dispatches the three shaders in order, swapping input
// and output bindings between iterations.

cbuffer MseConstants : register(b0)
{
    uint resolution;
    float terrainSizeMeters;
    float cellSizeMeters;
    float cellDiag;          // = cellSizeMeters * sqrt(2)

    // Resolution-invariant scaling (see node_graph.cpp comment near
    // mse::kRefCellSize). matter and rain*cellArea use this constant
    // instead of the live cellSize² to keep results stable across
    // simulation resolutions.
    float refCellArea;       // = 16.0f (= 4m * 4m)

    // Stream Power Erosion (erosion.glsl uniforms)
    float speStrength;       // k
    float streamExponent;    // p_sa
    float slopeExponent;     // p_sl
    float maxStreamPower;    // max_spe
    float flowExponent;      // flow_p, also used by deposition
    float speTimeStep;       // dt

    // Thermal (thermal.glsl uniforms)
    float thermalTanAngle;   // tan(thermalAngleDegrees * pi/180)
    float thermalStrength;   // eps
    uint  thermalNoisifyAngle;
    float thermalNoiseMin;
    float thermalNoiseMax;
    float thermalNoiseWavelength;

    // Deposition (deposition.glsl uniforms)
    float depositionStrength;
    float rain;

    float _pad0;
    float _pad1;
};

RWStructuredBuffer<float> HeightIn  : register(u0);
RWStructuredBuffer<float> HeightOut : register(u1);
RWStructuredBuffer<float> StreamIn  : register(u2);
RWStructuredBuffer<float> StreamOut : register(u3);
RWStructuredBuffer<float> SedIn     : register(u4);
RWStructuredBuffer<float> SedOut    : register(u5);

static const int2 kNext8[8] = {
    int2( 0,  1), int2( 1,  1), int2( 1,  0), int2( 1, -1),
    int2( 0, -1), int2(-1, -1), int2(-1,  0), int2(-1,  1)
};

uint IndexAt(int x, int z)
{
    return (uint)z * resolution + (uint)x;
}

bool InBounds(int x, int z)
{
    return x >= 0 && x < (int)resolution && z >= 0 && z < (int)resolution;
}

// Weighted D8 outflow distribution at cell (x, z): for each downhill neighbour,
// pow(slope, flowExponent), normalized to sum to 1. Uphill / OOB neighbours
// receive weight 0. Mirrors mse::GetFlowWeighted on the CPU.
void GetFlowWeights(int x, int z, out float weights[8])
{
    [unroll]
    for (int k = 0; k < 8; ++k) { weights[k] = 0.0f; }

    if (!InBounds(x, z)) { return; }
    const float h = HeightIn[IndexAt(x, z)];
    float sum = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const int qx = x + kNext8[i].x;
        const int qz = z + kNext8[i].y;
        if (!InBounds(qx, qz)) { continue; }
        const float hq = HeightIn[IndexAt(qx, qz)];
        const float ax = (float)kNext8[i].x * cellSizeMeters;
        const float az = (float)kNext8[i].y * cellSizeMeters;
        const float d = sqrt(ax * ax + az * az);
        const float slope = (h - hq) / d;
        if (slope > 0.0f)
        {
            const float w = pow(slope, flowExponent);
            weights[i] = w;
            sum += w;
        }
    }
    if (sum > 1e-6f)
    {
        [unroll]
        for (int j = 0; j < 8; ++j) { weights[j] /= sum; }
    }
}

// Steepest descent direction and its slope magnitude. dx/dz are in {-1,0,1}
// (zero if the cell is a local minimum / pit, in which case slope is 0).
void GetSteepestDescent(int x, int z, out int outDx, out int outDz, out float outSlope)
{
    outDx = 0;
    outDz = 0;
    outSlope = 0.0f;
    if (!InBounds(x, z)) { return; }
    const float h = HeightIn[IndexAt(x, z)];
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const int qx = x + kNext8[i].x;
        const int qz = z + kNext8[i].y;
        if (!InBounds(qx, qz)) { continue; }
        const float hq = HeightIn[IndexAt(qx, qz)];
        const float ax = (float)kNext8[i].x * cellSizeMeters;
        const float az = (float)kNext8[i].y * cellSizeMeters;
        const float d = sqrt(ax * ax + az * az);
        const float s = (h - hq) / d;
        if (s > outSlope)
        {
            outSlope = s;
            outDx = kNext8[i].x;
            outDz = kNext8[i].y;
        }
    }
}

// 2D value-noise approximation matching the CPU port (mse::ValueNoise2D).
// The reference shader uses 3D simplex noise on (x, y, height); we keep this
// cheaper 2D variant for parity with the CPU path.
float Hash2(float x, float y)
{
    const float s = sin(x * 12.9898f + y * 78.233f) * 43758.5453123f;
    return s - floor(s);
}

float ValueNoise2D(float x, float y)
{
    const float xi = floor(x);
    const float yi = floor(y);
    const float xf = x - xi;
    const float yf = y - yi;
    const float a = Hash2(xi,        yi);
    const float b = Hash2(xi + 1.0f, yi);
    const float c = Hash2(xi,        yi + 1.0f);
    const float d = Hash2(xi + 1.0f, yi + 1.0f);
    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = yf * yf * (3.0f - 2.0f * yf);
    return lerp(lerp(a, b, u), lerp(c, d, v), v) * 2.0f - 1.0f;
}

// =============================================================================
// SPE — Stream Power Erosion
// erosion.glsl: stream = cellDiag + Σ weighted upstream stream
//               spe    = clamp(stream^p_sa * clamp(slope^p_sl, 0, 1), 0, max) * k
//               h     -= dt * spe, clamped to receiver_height from below
// =============================================================================
[numthreads(8, 8, 1)]
void CSStreamPower(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint x = dispatchThreadId.x;
    const uint z = dispatchThreadId.y;
    if (x >= resolution || z >= resolution) { return; }

    const uint id = IndexAt((int)x, (int)z);

    // Incoming flow: for each neighbour q, look up q's outflow weight that
    // points back toward us (index (i+4)%8) and weight q's stream by it.
    float incoming = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const int qx = (int)x + kNext8[i].x;
        const int qz = (int)z + kNext8[i].y;
        if (!InBounds(qx, qz)) { continue; }
        float qWeights[8];
        GetFlowWeights(qx, qz, qWeights);
        const float w = qWeights[(i + 4) % 8];
        if (w > 0.0f)
        {
            incoming += w * StreamIn[IndexAt(qx, qz)];
        }
    }
    const float stream = cellDiag + incoming;

    int sdx = 0;
    int sdz = 0;
    float steepest = 0.0f;
    GetSteepestDescent((int)x, (int)z, sdx, sdz, steepest);

    const float receiverHeight = (sdx == 0 && sdz == 0)
        ? HeightIn[id]
        : HeightIn[IndexAt((int)x + sdx, (int)z + sdz)];

    float spe = pow(stream, streamExponent) * saturate(pow(steepest, slopeExponent));
    spe = clamp(spe, 0.0f, maxStreamPower) * speStrength;

    const float oldHeight = HeightIn[id];
    float newHeight = oldHeight - speTimeStep * spe;
    newHeight = max(newHeight, receiverHeight);

    HeightOut[id] = newHeight;
    StreamOut[id] = stream;
}

// =============================================================================
// Thermal — talus / angle-of-repose redistribution
// thermal.glsl: 3x3 stencil with wraparound; for each neighbour, count
//               receiveMul / distributeMul if the slope exceeds tan(angle).
//               h += matter * (receiveMul - distributeMul)
// =============================================================================
[numthreads(8, 8, 1)]
void CSThermal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint x = dispatchThreadId.x;
    const uint z = dispatchThreadId.y;
    if (x >= resolution || z >= resolution) { return; }

    const uint id = IndexAt((int)x, (int)z);
    const float h = HeightIn[id];

    // Resolution-invariant matter (anchored to refCellArea, see CPU port).
    const float matter = thermalStrength * refCellArea;

    float tanAngle = thermalTanAngle;
    if (thermalNoisifyAngle != 0)
    {
        const float t = ValueNoise2D(
            (float)x * thermalNoiseWavelength * (float)resolution,
            (float)z * thermalNoiseWavelength * (float)resolution) * 0.5f + 0.5f;
        tanAngle = thermalTanAngle * lerp(thermalNoiseMin, thermalNoiseMax, t);
    }

    float receiveMul = 0.0f;
    float distributeMul = 0.0f;
    [unroll]
    for (int j = -1; j <= 1; ++j)
    {
        [unroll]
        for (int i = -1; i <= 1; ++i)
        {
            if (i == 0 && j == 0) { continue; }
            // Wraparound — HLSL `%` on negative ints can return negative,
            // so add resolution before second mod to normalize.
            const int qx = (((int)x + i) % (int)resolution + (int)resolution) % (int)resolution;
            const int qz = (((int)z + j) % (int)resolution + (int)resolution) % (int)resolution;
            const float ax = (float)i * cellSizeMeters;
            const float az = (float)j * cellSizeMeters;
            const float d = sqrt(ax * ax + az * az);
            const float hq = HeightIn[IndexAt(qx, qz)];
            if ((hq - h) / d > tanAngle) { receiveMul += 1.0f; }
            if ((h - hq) / d > tanAngle) { distributeMul += 1.0f; }
        }
    }

    HeightOut[id] = h + matter * (receiveMul - distributeMul);
}

// =============================================================================
// Deposition — sediment transport and pit accumulation
// deposition.glsl: stream = rain*cellArea + Σ weighted upstream stream
//                  sed    = (pit-only retain) + Σ weighted upstream sed
//                           + 0.1 * streamPower
//                  Deposit when deposition_strength*sed > streamPower.
// =============================================================================
[numthreads(8, 8, 1)]
void CSDeposition(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint x = dispatchThreadId.x;
    const uint z = dispatchThreadId.y;
    if (x >= resolution || z >= resolution) { return; }

    const uint id = IndexAt((int)x, (int)z);
    const float h = HeightIn[id];
    float sed = SedIn[id];

    int sdx = 0;
    int sdz = 0;
    float steepest = 0.0f;
    GetSteepestDescent((int)x, (int)z, sdx, sdz, steepest);
    // Match deposition.glsl `if (!CheckPit(p)) sed = 0`: only local minima
    // (pits, no descent direction) retain sediment between iterations.
    const bool isPit = (sdx == 0 && sdz == 0);
    if (!isPit) { sed = 0.0f; }

    // Resolution-invariant cellArea for rain, anchored to refCellArea
    // (mirrors the CPU port).
    const float cellArea = refCellArea * 0.00001f;

    float incomingStream = 0.0f;
    float incomingSed = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        const int qx = (int)x + kNext8[i].x;
        const int qz = (int)z + kNext8[i].y;
        if (!InBounds(qx, qz)) { continue; }
        float qWeights[8];
        GetFlowWeights(qx, qz, qWeights);
        const float w = qWeights[(i + 4) % 8];
        if (w > 0.0f)
        {
            const uint qid = IndexAt(qx, qz);
            incomingStream += w * StreamIn[qid];
            incomingSed    += w * SedIn[qid];
        }
    }
    const float stream = rain * cellArea + incomingStream;
    sed += incomingSed;

    const float speed = saturate(pow(steepest, 2.0f));
    const float streamPower = pow(max(stream, 1e-12f), 0.3f) * speed;

    float newHeight = h;
    if (depositionStrength * sed > streamPower)
    {
        const float deposit = min(sed, (depositionStrength * sed - streamPower) * 0.1f);
        newHeight += deposit;
        sed = max(0.0f, sed - deposit);
    }
    sed += 0.1f * streamPower;

    HeightOut[id] = newHeight;
    StreamOut[id] = stream;
    SedOut[id]    = sed;
}
