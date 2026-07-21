#pragma once
#include <algorithm>
#include <cmath>

// Simple 2D vector used for Bezier handle authoring.
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float _x, float _y) : x(_x), y(_y) {}
};

// Represents a Cubic Bezier Curve with two end points (P0, P3) and two control handles (P1, P2).
// Y values of P1 / P2 are intentionally unconstrained so the artist can push above 1.0
// for slingshot overshoot or below 0.0 for elastic rebound without changing the timeline
// duration (which is governed strictly by X in [0, 1]).
struct BezierCurve {
    Vec2 P0 = { 0.0f, 0.0f };   // Start   (Time: 0, Value: 0)
    Vec2 P1 = { 0.25f, 1.25f }; // Handle 1 (Overshoot Y > 1.0)
    Vec2 P2 = { 0.50f, 0.90f }; // Handle 2 (Rebound possible)
    Vec2 P3 = { 1.0f, 1.0f };   // End     (Time: 1, Value: 1)

    // Evaluates the cubic Bezier Y value at parametric time t in range [0, 1].
    // B(t) = (1-t)^3 * P0 + 3*(1-t)^2 * t * P1 + 3*(1-t) * t^2 * P2 + t^3 * P3
    float Evaluate(float t) const {
        t = std::clamp(t, 0.0f, 1.0f);
        const float u   = 1.0f - t;
        const float tt  = t * t;
        const float uu  = u * u;
        const float uuu = uu * u;
        const float ttt = tt * t;

        return uuu * P0.y
             + 3.0f * uu  * t  * P1.y
             + 3.0f * u   * tt * P2.y
             + ttt * P3.y;
    }
};

class AnimationEngine {
public:
    AnimationEngine();
    ~AnimationEngine();

    void Update(float deltaTime);
    void Play();
    void Pause();
    void Reset();

    BezierCurve currentCurve;
    float currentTime = 0.0f;
    float duration    = 1.0f; // Seconds; guarded against zero in Update / GetAnimatedScale.
    bool  isPlaying   = true;
    bool  isLooping   = true;

    // Returns the animated pixel size for the demo shape. Base 100px * curve multiplier.
    float GetAnimatedScale() const;
};
