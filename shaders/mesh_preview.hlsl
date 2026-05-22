cbuffer Constants : register(b0)
{
    float4 cameraPosition;
    float4 cameraRight;
    float4 cameraUp;
    float4 cameraForward;
    float projScaleX;
    float projScaleY;
    float panNdcX;
    float panNdcY;
    float nearPlane;
    float farPlane;
    float maskPreview;
    float lightingMode;
    float4 sunDirection;
    float4 albedoColor;
    float sunIntensity;
    float ambientStrength;
    float shadowStrength;
    float shadowMapResolution;
    float shadowBias;
    float shadowEnabled;
    float maskShadingMode;  // 0 = Grayscale, 1 = GrayOrange, 2 = GrayscaleHatched
    float colorTextureMode; // bit 0 = sample Colorize texture, bit 1 = sample mask texture in PS
    float4 lightRight;
    float4 lightUp;
    float4 lightForward;
    float4 lightCenter;
    float lightWorldRadius;
    float lightNearPlane;
    float lightFarPlane;
    float lightDepthMin;
};

cbuffer CloudShadowMeshConstants : register(b1)
{
    float cloudShadowEnabled;
    float cloudShadowStrength;
    float cloudShadowAltitudeMin;
    float aoEnabled;  // 0 = off, 1 = on
    float cloudShadowMinX;
    float cloudShadowMinZ;
    float cloudShadowSizeX;
    float cloudShadowSizeZ;
    float4 skyZenithColor;
    float4 skyHorizonColor;
    float4 skyGroundColor;
    float4 skySunColor;
    float4 sectionColor;
    float atmosphereDensity;
    float atmosphereMieStrength;
    float aoStrength;     // AO の暗化強度 (0–1)
    float atmospherePad1;
    float waterLevelParam;       // PSWater: 水面高さ (m)
    float waterWavesScale;       // PSWater: 主波長 (m)
    float waterRefractiveIndex;  // PSWater: 屈折率 → Schlick F0
    float waterRefractionStrength;
    float waterRefractionBlur;
    float waterCbPad0;
    float waterCbPad1;
    float waterCbPad2;
};

Texture2D shadowMap : register(t0);
Texture2D<float> cloudShadowMap : register(t1);
// Phase 2 GPU-displacement only: input heightfield + mask sampled per-vertex.
// Bound to the same root signature as the CPU mesh path; the displacement
// VS reads them, the standard VSMain ignores them.
Texture2D<float> displacementHeights : register(t2);
Texture2D<float> displacementMask : register(t3);
Texture2D<float4> colorTexture : register(t4);
Texture2D<float>  aoTexture    : register(t5);
Texture2D<float4> sceneColorTexture : register(t6);
SamplerState shadowSampler : register(s0);
SamplerState linearSampler : register(s1);

cbuffer DisplacementConstants : register(b2)
{
    // Mesh-side parameters: gridResolution = M (vertex count along one
    // edge), terrainSize = world width in metres, halfSize precomputed.
    // worldDX = terrainSize / (M - 1) is also pre-computed for normal
    // gradient scaling.
    float displacementGridResolution;
    float displacementTerrainSize;
    float displacementHalfSize;
    float displacementWorldDX;
    float tessellationMinFactor;
    float tessellationMaxFactor;
    float tessellationNearDistance;
    float tessellationFarDistance;
};

float3 LightSpace01(float3 worldPos)
{
    float3 view = worldPos - lightCenter.xyz;
    float halfX = max(lightWorldRadius, 1.0);
    float halfY = max(lightNearPlane, 1.0);
    float depthRange = max(lightFarPlane, 1.0);
    float depthMin = lightDepthMin;
    return float3(
        dot(view, lightRight.xyz) / (halfX * 2.0) + 0.5,
        dot(view, lightUp.xyz) / (halfY * 2.0) + 0.5,
        (dot(worldPos, lightForward.xyz) - depthMin) / depthRange);
}

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
    float mask : TEXCOORD0;
    float3 color : TEXCOORD2;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 worldNor : NORMAL;
    float3 worldPos : TEXCOORD0;
    float mask : TEXCOORD1;
    float3 vertexColor : TEXCOORD2;
};

VSOut VSMain(VSIn i)
{
    float3 view = i.pos - cameraPosition.xyz;
    float cx = dot(view, cameraRight.xyz);
    float cy = dot(view, cameraUp.xyz);
    float d = dot(view, cameraForward.xyz);

    VSOut o;
    o.pos = float4(
        cx * projScaleX + panNdcX * d,
        cy * projScaleY + panNdcY * d,
        (d - nearPlane) / (farPlane - nearPlane) * d,
        d);
    o.worldNor = i.nor;
    o.worldPos = i.pos;
    o.mask = i.mask;
    o.vertexColor = i.color;
    return o;
}

float4 VSShadow(VSIn i) : SV_POSITION
{
    float3 lightUv = LightSpace01(i.pos);
    return float4(lightUv.x * 2.0 - 1.0, lightUv.y * 2.0 - 1.0, saturate(lightUv.z), 1.0);
}

// GPU vertex displacement path. The mesh has only a static UV grid (no
// position / normal / mask attributes); we reconstruct (x, z) from the
// vertex id, sample the height texture for Y, compute the normal from a
// 4-tap height gradient, and sample the mask texture. Same VSOut as the
// CPU mesh path so PSSurface / PSEdge work unchanged.
float3 SampleDisplacedWorldPos(float u, float v)
{
    float h = displacementHeights.SampleLevel(linearSampler, float2(u, v), 0).r;
    return float3(
        lerp(-displacementHalfSize, displacementHalfSize, u),
        h,
        lerp(displacementHalfSize, -displacementHalfSize, v));
}

