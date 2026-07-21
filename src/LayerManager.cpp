#include "LayerManager.h"

#include <algorithm>
#include <unordered_set>

LayerManager::LayerManager() {
    layers.reserve(32);   // small headroom; growable
}

void LayerManager::RebuildIndex() {
    idToIndex.clear();
    idToIndex.reserve(layers.size() * 2);
    for (size_t i = 0; i < layers.size(); ++i) {
        idToIndex[layers[i].id] = i;
    }
}

int LayerManager::IndexOfId(int id) const {
    auto it = idToIndex.find(id);
    if (it == idToIndex.end()) return -1;
    return static_cast<int>(it->second);
}

Layer* LayerManager::GetLayerById(int id) {
    const int idx = IndexOfId(id);
    if (idx < 0) return nullptr;
    return &layers[idx];
}

const Layer* LayerManager::GetLayerById(int id) const {
    const int idx = IndexOfId(id);
    if (idx < 0) return nullptr;
    return &layers[idx];
}

Layer* LayerManager::GetSelectedLayer() {
    return GetLayerById(selectedLayerId);
}

int LayerManager::AddLayer(ShapeType type, const std::string& nameHint) {
    Layer L;
    L.id   = nextId++;
    L.type = type;

    if (!nameHint.empty()) {
        L.name = nameHint;
    } else {
        switch (type) {
            case ShapeType::Rectangle:  L.name = "Rectangle " + std::to_string(L.id); break;
            case ShapeType::Ellipse:    L.name = "Ellipse "   + std::to_string(L.id); break;
            case ShapeType::CustomPath: L.name = "Path "      + std::to_string(L.id); break;
            case ShapeType::Camera:     L.name = "Camera "    + std::to_string(L.id); break;
            case ShapeType::Null:       L.name = "Null "      + std::to_string(L.id); break;
        }
    }

    // Sensible default: center of a 1280x720 composition.
    L.transform.position = { 640.0f, 360.0f, 0.0f };

    // Slight color variation so successive adds are visually distinguishable.
    // Cycles through a small palette to stay potato-PC friendly.
    static const unsigned int palette[6] = {
        0xFF00B4FF, // orange-ish (ABGR)
        0xFF7FFF00, // green
        0xFFFF6EC7, // pink
        0xFF00E5FF, // yellow-cyan
        0xFFFFB86C, // sky
        0xFF9CFF9C  // mint
    };
    L.fillColor = palette[L.id % 6];

    layers.push_back(std::move(L));
    RebuildIndex();

    // Auto-select the new layer for immediate editing.
    selectedLayerId = layers.back().id;
    return selectedLayerId;
}

bool LayerManager::DeleteLayerById(int id) {
    const int idx = IndexOfId(id);
    if (idx < 0) return false;

    // Orphan any children of the deleted layer (detach, don't cascade —
    // AE-style behavior; users don't expect deleting a parent to nuke kids).
    for (auto& child : layers) {
        if (child.parentId == id) child.parentId = -1;
    }

    layers.erase(layers.begin() + idx);
    RebuildIndex();

    if (selectedLayerId == id) {
        selectedLayerId = layers.empty() ? -1 : layers.front().id;
    }
    return true;
}

bool LayerManager::WouldCreateCycle(int childId, int candidateParentId) const {
    if (childId == candidateParentId) return true;        // self-parenting
    if (candidateParentId < 0)        return false;       // detach is always safe
    if (IndexOfId(childId) < 0)       return false;
    if (IndexOfId(candidateParentId) < 0) return false;

    // Walk up from the candidate parent. If we ever hit `childId`, that means
    // childId is already an ancestor of candidateParentId, so making
    // candidateParentId the new parent of childId would close the loop.
    std::unordered_set<int> visited;
    int cursor = candidateParentId;
    // Depth limit is a defensive backstop against a pre-existing corrupt cycle
    // in the data (shouldn't happen, but if it does we don't hang the frame).
    constexpr int kMaxDepth = 4096;
    for (int step = 0; step < kMaxDepth && cursor != -1; ++step) {
        if (cursor == childId)          return true;
        if (!visited.insert(cursor).second) return true; // pre-existing cycle
        const Layer* p = GetLayerById(cursor);
        if (!p) return false;
        cursor = p->parentId;
    }
    return false;
}

bool LayerManager::SetParent(int childId, int parentId) {
    if (IndexOfId(childId) < 0) return false;
    if (parentId != -1 && IndexOfId(parentId) < 0) return false;
    if (WouldCreateCycle(childId, parentId)) return false;

    Layer* child = GetLayerById(childId);
    if (!child) return false;
    child->parentId = parentId;
    return true;
}

void LayerManager::BeginFrame() {
    frameMatrixCache.clear();
    frameMatrix4Cache.clear();
    frameOpacityCache.clear();
}

int LayerManager::FindActiveCameraLayerId() const {
    // Convention: the FIRST Camera-type layer in the timeline is the active
    // camera. AE has richer semantics but this keeps Task 4 predictable.
    for (const auto& L : layers) {
        if (L.type == ShapeType::Camera) return L.id;
    }
    return -1;
}

Mat3 LayerManager::GetWorldMatrix(int layerId) {
    if (layerId < 0) return Mat3::Identity();

    auto cached = frameMatrixCache.find(layerId);
    if (cached != frameMatrixCache.end()) return cached->second;

    const Layer* L = GetLayerById(layerId);
    if (!L) return Mat3::Identity();

    Mat3 local = L->transform.ToLocalMatrix();

    // Column-vector convention: world = parent * local
    // (child transforms happen inside parent's coordinate frame — matches AE).
    Mat3 world;
    if (L->parentId >= 0 && L->parentId != L->id) {
        world = GetWorldMatrix(L->parentId) * local;
    } else {
        world = local;
    }

    frameMatrixCache[layerId] = world;
    return world;
}

Mat4 LayerManager::GetWorldMatrix4(int layerId) {
    if (layerId < 0) return Mat4::Identity();

    auto cached = frameMatrix4Cache.find(layerId);
    if (cached != frameMatrix4Cache.end()) return cached->second;

    const Layer* L = GetLayerById(layerId);
    if (!L) return Mat4::Identity();

    Mat4 local = L->transform.ToLocalMatrix4();
    Mat4 world;
    if (L->parentId >= 0 && L->parentId != L->id) {
        world = GetWorldMatrix4(L->parentId) * local;
    } else {
        world = local;
    }
    frameMatrix4Cache[layerId] = world;
    return world;
}

float LayerManager::GetWorldOpacity(int layerId) {
    if (layerId < 0) return 1.0f;

    auto cached = frameOpacityCache.find(layerId);
    if (cached != frameOpacityCache.end()) return cached->second;

    const Layer* L = GetLayerById(layerId);
    if (!L) return 1.0f;

    float op = std::clamp(L->transform.opacity, 0.0f, 1.0f);
    if (L->parentId >= 0 && L->parentId != L->id) {
        op *= GetWorldOpacity(L->parentId);
    }
    frameOpacityCache[layerId] = op;
    return op;
}
