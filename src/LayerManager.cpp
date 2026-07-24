#include "LayerManager.h"

#include <algorithm>
#include <unordered_set>
#include <cmath>

LayerManager::LayerManager() {
    layers.reserve(32);   // small headroom; growable
}

// Task 5.2: wipe everything back to empty state. LoadProject calls this
// before deserializing so nothing from the previous session leaks through.
void LayerManager::Clear() {
    layers.clear();
    idToIndex.clear();
    frameMatrixCache.clear();
    frameMatrix4Cache.clear();
    frameOpacityCache.clear();
    nextId = 1;
    selectedLayerId = -1;
    currentCompTime = 0.0f;
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
    // Task 5.1: writing to staticValue directly is correct here — this is a
    // brand-new layer with the stopwatch off, so there's no keyframe to key.
    L.transform.position.staticValue = Vec3(640.0f, 360.0f, 0.0f);

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

// Task 5.10: DuplicateLayer — deep-copy a layer (Layer is a POD-ish struct
// with only value fields + ComPtr text atlases that AddRef safely on copy),
// assign it a fresh id, and insert immediately AFTER the source in the
// stack. Newly-duplicated layer is auto-selected.
//
// Adjustment #4 locked: no time offset. Duplicate at the same in/out as
// the source — matches AE. Users can drag the trim bar to offset if
// they want.
//
// ComPtr safety: Layer.textTex / textSRV are Microsoft::WRL::ComPtr
// which AddRefs on copy — src and copy briefly share the atlas until
// the next frame's EnsureLayerCache runs. Fast-path (hash match) makes
// that a no-op; no double-free or dangling pointer risk.
int LayerManager::DuplicateLayer(int srcId) {
    const int srcIdx = IndexOfId(srcId);
    if (srcIdx < 0) return -1;
    Layer copy = layers[(size_t)srcIdx];
    copy.id   = nextId++;
    // Distinguish the duplicate in the layer list. AE convention.
    copy.name = layers[(size_t)srcIdx].name + " copy";
    // Insert at srcIdx + 1 so the duplicate renders ON TOP OF the source
    // (Z-order = later in vector). If srcIdx is the last layer, insert
    // still works — it just appends.
    layers.insert(layers.begin() + (srcIdx + 1), std::move(copy));
    RebuildIndex();
    // Auto-select the new layer for immediate editing.
    selectedLayerId = layers[(size_t)(srcIdx + 1)].id;
    return selectedLayerId;
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

// -----------------------------------------------------------------------------
// Task 5.3-fix-2: preserve-world-on-parent (matches AE + Alight Motion).
//
// Given: child currently has authored transform Tc and world Wc = P_old * Tc
// (where P_old is the OLD parent's world matrix, or Identity if none).
//
// Goal: after changing parent to newParent (with world P_new, or Identity if
// none), the child's VISIBLE world should still equal Wc. That means we need:
//    P_new * Tc_new = Wc          (Wc is what the user sees right now)
//    Tc_new = inverse(P_new) * Wc
//
// We then DECOMPOSE Tc_new (a 2D affine 3x3) into translate + rotate + scale
// and write those into the child's AnimatedProperty<Vec3> fields.
//
// Standard 2D affine decomposition (no shear):
//    M = [ a  b  tx ]
//        [ c  d  ty ]
//        [ 0  0   1 ]
//    scaleX   = sign(a) * sqrt(a*a + c*c)      (sign preserves horizontal flip)
//    scaleY   = sign(d) * sqrt(b*b + d*d)
//    rotation = atan2(c/|scaleX|, a/|scaleX|)  (in radians; convert to degrees)
//    translate = (tx, ty)
//
// Sign-preservation on scale keeps mirror flips intact.
//
// For 3D layers (is3D == true) we fall through to raw SetParent for now — 3D
// TRS decomposition is more involved and 3D isn't the current focus. Marked
// with a TODO comment inline.
// -----------------------------------------------------------------------------
bool LayerManager::SetParentPreservingWorld(int childId, int parentId, float compTime) {
    if (IndexOfId(childId) < 0) return false;
    if (parentId != -1 && IndexOfId(parentId) < 0) return false;
    if (WouldCreateCycle(childId, parentId)) return false;

    Layer* child = GetLayerById(childId);
    if (!child) return false;

    // 3D reparenting: not yet implemented — fall back to raw for correctness
    // (child WILL jump, but at least the parent link updates cleanly).
    // TODO: 3D preserve-world requires full 4x4 TRS decomposition.
    if (child->is3D) {
        child->parentId = parentId;
        return true;
    }

    // Capture the child's CURRENT world matrix (with its animated values baked
    // in) BEFORE we mutate anything. This is what we're preserving.
    // Note: GetWorldMatrix uses this manager's currentCompTime, which the
    // caller may not have updated for this frame yet. Pass compTime through
    // via BeginFrame's cache-reset side effect if needed — but here we just
    // trust that currentCompTime was set at BeginFrame this frame.
    (void)compTime;   // reserved for future when we need to sample off-clock
    const Mat3 oldChildWorld = GetWorldMatrix(childId);

    // Update the parent link, then invalidate the per-frame caches so the
    // next GetWorldMatrix call on any layer recomputes with the new parent.
    child->parentId = parentId;
    frameMatrixCache.clear();
    frameMatrix4Cache.clear();
    frameOpacityCache.clear();

    // Compute the new parent's world matrix under the updated hierarchy.
    // Identity when detaching to root.
    const Mat3 newParentWorld = (parentId == -1)
                                    ? Mat3::Identity()
                                    : GetWorldMatrix(parentId);

    // Solve for the child's new LOCAL matrix that preserves world:
    //   newChildLocal = inverse(newParentWorld) * oldChildWorld
    const Mat3 newChildLocal = newParentWorld.InverseAffine() * oldChildWorld;

    // Layer::Transform::ToLocalMatrix builds:
    //   M = T(pos) * R(rot.z) * S(scale) * T(-anchor*size)
    //
    // The T(-anchor*size) part is a POST-scale offset, so extracting position
    // from newChildLocal's raw translate gives (position - R * S * anchor_offset).
    // We need to reverse that offset to get the pure position component.
    //
    // In practice: sample the child's current anchorPoint + sizePixels at t,
    // build the anchor offset, and add it back after decomposing.
    const Vec2 anchor = child->transform.anchorPoint.Evaluate(compTime);
    const Vec2 size   = child->transform.sizePixels .Evaluate(compTime);
    const float ax = anchor.x * size.x;
    const float ay = anchor.y * size.y;

    // Decompose newChildLocal (row-major mat[row][col]).
    const float a = newChildLocal.m[0][0];
    const float b = newChildLocal.m[0][1];
    const float tx = newChildLocal.m[0][2];
    const float c = newChildLocal.m[1][0];
    const float d = newChildLocal.m[1][1];
    const float ty = newChildLocal.m[1][2];

    // Scale, with sign preservation.
    float sx = std::sqrt(a*a + c*c);
    float sy = std::sqrt(b*b + d*d);
    if (a < 0) sx = -sx;
    if (d < 0) sy = -sy;

    // Rotation in radians -> degrees. Handle sx == 0 defensively.
    float rotZDeg = 0.0f;
    if (std::fabs(sx) > 1e-6f) {
        const float na = a / sx;
        const float nc = c / sx;
        rotZDeg = std::atan2(nc, na) * (180.0f / 3.14159265358979323846f);
    }

    // Now reverse the anchor-offset baked into (tx, ty). We need:
    //   T(pos) * R * S * T(-anchor)
    // whose translate component is:
    //   pos - (R * S * (anchor_x, anchor_y))
    // Rearranging:
    //   pos = translate + R * S * anchor_offset
    //
    // Rebuild R * S from the decomposed values, apply to anchor offset,
    // and add to translate.
    Mat3 RS = Mat3::RotationDegrees(rotZDeg) * Mat3::Scale(sx, sy);
    const float roax = RS.m[0][0] * ax + RS.m[0][1] * ay;
    const float roay = RS.m[1][0] * ax + RS.m[1][1] * ay;
    const float posX = tx + roax;
    const float posY = ty + roay;

    // Read the child's current Z so we don't zero it out (2D preserves only
    // XY on position; Z stays authored).
    const float posZ = child->transform.position.Evaluate(compTime).z;

    // Write the decomposed values into the AnimatedProperty fields.
    // SetValue routes to either a keyframe at compTime (if stopwatch on) or
    // to staticValue (if stopwatch off) — either way the visible frame from
    // this reparent onward matches the pre-reparent visible world.
    child->transform.position.SetValue(compTime, Vec3(posX, posY, posZ));
    // Rotation: preserve X/Y (2D uses only Z), just update Z.
    const Vec3 rotCur = child->transform.rotation.Evaluate(compTime);
    child->transform.rotation.SetValue(compTime, Vec3(rotCur.x, rotCur.y, rotZDeg));
    // Scale: preserve Z (unused in 2D but keeps future 3D moves clean).
    const Vec3 sclCur = child->transform.scale.Evaluate(compTime);
    child->transform.scale.SetValue(compTime, Vec3(sx, sy, sclCur.z));
    return true;
}

void LayerManager::BeginFrame(float compTime) {
    frameMatrixCache.clear();
    frameMatrix4Cache.clear();
    frameOpacityCache.clear();
    currentCompTime = compTime;
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

    Mat3 local = L->transform.ToLocalMatrix(currentCompTime);

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

    Mat4 local = L->transform.ToLocalMatrix4(currentCompTime);
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

    float op = std::clamp(L->transform.opacity.Evaluate(currentCompTime), 0.0f, 1.0f);
    if (L->parentId >= 0 && L->parentId != L->id) {
        op *= GetWorldOpacity(L->parentId);
    }
    frameOpacityCache[layerId] = op;
    return op;
}
