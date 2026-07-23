#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include "MathTypes.h"
#include "AnimationEngine.h"
#include "AnimatedProperty.h"
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
// Transform: local TRS state for a layer, After Effects style.
//
// TASK 5.1 REWRITE (single source of truth = Adobe / Lottie / Pikimov):
//
//   Every animatable field is an AnimatedProperty<T>. Reads go through
//   .Evaluate(compTime); writes go through .SetValue(compTime, v). The
//   property KNOWS whether to return static or interpolate keyframes; the
//   caller does not care and cannot get it wrong.
//
//   This replaces the old parallel std::optional<PropertyTrack> hack.
// -----------------------------------------------------------------------------
struct Transform {
    AnimatedProperty<Vec3>  position    { Vec3(0.0f, 0.0f, 0.0f) };
    AnimatedProperty<Vec3>  rotation    { Vec3(0.0f, 0.0f, 0.0f) }; // degrees; only .z in 2D
    AnimatedProperty<Vec3>  scale       { Vec3(1.0f, 1.0f, 1.0f) };
    AnimatedProperty<Vec2>  anchorPoint { Vec2(0.5f, 0.5f) };
    AnimatedProperty<Vec2>  sizePixels  { Vec2(200.0f, 120.0f) };
    AnimatedProperty<float> opacity     { 1.0f };

    // Build the local 2D affine matrix (was Task 3's ToLocalMatrix).
    // Now takes compTime because every field is an AnimatedProperty that
    // needs to know when to sample. Semantics of the matrix are unchanged:
    //   v_parent = T(pos) * R * S * T(-anchor) * v_local
    Mat3 ToLocalMatrix(float compTime) const {
        const Vec3 pos    = position   .Evaluate(compTime);
        const Vec3 rot    = rotation   .Evaluate(compTime);
        const Vec3 scl    = scale      .Evaluate(compTime);
        const Vec2 anchor = anchorPoint.Evaluate(compTime);
        const Vec2 size   = sizePixels .Evaluate(compTime);

        const float ax = anchor.x * size.x;
        const float ay = anchor.y * size.y;
        return Mat3::Translation(pos.x, pos.y)
             * Mat3::RotationDegrees(rot.z)
             * Mat3::Scale(scl.x, scl.y)
             * Mat3::Translation(-ax, -ay);
    }

    // 3D variant (Task 4). Same story with compTime.
    // Rotation order Y * X * Z (aircraft convention).
    Mat4 ToLocalMatrix4(float compTime) const {
        const Vec3 pos    = position   .Evaluate(compTime);
        const Vec3 rot    = rotation   .Evaluate(compTime);
        const Vec3 scl    = scale      .Evaluate(compTime);
        const Vec2 anchor = anchorPoint.Evaluate(compTime);
        const Vec2 size   = sizePixels .Evaluate(compTime);

        const float ax = anchor.x * size.x;
        const float ay = anchor.y * size.y;
        Mat4 T  = Mat4::Translation(pos.x, pos.y, pos.z);
        Mat4 Ry = Mat4::RotationYDegrees(rot.y);
        Mat4 Rx = Mat4::RotationXDegrees(rot.x);
        Mat4 Rz = Mat4::RotationZDegrees(rot.z);
        Mat4 S  = Mat4::Scale(scl.x, scl.y, scl.z);
        Mat4 A  = Mat4::Translation(-ax, -ay, 0.0f);
        return T * Ry * Rx * Rz * S * A;
    }
};

// -----------------------------------------------------------------------------
// Layer: one row in the timeline / one shape in the composition.
//
// Same fields as before EXCEPT the four parallel std::optional<PropertyTrack>
// tracks and their KeyX/AutoKeyX/ToggleAnimateX/IsXAnimated/SampleTracks
// helpers are ALL DELETED. The equivalent capability now lives inside each
// AnimatedProperty<T> field of Transform. Call sites use:
//
//   layer.transform.position.Evaluate(t)         // was: layer.transform.position (with implicit sampling)
//   layer.transform.position.SetValue(t, newPos) // was: layer.transform.position = newPos + AutoKey
//   layer.transform.position.ToggleStopwatch(t)  // was: layer.ToggleAnimatePosition(t)
//   layer.transform.position.IsAnimated()        // was: layer.IsPositionAnimated()
//
// Cleaner, one type does one job, matches AE/Lottie/Pikimov 1:1.
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
    Transform   transform;
    unsigned int fillColor    = 0xFFCCCC00; // ABGR little-endian (matches IM_COL32 layout on x86)

    // Task 5.7: stroke + rounded corners. Drawn by the CompositionRenderer's
    // consolidated SDF pixel shader (no CPU tessellation). Strokes are drawn
    // INSIDE the shape boundary (Figma default) so a 100x100 rect with a
    // 4px stroke still occupies exactly 100x100 pixels. cornerRadius applies
    // only to Rectangle; Ellipse/Null/Camera ignore it.
    //
    // Not (yet) an AnimatedProperty: stroke color is a styling choice, not
    // an animation target in typical motion graphics. Trivially promotable
    // later if a user asks.
    unsigned int strokeColor  = 0xFF000000; // ABGR; opaque black by default
    float        strokeWidth  = 0.0f;       // pixels; 0 => no stroke
    float        cornerRadius = 0.0f;       // pixels; 0 => sharp corners (Rect only)

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
};
