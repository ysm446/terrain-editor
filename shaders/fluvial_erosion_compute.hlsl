cbuffer FluvialGridConstants : register(b0)
{
    uint resolution;
    uint cellCount;
    uint iteration;
    float terrainSizeMeters;
    float erosionStrength;
    float sedimentCapacity;
    float depositionRate;
    float channeling;
    float cellSizeMeters;
    float wearSlope;
    float maxSlope;
    float strengthScale;
};

RWStructuredBuffer<float> HeightIn : register(u0);
RWStructuredBuffer<float> HeightOut : register(u1);
RWStructuredBuffer<float> MaskOut : register(u2);
RWStructuredBuffer<float> FlowIn : register(u3);
RWStructuredBuffer<float> FlowOut : register(u4);

uint IndexAt(int x, int z)
{
    return (uint)z * resolution + (uint)x;
}

uint BestReceiverIndex(int x, int z)
{
    const uint index = IndexAt(x, z);
    const float center = HeightIn[index];
    float bestDrop = 0.0f;
    uint bestIndex = index;

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
            const int nx = clamp(x + dx, 0, (int)resolution - 1);
            const int nz = clamp(z + dz, 0, (int)resolution - 1);
            if (nx == x && nz == z)
            {
                continue;
            }
            const uint neighborIndex = IndexAt(nx, nz);
            const float distance = (dx != 0 && dz != 0) ? 1.41421356f : 1.0f;
            const float drop = (center - HeightIn[neighborIndex]) / distance;
            if (drop > bestDrop)
            {
                bestDrop = drop;
                bestIndex = neighborIndex;
            }
        }
    }

    return bestIndex;
}

[numthreads(8, 8, 1)]
void CSFlowAccumulation(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint x = dispatchThreadId.x;
    const uint z = dispatchThreadId.y;
    if (x >= resolution || z >= resolution)
    {
        return;
    }

    const uint index = z * resolution + x;
    float accumulated = 1.0f;
    if (x > 0 && z > 0 && x + 1 < resolution && z + 1 < resolution)
    {
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
                const int nx = (int)x + dx;
                const int nz = (int)z + dz;
                const uint neighborIndex = IndexAt(nx, nz);
                if (BestReceiverIndex(nx, nz) == index)
                {
                    accumulated += FlowIn[neighborIndex];
                }
            }
        }
    }
    FlowOut[index] = accumulated;
}

[numthreads(8, 8, 1)]
void CSGridErosion(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint x = dispatchThreadId.x;
    const uint z = dispatchThreadId.y;
    if (x >= resolution || z >= resolution)
    {
        return;
    }

    const uint index = z * resolution + x;
    if (x == 0 || z == 0 || x + 1 >= resolution || z + 1 >= resolution)
    {
        HeightOut[index] = HeightIn[index];
        return;
    }

    const float center = HeightIn[index];
    float bestDrop = 0.0f;
    float bestDistance = 1.0f;

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
            const uint nx = x + dx;
            const uint nz = z + dz;
            const uint neighborIndex = nz * resolution + nx;
            const float distance = (dx != 0 && dz != 0) ? 1.41421356f : 1.0f;
            const float drop = (center - HeightIn[neighborIndex]) / distance;
            if (drop > bestDrop)
            {
                bestDrop = drop;
                bestDistance = distance;
            }
        }
    }

    const float distanceMeters = max(cellSizeMeters * bestDistance, 0.0001f);
    const float slope = max(bestDrop, 0.0f) / distanceMeters;
    const float slopeAllowed = slope <= maxSlope ? 1.0f : 0.0f;
    const float flowWeight = saturate(log(1.0f + max(FlowIn[index], 1.0f)) / log(1.0f + max((float)cellCount * 0.15f, 2.0f)));
    const float wearGate = saturate((slope + 0.10f) / max(wearSlope + 0.10f, 0.0001f));
    const float capacity = (bestDrop * 0.16f + slope * cellSizeMeters * 0.08f) * (0.25f + flowWeight * 2.4f) * sedimentCapacity;
    const float amount = min(bestDrop * 0.72f, capacity * erosionStrength * strengthScale * wearGate * (0.65f + channeling)) * slopeAllowed;
    HeightOut[index] = center - amount;
    MaskOut[index] = max(MaskOut[index], amount + MaskOut[index] * 0.995f);
}
