// Fluvial Erosion compute shader.
//
// GPU port of the KTT-style force-field particle transport in
// src/evaluation/FluvialErosion.cpp (RunFluvialLevel). The structure maps
// 1:1 onto the CPU kernel: each iteration traces all particles against a
// frozen height/wear snapshot (nothing writes Heights/Wear until CSApply,
// so the buffers themselves act as the snapshot), accumulates per-cell
// height/wear deltas, then applies them with the soft-saturated sum.
//
// Particle splats are accumulated as fixed-point int32 (kFixedScale per
// metre) via InterlockedAdd: HLSL has no float atomics, and fixed-point
// addition is order-independent, which makes the GPU result fully
// deterministic (the CPU backend's float atomics are not).
//
// Pipeline per pyramid level (driven from FluvialErosionCompute.cpp):
//
//   CSClearLevel  — zero Wear / Flow / Deposit at level start.
//   per iteration:
//     CSClearIter — zero the per-iteration DeltaH / DeltaW accumulators.
//     CSTrace     — one thread per particle: spawn from the stateless PRNG,
//                   walk up to `steps` cells along the force field, splat
//                   erosion / deposition (fixed-point bilinear).
//     CSApply     — per cell: convert the fixed-point sums to metres and
//                   apply with the soft saturation cap (deltaCap).
//   between levels:
//     CSCopyToSrc — copy Heights into SrcHeights at the previous resolution.
//     CSUpsample  — bilinear-resample SrcHeights (srcN) into Heights (n).
//
// Buffer layout (sized for the target resolution, levels use the prefix):
//   u0 = Heights    (float, metres)
//   u1 = SrcHeights (float, upsample source)
//   u2 = Wear       (float, cumulative carve for the flowVolume feedback)
//   u3 = DeltaH     (int, fixed-point per-iteration height delta sum)
//   u4 = DeltaW     (int, fixed-point per-iteration wear delta sum)
//   u5 = Flow       (uint, visit counter -> Flows output)
//   u6 = Deposit    (int, fixed-point cumulative deposits -> Deposits output)

cbuffer FluvialErosionConstants : register(b0)
{
    uint  n;             // current level resolution
    uint  srcN;          // upsample source resolution
    uint  particleCount; // particles this iteration
    uint  steps;         // max walk length in cells

    int   seed;
    int   levelSeed;
    int   iter;
    float cellSize;      // metres per cell at this level

    float friction;
    float erodeStrength; // erosionStrength * geologicalAge gain
    float channeling;
    float sedimentVelocity;

    float flowVolume;
    float tanWear;
    float tanDeposit;
    float tanMax;

    float deltaCap;      // per-iteration per-cell apply cap (metres)
    float pad0;
    float pad1;
    float pad2;
};

RWStructuredBuffer<float> Heights    : register(u0);
RWStructuredBuffer<float> SrcHeights : register(u1);
RWStructuredBuffer<float> Wear       : register(u2);
RWStructuredBuffer<int>   DeltaH     : register(u3);
RWStructuredBuffer<int>   DeltaW     : register(u4);
RWStructuredBuffer<uint>  Flow       : register(u5);
RWStructuredBuffer<int>   Deposit    : register(u6);

static const float kFixedScale = 4096.0f;   // fixed-point counts per metre
static const float kWearRate = 0.05f;       // matches FluvialErosion.cpp
static const float kDepositRate = 0.25f;    // matches FluvialErosion.cpp

// --- PRNG (bit-exact port of FluvialErosion.cpp) ----------------------------

uint SeedFor(int seedV, int levelSeedV, int iterV, int particle)
{
    uint h = uint(seedV) * 2654435761u;
    h = (h ^ uint(levelSeedV + 1)) * 2246822519u;
    h = (h ^ uint(iterV + 1)) * 3266489917u;
    h = (h ^ uint(particle)) * 668265263u;
    return h ^ (h >> 15);
}

float Hash01(inout uint s)
{
    s += 0x9e3779b9u;
    uint z = s;
    z = (z ^ (z >> 16)) * 0x21f0aaadu;
    z = (z ^ (z >> 15)) * 0x735a2d97u;
    z = z ^ (z >> 15);
    return float(z >> 8) * (1.0f / 16777216.0f);
}

