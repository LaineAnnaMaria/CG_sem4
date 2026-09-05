// Tessellated G-buffer pass with displacement and tangent-space normal mapping.

struct GBuffer
{
    float4 diffuse : SV_Target0;
    float4 normal  : SV_Target1;
};

Texture2D gDiffuseMap      : register(t0);
Texture2D gNormalMap       : register(t1);
Texture2D gDisplacementMap : register(t2);

SamplerState pointWrap        : register(s0);
SamplerState pointClamp       : register(s1);
SamplerState linearWrap       : register(s2);
SamplerState linearClamp      : register(s3);
SamplerState anisotropicWrap  : register(s4);
SamplerState anisotropicClamp : register(s5);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    int gUseInstancing;
    float3 gObjectPad;
};

struct InstanceData
{
    float4x4 World;
};

StructuredBuffer<InstanceData> gInstanceData : register(t3);

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
};

cbuffer cbMaterial : register(b2)
{
    float4 gDiffuseAlbedo;
    float3 gFresnelR0;
    float gRoughness;
    float4x4 gMatTransform;
    float gDisplacementScale;
    float gMinTessDistance;
    float gMaxTessDistance;
    float gMinTessFactor;
    float gMaxTessFactor;
    int gUseNormalMap;
    int gUseDisplacementMap;
    float gMaterialPad0;
};

struct VertexIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float4 TangentL : TANGENT;
    float2 TexC     : TEXCOORD;
};

struct HullIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float4 TangentL : TANGENT;
    float2 TexC     : TEXCOORD;
    nointerpolation uint InstanceId : INSTANCEID;
};

struct HullPatchConstants
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess  : SV_InsideTessFactor;
};

struct DomainOut
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float4 TangentW : TANGENT;
    float2 TexC     : TEXCOORD;
};

HullIn VS(VertexIn vin, uint instanceId : SV_InstanceID)
{
    HullIn output;
    output.PosL = vin.PosL;
    output.NormalL = vin.NormalL;
    output.TangentL = vin.TangentL;
    output.InstanceId = instanceId;

    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    output.TexC = mul(texC, gMatTransform).xy;
    return output;
}

float4x4 WorldMatrix(uint instanceId)
{
    return gUseInstancing != 0 ? gInstanceData[instanceId].World : gWorld;
}

float TessFactorForEdge(float3 aL, float3 bL, uint instanceId)
{
    // Both triangles sharing an edge calculate the same factor from its midpoint.
    float3 midpointW = mul(float4(0.5f * (aL + bL), 1.0f), WorldMatrix(instanceId)).xyz;
    float distanceToCamera = distance(midpointW, gEyePosW);
    float range = max(gMaxTessDistance - gMinTessDistance, 0.001f);
    float falloff = saturate((distanceToCamera - gMinTessDistance) / range);
    return lerp(gMaxTessFactor, gMinTessFactor, falloff);
}

HullPatchConstants PatchHS(InputPatch<HullIn, 3> patch, uint patchId : SV_PrimitiveID)
{
    HullPatchConstants output;
    const uint instanceId = patch[0].InstanceId;
    output.EdgeTess[0] = TessFactorForEdge(patch[1].PosL, patch[2].PosL, instanceId);
    output.EdgeTess[1] = TessFactorForEdge(patch[2].PosL, patch[0].PosL, instanceId);
    output.EdgeTess[2] = TessFactorForEdge(patch[0].PosL, patch[1].PosL, instanceId);
    output.InsideTess = (output.EdgeTess[0] + output.EdgeTess[1] + output.EdgeTess[2]) / 3.0f;
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchHS")]
[maxtessfactor(64.0f)]
HullIn HS(InputPatch<HullIn, 3> patch, uint controlPointId : SV_OutputControlPointID,
    uint patchId : SV_PrimitiveID)
{
    return patch[controlPointId];
}

[domain("tri")]
DomainOut DS(HullPatchConstants patchConstants, float3 bary : SV_DomainLocation,
    const OutputPatch<HullIn, 3> patch)
{
    DomainOut output;

    float3 posL = bary.x * patch[0].PosL + bary.y * patch[1].PosL + bary.z * patch[2].PosL;
    float3 normalL = normalize(bary.x * patch[0].NormalL + bary.y * patch[1].NormalL + bary.z * patch[2].NormalL);
    float4 tangentL = bary.x * patch[0].TangentL + bary.y * patch[1].TangentL + bary.z * patch[2].TangentL;
    tangentL.xyz = normalize(tangentL.xyz);
    output.TexC = bary.x * patch[0].TexC + bary.y * patch[1].TexC + bary.z * patch[2].TexC;

    if (gUseDisplacementMap != 0)
    {
        float height = gDisplacementMap.SampleLevel(linearWrap, output.TexC, 0.0f).r;
        // This height map is unsigned: black is the original surface and white
        // is maximum outward displacement. Centering around 0.5 pushed most of
        // the walnut inward because its map is predominantly below mid-gray.
        posL += normalL * (height * gDisplacementScale);
    }

    const float4x4 world = WorldMatrix(patch[0].InstanceId);
    float4 posW = mul(float4(posL, 1.0f), world);
    output.PosW = posW.xyz;
    output.PosH = mul(posW, gViewProj);
    output.NormalW = normalize(mul(normalL, (float3x3)world));
    output.TangentW = float4(normalize(mul(tangentL.xyz, (float3x3)world)), tangentL.w);
    return output;
}

GBuffer PS(DomainOut pin)
{
    GBuffer output;
    output.diffuse = gDiffuseAlbedo * gDiffuseMap.Sample(anisotropicWrap, pin.TexC);

    float3 normalW = normalize(pin.NormalW);
    if (gUseNormalMap != 0)
    {
        float3 tangentW = normalize(pin.TangentW.xyz - dot(pin.TangentW.xyz, normalW) * normalW);
        float3 bitangentW = normalize(cross(normalW, tangentW)) * (pin.TangentW.w < 0.0f ? -1.0f : 1.0f);
        float3 normalT = gNormalMap.Sample(anisotropicWrap, pin.TexC).xyz * 2.0f - 1.0f;
        normalW = normalize(normalT.x * tangentW + normalT.y * bitangentW + normalT.z * normalW);
    }

    output.normal = float4(normalW, 1.0f);
    return output;
}
