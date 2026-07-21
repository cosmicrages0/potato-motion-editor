#pragma once
#include <string>
#include <optional>
#include <vector>

#include "MathTypes.h"
#include "AnimationEngine.h"

// -----------------------------------------------------------------------------
// Shape type: extensible enum. CustomPath is stubbed for Task 3 and will be
// wired up in a later milestone when we add the SVG-style path editor.
// -----------------------------------------------------------------------------
enum class ShapeType : int {
    Rectangle = 0,
    Ellipse   = 1,
    CustomPath = 2,
    Camera    = 3  // Task 4: layer whose transform drives the active 3D camera
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
// PropertyTrack: reserved slot for Task 5's per-property keyframe animation.
// For Task 3 we ship this as a forward-compat placeholder so the Layer struct
// stays ABI-stable when we wire in real tracks.
// -----------------------------------------------------------------------------
struct PropertyTrack {
    // Placeholder — Task 5 will fill in keyframes + easing.
    // Kept as an empty struct now so std::optional<PropertyTrack> costs
    // essentially nothing on layers that don't animate.
    bool enabled = false;
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
    bool        is3D          = false; // Reserved for Task 4
    Transform   transform;
    unsigned int fillColor    = 0xFFCCCC00; // ABGR little-endian (matches IM_COL32 layout on x86)

    // Optional animation tracks. Empty by default so unused layers pay zero
    // allocation cost. Task 5 will populate these.
    std::optional<PropertyTrack> positionTrack;
    std::optional<PropertyTrack> scaleTrack;
    std::optional<PropertyTrack> rotationTrack;
    std::optional<PropertyTrack> opacityTrack;
};
