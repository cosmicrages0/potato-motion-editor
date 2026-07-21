#include "AnimationEngine.h"

AnimationEngine::AnimationEngine() {}

AnimationEngine::~AnimationEngine() {}

void AnimationEngine::Play()  { isPlaying = true;  }
void AnimationEngine::Pause() { isPlaying = false; }
void AnimationEngine::Reset() { currentTime = 0.0f; }

void AnimationEngine::Update(float deltaTime) {
    if (!isPlaying) return;
    if (duration <= 0.0001f) {
        // Defensive: an ill-configured zero-duration clip would divide-by-zero
        // in GetAnimatedScale. Just hold at t = 0 and stop advancing.
        currentTime = 0.0f;
        return;
    }

    currentTime += deltaTime;
    if (currentTime > duration) {
        if (isLooping) {
            // fmod keeps the loop tight even after huge deltaTime spikes
            currentTime = std::fmod(currentTime, duration);
        } else {
            currentTime = duration;
            isPlaying   = false;
        }
    }
    if (currentTime < 0.0f) currentTime = 0.0f;
}

float AnimationEngine::GetAnimatedScale() const {
    const float safeDuration = (duration > 0.0001f) ? duration : 1.0f;
    const float normalizedTime = std::clamp(currentTime / safeDuration, 0.0f, 1.0f);
    const float curveMultiplier = currentCurve.Evaluate(normalizedTime);
    // Base scale is 100 px; a curve value of 1.25 -> 125 px (visible overshoot).
    // Negative Bezier Y is allowed mathematically but clamped to 0 here so the
    // demo shape never inverts (an inverted rect looks like a bug).
    const float pixels = 100.0f * curveMultiplier;
    return (pixels < 0.0f) ? 0.0f : pixels;
}
