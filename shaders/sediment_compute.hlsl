// Sediment compute shader.
//
// GPU port of the multi-scale gravity-sliding sediment simulation in
// src/node_graph.cpp (sediment_geogen::ThermalSlideUnitStride). Each
// sliding pass is split into two race-free sweeps:
//
//   CSSlideSweep1: read bedrock + sediment, compute per-cell directional
//                  outgoing shares using the (n+1) divisor formula
//                  (drops[k] / (activeCount + 1)) so post-flow slope
//                  converges to talusH in a single step.
//   CSSlideSweep2: read self outgoing + 4 neighbours' opposite-direction
//                  outgoing, write new sediment value.
//
// Auxiliary entry points:
//   CSSetup: initialise bedrock / sediment from input heights according
//            to the `convertTerrainToSediment` flag.
//   CSEmit:  add emissionPerIter to every sediment cell.
//
// Buffer layout (StructuredBuffer<float>, row-major float[res*res]):
//   u0 = bedrock        (read in sweep 1, written in CSSetup)
//   u1 = sediment       (read in sweep 1, written in sweep 2 / setup / emit)
//   u2 = outgoing       (4 floats per cell: 0=east,1=west,2=south,3=north)
//   u3 = inputHeights   (read-only in CSSetup)
//
// Outgoing direction encoding (matches CPU side):
//   k=0: east   (dx=+1, dz=0)
//   k=1: west   (dx=-1, dz=0)
//   k=2: south  (dx=0,  dz=+1)
//   k=3: north  (dx=0,  dz=-1)
//   opposite[k] = {1, 0, 3, 2}

cbuffer SedimentConstants : register(b0)
{
    uint  resolution;
    float talusH;
    float emissionPerIter;
    uint  convertTerrainToSediment;
};

RWStructuredBuffer<float> Bedrock      : register(u0);
RWStructuredBuffer<float> Sediment     : register(u1);
RWStructuredBuffer<float> Outgoing     : register(u2);  // 4 floats per cell
RWStructuredBuffer<float> InputHeights : register(u3);

[numthreads(8, 8, 1)]
void CSSetup(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint x = dispatchThreadID.x;
    uint z = dispatchThreadID.y;
    if (x >= resolution || z >= resolution) return;

    uint i = z * resolution + x;
    float h = InputHeights[i];
    if (convertTerrainToSediment != 0u)
    {
        // Treat the input height itself as movable sediment over a flat
        // bedrock = 0, so the entire mountain can be reshaped by gravity.
        Bedrock[i]  = 0.0f;
        Sediment[i] = h;
    }
    else
    {
        // Input is fixed bedrock; sediment starts empty (only Emission Amount fills it).
        Bedrock[i]  = h;
        Sediment[i] = 0.0f;
    }
}

[numthreads(8, 8, 1)]
void CSEmit(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint x = dispatchThreadID.x;
    uint z = dispatchThreadID.y;
    if (x >= resolution || z >= resolution) return;
    uint i = z * resolution + x;
    Sediment[i] += emissionPerIter;
}

[numthreads(8, 8, 1)]
void CSSlideSweep1(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint x = dispatchThreadID.x;
    uint z = dispatchThreadID.y;
    if (x >= resolution || z >= resolution) return;

    uint i = z * resolution + x;
    float h = Bedrock[i] + Sediment[i];

    // 4-connected neighbours: east, west, south, north.
    int dxs[4] = {+1, -1, 0, 0};
    int dzs[4] = {0, 0, +1, -1};

    float drops[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float totalDrop = 0.0f;
    int activeCount = 0;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        int nx = (int)x + dxs[k];
        int nz = (int)z + dzs[k];
        if (nx < 0 || nx >= (int)resolution || nz < 0 || nz >= (int)resolution) continue;
        uint j = (uint)nz * resolution + (uint)nx;
        float diff = h - Bedrock[j] - Sediment[j];
        if (diff > talusH)
        {
            drops[k] = diff - talusH;
            totalDrop += drops[k];
            ++activeCount;
        }
    }

    uint base = i * 4u;
    Outgoing[base + 0u] = 0.0f;
    Outgoing[base + 1u] = 0.0f;
    Outgoing[base + 2u] = 0.0f;
    Outgoing[base + 3u] = 0.0f;
    if (activeCount == 0 || totalDrop <= 0.0f) return;

    // (n+1)-divisor flow: ideal per-neighbour = drops[k] / (active+1).
    // Cap by available sediment, scale all shares uniformly.
    float divisor = (float)(activeCount + 1);
    float idealOut = totalDrop / divisor;
    float available = max(Sediment[i], 0.0f);
    float actualOut = min(available, idealOut);
    float scale = (idealOut > 0.0f) ? (actualOut / idealOut) : 0.0f;
    [unroll]
    for (int kk = 0; kk < 4; ++kk)
    {
        if (drops[kk] > 0.0f)
        {
            Outgoing[base + (uint)kk] = (drops[kk] / divisor) * scale;
        }
    }
}

[numthreads(8, 8, 1)]
void CSSlideSweep2(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint x = dispatchThreadID.x;
    uint z = dispatchThreadID.y;
    if (x >= resolution || z >= resolution) return;

    uint i = z * resolution + x;
    uint base = i * 4u;
    float totalOut = Outgoing[base + 0u] + Outgoing[base + 1u] + Outgoing[base + 2u] + Outgoing[base + 3u];

    // For each direction k pointing from this cell to neighbour j, read
    // the share that neighbour j sent back toward this cell — which is
    // stored in j's outgoing slot for the opposite direction.
    int dxs[4] = {+1, -1, 0, 0};
    int dzs[4] = {0, 0, +1, -1};
    int oppositeK[4] = {1, 0, 3, 2};

    float incoming = 0.0f;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        int nx = (int)x + dxs[k];
        int nz = (int)z + dzs[k];
        if (nx < 0 || nx >= (int)resolution || nz < 0 || nz >= (int)resolution) continue;
        uint j = (uint)nz * resolution + (uint)nx;
        incoming += Outgoing[j * 4u + (uint)oppositeK[k]];
    }

    Sediment[i] = max(0.0f, Sediment[i] - totalOut + incoming);
}
