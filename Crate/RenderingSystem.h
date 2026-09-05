#pragma once

#include <functional>

#include "Common/d3dUtil.h"
#include "Common/GBuffer.h"
#include "FrameResource.h"

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
    };

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
    };

    void Initialize(const BuildContext& context);

    void Render(const FrameContext& context,
        const std::function<void(ID3D12GraphicsCommandList*)>& drawGeometry) const;

    ID3D12RootSignature* GeometryRootSignature() const { return _geometryRootSignature.Get(); }
    ID3D12PipelineState* GeometryPso() const { return _geometryPso.Get(); }

private:
    void GeometryPass(const FrameContext& context,
        const std::function<void(ID3D12GraphicsCommandList*)>& drawGeometry) const;
    void DirectionalLightingPass(const FrameContext& context) const;
    void LocalLightingPass(const FrameContext& context) const;

    void BuildRootSignatures(ID3D12Device* device);
    void BuildShaders();
    void BuildPsOs(const BuildContext& context);
    static std::vector<CD3DX12_STATIC_SAMPLER_DESC> GetStaticSamplers();

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _geometryRootSignature = nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _lightingRootSignature = nullptr;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> _geometryPso = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _directionalLightingPso = nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _localLightingPso = nullptr;

    std::unordered_map<std::string, Microsoft::WRL::ComPtr<ID3DBlob>> _shaders;
};
