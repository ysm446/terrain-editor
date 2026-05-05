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
    float padding;
    float4 lightRight;
    float4 lightUp;
    float4 lightForward;
    float4 lightCenter;
    float lightWorldRadius;
    float lightNearPlane;
    float lightFarPlane;
    float padding2;
};

Texture2D shadowMap : register(t0);
SamplerState shadowSampler : register(s0);

float3 LightSpace01(float3 worldPos)
{
    float3 view = worldPos - lightCenter.xyz;
    float halfX = max(lightWorldRadius, 1.0);
    float halfY = max(lightNearPlane, 1.0);
    float depthRange = max(lightFarPlane, 1.0);
    float depthMin = padding2;
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
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 worldNor : NORMAL;
    float3 worldPos : TEXCOORD0;
    float mask : TEXCOORD1;
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
    return o;
}

float4 VSShadow(VSIn i) : SV_POSITION
{
    float3 lightUv = LightSpace01(i.pos);
    return float4(lightUv.x * 2.0 - 1.0, lightUv.y * 2.0 - 1.0, saturate(lightUv.z), 1.0);
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

    float light = ambient + key * 0.78 + fill * 0.18 + sky + rim * 0.34;
    float3 lowland = float3(0.32, 0.38, 0.32);
    float3 highland = float3(0.54, 0.52, 0.46);
    float3 slopeTint = float3(0.43, 0.39, 0.34);
    float3 baseColor = lerp(lowland, highland, height);
    baseColor = lerp(baseColor, slopeTint, slope * 0.42);

    float3 col = baseColor * light;
    if (lightingMode > 1.5 && maskPreview < 0.5)
    {
        return float4(saturate(DebugShadowColor(i.worldPos)), 1.0);
    }
    if (lightingMode > 0.5 && maskPreview < 0.5)
    {
        float3 L = normalize(sunDirection.xyz);
        float ndl = saturate(dot(n, L));
        float visibility = ComputeShadowVisibility(i.worldPos);
        float viewFacing = pow(saturate(dot(n, V) * 0.5 + 0.5), 0.35);
        float shadowAmount = (1.0 - visibility) * shadowStrength;
        float shadowMix = lerp(1.0 - shadowStrength * 0.75, 1.0, visibility);
        float upFacing = saturate(n.y * 0.55 + 0.45);
        float3 skyTint = lerp(float3(0.42, 0.45, 0.45), float3(0.58, 0.61, 0.62), upFacing) * ambientStrength;
        float3 sunTint = float3(1.00, 0.96, 0.88) * ndl * sunIntensity * shadowMix;
        float3 bounceTint = float3(0.28, 0.30, 0.28) * ambientStrength * shadowAmount;
        float3 slopeMicroShade = lerp(float3(0.78, 0.80, 0.82), float3(1.06, 1.05, 1.02), viewFacing);
        col = albedoColor.rgb * (skyTint + sunTint + bounceTint) * slopeMicroShade;
        col = lerp(col, dot(col, float3(0.299, 0.587, 0.114)).xxx, shadowAmount * 0.18);
        col += pow(saturate(ndl), 24.0) * sunIntensity * visibility * 0.045;
    }
    if (maskPreview > 0.5)
    {
        float mask = saturate(i.mask);
        float3 lowMask = float3(0.18, 0.20, 0.21);
        float3 highMask = float3(0.95, 0.56, 0.18);
        baseColor = lerp(lowMask, highMask, mask);
        col = baseColor * (ambient + key * 0.65 + fill * 0.18 + sky * 0.5);
        col += pow(mask, 2.2) * float3(0.42, 0.20, 0.05);
    }
    col += rim * float3(0.16, 0.18, 0.20);
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
    return float4(albedoColor.rgb, 0.88);
}
