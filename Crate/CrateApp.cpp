#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "common/d3dApp.h"
#include "common/MathHelper.h"
#include "common/UploadBuffer.h"
#include "Common/GeometryGenerator.h"
#include "FrameResource.h"
#include "RenderingSystem.h"
#include "TextureUploader.h"
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_dx12.h"
#include "imgui/backends/imgui_impl_win32.h"

#include <array>
#include <limits>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "D3D12.lib")

const int gNumFrameResources = 3;

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
    RenderItem() = default;

    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    XMFLOAT4X4 World = MathHelper::Identity4x4();

    XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();

    // Dirty flag indicating the object data has changed and we need to update the constant buffer.
    // Because we have an object cbuffer for each FrameResource, we have to apply the
    // update to each FrameResource.  Thus, when we modify obect data we should set 
    // NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
    int NumFramesDirty = gNumFrameResources;

    // Index into GPU constant buffer corresponding to the ObjectCB for this render item.
    UINT ObjCBIndex = -1;

    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
    UINT ModelIndex = 0;

    // Bounds are kept in world space so both the linear and octree culling
    // paths can test the same render items.
    BoundingBox WorldBounds;
    bool IsScatteredObject = false;
};

struct OctreeNode
{
    BoundingBox Bounds;
    std::vector<RenderItem*> Items;
    std::array<std::unique_ptr<OctreeNode>, 8> Children;
};

struct LoadedModel
{
    std::unique_ptr<Assimp::Importer> Importer;
    const aiScene* Scene = nullptr;
    std::string Directory;
    std::string Name;
    XMFLOAT4X4 World = MathHelper::Identity4x4();
    UINT MaterialOffset = 0;
    bool UseDetailMaps = false;
    bool Visible = true;
};

class CrateApp : public D3DApp
{
public:
    CrateApp(HINSTANCE hInstance);
    CrateApp(const CrateApp& rhs) = delete;
    CrateApp& operator=(const CrateApp& rhs) = delete;
    ~CrateApp();

    virtual bool Initialize()override;
    virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) override;

private:

    std::vector<LoadedModel> mModels;

    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
    void UpdateCamera(const GameTimer& gt);
    void AnimateMaterials(const GameTimer& gt);
    void UpdateObjectCBs(const GameTimer& gt);
    void UpdateMaterialCBs(const GameTimer& gt);
    void UpdateMainPassCB(const GameTimer& gt);
    void UpdateLightCB(const GameTimer& gt);

    void LoadTextures();
    void BuildDescriptorHeaps();
    void BuildShadersAndInputLayout();
    void BuildShapeGeometry();
    void BuildLightVolumeGeometry();
    void BuildFrameResources();
    void BuildMaterials();
    void BuildLights();
    void BuildRenderItems();
    void BuildOctree();
    std::unique_ptr<OctreeNode> BuildOctreeNode(const BoundingBox& bounds,
        const std::vector<RenderItem*>& items, UINT depth);
    void UpdateWorldFrustum();
    void CollectOctreeItems(const OctreeNode& node, ContainmentType parentContainment);
    void CollectAllOctreeItems(const OctreeNode& node);
    bool IsRenderItemEnabled(const RenderItem& item) const;
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
    void InitializeImGui();
    void DrawImGui();

    std::vector<CD3DX12_STATIC_SAMPLER_DESC> GetStaticSamplers();

