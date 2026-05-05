#include "TextureUploader.h"
#include "Helpers.h"
#include "Common/d3dUtil.h"
#include "Common/d3dx12.h"
#include "DirectXTex.h"

TextureUploader::TextureUploader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList) {
	_device = device;
	_uploadCmdList = commandList;
    _defaultTextureKey = LoadTexture(L"../../Textures/2024-11-13_16-44-50.png")->Name;
}


Texture* TextureUploader::LoadTexture(const std::wstring& filename)
{
    auto tex = std::make_unique<Texture>();
    tex->Name = Helpers::GetNameFromFileName(filename);
    tex->Filename = filename;
    Texture* texPtr = tex.get();
    if (CreateTexture(texPtr) == false)
    {
        OutputDebugStringA(("Failed to load texture: " + std::string(filename.begin(), filename.end()) + ". Using fallback texture.\n").c_str());
        return _textures[_defaultTextureKey].get();
    }

    _textures[tex->Name] = std::move(tex);

    return texPtr;
}

Texture* TextureUploader::GetDefaultTexture() const
{
    return _textures.at(_defaultTextureKey).get();
}

Texture* TextureUploader::GetTexture(const std::string& name) const
{
    auto iter = _textures.find(name);
    if (iter != _textures.end())
        return iter->second.get();
    else
        return nullptr;
}

bool TextureUploader::CreateTexture(Texture* tex)
{
    std::wstring ext = tex->Filename.substr(tex->Filename.find_last_of(L'.') + 1);
    for (auto& c : ext) c = towlower(c);

    if (ext == L"dds")
    {
        // Original path
        ThrowIfFailed(DirectX::CreateDDSTextureFromFile12(_device,
            _uploadCmdList, tex->Filename.c_str(),
            tex->Resource, tex->UploadHeap));
    }
    else
    {
        DirectX::TexMetadata metadata;
        DirectX::ScratchImage scratch;

        HRESULT res;

        if (ext == L"tga") {

            res = DirectX::LoadFromTGAFile(
                tex->Filename.c_str(),
                &metadata,
                scratch
            );
        }
        else
        {
            res = DirectX::LoadFromWICFile(
                tex->Filename.c_str(),
                DirectX::WIC_FLAGS_FORCE_RGB, // or _SRGB/_NONE if you care
                &metadata,
                scratch);
        }

        if (FAILED(res))
        {
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> texture;
        const CD3DX12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
            metadata.format,
            static_cast<UINT>(metadata.width),
            static_cast<UINT>(metadata.height),
            static_cast<UINT16>(metadata.arraySize),
            static_cast<UINT16>(metadata.mipLevels)
        );

        const auto heapPropertiesDefault = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ThrowIfFailed(_device->CreateCommittedResource(
            &heapPropertiesDefault,
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture)));

        UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture.Get(), 0, static_cast<UINT>(metadata.mipLevels));

        Microsoft::WRL::ComPtr<ID3D12Resource> textureUploadHeap;
        const auto heapPropertiesUpload = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        const auto buffer = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
        ThrowIfFailed(_device->CreateCommittedResource(
            &heapPropertiesUpload,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&textureUploadHeap)));

        ThrowIfFailed(_device->CreateCommittedResource(
            &heapPropertiesUpload,
            D3D12_HEAP_FLAG_NONE,
            &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&tex->UploadHeap)
        ));

        const UINT numSubresources = static_cast<UINT>(metadata.mipLevels * metadata.arraySize);
        std::vector<D3D12_SUBRESOURCE_DATA> subresources;
        subresources.reserve(numSubresources);

        const DirectX::Image* images = scratch.GetImages();
        for (UINT i = 0; i < numSubresources; ++i)
        {
            D3D12_SUBRESOURCE_DATA sr = {};
            sr.pData = images[i].pixels;
            sr.RowPitch = static_cast<long long>(images[i].rowPitch);
            sr.SlicePitch = static_cast<long long>(images[i].slicePitch);
            subresources.push_back(sr);
        }

        tex->Resource = texture;

        UpdateSubresources(_uploadCmdList, tex->Resource.Get(), tex->UploadHeap.Get(),
            0, 0, numSubresources, subresources.data());

        const auto resourceBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            tex->Resource.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_GENERIC_READ
        );
        _uploadCmdList->ResourceBarrier(1, &resourceBarrier);
    }
    return true;
}

