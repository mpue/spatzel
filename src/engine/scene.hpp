#pragma once

// The edit list — the authoritative description of the scene.
//
// This is data, uploaded to a storage buffer and evaluated by the raymarching
// kernel. Nothing about the scene is baked into the shader body. That is the
// whole point: the accelerated renderer that comes later has to consume this
// same list and match the brute-force result produced from it.

#include "engine/math.hpp"

#include <cstdint>
#include <vector>

namespace engine {

enum class PrimitiveType : int32_t {
    Sphere = 0,
    Box    = 1,
    Torus  = 2,
    Plane  = 3,
};

enum class Operator : int32_t {
    Union       = 0,
    SmoothUnion = 1,
};

// GPU layout. Every member is a 16-byte slot, which makes std430 (the storage
// buffer in the Vulkan variant) and std140 agree, and makes this struct
// byte-identical to its GLSL counterpart without any padding arithmetic.
struct alignas(16) GpuPrimitive {
    float   position[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz, w = smooth-union blend radius
    float   rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // unit quaternion
    float   params[4]   = {};                       // per type, see below
    float   albedo[4]   = {0.8f, 0.8f, 0.8f, 0.0f};
    int32_t control[4]  = {};                       // x = PrimitiveType, y = Operator
};
static_assert(sizeof(GpuPrimitive) == 80, "GpuPrimitive must match its GLSL counterpart");

// Parameter conventions, shared with raymarch.comp:
//   Sphere  params.x   = radius
//   Box     params.xyz = half extents, params.w = corner rounding
//   Torus   params.x   = major radius, params.y = minor radius
//   Plane   params.xyz = unit normal,  params.w = offset along it
//
// There is no scale: a non-uniform scale destroys the distance metric that
// sphere tracing depends on. Translation and rotation are what an SDF
// primitive can carry exactly.

// The fixed milestone scene: two spheres melted together by a smooth union, a
// rotated rounded box, and a ground plane.
[[nodiscard]] std::vector<GpuPrimitive> buildScene();

} // namespace engine
