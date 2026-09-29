struct FullscreenVertexOut
{
    float4 PosH : SV_POSITION;
};

// A fullscreen triangle covers the viewport without a vertex or index buffer.
FullscreenVertexOut FullscreenVS(uint vertexId : SV_VertexID)
{
    FullscreenVertexOut output;
    const float2 positions[3] =
    {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };
    output.PosH = float4(positions[vertexId], 0.0f, 1.0f);
    return output;
}
