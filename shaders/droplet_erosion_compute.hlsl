// Droplet Erosion compute shader.
//
// GPU port of the capacity-based ("Sebastian Lague / Hans Beyer" style)
// particle hydraulic erosion in src/evaluation/DropletErosion.cpp.
//
// The CPU kernel is strictly sequential: each droplet mutates the heightfield
// in place and later droplets (and later steps of the same droplet) see those
// edits. That cannot be reproduced in parallel. Instead this backend uses the
// same snapshot reformulation as the Fluvial Erosion compute shader: per
// iteration, every droplet traces against a frozen Heights snapshot (nothing
// writes Heights until CSApply), splats its erosion / deposition into a
// per-iteration delta buffer, and the deltas are then applied with a
// soft-saturated sum. The next iteration sees the carved channels, so the
// drainage network deepens and branches over the passes. Results are visually
// equivalent to the CPU backend but not bit-exact.
//
// Splats are accumulated as fixed-point int32 (kFixedScale per metre) via
// InterlockedAdd: HLSL has no float atomics, and fixed-point addition is
// order-independent, which keeps the GPU result fully deterministic.
//
// Note: the CPU backend spreads carve / oversaturation deposits over a radial
// brush (Erosion Radius). A radial brush per step is far too many atomics on
// the GPU, so this backend uses a bilinear 4-cell splat and relies on the
// per-iteration soft-saturation cap (deltaCap) to keep busy channel cells from
// spiking — the same smoothing mechanism the Fluvial compute backend uses.
//
// Pipeline per pyramid level (driven from DropletErosionCompute.cpp):
//
//   CSClearLevel  — zero Flow / Deposit at level start.
//   per iteration:
//     CSClearIter — zero the per-iteration DeltaH accumulator.
//     CSTrace     — one thread per droplet: spawn from the stateless PRNG,
//                   roll downhill with inertia, splat carve / deposit.
//     CSApply     — per cell: convert the fixed-point sum to metres and apply
//                   with the soft saturation cap (deltaCap).
//   between levels:
//     CSCopyToSrc — copy Heights into SrcHeights at the previous resolution.
//     CSUpsample  — bilinear-resample SrcHeights (srcN) into Heights (n).
//
// Buffer layout (sized for the target resolution, levels use the prefix):
//   u0 = Heights    (float, metres)
//   u1 = SrcHeights (float, upsample source)
//   u2 = DeltaH     (int, fixed-point per-iteration height delta sum)
//   u3 = Flow       (int, fixed-point water visitation -> Flows output)
//   u4 = Deposit    (int, fixed-point cumulative deposits -> Deposits output)

cbuffer DropletErosionConstants : register(b0)
{
    uint  n;             // current level resolution
    uint  srcN;          // upsample source resolution
    uint  particleCount; // droplets this iteration
    uint  steps;         // max walk length in cells (Travel Distance / cellSize)

    int   seed;
    int   levelSeed;
    int   iter;
    float cellSize;      // metres per cell at this level

    float inertia;
    float capacityFactor;
    float minSlope;
    float erodeRate;

    float depositRate;
    float evapStepFactor; // (1 - evaporationPerMetre)^cellSize
    float gravity;
    float deltaCap;       // per-iteration per-cell apply cap (metres)

    float edgeFadeCells;  // border taper width in cells
    float pad0;
    float pad1;
    float pad2;
};

RWStructuredBuffer<float> Heights    : register(u0);
RWStructuredBuffer<float> SrcHeights : register(u1);
RWStructuredBuffer<int>   DeltaH     : register(u2);
RWStructuredBuffer<int>   Flow       : register(u3);
RWStructuredBuffer<int>   Deposit    : register(u4);

static const float kFixedScale = 4096.0f; // fixed-point counts per metre

// --- PRNG (matches fluvial_erosion_compute.hlsl) ----------------------------

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

// --- Bilinear sampling / splatting ------------------------------------------

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
    Flow[i] = 0;
    Deposit[i] = 0;
}

[numthreads(256, 1, 1)]
void CSClearIter(uint3 dt : SV_DispatchThreadID)
{
    const uint i = dt.x;
    if (i >= n * n) return;
    DeltaH[i] = 0;
}