private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    UINT mCbvSrvDescriptorSize = 0;

    ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;
    ComPtr<ID3D12DescriptorHeap> mImGuiSrvDescriptorHeap = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::unordered_map<std::string, std::unique_ptr<Material>> mMaterials;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // List of all the render items.
    std::vector<std::unique_ptr<RenderItem>> mAllRitems;

    // Render items divided by PSO.
    std::vector<RenderItem*> mOpaqueRitems;
    std::vector<RenderItem*> mVisibleRitems;
    std::unique_ptr<OctreeNode> mOctreeRoot;
    BoundingFrustum mWorldFrustum;

    bool mEnableFrustumCulling = true;
    bool mEnableOctreeCulling = false;
    bool mShowScatteredObjects = true;
    UINT mSubmittedObjectCount = 0;
    UINT mCulledObjectCount = 0;
    UINT mOctreeNodesTested = 0;
    UINT mGeometryDrawCallCount = 0;
    UINT mScatteredInstancesSubmitted = 0;
    static constexpr UINT ScatterObjectCount = 1024;
    static constexpr UINT OctreeLeafCapacity = 16;
    static constexpr UINT OctreeMaxDepth = 6;

    PassConstants mMainPassCB;
    LightConstants mLightCB;
    DirectionalLight mDirectionalLight;
    std::vector<PointLight> mPointLights;
    std::vector<SpotLight> mSpotLights;

    XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4X4 mView = MathHelper::Identity4x4();
    XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.3f * XM_PI;
    float mPhi = 0.4f * XM_PI;
    float mRadius = 100.0f;
    float mWalnutDisplacementScale = 1.5f;

    POINT mLastMousePos;
    TextureUploader _textureLoader;
    std::unique_ptr<RenderingSystem> mRenderingSystem;

};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        CrateApp theApp(hInstance);
        if (!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch (DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

CrateApp::CrateApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
}

CrateApp::~CrateApp()
{
    if (md3dDevice != nullptr)
        FlushCommandQueue();

    if (ImGui::GetCurrentContext() != nullptr)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}


bool CrateApp::Initialize()
{
    if (!D3DApp::Initialize())
        return false;

    // Reset the command list to prep for initialization commands.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    // Get the increment size of a descriptor in this heap type.  This is hardware specific, 
    // so we have to query this information.
    mCbvSrvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    _textureLoader = TextureUploader(md3dDevice.Get(), mCommandList.Get());
    mRenderingSystem = std::make_unique<RenderingSystem>();

    const unsigned int importFlags = aiProcess_Triangulate |
        aiProcess_JoinIdenticalVertices |
        aiProcess_FlipUVs |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_MakeLeftHanded |
        aiProcess_FlipWindingOrder |
        aiProcess_GenUVCoords;

    auto loadModel = [this, importFlags](const std::string& name, const std::string& filename,
        const std::string& directory, FXMMATRIX world, bool useDetailMaps)
    {
        LoadedModel model;
        model.Importer = std::make_unique<Assimp::Importer>();
        model.Scene = model.Importer->ReadFile(filename, importFlags);
        if (!model.Scene || (model.Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !model.Scene->mRootNode)
        {
            const std::string error = model.Importer->GetErrorString();
            ::OutputDebugStringA(error.c_str());
            throw std::runtime_error(error);
        }

        model.Directory = directory;
        model.Name = name;
        model.UseDetailMaps = useDetailMaps;
        XMStoreFloat4x4(&model.World, world);
        mModels.push_back(std::move(model));
    };

    // Sponza remains the environment; the detailed walnut rests on its central floor.
    loadModel("Sponza", "sponza/sponza.obj", "sponza/",
        XMMatrixScaling(0.1f, 0.1f, 0.1f) * XMMatrixTranslation(0.0f, 13.0f, 0.0f), false);
    loadModel("Walnut", "Assets/Walnut/walnut.obj", "Assets/Walnut/",
        XMMatrixScaling(1.25f, 1.25f, 1.25f) * XMMatrixTranslation(0.0f, -3.8f, 0.0f), true);

    UINT materialOffset = 0;
    for (LoadedModel& model : mModels)
    {
        model.MaterialOffset = materialOffset;
        materialOffset += model.Scene->mNumMaterials;
    }
    BuildShadersAndInputLayout();
    BuildShapeGeometry();
    BuildLightVolumeGeometry();
    BuildMaterials();
    BuildLights();
    BuildDescriptorHeaps();
    BuildRenderItems();
    BuildOctree();
    BuildFrameResources();

    RenderingSystem::BuildContext renderBuildContext;
    renderBuildContext.Device = md3dDevice.Get();
    renderBuildContext.GeometryInputLayout = &mInputLayout;
    renderBuildContext.BackBufferFormat = mBackBufferFormat;
    renderBuildContext.DepthStencilFormat = mDepthStencilFormat;
    renderBuildContext.MsaaEnabled = m4xMsaaState;
    renderBuildContext.MsaaQuality = m4xMsaaQuality;
    mRenderingSystem->Initialize(renderBuildContext);

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();

    InitializeImGui();

    return true;
}

LRESULT CrateApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui::GetCurrentContext() != nullptr &&
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return 1;
    }

    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}

void CrateApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f * MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void CrateApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);
    UpdateCamera(gt);

    // Cycle through the circular frame resource array.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // Has the GPU finished processing the commands of the current frame resource?
    // If not, wait until the GPU has completed commands up to this fence point.
    if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    AnimateMaterials(gt);
    UpdateObjectCBs(gt);
    UpdateMaterialCBs(gt);
    UpdateMainPassCB(gt);
    UpdateLightCB(gt);
}

void CrateApp::Draw(const GameTimer& gt)
{
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawImGui();

    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mRenderingSystem->GeometryPso()));

    RenderingSystem::FrameContext renderContext;
    renderContext.CmdList = mCommandList.Get();
    renderContext.GBufferTarget = _gBuffer.get();
    renderContext.Viewport = mScreenViewport;
    renderContext.ScissorRect = mScissorRect;
    renderContext.BackBuffer = CurrentBackBuffer();
    renderContext.BackBufferView = CurrentBackBufferView();
    renderContext.SceneSrvHeap = mSrvDescriptorHeap.Get();
    renderContext.CurrFrameResource = mCurrFrameResource;
    renderContext.LocalLightVolumeGeo = mGeometries["lightVolumeGeo"].get();
    renderContext.LocalLightVolumeCount = (UINT)(mPointLights.size() + mSpotLights.size());

    mRenderingSystem->Render(renderContext,
        [this](ID3D12GraphicsCommandList* cmdList)
        {
            DrawRenderItems(cmdList, mOpaqueRitems);
        });

    // RenderingSystem leaves the swap-chain buffer in PRESENT. Draw ImGui last.
    auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &toRenderTarget);
    const auto backBufferView = CurrentBackBufferView();
    mCommandList->OMSetRenderTargets(1, &backBufferView, true, nullptr);

    ID3D12DescriptorHeap* imguiHeaps[1] = { mImGuiSrvDescriptorHeap.Get() };
    mCommandList->SetDescriptorHeaps(1, imguiHeaps);
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCommandList.Get());

    auto toPresent = CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &toPresent);

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, cmdsLists);

    // Swap the back and front buffers
    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Advance the fence value to mark commands up to this fence point.
    mCurrFrameResource->Fence = ++mCurrentFence;

    // Add an instruction to the command queue to set a new fence point. 
    // Because we are on the GPU timeline, the new fence point won't be 
    // set until the GPU finishes processing all the commands prior to this Signal().
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void CrateApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
        return;

    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mhMainWnd);
}

void CrateApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
    {
        ReleaseCapture();
        return;
    }

    ReleaseCapture();
}

void CrateApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
        return;

    if ((btnState & MK_LBUTTON) != 0)
    {
        // Make each pixel correspond to a quarter of a degree.
        float dx = XMConvertToRadians(0.25f * static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f * static_cast<float>(y - mLastMousePos.y));

        // Update angles based on input to orbit camera around box.
        mTheta += dx;
        mPhi += dy;

        // Restrict the angle mPhi.
        mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
    }
    else if ((btnState & MK_RBUTTON) != 0)
    {
        // Make each pixel correspond to 0.2 unit in the scene.
        float dx = 0.05f * static_cast<float>(x - mLastMousePos.x);
        float dy = 0.05f * static_cast<float>(y - mLastMousePos.y);

        // Update the camera radius based on input.
        mRadius += dx - dy;

        // Restrict the radius.
        mRadius = MathHelper::Clamp(mRadius, 20.0f, 350.0f);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}

void CrateApp::OnKeyboardInput(const GameTimer& gt)
{
}

void CrateApp::UpdateCamera(const GameTimer& gt)
{
    // Convert Spherical to Cartesian coordinates.
    XMFLOAT3 focus = { 0.0f, 15.0f, 0.0f };
    mEyePos.x = focus.x + mRadius * sinf(mPhi) * cosf(mTheta);
    mEyePos.z = focus.z + mRadius * sinf(mPhi) * sinf(mTheta);
    mEyePos.y = focus.y + mRadius * cosf(mPhi);

    // Build the view matrix.
    XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
    XMVECTOR target = XMLoadFloat3(&focus);
    XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
    XMStoreFloat4x4(&mView, view);
    UpdateWorldFrustum();
}

void CrateApp::AnimateMaterials(const GameTimer& gt)
{

}

void CrateApp::UpdateObjectCBs(const GameTimer& gt)
{
    auto currObjectCB = mCurrFrameResource->ObjectCB.get();
    for (auto& e : mAllRitems)
    {
        // Only update the cbuffer data if the constants have changed.  
        // This needs to be tracked per frame resource.
        if (e->NumFramesDirty > 0)
        {
            XMMATRIX world = XMLoadFloat4x4(&e->World);
            XMMATRIX texTransform = XMLoadFloat4x4(&e->TexTransform);

            ObjectConstants objConstants;
            XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
            XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
            objConstants.UseInstancing = e->IsScatteredObject ? 1 : 0;

            currObjectCB->CopyData(e->ObjCBIndex, objConstants);

            // Next FrameResource need to be updated too.
            e->NumFramesDirty--;
        }
    }
}

void CrateApp::UpdateMaterialCBs(const GameTimer& gt)
{
    auto currMaterialCB = mCurrFrameResource->MaterialCB.get();
    for (auto& e : mMaterials)
    {
        // Only update the cbuffer data if the constants have changed.  If the cbuffer
        // data changes, it needs to be updated for each FrameResource.
        Material* mat = e.second.get();
        if (mat->NumFramesDirty > 0)
        {
            XMMATRIX matTransform = XMLoadFloat4x4(&mat->MatTransform);

            MaterialConstants matConstants;
            matConstants.DiffuseAlbedo = mat->DiffuseAlbedo;
            matConstants.FresnelR0 = mat->FresnelR0;
            matConstants.Roughness = mat->Roughness;
            matConstants.DisplacementScale = mat->DisplacementScale;
            matConstants.MinTessDistance = mat->MinTessDistance;
            matConstants.MaxTessDistance = mat->MaxTessDistance;
            matConstants.MinTessFactor = mat->MinTessFactor;
            matConstants.MaxTessFactor = mat->MaxTessFactor;
            matConstants.UseNormalMap = mat->NormalTexturePath.empty() ? 0 : 1;
            matConstants.UseDisplacementMap = mat->DisplacementTexturePath.empty() ? 0 : 1;
            XMStoreFloat4x4(&matConstants.MatTransform, XMMatrixTranspose(matTransform));

            currMaterialCB->CopyData(mat->MatCBIndex, matConstants);

            // Next FrameResource need to be updated too.
            mat->NumFramesDirty--;
        }
    }
}

void CrateApp::UpdateMainPassCB(const GameTimer& gt)
{
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
    XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
    XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
    XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
    XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
    XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
    XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
    mMainPassCB.EyePosW = mEyePos;
    mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
    mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
    mMainPassCB.NearZ = 1.0f;
    mMainPassCB.FarZ = 5000.0f;
    mMainPassCB.TotalTime = gt.TotalTime();
    mMainPassCB.DeltaTime = gt.DeltaTime();

    auto currPassCB = mCurrFrameResource->PassCB.get();
    currPassCB->CopyData(0, mMainPassCB);

}