float3 SampleDisplacedNormal(float u, float v)
{
    float invM1 = 1.0 / (displacementGridResolution - 1.0);
    float uMinus = max(u - invM1, 0.0);
    float uPlus  = min(u + invM1, 1.0);
    float vMinus = max(v - invM1, 0.0);
    float vPlus  = min(v + invM1, 1.0);
    float hxm = displacementHeights.SampleLevel(linearSampler, float2(uMinus, v), 0).r;
    float hxp = displacementHeights.SampleLevel(linearSampler, float2(uPlus,  v), 0).r;
    float hzm = displacementHeights.SampleLevel(linearSampler, float2(u, vMinus), 0).r;
    float hzp = displacementHeights.SampleLevel(linearSampler, float2(u, vPlus),  0).r;
    // worldZ = lerp(halfSize, -halfSize, v) so dhdz = (hzm - hzp) / (2*dx)
    float dhdx = (hxp - hxm) / (2.0 * displacementWorldDX);
    float dhdz = (hzm - hzp) / (2.0 * displacementWorldDX);
    return normalize(float3(-dhdx, 1.0, -dhdz));
}

VSOut VSDisplacement(uint vid : SV_VertexID)
{
    uint M = (uint)displacementGridResolution;
    uint x = vid % M;
    uint z = vid / M;
    float u = (float)x / (displacementGridResolution - 1.0);
    float v = (float)z / (displacementGridResolution - 1.0);

    float3 worldPos = SampleDisplacedWorldPos(u, v);
    float3 worldNor = SampleDisplacedNormal(u, v);
    float maskVal = displacementMask.SampleLevel(linearSampler, float2(u, v), 0).r;

    float3 view = worldPos - cameraPosition.xyz;
    float cx = dot(view, cameraRight.xyz);
    float cy = dot(view, cameraUp.xyz);
    float d  = dot(view, cameraForward.xyz);

    VSOut o;
    o.pos = float4(
        cx * projScaleX + panNdcX * d,
        cy * projScaleY + panNdcY * d,
        (d - nearPlane) / (farPlane - nearPlane) * d,
        d);
    o.worldNor = worldNor;
    o.worldPos = worldPos;
    o.mask = maskVal;
    o.vertexColor = float3(0.0, 0.0, 0.0);
    return o;
}

float4 VSDisplacementShadow(uint vid : SV_VertexID) : SV_POSITION
{
    uint M = (uint)displacementGridResolution;
    uint x = vid % M;
    uint z = vid / M;
    float u = (float)x / (displacementGridResolution - 1.0);
    float v = (float)z / (displacementGridResolution - 1.0);
    float3 worldPos = SampleDisplacedWorldPos(u, v);
    float3 lightUv = LightSpace01(worldPos);
    return float4(lightUv.x * 2.0 - 1.0, lightUv.y * 2.0 - 1.0, saturate(lightUv.z), 1.0);
}

float2 SectionUvForVertex(uint vid, out bool isBottomGrid, out bool isWallBottom, out float3 normal)
{
    uint M = (uint)displacementGridResolution;
    uint wallVertexCount = 8u * M;
    isBottomGrid = vid >= wallVertexCount;
    isWallBottom = false;

    if (isBottomGrid)
    {
        uint gridVid = vid - wallVertexCount;
        uint x = gridVid % M;
        uint z = gridVid / M;
        normal = float3(0.0, -1.0, 0.0);
        return float2(
            (float)x / (displacementGridResolution - 1.0),
            (float)z / (displacementGridResolution - 1.0));
    }

    uint side = vid / (2u * M);
    uint sideLocal = vid - side * 2u * M;
    uint i = sideLocal / 2u;
    isWallBottom = (sideLocal & 1u) != 0u;
    float t = (float)i / (displacementGridResolution - 1.0);

    if (side == 0u)
    {
        normal = float3(0.0, 0.0, 1.0);
        return float2(t, 0.0);
    }
    if (side == 1u)
    {
        normal = float3(1.0, 0.0, 0.0);
        return float2(1.0, t);
    }
    if (side == 2u)
    {
        normal = float3(0.0, 0.0, -1.0);
        return float2(1.0 - t, 1.0);
    }

    normal = float3(-1.0, 0.0, 0.0);
    return float2(0.0, 1.0 - t);
}

VSOut VSDisplacementSection(uint vid : SV_VertexID)
{
    bool isBottomGrid;
    bool isWallBottom;
    float3 worldNor;
    float2 uv = SectionUvForVertex(vid, isBottomGrid, isWallBottom, worldNor);
    float3 worldPos = isWallBottom || isBottomGrid
        ? float3(
            lerp(-displacementHalfSize, displacementHalfSize, uv.x),
            0.0,
            lerp(displacementHalfSize, -displacementHalfSize, uv.y))
        : SampleDisplacedWorldPos(uv.x, uv.y);

    float3 view = worldPos - cameraPosition.xyz;
    float cx = dot(view, cameraRight.xyz);
    float cy = dot(view, cameraUp.xyz);
    float d  = dot(view, cameraForward.xyz);

    VSOut o;
    o.pos = float4(
        cx * projScaleX + panNdcX * d,
        cy * projScaleY + panNdcY * d,
        (d - nearPlane) / (farPlane - nearPlane) * d,
        d);
    o.worldNor = worldNor;
    o.worldPos = worldPos;
    o.mask = isBottomGrid ? 0.0 : 2.0;
    o.vertexColor = float3(0.0, 0.0, 0.0);
    return o;
}

float4 VSDisplacementSectionShadow(uint vid : SV_VertexID) : SV_POSITION
{
    bool isBottomGrid;
    bool isWallBottom;
    float3 worldNor;
    float2 uv = SectionUvForVertex(vid, isBottomGrid, isWallBottom, worldNor);
    float3 worldPos = isWallBottom || isBottomGrid
        ? float3(
            lerp(-displacementHalfSize, displacementHalfSize, uv.x),
            0.0,
            lerp(displacementHalfSize, -displacementHalfSize, uv.y))
        : SampleDisplacedWorldPos(uv.x, uv.y);
    float3 lightUv = LightSpace01(worldPos);
    return float4(lightUv.x * 2.0 - 1.0, lightUv.y * 2.0 - 1.0, saturate(lightUv.z), 1.0);
}

