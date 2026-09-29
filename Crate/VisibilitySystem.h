#pragma once

#include "RenderScene.h"

class VisibilitySystem
{
public:
    struct Stats
    {
        UINT SubmittedObjects = 0;
        UINT CulledObjects = 0;
        UINT OctreeNodesTested = 0;
    };

    void Initialize(const std::vector<std::unique_ptr<RenderItem>>& items);
    const std::vector<RenderItem*>& Cull(const DirectX::XMFLOAT4X4& view,
        const DirectX::XMFLOAT4X4& projection);
    const std::vector<RenderItem*>& ShadowCasters();
    const DirectX::BoundingBox& SceneBounds() const { return _sceneBounds; }

    bool& FrustumCullingEnabled() { return _enableFrustumCulling; }
    bool& OctreeCullingEnabled() { return _enableOctreeCulling; }
    const Stats& CurrentStats() const { return _stats; }

private:
    struct OctreeNode
    {
        DirectX::BoundingBox Bounds;
        std::vector<RenderItem*> Items;
        std::array<std::unique_ptr<OctreeNode>, 8> Children;
    };

    static constexpr UINT LeafCapacity = 16;
    static constexpr UINT MaxDepth = 6;

    std::unique_ptr<OctreeNode> BuildNode(const DirectX::BoundingBox& bounds,
        const std::vector<RenderItem*>& items, UINT depth);
    void Collect(const OctreeNode& node, DirectX::ContainmentType parentContainment);
    void CollectAll(const OctreeNode& node);

    std::vector<RenderItem*> _items;
    std::vector<RenderItem*> _visibleItems;
    std::vector<RenderItem*> _shadowCasters;
    std::unique_ptr<OctreeNode> _root;
    DirectX::BoundingBox _sceneBounds;
    DirectX::BoundingFrustum _worldFrustum;
    bool _enableFrustumCulling = true;
    bool _enableOctreeCulling = false;
    Stats _stats;
};
