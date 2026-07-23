#pragma once
// =============================================================================
// AnimatedProperty<T> — the After Effects "Property" pattern, AE-accurate.
//
// Adobe's own docs describe AE evaluation as:
//   base value  ->  keyframe value  ->  expression result
// (see https://ae-scripting.docsforadobe.dev/property/property/)
//
// Every animatable field on a Layer's Transform is an AnimatedProperty<T>
// containing:
//
//   staticValue       — the "base value" (default / fallback)
//   keyframes         — sparse list of Keyframe<T>; sorted by time
//   stopwatchEnabled  — AE's stopwatch icon state. When false, the property
//                       is static and SetValue() writes staticValue. When
//                       true, SetValue() creates or updates a keyframe at the
//                       supplied comp time.
//
// Evaluate(t) is THE ONE READ PATH. SetValue(t, v) is THE ONE WRITE PATH.
//
// Task 5.4-fix (Commit 6): storage is now AE-native. Each Keyframe<T> holds
// per-side (in / out):
//   * speed      — units/second at which value leaves/enters the key
//   * influence  — 0..100 %; how far in TIME toward the neighbor key the
//                  tangent handle reaches
//   * mode       — Linear / Bezier / ContinuousBezier / AutoBezier / Hold
// This matches AE's actual "Keyframe Velocity" data. Every displayed Bezier
// handle is derived from (speed, influence) at draw time.
//
// Between two keys A -> B the cubic Bezier control points are:
//     dt = B.time - A.time
//     P0 = A.value
//     P3 = B.value
//     P1 = A.value + A.outSpeed * (dt * A.outInfluence / 100)
//          -> its X (time) sits at A.time + (dt * A.outInfluence / 100)
//     P2 = B.value - B.inSpeed  * (dt * B.inInfluence  / 100)
//          -> its X (time) sits at B.time - (dt * B.inInfluence  / 100)
// Value is B(u) with Newton-Raphson-solved u; speed is |B'(u)| / dt.
// Same evaluator handles Bezier / ContinuousBezier / AutoBezier — they differ
// only in HOW their speed/influence get written, not HOW they're evaluated.
//
// Backward compat:
//   * Task 5.2 keys (no tangent data) load with defaults => Linear both sides,
//     zero speed, 16.667% influence. Evaluator picks the Linear fast path so
//     playback is bit-identical.
//   * Task 5.4 keys (with time/value offset tangents) are converted at load
//     time in Serialization.cpp: speed = value_offset / time_offset, influence
//     = |time_offset| / segment_span * 100. Evaluator produces the same value
//     curve because it's the same math on both sides of the conversion.
// =============================================================================

#include <vector>
#include <algorithm>
#include <cmath>

#include "MathTypes.h"

// -----------------------------------------------------------------------------
// Lerp overloads. Free functions so AnimatedProperty<T> can call unqualified
// Lerp() and get the right overload picked at compile time.
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
// Scale-by-scalar + componentwise-add overloads. Used inside the Bezier
// evaluator to weight the four control points.
// -----------------------------------------------------------------------------
inline float ScaleT(float a, float s)      { return a * s; }
inline Vec2  ScaleT(const Vec2& a, float s){ return Vec2(a.x * s, a.y * s); }
inline Vec3  ScaleT(const Vec3& a, float s){ return Vec3(a.x * s, a.y * s, a.z * s); }

inline float AddT(float a, float b)                     { return a + b; }
inline Vec2  AddT(const Vec2& a, const Vec2& b)         { return Vec2(a.x + b.x, a.y + b.y); }
inline Vec3  AddT(const Vec3& a, const Vec3& b)         { return Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }

inline float SubT(float a, float b)                     { return a - b; }
inline Vec2  SubT(const Vec2& a, const Vec2& b)         { return Vec2(a.x - b.x, a.y - b.y); }
inline Vec3  SubT(const Vec3& a, const Vec3& b)         { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }

