#include "RenderingSystem.h"

using namespace DirectX;

void RenderingSystem::Initialize(const BuildContext& context)
{
    BuildRootSignatures(context.Device);
    BuildShaders();
    BuildPsOs(context);
}

void RenderingSystem::Render(const FrameContext& context,
    const std::function<void(ID3D12GraphicsCommandList*)>& drawGeometry) const
{
    const auto cmdList = context.CmdList;

    cmdList->RSSetViewports(1, &context.Viewport);
    cmdList->RSSetScissorRects(1, &context.ScissorRect);

    GeometryPass(context, drawGeometry);

    const auto resourceBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
        context.BackBuffer,
        D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    cmdList->ResourceBarrier(1, &resourceBarrier);

    cmdList->ClearRenderTargetView(context.BackBufferView, Colors::Black, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &context.BackBufferView, true, nullptr);

    DirectionalLightingPass(context);
    LocalLightingPass(context);

    const auto fromRtToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        context.BackBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);

    cmdList->ResourceBarrier(1, &fromRtToPresent);
}

void RenderingSystem::GeometryPass(const FrameContext& context,
    const std::function<void(ID3D12GraphicsCommandList*)>& drawGeometry) const
{
    const auto cmdList = context.CmdList;
    const auto buffer = context.GBufferTarget;

    cmdList->SetPipelineState(_geometryPso.Get());
    buffer->ChangeRTVsState(D3D12_RESOURCE_STATE_RENDER_TARGET);
    buffer->ChangeDSVState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    buffer->ClearInfo(Colors::Transparent);

    const auto rtvs = buffer->RTVs();
    const auto dsv = buffer->DepthStencilView();
    cmdList->OMSetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), false, &dsv);

    ID3D12DescriptorHeap* descriptorHeaps[] = { context.SceneSrvHeap };
    cmdList->SetDescriptorHeaps(1, descriptorHeaps);
    cmdList->SetGraphicsRootSignature(_geometryRootSignature.Get());

    const auto passCb = context.CurrFrameResource->PassCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(2, passCb->GetGPUVirtualAddress());

    drawGeometry(cmdList);
}