void CrateApp::UpdateLightCB(const GameTimer& gt)
{
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);
    XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

    XMStoreFloat4x4(&mLightCB.InvViewProj, XMMatrixTranspose(invViewProj));
    mLightCB.EyePosW = mEyePos;
    mLightCB.AmbientStrength = 0.16f;
    mLightCB.Directional = mDirectionalLight;
    mLightCB.PointLightCount = (int)std::min<size_t>(mPointLights.size(), MaxPointLights);
    mLightCB.SpotLightCount = (int)std::min<size_t>(mSpotLights.size(), MaxSpotLights);

    for (int i = 0; i < mLightCB.PointLightCount; ++i)
        mLightCB.PointLights[i] = mPointLights[i];

    for (int i = 0; i < mLightCB.SpotLightCount; ++i)
        mLightCB.SpotLights[i] = mSpotLights[i];

    auto currLightCB = mCurrFrameResource->LightCB.get();
    currLightCB->CopyData(0, mLightCB);
}

void CrateApp::LoadTextures()
{
}

void CrateApp::InitializeImGui()
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 1;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&heapDesc,
        IID_PPV_ARGS(mImGuiSrvDescriptorHeap.GetAddressOf())));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplDX12_InitInfo initInfo;
    initInfo.Device = md3dDevice.Get();
    initInfo.CommandQueue = mCommandQueue.Get();
    initInfo.NumFramesInFlight = gNumFrameResources;
    initInfo.RTVFormat = mBackBufferFormat;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = mImGuiSrvDescriptorHeap.Get();
    initInfo.LegacySingleSrvCpuDescriptor = mImGuiSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    initInfo.LegacySingleSrvGpuDescriptor = mImGuiSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();

    if (!ImGui_ImplDX12_Init(&initInfo) || !ImGui_ImplWin32_Init(mhMainWnd))
        throw std::runtime_error("Failed to initialize Dear ImGui.");
}

void CrateApp::DrawImGui()
{
    ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(240.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene Models");
    ImGui::TextUnformatted("Show or hide imported models:");
    ImGui::Separator();

    for (LoadedModel& model : mModels)
        ImGui::Checkbox(model.Name.c_str(), &model.Visible);

    ImGui::Checkbox("Scattered objects", &mShowScatteredObjects);
    ImGui::Text("Scattered population: %u", ScatterObjectCount);

    ImGui::Separator();
    ImGui::TextUnformatted("Visibility culling:");
    ImGui::Checkbox("Frustum culling", &mEnableFrustumCulling);
    ImGui::Checkbox("Frustum culling with octree", &mEnableOctreeCulling);

    const char* cullingMode = mEnableOctreeCulling ? "Octree" :
        (mEnableFrustumCulling ? "Linear" : "Disabled");
    ImGui::Text("Active mode: %s", cullingMode);
    ImGui::Text("Submitted: %u / %zu", mSubmittedObjectCount, mOpaqueRitems.size());
    ImGui::Text("Culled: %u", mCulledObjectCount);
    ImGui::Text("Geometry draw calls: %u", mGeometryDrawCallCount);
    ImGui::Text("Boxes in instanced draw: %u", mScatteredInstancesSubmitted);
    if (mEnableOctreeCulling)
        ImGui::Text("Octree nodes tested: %u", mOctreeNodesTested);

    ImGui::Separator();
    if (ImGui::SliderFloat("Walnut displacement", &mWalnutDisplacementScale,
        0.0f, 4.0f, "%.2f"))
    {
        for (const LoadedModel& model : mModels)
        {
            if (!model.UseDetailMaps)
                continue;

            for (UINT materialIndex = 0; materialIndex < model.Scene->mNumMaterials; ++materialIndex)
            {
                const std::string materialName = "mat" +
                    std::to_string(model.MaterialOffset + materialIndex);
                Material* material = mMaterials.at(materialName).get();
                material->DisplacementScale = mWalnutDisplacementScale;
                material->NumFramesDirty = gNumFrameResources;
            }
        }
    }
    ImGui::TextDisabled("0 = flat, 4 = exaggerated");
    ImGui::Separator();
    ImGui::TextDisabled("Right mouse: zoom");
    ImGui::TextDisabled("Left mouse: orbit");
    ImGui::End();
}

void CrateApp::BuildDescriptorHeaps()
{
    constexpr UINT texturesPerMaterial = 3;
    UINT numDescriptors = (UINT)mMaterials.size() * texturesPerMaterial;
    if (numDescriptors == 0)
        numDescriptors = 1;

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = numDescriptors;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));

    CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

    Texture* fallbackTexPtr = _textureLoader.GetDefaultTexture();
    if (fallbackTexPtr == nullptr)
        throw std::runtime_error("Fallback texture not found.");

    auto fallbackTex = fallbackTexPtr->Resource;

    UINT index = 0;
    for (auto& pair : mMaterials)
    {
        Material* mat = pair.second.get();
        const std::string paths[texturesPerMaterial] =
        {
            mat->TexturePath,
            mat->NormalTexturePath,
            mat->DisplacementTexturePath
        };

        mat->DiffuseSrvHeapIndex = index;
        mat->NormalSrvHeapIndex = index + 1;
        mat->DisplacementSrvHeapIndex = index + 2;

        for (const std::string& path : paths)
        {
            ID3D12Resource* texResource = fallbackTex.Get();
            if (!path.empty())
            {
                std::string fullPath = mat->TextureDirectory + path;
                Texture* texPtr = _textureLoader.LoadTexture(std::wstring(fullPath.begin(), fullPath.end()));
                if (!texPtr)
                    throw std::runtime_error("Failed to load texture: " + fullPath);
                texResource = texPtr->Resource.Get();
            }

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = texResource->GetDesc().Format;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = texResource->GetDesc().MipLevels;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
            md3dDevice->CreateShaderResourceView(texResource, &srvDesc, hDescriptor);

            ++index;
            hDescriptor.Offset(1, mCbvSrvDescriptorSize);
        }
    }
}


