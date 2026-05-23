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
    float  nearPlane;
    float  farPlane;
    float  pad0;
    float  fieldCenterX;
    float  fieldCenterZ;
    float  fieldRadius;
    float  fieldFalloff;
    float4 atmosphereSunColor;    // sun colour after atmospheric transmittance; white when atmospheric mode is off
    float4 atmosphereSkyColor;    // mid-zenith sky colour for cloud bottom shading
    int    lightSamples;          // 0 disables self-shadowing, falls back to vertical ramp only
    float  lightStepMeters;       // step length toward the sun (m)
    float  phaseEccentricity;     // Henyey-Greenstein g; 0 isotropic, 0.4 gentle silver lining
    float  shadowAmbientStrength; // sky-tinted fill added to self-shadowed cloud regions
};

Texture3D<float> CloudVolume : register(t0);
Texture2D<float> DepthBuffer : register(t1);
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

float ComputeFieldFade(float worldX, float worldZ)
{
    // Distance from the field center, with a soft circular falloff. Inside
    // (radius - falloff) returns 1; between (radius - falloff) and radius
    // it ramps to 0; outside is 0. Keeps clouds confined to a finite disc
    // around the terrain instead of tiling out to the horizon.
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
    return density * densityMultiplier * fieldFade;
}

float4 CloudPS(VsOut input) : SV_Target
{
    float screenX = (input.ndc.x - panNdcX) / projScaleX;
    float screenY = (input.ndc.y - panNdcY) / projScaleY;
    float3 ray = normalize(cameraForward.xyz + cameraRight.xyz * screenX + cameraUp.xyz * screenY);

    // Read terrain depth to limit the ray march. Mesh shader writes
    // ndc.z = (d - nearPlane) / (farPlane - nearPlane) where d is
    // dot(view, cameraForward). Reverse it back to a world-space ray
    // distance and clamp tExit so cloud doesn't bleed past terrain.
    float ndcZ = DepthBuffer.Load(int3(input.pos.xy, 0));
    float forwardDist = ndcZ * (farPlane - nearPlane) + nearPlane;
    float rayForward = max(dot(ray, cameraForward.xyz), 1e-4);
    float tTerrain = (ndcZ < 1.0) ? (forwardDist / rayForward) : 1e30;

    // The cloud is bounded by both a vertical slab [altitudeMin, altitudeMax]
    // and a horizontal disc of radius fieldRadius centered on (fieldCenterX,
    // fieldCenterZ). Find the t-range where the ray is inside both. Using
    // the disc bound (instead of a fixed 50km cap) eliminates the horizontal
    // line that would otherwise appear on grazing-angle rays where the
    // simple "march to 50km" cutoff toggled cloud on/off abruptly.

    // Slab (vertical) range.
    float tSlabEnter;
    float tSlabExit;
    if (abs(ray.y) > 1e-5)
    {
        float t1 = (altitudeMin - cameraPosition.y) / ray.y;
        float t2 = (altitudeMax - cameraPosition.y) / ray.y;
        tSlabEnter = min(t1, t2);
        tSlabExit  = max(t1, t2);
    }
    else
    {
        // Purely horizontal ray: only contributes if the camera is already
        // inside the cloud band.
        if (cameraPosition.y < altitudeMin || cameraPosition.y > altitudeMax)
        {
            return float4(0, 0, 0, 0);
        }
        tSlabEnter = -1e30;
        tSlabExit  =  1e30;
    }

    // Disc (horizontal) range — ray vs vertical cylinder.
    float tDiscEnter;
    float tDiscExit;
    float2 rayXZ = float2(ray.x, ray.z);
    float2 originXZ = float2(cameraPosition.x - fieldCenterX, cameraPosition.z - fieldCenterZ);
    float aXZ = dot(rayXZ, rayXZ);
    float cXZ = dot(originXZ, originXZ) - fieldRadius * fieldRadius;
    if (aXZ > 1e-6)
    {
        float bXZ = 2.0 * dot(originXZ, rayXZ);
        float discXZ = bXZ * bXZ - 4.0 * aXZ * cXZ;
        if (discXZ < 0.0)
        {
            return float4(0, 0, 0, 0);
        }
        float sq = sqrt(discXZ);
        tDiscEnter = (-bXZ - sq) / (2.0 * aXZ);
        tDiscExit  = (-bXZ + sq) / (2.0 * aXZ);
    }
    else
    {
        // Purely vertical ray: only contributes if the camera is already
        // inside the field disc.
        if (cXZ > 0.0)
        {
            return float4(0, 0, 0, 0);
        }
        tDiscEnter = -1e30;
        tDiscExit  =  1e30;
    }

    float tEnter = max(max(tSlabEnter, tDiscEnter), 0.0);
    float tExit  = min(tSlabExit, tDiscExit);
    tExit = min(tExit, tTerrain);
    if (tExit <= tEnter) return float4(0, 0, 0, 0);

    // Adaptive step length: base the *minimum* sample density on the
    // cloud-band thickness so vertical rays use roughly qualitySamples
    // steps, but ramp up step count for grazing rays whose slab traversal
    // is much longer (otherwise samples are hundreds of metres apart and
    // the noise tile shows up as horizontal bands). Hard cap at 4× quality
    // to bound pixel cost.
    float bandThickness = max(altitudeMax - altitudeMin, 1.0);
    float idealStep = bandThickness / max((float)qualitySamples, 1.0);
    int numSteps = (int)ceil((tExit - tEnter) / idealStep);
    numSteps = clamp(numSteps, qualitySamples, qualitySamples * 4);
    float stepLen = (tExit - tEnter) / (float)numSteps;

    // Per-pixel jitter to break sample-aligned banding into noise that's
    // averaged out by the eye. Cheap hashed jitter from screen-space
    // pixel coords.
    float jitter = frac(sin(dot(input.pos.xy, float2(12.9898, 78.233))) * 43758.5453);

    float transmittance = 1.0;
    float3 accumulated = float3(0, 0, 0);

    // HG phase factor (4π normalised so isotropic g=0 evaluates to 1.0).
    // Computed once per pixel — depends only on view-ray vs sun angle.
    const float kPi = 3.14159265;
    float cosTheta = dot(ray, sunDirection.xyz);
    float gg = phaseEccentricity * phaseEccentricity;
    float hgDenom = pow(max(1.0 + gg - 2.0 * phaseEccentricity * cosTheta, 1e-4), 1.5);
    float phase = ((1.0 - gg) / hgDenom);  // already 4π-normalised: forward peaks, backward dim

    [loop]
    for (int i = 0; i < numSteps; ++i)
    {
        float t = tEnter + (i + jitter) * stepLen;
        float3 p = cameraPosition.xyz + ray * t;
        float density = SampleCloudDensity(p);
        if (density > 0.0)
        {
            // Ambient term — bottom-of-cloud feel: hemispherical sky +
            // a small ground bounce, lightly desaturated to mimic the
            // multi-scatter wash inside a thick cloud.
            float3 skyTerm = atmosphereSkyColor.rgb * 1.5;
            float skyLum = dot(skyTerm, float3(0.299, 0.587, 0.114));
            skyTerm = lerp(skyTerm, float3(skyLum, skyLum, skyLum), 0.35);
            float3 sunBounce = atmosphereSunColor.rgb * 0.5;
            float3 ambientColor = cloudColor.rgb * (skyTerm + sunBounce);
            float3 sunlitColor = cloudColor.rgb * atmosphereSunColor.rgb;

            float3 lit;
            if (lightSamples > 0)
            {
                // Self-shadowing: march toward the sun and accumulate density,
                // then Beer-Lambert. A few cheap samples are enough — the
                // density texture has bilinear filtering and we don't need
                // sub-cell precision for a transmittance estimate.
                float lightDensity = 0.0;
                [loop]
                for (int j = 0; j < lightSamples; ++j)
                {
                    float3 lp = p + sunDirection.xyz * (lightStepMeters * ((float)j + 0.5));
                    lightDensity += SampleCloudDensity(lp);
                }
                float lightTransmittance = exp(-lightDensity * absorption * lightStepMeters * 2.0);
                // Direct sun = sunlit colour × (transmittance through cloud
                // toward the sun) × (HG phase). The phase brightens cells
                // near the sun direction (silver lining) and dims the
                // shadow-side without changing total cloud brightness.
                float3 directLight = sunlitColor * lightTransmittance * phase;
                float shadowWeight = saturate(1.0 - lightTransmittance);
                float ambientVisibility = lerp(1.0, max(lightTransmittance, 0.25), 0.55);
                float3 skyShadowLight = cloudColor.rgb * atmosphereSkyColor.rgb * shadowAmbientStrength * shadowWeight;
                lit = ambientColor * ambientVisibility + directLight + skyShadowLight;
            }
            else
            {
                // Fallback: original vertical ramp (no self-shadowing).
                float yNorm = saturate((p.y - altitudeMin) / max(altitudeMax - altitudeMin, 1.0));
                float3 skyShadowLight = cloudColor.rgb * atmosphereSkyColor.rgb * shadowAmbientStrength * (1.0 - yNorm);
                lit = lerp(ambientColor, sunlitColor, yNorm) + skyShadowLight;
            }

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
