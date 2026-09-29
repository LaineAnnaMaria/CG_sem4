#pragma once
#include "d3dUtil.h"

enum class GBufferInfo
{
	Diffuse = 0,
	Normals,
	Depth,
	Count
};

struct GBufferTexture
{
	Microsoft::WRL::ComPtr<ID3D12Resource> Resource = nullptr;
	D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle = {};
	D3D12_CPU_DESCRIPTOR_HANDLE SrvHandle = {};
	D3D12_RESOURCE_STATES prevState = D3D12_RESOURCE_STATE_COMMON;

	void Reset()
	{
		Resource.Reset();
		RtvHandle.ptr = 0;
		SrvHandle.ptr = 0;
	}
};

class GBuffer
{
public:
	// Descriptor layout is fixed so every pass agrees on the exact slots.
	static constexpr UINT RenderTargetCount = static_cast<UINT>(GBufferInfo::Count) - 1;
	static constexpr UINT ShadowSrvIndex = static_cast<UINT>(GBufferInfo::Count);
	static constexpr UINT SceneColorRtvIndex = RenderTargetCount;
	static constexpr UINT SceneColorSrvIndex = ShadowSrvIndex + 1;
	static constexpr UINT RtvDescriptorCount = SceneColorRtvIndex + 1;
	static constexpr UINT SrvDescriptorCount = SceneColorSrvIndex + 1;

	GBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, int width, int height);
	~GBuffer();
	//onresize
	void OnResize(int width, int height);

	void CreateGBufferTexture(int i, CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle, CD3DX12_CPU_DESCRIPTOR_HANDLE srvHeapHandle,
		CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHeapHandle);

	D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView();

	void ClearInfo(const FLOAT* color);

	static int InfoCount(bool forRTVS = true)
	{
		return (int)GBufferInfo::Count - (int)forRTVS;
	}

	void ChangeRTVsState(D3D12_RESOURCE_STATES stateAfter);
	void ChangeDSVState(D3D12_RESOURCE_STATES stateAfter);

	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> RTVs() const;

	ID3D12DescriptorHeap* SRVHeap()
	{
		return _infoSRVHeap.Get();
	}

	static constexpr DXGI_FORMAT infoFormats[(int)GBufferInfo::Count] = {
	DXGI_FORMAT_R8G8B8A8_UNORM,        // Diffuse
	DXGI_FORMAT_R16G16B16A16_FLOAT,    // Normals
	DXGI_FORMAT_R24G8_TYPELESS         // Depth
	};

	D3D12_CPU_DESCRIPTOR_HANDLE lightingHandle() const
	{
		return _srvHandleForLighting;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE SceneColorRtv() const
	{
		auto handle = _infoRTVHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(SceneColorRtvIndex) * _rtvDescriptorSize;
		return handle;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE SceneColorSrvCpu() const
	{
		auto handle = _infoSRVHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<SIZE_T>(SceneColorSrvIndex) * _srvDescriptorSize;
		return handle;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE SceneColorSrvGpu() const
	{
		auto handle = _infoSRVHeap->GetGPUDescriptorHandleForHeapStart();
		handle.ptr += static_cast<UINT64>(SceneColorSrvIndex) * _srvDescriptorSize;
		return handle;
	}

private:
	int _height;
	int _width;
	UINT _rtvDescriptorSize;
	UINT _srvDescriptorSize;
	UINT _dsvDescriptorSize;

	GBufferTexture _info[(int)GBufferInfo::Count];

	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _infoRTVHeap;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _infoSRVHeap;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> _infoDSVHeap;
	D3D12_CPU_DESCRIPTOR_HANDLE _srvHandleForLighting;

	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> _cmdList;

	Microsoft::WRL::ComPtr<ID3D12Device> _device;
};