// --- Bilinear sampling (ports of ParticleErosionCommon.h) -------------------

int Index1D(int x, int z)
{
    return z * int(n) + x;
}

void BilinearCorners(float px, float pz, out int x0, out int z0, out int x1, out int z1, out float u, out float v)
{
    x0 = clamp(int(floor(px)), 0, int(n) - 1);
    z0 = clamp(int(floor(pz)), 0, int(n) - 1);
    x1 = min(x0 + 1, int(n) - 1);
    z1 = min(z0 + 1, int(n) - 1);
    u = px - float(x0);
    v = pz - float(z0);
}

// height + analytic gradient of the bilinear surface of `buf` at (px, pz).
void SampleHeightGradient(RWStructuredBuffer<float> buf, float px, float pz,
                          out float height, out float gradX, out float gradZ)
{
    int x0, z0, x1, z1;
    float u, v;
    BilinearCorners(px, pz, x0, z0, x1, z1, u, v);
    const float nw = buf[Index1D(x0, z0)];
    const float ne = buf[Index1D(x1, z0)];
    const float sw = buf[Index1D(x0, z1)];
    const float se = buf[Index1D(x1, z1)];
    gradX = lerp(ne - nw, se - sw, v);
    gradZ = lerp(sw - nw, se - ne, u);
    height = lerp(lerp(nw, ne, u), lerp(sw, se, u), v);
}

int ToFixed(float metres)
{
    return int(metres * kFixedScale + (metres >= 0.0f ? 0.5f : -0.5f));
}

void SplatFixed(RWStructuredBuffer<int> buf, float px, float pz, float amount)
{
    int x0, z0, x1, z1;
    float u, v;
    BilinearCorners(px, pz, x0, z0, x1, z1, u, v);
    int orig;
    InterlockedAdd(buf[Index1D(x0, z0)], ToFixed(amount * (1.0f - u) * (1.0f - v)), orig);
    InterlockedAdd(buf[Index1D(x1, z0)], ToFixed(amount * u * (1.0f - v)), orig);
    InterlockedAdd(buf[Index1D(x0, z1)], ToFixed(amount * (1.0f - u) * v), orig);
    InterlockedAdd(buf[Index1D(x1, z1)], ToFixed(amount * u * v), orig);
}

// --- Per-cell housekeeping ---------------------------------------------------

[numthreads(256, 1, 1)]
void CSClearLevel(uint3 dt : SV_DispatchThreadID)
{
    const uint i = dt.x;
    if (i >= n * n) return;
    Wear[i] = 0.0f;
    Flow[i] = 0u;
    Deposit[i] = 0;
}

[numthreads(256, 1, 1)]
void CSClearIter(uint3 dt : SV_DispatchThreadID)
{
    const uint i = dt.x;
    if (i >= n * n) return;
    DeltaH[i] = 0;
    DeltaW[i] = 0;
}

// --- Particle trace (port of the RunFluvialLevel lambda) ---------------------