// -----------------------------------------------------------------------------
// InterpMode — per-side interpolation policy of a single keyframe. Matches
// the five AE modes exactly.
//
// A segment between A and B evaluates linearly ONLY when BOTH sides are
// Linear (fast Lerp path). Any Bezier variant on either side routes through
// the cubic evaluator. Hold on A's outgoing side short-circuits to A.value
// across the whole segment.
// -----------------------------------------------------------------------------
enum class InterpMode : int {
    Linear           = 0,  // straight line; no tangents used
    Bezier           = 1,  // user drags in/out handles independently
    ContinuousBezier = 2,  // handles mirrored: equal speed magnitude, matched influence
    AutoBezier       = 3,  // speed auto-computed from neighbor slopes; handles locked
    Hold             = 4,  // value snaps at A, no interp until next key
};

// -----------------------------------------------------------------------------
// Keyframe<T>. AE-native storage.
//
// Default influence = 16.667% comes from Adobe's documented default for a
// newly-created bezier keyframe (a third of the segment split into thirds).
// F9 Easy Ease bumps it to 33.33% on both sides. Speed defaults to zero
// (which is what Easy Ease sets, and what all newly-promoted-to-Bezier keys
// use until the user drags).
// -----------------------------------------------------------------------------
template <typename T>
struct Keyframe {
    float      time  = 0.0f;   // seconds in composition time
    T          value = T{};    // value at that time

    // Per-side AE-native tangent data.
    T          inSpeed      = T{};
    T          outSpeed     = T{};
    float      inInfluence  = 16.667f;   // 0..100
    float      outInfluence = 16.667f;

    InterpMode incomingMode = InterpMode::Linear;
    InterpMode outgoingMode = InterpMode::Linear;

    // Task 5.4-fix: roving flag stored per-key so the schema is future-proof.
    // Evaluator ignores it for now (spatial-interp support is a later commit).
    bool       roving = false;
};

// -----------------------------------------------------------------------------
// Compute the cubic Bezier control points (time + value per point) for a
// segment [A, B] given the AE-native (speed, influence) data on both sides.
// Callers use this both for evaluation and for placing screen-space handles
// in the Graph Editor.
// -----------------------------------------------------------------------------
template <typename T>
struct BezierSegment {
    float t0 = 0.0f, t1 = 0.0f, t2 = 0.0f, t3 = 0.0f;   // control-point times
    T     v0{}, v1{}, v2{}, v3{};                       // control-point values
};

template <typename T>
inline BezierSegment<T> BuildBezierSegment(const Keyframe<T>& A, const Keyframe<T>& B) {
    BezierSegment<T> s;
    s.t0 = A.time;
    s.t3 = B.time;
    const float dt = s.t3 - s.t0;
    // Clamp influences to sane range so a corrupt file can't blow up eval.
    const float outInf = std::clamp(A.outInfluence, 0.0f, 100.0f) * 0.01f;
    const float inInf  = std::clamp(B.inInfluence,  0.0f, 100.0f) * 0.01f;
    s.t1 = s.t0 + dt * outInf;
    s.t2 = s.t3 - dt * inInf;
    s.v0 = A.value;
    s.v3 = B.value;
    // Value offset from the key: speed * (time distance to control point).
    s.v1 = AddT(A.value, ScaleT(A.outSpeed, dt * outInf));
    s.v2 = SubT(B.value, ScaleT(B.inSpeed,  dt * inInf));
    return s;
}

