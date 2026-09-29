cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gTexTransform;
    int gUseInstancing;
    float3 gObjectPad;
};

cbuffer cbShadow : register(b1)
{
    float4x4 gShadowViewProj;
};

struct InstanceData
{
    float4x4 World;
    float4 Color;
    float Size;
    float3 Pad;
};

StructuredBuffer<InstanceData> gInstanceData : register(t0);

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float4 TangentL : TANGENT;
    float2 TexC : TEXCOORD;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

VertexOut VS(VertexIn input, uint instanceId : SV_InstanceID)
{
    VertexOut output;
    const float4x4 world = gUseInstancing != 0 ? gInstanceData[instanceId].World : gWorld;
    const float size = gUseInstancing != 0 ? gInstanceData[instanceId].Size : 1.0f;
    output.PosH = mul(mul(float4(input.PosL * size, 1.0f), world), gShadowViewProj);
    return output;
}