struct PatchControlPoint
{
    float2 uv : TEXCOORD0;
};

struct PatchTessFactors
{
    float edge[4] : SV_TessFactor;
    float inside[2] : SV_InsideTessFactor;
};

PatchControlPoint VSDisplacementPatch(uint vid : SV_VertexID)
{
    uint M = (uint)displacementGridResolution;
    uint x = vid % M;
    uint z = vid / M;

    PatchControlPoint o;
    o.uv = float2(
        (float)x / (displacementGridResolution - 1.0),
        (float)z / (displacementGridResolution - 1.0));
    return o;
}

float TessFactorForUv(float2 uv)
{
    float3 worldPos = SampleDisplacedWorldPos(uv.x, uv.y);
    float dist = length(worldPos - cameraPosition.xyz);
    float t = saturate((dist - tessellationNearDistance) / max(tessellationFarDistance - tessellationNearDistance, 1.0));
    return lerp(tessellationMaxFactor, tessellationMinFactor, t);
}

PatchTessFactors HSDisplacementConstants(InputPatch<PatchControlPoint, 4> patch, uint patchId : SV_PrimitiveID)
{
    PatchTessFactors o;
    float2 uv0 = patch[0].uv;
    float2 uv1 = patch[1].uv;
    float2 uv2 = patch[2].uv;
    float2 uv3 = patch[3].uv;

    o.edge[0] = TessFactorForUv((uv0 + uv1) * 0.5);
    o.edge[1] = TessFactorForUv((uv1 + uv2) * 0.5);
    o.edge[2] = TessFactorForUv((uv2 + uv3) * 0.5);
    o.edge[3] = TessFactorForUv((uv3 + uv0) * 0.5);
    float inside = (o.edge[0] + o.edge[1] + o.edge[2] + o.edge[3]) * 0.25;
    o.inside[0] = inside;
    o.inside[1] = inside;
    return o;
}

[domain("quad")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("HSDisplacementConstants")]
PatchControlPoint HSDisplacement(InputPatch<PatchControlPoint, 4> patch, uint controlPointId : SV_OutputControlPointID)
{
    return patch[controlPointId];
}

float2 InterpolatePatchUv(OutputPatch<PatchControlPoint, 4> patch, float2 uv)
{
    float2 top = lerp(patch[0].uv, patch[1].uv, uv.x);
    float2 bottom = lerp(patch[3].uv, patch[2].uv, uv.x);
    return lerp(top, bottom, uv.y);
}

[domain("quad")]
VSOut DSDisplacement(PatchTessFactors factors, float2 domainUv : SV_DomainLocation, const OutputPatch<PatchControlPoint, 4> patch)
{
    float2 terrainUv = InterpolatePatchUv(patch, domainUv);
    float3 worldPos = SampleDisplacedWorldPos(terrainUv.x, terrainUv.y);
    float3 worldNor = SampleDisplacedNormal(terrainUv.x, terrainUv.y);
    float maskVal = displacementMask.SampleLevel(linearSampler, terrainUv, 0).r;

    float3 view = worldPos - cameraPosition.xyz;
    float cx = dot(view, cameraRight.xyz);
    float cy = dot(view, cameraUp.xyz);
    float d  = dot(view, cameraForward.xyz);

    VSOut o;
    o.pos = float4(
        cx * projScaleX + panNdcX * d,
        cy * projScaleY + panNdcY * d,
        (d - nearPlane) / (farPlane - nearPlane) * d,
        d);
    o.worldNor = worldNor;
    o.worldPos = worldPos;
    o.mask = maskVal;
    o.vertexColor = float3(0.0, 0.0, 0.0);
    return o;
}

[domain("quad")]
float4 DSDisplacementShadow(PatchTessFactors factors, float2 domainUv : SV_DomainLocation, const OutputPatch<PatchControlPoint, 4> patch) : SV_POSITION
{
    float2 terrainUv = InterpolatePatchUv(patch, domainUv);
    float3 worldPos = SampleDisplacedWorldPos(terrainUv.x, terrainUv.y);
    float3 lightUv = LightSpace01(worldPos);
    return float4(lightUv.x * 2.0 - 1.0, lightUv.y * 2.0 - 1.0, saturate(lightUv.z), 1.0);
}

// Sample the cloud shadow texture by projecting the ground point along the
// sun direction up to altitudeMin (the cloud band base) and looking up the
// transmittance at that (x, z). Returns 1.0 when not in cloud shadow,
// 0.0 when fully under cloud.
float ComputeCloudShadowVisibility(float3 worldPos)
{
    if (cloudShadowEnabled < 0.5 || cloudShadowSizeX <= 0.0 || cloudShadowSizeZ <= 0.0)
    {
        return 1.0;
    }
    if (sunDirection.y < 0.05)
    {
        return 1.0;
    }
    float dy = cloudShadowAltitudeMin - worldPos.y;
    float2 offsetXZ = float2(sunDirection.x, sunDirection.z) * (dy / sunDirection.y);
    float worldX = worldPos.x + offsetXZ.x;
    float worldZ = worldPos.z + offsetXZ.y;
    float u = (worldX - cloudShadowMinX) / cloudShadowSizeX;
    float v = (worldZ - cloudShadowMinZ) / cloudShadowSizeZ;
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
    {
        return 1.0;
    }
    return cloudShadowMap.SampleLevel(linearSampler, float2(u, v), 0);
}

float ComputeShadowVisibility(float3 worldPos)
{
    if (shadowEnabled < 0.5)
    {
        return 1.0;
    }

    float3 lightUv = LightSpace01(worldPos);
    float2 uv = float2(lightUv.x, 1.0 - lightUv.y);
    float depth = lightUv.z;

    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || depth <= 0.0 || depth >= 1.0)
    {
        return 1.0;
    }

    float texel = 1.0 / max(shadowMapResolution, 1.0);
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            float mapDepth = shadowMap.SampleLevel(shadowSampler, uv + float2(x, y) * texel, 0).r;
            visibility += (depth - shadowBias <= mapDepth) ? 1.0 : 0.0;
        }
    }
    return visibility / 9.0;
}

