#pragma once

// Just enough 4x4 matrix maths for the transform gizmo, kept out of math.hpp on
// purpose: that file promises "no matrices" and holds the shader-facing Vec3/Quat
// maths the scene and camera share. Gizmos are the first thing that genuinely
// needs matrices, so they get their own corner rather than growing math.hpp.
//
// Layout is column-major float[16] — the convention ImGuizmo (and OpenGL) use,
// so a Mat4 passes straight to ImGuizmo::Manipulate with no transpose. The view
// and projection are reconstructed to match the compute raymarcher's own ray
// generation (shaders/raymarch.comp) exactly, so the gizmo lands on the rendered
// object; ImGuizmo applies the top-left screen y-flip itself, which is the exact
// analogue of the shader's `ndc.y = -ndc.y`, so nothing here flips a sign.

#include "engine/math.hpp"

#include <array>
#include <cmath>

namespace engine {

struct Mat4 {
    std::array<float, 16> m{}; // column-major: m[col*4 + row]

    [[nodiscard]] float*       data() { return m.data(); }
    [[nodiscard]] const float* data() const { return m.data(); }
};

// View matrix from the camera's orthonormal basis and eye position. `right`,
// `up` and `forward` are the camera axes (forward points where it looks, down
// −Z in view space), passed straight from FlyCamera so the gizmo basis is
// bit-identical to the ray basis at every pitch.
[[nodiscard]] inline Mat4 viewFromBasis(Vec3 right, Vec3 up, Vec3 forward, Vec3 eye) {
    Mat4 v;
    v.m[0] = right.x;   v.m[1] = up.x;   v.m[2]  = -forward.x;  v.m[3]  = 0.0f;
    v.m[4] = right.y;   v.m[5] = up.y;   v.m[6]  = -forward.y;  v.m[7]  = 0.0f;
    v.m[8] = right.z;   v.m[9] = up.z;   v.m[10] = -forward.z;  v.m[11] = 0.0f;
    v.m[12] = -dot(right, eye);
    v.m[13] = -dot(up, eye);
    v.m[14] = dot(forward, eye);
    v.m[15] = 1.0f;
    return v;
}

// OpenGL right-handed perspective (clip z in [-1, 1]). fovY in radians. The
// depth range is arbitrary — the raymarcher has no depth buffer and ImGuizmo
// uses z only for behind-camera culling — so near/far just need to bracket the
// scene.
[[nodiscard]] inline Mat4 perspective(float fovYRadians, float aspect, float nearZ,
                                      float farZ) {
    const float g = 1.0f / std::tan(fovYRadians * 0.5f);
    Mat4 p;
    p.m[0]  = g / aspect;
    p.m[5]  = g;
    p.m[10] = (farZ + nearZ) / (nearZ - farZ);
    p.m[11] = -1.0f;
    p.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    return p;
}

// Pure translation.
[[nodiscard]] inline Mat4 translation(Vec3 t) {
    Mat4 m;
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}

// Column-major matrix product a*b.
[[nodiscard]] inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = s;
        }
    }
    return r;
}

// Transform a homogeneous point, returning clip-space (x, y, z, w).
[[nodiscard]] inline std::array<float, 4> transformVec4(const Mat4& m, float x, float y, float z,
                                                        float w) {
    return {m.m[0] * x + m.m[4] * y + m.m[8] * z + m.m[12] * w,
            m.m[1] * x + m.m[5] * y + m.m[9] * z + m.m[13] * w,
            m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14] * w,
            m.m[3] * x + m.m[7] * y + m.m[11] * z + m.m[15] * w};
}

// Model matrix from translation + unit quaternion. No scale term: primitives
// carry none (a non-uniform scale breaks the SDF metric), so the gizmo edits a
// pure rigid transform and the dimensions mode writes params separately.
[[nodiscard]] inline Mat4 composeTR(Vec3 pos, Quat q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 model;
    model.m[0]  = 1.0f - 2.0f * (y * y + z * z);
    model.m[1]  = 2.0f * (x * y + w * z);
    model.m[2]  = 2.0f * (x * z - w * y);
    model.m[4]  = 2.0f * (x * y - w * z);
    model.m[5]  = 1.0f - 2.0f * (x * x + z * z);
    model.m[6]  = 2.0f * (y * z + w * x);
    model.m[8]  = 2.0f * (x * z + w * y);
    model.m[9]  = 2.0f * (y * z - w * x);
    model.m[10] = 1.0f - 2.0f * (x * x + y * y);
    model.m[12] = pos.x;
    model.m[13] = pos.y;
    model.m[14] = pos.z;
    model.m[15] = 1.0f;
    return model;
}

// Extract the unit quaternion from a model matrix's upper-left 3x3. Columns are
// normalised first so any residual scale ImGuizmo left in the matrix does not
// leak into the rotation; with the gizmo locked to rotate-only they are already
// unit, so this is just insurance. Shepperd's method — numerically stable across
// all four cases. The caller resolves the quaternion's sign against the stored
// one, because q and -q are the same rotation.
[[nodiscard]] inline Quat quatFromMat4(const Mat4& t) {
    Vec3 c0 = normalise(Vec3{t.m[0], t.m[1], t.m[2]});
    Vec3 c1 = normalise(Vec3{t.m[4], t.m[5], t.m[6]});
    Vec3 c2 = normalise(Vec3{t.m[8], t.m[9], t.m[10]});

    const float r00 = c0.x, r10 = c0.y, r20 = c0.z;
    const float r01 = c1.x, r11 = c1.y, r21 = c1.z;
    const float r02 = c2.x, r12 = c2.y, r22 = c2.z;

    const float trace = r00 + r11 + r22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (r21 - r12) / s;
        q.y = (r02 - r20) / s;
        q.z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2.0f;
        q.w = (r21 - r12) / s;
        q.x = 0.25f * s;
        q.y = (r01 + r10) / s;
        q.z = (r02 + r20) / s;
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2.0f;
        q.w = (r02 - r20) / s;
        q.x = (r01 + r10) / s;
        q.y = 0.25f * s;
        q.z = (r12 + r21) / s;
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2.0f;
        q.w = (r10 - r01) / s;
        q.x = (r02 + r20) / s;
        q.y = (r12 + r21) / s;
        q.z = 0.25f * s;
    }

    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-6f) {
        q.x /= len;
        q.y /= len;
        q.z /= len;
        q.w /= len;
    }
    return q;
}

// Orientation quaternion from a camera basis (right, up, forward). The camera's
// world rotation has columns (right, up, -forward) — it looks down its local −Z —
// so quatRotate(result, {0,0,-1}) recovers `forward`. Used to capture a camera
// keyframe; sampled back with quatRotate on playback.
[[nodiscard]] inline Quat quatFromCameraBasis(Vec3 right, Vec3 up, Vec3 forward) {
    Mat4 r;
    r.m[0]  = right.x;   r.m[1]  = right.y;   r.m[2]  = right.z;
    r.m[4]  = up.x;      r.m[5]  = up.y;      r.m[6]  = up.z;
    r.m[8]  = -forward.x; r.m[9] = -forward.y; r.m[10] = -forward.z;
    r.m[15] = 1.0f;
    return quatFromMat4(r);
}

} // namespace engine
