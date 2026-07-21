#pragma once
#include "MathTypes.h"

// -----------------------------------------------------------------------------
// Camera: single-node 3D camera modeled after Alight Motion.
//
// The camera can be driven in two ways:
//   1. Directly via its public fields (position/target/rotation/fov).
//   2. Indirectly by attaching it to a Layer whose ShapeType == Camera.
//      In that mode, RenderEngine copies the layer's transform into this
//      camera every frame BEFORE building the view matrix, so keyframe
//      animation on the layer drives the camera automatically.
//
// Rotation is applied in Y (yaw), X (pitch), Z (roll) order — the standard
// "aircraft" convention, which is what artists expect when they type into
// a rotation field.
// -----------------------------------------------------------------------------
class Camera {
public:
    // Placement
    Vec3 position = { 0.0f, 0.0f, -1000.0f }; // eye
    Vec3 target   = { 0.0f, 0.0f,     0.0f }; // point of interest ("look-at")
    Vec3 rotation = { 0.0f, 0.0f,     0.0f }; // pitch(X), yaw(Y), roll(Z) — degrees
    Vec3 up       = { 0.0f, 1.0f,     0.0f }; // world up

    // Lens
    float fov   = 45.0f;   // vertical FOV in degrees
    float nearZ = 1.0f;
    float farZ  = 10000.0f;
    float zoom  = 1000.0f; // distance target-to-eye used by orbit controls

    // Which mode is driving the view direction:
    //   true  = use `target` (LookAt mode; orbit/pan controls work naturally)
    //   false = use `rotation` (free-look mode; useful for keyframe-animated cameras)
    bool useTargetMode = true;

    Mat4 GetViewMatrix() const;
    Mat4 GetProjectionMatrix(float aspect) const;

    // Project a world-space 3D point to viewport pixel coordinates.
    // Returns:
    //   out_screen.x, out_screen.y   -> pixel position in the viewport
    //   out_screen.z                 -> normalized device Z (for depth sort)
    //   out_screen.w                 -> clip-space W (<=0 means behind camera)
    // The caller should treat w <= 0 as "do not draw".
    Vec4 ProjectPoint(const Vec3& worldPoint, float viewportWidth, float viewportHeight) const;

    // Reset to a sensible default framing for a 1280x720 composition.
    void ResetToDefault();
};
