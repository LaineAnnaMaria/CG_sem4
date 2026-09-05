#include "RenderingSystem.h"

using namespace DirectX;

void RenderingSystem::Initialize(const BuildContext& context)
{
    BuildRootSignatures(context.Device);
    BuildShaders();
    BuildPSOs(context);
}

void RenderingSystem::Render(const FrameContext& context,
    const std::function<void(ID3D12GraphicsCommandList*)>& drawGeometry) const
{
    auto cmdList = context.CmdList;

    cmdList->RSSetViewports(1, &context.Viewport);
    cmdList->RSSetScissorRects(1, &context.ScissorRect);

    GeometryPass(context, drawGeometry);

    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        context.BackBuffer,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET));

    cmdList->ClearRenderTargetView(context.BackBufferView, Colors::Black, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &context.BackBufferView, true, nullptr);

    DirectionalLightingPass(context);
    LocalLightingPass(context);

    cmdList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        context.BackBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT));
}

void RenderingSystem::GeometryPass(const FrameContext& context,
    const std::function<void(ID3D12GraphicsCommandList*)>& drawGeometry) const
{
    auto cmdList = context.CmdList;
    auto gBuffer = context.GBufferTarget;

    cmdList->SetPipelineState(_geometryPso.Get());
    gBuffer->ChangeRTVsState(D3D12_RESOURCE_STATE_RENDER_TARGET);
    gBuffer->ChangeDSVState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    gBuffer->ClearInfo(Colors::Transparent);

    auto rtvs = gBuffer->RTVs();
    auto dsv = gBuffer->DepthStencilView();
    cmdList->OMSetRenderTargets((UINT)rtvs.size(), rtvs.data(), false, &dsv);

    ID3D12DescriptorHeap* descriptorHeaps[] = { context.SceneSrvHeap };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
    cmdList->SetGraphicsRootSignature(_geometryRootSignature.Get());

    auto passCB = context.CurrFrameResource->PassCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

    drawGeometry(cmdList);
}

void RenderingSystem::DirectionalLightingPass(const FrameContext& context) const
{
    auto cmdList = context.CmdList;
    auto gBuffer = context.GBufferTarget;

    cmdList->SetPipelineState(_directionalLightingPso.Get());
    cmdList->SetGraphicsRootSignature(_lightingRootSignature.Get());

    ID3D12DescriptorHeap* descriptorHeaps[] = { gBuffer->SRVHeap() };
    cmdList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    gBuffer->ChangeRTVsState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gBuffer->ChangeDSVState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    CD3DX12_GPU_DESCRIPTOR_HANDLE gbufferSrv(gBuffer->SRVHeap()->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetGraphicsRootDescriptorTable(0, gbufferSrv);

    auto lightCB = context.CurrFrameResource->LightCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(1, lightCB->GetGPUVirtualAddress());

    cmdList->DrawInstanced(3, 1, 0, 0);
}

void RenderingSystem::LocalLightingPass(const FrameContext& context) const
{
    if (context.LocalLightVolumeGeo == nullptr || context.LocalLightVolumeCount == 0)
        return;

    auto cmdList = context.CmdList;
    auto geo = context.LocalLightVolumeGeo;

    cmdList->SetPipelineState(_localLightingPso.Get());
    cmdList->SetGraphicsRootSignature(_lightingRootSignature.Get());

    CD3DX12_GPU_DESCRIPTOR_HANDLE gbufferSrv(context.GBufferTarget->SRVHeap()->GetGPUDescriptorHandleForHeapStart());
    cmdList->SetGraphicsRootDescriptorTable(0, gbufferSrv);

    auto lightCB = context.CurrFrameResource->LightCB->Resource();
    auto passCB = context.CurrFrameResource->PassCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(1, lightCB->GetGPUVirtualAddress());
    cmdList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

    auto vertexBufferView = geo->VertexBufferView();
    auto indexBufferView = geo->IndexBufferView();
    cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
    cmdList->IASetIndexBuffer(&indexBufferView);
    cmdList->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const auto& box = geo->DrawArgs.at("box");
    cmdList->DrawIndexedInstanced(box.IndexCount, context.LocalLightVolumeCount,
        box.StartIndexLocation, box.BaseVertexLocation, 0);
}

void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
    {
        CD3DX12_DESCRIPTOR_RANGE texTable;
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER slotRootParameter[4];
        slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[1].InitAsConstantBufferView(0);
        slotRootParameter[2].InitAsConstantBufferView(1);
        slotRootParameter[3].InitAsConstantBufferView(2);

        auto staticSamplers = GetStaticSamplers();
        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
            (UINT)staticSamplers.size(), staticSamplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
            ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        ThrowIfFailed(hr);

        ThrowIfFailed(device->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(_geometryRootSignature.GetAddressOf())));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE gbufferTable;
        gbufferTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

        CD3DX12_ROOT_PARAMETER slotRootParameter[3];
        slotRootParameter[0].InitAsDescriptorTable(1, &gbufferTable, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[1].InitAsConstantBufferView(0);
        slotRootParameter[2].InitAsConstantBufferView(1);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
            0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
            ::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
        ThrowIfFailed(hr);

        ThrowIfFailed(device->CreateRootSignature(
            0,
            serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(_lightingRootSignature.GetAddressOf())));
    }
}

