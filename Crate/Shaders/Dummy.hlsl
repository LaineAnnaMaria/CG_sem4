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
    return float4(1.f, 0.5f, 0.f, 1.f);
}