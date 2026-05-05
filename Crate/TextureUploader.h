#pragma once
#include <unordered_map>
#include <string>
#include <memory>
#include <wrl.h>
#include <d3d12.h>
#include "Common/d3dUtil.h"
#include <DirectXTex.h>

class TextureUploader {

public:
	TextureUploader(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);

	TextureUploader() = default;

	Texture* LoadTexture(const std::wstring& filename);
	Texture* GetDefaultTexture() const;
	Texture* GetTexture(const std::string& name) const;

private:
	std::string _defaultTextureKey;
	bool CreateTexture(Texture* tex);
	ID3D12Device* _device;
	ID3D12GraphicsCommandList* _uploadCmdList;
	std::unordered_map<std::string, std::unique_ptr<Texture>> _textures{};

};