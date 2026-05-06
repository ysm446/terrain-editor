// 3D cloud density volume generator.
//
// Compute shader that fills a 128^3 R8_UNORM volume with fBM noise. The CPU
// dispatches this once per (seed, scale) change; ray-march then trilinear-
// samples the volume per step. The hash + gradient functions mirror the 2D
// mask-noise shader extended to three axes.

cbuffer CloudVolumeConstants : register(b0)
{
    uint  resolution;   // = 128
    int   seed;
    float pad0;
    float pad1;
};

RWTexture3D<float> Output : register(u0);

uint Hash4(int x, int y, int z, int s)
{
    uint h = (uint)x * 0x27d4eb2du;
    h ^= (uint)y * 0x165667b1u;
    h ^= (uint)z * 0x9e3779b9u;
    h ^= (uint)s * 0xc2b2ae35u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Random unit-sphere gradient. Two-angle parameterization keeps it cheap and
// avoids the rejection loop / atan trig stacking up across millions of voxels.
float3 Gradient3(int x, int y, int z, int s)
{
    uint h = Hash4(x, y, z, s);
    const float twoPi = 6.28318530717958647692f;
    float phi = ((float)(h & 0xFFFFu) / 65535.0f) * twoPi;
    float cosTheta = ((float)((h >> 16) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

float Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float Perlin3D(float x, float y, float z, int s)
{
    float fx = floor(x);
    float fy = floor(y);
    float fz = floor(z);
    int x0 = (int)fx, y0 = (int)fy, z0 = (int)fz;
    int x1 = x0 + 1, y1 = y0 + 1, z1 = z0 + 1;
    float dx = x - fx, dy = y - fy, dz = z - fz;

    float3 g000 = Gradient3(x0, y0, z0, s);
    float3 g100 = Gradient3(x1, y0, z0, s);
    float3 g010 = Gradient3(x0, y1, z0, s);
    float3 g110 = Gradient3(x1, y1, z0, s);
    float3 g001 = Gradient3(x0, y0, z1, s);
    float3 g101 = Gradient3(x1, y0, z1, s);
    float3 g011 = Gradient3(x0, y1, z1, s);
    float3 g111 = Gradient3(x1, y1, z1, s);

    float v000 = dot(g000, float3(dx,         dy,         dz));
    float v100 = dot(g100, float3(dx - 1.0f,  dy,         dz));
    float v010 = dot(g010, float3(dx,         dy - 1.0f,  dz));
    float v110 = dot(g110, float3(dx - 1.0f,  dy - 1.0f,  dz));
    float v001 = dot(g001, float3(dx,         dy,         dz - 1.0f));
    float v101 = dot(g101, float3(dx - 1.0f,  dy,         dz - 1.0f));
    float v011 = dot(g011, float3(dx,         dy - 1.0f,  dz - 1.0f));
    float v111 = dot(g111, float3(dx - 1.0f,  dy - 1.0f,  dz - 1.0f));

    float u = Fade(dx), v = Fade(dy), w = Fade(dz);
    float lx0 = lerp(lerp(v000, v100, u), lerp(v010, v110, u), v);
    float lx1 = lerp(lerp(v001, v101, u), lerp(v011, v111, u), v);
    return lerp(lx0, lx1, w);
}

float Fbm3D(float3 p, int s)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float maxAmp = 0.0f;
    float freq = 1.0f;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        total += Perlin3D(p.x * freq, p.y * freq, p.z * freq, s + i * 1013) * amplitude;
        maxAmp += amplitude;
        amplitude *= 0.5f;
        freq *= 2.0f;
    }
    return maxAmp > 0.0f ? total / maxAmp : 0.0f;
}

[numthreads(4, 4, 4)]
void CSGenerate(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid >= resolution))
    {
        return;
    }
    float3 uvw = ((float3)dtid + 0.5) / (float)resolution;

    // Two scales of fBM combined: a wide base shape + finer detail.
    float baseShape = Fbm3D(uvw * 4.0, seed);
    float detail    = Fbm3D(uvw * 12.0, seed + 1);
    float n = baseShape * 0.7 + detail * 0.3;
    n = saturate(n * 0.5 + 0.5);

    Output[dtid] = n;
}