void CrateApp::BuildShadersAndInputLayout()
{
    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void CrateApp::BuildShapeGeometry()
{
    if (mModels.empty())
        throw std::runtime_error("No models loaded.");

    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "objGeo";

    for (UINT modelIndex = 0; modelIndex < (UINT)mModels.size(); ++modelIndex)
    {
        const LoadedModel& model = mModels[modelIndex];
        for (UINT meshIndex = 0; meshIndex < model.Scene->mNumMeshes; ++meshIndex)
        {
            aiMesh* mesh = model.Scene->mMeshes[meshIndex];

            SubmeshGeometry submesh;
            submesh.BaseVertexLocation = (UINT)vertices.size();
            submesh.StartIndexLocation = (UINT)indices.size();
            submesh.IndexCount = mesh->mNumFaces * 3;
            submesh.MaterialIndex = model.MaterialOffset + mesh->mMaterialIndex;
            submesh.ModelIndex = modelIndex;

            for (UINT i = 0; i < mesh->mNumVertices; ++i)
            {
                Vertex v;
                v.Pos = { mesh->mVertices[i].x, mesh->mVertices[i].y, mesh->mVertices[i].z };
                v.Normal = { mesh->mNormals[i].x, mesh->mNormals[i].y, mesh->mNormals[i].z };

                if (mesh->HasTangentsAndBitangents())
                {
                    XMVECTOR normal = XMLoadFloat3(&v.Normal);
                    XMVECTOR tangent = XMVectorSet(mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z, 0.0f);
                    XMVECTOR bitangent = XMVectorSet(mesh->mBitangents[i].x, mesh->mBitangents[i].y, mesh->mBitangents[i].z, 0.0f);
                    float handedness = XMVectorGetX(XMVector3Dot(XMVector3Cross(normal, tangent), bitangent)) < 0.0f ? -1.0f : 1.0f;
                    v.TangentU = { mesh->mTangents[i].x, mesh->mTangents[i].y, mesh->mTangents[i].z, handedness };
                }
                else
                    v.TangentU = { 1.0f, 0.0f, 0.0f, 1.0f };

                if (mesh->HasTextureCoords(0))
                    v.TexC = { mesh->mTextureCoords[0][i].x, mesh->mTextureCoords[0][i].y };
                else
                    v.TexC = { 0.0f, 0.0f };

                vertices.push_back(v);
            }

            BoundingBox::CreateFromPoints(submesh.Bounds, mesh->mNumVertices,
                &vertices[submesh.BaseVertexLocation].Pos, sizeof(Vertex));

            for (UINT i = 0; i < mesh->mNumFaces; ++i)
            {
                const aiFace& face = mesh->mFaces[i];
                for (UINT j = 0; j < face.mNumIndices; ++j)
                    indices.push_back(face.mIndices[j]);
            }

            const std::string submeshName = "model" + std::to_string(modelIndex) +
                "_mesh" + std::to_string(meshIndex);
            geo->DrawArgs[submeshName] = submesh;
        }
    }

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    mGeometries[geo->Name] = std::move(geo);
}

void CrateApp::BuildLightVolumeGeometry()
{
    GeometryGenerator geoGen;
    GeometryGenerator::MeshData box = geoGen.CreateBox(2.0f, 2.0f, 2.0f, 0);

    std::vector<Vertex> vertices(box.Vertices.size());
    for (size_t i = 0; i < box.Vertices.size(); ++i)
    {
        vertices[i].Pos = box.Vertices[i].Position;
        vertices[i].Normal = box.Vertices[i].Normal;
        vertices[i].TangentU = { box.Vertices[i].TangentU.x, box.Vertices[i].TangentU.y,
            box.Vertices[i].TangentU.z, 1.0f };
        vertices[i].TexC = box.Vertices[i].TexC;
    }

    const auto& indices = box.Indices32;
    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size() * sizeof(std::uint32_t);

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "lightVolumeGeo";

    ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
    CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

    ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
    CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        vertices.data(), vbByteSize, geo->VertexBufferUploader);

    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(), mCommandList.Get(),
        indices.data(), ibByteSize, geo->IndexBufferUploader);

    geo->VertexByteStride = sizeof(Vertex);
    geo->VertexBufferByteSize = vbByteSize;
    geo->IndexFormat = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize = ibByteSize;

    SubmeshGeometry submesh;
    submesh.IndexCount = (UINT)indices.size();
    submesh.StartIndexLocation = 0;
    submesh.BaseVertexLocation = 0;
    submesh.Bounds = BoundingBox({ 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f });

    geo->DrawArgs["box"] = submesh;
    mGeometries[geo->Name] = std::move(geo);
}

void CrateApp::BuildFrameResources()
{
    for (int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size(), (UINT)mMaterials.size(), ScatterObjectCount));
    }
}

