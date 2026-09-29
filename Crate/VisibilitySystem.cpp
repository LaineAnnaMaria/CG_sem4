#include "VisibilitySystem.h"

using namespace DirectX;

void VisibilitySystem::Initialize(const std::vector<std::unique_ptr<RenderItem>>& items)
{
    _items.clear();
    _items.reserve(items.size());
    for (const auto& item : items)
        _items.push_back(item.get());
    _visibleItems.reserve(_items.size());
    _shadowCasters.reserve(_items.size());

    if (_items.empty())
        return;
    BoundingBox sceneBounds = _items.front()->WorldBounds;
    for (size_t i = 1; i < _items.size(); ++i)
        BoundingBox::CreateMerged(sceneBounds, sceneBounds, _items[i]->WorldBounds);
    _sceneBounds = sceneBounds;
    const float halfSize = (std::max)(sceneBounds.Extents.x,
        (std::max)(sceneBounds.Extents.y, sceneBounds.Extents.z));
    sceneBounds.Extents = { halfSize, halfSize, halfSize };
    _root = BuildNode(sceneBounds, _items, 0);
}

const std::vector<RenderItem*>& VisibilitySystem::ShadowCasters()
{
    _shadowCasters.clear();
    for (RenderItem* item : _items)
    {
        if (item->IsVisible())
            _shadowCasters.push_back(item);
    }
    return _shadowCasters;
}

const std::vector<RenderItem*>& VisibilitySystem::Cull(const XMFLOAT4X4& view,
    const XMFLOAT4X4& projection)
{
    BoundingFrustum viewFrustum;
    BoundingFrustum::CreateFromMatrix(viewFrustum, XMLoadFloat4x4(&projection));
    viewFrustum.Transform(_worldFrustum,
        XMMatrixInverse(nullptr, XMLoadFloat4x4(&view)));

    _visibleItems.clear();
    _stats = {};
    if (_enableOctreeCulling && _root)
        Collect(*_root, INTERSECTS);
    else
    {
        for (RenderItem* item : _items)
        {
            if (item->IsVisible() && (!_enableFrustumCulling ||
                _worldFrustum.Contains(item->WorldBounds) != DISJOINT))
                _visibleItems.push_back(item);
        }
    }

    UINT enabledCount = 0;
    for (RenderItem* item : _items)
        enabledCount += item->IsVisible() ? 1u : 0u;
    _stats.SubmittedObjects = static_cast<UINT>(_visibleItems.size());
    _stats.CulledObjects = enabledCount - _stats.SubmittedObjects;
    return _visibleItems;
}

std::unique_ptr<VisibilitySystem::OctreeNode> VisibilitySystem::BuildNode(
    const BoundingBox& bounds, const std::vector<RenderItem*>& items, UINT depth)
{
    auto node = std::make_unique<OctreeNode>();
    node->Bounds = bounds;
    if (items.size() <= LeafCapacity || depth >= MaxDepth)
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
            (i & 4) ? childExtents.z : -childExtents.z };
        childBounds[i] = BoundingBox(
            { bounds.Center.x + offset.x, bounds.Center.y + offset.y,
              bounds.Center.z + offset.z }, childExtents);
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
            node->Children[i] = BuildNode(childBounds[i], childItems[i], depth + 1);
    }
    return node;
}

void VisibilitySystem::CollectAll(const OctreeNode& node)
{
    for (RenderItem* item : node.Items)
    {
        if (item->IsVisible())
            _visibleItems.push_back(item);
    }
    for (const auto& child : node.Children)
    {
        if (child)
            CollectAll(*child);
    }
}

void VisibilitySystem::Collect(const OctreeNode& node, ContainmentType parentContainment)
{
    ++_stats.OctreeNodesTested;
    const ContainmentType containment = parentContainment == CONTAINS ? CONTAINS :
        _worldFrustum.Contains(node.Bounds);
    if (containment == DISJOINT)
        return;
    if (containment == CONTAINS)
    {
        CollectAll(node);
        return;
    }
    for (RenderItem* item : node.Items)
    {
        if (item->IsVisible() && _worldFrustum.Contains(item->WorldBounds) != DISJOINT)
            _visibleItems.push_back(item);
    }
    for (const auto& child : node.Children)
    {
        if (child)
            Collect(*child, INTERSECTS);
    }
}
