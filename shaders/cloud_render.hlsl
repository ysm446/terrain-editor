// Volumetric cloud rendering pass.
//
// Drawn as a fullscreen triangle between the sky pass and the terrain mesh
// pass. Each pixel reconstructs its world-space view ray (matching the camera
// projection in shaders/mesh_preview.hlsl), intersects with the cloud band
// [altMin, altMax] and ray-marches through that slab sampling the 3D density
// volume produced by cloud_density.hlsl.
//
// The result is alpha-blended (SRC_ALPHA, INV_SRC_ALPHA) over the sky color
// already in the render target. The terrain mesh draws afterwards with depth
// test, so any pixel covered by a closer mountain will overwrite the cloud
// fragment naturally — the "mountain breaks through clouds" look.
//
// We deliberately keep the integration cheap (no light-march toward the sun).
// The shading model is base color × Beer-Lambert transmittance with a gentle
// vertical brightness ramp (top of cloud = brighter, bottom = darker) to
// suggest sun illumination from above.

cbuffer CloudConstants : register(b0)
{
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float  projScaleX;
    float  projScaleY;
    float  panNdcX;
    float  panNdcY;
    float4 sunDirection;
    float4 cloudColor;            // .a unused
    float  altitudeMin;
    float  altitudeMax;
    float  horizontalScale;
    float  coverage;
    float  densityMultiplier;
    float  absorption;
    float  windOffsetX;
    float  windOffsetZ;
    int    qualitySamples;
    float  pad0;
    float  pad1;
    float  pad2;
};

Texture3D<float> CloudVolume : register(t0);
SamplerState LinearSampler : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

VsOut CloudVS(uint vid : SV_VertexID)
{
    VsOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p * 2.0 - 1.0, 1.0, 1.0);
    o.ndc = p * 2.0 - 1.0;
    return o;
}

float SampleCloudDensity(float3 worldPos)
{
    float bandThickness = max(altitudeMax - altitudeMin, 1.0);
    float yNorm = saturate((worldPos.y - altitudeMin) / bandThickness);

    // Vertical profile — a soft cumulus shape: peaks ~30% up the band and
    // fades to zero at the top and bottom. Multiplying with the sampled noise
    // suppresses cloud at the edges and concentrates it in the middle.
    float vp = saturate(yNorm * 5.0) * saturate(1.0 - yNorm) * 1.6;

    float3 uvw;
    uvw.x = frac((worldPos.x + windOffsetX) / horizontalScale);
    uvw.z = frac((worldPos.z + windOffsetZ) / horizontalScale);
    uvw.y = yNorm;
    float baseDensity = CloudVolume.SampleLevel(LinearSampler, uvw, 0);

    float density = baseDensity * vp;
    density = max(0.0, density - (1.0 - coverage));
    return density * densityMultiplier;
}

float4 CloudPS(VsOut input) : SV_Target
{
    float screenX = (input.ndc.x - panNdcX) / projScaleX;
    float screenY = (input.ndc.y - panNdcY) / projScaleY;
    float3 ray = normalize(cameraForward.xyz + cameraRight.xyz * screenX + cameraUp.xyz * screenY);

    // Find ray entry/exit with the slab altitudeMin <= y <= altitudeMax.
    if (abs(ray.y) < 1e-4)
    {
        return float4(0, 0, 0, 0);
    }
    float t1 = (altitudeMin - cameraPosition.y) / ray.y;
    float t2 = (altitudeMax - cameraPosition.y) / ray.y;
    float tEnter = min(t1, t2);
    float tExit  = max(t1, t2);
    tEnter = max(tEnter, 0.0);
    if (tExit <= tEnter) return float4(0, 0, 0, 0);
    // Cap how far we march. Beyond ~50km the noise tiling pattern gets
    // obvious, and there's nothing of interest at that distance for an editor
    // preview anyway.
    tExit = min(tExit, 50000.0);

    int numSteps = qualitySamples;
    float stepLen = (tExit - tEnter) / numSteps;

    float transmittance = 1.0;
    float3 accumulated = float3(0, 0, 0);

    // Vertical brightness ramp suggests top-lit clouds without a real light
    // march. Multiplied into the lit color of each step.
    float sunLit = saturate(sunDirection.y) * 0.7 + 0.3;

    [loop]
    for (int i = 0; i < numSteps; ++i)
    {
        float t = tEnter + (i + 0.5) * stepLen;
        float3 p = cameraPosition.xyz + ray * t;
        float density = SampleCloudDensity(p);
        if (density > 0.0)
        {
            float yNorm = saturate((p.y - altitudeMin) / max(altitudeMax - altitudeMin, 1.0));
            float topLight = lerp(0.55, 1.0, yNorm);

            float3 lit = cloudColor.rgb * topLight * sunLit;
            float dT = exp(-density * absorption * stepLen);
            float dA = (1.0 - dT) * transmittance;
            accumulated += lit * dA;
            transmittance *= dT;
            if (transmittance < 0.01) break;
        }
    }

    float alpha = saturate(1.0 - transmittance);
    return float4(accumulated, alpha);
}
