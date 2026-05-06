// Procedural sky pass.
//
// Drawn as a fullscreen triangle behind the terrain mesh. Each pixel
// reconstructs its world-space view ray from the camera basis (matching the
// projection in shaders/mesh_preview.hlsl) and shades:
//   * Vertical gradient from horizonColor (ray.y == 0) to zenithColor (ray.y == 1).
//     `horizonSoftness` controls the exponent — higher values keep the
//     horizon color visible further up the dome.
//   * Sun disc + glow oriented along sunDirection (already normalized).
//
// Depth state: depth test/write disabled. The fullscreen triangle writes z=1
// in clip space, but mesh draws after with depth test LESS so terrain wins.

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
    float4 zenithColor;
    float4 horizonColor;
    float3 sunColor;
    float  sunSize;          // cos(angularRadius) — closer to 1.0 = smaller disc
    float  horizonSoftness;  // pow exponent for the gradient (>=1 = softer)
    float  sunGlowStrength;
    float  pad0;
    float  pad1;
};

struct VsOut
{
    float4 pos : SV_Position;
    float2 ndc : TEXCOORD0;
};

VsOut SkyVS(uint vid : SV_VertexID)
{
    // Fullscreen triangle covering NDC (-1,-1) to (3,3).
    VsOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);   // (0,0), (2,0), (0,2)
    o.pos = float4(p * 2.0 - 1.0, 1.0, 1.0);      // z=1 (far plane)
    o.ndc = p * 2.0 - 1.0;
    return o;
}

float4 SkyPS(VsOut input) : SV_Target
{
    // Invert the mesh shader projection: ndc.x = cx*projScaleX/d + panNdcX.
    // ray = forward + right * (cx/d) + up * (cy/d) is what the mesh shader
    // implicitly walks down for each vertex.
    float screenX = (input.ndc.x - panNdcX) / projScaleX;
    float screenY = (input.ndc.y - panNdcY) / projScaleY;
    float3 ray = normalize(cameraForward.xyz + cameraRight.xyz * screenX + cameraUp.xyz * screenY);

    float t = saturate(ray.y);
    float gradient = pow(t, max(horizonSoftness, 0.001));
    float3 sky = lerp(horizonColor.xyz, zenithColor.xyz, gradient);

    float cosTheta = saturate(dot(ray, sunDirection.xyz));
    float disc = smoothstep(sunSize - 0.0008, sunSize + 0.001, cosTheta);
    sky = lerp(sky, sunColor.xyz, disc);

    float glow = pow(saturate(cosTheta), 256.0);
    sky += sunColor.xyz * glow * sunGlowStrength;

    return float4(sky, 1.0);
}
