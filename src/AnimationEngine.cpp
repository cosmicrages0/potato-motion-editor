#include "AnimationEngine.h"

AnimationEngine::AnimationEngine() {}

AnimationEngine::~AnimationEngine() {}

void AnimationEngine::Play() {
isPlaying = true;
}

void AnimationEngine::Pause() {
isPlaying = false;
}

void AnimationEngine::Reset() {
currentTime = 0.0f;
}

void AnimationEngine::Update(float deltaTime) {
if (!isPlaying) return;

currentTime += deltaTime;
if (currentTime > duration) {
    if (isLooping) {
        currentTime = 0.0f;
    } else {
        currentTime = duration;
        isPlaying = false;
    }
}


}

float AnimationEngine::GetAnimatedScale() const {
float normalizedTime = currentTime / duration;
float curveMultiplier = currentCurve.Evaluate(normalizedTime);
// Base scale is 100px, multiplied by curve value (e.g. 1.25 -> 125px)
return 100.0f * curveMultiplier;
}
