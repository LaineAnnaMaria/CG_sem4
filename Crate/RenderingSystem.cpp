#include "RenderingSystem.h"

#include <cfloat>
#include <cmath>

using namespace DirectX;

void RenderingSystem::Initialize(const BuildContext& context)
{
    _context = context;
    BuildRootSignatures(context.Device);
    BuildShaders();
    BuildPsOs(context);
    BuildShadowResources(context.Device, context.GBufferTarget);
    BuildPostProcessResources(context.Device,
        static_cast<UINT>(context.Viewport->Width),
        static_cast<UINT>(context.Viewport->Height));
    ParticleSystem::BuildContext particleContext;
    particleContext.Device = context.Device;
    particleContext.CmdList = context.CmdList;
    particleContext.DepthStencilFormat = context.DepthStencilFormat;
    particleContext.MsaaEnabled = context.MsaaEnabled;
    particleContext.MsaaQuality = context.MsaaQuality;
    _particleSystem.Initialize(particleContext);
    _visibilitySystem.Initialize(*context.RenderItems);
}

void RenderingSystem::OnResize(UINT width, UINT height)
{
    if (_context.Device != nullptr && width > 0 && height > 0)
        BuildPostProcessResources(_context.Device, width, height);
}

RenderingSystem::RenderStats RenderingSystem::Render(FrameResource* frameResource,
    ID3D12Resource* backBuffer, D3D12_CPU_DESCRIPTOR_HANDLE backBufferView,
    UINT localLightVolumeCount)
{
    const std::vector<RenderItem*>& visibleItems = _visibilitySystem.Cull(
        *_context.View, *_context.Proj);
    const std::vector<RenderItem*>& shadowCasters = _visibilitySystem.ShadowCasters();
    FrameContext context;
    context.CmdList = _context.CmdList;
    context.GBufferTarget = _context.GBufferTarget;
    context.Viewport = *_context.Viewport;
    context.ScissorRect = *_context.ScissorRect;
    context.BackBuffer = backBuffer;
    context.BackBufferView = backBufferView;
    context.SceneSrvHeap = _context.SceneSrvHeap;
    context.CurrFrameResource = frameResource;
    context.LocalLightVolumeGeo = _context.LocalLightVolumeGeo;
    context.LocalLightVolumeCount = localLightVolumeCount;
    context.VisibleRenderItems = &visibleItems;
    context.ShadowRenderItems = &shadowCasters;
    context.View = *_context.View;
    context.Proj = *_context.Proj;
    context.DirectionalLightDirection = *_context.DirectionalLightDirection;
    context.NearZ = _context.NearZ;
    context.FarZ = _context.FarZ;
    context.SceneSrvDescriptorSize = _context.SceneSrvDescriptorSize;

    const auto cmdList = context.CmdList;

    UpdateCascades(context);
    ShadowPass(context);

    cmdList->RSSetViewports(1, &context.Viewport);
    cmdList->RSSetScissorRects(1, &context.ScissorRect);

    RenderStats stats = GeometryPass(context);

    const auto sceneColorToRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        _sceneColor.Get(), _sceneColorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmdList->ResourceBarrier(1, &sceneColorToRenderTarget);
    _sceneColorState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    const auto sceneColorRtv = context.GBufferTarget->SceneColorRtv();
    cmdList->ClearRenderTargetView(sceneColorRtv, Colors::Black, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &sceneColorRtv, true, nullptr);

    DirectionalLightingPass(context);
    LocalLightingPass(context);

    const CD3DX12_RESOURCE_BARRIER postProcessBarriers[] =
    {
        CD3DX12_RESOURCE_BARRIER::Transition(_sceneColor.Get(), _sceneColorState,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(context.BackBuffer,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET)
    };
    cmdList->ResourceBarrier(_countof(postProcessBarriers), postProcessBarriers);
    _sceneColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    cmdList->ClearRenderTargetView(context.BackBufferView, Colors::Black, 0, nullptr);
    cmdList->OMSetRenderTargets(1, &context.BackBufferView, true, nullptr);
    PostProcessPass(context);

    const auto fromRtToPresent = CD3DX12_RESOURCE_BARRIER::Transition(
        context.BackBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);

    cmdList->ResourceBarrier(1, &fromRtToPresent);
    return stats;
}

void RenderingSystem::UpdateObjectConstants(FrameResource* frameResource,
    const std::vector<std::unique_ptr<RenderItem>>& items)
{
    for (const auto& item : items)
    {
        if (item->NumFramesDirty <= 0)
            continue;

        ObjectConstants constants;
        XMStoreFloat4x4(&constants.World,
            XMMatrixTranspose(XMLoadFloat4x4(&item->World)));
        XMStoreFloat4x4(&constants.TexTransform,
            XMMatrixTranspose(XMLoadFloat4x4(&item->TexTransform)));
        constants.UseInstancing = item->IsScatteredObject ? 1 : 0;
        frameResource->ObjectCB->CopyData(item->ObjCBIndex, constants);
        --item->NumFramesDirty;
    }
}

void RenderingSystem::UpdateMaterialConstants(FrameResource* frameResource,
    const std::unordered_map<std::string, std::unique_ptr<Material>>& materials)
{
    for (const auto& entry : materials)
    {
        Material* material = entry.second.get();
        if (material->NumFramesDirty <= 0)
            continue;

        MaterialConstants constants;
        constants.DiffuseAlbedo = material->DiffuseAlbedo;
        constants.FresnelR0 = material->FresnelR0;
        constants.Roughness = material->Roughness;
        constants.DisplacementScale = material->DisplacementScale;
        constants.MinTessDistance = material->MinTessDistance;
        constants.MaxTessDistance = material->MaxTessDistance;
        constants.MinTessFactor = material->MinTessFactor;
        constants.MaxTessFactor = material->MaxTessFactor;
        constants.UseNormalMap = material->NormalTexturePath.empty() ? 0 : 1;
        constants.UseDisplacementMap = material->DisplacementTexturePath.empty() ? 0 : 1;
        XMStoreFloat4x4(&constants.MatTransform,
            XMMatrixTranspose(XMLoadFloat4x4(&material->MatTransform)));
        frameResource->MaterialCB->CopyData(material->MatCBIndex, constants);
        --material->NumFramesDirty;
    }
}

RenderingSystem::RenderStats RenderingSystem::GeometryPass(const FrameContext& context)
{
    const auto cmdList = context.CmdList;
    const auto buffer = context.GBufferTarget;
    const auto passCb = context.CurrFrameResource->PassCB->Resource();

    if (_particlesEnabled)
        _particleSystem.Simulate(cmdList, passCb->GetGPUVirtualAddress());

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

    cmdList->SetGraphicsRootConstantBufferView(2, passCb->GetGPUVirtualAddress());

    RenderStats stats = DrawRenderItems(context, false);
    if (_particlesEnabled)
    {
        _particleSystem.Draw(cmdList, passCb->GetGPUVirtualAddress());
        ++stats.DrawCalls;
    }
    return stats;
}

RenderingSystem::RenderStats RenderingSystem::DrawRenderItems(const FrameContext& context,
    bool shadowPass) const
{
    RenderStats stats;
    const std::vector<RenderItem*>* renderItems = shadowPass
        ? context.ShadowRenderItems
        : context.VisibleRenderItems;
    if (renderItems == nullptr)
        return stats;

    auto cmdList = context.CmdList;
    auto objectBuffer = context.CurrFrameResource->ObjectCB->Resource();
    UploadBuffer<InstanceData>* instanceUploadBuffer = shadowPass
        ? context.CurrFrameResource->ShadowInstanceBuffer.get()
        : context.CurrFrameResource->InstanceBuffer.get();
    auto instanceBuffer = instanceUploadBuffer->Resource();
    const UINT objectByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    const UINT materialByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    cmdList->SetGraphicsRootShaderResourceView(shadowPass ? 2 : 4,
        instanceBuffer->GetGPUVirtualAddress());

    auto bindItem = [&](RenderItem* item)
    {
        const auto vertexBufferView = item->Geo->VertexBufferView();
        const auto indexBufferView = item->Geo->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
        cmdList->IASetIndexBuffer(&indexBufferView);
        cmdList->IASetPrimitiveTopology(shadowPass
            ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
            : item->PrimitiveType);

        const D3D12_GPU_VIRTUAL_ADDRESS objectAddress = objectBuffer->GetGPUVirtualAddress() +
            static_cast<UINT64>(item->ObjCBIndex) * objectByteSize;
        cmdList->SetGraphicsRootConstantBufferView(shadowPass ? 0 : 1, objectAddress);

        if (!shadowPass)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE texture = context.SceneSrvHeap->GetGPUDescriptorHandleForHeapStart();
            texture.ptr += static_cast<UINT64>(item->Mat->DiffuseSrvHeapIndex) *
                context.SceneSrvDescriptorSize;
            const auto materialBuffer = context.CurrFrameResource->MaterialCB->Resource();
            const D3D12_GPU_VIRTUAL_ADDRESS materialAddress = materialBuffer->GetGPUVirtualAddress() +
                static_cast<UINT64>(item->Mat->MatCBIndex) * materialByteSize;
            cmdList->SetGraphicsRootDescriptorTable(0, texture);
            cmdList->SetGraphicsRootConstantBufferView(3, materialAddress);
        }
    };

    RenderItem* scatteredPrototype = nullptr;
    UINT scatteredCount = 0;
    for (RenderItem* item : *renderItems)
    {
        if (item->IsScatteredObject)
        {
            if (scatteredPrototype == nullptr)
                scatteredPrototype = item;

            InstanceData instanceData;
            XMStoreFloat4x4(&instanceData.World,
                XMMatrixTranspose(XMLoadFloat4x4(&item->World)));
            const float colorAmount = _scatterColorVariation;
            instanceData.Color = {
                1.0f + (item->InstanceColor.x - 1.0f) * colorAmount,
                1.0f + (item->InstanceColor.y - 1.0f) * colorAmount,
                1.0f + (item->InstanceColor.z - 1.0f) * colorAmount,
                1.0f
            };
            instanceData.Size = item->InstanceSize * _scatterSizeScale;
            instanceUploadBuffer->CopyData(scatteredCount++, instanceData);
            continue;
        }

        bindItem(item);
        cmdList->DrawIndexedInstanced(item->IndexCount, 1, item->StartIndexLocation,
            item->BaseVertexLocation, 0);
        ++stats.DrawCalls;
    }

    if (scatteredPrototype != nullptr && scatteredCount != 0)
    {
        bindItem(scatteredPrototype);
        cmdList->DrawIndexedInstanced(scatteredPrototype->IndexCount, scatteredCount,
            scatteredPrototype->StartIndexLocation, scatteredPrototype->BaseVertexLocation, 0);
        ++stats.DrawCalls;
        stats.ScatteredInstances = scatteredCount;
    }
    return stats;
}