// -----------------------------------------------------------------------------
// Solve for the parametric u in [0,1] such that x(u) == t on the segment's
// time-Bezier. Newton-Raphson, 6 iterations, with initial guess = linear
// parameter. Same math CSS cubic-bezier() uses.
// -----------------------------------------------------------------------------
template <typename T>
inline float SolveBezierU(const BezierSegment<T>& s, float t) {
    const float span = s.t3 - s.t0;
    if (span <= 1e-6f) return 1.0f;
    float u = (t - s.t0) / span;
    if (u < 0.0f) u = 0.0f;
    if (u > 1.0f) u = 1.0f;
    for (int iter = 0; iter < 6; ++iter) {
        const float mu = 1.0f - u;
        const float x  = mu*mu*mu*s.t0
                       + 3.0f*mu*mu*u*s.t1
                       + 3.0f*mu*u*u*s.t2
                       + u*u*u*s.t3;
        const float dx = 3.0f*mu*mu*(s.t1 - s.t0)
                       + 6.0f*mu*u*(s.t2 - s.t1)
                       + 3.0f*u*u*(s.t3 - s.t2);
        const float err = x - t;
        if (std::fabs(err) < 1e-5f) break;
        if (std::fabs(dx) < 1e-6f) break;
        u -= err / dx;
        if (u < 0.0f) u = 0.0f;
        if (u > 1.0f) u = 1.0f;
    }
    return u;
}

// Evaluate value at parameter u on the value-Bezier.
template <typename T>
inline T EvalBezierValueAtU(const BezierSegment<T>& s, float u) {
    const float mu = 1.0f - u;
    T acc = ScaleT(s.v0, mu*mu*mu);
    acc   = AddT(acc, ScaleT(s.v1, 3.0f*mu*mu*u));
    acc   = AddT(acc, ScaleT(s.v2, 3.0f*mu*u*u));
    acc   = AddT(acc, ScaleT(s.v3, u*u*u));
    return acc;
}

// Full segment eval: build cubic, solve u for time t, plug u into value cubic.
template <typename T>
inline T EvaluateBezierSegment(const Keyframe<T>& A, const Keyframe<T>& B, float t) {
    const float span = B.time - A.time;
    if (span <= 1e-6f) return B.value;
    const BezierSegment<T> s = BuildBezierSegment(A, B);
    const float u = SolveBezierU(s, t);
    return EvalBezierValueAtU(s, u);
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
    AnimatedProperty(const T& initial) : staticValue(initial) {}

    // -------------------------------------------------------------------------
    // Read path. Returns the value at composition time t.
    // -------------------------------------------------------------------------
    T Evaluate(float t) const {
        if (!stopwatchEnabled || keyframes.empty()) return staticValue;
        if (keyframes.size() == 1)                  return keyframes.front().value;
        if (t <= keyframes.front().time)            return keyframes.front().value;
        if (t >= keyframes.back().time)             return keyframes.back().value;

        for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
            const auto& a = keyframes[i];
            const auto& b = keyframes[i + 1];
            if (t >= a.time && t <= b.time) {
                // Hold on outgoing side: no interp inside this segment.
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
                // Any Bezier flavor on either side -> cubic eval.
                return EvaluateBezierSegment(a, b, t);
            }
        }
        return keyframes.back().value;
    }

    // -------------------------------------------------------------------------
    // Write path. If stopwatch on, insert-or-replace a keyframe at time t.
    // Same-time tolerance (1 ms) prevents mid-gizmo-drag key spam.
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
    // -------------------------------------------------------------------------
    void ToggleStopwatch(float t) {
        if (stopwatchEnabled) {
            stopwatchEnabled = false;
            keyframes.clear();
        } else {
            stopwatchEnabled = true;
            SetValue(t, staticValue);
        }
    }

    bool IsAnimated() const  { return stopwatchEnabled && !keyframes.empty(); }
    bool HasStopwatch() const { return stopwatchEnabled; }

    // Remove a keyframe close to t. Returns true if one was removed.
    bool RemoveKeyAt(float t) {
        constexpr float kEps = 1e-3f;
        auto it = std::find_if(keyframes.begin(), keyframes.end(),
            [t](const Keyframe<T>& k){ return std::fabs(k.time - t) < kEps; });
        if (it == keyframes.end()) return false;
        keyframes.erase(it);
        return true;
    }
};
