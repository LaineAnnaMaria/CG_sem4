struct Particle
{
    float3 Position;
    float Age;
    float3 Velocity;
    float Lifetime;
    float4 Color;
    float Size;
    uint Seed;
    float2 Pad;
};

cbuffer cbPass : register(b0)
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

uint Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return value;
}

float Random01(inout uint state)
{
    state = Hash(state);
    return (state & 0x00ffffff) / 16777216.0f;
}

#if defined(PARTICLE_RESET_COUNTER)
RWByteAddressBuffer gCounter : register(u0);

[numthreads(1, 1, 1)]
void ResetCounterCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    gCounter.Store(0, 0);
}
#elif defined(PARTICLE_INITIALIZE)
AppendStructuredBuffer<Particle> gInitializeParticles : register(u0);

[numthreads(64, 1, 1)]
void InitializeCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particleIndex = dispatchThreadId.x;
    const float u = particleIndex / 1024.0f;
    const float angle = u * 6.28318530718f * 17.0f;
    const float radialSpeed = 1.5f + 2.0f * ((particleIndex * 37u) % 101u) / 100.0f;

    Particle particle;
    particle.Lifetime = 3.5f + 2.5f * ((particleIndex * 61u) % 97u) / 96.0f;
    particle.Age = particle.Lifetime * u;
    const float3 initialVelocity = float3(
        cos(angle) * radialSpeed,
        8.0f + 4.0f * ((particleIndex * 29u) % 89u) / 88.0f,
        sin(angle) * radialSpeed);
    particle.Position = float3(0.0f, -3.5f, 0.0f) + initialVelocity * particle.Age;
    particle.Position.y += 0.5f * -5.5f * particle.Age * particle.Age;
    particle.Velocity = initialVelocity;
    particle.Velocity.y += -5.5f * particle.Age;
    const float colorPhase = ((particleIndex * 43u) % 113u) / 112.0f;
    particle.Color = float4(0.95f, 0.25f + 0.55f * colorPhase,
        0.06f + 0.16f * (1.0f - colorPhase), 1.0f);
    particle.Size = 0.55f + 0.65f * ((particleIndex * 17u) % 79u) / 78.0f;
    particle.Seed = particleIndex * 747796405u + 2891336453u;
    particle.Pad = 0.0f;
    gInitializeParticles.Append(particle);
}
#else
ConsumeStructuredBuffer<Particle> gConsumeParticles : register(u0);
AppendStructuredBuffer<Particle> gAppendParticles : register(u1);

[numthreads(64, 1, 1)]
void CS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    Particle particle = gConsumeParticles.Consume();
    const float deltaTime = min(gDeltaTime, 0.05f);
    particle.Age += deltaTime;
    particle.Velocity.y -= 5.5f * deltaTime;
    particle.Position += particle.Velocity * deltaTime;

    if (particle.Age >= particle.Lifetime || particle.Position.y < -5.0f)
    {
        uint randomState = Hash(particle.Seed + asuint(gTotalTime) + dispatchThreadId.x);
        const float angle = Random01(randomState) * 6.28318530718f;
        const float radialSpeed = lerp(1.5f, 3.5f, Random01(randomState));
        particle.Position = float3(0.0f, -3.5f, 0.0f);
        particle.Velocity = float3(cos(angle) * radialSpeed,
            lerp(8.0f, 12.0f, Random01(randomState)), sin(angle) * radialSpeed);
        particle.Age = 0.0f;
        particle.Lifetime = lerp(3.5f, 6.0f, Random01(randomState));
        const float heat = Random01(randomState);
        particle.Color = float4(0.95f, lerp(0.25f, 0.8f, heat),
            lerp(0.22f, 0.06f, heat), 1.0f);
        particle.Size = lerp(0.55f, 1.2f, Random01(randomState));
        particle.Seed = randomState;
    }

    gAppendParticles.Append(particle);
}
#endif

StructuredBuffer<Particle> gParticles : register(t0);

struct VertexOut
{
    float3 PositionW : POSITION;
    float4 Color : COLOR;
    float Size : SIZE;
};

VertexOut VS(uint vertexId : SV_VertexID)
{
    const Particle particle = gParticles[vertexId];
    VertexOut output;
    output.PositionW = particle.Position;
    output.Color = particle.Color;
    output.Size = particle.Size;
    return output;
}

struct GeometryOut
{
    float4 PositionH : SV_POSITION;
    float3 NormalW : NORMAL;
    float4 Color : COLOR;
};

[maxvertexcount(36)]
void GS(point VertexOut input[1], inout TriangleStream<GeometryOut> outputStream)
{
    const float3 center = input[0].PositionW;
    const float halfSize = 0.5f * input[0].Size;
    const float3 corners[8] =
    {
        center + float3(-halfSize, -halfSize, -halfSize),
        center + float3(-halfSize,  halfSize, -halfSize),
        center + float3( halfSize,  halfSize, -halfSize),
        center + float3( halfSize, -halfSize, -halfSize),
        center + float3(-halfSize, -halfSize,  halfSize),
        center + float3(-halfSize,  halfSize,  halfSize),
        center + float3( halfSize,  halfSize,  halfSize),
        center + float3( halfSize, -halfSize,  halfSize)
    };
    const uint faceIndices[36] =
    {
        0, 1, 2, 0, 2, 3, // -Z
        7, 6, 5, 7, 5, 4, // +Z
        4, 5, 1, 4, 1, 0, // -X
        3, 2, 6, 3, 6, 7, // +X
        1, 5, 6, 1, 6, 2, // +Y
        4, 0, 3, 4, 3, 7  // -Y
    };
    const float3 faceNormals[6] =
    {
        float3(0.0f, 0.0f, -1.0f), float3(0.0f, 0.0f, 1.0f),
        float3(-1.0f, 0.0f, 0.0f), float3(1.0f, 0.0f, 0.0f),
        float3(0.0f, 1.0f, 0.0f), float3(0.0f, -1.0f, 0.0f)
    };

    [unroll]
    for (uint face = 0; face < 6; ++face)
    {
        [unroll]
        for (uint corner = 0; corner < 6; ++corner)
        {
            GeometryOut output;
            output.PositionH = mul(float4(corners[faceIndices[face * 6 + corner]], 1.0f), gViewProj);
            output.NormalW = faceNormals[face];
            output.Color = input[0].Color;
            outputStream.Append(output);
            if (corner == 2 || corner == 5)
                outputStream.RestartStrip();
        }
    }
}

struct GBufferOutput
{
    float4 Diffuse : SV_Target0;
    float4 Normal : SV_Target1;
};

GBufferOutput PS(GeometryOut input)
{
    GBufferOutput output;
    output.Diffuse = float4(input.Color.rgb, 1.0f);
    output.Normal = float4(normalize(input.NormalW), 1.0f);
    return output;
}