// --- Droplet trace (port of RunDropletLevel's inner loop) --------------------

[numthreads(64, 1, 1)]
void CSTrace(uint3 dt : SV_DispatchThreadID)
{
    const uint p = dt.x;
    if (p >= particleCount) return;

    uint rng = SeedFor(seed, levelSeed, iter, int(p));
    const float spawnRange = float(int(n) - 3); // start positions in [1, n-2]
    float px = 1.0f + Hash01(rng) * spawnRange;
    float pz = 1.0f + Hash01(rng) * spawnRange;
    float dirX = 0.0f;
    float dirZ = 0.0f;
    float speed = 1.0f;
    float water = 1.0f;
    float sediment = 0.0f;

    [loop]
    for (uint stepIndex = 0; stepIndex < steps; ++stepIndex) // `step` is an HLSL intrinsic
    {
        float h, gx, gz;
        SampleHeightGradient(Heights, px, pz, h, gx, gz);

        dirX = dirX * inertia - gx * (1.0f - inertia);
        dirZ = dirZ * inertia - gz * (1.0f - inertia);
        const float dirLen = sqrt(dirX * dirX + dirZ * dirZ);
        if (dirLen < 1e-6f) { break; }
        dirX /= dirLen;
        dirZ /= dirLen;

        const float npx = px + dirX;
        const float npz = pz + dirZ;
        if (npx < 1.0f || npx > float(int(n) - 2) || npz < 1.0f || npz > float(int(n) - 2))
        {
            break;
        }

        float nh, ngx, ngz;
        SampleHeightGradient(Heights, npx, npz, nh, ngx, ngz);
        const float dH = nh - h;

        int origFlow;
        InterlockedAdd(Flow[Index1D(int(px), int(pz))], ToFixed(water), origFlow);

        // Fade carving/deposition out near the map border (matches the CPU
        // kernel): every off-map droplet path terminates at the edge.
        const float edgeDist = min(min(px, pz), min(float(int(n) - 1) - px, float(int(n) - 1) - pz));
        const float edgeFade = saturate((edgeDist - 1.0f) / edgeFadeCells);

        // Capacity scales with the true slope (rise/run), resolution-invariant.
        const float slope = -dH / cellSize;
        const float capacity = max(slope, minSlope) * speed * water * capacityFactor;

        if (dH > 0.0f)
        {
            // Moved uphill: fill the pit so the droplet can continue.
            const float deposit = min(dH, sediment);
            sediment -= deposit;
            SplatFixed(DeltaH, px, pz, deposit);
            SplatFixed(Deposit, px, pz, deposit);
        }
        else if (sediment > capacity)
        {
            const float deposit = (sediment - capacity) * depositRate * edgeFade;
            sediment -= deposit;
            SplatFixed(DeltaH, px, pz, deposit);
            SplatFixed(Deposit, px, pz, deposit);
        }
        else
        {
            const float erode = min((capacity - sediment) * erodeRate, -dH) * edgeFade;
            SplatFixed(DeltaH, px, pz, -erode);
            sediment += erode;
        }

        speed = sqrt(max(0.0f, speed * speed + (-dH) * gravity));
        water *= evapStepFactor;
        if (water < 1e-4f) { break; }

        px = npx;
        pz = npz;
    }
}

// --- Apply (soft-saturated sum, matches the Fluvial apply pass) ---------------

[numthreads(256, 1, 1)]
void CSApply(uint3 dt : SV_DispatchThreadID)
{
    const uint i = dt.x;
    if (i >= n * n) return;

    const float sum = float(DeltaH[i]) / kFixedScale;
    if (sum != 0.0f)
    {
        // Cells crossed by many droplets move further than cells crossed by one
        // (the feedback that grows dendritic networks), but never by more than
        // ~deltaCap in one iteration, so overlapping droplets cannot spike.
        Heights[i] += sum / (1.0f + abs(sum) / deltaCap);
    }
}

// --- Level-to-level upsample (identical to fluvial_erosion_compute.hlsl) ------

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