float3 DebugShadowColor(float3 worldPos)
{
    float3 lightUv = LightSpace01(worldPos);
    float2 uv = float2(lightUv.x, 1.0 - lightUv.y);
    float depth = lightUv.z;

    float inRange = (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0 && depth > 0.0 && depth < 1.0) ? 1.0 : 0.0;
    if (inRange < 0.5)
    {
        if (depth <= 0.0)
        {
            return float3(0.0, 0.0, 1.0);
        }
        if (depth >= 1.0)
        {
            return float3(1.0, 0.0, 1.0);
        }
        if (uv.x <= 0.0 || uv.x >= 1.0)
        {
            return float3(1.0, 0.0, 0.0);
        }
        return float3(0.0, 1.0, 0.0);
    }

    if (uv.x <= 0.0 || uv.x >= 1.0 || uv.y <= 0.0 || uv.y >= 1.0 || depth <= 0.0 || depth >= 1.0)
    {
        return float3(0.0, 0.0, 1.0);
    }

    float mapDepth = shadowMap.SampleLevel(shadowSampler, uv, 0).r;
    float visible = (depth - shadowBias <= mapDepth) ? 1.0 : 0.0;
    float delta = saturate(abs(depth - mapDepth) * 24.0);
    return lerp(float3(0.02, 0.02, 0.025), float3(1.0, 0.95, 0.82), visible) + delta * float3(0.0, 0.18, 0.0);
}

float2 TerrainTextureUv(float3 worldPos)
{
    float terrainSize = max(albedoColor.a, 1.0);
    return float2(worldPos.x / terrainSize + 0.5, 0.5 - worldPos.z / terrainSize);
}

bool UseColorTexture()
{
    return (colorTextureMode - 2.0 * floor(colorTextureMode * 0.5)) > 0.5;
}

bool UseMaskTexture()
{
    return colorTextureMode > 1.5;
}

float SamplePreviewMask(VSOut i)
{
    if (i.mask > 1.5)
    {
        return i.mask;
    }
    if (UseMaskTexture())
    {
        return displacementMask.Sample(linearSampler, TerrainTextureUv(i.worldPos)).r;
    }
    return i.mask;
}

bool IsSectionSurface(VSOut i)
{
    return i.mask > 1.5 || i.worldNor.y < -0.5;
}

