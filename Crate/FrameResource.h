#pragma once

#include "Common/d3dUtil.h"
#include "Common/MathHelper.h"
#include "Common/UploadBuffer.h"

constexpr int MaxPointLights = 8;
constexpr int MaxSpotLights = 4;

struct DirectionalLight
{
    DirectX::XMFLOAT3 Direction = { 0.35f, -1.0f, 0.25f };
    float Intensity = 0.75f;
    DirectX::XMFLOAT3 Color = { 1.0f, 0.96f, 0.88f };
    float Pad0 = 0.0f;
};

struct PointLight
{
    DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
    float Radius = 10.0f;
    DirectX::XMFLOAT3 Color = { 1.0f, 1.0f, 1.0f };
    float Intensity = 1.0f;
};

struct SpotLight
{
    DirectX::XMFLOAT3 Position = { 0.0f, 0.0f, 0.0f };
    float Radius = 18.0f;
    DirectX::XMFLOAT3 Direction = { 0.0f, -1.0f, 0.0f };
    float SpotPower = 24.0f;
    DirectX::XMFLOAT3 Color = { 1.0f, 1.0f, 1.0f };
    float Intensity = 1.0f;
};

struct LightConstants
{
    DirectX::XMFLOAT4X4 InvViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float AmbientStrength = 0.08f;
    DirectionalLight Directional;
    int PointLightCount = 0;
    int SpotLightCount = 0;
    DirectX::XMFLOAT2 Pad0 = { 0.0f, 0.0f };
    PointLight PointLights[MaxPointLights];
    SpotLight SpotLights[MaxSpotLights];
};

struct ObjectConstants
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
	DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
    int UseInstancing = 0;
    DirectX::XMFLOAT3 Pad = { 0.0f, 0.0f, 0.0f };
};

struct InstanceData
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
};

struct PassConstants
{
    DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvView = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 ViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 InvViewProj = MathHelper::Identity4x4();
    DirectX::XMFLOAT3 EyePosW = { 0.0f, 0.0f, 0.0f };
    float cbPerObjectPad1 = 0.0f;
    DirectX::XMFLOAT2 RenderTargetSize = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 InvRenderTargetSize = { 0.0f, 0.0f };
    float NearZ = 0.0f;
    float FarZ = 0.0f;
    float TotalTime = 0.0f;
    float DeltaTime = 0.0f;

};

struct Vertex
{
    DirectX::XMFLOAT3 Pos;
    DirectX::XMFLOAT3 Normal;
    // xyz is the tangent; w preserves the tangent-space handedness.
    DirectX::XMFLOAT4 TangentU;
	DirectX::XMFLOAT2 TexC;
};

// Stores the resources needed for the CPU to build the command lists
// for a frame.  
struct FrameResource
{
public:
    
    FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount, UINT materialCount,
        UINT instanceCount);
    FrameResource(const FrameResource& rhs) = delete;
    FrameResource& operator=(const FrameResource& rhs) = delete;
    ~FrameResource();

    // We cannot reset the allocator until the GPU is done processing the commands.
    // So each frame needs their own allocator.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CmdListAlloc;

    // We cannot update a cbuffer until the GPU is done processing the commands
    // that reference it.  So each frame needs their own cbuffers.
   // std::unique_ptr<UploadBuffer<FrameConstants>> FrameCB = nullptr;
    std::unique_ptr<UploadBuffer<PassConstants>> PassCB = nullptr;
    std::unique_ptr<UploadBuffer<MaterialConstants>> MaterialCB = nullptr;
    std::unique_ptr<UploadBuffer<ObjectConstants>> ObjectCB = nullptr;
    std::unique_ptr<UploadBuffer<InstanceData>> InstanceBuffer = nullptr;
    std::unique_ptr<UploadBuffer<LightConstants>> LightCB = nullptr;

    // Fence value to mark commands up to this fence point.  This lets us
    // check if these frame resources are still in use by the GPU.
    UINT64 Fence = 0;
};
