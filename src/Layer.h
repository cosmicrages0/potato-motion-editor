#pragma once
#include <string>
#include <optional>
#include <vector>
#include <algorithm>
#include <cmath>

#include "MathTypes.h"
#include "AnimationEngine.h"
#include "Effect.h"

// -----------------------------------------------------------------------------
// Shape type: extensible enum. CustomPath is stubbed for Task 3 and will be
// wired up in a later milestone when we add the SVG-style path editor.
// -----------------------------------------------------------------------------
enum class ShapeType : int {
    Rectangle  = 0,
    Ellipse    = 1,
    CustomPath = 2,
    Camera     = 3,  // Task 4: layer whose transform drives the active 3D camera
    Null       = 4   // Task 4.5: invisible transform-only layer for parenting rigs
};

// -----------------------------------------------------------------------------
// Transform: local TRS state for a layer.
//
// - position/rotation/scale are Vec3 so Task 4's 3D camera can lift them
//   without an ABI break. In Task 3 we only use the .x/.y components.
// - anchorPoint is in NORMALIZED bounds space [0..1], where {0.5, 0.5} is
//   the center of the layer's bounding box. Rotation and scale pivot here.
// - opacity is [0..1], multiplied down the parent chain.
// - sizePixels is the base authoring size before scale is applied; e.g. a
//   200x120 rectangle stays 200x120 in the model even after Scale=(2,2).
// -----------------------------------------------------------------------------
struct Transform {
    Vec3  position    = { 0.0f, 0.0f, 0.0f };
    Vec3  rotation    = { 0.0f, 0.0f, 0.0f }; // degrees; only .z used in 2D
    Vec3  scale       = { 1.0f, 1.0f, 1.0f };
    Vec2  anchorPoint = { 0.5f, 0.5f };
    Vec2  sizePixels  = { 200.0f, 120.0f };
    float opacity     = 1.0f;

    // Build the local 2D affine matrix that takes a point in the layer's
    // *pre-anchor* local space (origin at top-left of sizePixels) into
    // parent space. Order: translate-by-position * rotate * scale * translate-by-(-anchor)
    // Used by the 2D (is3D == false) rendering path.
    Mat3 ToLocalMatrix() const {
        const float ax = anchorPoint.x * sizePixels.x;
        const float ay = anchorPoint.y * sizePixels.y;
        // v_parent = T(pos) * R * S * T(-anchor) * v_local
        Mat3 M = Mat3::Translation(position.x, position.y)
               * Mat3::RotationDegrees(rotation.z)
               * Mat3::Scale(scale.x, scale.y)
               * Mat3::Translation(-ax, -ay);
        return M;
    }

    // Full 3D local matrix for the is3D == true rendering path (Task 4).
    // Same TRS layout as ToLocalMatrix() but honoring all three axes.
    // Rotation order: Y (yaw) * X (pitch) * Z (roll) — aircraft convention.
    Mat4 ToLocalMatrix4() const {
        const float ax = anchorPoint.x * sizePixels.x;
        const float ay = anchorPoint.y * sizePixels.y;
        // Note: anchor is 2D-only; on the Z axis the layer's origin is 0.
        Mat4 T  = Mat4::Translation(position.x, position.y, position.z);
        Mat4 Ry = Mat4::RotationYDegrees(rotation.y);
        Mat4 Rx = Mat4::RotationXDegrees(rotation.x);
        Mat4 Rz = Mat4::RotationZDegrees(rotation.z);
        Mat4 S  = Mat4::Scale(scale.x, scale.y, scale.z);
        Mat4 A  = Mat4::Translation(-ax, -ay, 0.0f);
        // v_parent = T * (Ry * Rx * Rz) * S * A * v_local
        return T * Ry * Rx * Rz * S * A;
    }
};

// -----------------------------------------------------------------------------
// Keyframe + PropertyTrack (Task 4.5): real per-property animation.
// A track holds a sorted list of Vec3-valued keyframes in COMPOSITION TIME
// (seconds). Evaluation returns the interpolated value at a given time,
// using the track's easing curve between segments.
//
// Scalar properties (opacity) just use .x and ignore .y/.z.
// -----------------------------------------------------------------------------
struct Keyframe {
    float time  = 0.0f;   // seconds in comp time
    Vec3  value = { 0.0f, 0.0f, 0.0f };
};

struct PropertyTrack {
    std::vector<Keyframe> keys;

    bool empty() const { return keys.empty(); }

    // Insert or replace a keyframe at (or very close to) `t`.
    void SetKey(float t, const Vec3& v) {
        constexpr float kEps = 1e-3f;
        for (auto& k : keys) {
            if (std::fabs(k.time - t) < kEps) { k.value = v; return; }
        }
        keys.push_back({ t, v });
        std::sort(keys.begin(), keys.end(),
                  [](const Keyframe& a, const Keyframe& b){ return a.time < b.time; });
    }

    // Linear interpolation for Task 4.5. Task 7 will upgrade to per-segment
    // Bezier easing so the slingshot curve can be used per keyframe.
    Vec3 Evaluate(float t) const {
        if (keys.empty()) return Vec3(0, 0, 0);
        if (keys.size() == 1 || t <= keys.front().time) return keys.front().value;
        if (t >= keys.back().time) return keys.back().value;
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            const auto& a = keys[i];
            const auto& b = keys[i + 1];
            if (t >= a.time && t <= b.time) {
                const float span = b.time - a.time;
                const float u = (span > 1e-6f) ? (t - a.time) / span : 0.0f;
                return Vec3(a.value.x + (b.value.x - a.value.x) * u,
                            a.value.y + (b.value.y - a.value.y) * u,
                            a.value.z + (b.value.z - a.value.z) * u);
            }
        }
        return keys.back().value;
    }
};