float4 PSSurface(VSOut i) : SV_TARGET
{
    float3 n = normalize(i.worldNor);
    float3 V = normalize(cameraPosition.xyz - i.worldPos);

    float3 keyLight = normalize(float3(-0.58, 0.72, 0.38));
    float3 fillLight = normalize(float3(0.42, 0.36, -0.82));
    float3 rimLight = normalize(float3(0.18, 0.54, -0.82));

    float key = saturate(dot(n, keyLight));
    float fill = saturate(dot(n, fillLight));
    float rim = pow(saturate(1.0 - abs(dot(n, V))), 2.8) * saturate(dot(n, rimLight) * 0.5 + 0.5);
    float sky = saturate(n.y) * 0.28;
    float slope = 1.0 - saturate(n.y);
    float height = saturate(i.worldPos.y / 1800.0 + 0.45);
    float ambient = 0.18;

    if (IsSectionSurface(i))
    {
        float3 sectionBaseColor = saturate(sectionColor.rgb);
        float sectionLight = 0.72 + saturate(dot(n, keyLight)) * 0.18 + saturate(n.y) * 0.10;
        float3 col = sectionBaseColor * sectionLight;
        if (lightingMode > 0.5 && maskPreview < 0.5)
        {
            float3 L = normalize(sunDirection.xyz);
            float ndl = saturate(dot(n, L));
            float visibility = ComputeShadowVisibility(i.worldPos);
            float cloudVisibility = ComputeCloudShadowVisibility(i.worldPos);
            float cloudShadowFactor = lerp(1.0, cloudVisibility, cloudShadowStrength);
            float shadowAmount = (1.0 - visibility) * shadowStrength;
            float shadowMix = lerp(1.0 - shadowStrength * 0.75, 1.0, visibility);

            float3 skyAmbient = n.y >= 0.0
                ? lerp(skyHorizonColor.rgb, skyZenithColor.rgb, n.y)
                : lerp(skyHorizonColor.rgb, skyGroundColor.rgb, -n.y);
            skyAmbient *= ambientStrength;

            float ambientCloudMix = lerp(1.0, cloudVisibility, cloudShadowStrength * 0.4);
            float3 sunTint = skySunColor.rgb * ndl * sunIntensity * shadowMix * cloudShadowFactor;
            float3 bounceTint = skyGroundColor.rgb * ambientStrength * shadowAmount * 0.35;
            col = sectionBaseColor * (skyAmbient * ambientCloudMix + sunTint + bounceTint);
            col = lerp(col, dot(col, float3(0.299, 0.587, 0.114)).xxx, shadowAmount * 0.18);
        }
        if (atmosphereDensity > 0.001 && maskPreview < 0.5)
        {
            float viewDist = length(cameraPosition.xyz - i.worldPos);
            float fogExtinction = atmosphereDensity * (45e-6 + atmosphereMieStrength * 12e-6);
            float fogFactor = saturate(1.0 - exp(-viewDist * fogExtinction));
            float3 viewDir = normalize(i.worldPos - cameraPosition.xyz);
            float cosSun = saturate(dot(viewDir, sunDirection.xyz));
            float warmPush = cosSun * saturate(atmosphereMieStrength * 0.12);
            float3 fogColor = lerp(skyHorizonColor.rgb, skySunColor.rgb, warmPush);
            col = lerp(col, fogColor, fogFactor);
        }
        col = pow(saturate(col), 1.0 / 1.18);
        return float4(col, 1.0);
    }

    float aoSample = (aoEnabled > 0.5 && maskPreview < 0.5)
        ? aoTexture.Sample(linearSampler, TerrainTextureUv(i.worldPos))
        : 1.0;
    float aoFactor = lerp(1.0, aoSample, aoStrength);
    float light = ambient * aoFactor + key * 0.78 + fill * 0.18 + sky * aoFactor + rim * 0.34;
    float3 lowland = float3(0.32, 0.38, 0.32);
    float3 highland = float3(0.54, 0.52, 0.46);
    float3 slopeTint = float3(0.43, 0.39, 0.34);
    float3 baseColor = UseColorTexture()
        ? colorTexture.Sample(linearSampler, TerrainTextureUv(i.worldPos)).rgb
        : lerp(lerp(lowland, highland, height), slopeTint, slope * 0.42);

    float3 col = baseColor * light;
    if (lightingMode > 0.5 && maskPreview < 0.5)
    {
        float3 L = normalize(sunDirection.xyz);
        float ndl = saturate(dot(n, L));
        float visibility = ComputeShadowVisibility(i.worldPos);
        float cloudVisibility = ComputeCloudShadowVisibility(i.worldPos);
        float cloudShadowFactor = lerp(1.0, cloudVisibility, cloudShadowStrength);
        float viewFacing = pow(saturate(dot(n, V) * 0.5 + 0.5), 0.35);
        float shadowAmount = (1.0 - visibility) * shadowStrength;
        float shadowMix = lerp(1.0 - shadowStrength * 0.75, 1.0, visibility);

        // Hemisphere ambient driven by sky settings: surfaces facing up
        // sample the zenith colour, surfaces facing the horizon sample the
        // horizon colour, surfaces facing down sample the ground colour
        // (= bounce light from the world below the horizon).
        float3 skyAmbient;
        if (n.y >= 0.0)
        {
            skyAmbient = lerp(skyHorizonColor.rgb, skyZenithColor.rgb, n.y);
        }
        else
        {
            skyAmbient = lerp(skyHorizonColor.rgb, skyGroundColor.rgb, -n.y);
        }
        skyAmbient *= ambientStrength;
        skyAmbient *= aoFactor;

        // Cloud shadow attenuates the sun (direct light) fully but only
        // partially attenuates ambient sky light, since clouds scatter light
        // back down and the sky term is dominated by skylight not sun.
        float ambientCloudMix = lerp(1.0, cloudVisibility, cloudShadowStrength * 0.4);

        // Direct sun: uses the sky's sun colour so a warm sky tints the
        // direct light too.
        float3 sunTint = skySunColor.rgb * ndl * sunIntensity * shadowMix * cloudShadowFactor;

        // Bounce / fill: light reflected off the ground reaching the
        // shadowed side of surfaces. Tinted by groundColor for consistency
        // with the lower hemisphere.
        float3 bounceTint = skyGroundColor.rgb * ambientStrength * shadowAmount * 0.55;

        float3 slopeMicroShade = lerp(float3(0.78, 0.80, 0.82), float3(1.06, 1.05, 1.02), viewFacing);
        float3 effectiveAlbedo = UseColorTexture()
            ? colorTexture.Sample(linearSampler, TerrainTextureUv(i.worldPos)).rgb
            : albedoColor.rgb;
        col = effectiveAlbedo * (skyAmbient * ambientCloudMix + sunTint + bounceTint) * slopeMicroShade;
        col = lerp(col, dot(col, float3(0.299, 0.587, 0.114)).xxx, shadowAmount * 0.18);
        col += pow(saturate(ndl), 24.0) * sunIntensity * visibility * cloudShadowFactor * 0.045;
    }
    if (maskPreview > 0.5)
    {
        // 壁面 (BuildMeshFromHeightfield の側壁) は mask に sentinel = 2.0
        // を入れている。マスクプレビュー時にここをモードごとのグレーで
        // 一律塗り潰し、上端マスクが縦方向に引き伸ばされて見えないようにする。
        //   モード 0 / モード 2: 25% グレー (RGB=0.25) 一律。
        //   モード 1 (グレー×オレンジ): このモードのベースグレー lowMask
        //     をライティング非依存の一色で塗る (周囲のライティング差に
        //     惑わされず壁面が均一に見える)。
        float maskSample = SamplePreviewMask(i);
        if (maskSample > 1.5)
        {
            if (maskShadingMode > 0.5 && maskShadingMode < 1.5)
            {
                return float4(0.18, 0.20, 0.21, 1.0);
            }
            return float4(0.25, 0.25, 0.25, 1.0);
        }
        float mask = saturate(maskSample);
        if (maskShadingMode < 0.5)
        {
            // モード 0: 純粋グレースケール (mask=0→黒, mask=1→白)。
            // リム/ガンマも掛けず、2D マップ表示と同じ素直なランプにする。
            return float4(mask, mask, mask, 1.0);
        }
        if (maskShadingMode > 1.5)
        {
            // モード 2: グレースケール + 斜線オーバーレイ (GeoGen 風)。
            // 飽和域に対角線パターンを均等な 3:1 比率でオーバーレイし、
            // マスクのクリッピング (1.0 への張り付き / 0.0 への張り付き)
            // を視覚的に可視化する。背景は純白 / 純黒、4 ストライプ中
            // 1 つだけグレー (= 斜線) にしてコントラストを抑えた。
            //   mask >= 0.99 (白の所)  → 白×3 + グレー×1
            //   mask <= 0.01 (黒の所)  → 黒×3 + グレー×1
            //   中間域                  → 通常のグレースケールランプ (斜線なし)
            // ストライプ幅は 1 px、4 ストライプ周期 (= 4 px) で 1 つだけ反転。
            // SV_POSITION.xy はピクセル中心 (0.5 オフセット)。x+y 合計を
            // float でやって floor すると合計時に精度を失って (例 0.5 + 1.5
            // = 1.999...) 隣接ピクセルで stripe index が予期せず揺れて
            // 見える幅広のバンドになるので、x と y を別々に int に
            // トランケートしてから加算する。
            // 仕上げにハーフランバート (n·L*0.5+0.5) を最低 0.3 にクランプ
            // (lerp(0.3, 1.0, halfL)) して乗算し、GeoGen 風の陰影を載せる。
            // 太陽方向=1.0 / 真横=0.65 / 反対側=0.3 の線形カーブ。床 0.3 で
            // 反対側でも完全な黒に潰れず、ハッチが陰影パターンとして
            // 読める明度を保つ。モード 2 限定の処理。
            float c;
            if (mask >= 0.99 || mask <= 0.01)
            {
                int2 px = int2(i.pos.xy);
                int stripeIdx = (px.x + px.y) & 3;
                bool isMinor = (stripeIdx == 3);
                float majorVal = (mask >= 0.99) ? 1.0 : 0.0;
                float stripeGray = 0.5;
                c = isMinor ? stripeGray : majorVal;
            }
            else
            {
                c = mask;
            }
            float3 hatchN = normalize(i.worldNor);
            float3 hatchL = normalize(sunDirection.xyz);
            float halfL = saturate(dot(hatchN, hatchL) * 0.5 + 0.5);
            halfL = lerp(0.3, 1.0, halfL);
            c *= halfL;
            return float4(c, c, c, 1.0);
        }
        // モード 1: グレー×オレンジ (ライティング付き)。
        float3 lowMask = float3(0.18, 0.20, 0.21);
        float3 highMask = float3(0.95, 0.56, 0.18);
        baseColor = lerp(lowMask, highMask, mask);
        col = baseColor * (ambient + key * 0.65 + fill * 0.18 + sky * 0.5);
        col += pow(mask, 2.2) * float3(0.42, 0.20, 0.05);
    }
    col += rim * float3(0.16, 0.18, 0.20);

    // Aerial perspective — fog distant terrain toward an atmospheric tint.
    // Skipped for mask preview so masks stay readable.
    if (atmosphereDensity > 0.001 && maskPreview < 0.5)
    {
        float viewDist = length(cameraPosition.xyz - i.worldPos);
        // Automatic aerial perspective. Distance haze is integrated as a
        // standard Beer-Lambert extinction with no upper clip — far terrain
        // is allowed to converge fully to fogColor, which is what real
        // atmospheric perspective does. Users dial overall strength via
        // atmosphereDensity / atmosphereMieStrength.
        float fogExtinction = atmosphereDensity * (45e-6 + atmosphereMieStrength * 12e-6);
        float fogFactor = saturate(1.0 - exp(-viewDist * fogExtinction));

        // Direction-dependent fog colour. Use the sky's actual horizon
        // colour (matches the sky shader so terrain blends seamlessly into
        // the sky), with a mild warm push near the sun direction.
        float3 viewDir = normalize(i.worldPos - cameraPosition.xyz);
        float cosSun = saturate(dot(viewDir, sunDirection.xyz));
        float warmPush = cosSun * saturate(atmosphereMieStrength * 0.12);
        float3 fogColor = lerp(skyHorizonColor.rgb, skySunColor.rgb, warmPush);
        col = lerp(col, fogColor, fogFactor);
    }

    col = pow(saturate(col), 1.0 / 1.18);
    return float4(col, 1.0);
}