void CrateApp::BuildMaterials()
{
    if (mModels.empty())
        throw std::runtime_error("No models loaded.");

    for (const LoadedModel& model : mModels)
    {
        for (UINT i = 0; i < model.Scene->mNumMaterials; ++i)
        {
            aiMaterial* aiMat = model.Scene->mMaterials[i];
            const UINT globalIndex = model.MaterialOffset + i;

            auto mat = std::make_unique<Material>();
            mat->Name = "mat" + std::to_string(globalIndex);
            mat->MatCBIndex = globalIndex;
            mat->TextureDirectory = model.Directory;
            mat->DiffuseSrvHeapIndex = 0;
            mat->DiffuseAlbedo = XMFLOAT4(1, 1, 1, 1);
            mat->FresnelR0 = XMFLOAT3(0.04f, 0.04f, 0.04f);
            mat->Roughness = 0.5f;
            mat->MatTransform = MathHelper::Identity4x4();

            aiColor3D diffuse(1.f, 1.f, 1.f);
            if (aiMat->Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) == AI_SUCCESS)
                mat->DiffuseAlbedo = XMFLOAT4(diffuse.r, diffuse.g, diffuse.b, 1.0f);

            aiString texPath;
            if (aiMat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS)
                mat->TexturePath = texPath.C_Str();

            if (model.UseDetailMaps)
            {
                if (aiMat->GetTexture(aiTextureType_NORMALS, 0, &texPath) == AI_SUCCESS)
                    mat->NormalTexturePath = texPath.C_Str();

                if (aiMat->GetTexture(aiTextureType_DISPLACEMENT, 0, &texPath) == AI_SUCCESS ||
                    aiMat->GetTexture(aiTextureType_HEIGHT, 0, &texPath) == AI_SUCCESS)
                    mat->DisplacementTexturePath = texPath.C_Str();

                // Explicit fallbacks keep the assignment maps active across Assimp MTL variants.
                if (mat->NormalTexturePath.empty())
                    mat->NormalTexturePath = "walnut_normal.tiff";
                if (mat->DisplacementTexturePath.empty())
                    mat->DisplacementTexturePath = "walnut_displacement.dds";

                mat->DisplacementScale = mWalnutDisplacementScale;
                mat->MinTessDistance = 20.0f;
                // The camera frames both models, so retain visible detail at the initial radius.
                mat->MaxTessDistance = 160.0f;
                mat->MinTessFactor = 1.0f;
                mat->MaxTessFactor = 16.0f;
            }
            else
            {
                // Sponza stays at one generated triangle per source triangle.
                mat->DisplacementScale = 0.0f;
                mat->MinTessFactor = 1.0f;
                mat->MaxTessFactor = 1.0f;
            }

            mMaterials[mat->Name] = std::move(mat);
        }
    }
}

void CrateApp::BuildLights()
{
    mDirectionalLight.Direction = { 0.35f, -1.0f, 0.2f };
    mDirectionalLight.Color = { 1.0f, 0.96f, 0.88f };
    mDirectionalLight.Intensity = 1.1f;

    const PointLight pointLights[] =
    {
        { { -80.0f, 30.0f, -40.0f }, 48.0f, { 1.0f, 0.35f, 0.18f }, 1.35f },
        { {   0.0f, 40.0f,  60.0f }, 55.0f, { 0.20f, 0.55f, 1.0f }, 1.45f },
        { {  80.0f, 30.0f, -20.0f }, 48.0f, { 0.35f, 1.0f, 0.55f }, 1.25f },
        { { -30.0f, 60.0f, 110.0f }, 42.0f, { 1.0f, 0.85f, 0.30f }, 1.25f },
        { {  50.0f, 25.0f,-100.0f }, 38.0f, { 0.95f, 0.30f, 1.0f }, 1.15f }
    };

    mPointLights.assign(pointLights, pointLights + 5);

    SpotLight spot;
    spot.Position = { 0.0f, 95.0f, 5.0f };
    spot.Direction = { 0.15f, -1.0f, 0.35f };
    spot.Radius = 120.0f;
    spot.SpotPower = 18.0f;
    spot.Color = { 1.0f, 0.86f, 0.55f };
    spot.Intensity = 5.0f;
    mSpotLights.push_back(spot);

}

