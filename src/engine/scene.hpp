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
    Sphere    = 0,
    Box       = 1,
    Torus     = 2,
    Plane     = 3,
    RoundCone = 4, // tapered capsule: the natural branch/trunk primitive
    Cylinder  = 5, // capped cylinder along local Y
};

enum class Operator : int32_t {
    Union       = 0,
    SmoothUnion = 1,
    Subtract    = 2, // max(acc, -d): carve this primitive out of what precedes it
    Intersect   = 3, // max(acc,  d): keep only the overlap with what precedes it
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
    float   material[4] = {0.6f, 0.0f, 0.0f, 0.0f}; // x = roughness, y = metallic,
                                                    // z = emissive, w = reserved
};
static_assert(sizeof(GpuPrimitive) == 96, "GpuPrimitive must match its GLSL counterpart");

// Parameter conventions, shared with raymarch.comp:
//   Sphere    params.x   = radius
//   Box       params.xyz = half extents, params.w = corner rounding
//   Torus     params.x   = major radius, params.y = minor radius
//   Plane     params.xyz = unit normal,  params.w = offset along it
//   RoundCone params.x   = height h, params.y = base radius, params.z = tip radius.
//             The base sits at `position`; the tip at position + rotation·(0, h, 0).
//             A proper 1-Lipschitz distance function, like every primitive here.
//   Cylinder  params.x   = radius, params.y = half-height. Capped, axis local Y.
//
// There is no scale: a non-uniform scale destroys the distance metric that
// sphere tracing depends on. Translation and rotation are what an SDF
// primitive can carry exactly.
//
// material.w is the glass transmission (0 = opaque, 1 = clear): at a hit the
// marcher refracts through the object and tints the transmitted light by the
// albedo (Beer-Lambert). See traceGlass in shading.glsl.

// The fixed milestone scene: two spheres melted together by a smooth union, a
// rotated rounded box, and a ground plane.
[[nodiscard]] std::vector<GpuPrimitive> buildScene();

} // namespace engine