void RenderingSystem::ShadowPass(const FrameContext& context)
{
    auto cmdList = context.CmdList;
    const auto toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(_cascadeShadowMap.Get(),
        _shadowMapState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    cmdList->ResourceBarrier(1, &toDepthWrite);
    _shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    cmdList->RSSetViewports(1, &_shadowViewport);
    cmdList->RSSetScissorRects(1, &_shadowScissorRect);
    cmdList->SetPipelineState(_shadowPso.Get());
    cmdList->SetGraphicsRootSignature(_shadowRootSignature.Get());

    const UINT shadowCbByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ShadowConstants));
    const UINT descriptorSize = _shadowDsvHeapDescriptorSize;
    for (int cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart(),
            cascadeIndex, descriptorSize);
        cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cmdList->OMSetRenderTargets(0, nullptr, false, &dsv);
        cmdList->SetGraphicsRootConstantBufferView(1,
            context.CurrFrameResource->ShadowCB->Resource()->GetGPUVirtualAddress() +
            static_cast<UINT64>(cascadeIndex) * shadowCbByteSize);
        DrawRenderItems(context, true);
    }

    const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(_cascadeShadowMap.Get(),
        _shadowMapState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmdList->ResourceBarrier(1, &toShaderResource);
    _shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void RenderingSystem::AppendScatteredBoxes(std::vector<std::unique_ptr<RenderItem>>& items,
    UINT& nextObjectIndex, MeshGeometry* geometry, const SubmeshGeometry& submesh,
    Material* material, const bool* visibility)
{
    constexpr UINT sideLength = 32;
    constexpr float spacing = 16.0f;
    for (UINT z = 0; z < sideLength; ++z)
    {
        for (UINT x = 0; x < sideLength; ++x)
        {
            auto item = std::make_unique<RenderItem>();
            const float worldX = (static_cast<float>(x) - 0.5f * (sideLength - 1)) * spacing;
            const float worldZ = (static_cast<float>(z) - 0.5f * (sideLength - 1)) * spacing;
            const float scale = 1.4f + 0.35f * sinf(static_cast<float>(x * 13 + z * 7));
            const float worldY = -3.0f + 2.0f * sinf(static_cast<float>(x) * 0.47f) *
                cosf(static_cast<float>(z) * 0.39f);
            const XMMATRIX world = XMMatrixRotationY(0.31f * static_cast<float>(x + z)) *
                XMMatrixTranslation(worldX, worldY, worldZ);

            XMStoreFloat4x4(&item->World, world);
            item->InstanceSize = scale;
            const float seed = static_cast<float>(x * 31 + z * 17);
            item->InstanceColor = {
                0.65f + 0.35f * (0.5f + 0.5f * sinf(seed * 0.17f)),
                0.65f + 0.35f * (0.5f + 0.5f * sinf(seed * 0.23f + 2.1f)),
                0.65f + 0.35f * (0.5f + 0.5f * sinf(seed * 0.31f + 4.2f)),
                1.0f
            };
            item->ObjCBIndex = nextObjectIndex++;
            item->ModelIndex = UINT_MAX;
            item->Geo = geometry;
            item->Mat = material;
            item->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            item->IndexCount = submesh.IndexCount;
            item->StartIndexLocation = submesh.StartIndexLocation;
            item->BaseVertexLocation = submesh.BaseVertexLocation;
            item->IsScatteredObject = true;
            item->Visibility = visibility;
            submesh.Bounds.Transform(item->WorldBounds, XMMatrixScaling(scale, scale, scale) * world);
            items.push_back(std::move(item));
        }
    }
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

    D3D12_GPU_DESCRIPTOR_HANDLE shadowSrv = gbufferSrv;
    shadowSrv.ptr += static_cast<UINT64>(GBuffer::InfoCount(false)) * context.SceneSrvDescriptorSize;
    cmdList->SetGraphicsRootDescriptorTable(3, shadowSrv);

    const auto lightCb = context.CurrFrameResource->LightCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(1, lightCb->GetGPUVirtualAddress());
    const auto passCb = context.CurrFrameResource->PassCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(2, passCb->GetGPUVirtualAddress());
    const auto cascadeCb = context.CurrFrameResource->CascadeCB->Resource();
    cmdList->SetGraphicsRootConstantBufferView(4, cascadeCb->GetGPUVirtualAddress());

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

void RenderingSystem::PostProcessPass(const FrameContext& context) const
{
    auto cmdList = context.CmdList;
    cmdList->SetPipelineState(_postProcessPso.Get());
    cmdList->SetGraphicsRootSignature(_postProcessRootSignature.Get());

    ID3D12DescriptorHeap* descriptorHeaps[] = { context.GBufferTarget->SRVHeap() };
    cmdList->SetDescriptorHeaps(1, descriptorHeaps);
    const auto sourceSrv = _gBufferDebugMode == 0
        ? context.GBufferTarget->SceneColorSrvGpu()
        : context.GBufferTarget->InfoSrvGpu(static_cast<GBufferInfo>(_gBufferDebugMode - 1));
    cmdList->SetGraphicsRootDescriptorTable(0, sourceSrv);

    struct PostProcessSettings
    {
        UINT ChromaticAberrationEnabled;
        UINT VignetteEnabled;
        UINT GBufferDebugMode;
        float CameraMotionAmount;
    } settings =
    {
        _chromaticAberrationEnabled ? 1u : 0u,
        _vignetteEnabled ? 1u : 0u,
        static_cast<UINT>(_gBufferDebugMode),
        _cameraMotionAmount
    };
    cmdList->SetGraphicsRoot32BitConstants(1, 4, &settings, 0);
    cmdList->IASetVertexBuffers(0, 0, nullptr);
    cmdList->IASetIndexBuffer(nullptr);
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->DrawInstanced(3, 1, 0, 0);
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
        CD3DX12_DESCRIPTOR_RANGE shadowTable;
        shadowTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

        CD3DX12_ROOT_PARAMETER slotRootParameter[5];
        slotRootParameter[0].InitAsDescriptorTable(1, &gbufferTable, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[1].InitAsConstantBufferView(0);
        slotRootParameter[2].InitAsConstantBufferView(1);
        slotRootParameter[3].InitAsDescriptorTable(1, &shadowTable, D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[4].InitAsConstantBufferView(2);

        const CD3DX12_STATIC_SAMPLER_DESC shadowSampler(
            0, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER,
            0.0f, 1, D3D12_COMPARISON_FUNC_LESS_EQUAL,
            D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
            1, &shadowSampler,
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

    {
        CD3DX12_ROOT_PARAMETER slotRootParameter[3];
        slotRootParameter[0].InitAsConstantBufferView(0);
        slotRootParameter[1].InitAsConstantBufferView(1);
        slotRootParameter[2].InitAsShaderResourceView(0);

        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        const auto hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());
        if (errorBlob != nullptr)
            ::OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(), IID_PPV_ARGS(_shadowRootSignature.GetAddressOf())));
    }

    {
        CD3DX12_DESCRIPTOR_RANGE sceneColorTable;
        sceneColorTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        CD3DX12_ROOT_PARAMETER slotRootParameter[2];
        slotRootParameter[0].InitAsDescriptorTable(1, &sceneColorTable,
            D3D12_SHADER_VISIBILITY_PIXEL);
        slotRootParameter[1].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

        const CD3DX12_STATIC_SAMPLER_DESC linearClampSampler(
            0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(_countof(slotRootParameter), slotRootParameter,
            1, &linearClampSampler,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        Microsoft::WRL::ComPtr<ID3DBlob> serializedRootSig;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
        const auto hr = D3D12SerializeRootSignature(&rootSigDesc,
            D3D_ROOT_SIGNATURE_VERSION_1, serializedRootSig.GetAddressOf(),
            errorBlob.GetAddressOf());
        if (errorBlob != nullptr)
            ::OutputDebugStringA(static_cast<char*>(errorBlob->GetBufferPointer()));
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(0, serializedRootSig->GetBufferPointer(),
            serializedRootSig->GetBufferSize(),
            IID_PPV_ARGS(_postProcessRootSignature.GetAddressOf())));
    }
}

void RenderingSystem::BuildShaders()
{
    _shaders["geometryVS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "VS", "vs_5_0");
    _shaders["geometryHS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "HS", "hs_5_0");
    _shaders["geometryDS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "DS", "ds_5_0");
    _shaders["geometryPS"] = d3dUtil::CompileShader(L"Shaders\\Default.hlsl", nullptr, "PS", "ps_5_0");
    _shaders["directionalVS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "FullscreenVS", "vs_5_0");
    _shaders["directionalPS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "PS", "ps_5_0");
    _shaders["localLightingVS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "LocalLightingVS", "vs_5_0");
    _shaders["localLightingPS"] = d3dUtil::CompileShader(L"Shaders\\DirLight.hlsl", nullptr, "LocalLightingPS", "ps_5_0");
    _shaders["shadowVS"] = d3dUtil::CompileShader(L"Shaders\\Shadow.hlsl", nullptr, "VS", "vs_5_0");
    _shaders["postProcessVS"] = d3dUtil::CompileShader(L"Shaders\\PostProcess.hlsl", nullptr, "FullscreenVS", "vs_5_0");
    _shaders["postProcessPS"] = d3dUtil::CompileShader(L"Shaders\\PostProcess.hlsl", nullptr, "PS", "ps_5_0");
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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC shadowPsoDesc = geometryPsoDesc;
    shadowPsoDesc.pRootSignature = _shadowRootSignature.Get();
    shadowPsoDesc.VS =
    {
        static_cast<BYTE*>(_shaders["shadowVS"]->GetBufferPointer()),
        _shaders["shadowVS"]->GetBufferSize()
    };
    shadowPsoDesc.HS = {};
    shadowPsoDesc.DS = {};
    shadowPsoDesc.PS = {};
    shadowPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    shadowPsoDesc.NumRenderTargets = 0;
    for (auto& format : shadowPsoDesc.RTVFormats)
        format = DXGI_FORMAT_UNKNOWN;
    shadowPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    shadowPsoDesc.SampleDesc.Count = 1;
    shadowPsoDesc.SampleDesc.Quality = 0;
    shadowPsoDesc.RasterizerState.DepthBias = 1000;
    shadowPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
    shadowPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.5f;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&shadowPsoDesc,
        IID_PPV_ARGS(&_shadowPso)));

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

    D3D12_GRAPHICS_PIPELINE_STATE_DESC postProcessPsoDesc = directionalPsoDesc;
    postProcessPsoDesc.pRootSignature = _postProcessRootSignature.Get();
    postProcessPsoDesc.VS =
    {
        static_cast<BYTE*>(_shaders["postProcessVS"]->GetBufferPointer()),
        _shaders["postProcessVS"]->GetBufferSize()
    };
    postProcessPsoDesc.PS =
    {
        static_cast<BYTE*>(_shaders["postProcessPS"]->GetBufferPointer()),
        _shaders["postProcessPS"]->GetBufferSize()
    };
    postProcessPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    postProcessPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    ThrowIfFailed(context.Device->CreateGraphicsPipelineState(&postProcessPsoDesc,
        IID_PPV_ARGS(&_postProcessPso)));
}

void RenderingSystem::BuildPostProcessResources(ID3D12Device* device, UINT width, UINT height)
{
    _sceneColor.Reset();
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = _context.BackBufferFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = _context.BackBufferFormat;
    clearValue.Color[3] = 1.0f;
    const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
        &textureDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(_sceneColor.GetAddressOf())));
    _sceneColorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    device->CreateRenderTargetView(_sceneColor.Get(), nullptr,
        _context.GBufferTarget->SceneColorRtv());

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = _context.BackBufferFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(_sceneColor.Get(), &srvDesc,
        _context.GBufferTarget->SceneColorSrvCpu());
}

void RenderingSystem::BuildShadowResources(ID3D12Device* device, GBuffer* gBuffer)
{
    if (gBuffer == nullptr)
        throw std::runtime_error("RenderingSystem requires a G-buffer for the cascade shadow SRV.");

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = ShadowMapResolution;
    textureDesc.Height = ShadowMapResolution;
    textureDesc.DepthOrArraySize = ShadowCascadeCount;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    const CD3DX12_HEAP_PROPERTIES heapProperties(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
        &textureDesc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(_cascadeShadowMap.GetAddressOf())));

    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = ShadowCascadeCount;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device->CreateDescriptorHeap(&dsvHeapDesc,
        IID_PPV_ARGS(_shadowDsvHeap.GetAddressOf())));
    _shadowDsvHeapDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    for (int cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.FirstArraySlice = cascadeIndex;
        dsvDesc.Texture2DArray.ArraySize = 1;
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(_shadowDsvHeap->GetCPUDescriptorHandleForHeapStart(),
            cascadeIndex, _shadowDsvHeapDescriptorSize);
        device->CreateDepthStencilView(_cascadeShadowMap.Get(), &dsvDesc, dsv);
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.ArraySize = ShadowCascadeCount;
    device->CreateShaderResourceView(_cascadeShadowMap.Get(), &srvDesc, gBuffer->lightingHandle());

    _shadowViewport = { 0.0f, 0.0f, static_cast<float>(ShadowMapResolution),
        static_cast<float>(ShadowMapResolution), 0.0f, 1.0f };
    _shadowScissorRect = { 0, 0, static_cast<LONG>(ShadowMapResolution),
        static_cast<LONG>(ShadowMapResolution) };
}

void RenderingSystem::UpdateCascades(const FrameContext& context)
{
    BoundingFrustum cameraFrustum;
    BoundingFrustum::CreateFromMatrix(cameraFrustum, XMLoadFloat4x4(&context.Proj));
    XMFLOAT3 localCorners[8];
    cameraFrustum.GetCorners(localCorners);

    const XMMATRIX inverseView = XMMatrixInverse(nullptr, XMLoadFloat4x4(&context.View));
    XMVECTOR worldCorners[8];
    for (int corner = 0; corner < 8; ++corner)
        worldCorners[corner] = XMVector3TransformCoord(XMLoadFloat3(&localCorners[corner]), inverseView);

    const float nearZ = (std::max)(context.NearZ, 0.001f);
    const float farZ = (std::max)(context.FarZ, nearZ + 0.001f);
    constexpr float logarithmicWeight = 0.8f;
    float splits[ShadowCascadeCount + 1] = {};
    splits[0] = nearZ;
    for (int splitIndex = 1; splitIndex < ShadowCascadeCount; ++splitIndex)
    {
        const float ratio = static_cast<float>(splitIndex) / ShadowCascadeCount;
        const float logarithmic = nearZ * powf(farZ / nearZ, ratio);
        const float uniform = nearZ + (farZ - nearZ) * ratio;
        splits[splitIndex] = logarithmicWeight * logarithmic +
            (1.0f - logarithmicWeight) * uniform;
    }
    splits[ShadowCascadeCount] = farZ;

    XMVECTOR lightDirection = XMVector3Normalize(XMLoadFloat3(&context.DirectionalLightDirection));
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    if (fabsf(XMVectorGetX(XMVector3Dot(lightDirection, up))) > 0.9f)
        up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);

    // A directional light has no physical position.  Keep its coordinate system
    // anchored to the world so rotating the camera cannot slide the shadow grid.
    // Only each cascade's orthographic window follows the camera frustum.
    constexpr float lightAnchorDistance = 2000.0f;
    const XMVECTOR worldOrigin = XMVectorZero();
    const XMMATRIX fixedLightView = XMMatrixLookAtLH(
        worldOrigin - lightDirection * lightAnchorDistance, worldOrigin, up);

    XMFLOAT3 sceneCorners[BoundingBox::CORNER_COUNT];
    _visibilitySystem.SceneBounds().GetCorners(sceneCorners);
    float sceneMinimumZ = FLT_MAX;
    float sceneMaximumZ = -FLT_MAX;
    for (const XMFLOAT3& sceneCorner : sceneCorners)
    {
        const XMVECTOR lightSpaceCorner = XMVector3TransformCoord(
            XMLoadFloat3(&sceneCorner), fixedLightView);
        sceneMinimumZ = (std::min)(sceneMinimumZ, XMVectorGetZ(lightSpaceCorner));
        sceneMaximumZ = (std::max)(sceneMaximumZ, XMVectorGetZ(lightSpaceCorner));
    }

    CascadeSetConstants cascadeSet;
    for (int cascadeIndex = 0; cascadeIndex < ShadowCascadeCount; ++cascadeIndex)
    {
        const float nearRatio = (splits[cascadeIndex] - nearZ) / (farZ - nearZ);
        const float farRatio = (splits[cascadeIndex + 1] - nearZ) / (farZ - nearZ);
        XMVECTOR cascadeCorners[8];
        XMVECTOR center = XMVectorZero();
        for (int corner = 0; corner < 4; ++corner)
        {
            cascadeCorners[corner] = XMVectorLerp(worldCorners[corner], worldCorners[corner + 4], nearRatio);
            cascadeCorners[corner + 4] = XMVectorLerp(worldCorners[corner], worldCorners[corner + 4], farRatio);
            center += cascadeCorners[corner] + cascadeCorners[corner + 4];
        }
        center /= 8.0f;

        float radius = 0.0f;
        for (const XMVECTOR& corner : cascadeCorners)
            radius = (std::max)(radius, XMVectorGetX(XMVector3Length(corner - center)));
        radius = ceilf(radius * 16.0f) / 16.0f;

        const XMVECTOR centerLightSpace = XMVector3TransformCoord(center, fixedLightView);
        const float worldUnitsPerTexel = (2.0f * radius) / ShadowMapResolution;
        const float centerX = floorf(XMVectorGetX(centerLightSpace) / worldUnitsPerTexel) *
            worldUnitsPerTexel;
        const float centerY = floorf(XMVectorGetY(centerLightSpace) / worldUnitsPerTexel) *
            worldUnitsPerTexel;
        const XMFLOAT3 minPoint = { centerX - radius, centerY - radius, sceneMinimumZ - 50.0f };
        const XMFLOAT3 maxPoint = { centerX + radius, centerY + radius, sceneMaximumZ + 50.0f };

        const XMMATRIX lightProjection = XMMatrixOrthographicOffCenterLH(minPoint.x, maxPoint.x,
            minPoint.y, maxPoint.y, minPoint.z, maxPoint.z);
        const XMMATRIX viewProjection = fixedLightView * lightProjection;
        _cascades[cascadeIndex].SplitNear = splits[cascadeIndex];
        _cascades[cascadeIndex].SplitFar = splits[cascadeIndex + 1];
        XMStoreFloat4x4(&_cascades[cascadeIndex].ViewProj, XMMatrixTranspose(viewProjection));
        cascadeSet.Cascades[cascadeIndex] = _cascades[cascadeIndex];

        ShadowConstants shadowConstants;
        shadowConstants.ViewProj = _cascades[cascadeIndex].ViewProj;
        context.CurrFrameResource->ShadowCB->CopyData(cascadeIndex, shadowConstants);
    }
    context.CurrFrameResource->CascadeCB->CopyData(0, cascadeSet);
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
