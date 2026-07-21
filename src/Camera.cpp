#include "Camera.h"

Mat4 Camera::GetViewMatrix() const {
    if (useTargetMode) {
        // Guard against target == position (LookAt would produce a NaN axis).
        Vec3 delta = target - position;
        if (Vec3Length(delta) < 1e-4f) {
            // Nudge target 1 unit along +Z so we still get a valid basis.
            Vec3 nudged = position + Vec3(0.0f, 0.0f, 1.0f);
            return Mat4::LookAtLH(position, nudged, up);
        }
        return Mat4::LookAtLH(position, target, up);
    }

    // Free-look mode: rotation defines the view basis.
    // Build R = Ry(yaw) * Rx(pitch) * Rz(roll), then translate by -eye.
    Mat4 R = Mat4::RotationYDegrees(rotation.y)
           * Mat4::RotationXDegrees(rotation.x)
           * Mat4::RotationZDegrees(rotation.z);
    Mat4 T = Mat4::Translation(-position.x, -position.y, -position.z);
    // View = R * T (rotate world into camera space after translating eye to origin).
    return R * T;
}

Mat4 Camera::GetProjectionMatrix(float aspect) const {
    return Mat4::PerspectiveFovLH(fov, aspect, nearZ, farZ);
}

Vec4 Camera::ProjectPoint(const Vec3& worldPoint,
                          float viewportWidth, float viewportHeight) const {
    if (viewportWidth  < 1.0f) viewportWidth  = 1.0f;
    if (viewportHeight < 1.0f) viewportHeight = 1.0f;
    const float aspect = viewportWidth / viewportHeight;

    Mat4 view = GetViewMatrix();
    Mat4 proj = GetProjectionMatrix(aspect);

    Vec4 clip = proj.TransformVec4(view.TransformVec4(Vec4(worldPoint, 1.0f)));

    // Behind-camera clip: caller checks w <= 0.
    // We still divide by max(w, epsilon) to avoid NaNs polluting downstream math.
    const float safeW = (clip.w > 1e-6f) ? clip.w : 1e-6f;
    const float ndcX = clip.x / safeW;
    const float ndcY = clip.y / safeW;
    const float ndcZ = clip.z / safeW;

    // Map NDC [-1,1] -> pixel [0,W] / [0,H] (Y flipped: NDC +Y is up, screen +Y is down)
    const float sx = (ndcX * 0.5f + 0.5f) * viewportWidth;
    const float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * viewportHeight;

    return Vec4(sx, sy, ndcZ, clip.w);
}

void Camera::ResetToDefault() {
    position      = { 640.0f, 360.0f, -1000.0f };
    target        = { 640.0f, 360.0f,     0.0f };
    rotation      = { 0.0f,   0.0f,      0.0f };
    up            = { 0.0f,   1.0f,      0.0f };
    fov           = 45.0f;
    nearZ         = 1.0f;
    farZ          = 10000.0f;
    zoom          = 1000.0f;
    useTargetMode = true;
}
