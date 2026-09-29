#pragma once

#include "Common/d3dUtil.h"
#include "Common/GBuffer.h"
#include "FrameResource.h"
#include "ParticleSystem.h"
#include "RenderScene.h"
#include "VisibilitySystem.h"

class RenderingSystem
{
public:
    struct BuildContext
    {
        ID3D12Device* Device = nullptr;
        const std::vector<D3D12_INPUT_ELEMENT_DESC>* GeometryInputLayout = nullptr;
        DXGI_FORMAT BackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT DepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        bool MsaaEnabled = false;
        UINT MsaaQuality = 0;
        GBuffer* GBufferTarget = nullptr;
        ID3D12GraphicsCommandList* CmdList = nullptr;
        ID3D12DescriptorHeap* SceneSrvHeap = nullptr;
        MeshGeometry* LocalLightVolumeGeo = nullptr;
        const D3D12_VIEWPORT* Viewport = nullptr;
        const D3D12_RECT* ScissorRect = nullptr;
        const DirectX::XMFLOAT4X4* View = nullptr;
        const DirectX::XMFLOAT4X4* Proj = nullptr;
        const DirectX::XMFLOAT3* DirectionalLightDirection = nullptr;
        const std::vector<std::unique_ptr<RenderItem>>* RenderItems = nullptr;
        UINT SceneSrvDescriptorSize = 0;
        float NearZ = 1.0f;
        float FarZ = 1000.0f;
    };

private:
    struct FrameContext
    {
        ID3D12GraphicsCommandList* CmdList = nullptr;
        GBuffer* GBufferTarget = nullptr;
        D3D12_VIEWPORT Viewport = {};
        D3D12_RECT ScissorRect = {};
        ID3D12Resource* BackBuffer = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE BackBufferView = {};
        ID3D12DescriptorHeap* SceneSrvHeap = nullptr;
        FrameResource* CurrFrameResource = nullptr;
        MeshGeometry* LocalLightVolumeGeo = nullptr;
        UINT LocalLightVolumeCount = 0;
        const std::vector<RenderItem*>* VisibleRenderItems = nullptr;
        const std::vector<RenderItem*>* ShadowRenderItems = nullptr;
        DirectX::XMFLOAT4X4 View = MathHelper::Identity4x4();
        DirectX::XMFLOAT4X4 Proj = MathHelper::Identity4x4();
        DirectX::XMFLOAT3 DirectionalLightDirection = { 0.0f, -1.0f, 0.0f };
        float NearZ = 1.0f;
        float FarZ = 1000.0f;
        UINT SceneSrvDescriptorSize = 0;
    };

public:
    struct RenderStats
    {
        UINT DrawCalls = 0;
        UINT ScatteredInstances = 0;
    };

    static constexpr UINT ScatterObjectCount = 1024;
    static constexpr UINT ShadowMapResolution = 1024;

    void Initialize(const BuildContext& context);
    void OnResize(UINT width, UINT height);

    RenderStats Render(FrameResource* frameResource, ID3D12Resource* backBuffer,
        D3D12_CPU_DESCRIPTOR_HANDLE backBufferView, UINT localLightVolumeCount);

    static void UpdateObjectConstants(FrameResource* frameResource,
        const std::vector<std::unique_ptr<RenderItem>>& items);
    static void UpdateMaterialConstants(FrameResource* frameResource,
        const std::unordered_map<std::string, std::unique_ptr<Material>>& materials);

    static void AppendScatteredBoxes(std::vector<std::unique_ptr<RenderItem>>& items,
        UINT& nextObjectIndex, MeshGeometry* geometry, const SubmeshGeometry& submesh,
        Material* material, const bool* visibility);

    ID3D12RootSignature* GeometryRootSignature() const { return _geometryRootSignature.Get(); }
    ID3D12PipelineState* GeometryPso() const { return _geometryPso.Get(); }
    VisibilitySystem& Visibility() { return _visibilitySystem; }
    bool& ParticlesEnabled() { return _particlesEnabled; }
    bool& ChromaticAberrationEnabled() { return _chromaticAberrationEnabled; }
    bool& VignetteEnabled() { return _vignetteEnabled; }
    int& GBufferDebugMode() { return _gBufferDebugMode; }
    float& ScatterSizeScale() { return _scatterSizeScale; }
    float& ScatterColorVariation() { return _scatterColorVariation; }
    bool& CascadeColorDebugEnabled() { return _cascadeColorDebugEnabled; }
    float& CameraMotionAmount() { return _cameraMotionAmount; }

private:
    RenderStats GeometryPass(const FrameContext& context);
    void ShadowPass(const FrameContext& context);
    RenderStats DrawRenderItems(const FrameContext& context, bool shadowPass) const;
    void DirectionalLightingPass(const FrameContext& context) const;
    void LocalLightingPass(const FrameContext& context) const;
    void PostProcessPass(const FrameContext& context) const;

    void BuildRootSignatures(ID3D12Device* device);
    void BuildShaders();
    void BuildPsOs(const BuildContext& context);
    void BuildShadowResources(ID3D12Device* device, GBuffer* gBuffer);
    void BuildPostProcessResources(ID3D12Device* device, UINT width, UINT height);
    void UpdateCascades(const FrameContext& context);
    static std::vector<CD3DX12_STATIC_SAMPLER_DESC> GetStaticSamplers();

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _geometryRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _lightingRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _shadowRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _postProcessRootSignature = nullptr;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> _geometryPso = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _directionalLightingPso = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _localLightingPso = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _shadowPso = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _postProcessPso = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource> _sceneColor = nullptr;
    D3D12_RESOURCE_STATES _sceneColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D12Resource> _cascadeShadowMap = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _shadowDsvHeap = nullptr;
    D3D12_VIEWPORT _shadowViewport = {};
    D3D12_RECT _shadowScissorRect = {};
    D3D12_RESOURCE_STATES _shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    UINT _shadowDsvHeapDescriptorSize = 0;
    std::array<CascadeConstants, ShadowCascadeCount> _cascades = {};

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> _shaders;
    BuildContext _context;
    VisibilitySystem _visibilitySystem;
    ParticleSystem _particleSystem;
    bool _particlesEnabled = true;
    bool _chromaticAberrationEnabled = false;
    bool _vignetteEnabled = false;
    int _gBufferDebugMode = 0;
    float _scatterSizeScale = 1.0f;
    float _scatterColorVariation = 0.65f;
    bool _cascadeColorDebugEnabled = false;
    float _cameraMotionAmount = 0.0f;
};
