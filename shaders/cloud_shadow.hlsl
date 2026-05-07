// Cloud shadow texture generator.
//
// Top-down 2D R8_UNORM transmittance map computed by ray-marching through the
// cloud volume from the cloud-base altitude toward the sun. The mesh shader
// samples this texture using projected ground-space (x, z) to multiply
// terrain lighting. Stored value: 1.0 = unshadowed, 0.0 = fully occluded.
//
// The 3D density evaluation mirrors cloud_render.hlsl:: SampleCloudDensity so
// the cloud the user sees in the sky and the shadow it casts are computed
// from the exact same parameters.

cbuffer CloudShadowConstants : register(b0)
{
    float boundsMinX;
    float boundsMinZ;
    float boundsSizeX;
    float boundsSizeZ;
    float altitudeMin;
    float altitudeMax;
    float horizontalScale;
    float coverage;
    float densityMultiplier;
    float absorption;
    float windOffsetX;
    float windOffsetZ;
    float4 sunDirection;       // .w unused
    uint  resolution;
    uint  numSamples;
    float pad0;
    float pad1;
    float fieldCenterX;
    float fieldCenterZ;
    float fieldRadius;
    float fieldFalloff;
};

Texture3D<float> CloudVolume : register(t0);
SamplerState LinearSampler : register(s0);
RWTexture2D<float> Output : register(u0);

float ComputeFieldFade(float worldX, float worldZ)
{
    float dx = worldX - fieldCenterX;
    float dz = worldZ - fieldCenterZ;
    float dist = sqrt(dx * dx + dz * dz);
    float inner = max(fieldRadius - fieldFalloff, 0.0);
    float falloff = max(fieldFalloff, 1.0);
    return saturate(1.0 - (dist - inner) / falloff);
}

float SampleCloudDensity(float3 worldPos)
{
    float fieldFade = ComputeFieldFade(worldPos.x, worldPos.z);
    if (fieldFade <= 0.0)
    {
        return 0.0;
    }

    float bandThickness = max(altitudeMax - altitudeMin, 1.0);
    float yNorm = saturate((worldPos.y - altitudeMin) / bandThickness);
    float vp = saturate(yNorm * 5.0) * saturate(1.0 - yNorm) * 1.6;

    float3 uvw;
    uvw.x = frac((worldPos.x + windOffsetX) / horizontalScale);
    uvw.z = frac((worldPos.z + windOffsetZ) / horizontalScale);
    uvw.y = yNorm;
    float baseDensity = CloudVolume.SampleLevel(LinearSampler, uvw, 0);

    float density = baseDensity * vp;
    density = max(0.0, density - (1.0 - coverage));
    return density * densityMultiplier * fieldFade;
}

[numthreads(8, 8, 1)]
void CSGenerate(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid.xy >= resolution))
    {
        return;
    }

    float u = ((float)dtid.x + 0.5) / (float)resolution;
    float v = ((float)dtid.y + 0.5) / (float)resolution;
    float3 startPos = float3(
        boundsMinX + u * boundsSizeX,
        altitudeMin,
        boundsMinZ + v * boundsSizeZ);

    if (sunDirection.y < 0.001)
    {
        // Sun at or below horizon: leave unshadowed and let terrain shadowing
        // handle it.
        Output[dtid.xy] = 1.0;
        return;
    }

    float tExit = (altitudeMax - startPos.y) / sunDirection.y;
    if (tExit <= 0.0)
    {
        Output[dtid.xy] = 1.0;
        return;
    }

    float stepLen = tExit / (float)numSamples;
    float transmittance = 1.0;
    [loop]
    for (uint i = 0; i < numSamples; ++i)
    {
        float t = ((float)i + 0.5) * stepLen;
        float3 p = startPos + sunDirection.xyz * t;
        float density = SampleCloudDensity(p);
        if (density > 0.0)
        {
            transmittance *= exp(-density * absorption * stepLen);
            if (transmittance < 0.01) break;
        }
    }

    Output[dtid.xy] = transmittance;
}
