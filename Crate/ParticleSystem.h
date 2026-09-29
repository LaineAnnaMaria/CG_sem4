#pragma once

#include "Common/d3dUtil.h"

class ParticleSystem
{
public:
    static constexpr UINT ParticleCount = 1024;

    struct BuildContext
    {
        ID3D12Device* Device = nullptr;
        ID3D12GraphicsCommandList* CmdList = nullptr;
        DXGI_FORMAT DepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        bool MsaaEnabled = false;
        UINT MsaaQuality = 0;
    };

    void Initialize(const BuildContext& context);
    void Simulate(ID3D12GraphicsCommandList* cmdList,
        D3D12_GPU_VIRTUAL_ADDRESS passConstants);
    void Draw(ID3D12GraphicsCommandList* cmdList,
        D3D12_GPU_VIRTUAL_ADDRESS passConstants) const;

private:
    static constexpr UINT ParticleStride = 64;

    void BuildRootSignatures(ID3D12Device* device);
    void BuildPipelineStates(const BuildContext& context);
    void BuildBuffers(const BuildContext& context);
    void ResetCounterOnGpu(ID3D12GraphicsCommandList* cmdList, UINT bufferIndex) const;
    void InitializeParticlesOnGpu(ID3D12GraphicsCommandList* cmdList);

    Microsoft::WRL::ComPtr<ID3D12RootSignature> _resetCounterRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _initializeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _computeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> _drawRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _resetCounterPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _initializePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _computePso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> _drawPso;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _uavHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> _buffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> _counters[2];
    D3D12_RESOURCE_STATES _bufferStates[2] =
    {
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    };
    UINT _consumeIndex = 0;
    UINT _appendIndex = 1;
    UINT _descriptorSize = 0;
};
