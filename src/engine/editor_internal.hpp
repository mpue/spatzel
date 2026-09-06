#pragma once

// Helpers shared across the editor translation units (editor.cpp and the
// editor_*.cpp panel files). Small enough to be inline in a header; kept out of
// editor.hpp so they do not leak into every includer of the Editor class.

#include "engine/scene.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace engine {

// Display names for the type/operator combos (with spaces, unlike the JSON
// serialisation names in scene_io.cpp).
inline constexpr std::array<const char*, 6> kTypeNames{"Sphere", "Box",        "Torus",
                                                       "Plane",  "Round Cone", "Cylinder"};
inline constexpr std::array<const char*, 4> kOperatorNames{"Union", "SmoothUnion", "Subtract",
                                                           "Intersect"};

inline constexpr std::size_t kMaxUndo = 128;

inline const char* typeLabel(std::int32_t type) {
    return (type >= 0 && type < static_cast<std::int32_t>(kTypeNames.size()))
               ? kTypeNames[static_cast<std::size_t>(type)]
               : "?";
}

// A fresh primitive of the given type, lifted off the ground and given sensible
// per-type default parameters.
inline GpuPrimitive makeDefault(PrimitiveType type) {
    GpuPrimitive p{};             // rotation defaults to identity, albedo to grey
    p.position[1] = 1.0f;         // lifted off the ground so it is visible
    p.albedo[0]   = 0.80f;
    p.albedo[1]   = 0.70f;
    p.albedo[2]   = 0.45f;
    p.control[0]  = static_cast<std::int32_t>(type);
    p.control[1]  = static_cast<std::int32_t>(Operator::Union);
    switch (type) {
        case PrimitiveType::Sphere:
            p.params[0] = 0.5f;
            break;
        case PrimitiveType::Box:
            p.params[0] = p.params[1] = p.params[2] = 0.5f;
            p.params[3] = 0.05f;
            break;
        case PrimitiveType::Torus:
            p.params[0] = 0.6f;
            p.params[1] = 0.2f;
            break;
        case PrimitiveType::Plane:
            p.position[1] = 0.0f;
            p.params[1]   = 1.0f; // unit normal pointing up
            break;
        case PrimitiveType::RoundCone:
            p.params[0] = 1.0f;  // height
            p.params[1] = 0.15f; // base radius
            p.params[2] = 0.08f; // tip radius
            break;
        case PrimitiveType::Cylinder:
            p.params[0] = 0.4f; // radius
            p.params[1] = 0.6f; // half-height
            break;
    }
    return p;
}

// Renormalise a quaternion in place; the shaders assume a unit quaternion.
inline void normaliseQuat(float q[4]) {
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-6f) {
        for (int i = 0; i < 4; ++i) {
            q[i] /= len;
        }
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

} // namespace engine
