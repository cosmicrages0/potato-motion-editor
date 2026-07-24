#pragma once
#include <string>

// -----------------------------------------------------------------------------
// Effect: POD-ish per-layer post-processing entry.
//
// Design decisions (see PROJECT_BRIEFING.md Section 9.5 for context):
//   * Value type, not std::shared_ptr<Effect>. Layers hold std::vector<Effect>
//     with capacity reserved up-front; adding/removing an effect does NOT
//     allocate inside the frame loop.
//   * All parameters live in a single POD 'params' struct with float4 slots.
//     That way the EffectManager can memcpy the whole struct into the shader
//     constant buffer without per-type serialization code.
//   * Stable per-effect `id` (unique within its owning layer) so the UI can
//     reference an effect across frames even if the vector is reordered.
// -----------------------------------------------------------------------------

enum class EffectType : int {
    MotionTile          = 0, // Tile / mirror-edge repetition
    DirectionalMotionBlur = 1,
    ChromaticAberration = 2,
    BlendMode           = 3, // Not a filter — sets how the layer composites over layers below
    // Task 5.13: DropShadow — first per-layer isolation-mode effect.
    // Runs two internal passes (offset+blur into pong, composite original
    // over shadow into ping) via a dedicated shader that binds 2 SRVs.
    DropShadow          = 4,
    COUNT
};

enum class BlendMode : int {
    Normal      = 0,
    Additive    = 1,
    Multiply    = 2,
    Screen      = 3,
    Overlay     = 4,
    ColorDodge  = 5
};

// Fixed-size parameter block. Interpretation depends on `type`:
//
// MotionTile:              p0.x = TileCount   p0.y = Phase   p0.z = MirrorEdges (0/1)
// DirectionalMotionBlur:   p0.x = Angle(deg)  p0.y = Intensity (0..100)  p0.z = Samples (int, capped 16)
// ChromaticAberration:     p0.x = Amount      p0.y = Angle(deg)          p0.z = Radial (0/1)
// BlendMode:               p0.x = mode enum value (int-in-float)
//
// p1 / p2 / p3 are reserved for later effects so the constant buffer layout
// stays stable. The full 64-byte struct is what gets uploaded to the GPU.
struct EffectParams {
    float p0[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float p1[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float p2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float p3[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct Effect {
    int          id       = 0;    // unique within owning layer
    EffectType   type     = EffectType::MotionTile;
    bool         enabled  = true;
    EffectParams params;
    std::string  displayName; // populated at add-time for the Inspector row

    // Convenience factories with sensible defaults so a freshly-added effect
    // is immediately visible in the viewport (matches AE behavior).
    static Effect MakeMotionTile() {
        Effect e; e.type = EffectType::MotionTile; e.displayName = "Motion Tile";
        e.params.p0[0] = 2.0f;  // TileCount
        e.params.p0[1] = 0.0f;  // Phase
        e.params.p0[2] = 1.0f;  // MirrorEdges = true
        return e;
    }
    static Effect MakeMotionBlur() {
        Effect e; e.type = EffectType::DirectionalMotionBlur; e.displayName = "Directional Motion Blur";
        e.params.p0[0] = 0.0f;  // Angle
        e.params.p0[1] = 8.0f;  // Intensity
        e.params.p0[2] = 8.0f;  // Samples
        return e;
    }
    static Effect MakeChromaticAberration() {
        Effect e; e.type = EffectType::ChromaticAberration; e.displayName = "Chromatic Aberration";
        e.params.p0[0] = 4.0f;  // Amount
        e.params.p0[1] = 0.0f;  // Angle
        e.params.p0[2] = 1.0f;  // Radial = true
        return e;
    }
    static Effect MakeBlendMode(BlendMode mode) {
        Effect e; e.type = EffectType::BlendMode; e.displayName = "Blend Mode";
        e.params.p0[0] = (float)(int)mode;
        return e;
    }
    // Task 5.13: Drop Shadow — 5-tap blurred offset copy of the layer,
    // tinted by shadow color, composited behind the original.
    //   p0.x = Distance (px)
    //   p0.y = Angle (degrees, 0=right, 90=down)
    //   p0.z = Softness (px, gaussian-ish radius)
    //   p1.x = Opacity (0..1)
    //   p2.xyz = Shadow color RGB (0..1)
    static Effect MakeDropShadow() {
        Effect e; e.type = EffectType::DropShadow; e.displayName = "Drop Shadow";
        e.params.p0[0] = 5.0f;    // Distance
        e.params.p0[1] = 135.0f;  // Angle (down-right, AE default)
        e.params.p0[2] = 3.0f;    // Softness
        e.params.p1[0] = 0.6f;    // Opacity
        e.params.p2[0] = 0.0f;    // R
        e.params.p2[1] = 0.0f;    // G
        e.params.p2[2] = 0.0f;    // B (black shadow)
        return e;
    }
};
