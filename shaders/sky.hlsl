// Atmospheric sky pass.
//
// Drawn as a fullscreen triangle behind the terrain mesh. Each pixel
// reconstructs its world-space view ray from the camera basis (matching
// the projection in shaders/mesh_preview.hlsl) and shades using the
// shared Nishita single-scatter atmospheric model in atmosphere.hlsli.
//
// Sun colour is derived analytically from atmospheric transmittance along
// the sun direction so the sun naturally warms / reddens at low elevation
// and dims when below the horizon. The same C++ port of the model feeds
// the terrain ambient and cloud lighting via b1 cbuffer / cloud constants
// so the whole scene stays internally consistent.

#include "atmosphere.hlsli"

cbuffer SkyConstants : register(b0)
{
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float  projScaleX;
    float  projScaleY;
    float  panNdcX;
    float  panNdcY;
    float4 sunDirection;
    float  atmosphereDensity;
    float  mieStrength;
    float  mieEccentricity;
    float  sunSize;          // cos(angularRadius) — closer to 1.0 = smaller disc
    float  sunGlowStrength;
    float  pad0;
    float  pad1;
    float  pad2;
    float4 groundAlbedo;     // .a unused
};

Texture2D<float4> MultiScatterLut : register(t0);
SamplerState MultiScatterSampler : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

VsOut SkyVS(uint vid : SV_VertexID)
{
    VsOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p * 2.0 - 1.0, 1.0, 1.0);
    o.ndc = p * 2.0 - 1.0;
    return o;
}

float4 SkyPS(VsOut input) : SV_Target
{
    float screenX = (input.ndc.x - panNdcX) / projScaleX;
    float screenY = (input.ndc.y - panNdcY) / projScaleY;
    float3 ray = normalize(cameraForward.xyz + cameraRight.xyz * screenX + cameraUp.xyz * screenY);

    // Below the horizon: blend the (nearly-black) atmospheric ground term
    // with the user-tunable groundAlbedo tinted by the upward-facing sky
    // colour. Looks far better than the model's clipped-near-zero output
    // on rays that immediately hit the planet.
    float3 sky;
    if (ray.y >= 0.0)
    {
        sky = AtmComputeScattering(ray, sunDirection.xyz, atmosphereDensity, mieStrength, mieEccentricity,
                                   MultiScatterLut, MultiScatterSampler, true);
    }
    else
    {
        // Use a horizon-grazing sample as the atmospheric reference (it
        // carries the long-path haze colour) and fade to groundAlbedo-tinted
        // ambient over a narrow band right under the geometric horizon. A
        // wide fade looked unnatural — clouds/terrain occupy the lower
        // hemisphere in normal viewing, so the visible band is small.
        float3 upRay = float3(ray.x, max(-ray.y, 0.02), ray.z);
        upRay = normalize(upRay);
        float3 ambient = AtmComputeScattering(upRay, sunDirection.xyz, atmosphereDensity, mieStrength, mieEccentricity,
                                              MultiScatterLut, MultiScatterSampler, true);
        float fade = smoothstep(0.0, 0.08, -ray.y);
        sky = lerp(ambient, ambient * groundAlbedo.rgb, fade);
    }

    // Subtle horizon desaturation. The multi-scatter LUT already handles
    // most of the "warm noon horizon" artefact; this is just a small nudge
    // toward the band's own luminance with a slight cool lift, no
    // fabricated brightness floor — the atmosphere model owns radiance.
    // Gated to mid-high sun so sunset / dusk colours stay saturated.
    float horizonBand = 1.0 - smoothstep(0.0, 0.20, abs(ray.y));
    float dayHaze = smoothstep(0.05, 0.30, sunDirection.y);
    float skyLum = dot(sky, float3(0.2126, 0.7152, 0.0722));
    float3 cool = float3(skyLum * 0.96, skyLum, skyLum * 1.04);
    float hazeAmount = horizonBand * dayHaze * 0.35;
    sky = lerp(sky, cool, hazeAmount);

    // Sun disc + glow. The sun's colour as seen from the ground is just
    // the white solar spectrum × atmospheric transmittance along sunDir,
    // which already encodes the sunset reddening.
    float3 sunBase = AtmComputeSunTransmittance(sunDirection.xyz, atmosphereDensity, mieStrength);
    float cosTheta = saturate(dot(ray, sunDirection.xyz));
    float disc = smoothstep(sunSize - 0.0008, sunSize + 0.001, cosTheta);
    sky = lerp(sky, sunBase * 6.0, disc);

    // Soft additive glow merged with the Mie forward peak. A sharper
    // exponent (e.g. 256) carved a visible inner halo edge inside the
    // already-bright Mie cone; pow(cos, 96) gives a smoother bloom that
    // sits naturally on top of the HG falloff. Scale lowered so the
    // total brightness near the disc roughly matches the previous look.
    float glow = pow(saturate(cosTheta), 96.0);
    sky += sunBase * glow * sunGlowStrength * 0.4;

    return float4(sky, 1.0);
}
