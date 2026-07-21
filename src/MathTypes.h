#pragma once
// Small header-only math primitives shared across the engine.
//
// Convention: LEFT-HANDED, column-vector, right-multiply (v' = M * v).
// Left-handed matches Direct3D's native convention, so at Task 6 (FFmpeg
// export via GPU readback) we avoid a sign flip on the Z axis.
//
// Mat3 remains for 2D on-canvas gizmo math (cheap closed-form inverse).
// Mat4 is added for Task 4's full 3D pipeline (view/projection/MVP).

#include <cmath>
#include <algorithm>

#ifndef POTATO_PI
#define POTATO_PI 3.14159265358979323846f
#endif

// Reuse the Vec2 defined by AnimationEngine.h so we don't get ODR conflicts.
#include "AnimationEngine.h"

// -----------------------------------------------------------------------------
// Vec3 / Vec4
// -----------------------------------------------------------------------------
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float _x, float _y, float _z = 0.0f) : x(_x), y(_y), z(_z) {}
    explicit Vec3(const Vec2& v, float _z = 0.0f) : x(v.x), y(v.y), z(_z) {}

    Vec2 xy() const { return Vec2(x, y); }

    Vec3 operator+(const Vec3& o) const { return Vec3(x+o.x, y+o.y, z+o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x-o.x, y-o.y, z-o.z); }
    Vec3 operator*(float s) const       { return Vec3(x*s,   y*s,   z*s);   }
};

inline float Vec3Dot(const Vec3& a, const Vec3& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
inline Vec3 Vec3Cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y*b.z - a.z*b.y,
                a.z*b.x - a.x*b.z,
                a.x*b.y - a.y*b.x);
}
inline float Vec3Length(const Vec3& v) {
    return std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
}
inline Vec3 Vec3Normalize(const Vec3& v) {
    const float len = Vec3Length(v);
    if (len < 1e-8f) return Vec3(0, 0, 1); // safe fallback along +Z
    const float inv = 1.0f / len;
    return Vec3(v.x*inv, v.y*inv, v.z*inv);
}

struct Vec4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    Vec4() = default;
    Vec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
    Vec4(const Vec3& v, float _w = 1.0f) : x(v.x), y(v.y), z(v.z), w(_w) {}
};

// -----------------------------------------------------------------------------
// Mat3: 2D affine (translate/rotate/scale). Row-major storage.
//   [ m[0][0]  m[0][1]  m[0][2] ]     [ a  b  tx ]
//   [ m[1][0]  m[1][1]  m[1][2] ]  =  [ c  d  ty ]
//   [ m[2][0]  m[2][1]  m[2][2] ]     [ 0  0  1  ]
// -----------------------------------------------------------------------------
struct Mat3 {
    float m[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    };

    static Mat3 Identity() { return Mat3(); }

    static Mat3 Translation(float tx, float ty) {
        Mat3 r; r.m[0][2] = tx; r.m[1][2] = ty; return r;
    }
    static Mat3 Scale(float sx, float sy) {
        Mat3 r; r.m[0][0] = sx; r.m[1][1] = sy; return r;
    }
    static Mat3 RotationDegrees(float degrees) {
        const float rad = degrees * (POTATO_PI / 180.0f);
        const float c = std::cos(rad);
        const float s = std::sin(rad);
        Mat3 r;
        r.m[0][0] =  c;  r.m[0][1] = -s;
        r.m[1][0] =  s;  r.m[1][1] =  c;
        return r;
    }

    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                r.m[i][j] = m[i][0]*o.m[0][j] + m[i][1]*o.m[1][j] + m[i][2]*o.m[2][j];
        return r;
    }

    Vec2 TransformPoint(const Vec2& p) const {
        return Vec2(m[0][0]*p.x + m[0][1]*p.y + m[0][2],
                    m[1][0]*p.x + m[1][1]*p.y + m[1][2]);
    }

    // Closed-form inverse for an affine 2D matrix (last row = [0 0 1]).
    Mat3 InverseAffine() const {
        const float a = m[0][0], b = m[0][1], tx = m[0][2];
        const float c = m[1][0], d = m[1][1], ty = m[1][2];
        const float det = a*d - b*c;
        if (std::fabs(det) < 1e-8f) return Mat3::Identity();
        const float invDet = 1.0f / det;
        Mat3 r;
        r.m[0][0] =  d * invDet;
        r.m[0][1] = -b * invDet;
        r.m[1][0] = -c * invDet;
        r.m[1][1] =  a * invDet;
        r.m[0][2] = -(r.m[0][0]*tx + r.m[0][1]*ty);
        r.m[1][2] = -(r.m[1][0]*tx + r.m[1][1]*ty);
        return r;
    }
};

// -----------------------------------------------------------------------------
// Mat4: full 4x4 for Task 4's 3D pipeline. Row-major storage.
//
// Multiplication convention: (A * B) applied to v means "first B, then A"
// (v' = A * B * v). Left-handed coordinate system.
// -----------------------------------------------------------------------------
struct Mat4 {
    float m[4][4] = {
        {1,0,0,0},
        {0,1,0,0},
        {0,0,1,0},
        {0,0,0,1}
    };