float4 PSEdge(VSOut i) : SV_TARGET
{
    if (i.mask < -1.5)
    {
        return float4(0.30, 0.51, 0.86, 0.88);
    }
    if (i.mask < -0.5)
    {
        return float4(0.82, 0.30, 0.30, 0.88);
    }
    return float4(albedoColor.rgb, saturate(albedoColor.a));
}

float WaterDepthOpacity(float depthMeters, float pathScale)
{
    float depth = max(depthMeters, 0.0);
    float scale = max(pathScale, 1.0);
    return 1.0 - exp(-depth / scale);
}

float WaterOpacityClarityDistance(float opacity)
{
    return 30.0 * pow(40.0, 1.0 - saturate(opacity));
}

float WaterAbsorptionAlpha(float depthMeters, float opacity)
{
    return WaterDepthOpacity(depthMeters, WaterOpacityClarityDistance(opacity));
}

float WaterVisibleAlpha(float depthOpacity, float opacity, float minVisibleAlpha)
{
    float coverage = saturate(depthOpacity);
    float alphaFloor = minVisibleAlpha * saturate(opacity) * step(0.0001, coverage);
    return max(coverage, alphaFloor);
}

float SampleWaterTerrainHeight(float2 uv)
{
    return displacementHeights.SampleLevel(linearSampler, saturate(uv), 0).r;
}