// -----------------------------------------------------------------------------
// Layer: one row in the timeline / one shape in the composition.
//
// - id is STABLE across the layer's lifetime. parentId refers to another
//   layer's id, NOT its index in the LayerManager's vector, so deletes and
//   reorders don't invalidate parenting.
// - is3D flips the layer into camera-space rendering (Task 4).
// - color is applied only to Rectangle/Ellipse fills for Task 3.
// -----------------------------------------------------------------------------
struct Layer {
    int         id            = 0;
    int         parentId      = -1;   // -1 = no parent
    std::string name          = "Layer";
    ShapeType   type          = ShapeType::Rectangle;
    bool        isVisible     = true;
    bool        isSolo        = false; // Timeline flag, stubbed for now
    bool        isLocked      = false; // Timeline flag, stubbed for now
    bool        is3D          = false; // Task 4: render through the 3D pipeline
    bool        stickToCamera = false; // Task 4.5: Alight-style HUD attachment
                                        // (layer follows camera as a screen-space overlay)
    Transform   transform;
    unsigned int fillColor    = 0xFFCCCC00; // ABGR little-endian (matches IM_COL32 layout on x86)

    // Optional animation tracks (Task 4.5). Empty by default so a static
    // layer pays zero allocation cost. When a track is present the renderer
    // samples it every frame and overrides the corresponding Transform field.
    std::optional<PropertyTrack> positionTrack;
    std::optional<PropertyTrack> scaleTrack;
    std::optional<PropertyTrack> rotationTrack;
    std::optional<PropertyTrack> opacityTrack;

    // Task 5: ordered stack of post-processing effects. Reserved so add/remove
    // never allocates inside the frame loop for typical projects.
    std::vector<Effect> effects;
    int nextEffectId = 1;

    Effect* AddEffect(const Effect& proto) {
        Effect e   = proto;
        e.id       = nextEffectId++;
        effects.push_back(std::move(e));
        return &effects.back();
    }
    bool RemoveEffectById(int effectId) {
        auto it = std::find_if(effects.begin(), effects.end(),
            [&](const Effect& x){ return x.id == effectId; });
        if (it == effects.end()) return false;
        effects.erase(it);
        return true;
    }
    Effect* FindEffectById(int effectId) {
        auto it = std::find_if(effects.begin(), effects.end(),
            [&](const Effect& x){ return x.id == effectId; });
        return (it == effects.end()) ? nullptr : &*it;
    }
    bool MoveEffect(int effectId, int delta) {
        auto it = std::find_if(effects.begin(), effects.end(),
            [&](const Effect& x){ return x.id == effectId; });
        if (it == effects.end()) return false;
        const size_t idx = (size_t)std::distance(effects.begin(), it);
        const size_t targetIdx = (size_t)std::clamp<int>((int)idx + delta, 0, (int)effects.size() - 1);
        if (targetIdx == idx) return false;
        // Simple two-swap move that preserves the rest of the order.
        if (delta > 0) std::rotate(effects.begin() + idx,
                                    effects.begin() + idx + 1,
                                    effects.begin() + targetIdx + 1);
        else           std::rotate(effects.begin() + targetIdx,
                                    effects.begin() + idx,
                                    effects.begin() + idx + 1);
        return true;
    }
    bool HasAnyEnabledEffect() const {
        for (const auto& e : effects) if (e.enabled) return true;
        return false;
    }

    // Convenience: set a keyframe on a property at the given comp time,
    // sampling the current transform value if the track doesn't exist yet.
    void KeyPosition(float t) {
        if (!positionTrack) positionTrack = PropertyTrack{};
        positionTrack->SetKey(t, transform.position);
    }
    void KeyScale(float t) {
        if (!scaleTrack) scaleTrack = PropertyTrack{};
        scaleTrack->SetKey(t, transform.scale);
    }
    void KeyRotation(float t) {
        if (!rotationTrack) rotationTrack = PropertyTrack{};
        rotationTrack->SetKey(t, transform.rotation);
    }
    void KeyOpacity(float t) {
        if (!opacityTrack) opacityTrack = PropertyTrack{};
        opacityTrack->SetKey(t, Vec3(transform.opacity, 0, 0));
    }

    // Called by the render engine at the start of each frame to bake the
    // sampled track values into the live transform so the rest of the pipeline
    // (matrix build, gizmos, hit-test) sees a single consistent state.
    void SampleTracks(float compTime) {
        if (positionTrack && !positionTrack->empty()) transform.position = positionTrack->Evaluate(compTime);
        if (scaleTrack    && !scaleTrack->empty())    transform.scale    = scaleTrack->Evaluate(compTime);
        if (rotationTrack && !rotationTrack->empty()) transform.rotation = rotationTrack->Evaluate(compTime);
        if (opacityTrack  && !opacityTrack->empty()) {
            const Vec3 v = opacityTrack->Evaluate(compTime);
            transform.opacity = std::clamp(v.x, 0.0f, 1.0f);
        }
    }
};
