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
    float2 pad;
};

struct VSIn
{
    float3 pos : POSITION;
    float3 nor : NORMAL;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float3 worldNor : NORMAL;
    float3 worldPos : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
    float3 view = i.pos - cameraPosition.xyz;
    float cx = dot(view, cameraRight.xyz);
    float cy = dot(view, cameraUp.xyz);
    float d = max(nearPlane, dot(view, cameraForward.xyz));

    VSOut o;
    o.pos = float4(
        cx * projScaleX + panNdcX * d,
        cy * projScaleY + panNdcY * d,
        (d - nearPlane) / (farPlane - nearPlane) * d,
        d);
    o.worldNor = i.nor;
    o.worldPos = i.pos;
    return o;
}

float4 PSSurface(VSOut i) : SV_TARGET
{
    float3 n = normalize(i.worldNor);
    float3 L = normalize(float3(-0.45, 0.78, 0.42));
    float3 V = normalize(cameraPosition.xyz - i.worldPos);
    float diff = saturate(dot(n, L)) * 0.5 + 0.5;
    float fres = pow(saturate(1.0 - abs(dot(n, V))), 2.0);
    float h = saturate(i.worldPos.y * 0.5 + 0.5);
    float3 col = float3(
        (92.0 + diff * 76.0 + fres * 26.0 + h * 10.0) / 255.0,
        (92.0 + diff * 76.0 + fres * 26.0 + h * 10.0) / 255.0,
        (92.0 + diff * 76.0 + fres * 26.0 + h * 10.0) / 255.0);
    return float4(col, 1.0);
}

float4 PSEdge(VSOut i) : SV_TARGET
{
    return float4(0.34, 0.34, 0.34, 0.86);
}