float2 ProjectWorldToSceneUv(float3 worldPos)
{
    float3 view = worldPos - cameraPosition.xyz;
    float cx = dot(view, cameraRight.xyz);
    float cy = dot(view, cameraUp.xyz);
    float d = max(dot(view, cameraForward.xyz), nearPlane);
    float2 ndc = float2(
        (cx * projScaleX + panNdcX * d) / d,
        (cy * projScaleY + panNdcY * d) / d);
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float EstimateWaterViewPathLength(float3 waterPos, float3 rayDir, float terrainSize)
{
    float3 dir = normalize(rayDir);
    float maxDistance = min(max(terrainSize * 1.25, 100.0), 4000.0);
    if (dir.y >= -0.001)
    {
        return maxDistance;
    }

    const int stepCount = 24;
    float prevT = 0.0;
    float2 prevUV = float2(waterPos.x / terrainSize + 0.5, 0.5 - waterPos.z / terrainSize);
    float prevClearance = waterPos.y - SampleWaterTerrainHeight(prevUV);

    [unroll]
    for (int stepIndex = 1; stepIndex <= stepCount; ++stepIndex)
    {
        float t = maxDistance * (float)stepIndex / (float)stepCount;
        float3 p = waterPos + dir * t;
        float2 uv = float2(p.x / terrainSize + 0.5, 0.5 - p.z / terrainSize);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        {
            return maxDistance;
        }

        float clearance = p.y - SampleWaterTerrainHeight(uv);
        if (clearance <= 0.0)
        {
            float denom = max(prevClearance - clearance, 1e-4);
            float hitT = lerp(prevT, t, saturate(prevClearance / denom));
            return hitT;
        }

        prevT = t;
        prevClearance = clearance;
    }

    return maxDistance;
}

struct WaterWaveInfo
{
    float3 normal;
    float3 specularNormal;
    float ripple;
};

void AccumulateWaterWave(float2 pos, float2 dir, float wavelength, float steepness, float phaseOffset, float specularWeight, inout float2 gradient, inout float2 specularGradient, inout float ripple)
{
    float2 waveDir = normalize(dir);
    float safeWavelength = max(wavelength, 1.0);
    float waveNumber = 6.2831853 / safeWavelength;
    float amplitude = steepness / waveNumber;
    float phase = dot(pos, waveDir) * waveNumber + phaseOffset;
    float s = sin(phase);
    float c = cos(phase);
    gradient += waveDir * (c * steepness);
    specularGradient += waveDir * (c * steepness * specularWeight);
    ripple += s * amplitude;
}

WaterWaveInfo ComputeWaterWaves(float2 worldXZ, float waveScaleMeters)
{
    float scale = max(waveScaleMeters, 1.0);
    float2 gradient = float2(0.0, 0.0);
    float2 specularGradient = float2(0.0, 0.0);
    float ripple = 0.0;

    AccumulateWaterWave(worldXZ, float2(0.87, 0.49), scale * 1.00, 0.060, 0.10, 0.95, gradient, specularGradient, ripple);
    AccumulateWaterWave(worldXZ, float2(-0.36, 0.93), scale * 0.58, 0.024, 1.70, 0.45, gradient, specularGradient, ripple);
    AccumulateWaterWave(worldXZ, float2(0.16, 0.99), scale * 1.73, 0.036, 3.10, 0.85, gradient, specularGradient, ripple);
    AccumulateWaterWave(worldXZ, float2(-0.91, 0.41), scale * 0.31, 0.010, 4.25, 0.05, gradient, specularGradient, ripple);
    AccumulateWaterWave(worldXZ, float2(0.64, -0.77), scale * 2.45, 0.030, 5.50, 0.75, gradient, specularGradient, ripple);
    AccumulateWaterWave(worldXZ, float2(-0.74, -0.67), scale * 0.83, 0.016, 2.35, 0.25, gradient, specularGradient, ripple);

    WaterWaveInfo info;
    info.normal = normalize(float3(-gradient.x, 1.0, -gradient.y));
    info.specularNormal = normalize(float3(-specularGradient.x * 0.72, 1.0, -specularGradient.y * 0.72));
    info.ripple = ripple / max(scale * 0.042, 0.001);
    return info;
}

float4 PSWater(VSOut i) : SV_TARGET
{
    float terrainSize = max(albedoColor.a, 1.0);
    float2 terrainUV = float2(i.worldPos.x / terrainSize + 0.5, 0.5 - i.worldPos.z / terrainSize);
    float opacity = saturate(i.mask);

    // === 断面 側壁 (normal.y ≈ 0) ===
    if (i.worldNor.y < 0.5)
    {
        float3 n = normalize(i.worldNor);
        float3 L = normalize(sunDirection.xyz);
        float ndl = dot(n, L) * 0.35 + 0.65;

        float terrainH = displacementHeights.SampleLevel(linearSampler, saturate(terrainUV), 0).r;
        float waterColumnDepth = max(waterLevelParam - terrainH, 0.0);
        float distanceFromTerrain = max(i.worldPos.y - terrainH, 0.0);
        float thicknessNorm = WaterAbsorptionAlpha(waterColumnDepth, opacity);

        float3 shallowCol = albedoColor.rgb * 1.5 + float3(0.01, 0.03, 0.0);
        float3 deepCol    = albedoColor.rgb * 0.35 + float3(0.0, 0.01, 0.09);
        float3 wallColor  = lerp(shallowCol, deepCol, thicknessNorm) * ndl;

        float2 sceneUv = ProjectWorldToSceneUv(i.worldPos);
        float2 sideDir = float2(dot(n, cameraRight.xyz), -dot(n, cameraUp.xyz));
        float wobbleA = sin(dot(i.worldPos.xz, float2(0.037, 0.021)) + i.worldPos.y * 0.018);
        float wobbleB = sin(dot(i.worldPos.xz, float2(-0.019, 0.044)) + i.worldPos.y * 0.031 + 1.7);
        float2 wobble = float2(wobbleA, wobbleB) * 0.5 + sideDir;
        float sideRefractionWeight = saturate((1.0 - thicknessNorm) * (1.0 - opacity * 0.25) * waterRefractionStrength);
        float blurAmount = saturate(waterRefractionBlur);
        float distortionRadius = (0.0015 + saturate(distanceFromTerrain / 220.0) * 0.0045) * sideRefractionWeight;
        float blurRadius = (0.0030 + saturate(distanceFromTerrain / 220.0) * 0.0180) * sideRefractionWeight * blurAmount;
        float2 refractUv = saturate(sceneUv + wobble * distortionRadius * 1.8);
        float3 sideScene =
            sceneColorTexture.Sample(linearSampler, refractUv).rgb * 0.24 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2( blurRadius, 0.0))).rgb * 0.12 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2(-blurRadius, 0.0))).rgb * 0.12 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2(0.0,  blurRadius))).rgb * 0.12 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2(0.0, -blurRadius))).rgb * 0.12 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2( blurRadius,  blurRadius))).rgb * 0.07 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2(-blurRadius,  blurRadius))).rgb * 0.07 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2( blurRadius, -blurRadius))).rgb * 0.07 +
            sceneColorTexture.Sample(linearSampler, saturate(refractUv + float2(-blurRadius, -blurRadius))).rgb * 0.07;
        wallColor = lerp(wallColor, lerp(sideScene, wallColor, saturate(thicknessNorm * 0.70 + opacity * 0.30)), sideRefractionWeight * 0.42);
        wallColor = pow(saturate(wallColor), 1.0 / 1.18);

        float alpha = WaterVisibleAlpha(WaterAbsorptionAlpha(distanceFromTerrain, opacity), opacity, 0.24);
        return float4(wallColor, alpha);
    }

    // === 水面 上面 ===
    WaterWaveInfo wave = ComputeWaterWaves(i.worldPos.xz, waterWavesScale);
    float3 n = wave.normal;
    float3 specularN = wave.specularNormal;
    float3 fresnelN = normalize(lerp(n, specularN, 0.65));

    float3 V = normalize(cameraPosition.xyz - i.worldPos);
    float3 L = normalize(sunDirection.xyz);

    // IOR ベースの Schlick フレネル
    float r   = (1.0 - waterRefractiveIndex) / (1.0 + waterRefractiveIndex);
    float F0  = r * r;
    float cosTheta = saturate(abs(dot(fresnelN, V)));
    float fresnel  = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);

    // スペキュラー
    float flatSunMirror = saturate(dot(reflect(-L, float3(0.0, 1.0, 0.0)), V));
    float sunGlintPath = pow(smoothstep(0.82, 0.97, flatSunMirror), 1.8);
    float sunMirror = saturate(dot(reflect(-L, specularN), V));
    float broadSpec = pow(sunMirror, 18.0) * 0.055;
    float sparkleSpec = pow(sunMirror, 88.0) * 0.32;
    float spec = min((broadSpec + sparkleSpec) * sunGlintPath * sunIntensity, 0.55);

    // 地形高さから鉛直水深を算出し、上面は視線方向の水中距離を主に使う
    float terrainH  = displacementHeights.SampleLevel(linearSampler, terrainUV, 0).r;
    float vertDepth = max(waterLevelParam - terrainH, 0.0);

    float viewPathLength = EstimateWaterViewPathLength(i.worldPos, -V, terrainSize);
    float shallowDepth = min(vertDepth, viewPathLength);

    // 水深で色を変化: 上面は鉛直方向ではなく、カメラから見た水中距離で濁らせる
    float depthColorFactor = WaterAbsorptionAlpha(viewPathLength, saturate(opacity + 0.18));
    float3 shallowCol = albedoColor.rgb * 1.55 + float3(0.01, 0.04, -0.02);
    float3 deepCol    = albedoColor.rgb * 0.55 + float3(0.0, 0.02, 0.10);
    float3 water = lerp(shallowCol, deepCol, depthColorFactor) * (1.0 + wave.ripple * 0.035);

    float2 sceneUv = ProjectWorldToSceneUv(i.worldPos);
    float2 refractDir = float2(dot(n, cameraRight.xyz), -dot(n, cameraUp.xyz));
    float clarity = 1.0 - WaterAbsorptionAlpha(viewPathLength, opacity);
    float refractionWeight = saturate((1.0 - fresnel) * clarity * (1.0 - opacity * 0.35) * waterRefractionStrength);
    float refractionOffset = (0.0025 + saturate(viewPathLength / 600.0) * 0.010) * refractionWeight;
    float2 refractUv = saturate(sceneUv + refractDir * refractionOffset);
    float3 refractedScene = sceneColorTexture.Sample(linearSampler, refractUv).rgb;
    float3 refractedWater = lerp(refractedScene, water, saturate(depthColorFactor * 0.72 + opacity * 0.25));
    water = lerp(water, refractedWater, refractionWeight * 0.72);

    // 空反射 (反射方向 = V の Y 反転)
    float3 reflRay = float3(-V.x, V.y, -V.z);
    float3 skyRefl = lerp(skyHorizonColor.rgb, skyZenithColor.rgb, saturate(reflRay.y * 1.8));

    // 地形色反射 (高さベース近似 + 視差オフセット)
    float2 parallaxUV  = terrainUV + reflRay.xz * (vertDepth * 0.06 / terrainSize);
    float  reflTerrH   = displacementHeights.SampleLevel(linearSampler, saturate(parallaxUV), 0).r;
    float  reflHeight  = saturate(reflTerrH / 1800.0 + 0.45);
    float3 terrainRefl = lerp(float3(0.32, 0.38, 0.32), float3(0.54, 0.52, 0.46), reflHeight);

    // 辺縁部で地形近似反射だけ抑制 (輝線防止、opacity と太陽スペキュラーには影響させない)
    float2 edgeMask   = abs(terrainUV - 0.5) * 2.0;
    float edgeReflFade = 1.0 - smoothstep(0.92, 1.0, max(edgeMask.x, edgeMask.y));

    float terrainReflWeight = saturate(1.0 - shallowDepth * 0.008) * fresnel;
    float3 reflection = lerp(skyRefl * fresnel, terrainRefl, terrainReflWeight) * edgeReflFade;
    water = lerp(water, reflection, saturate(fresnel + terrainReflWeight * 0.5) * edgeReflFade);
    water += spec * skySunColor.rgb;

    float viewPathAlpha = WaterAbsorptionAlpha(viewPathLength, opacity);
    float depthAlpha = saturate(viewPathAlpha);
    float shoreAlpha = 1.0 - smoothstep(0.0, max(WaterOpacityClarityDistance(opacity) * 0.28, 2.0), shallowDepth);
    float reflectionAlpha = saturate(fresnel * 0.10 + spec * 0.05);
    float alpha = max(max(WaterVisibleAlpha(depthAlpha, opacity, 0.16), opacity * shoreAlpha * 0.18), reflectionAlpha);

    water = pow(saturate(water), 1.0 / 1.18);
    return float4(water, alpha);
}
