#pragma once

// Just enough vector maths for a camera and a scene description. Deliberately
// not a maths library: when something needs matrices, quaternion slerp or SIMD,
// that is the moment to pull in a real one rather than to grow this file.

#include <cmath>

namespace engine {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

[[nodiscard]] inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] inline Vec3 operator*(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
inline Vec3& operator+=(Vec3& a, Vec3 b) { return a = a + b; }

[[nodiscard]] inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

[[nodiscard]] inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }

[[nodiscard]] inline Vec3 normalise(Vec3 v) {
    const float len = length(v);
    return len > 0.0f ? v * (1.0f / len) : Vec3{};
}

// Unit quaternion, xyz + w. Scene primitives carry one of these; there is no
// non-uniform scale anywhere, on purpose — see ARCHITECTURE.md.
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

[[nodiscard]] inline Quat quatFromAxisAngle(Vec3 axis, float radians) {
    const Vec3  n = normalise(axis);
    const float s = std::sin(radians * 0.5f);
    return {n.x * s, n.y * s, n.z * s, std::cos(radians * 0.5f)};
}

// Hamilton product: the rotation `b` followed by `a` (a * b). Enough to compose
// turtle turns; still no slerp or matrices here, on purpose (see the file note).
[[nodiscard]] inline Quat quatMul(Quat a, Quat b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

// Rotate a vector by a unit quaternion: v + 2w(q×v) + 2(q×(q×v)).
[[nodiscard]] inline Vec3 quatRotate(Quat q, Vec3 v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

} // namespace engine