void RenderingSystem::BuildShaders()
{
    _shaders["geometryVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_0");
    _shaders["geometryPS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_0");
    _shaders["directionalVS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "VS", "vs_5_0");
    _shaders["directionalPS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "PS", "ps_5_0");
    _shaders["localLightingVS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "LocalLightingVS", "vs_5_0");
    _shaders["localLightingPS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "LocalLightingPS", "ps_5_0");
}

void RenderingSystem::BuildPSOs(const BuildContext& context)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPsoDesc = {};
    geometryPsoDesc.InputLayout = { context.GeometryInputLayout->data(), (UINT)context.GeometryInputLayout->size() };
    geometryPsoDesc.pRootSignature = _geometryRootSignature.Get();
    geometryPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(_shaders["geometryVS"]->GetBufferPointer()),
        _shaders["geometryVS"]->GetBufferSize()
    };
    geometryPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(_shaders["geometryPS"]->GetBufferPointer()),
        _shaders["geometryPS"]->GetBufferSize()
    };
    geometryPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geometryPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geometryPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geometryPsoDesc.SampleMask = UINT_MAX;
    geometryPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    geometryPsoDesc.NumRenderTargets = GBuffer::InfoCount();
    for (int i = 0; i < GBuffer::InfoCount(); i++)
        geometryPsoDesc.RTVFormats[i] = GBuffer::infoFormats[i];
    geometryPsoDesc.SampleDesc.Count = context.MsaaEnabled ? 4 : 1;
    geometryPsoDesc.SampleDesc.Quality = context.MsaaEnabled ? (context.MsaaQuality - 1) : 0;
    geometryPsoDesc.DSVFormat = context.DepthStencilFormat;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&geometryPsoDesc, IID_PPV_ARGS(&_geometryPso)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC directionalPsoDesc = geometryPsoDesc;
    directionalPsoDesc.InputLayout = {};
    directionalPsoDesc.pRootSignature = _lightingRootSignature.Get();
    directionalPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(_shaders["directionalVS"]->GetBufferPointer()),
        _shaders["directionalVS"]->GetBufferSize()
    };
    directionalPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(_shaders["directionalPS"]->GetBufferPointer()),
        _shaders["directionalPS"]->GetBufferSize()
    };
    directionalPsoDesc.NumRenderTargets = 1;
    for (int i = 0; i < 8; ++i)
        directionalPsoDesc.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
    directionalPsoDesc.RTVFormats[0] = context.BackBufferFormat;
    directionalPsoDesc.DepthStencilState.DepthEnable = false;
    directionalPsoDesc.DepthStencilState.StencilEnable = false;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&directionalPsoDesc, IID_PPV_ARGS(&_directionalLightingPso)));

    std::vector<D3D12_INPUT_ELEMENT_DESC> localLightInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC localPsoDesc = directionalPsoDesc;
    localPsoDesc.InputLayout = { localLightInputLayout.data(), (UINT)localLightInputLayout.size() };
    localPsoDesc.VS =
    {
        reinterpret_cast<BYTE*>(_shaders["localLightingVS"]->GetBufferPointer()),
        _shaders["localLightingVS"]->GetBufferSize()
    };
    localPsoDesc.PS =
    {
        reinterpret_cast<BYTE*>(_shaders["localLightingPS"]->GetBufferPointer()),
        _shaders["localLightingPS"]->GetBufferSize()
    };
    localPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    localPsoDesc.BlendState.RenderTarget[0].BlendEnable = true;
    localPsoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    localPsoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
    localPsoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    localPsoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    localPsoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    localPsoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    localPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&localPsoDesc, IID_PPV_ARGS(&_localLightingPso)));
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> RenderingSystem::GetStaticSamplers() const
{
    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1, D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4, D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        0.0f,
        8);

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5, D3D12_FILTER_ANISOTROPIC,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0.0f,
        8);

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp };
}