void CrateApp::BuildRenderItems()
{
    auto geo = mGeometries["objGeo"].get();
    int objIndex = 0;

    for (auto& pair : geo->DrawArgs)
    {
        SubmeshGeometry& submesh = pair.second;

        auto ritem = std::make_unique<RenderItem>();
        ritem->World = mModels.at(submesh.ModelIndex).World;
        ritem->ObjCBIndex = objIndex++;
        ritem->ModelIndex = submesh.ModelIndex;
        ritem->Geo = geo;
        ritem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        ritem->IndexCount = submesh.IndexCount;
        ritem->StartIndexLocation = submesh.StartIndexLocation;
        ritem->BaseVertexLocation = submesh.BaseVertexLocation;

        submesh.Bounds.Transform(ritem->WorldBounds, XMLoadFloat4x4(&ritem->World));

        std::string matName = "mat" + std::to_string(submesh.MaterialIndex);
        auto matIter = mMaterials.find(matName);
        if (matIter != mMaterials.end())
            ritem->Mat = matIter->second.get();
        else
            ritem->Mat = mMaterials.begin()->second.get();

        mAllRitems.push_back(std::move(ritem));
    }

    // A regular, deterministic field makes culling differences easy to see and
    // keeps the assignment reproducible.  The box geometry is shared; only the
    // per-object transform and constant-buffer slot are unique.
    MeshGeometry* scatterGeo = mGeometries.at("lightVolumeGeo").get();
    const SubmeshGeometry& scatterSubmesh = scatterGeo->DrawArgs.at("box");
    Material* scatterMaterial = mMaterials.at("mat0").get();
    constexpr UINT sideLength = 32;
    constexpr float spacing = 16.0f;

    for (UINT z = 0; z < sideLength; ++z)
    {
        for (UINT x = 0; x < sideLength; ++x)
        {
            const float worldX = (static_cast<float>(x) - 0.5f * (sideLength - 1)) * spacing;
            const float worldZ = (static_cast<float>(z) - 0.5f * (sideLength - 1)) * spacing;
            const float scale = 1.4f + 0.35f * sinf(static_cast<float>(x * 13 + z * 7));
            const float worldY = -3.0f + 2.0f * sinf(static_cast<float>(x) * 0.47f) *
                cosf(static_cast<float>(z) * 0.39f);

            auto ritem = std::make_unique<RenderItem>();
            XMMATRIX world = XMMatrixScaling(scale, scale, scale) *
                XMMatrixRotationY(0.31f * static_cast<float>(x + z)) *
                XMMatrixTranslation(worldX, worldY, worldZ);
            XMStoreFloat4x4(&ritem->World, world);
            ritem->ObjCBIndex = objIndex++;
            ritem->ModelIndex = (std::numeric_limits<UINT>::max)();
            ritem->Geo = scatterGeo;
            ritem->Mat = scatterMaterial;
            ritem->PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
            ritem->IndexCount = scatterSubmesh.IndexCount;
            ritem->StartIndexLocation = scatterSubmesh.StartIndexLocation;
            ritem->BaseVertexLocation = scatterSubmesh.BaseVertexLocation;
            ritem->IsScatteredObject = true;
            scatterSubmesh.Bounds.Transform(ritem->WorldBounds, world);
            mAllRitems.push_back(std::move(ritem));
        }
    }

    for (auto& e : mAllRitems)
        mOpaqueRitems.push_back(e.get());

    mVisibleRitems.reserve(mOpaqueRitems.size());
}

void CrateApp::BuildOctree()
{
    if (mOpaqueRitems.empty())
        return;

    BoundingBox sceneBounds = mOpaqueRitems.front()->WorldBounds;
    for (size_t i = 1; i < mOpaqueRitems.size(); ++i)
        BoundingBox::CreateMerged(sceneBounds, sceneBounds, mOpaqueRitems[i]->WorldBounds);

    const float halfSize = (std::max)(sceneBounds.Extents.x,
        (std::max)(sceneBounds.Extents.y, sceneBounds.Extents.z));
    sceneBounds.Extents = { halfSize, halfSize, halfSize };
    mOctreeRoot = BuildOctreeNode(sceneBounds, mOpaqueRitems, 0);
}

std::unique_ptr<OctreeNode> CrateApp::BuildOctreeNode(const BoundingBox& bounds,
    const std::vector<RenderItem*>& items, UINT depth)
{
    auto node = std::make_unique<OctreeNode>();
    node->Bounds = bounds;
    if (items.size() <= OctreeLeafCapacity || depth >= OctreeMaxDepth)
    {
        node->Items = items;
        return node;
    }

    const XMFLOAT3 childExtents = { bounds.Extents.x * 0.5f,
        bounds.Extents.y * 0.5f, bounds.Extents.z * 0.5f };
    std::array<BoundingBox, 8> childBounds;
    std::array<std::vector<RenderItem*>, 8> childItems;

    for (UINT i = 0; i < 8; ++i)
    {
        const XMFLOAT3 offset = {
            (i & 1) ? childExtents.x : -childExtents.x,
            (i & 2) ? childExtents.y : -childExtents.y,
            (i & 4) ? childExtents.z : -childExtents.z
        };
        childBounds[i] = BoundingBox(
            { bounds.Center.x + offset.x, bounds.Center.y + offset.y, bounds.Center.z + offset.z },
            childExtents);
    }

    for (RenderItem* item : items)
    {
        UINT childIndex = 0;
        if (item->WorldBounds.Center.x >= bounds.Center.x) childIndex |= 1;
        if (item->WorldBounds.Center.y >= bounds.Center.y) childIndex |= 2;
        if (item->WorldBounds.Center.z >= bounds.Center.z) childIndex |= 4;

        if (childBounds[childIndex].Contains(item->WorldBounds) == CONTAINS)
            childItems[childIndex].push_back(item);
        else
            node->Items.push_back(item);
    }

    for (UINT i = 0; i < 8; ++i)
    {
        if (!childItems[i].empty())
            node->Children[i] = BuildOctreeNode(childBounds[i], childItems[i], depth + 1);
    }

    return node;
}

void CrateApp::UpdateWorldFrustum()
{
    BoundingFrustum viewFrustum;
    BoundingFrustum::CreateFromMatrix(viewFrustum, XMLoadFloat4x4(&mProj));
    XMMATRIX view = XMLoadFloat4x4(&mView);
    XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
    viewFrustum.Transform(mWorldFrustum, inverseView);
}

bool CrateApp::IsRenderItemEnabled(const RenderItem& item) const
{
    if (item.IsScatteredObject)
        return mShowScatteredObjects;
    return item.ModelIndex < mModels.size() && mModels[item.ModelIndex].Visible;
}

void CrateApp::CollectAllOctreeItems(const OctreeNode& node)
{
    for (RenderItem* item : node.Items)
    {
        if (IsRenderItemEnabled(*item))
            mVisibleRitems.push_back(item);
    }
    for (const auto& child : node.Children)
    {
        if (child)
            CollectAllOctreeItems(*child);
    }
}

