#include "ParticleSystem.h"

#include "Common/GBuffer.h"

using Microsoft::WRL::ComPtr;

namespace
{
    void CreateRootSignature(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
        ID3D12RootSignature** rootSignature)
    {
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
            serialized.GetAddressOf(), errors.GetAddressOf());
        if (errors != nullptr)
            ::OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(rootSignature)));
    }
}

void ParticleSystem::Initialize(const BuildContext& context)
{
    if (context.Device == nullptr || context.CmdList == nullptr)
        throw std::runtime_error("ParticleSystem requires a device and an open command list.");

    BuildRootSignatures(context.Device);
    BuildPipelineStates(context);
    BuildBuffers(context);
}

void ParticleSystem::BuildRootSignatures(ID3D12Device* device)
{
    CD3DX12_ROOT_PARAMETER resetCounterParameter;
    resetCounterParameter.InitAsUnorderedAccessView(0);
    CD3DX12_ROOT_SIGNATURE_DESC resetCounterDesc(1, &resetCounterParameter, 0, nullptr);
    CreateRootSignature(device, resetCounterDesc, _resetCounterRootSignature.GetAddressOf());

    CD3DX12_DESCRIPTOR_RANGE initializeUavTable;
    initializeUavTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    CD3DX12_ROOT_PARAMETER initializeParameter;
    initializeParameter.InitAsDescriptorTable(1, &initializeUavTable);
    CD3DX12_ROOT_SIGNATURE_DESC initializeDesc(1, &initializeParameter, 0, nullptr);
    CreateRootSignature(device, initializeDesc, _initializeRootSignature.GetAddressOf());

    CD3DX12_DESCRIPTOR_RANGE uavTable;
    uavTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0);

    CD3DX12_ROOT_PARAMETER computeParameters[2];
    computeParameters[0].InitAsDescriptorTable(1, &uavTable);
    computeParameters[1].InitAsConstantBufferView(0);
    CD3DX12_ROOT_SIGNATURE_DESC computeDesc(2, computeParameters, 0, nullptr);
    CreateRootSignature(device, computeDesc, _computeRootSignature.GetAddressOf());

    CD3DX12_ROOT_PARAMETER drawParameters[2];
    drawParameters[0].InitAsShaderResourceView(0);
    drawParameters[1].InitAsConstantBufferView(0);
    CD3DX12_ROOT_SIGNATURE_DESC drawDesc(2, drawParameters, 0, nullptr,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    CreateRootSignature(device, drawDesc, _drawRootSignature.GetAddressOf());
}

