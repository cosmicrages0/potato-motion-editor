#pragma once
// =============================================================================
// AnimatedProperty<T> — the After Effects "Property" pattern.
//
// Adobe's own docs describe AE evaluation as:
//   base value  ->  keyframe value  ->  expression result
// (see https://ae-scripting.docsforadobe.dev/property/property/)
//
// Lottie's JSON schema encodes exactly this per animatable field:
//   { "a": 0, "k": [x, y] }        // static value
//   { "a": 1, "k": [ {t, s}, ... ] } // animated over keyframes
//
// This template implements the same model. Every animatable field on a
// Layer's Transform (position/rotation/scale/anchor/size/opacity) is an
// AnimatedProperty<T> containing:
//
//   staticValue       — the "base value" (default / fallback)
//   keyframes         — sparse list of Keyframe<T>; sorted by time
//   stopwatchEnabled  — AE's stopwatch icon state. When false, the property
//                       is static and SetValue() writes staticValue. When
//                       true, SetValue() creates or updates a keyframe at the
//                       supplied comp time.
//
// Evaluate(t) is THE ONE READ PATH. SetValue(t, v) is THE ONE WRITE PATH.
// All Inspector edits, gizmo drags, and renderer reads go through these two
// functions. That's what makes Inspector-vs-canvas value mismatches, undo
// weirdness, and gizmo-drag-fights-animation impossible by construction.
//
// When per-keyframe Bezier easing lands (Phase 2), Keyframe<T> gains inTangent
// and outTangent fields; Evaluate()'s inner loop switches from Lerp to a
// cubic Bezier evaluator. Every call site keeps working unchanged.
//
// When expressions land (Phase 5+), Evaluate() gets one more override branch
// at the top:  if (expressionEnabled) return RunExpression(t);
// Again, every call site keeps working unchanged.
// =============================================================================

#include <vector>
#include <algorithm>
#include <cmath>

#include "MathTypes.h"

// -----------------------------------------------------------------------------
// Lerp overloads. Free functions so AnimatedProperty<T> can call unqualified
// Lerp() and get the right overload picked at compile time.
//
// If we later animate a new type (Mat3, Color, string) we just add another
// overload here and the template lights up for the new type.
// -----------------------------------------------------------------------------
inline float Lerp(float a, float b, float u) { return a + (b - a) * u; }

inline Vec2 Lerp(const Vec2& a, const Vec2& b, float u) {
    return Vec2(a.x + (b.x - a.x) * u,
                a.y + (b.y - a.y) * u);
}

inline Vec3 Lerp(const Vec3& a, const Vec3& b, float u) {
    return Vec3(a.x + (b.x - a.x) * u,
                a.y + (b.y - a.y) * u,
                a.z + (b.z - a.z) * u);
}

// -----------------------------------------------------------------------------
// Keyframe<T> — single (time, value) sample. Bezier tangents come in Phase 2.
// -----------------------------------------------------------------------------
template <typename T>
struct Keyframe {
    float time  = 0.0f;   // seconds in composition time
    T     value = T{};    // value at that time
};

// -----------------------------------------------------------------------------
// AnimatedProperty<T>.
// -----------------------------------------------------------------------------
template <typename T>
struct AnimatedProperty {
    T                        staticValue{};
    std::vector<Keyframe<T>> keyframes;
    bool                     stopwatchEnabled = false;

    AnimatedProperty() = default;
    // Convenience constructor for the common case "give me a static default".
    AnimatedProperty(const T& initial) : staticValue(initial) {}

    // -------------------------------------------------------------------------
    // Read path. Returns the value at composition time t.
    // -------------------------------------------------------------------------
    T Evaluate(float t) const {
        // Not animated -> constant.
        if (!stopwatchEnabled || keyframes.empty()) return staticValue;
        // Animated but only one key -> hold that value everywhere.
        if (keyframes.size() == 1) return keyframes.front().value;
        // Before the first key -> hold the first-key value.
        if (t <= keyframes.front().time) return keyframes.front().value;
        // After the last key -> hold the last-key value.
        if (t >= keyframes.back().time)  return keyframes.back().value;
        // Between two keys -> linear interp (Phase 2 upgrades to Bezier).
        for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
            const auto& a = keyframes[i];
            const auto& b = keyframes[i + 1];
            if (t >= a.time && t <= b.time) {
                const float span = b.time - a.time;
                const float u    = (span > 1e-6f) ? (t - a.time) / span : 0.0f;
                return Lerp(a.value, b.value, u);
            }
        }
        return keyframes.back().value;
    }

    // -------------------------------------------------------------------------
    // Write path. If stopwatch is on, insert-or-replace a keyframe at time t.
    // If stopwatch is off, update the static value.
    //
    // Same-time tolerance (1 ms) is what prevents mid-gizmo-drag key spam:
    // every mouse-move frame during the same drag has ~16 ms delta but stamps
    // at the SAME playhead time, so SetValue overwrites the single key instead
    // of appending new ones.
    // -------------------------------------------------------------------------
    void SetValue(float t, const T& v) {
        if (!stopwatchEnabled) { staticValue = v; return; }
        constexpr float kEps = 1e-3f;
        for (auto& k : keyframes) {
            if (std::fabs(k.time - t) < kEps) { k.value = v; return; }
        }
        keyframes.push_back({ t, v });
        std::sort(keyframes.begin(), keyframes.end(),
                  [](const Keyframe<T>& a, const Keyframe<T>& b){ return a.time < b.time; });
    }

    // -------------------------------------------------------------------------
    // AE stopwatch toggle.
    //   Turning ON  -> stopwatch lit, one keyframe dropped at t seeded from
    //                  the current staticValue (so the animation already has
    //                  an anchor point).
    //   Turning OFF -> stopwatch dim, ALL keyframes cleared, staticValue
    //                  preserved (matches AE: the static value survives).
    // -------------------------------------------------------------------------
    void ToggleStopwatch(float t) {
        if (stopwatchEnabled) {
            stopwatchEnabled = false;
            keyframes.clear();
        } else {
            stopwatchEnabled = true;
            // SetValue below will append the seed key because keyframes is empty.
            SetValue(t, staticValue);
        }
    }

    // Predicates for UI: is the property currently animating on-screen?
    bool IsAnimated() const { return stopwatchEnabled && !keyframes.empty(); }
    // Is the stopwatch lit (even if no keys yet)?
    bool HasStopwatch() const { return stopwatchEnabled; }

    // Remove a keyframe close to t. Returns true if one was removed.
    // Used by future right-click-diamond-delete UX.
    bool RemoveKeyAt(float t) {
        constexpr float kEps = 1e-3f;
        auto it = std::find_if(keyframes.begin(), keyframes.end(),
            [t](const Keyframe<T>& k){ return std::fabs(k.time - t) < kEps; });
        if (it == keyframes.end()) return false;
        keyframes.erase(it);
        return true;
    }
};
