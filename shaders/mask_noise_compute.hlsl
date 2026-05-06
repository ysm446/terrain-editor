// Mask Noise compute shader.
//
// Direct port of the CPU mask_noise namespace in src/node_graph.cpp
// (Hash3 / Gradient2 / Fade / Perlin2D / Fbm2D). The output buffer is laid out
// as row-major float[resolution * resolution] in [0, 1].
//
// Octaves are accumulated in the same order as the CPU loop so that
// floating-point summation order matches and the GPU path produces visually
// identical results to the CPU path.

cbuffer MaskNoiseConstants : register(b0)
{
    uint  resolution;
    uint  octaves;
    int   seed;
    float frequency;
    float lacunarity;
    float persistence;
};

RWStructuredBuffer<float> Output : register(u0);

uint Hash3(int x, int y, int s)
{
    uint h = (uint)x * 0x27d4eb2du;
    h ^= (uint)y * 0x165667b1u;
    h ^= (uint)s * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

float2 Gradient2(int x, int y, int s)
{
    const float twoPi = 6.28318530717958647692f;
    uint h = Hash3(x, y, s);
    float angle = ((float)(h & 0xFFFFu) / 65535.0f) * twoPi;
    return float2(cos(angle), sin(angle));
}

float Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float Perlin2D(float x, float y, int s)
{
    float fx = floor(x);
    float fy = floor(y);
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float dx = x - fx;
    float dy = y - fy;

    float2 g00 = Gradient2(x0, y0, s);
    float2 g10 = Gradient2(x1, y0, s);
    float2 g01 = Gradient2(x0, y1, s);
    float2 g11 = Gradient2(x1, y1, s);

    float v00 = g00.x * dx         + g00.y * dy;
    float v10 = g10.x * (dx - 1.0f) + g10.y * dy;
    float v01 = g01.x * dx         + g01.y * (dy - 1.0f);
    float v11 = g11.x * (dx - 1.0f) + g11.y * (dy - 1.0f);

    float u = Fade(dx);
    float v = Fade(dy);
    return lerp(lerp(v00, v10, u), lerp(v01, v11, u), v);
}

float Fbm2D(float x, float y, int oct, float lac, float per, int s)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float maxAmp = 0.0f;
    float freq = 1.0f;
    [loop]
    for (int i = 0; i < oct; ++i)
    {
        total += Perlin2D(x * freq, y * freq, s + i * 1013) * amplitude;
        maxAmp += amplitude;
        amplitude *= per;
        freq *= lac;
    }
    return maxAmp > 0.0f ? total / maxAmp : 0.0f;
}

[numthreads(8, 8, 1)]
void CSGenerate(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint x = dispatchThreadID.x;
    uint z = dispatchThreadID.y;
    if (x >= resolution || z >= resolution)
    {
        return;
    }

    float invDenom = (resolution > 1u) ? 1.0f / (float)(resolution - 1u) : 0.0f;
    float u = (float)x * invDenom;
    float v = (float)z * invDenom;

    float n = Fbm2D(u * frequency, v * frequency, (int)octaves, lacunarity, persistence, seed);
    float result = clamp(n * 0.5f + 0.5f, 0.0f, 1.0f);
    Output[z * resolution + x] = result;
}
