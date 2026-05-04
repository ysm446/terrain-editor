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

float NormalizedFlowWeight(uint index)
{
    const float normalizedFlow = saturate(log(1.0f + max(FlowIn[index], 1.0f)) / log(1.0f + max((float)cellCount, 2.0f)));
    return pow(normalizedFlow, 1.35f);
}

void ComputeGridErosionAt(int x, int z, out uint receiverIndex, out float erodeAmount, out float depositAmount)
{
    const uint index = IndexAt(x, z);
    receiverIndex = index;
    erodeAmount = 0.0f;
    depositAmount = 0.0f;
    if (x <= 0 || z <= 0 || x + 1 >= (int)resolution || z + 1 >= (int)resolution)
    {
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
            const int nx = x + dx;
            const int nz = z + dz;
            const uint neighborIndex = IndexAt(nx, nz);
            const float distance = (dx != 0 && dz != 0) ? 1.41421356f : 1.0f;
            const float drop = (center - HeightIn[neighborIndex]) / distance;
            if (drop > bestDrop)
            {
                bestDrop = drop;
                bestDistance = distance;
                receiverIndex = neighborIndex;
            }
        }
    }

    const float distanceMeters = max(cellSizeMeters * bestDistance, 0.0001f);
    const float slope = max(bestDrop, 0.0f) / distanceMeters;
    if (slope > maxSlope)
    {
        return;
    }

    const float flowWeight = NormalizedFlowWeight(index);
    const float wearGate = saturate((slope + flowWeight * 0.10f) / max(wearSlope + 0.10f, 0.0001f));
    const float capacity = (bestDrop * 0.16f + slope * cellSizeMeters * 0.08f) * (0.25f + flowWeight * 2.4f) * sedimentCapacity;
    erodeAmount = min(bestDrop * 0.72f, capacity * erosionStrength * strengthScale * wearGate * (0.65f + channeling));
    depositAmount = erodeAmount * depositionRate * lerp(0.18f, 0.42f, flowWeight);
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

    uint ownReceiver = index;
    float ownErode = 0.0f;
    float ownDeposit = 0.0f;
    ComputeGridErosionAt((int)x, (int)z, ownReceiver, ownErode, ownDeposit);

    float incomingDeposit = 0.0f;
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
            uint neighborReceiver = IndexAt(nx, nz);
            float neighborErode = 0.0f;
            float neighborDeposit = 0.0f;
            ComputeGridErosionAt(nx, nz, neighborReceiver, neighborErode, neighborDeposit);
            if (neighborReceiver == index)
            {
                incomingDeposit += neighborDeposit;
            }
        }
    }

    HeightOut[index] = HeightIn[index] - ownErode + incomingDeposit;
    MaskOut[index] = max(MaskOut[index], ownErode + incomingDeposit * 0.5f + MaskOut[index] * 0.995f);
}