void RenderingSystem::DirectionalLightingPass(const FrameContext& context) const
{
    auto cmdList = context.CmdList;
    auto gBuffer = context.GBufferTarget;

    cmdList->SetPipelineState(_directionalLightingPso.Get());
    cmdList->SetGraphicsRootSignature(_lightingRootSignature.Get());

    ID3D12DescriptorHeap* descriptorHeaps[] = { gBuffer->SRVHeap() };
    cmdList->SetDescriptorHeaps(1, descriptorHeaps);

    gBuffer->ChangeRTVsState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gBuffer->ChangeDSVState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrv = {};
    gbufferSrv = gBuffer->SRVHeap()->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetGraphicsRootDescriptorTable(0, gbufferSrv);

    const auto lightCb = context.CurrFrameResource->LightCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(1, lightCb->GetGPUVirtualAddress());

    // Geometry leaves the IA in patch-list mode; the fullscreen pass needs a triangle.
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
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

    ID3D12DescriptorHeap* descriptorHeaps[1] = { context.GBufferTarget->SRVHeap() };
    cmdList->SetDescriptorHeaps(1, descriptorHeaps);

    D3D12_GPU_DESCRIPTOR_HANDLE gbufferSrv = {};
    gbufferSrv = context.GBufferTarget->SRVHeap()->GetGPUDescriptorHandleForHeapStart();
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
        // Every material owns three contiguous descriptors: diffuse, normal, displacement.
        texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);

        CD3DX12_ROOT_PARAMETER slotRootParameter[5];
        // The domain shader samples displacement, while the pixel shader samples color/normal.
        slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_ALL);
        slotRootParameter[1].InitAsConstantBufferView(0);
        slotRootParameter[2].InitAsConstantBufferView(1);
        slotRootParameter[3].InitAsConstantBufferView(2);
        // Visible instance transforms are compacted into this structured buffer each frame.
        slotRootParameter[4].InitAsShaderResourceView(3);

        const auto staticSamplers = GetStaticSamplers();
        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
            static_cast<UINT>(staticSamplers.size()), staticSamplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig = nullptr;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob = nullptr;
        const auto hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
            ::OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
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
        const auto hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

        if (errorBlob != nullptr)
            ::OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
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
    _shaders["geometryHS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "HS", "hs_5_0");
    _shaders["geometryDS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "DS", "ds_5_0");
    _shaders["geometryPS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_0");
    _shaders["directionalVS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "VS", "vs_5_0");
    _shaders["directionalPS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "PS", "ps_5_0");
    _shaders["localLightingVS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "LocalLightingVS", "vs_5_0");
    _shaders["localLightingPS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "LocalLightingPS", "ps_5_0");
}

void RenderingSystem::BuildPsOs(const BuildContext& context)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC geometryPsoDesc = {};
    geometryPsoDesc.InputLayout = { context.GeometryInputLayout->data(), static_cast<UINT>(context.GeometryInputLayout->size()) };
    geometryPsoDesc.pRootSignature = _geometryRootSignature.Get();
    geometryPsoDesc.VS =
    {
        static_cast<BYTE*>(_shaders["geometryVS"]->GetBufferPointer()),
        _shaders["geometryVS"]->GetBufferSize()
    };
    geometryPsoDesc.HS =
    {
        static_cast<BYTE*>(_shaders["geometryHS"]->GetBufferPointer()),
        _shaders["geometryHS"]->GetBufferSize()
    };
    geometryPsoDesc.DS =
    {
        static_cast<BYTE*>(_shaders["geometryDS"]->GetBufferPointer()),
        _shaders["geometryDS"]->GetBufferSize()
    };
    geometryPsoDesc.PS =
    {
        static_cast<BYTE*>(_shaders["geometryPS"]->GetBufferPointer()),
        _shaders["geometryPS"]->GetBufferSize()
    };
    geometryPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    geometryPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    geometryPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    geometryPsoDesc.SampleMask = UINT_MAX;
    geometryPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
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
    directionalPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    directionalPsoDesc.HS = {};
    directionalPsoDesc.DS = {};
    directionalPsoDesc.VS =
    {
        static_cast<BYTE*>(_shaders["directionalVS"]->GetBufferPointer()),
        _shaders["directionalVS"]->GetBufferSize()
    };
    directionalPsoDesc.PS =
    {
        static_cast<BYTE*>(_shaders["directionalPS"]->GetBufferPointer()),
        _shaders["directionalPS"]->GetBufferSize()
    };
    directionalPsoDesc.NumRenderTargets = 1;
    for (auto& rtvFormat : directionalPsoDesc.RTVFormats)
        rtvFormat = DXGI_FORMAT_UNKNOWN;
    directionalPsoDesc.RTVFormats[0] = context.BackBufferFormat;
    directionalPsoDesc.DepthStencilState.DepthEnable = false;
    directionalPsoDesc.DepthStencilState.StencilEnable = false;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&directionalPsoDesc, IID_PPV_ARGS(&_directionalLightingPso)));

    std::vector<D3D12_INPUT_ELEMENT_DESC> localLightInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC localPsoDesc = directionalPsoDesc;
    localPsoDesc.InputLayout = { localLightInputLayout.data(), static_cast<UINT>(localLightInputLayout.size()) };
    localPsoDesc.VS =
    {
        static_cast<BYTE*>(_shaders["localLightingVS"]->GetBufferPointer()),
        _shaders["localLightingVS"]->GetBufferSize()
    };
    localPsoDesc.PS =
    {
        static_cast<BYTE*>(_shaders["localLightingPS"]->GetBufferPointer()),
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
    // Render one back-facing shell, whether the camera is inside or outside.
    localPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&localPsoDesc, IID_PPV_ARGS(&_localLightingPso)));
}

std::vector<CD3DX12_STATIC_SAMPLER_DESC> RenderingSystem::GetStaticSamplers()
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
