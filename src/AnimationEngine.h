#pragma once
#include <algorithm>
#include <cmath>

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float _x, float _y) : x(_x), y(_y) {}
};

// Represents a Cubic Bézier Curve with two end points (P0, P3) and two control handles (P1, P2)
struct BezierCurve {
    Vec2 P0 = { 0.0f, 0.0f };   // Start (Time: 0, Value: 0)
    Vec2 P1 = { 0.25f, 1.25f }; // Handle 1 (Can overshoot Y > 1.0 for slingshot)
    Vec2 P2 = { 0.50f, 0.90f }; // Handle 2 (Can rebound Y < 1.0)
    Vec2 P3 = { 1.0f, 1.0f };   // End (Time: 1, Value: 1)

    // Evaluates cubic curve value B_y at time t in range [0, 1]
    float Evaluate(float t) const {
        t = std::clamp(t, 0.0f, 1.0f);
        float u = 1.0f - t;
        float tt = t * t;
        float uu = u * u;
        float uuu = uu * u;
        float ttt = tt * t;

        // B(t) = (1-t)^3 * P0 + 3*(1-t)^2 * t * P1 + 3*(1-t) * t^2 * P2 + t^3 * P3
        float y = uuu * P0.y + 3.0f * uu * t * P1.y + 3.0f * u * tt * P2.y + ttt * P3.y;
        return y;
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
    float duration = 1.0f; // 1.0 second animation duration
    bool isPlaying = true;
    bool isLooping = true;

    // Evaluates current animated scale value based on slingshot Bézier curve
    float GetAnimatedScale() const;
};
