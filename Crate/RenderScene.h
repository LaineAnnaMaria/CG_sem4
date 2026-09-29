#pragma once

#include "Common/d3dUtil.h"

struct RenderItem
{
    DirectX::XMFLOAT4X4 World = MathHelper::Identity4x4();
    DirectX::XMFLOAT4X4 TexTransform = MathHelper::Identity4x4();
    int NumFramesDirty = 3;
    UINT ObjCBIndex = UINT_MAX;
    Material* Mat = nullptr;
    MeshGeometry* Geo = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
    UINT ModelIndex = 0;
    DirectX::BoundingBox WorldBounds;
    DirectX::XMFLOAT4 InstanceColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    float InstanceSize = 1.0f;
    bool IsScatteredObject = false;
    const bool* Visibility = nullptr;

    bool IsVisible() const { return Visibility == nullptr || *Visibility; }
};