void CrateApp::CollectOctreeItems(const OctreeNode& node, ContainmentType parentContainment)
{
    ++mOctreeNodesTested;
    const ContainmentType containment = parentContainment == CONTAINS ? CONTAINS :
        mWorldFrustum.Contains(node.Bounds);
    if (containment == DISJOINT)
        return;
    if (containment == CONTAINS)
    {
        CollectAllOctreeItems(node);
        return;
    }

    for (RenderItem* item : node.Items)
    {
        if (IsRenderItemEnabled(*item) && mWorldFrustum.Contains(item->WorldBounds) != DISJOINT)
            mVisibleRitems.push_back(item);
    }
    for (const auto& child : node.Children)
    {
        if (child)
            CollectOctreeItems(*child, INTERSECTS);
    }
}

void CrateApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
    UINT matCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(MaterialConstants));

    auto objectCB = mCurrFrameResource->ObjectCB->Resource();
    auto matCB = mCurrFrameResource->MaterialCB->Resource();
    auto instanceBuffer = mCurrFrameResource->InstanceBuffer->Resource();
    cmdList->SetGraphicsRootShaderResourceView(4, instanceBuffer->GetGPUVirtualAddress());

    mVisibleRitems.clear();
    mOctreeNodesTested = 0;

    if (mEnableOctreeCulling && mOctreeRoot)
    {
        CollectOctreeItems(*mOctreeRoot, INTERSECTS);
    }
    else
    {
        for (RenderItem* item : ritems)
        {
            if (!IsRenderItemEnabled(*item))
                continue;
            if (!mEnableFrustumCulling || mWorldFrustum.Contains(item->WorldBounds) != DISJOINT)
                mVisibleRitems.push_back(item);
        }
    }

    mSubmittedObjectCount = static_cast<UINT>(mVisibleRitems.size());
    UINT enabledCount = 0;
    for (RenderItem* item : ritems)
        enabledCount += IsRenderItemEnabled(*item) ? 1u : 0u;
    mCulledObjectCount = enabledCount - mSubmittedObjectCount;
    mGeometryDrawCallCount = 0;
    mScatteredInstancesSubmitted = 0;

    auto bindRenderItem = [&](RenderItem* ri)
    {
        auto vertexBufferView = ri->Geo->VertexBufferView();
        auto indexBufferView = ri->Geo->IndexBufferView();
        cmdList->IASetVertexBuffers(0, 1, &vertexBufferView);
        cmdList->IASetIndexBuffer(&indexBufferView);
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        D3D12_GPU_DESCRIPTOR_HANDLE tex = mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
        tex.ptr += static_cast<UINT64>(ri->Mat->DiffuseSrvHeapIndex) * mCbvSrvDescriptorSize;

        D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + ri->ObjCBIndex * objCBByteSize;
        D3D12_GPU_VIRTUAL_ADDRESS matCBAddress = matCB->GetGPUVirtualAddress() + ri->Mat->MatCBIndex * matCBByteSize;

        cmdList->SetGraphicsRootDescriptorTable(0, tex);
        cmdList->SetGraphicsRootConstantBufferView(1, objCBAddress);
        cmdList->SetGraphicsRootConstantBufferView(3, matCBAddress);
    };

    RenderItem* scatteredPrototype = nullptr;
    UINT scatteredInstanceCount = 0;

    // Imported meshes retain their existing per-object draws.  Visible scattered
    // objects are compacted into a contiguous GPU buffer for one batched draw.
    for (RenderItem* ri : mVisibleRitems)
    {
        if (ri->IsScatteredObject)
        {
            if (scatteredPrototype == nullptr)
                scatteredPrototype = ri;

            InstanceData instanceData;
            XMMATRIX world = XMLoadFloat4x4(&ri->World);
            XMStoreFloat4x4(&instanceData.World, XMMatrixTranspose(world));
            mCurrFrameResource->InstanceBuffer->CopyData(scatteredInstanceCount++, instanceData);
            continue;
        }

        bindRenderItem(ri);
        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
        ++mGeometryDrawCallCount;
    }

    if (scatteredPrototype != nullptr && scatteredInstanceCount > 0)
    {
        bindRenderItem(scatteredPrototype);
        cmdList->DrawIndexedInstanced(scatteredPrototype->IndexCount, scatteredInstanceCount,
            scatteredPrototype->StartIndexLocation, scatteredPrototype->BaseVertexLocation, 0);
        mScatteredInstancesSubmitted = scatteredInstanceCount;
        ++mGeometryDrawCallCount;
    }
}

std::vector<CD3DX12_STATIC_SAMPLER_DESC> CrateApp::GetStaticSamplers()
{
    // Applications usually only need a handful of samplers.  So just define them all up front
    // and keep them available as part of the root signature.  

    const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
        0, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
        1, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
        2, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        3, // shaderRegister
        D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
        4, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
        0.0f,                             // mipLODBias
        8);                               // maxAnisotropy

    const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
        5, // shaderRegister
        D3D12_FILTER_ANISOTROPIC, // filter
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
        0.0f,                              // mipLODBias
        8);                                // maxAnisotropy

    return {
        pointWrap, pointClamp,
        linearWrap, linearClamp,
        anisotropicWrap, anisotropicClamp };
}

