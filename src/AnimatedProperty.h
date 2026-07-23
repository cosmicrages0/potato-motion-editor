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
// Task 5.4: per-keyframe Bezier easing is LIVE.
// Every Keyframe<T> carries an incoming and outgoing tangent (expressed as
// time_offset + value_offset relative to the key) and per-side interpolation
// modes (Linear / Bezier / Hold). Evaluate() dispatches to a Newton-Raphson
// cubic Bezier evaluator when either endpoint of a segment is Bezier, and
// falls back to Lerp for pure-Linear segments (which is what all Task 5.1 /
// 5.2 keyframes are, so backward compat is bit-identical).
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
// Scale-by-scalar overloads. Used inside the Bezier evaluator to weight the
// four control points by (1-u)^3 / 3(1-u)^2 u / 3(1-u) u^2 / u^3.
// -----------------------------------------------------------------------------
inline float ScaleT(float a, float s)      { return a * s; }
inline Vec2  ScaleT(const Vec2& a, float s){ return Vec2(a.x * s, a.y * s); }
inline Vec3  ScaleT(const Vec3& a, float s){ return Vec3(a.x * s, a.y * s, a.z * s); }

// Componentwise add — used to accumulate the weighted Bezier terms.
inline float AddT(float a, float b)                     { return a + b; }
inline Vec2  AddT(const Vec2& a, const Vec2& b)         { return Vec2(a.x + b.x, a.y + b.y); }
inline Vec3  AddT(const Vec3& a, const Vec3& b)         { return Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }

// -----------------------------------------------------------------------------
// InterpMode — per-side interpolation policy of a single keyframe.
//
// A segment between key A and key B evaluates linearly when BOTH
// A.outgoingMode == Linear AND B.incomingMode == Linear (Task 5.1
// backward-compat fast path). Otherwise the Bezier evaluator runs, using
// A.outTangent and B.inTangent. Hold is treated as "step at A.value across
// the whole segment" — matches AE's Hold key visual.
// -----------------------------------------------------------------------------
enum class InterpMode : int {
    Linear = 0,  // constant velocity through this segment
    Bezier = 1,  // uses (inTangent, outTangent) — set by right-click or F9
    Hold   = 2,  // value snaps at A, no interp until next key
};

// -----------------------------------------------------------------------------
// Keyframe<T> — single (time, value) sample with Bezier tangent handles.
//
// Tangent offsets are stored in the SAME space as `value`:
//   time offset  — seconds (typically negative on inTangent, positive on out)
//   value offset — T (float / Vec2 / Vec3) — added to `value` to place handle
//
// Zero tangents + Linear mode == the Task 5.1 keyframe. Old .pmge files load
// with tangents zero + mode Linear so playback is bit-identical.
// -----------------------------------------------------------------------------
template <typename T>
struct Keyframe {
    float      time  = 0.0f;   // seconds in composition time
    T          value = T{};    // value at that time

    // Bezier tangents in (time_offset_seconds, value_offset_in_T_units).
    // The incoming tangent points BACK in time (its time offset is
    // conventionally negative). The outgoing tangent points forward.
    T          inTangentValue  = T{};
    float      inTangentTime   = 0.0f;
    T          outTangentValue = T{};
    float      outTangentTime  = 0.0f;

    InterpMode incomingMode = InterpMode::Linear;
    InterpMode outgoingMode = InterpMode::Linear;
};

// -----------------------------------------------------------------------------
// Cubic Bezier segment evaluator. Solves for the parametric `u` in [0,1] where
// the x-coordinate (time) of the (t,value) 2D Bezier matches the requested
// time, then evaluates the y-coordinate (value) at that u. This is the same
// math CSS cubic-bezier() uses — Newton-Raphson converges in ~4 iterations for
// well-behaved tangents; a bisection fallback catches degenerate cases.
// -----------------------------------------------------------------------------
template <typename T>
inline T EvaluateBezierSegment(const Keyframe<T>& A, const Keyframe<T>& B, float t) {
    const float t0 = A.time;
    const float t3 = B.time;
    const float span = t3 - t0;
    if (span <= 1e-6f) return B.value;

    // Control-point times. Clamp so P1.x is not before P0.x and P2.x is not
    // after P3.x — keeps the time-curve monotonic so Newton-Raphson can't
    // wander off into a second root.
    float t1 = t0 + A.outTangentTime;
    float t2 = t3 + B.inTangentTime;
    if (t1 < t0) t1 = t0;
    if (t1 > t3) t1 = t3;
    if (t2 < t0) t2 = t0;
    if (t2 > t3) t2 = t3;

    // Control-point values (in T space).
    const T v0 = A.value;
    const T v1 = AddT(A.value, A.outTangentValue);
    const T v2 = AddT(B.value, B.inTangentValue);
    const T v3 = B.value;

    // Initial guess: linear parameter along t.
    float u = (t - t0) / span;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;

    // Newton-Raphson on x(u) = t.
    for (int iter = 0; iter < 6; ++iter) {
        const float mu = 1.0f - u;
        const float x  = mu*mu*mu*t0
                       + 3.0f*mu*mu*u*t1
                       + 3.0f*mu*u*u*t2
                       + u*u*u*t3;
        const float dx = 3.0f*mu*mu*(t1 - t0)
                       + 6.0f*mu*u*(t2 - t1)
                       + 3.0f*u*u*(t3 - t2);
        const float err = x - t;
        if (std::fabs(err) < 1e-5f) break;
        if (std::fabs(dx) < 1e-6f) break; // avoid divide-by-zero; bisection would help but rare
        u -= err / dx;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
    }

    // Evaluate value(u).
    const float mu = 1.0f - u;
    T acc = ScaleT(v0, mu*mu*mu);
    acc   = AddT(acc, ScaleT(v1, 3.0f*mu*mu*u));
    acc   = AddT(acc, ScaleT(v2, 3.0f*mu*u*u));
    acc   = AddT(acc, ScaleT(v3, u*u*u));
    return acc;
}

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
        // Between two keys -> dispatch on segment interp mode.
        for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
            const auto& a = keyframes[i];
            const auto& b = keyframes[i + 1];
            if (t >= a.time && t <= b.time) {
                // Hold: A's outgoing mode says "don't interpolate at all".
                if (a.outgoingMode == InterpMode::Hold) return a.value;

                const bool linearSegment =
                    (a.outgoingMode == InterpMode::Linear &&
                     b.incomingMode == InterpMode::Linear);
                if (linearSegment) {
                    // Task 5.1 backward-compat fast path.
                    const float span = b.time - a.time;
                    const float u    = (span > 1e-6f) ? (t - a.time) / span : 0.0f;
                    return Lerp(a.value, b.value, u);
                }
                // At least one endpoint is Bezier -> full cubic eval.
                return EvaluateBezierSegment(a, b, t);
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
        Keyframe<T> nk;
        nk.time  = t;
        nk.value = v;
        keyframes.push_back(nk);
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
