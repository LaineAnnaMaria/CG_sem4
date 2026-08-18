Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDepthMap : register(t2);

cbuffer cbDirLight : register(b0)
{
    float3 direction;
    float pad;
}

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

VertexOut VS(uint id : SV_VertexID)
{
    VertexOut vout;
    
    float2 positions[3] = { float2(-1, -1), float2(-1, 3), float2(3, -1) };
    
    vout.PosH = float4(positions[id], 0, 1);

    
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    
    int3 coord = pin.PosH.xyz;
    float3 normal = gNormalMap.Load(coord);
    float3 color = gDiffuseMap.Load(coord);
    float light = max(dot(normal, normalize(direction)), 0);
    float3 litColor = color * light;
    return float4(litColor, 1.f);

}