    static Mat4 Identity() { return Mat4(); }

    static Mat4 Translation(float tx, float ty, float tz) {
        Mat4 r;
        r.m[0][3] = tx;
        r.m[1][3] = ty;
        r.m[2][3] = tz;
        return r;
    }

    static Mat4 Scale(float sx, float sy, float sz) {
        Mat4 r;
        r.m[0][0] = sx;
        r.m[1][1] = sy;
        r.m[2][2] = sz;
        return r;
    }

    static Mat4 RotationXDegrees(float deg) {
        const float rad = deg * (POTATO_PI / 180.0f);
        const float c = std::cos(rad), s = std::sin(rad);
        Mat4 r;
        r.m[1][1] =  c; r.m[1][2] = -s;
        r.m[2][1] =  s; r.m[2][2] =  c;
        return r;
    }
    static Mat4 RotationYDegrees(float deg) {
        const float rad = deg * (POTATO_PI / 180.0f);
        const float c = std::cos(rad), s = std::sin(rad);
        Mat4 r;
        r.m[0][0] =  c; r.m[0][2] =  s;
        r.m[2][0] = -s; r.m[2][2] =  c;
        return r;
    }
    static Mat4 RotationZDegrees(float deg) {
        const float rad = deg * (POTATO_PI / 180.0f);
        const float c = std::cos(rad), s = std::sin(rad);
        Mat4 r;
        r.m[0][0] =  c; r.m[0][1] = -s;
        r.m[1][0] =  s; r.m[1][1] =  c;
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = m[i][0]*o.m[0][j] + m[i][1]*o.m[1][j]
                          + m[i][2]*o.m[2][j] + m[i][3]*o.m[3][j];
        return r;
    }

    Vec4 TransformVec4(const Vec4& v) const {
        return Vec4(
            m[0][0]*v.x + m[0][1]*v.y + m[0][2]*v.z + m[0][3]*v.w,
            m[1][0]*v.x + m[1][1]*v.y + m[1][2]*v.z + m[1][3]*v.w,
            m[2][0]*v.x + m[2][1]*v.y + m[2][2]*v.z + m[2][3]*v.w,
            m[3][0]*v.x + m[3][1]*v.y + m[3][2]*v.z + m[3][3]*v.w);
    }

    // Transform a point (implicit w=1) and return the divided xyz (with w
    // returned so the caller can clip). Clipping is the caller's job.
    Vec4 TransformPoint(const Vec3& p) const {
        return TransformVec4(Vec4(p, 1.0f));
    }

    // Standard LEFT-HANDED LookAt matrix.
    static Mat4 LookAtLH(const Vec3& eye, const Vec3& target, const Vec3& up) {
        Vec3 zaxis = Vec3Normalize(target - eye);       // forward
        Vec3 xaxis = Vec3Normalize(Vec3Cross(up, zaxis)); // right
        Vec3 yaxis = Vec3Cross(zaxis, xaxis);             // true up
        Mat4 r;
        r.m[0][0] = xaxis.x; r.m[0][1] = xaxis.y; r.m[0][2] = xaxis.z; r.m[0][3] = -Vec3Dot(xaxis, eye);
        r.m[1][0] = yaxis.x; r.m[1][1] = yaxis.y; r.m[1][2] = yaxis.z; r.m[1][3] = -Vec3Dot(yaxis, eye);
        r.m[2][0] = zaxis.x; r.m[2][1] = zaxis.y; r.m[2][2] = zaxis.z; r.m[2][3] = -Vec3Dot(zaxis, eye);
        r.m[3][0] = 0.0f;    r.m[3][1] = 0.0f;    r.m[3][2] = 0.0f;    r.m[3][3] = 1.0f;
        return r;
    }

    // Standard LEFT-HANDED perspective projection matrix using VERTICAL FOV.
    //   fovY_deg: vertical field of view in DEGREES
    //   aspect:   width / height of the target viewport (guarded)
    //   nearZ, farZ: clip planes; both must be > 0 and near < far
    static Mat4 PerspectiveFovLH(float fovY_deg, float aspect, float nearZ, float farZ) {
        // Defensive clamps — a squished viewport or misconfigured lens should
        // NOT produce NaN pixels.
        if (aspect < 1e-4f) aspect = 1e-4f;
        if (fovY_deg < 1.0f)  fovY_deg = 1.0f;
        if (fovY_deg > 179.0f) fovY_deg = 179.0f;
        if (nearZ < 1e-4f)  nearZ = 1e-4f;
        if (farZ <= nearZ)  farZ  = nearZ + 1.0f;

        const float f = 1.0f / std::tan(fovY_deg * 0.5f * (POTATO_PI / 180.0f));
        Mat4 r;
        // Clear identity because we're writing every slot we care about
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = 0.0f;

        r.m[0][0] = f / aspect;
        r.m[1][1] = f;
        r.m[2][2] = farZ / (farZ - nearZ);
        r.m[2][3] = -(nearZ * farZ) / (farZ - nearZ);
        r.m[3][2] = 1.0f;   // left-handed: pushes z into w for perspective divide
        r.m[3][3] = 0.0f;
        return r;
    }
};