void ParticleSystem::BuildPipelineStates(const BuildContext& context)
{
    const D3D_SHADER_MACRO resetCounterDefines[] =
    {
        { "PARTICLE_RESET_COUNTER", "1" },
        { nullptr, nullptr }
    };
    const D3D_SHADER_MACRO initializeDefines[] =
    {
        { "PARTICLE_INITIALIZE", "1" },
        { nullptr, nullptr }
    };
    const ComPtr<ID3DBlob> resetCounterShader = d3dUtil::CompileShader(
        L"Shaders\\Particle.hlsl", resetCounterDefines, "ResetCounterCS", "cs_5_0");
    const ComPtr<ID3DBlob> initializeShader = d3dUtil::CompileShader(
        L"Shaders\\Particle.hlsl", initializeDefines, "InitializeCS", "cs_5_0");
    const ComPtr<ID3DBlob> computeShader =
        d3dUtil::CompileShader(L"Shaders\\Particle.hlsl", nullptr, "CS", "cs_5_0");
    const ComPtr<ID3DBlob> vertexShader =
        d3dUtil::CompileShader(L"Shaders\\Particle.hlsl", nullptr, "VS", "vs_5_0");
    const ComPtr<ID3DBlob> geometryShader =
        d3dUtil::CompileShader(L"Shaders\\Particle.hlsl", nullptr, "GS", "gs_5_0");
    const ComPtr<ID3DBlob> pixelShader =
        d3dUtil::CompileShader(L"Shaders\\Particle.hlsl", nullptr, "PS", "ps_5_0");

    D3D12_COMPUTE_PIPELINE_STATE_DESC resetCounterPsoDesc = {};
    resetCounterPsoDesc.pRootSignature = _resetCounterRootSignature.Get();
    resetCounterPsoDesc.CS =
    {
        resetCounterShader->GetBufferPointer(), resetCounterShader->GetBufferSize()
    };
    ThrowIfFailed(context.Device->CreateComputePipelineState(&resetCounterPsoDesc,
        IID_PPV_ARGS(_resetCounterPso.GetAddressOf())));

    D3D12_COMPUTE_PIPELINE_STATE_DESC initializePsoDesc = {};
    initializePsoDesc.pRootSignature = _initializeRootSignature.Get();
    initializePsoDesc.CS =
    {
        initializeShader->GetBufferPointer(), initializeShader->GetBufferSize()
    };
    ThrowIfFailed(context.Device->CreateComputePipelineState(&initializePsoDesc,
        IID_PPV_ARGS(_initializePso.GetAddressOf())));

    D3D12_COMPUTE_PIPELINE_STATE_DESC computeDesc = {};
    computeDesc.pRootSignature = _computeRootSignature.Get();
    computeDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };
    ThrowIfFailed(context.Device->CreateComputePipelineState(&computeDesc,
        IID_PPV_ARGS(_computePso.GetAddressOf())));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC drawDesc = {};
    drawDesc.pRootSignature = _drawRootSignature.Get();
    drawDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    drawDesc.GS = { geometryShader->GetBufferPointer(), geometryShader->GetBufferSize() };
    drawDesc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    drawDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    drawDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    drawDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    drawDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    drawDesc.SampleMask = UINT_MAX;
    drawDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    drawDesc.NumRenderTargets = GBuffer::InfoCount();
    for (int i = 0; i < GBuffer::InfoCount(); ++i)
        drawDesc.RTVFormats[i] = GBuffer::infoFormats[i];
    drawDesc.DSVFormat = context.DepthStencilFormat;
    drawDesc.SampleDesc.Count = context.MsaaEnabled ? 4 : 1;
    drawDesc.SampleDesc.Quality = context.MsaaEnabled ? context.MsaaQuality - 1 : 0;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&drawDesc,
        IID_PPV_ARGS(_drawPso.GetAddressOf())));
}

void ParticleSystem::BuildBuffers(const BuildContext& context)
{
    constexpr UINT64 particleBufferSize = static_cast<UINT64>(ParticleCount) * ParticleStride;

    const CD3DX12_HEAP_PROPERTIES defaultHeap(D3D12_HEAP_TYPE_DEFAULT);
    const auto particleDesc = CD3DX12_RESOURCE_DESC::Buffer(particleBufferSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    const auto counterDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(UINT),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    for (UINT i = 0; i < 2; ++i)
    {
        ThrowIfFailed(context.Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &particleDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(_buffers[i].GetAddressOf())));
        ThrowIfFailed(context.Device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE,
            &counterDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(_counters[i].GetAddressOf())));
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 4;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(context.Device->CreateDescriptorHeap(&heapDesc,
        IID_PPV_ARGS(_uavHeap.GetAddressOf())));
    _descriptorSize = context.Device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = ParticleCount;
    uavDesc.Buffer.StructureByteStride = ParticleStride;
    uavDesc.Buffer.CounterOffsetInBytes = 0;

    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(_uavHeap->GetCPUDescriptorHandleForHeapStart());
    context.Device->CreateUnorderedAccessView(_buffers[0].Get(), _counters[0].Get(),
        &uavDesc, handle);
    handle.Offset(1, _descriptorSize);
    context.Device->CreateUnorderedAccessView(_buffers[1].Get(), _counters[1].Get(),
        &uavDesc, handle);
    handle.Offset(1, _descriptorSize);
    context.Device->CreateUnorderedAccessView(_buffers[1].Get(), _counters[1].Get(),
        &uavDesc, handle);
    handle.Offset(1, _descriptorSize);
    context.Device->CreateUnorderedAccessView(_buffers[0].Get(), _counters[0].Get(),
        &uavDesc, handle);

    ResetCounterOnGpu(context.CmdList, 0);
    ResetCounterOnGpu(context.CmdList, 1);
    InitializeParticlesOnGpu(context.CmdList);
}

