#include "Fullscreen.hlsl"

Texture2D gSceneColor : register(t0);
SamplerState gLinearClamp : register(s0);

cbuffer PostProcessSettings : register(b0)
{
    uint gChromaticAberrationEnabled;
    uint gVignetteEnabled;
};

float4 PS(FullscreenVertexOut input) : SV_Target
{
    uint width;
    uint height;
    gSceneColor.GetDimensions(width, height);

    const float2 uv = input.PosH.xy / float2(width, height);
    const float2 fromCenter = uv - 0.5f;

    float3 color;
    if (gChromaticAberrationEnabled != 0)
    {
        // Separate red and blue increasingly toward the image border.
        const float radialAmount = dot(fromCenter, fromCenter) * 0.045f;
        const float2 offset = fromCenter * radialAmount;
        color.r = gSceneColor.SampleLevel(gLinearClamp, uv + offset, 0).r;
        color.g = gSceneColor.SampleLevel(gLinearClamp, uv, 0).g;
        color.b = gSceneColor.SampleLevel(gLinearClamp, uv - offset, 0).b;
    }
    else
    {
        color = gSceneColor.SampleLevel(gLinearClamp, uv, 0).rgb;
    }

    if (gVignetteEnabled != 0)
    {
        float2 centered = uv * 2.0f - 1.0f;
        centered.x *= width / height;
        const float edgeDistance = length(centered);
        const float vignette = smoothstep(1.15f, 0.35f, edgeDistance);
        color *= lerp(1.0f, vignette, 0.72f);
    }

    return float4(color, 1.0f);
}