[numthreads(64, 1, 1)]
void CSTrace(uint3 dt : SV_DispatchThreadID)
{
    const uint p = dt.x;
    if (p >= particleCount) return;

    uint rng = SeedFor(seed, levelSeed, iter, int(p));
    const float spawnRange = float(int(n) - 3); // start positions in [1, n-2]
    float px = 1.0f + Hash01(rng) * spawnRange;
    float pz = 1.0f + Hash01(rng) * spawnRange;
    float velX = 0.0f;
    float velZ = 0.0f;
    float sediment = 0.0f; // carved material carried by this particle (m)

    [loop]
    for (uint stepIndex = 0; stepIndex < steps; ++stepIndex) // `step` is an HLSL intrinsic
    {
        float h, gx, gz;
        SampleHeightGradient(Heights, px, pz, h, gx, gz);
        if (flowVolume > 0.0f)
        {
            float wh, wgx, wgz;
            SampleHeightGradient(Wear, px, pz, wh, wgx, wgz);
            gx -= flowVolume * wgx;
            gz -= flowVolume * wgz;
        }
        // Divide by cell size so the force is a true slope (rise/run).
        const float fx = -gx / cellSize;
        const float fz = -gz / cellSize;
        const float slope = sqrt(fx * fx + fz * fz);

        velX = velX * (1.0f - friction) + fx * sedimentVelocity;
        velZ = velZ * (1.0f - friction) + fz * sedimentVelocity;
        const float velLen = sqrt(velX * velX + velZ * velZ);
        if (velLen < 1e-6f) { break; }
        const float sx = velX / velLen;
        const float sz = velZ / velLen;

        uint origFlow;
        InterlockedAdd(Flow[Index1D(int(px), int(pz))], 1u, origFlow);

        // Fade all terrain edits out near the map border (see the CPU kernel
        // comment: every off-map path terminates there).
        const float edgeDist = min(min(px, pz), min(float(int(n) - 1) - px, float(int(n) - 1) - pz));
        const float edgeFade = saturate((edgeDist - 1.0f) / 3.0f);

        // Effective slope: terrain slope or the momentum-equivalent slope,
        // whichever is larger (carries erosion onto the gentler lower slopes).
        const float velSlope = velLen * friction / sedimentVelocity;
        const float effSlope = max(slope, velSlope);
        const bool movingDownhill = (velX * fx + velZ * fz) > 0.0f;

        if (movingDownhill && effSlope >= tanWear && slope <= tanMax)
        {
            const float erode = min(erodeStrength * effSlope * cellSize * kWearRate * edgeFade, deltaCap);
            if (erode > 0.0f)
            {
                SplatFixed(DeltaH, px, pz, -erode);
                SplatFixed(DeltaW, px, pz, erode);
                sediment += erode;
            }
        }
        else if (effSlope < tanDeposit && sediment > 0.0f)
        {
            const float released = sediment * kDepositRate;
            sediment -= released;
            const float dep = released * (1.0f - channeling) * edgeFade;
            if (dep > 0.0f)
            {
                SplatFixed(DeltaH, px, pz, dep);
                SplatFixed(Deposit, px, pz, dep);
            }
        }

        const float npx = px + sx;
        const float npz = pz + sz;
        if (npx < 1.0f || npx > float(int(n) - 2) || npz < 1.0f || npz > float(int(n) - 2))
        {
            break;
        }
        px = npx;
        pz = npz;
    }
}

// --- Apply (soft-saturated sum, port of the end-of-iteration loop) -----------

[numthreads(256, 1, 1)]
void CSApply(uint3 dt : SV_DispatchThreadID)
{
    const uint i = dt.x;
    if (i >= n * n) return;

    const float sum = float(DeltaH[i]) / kFixedScale;
    if (sum != 0.0f)
    {
        Heights[i] += sum / (1.0f + abs(sum) / deltaCap);
    }
    const float w = float(DeltaW[i]) / kFixedScale;
    if (w > 0.0f)
    {
        Wear[i] += w / (1.0f + w / deltaCap);
    }
}

// --- Level-to-level upsample --------------------------------------------------

[numthreads(256, 1, 1)]
void CSCopyToSrc(uint3 dt : SV_DispatchThreadID)
{
    const uint i = dt.x;
    if (i >= srcN * srcN) return;
    SrcHeights[i] = Heights[i];
}

[numthreads(256, 1, 1)]
void CSUpsample(uint3 dt : SV_DispatchThreadID)
{
    const uint i = dt.x;
    if (i >= n * n) return;
    const uint x = i % n;
    const uint z = i / n;
    const float u = n > 1 ? float(x) / float(n - 1) : 0.0f;
    const float v = n > 1 ? float(z) / float(n - 1) : 0.0f;
    const float sx = u * float(max(1u, srcN - 1u));
    const float sz = v * float(max(1u, srcN - 1u));

    const int x0 = clamp(int(floor(sx)), 0, int(srcN) - 1);
    const int z0 = clamp(int(floor(sz)), 0, int(srcN) - 1);
    const int x1 = min(x0 + 1, int(srcN) - 1);
    const int z1 = min(z0 + 1, int(srcN) - 1);
    const float fu = sx - float(x0);
    const float fv = sz - float(z0);
    const float nw = SrcHeights[z0 * int(srcN) + x0];
    const float ne = SrcHeights[z0 * int(srcN) + x1];
    const float sw = SrcHeights[z1 * int(srcN) + x0];
    const float se = SrcHeights[z1 * int(srcN) + x1];
    Heights[i] = lerp(lerp(nw, ne, fu), lerp(sw, se, fu), fv);
}
