Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDepthMap : register(t2);
Texture2DArray gCascadeShadowMap : register(t3);
SamplerComparisonState gShadowSampler : register(s0);

#include "Fullscreen.hlsl"

#define MAX_POINT_LIGHTS 8
#define MAX_SPOT_LIGHTS 4

struct DirectionalLight
{
    float3 Direction;
    float Intensity;
    float3 Color;
    float Pad0;
};

struct PointLight
{
    float3 Position;
    float Radius;
    float3 Color;
    float Intensity;
};

struct SpotLight
{
    float3 Position;
    float Radius;
    float3 Direction;
    float SpotPower;
    float3 Color;
    float Intensity;
};

cbuffer cbLight : register(b0)
{
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float gAmbientStrength;
    DirectionalLight gDirectional;
    int gPointLightCount;
    int gSpotLightCount;
    float2 gPad0;
    PointLight gPointLights[MAX_POINT_LIGHTS];
    SpotLight gSpotLights[MAX_SPOT_LIGHTS];
}

struct LocalVertexIn
{
    float3 PosL : POSITION;
};

struct LocalVertexOut
{
    float4 PosH : SV_POSITION;
    nointerpolation int LightIndex : TEXCOORD0;
    nointerpolation int LightType : TEXCOORD1;
};

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gPassInvViewProj;
    float3 gPassEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
};

#define SHADOW_CASCADE_COUNT 3

struct Cascade
{
    float SplitNear;
    float SplitFar;
    float2 Pad;
    float4x4 ViewProj;
};

cbuffer cbCascades : register(b2)
{
    Cascade gCascades[SHADOW_CASCADE_COUNT];
};

float3 ReconstructWorldPosition(float2 pixel, float depth, float2 size)
{
    float2 uv = (pixel + 0.5f) / size;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f);
    float4 world = mul(float4(ndc, depth, 1.0f), gInvViewProj);
    return world.xyz / world.w;
}

float3 ApplyLight(float3 albedo, float3 normal, float3 viewDir, float3 lightDir, float3 lightColor, float intensity)
{
    float nDotL = saturate(dot(normal, lightDir));
    float3 halfVector = normalize(lightDir + viewDir);
    float specular = pow(saturate(dot(normal, halfVector)), 32.0f);
    return albedo * lightColor * nDotL * intensity + lightColor * specular * 0.25f * intensity;
}

float CascadeShadowFactor(float3 posW, float3 normalW, float3 lightDir, out int selectedCascade)
{
    const float viewDepth = mul(float4(posW, 1.0f), gView).z;
    int cascadeIndex = SHADOW_CASCADE_COUNT - 1;
    [unroll]
    for (int i = 0; i < SHADOW_CASCADE_COUNT; ++i)
    {
        if (viewDepth <= gCascades[i].SplitFar)
        {
            cascadeIndex = i;
            break;
        }
    }
    selectedCascade = cascadeIndex;

    const float4 lightPosition = mul(float4(posW, 1.0f), gCascades[cascadeIndex].ViewProj);
    const float3 ndc = lightPosition.xyz / lightPosition.w;
    const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    if (ndc.z <= 0.0f || ndc.z >= 1.0f || any(uv < 0.0f) || any(uv > 1.0f))
        return 1.0f;

    uint width;
    uint height;
    uint layers;
    gCascadeShadowMap.GetDimensions(width, height, layers);
    const float2 texelSize = 1.0f / float2(width, height);
    const float bias = max(0.00035f, 0.0015f * (1.0f - saturate(dot(normalW, lightDir))));
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            visibility += gCascadeShadowMap.SampleCmpLevelZero(gShadowSampler,
                float3(uv + float2(x, y) * texelSize, cascadeIndex), ndc.z - bias);
        }
    }
    return visibility / 9.0f;
}