void ParticleSystem::ResetCounterOnGpu(ID3D12GraphicsCommandList* cmdList,
    UINT bufferIndex) const
{
    cmdList->SetPipelineState(_resetCounterPso.Get());
    cmdList->SetComputeRootSignature(_resetCounterRootSignature.Get());
    cmdList->SetComputeRootUnorderedAccessView(0,
        _counters[bufferIndex]->GetGPUVirtualAddress());
    cmdList->Dispatch(1, 1, 1);

    const auto counterBarrier = CD3DX12_RESOURCE_BARRIER::UAV(_counters[bufferIndex].Get());
    cmdList->ResourceBarrier(1, &counterBarrier);
}

void ParticleSystem::InitializeParticlesOnGpu(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->SetPipelineState(_initializePso.Get());
    cmdList->SetComputeRootSignature(_initializeRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { _uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    cmdList->SetComputeRootDescriptorTable(0,
        _uavHeap->GetGPUDescriptorHandleForHeapStart());
    cmdList->Dispatch(ParticleCount / 64, 1, 1);

    const CD3DX12_RESOURCE_BARRIER barriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(_buffers[0].Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(_counters[0].Get())
    };
    cmdList->ResourceBarrier(_countof(barriers), barriers);
}

void ParticleSystem::Simulate(ID3D12GraphicsCommandList* cmdList,
    D3D12_GPU_VIRTUAL_ADDRESS passConstants)
{
    if (_bufferStates[_consumeIndex] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        const auto toUav = CD3DX12_RESOURCE_BARRIER::Transition(_buffers[_consumeIndex].Get(),
            _bufferStates[_consumeIndex], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmdList->ResourceBarrier(1, &toUav);
        _bufferStates[_consumeIndex] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    ResetCounterOnGpu(cmdList, _appendIndex);

    cmdList->SetPipelineState(_computePso.Get());
    cmdList->SetComputeRootSignature(_computeRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { _uavHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    CD3DX12_GPU_DESCRIPTOR_HANDLE table(_uavHeap->GetGPUDescriptorHandleForHeapStart(),
        _consumeIndex == 0 ? 0 : 2, _descriptorSize);
    cmdList->SetComputeRootDescriptorTable(0, table);
    cmdList->SetComputeRootConstantBufferView(1, passConstants);
    cmdList->Dispatch(ParticleCount / 64, 1, 1);

    const CD3DX12_RESOURCE_BARRIER uavBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::UAV(_buffers[_consumeIndex].Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(_buffers[_appendIndex].Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(_counters[_consumeIndex].Get()),
        CD3DX12_RESOURCE_BARRIER::UAV(_counters[_appendIndex].Get())
    };
    cmdList->ResourceBarrier(_countof(uavBarriers), uavBarriers);

    const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(_buffers[_appendIndex].Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toShaderResource);
    _bufferStates[_appendIndex] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::swap(_consumeIndex, _appendIndex);
}

void ParticleSystem::Draw(ID3D12GraphicsCommandList* cmdList,
    D3D12_GPU_VIRTUAL_ADDRESS passConstants) const
{
    cmdList->SetPipelineState(_drawPso.Get());
    cmdList->SetGraphicsRootSignature(_drawRootSignature.Get());
    cmdList->SetGraphicsRootShaderResourceView(0,
        _buffers[_consumeIndex]->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootConstantBufferView(1, passConstants);
    cmdList->IASetVertexBuffers(0, 0, nullptr);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    cmdList->DrawInstanced(ParticleCount, 1, 0, 0);
}
