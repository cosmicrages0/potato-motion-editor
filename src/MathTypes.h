#pragma once
// Small header-only math primitives shared across the engine.
// Kept minimal so it compiles fast and has no dependencies.
//
// Convention: column-vector, right-multiply (v' = M * v). This matches
// DirectXMath and every AE-alike, and is important for Task 4's WVP math.

#include <cmath>
#include <algorithm>

#ifndef POTATO_PI
#define POTATO_PI 3.14159265358979323846f
#endif

// Reuse the Vec2 defined by AnimationEngine.h so we don't get ODR conflicts.
#include "AnimationEngine.h"

// 3D vector; used now for Z-ready transforms so Task 4 (single-node 3D camera)
// can promote the same struct without an ABI break.
struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float _x, float _y, float _z = 0.0f) : x(_x), y(_y), z(_z) {}
    explicit Vec3(const Vec2& v, float _z = 0.0f) : x(v.x), y(v.y), z(_z) {}

    Vec2 xy() const { return Vec2(x, y); }
};

// 3x3 affine matrix stored row-major:
//   [ m[0][0]  m[0][1]  m[0][2] ]     [ a  b  tx ]
//   [ m[1][0]  m[1][1]  m[1][2] ]  =  [ c  d  ty ]
//   [ m[2][0]  m[2][1]  m[2][2] ]     [ 0  0  1  ]
//
// This is enough for 2D scale + rotate + translate (Task 3). Task 4 will
// introduce a Mat4 for full 3D; Mat3 stays for on-canvas gizmo math because
// the 2D affine inverse is closed-form and cheap.
struct Mat3 {
    float m[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f}
    };

    static Mat3 Identity() { return Mat3(); }

    static Mat3 Translation(float tx, float ty) {
        Mat3 r;
        r.m[0][2] = tx;
        r.m[1][2] = ty;
        return r;
    }

    static Mat3 Scale(float sx, float sy) {
        Mat3 r;
        r.m[0][0] = sx;
        r.m[1][1] = sy;
        return r;
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

    // Column-vector convention: (A * B) applied to v means "first B, then A".
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r.m[i][j] = m[i][0] * o.m[0][j]
                          + m[i][1] * o.m[1][j]
                          + m[i][2] * o.m[2][j];
            }
        }
        return r;
    }

    // Transform a 2D point (implicit w = 1).
    Vec2 TransformPoint(const Vec2& p) const {
        const float x = m[0][0] * p.x + m[0][1] * p.y + m[0][2];
        const float y = m[1][0] * p.x + m[1][1] * p.y + m[1][2];
        return Vec2(x, y);
    }

    // Closed-form inverse for an affine 2D matrix (last row = [0 0 1]).
    // Cheap and stable enough for gizmo hit-testing. Falls back to identity
    // if the matrix is singular (e.g. scale of zero) so we never NaN the UI.
    Mat3 InverseAffine() const {
        const float a = m[0][0], b = m[0][1], tx = m[0][2];
        const float c = m[1][0], d = m[1][1], ty = m[1][2];
        const float det = a * d - b * c;
        if (std::fabs(det) < 1e-8f) return Mat3::Identity();
        const float invDet = 1.0f / det;
        Mat3 r;
        r.m[0][0] =  d * invDet;
        r.m[0][1] = -b * invDet;
        r.m[1][0] = -c * invDet;
        r.m[1][1] =  a * invDet;
        r.m[0][2] = -(r.m[0][0] * tx + r.m[0][1] * ty);
        r.m[1][2] = -(r.m[1][0] * tx + r.m[1][1] * ty);
        return r;
    }
};