float4 PS(FullscreenVertexOut pin) : SV_Target
{
    int3 coord = int3(pin.PosH.xy, 0);
    float4 albedo = gDiffuseMap.Load(coord);

    if (albedo.a <= 0.0f)
        discard;

    float depth = gDepthMap.Load(coord).r;
    uint width;
    uint height;
    gDepthMap.GetDimensions(width, height);
    float3 posW = ReconstructWorldPosition(pin.PosH.xy, depth, float2(width, height));
    float3 normal = normalize(gNormalMap.Load(coord).xyz);
    float3 viewDir = normalize(gEyePosW - posW);

    float3 color = albedo.rgb * gAmbientStrength;

    float3 directionalDir = normalize(-gDirectional.Direction);
    int cascadeIndex;
    const float shadow = CascadeShadowFactor(posW, normal, directionalDir, cascadeIndex);
    color += shadow * ApplyLight(albedo.rgb, normal, viewDir, directionalDir,
        gDirectional.Color, gDirectional.Intensity);

    if (gPad0.x > 0.5f)
    {
        const float3 cascadeColors[SHADOW_CASCADE_COUNT] =
        {
            float3(1.0f, 0.16f, 0.12f),
            float3(0.12f, 1.0f, 0.20f),
            float3(0.16f, 0.35f, 1.0f)
        };
        color = lerp(color, cascadeColors[cascadeIndex], 0.65f);
    }

    return float4(saturate(color), 1.0f);
}

LocalVertexOut LocalLightingVS(LocalVertexIn vin, uint instanceId : SV_InstanceID)
{
    LocalVertexOut vout;
    vout.LightType = instanceId < (uint)gPointLightCount ? 0 : 1;
    vout.LightIndex = vout.LightType == 0 ? (int)instanceId : (int)instanceId - gPointLightCount;

    float3 position;
    float radius;
    if (vout.LightType == 0)
    {
        position = gPointLights[vout.LightIndex].Position;
        radius = gPointLights[vout.LightIndex].Radius;
    }
    else
    {
        position = gSpotLights[vout.LightIndex].Position;
        radius = gSpotLights[vout.LightIndex].Radius;
    }

    float4 posW = float4(position + vin.PosL * radius, 1.0f);
    vout.PosH = mul(posW, gViewProj);
    return vout;
}

float4 ComputePointLight(int lightIndex, float3 albedo, float3 normal, float3 viewDir, float3 posW)
{
    float3 toLight = gPointLights[lightIndex].Position - posW;
    float distanceToLight = length(toLight);
    float attenuation = saturate(1.0f - distanceToLight / gPointLights[lightIndex].Radius);
    attenuation *= attenuation;

    if (attenuation <= 0.0f)
        discard;

    return float4(ApplyLight(albedo, normal, viewDir, toLight / max(distanceToLight, 0.001f),
        gPointLights[lightIndex].Color, gPointLights[lightIndex].Intensity * attenuation), 1.0f);
}

float4 ComputeSpotLight(int lightIndex, float3 albedo, float3 normal, float3 viewDir, float3 posW)
{
    float3 toLight = gSpotLights[lightIndex].Position - posW;
    float distanceToLight = length(toLight);
    float3 lightDir = toLight / max(distanceToLight, 0.001f);
    float cone = saturate(dot(-lightDir, normalize(gSpotLights[lightIndex].Direction)));
    float spotFactor = pow(cone, gSpotLights[lightIndex].SpotPower);
    float attenuation = saturate(1.0f - distanceToLight / gSpotLights[lightIndex].Radius);
    attenuation *= attenuation;

    if (attenuation <= 0.0f || spotFactor <= 0.001f)
        discard;

    return float4(ApplyLight(albedo, normal, viewDir, lightDir,
        gSpotLights[lightIndex].Color, gSpotLights[lightIndex].Intensity * attenuation * spotFactor), 1.0f);
}

float4 LocalLightingPS(LocalVertexOut pin) : SV_Target
{
    int3 coord = int3(pin.PosH.xy, 0);
    float4 albedo = gDiffuseMap.Load(coord);

    if (albedo.a <= 0.0f)
        discard;

    float depth = gDepthMap.Load(coord).r;
    uint width;
    uint height;
    gDepthMap.GetDimensions(width, height);
    float3 posW = ReconstructWorldPosition(pin.PosH.xy, depth, float2(width, height));
    float3 normal = normalize(gNormalMap.Load(coord).xyz);
    float3 viewDir = normalize(gEyePosW - posW);

    if (pin.LightType == 0)
    {
        return ComputePointLight(pin.LightIndex, albedo.rgb, normal, viewDir, posW);
    }

    return ComputeSpotLight(pin.LightIndex, albedo.rgb, normal, viewDir, posW);
}
