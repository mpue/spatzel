#pragma once

// The sparse SDF brick structure, C++ side. Every constant and layout here
// mirrors shaders/brick_common.glsl and must agree with it byte for byte — the
// bake shaders read and write these buffers with the GLSL definitions, the
// engine allocates and inspects them with these.
//
// v1 is deliberately conservative: a fixed-resolution dense top-level index
// over a bounded AABB, and a fixed brick pool. No hierarchy, no streaming, no
// dynamic allocation — see the non-goals in ARCHITECTURE.md.

#include "engine/math.hpp"

#include <cstdint>

namespace engine::brick {

// --- structure constants (mirror brick_common.glsl) ------------------------
inline constexpr int32_t kGridRes       = 64;                                     // cells per axis
inline constexpr int32_t kBrickInterior = 8;                                      // interior voxels per axis
inline constexpr int32_t kBrickApron    = 1;                                      // ghost voxels each side
inline constexpr int32_t kBrickSize     = kBrickInterior + 2 * kBrickApron;       // 10
inline constexpr int32_t kBrickVoxels   = kBrickSize * kBrickSize * kBrickSize;   // 1000
inline constexpr int32_t kCellCount     = kGridRes * kGridRes * kGridRes;         // 32768

// Brick pool capacity in slots. kCellCount is the absolute ceiling, but only
// surface-adjacent cells take a slot. The fixed scene bakes to ~20k occupied
// cells at this resolution — the ground plane dominates, because the
// conservative occupancy test uses the cell's 3D diagonal and a flat plane
// still claims several vertical layers. Sized with headroom above that;
// overflow is counted and reported as a hard failure rather than silently
// truncating the field.
inline constexpr int32_t kPoolCapacity = 24576; // 24576 * 1000 floats = ~94 MiB

// --- scene bounds ----------------------------------------------------------
// The world the brick renderer can represent. Non-cubic on purpose: the ground
// plane is linear and therefore reproduced exactly by trilinear interpolation
// however coarse the horizontal cells, so the x/z axes are stretched to cover
// far more ground (fewer cells wasted on it) while y stays fine for the
// objects. Passed to every brick pass as push-constant data, so retuning these
// is a recompile of nothing.
inline constexpr Vec3 kAabbMin{-20.0f, -4.0f, -20.0f};
inline constexpr Vec3 kAabbMax{ 20.0f, 12.0f,  20.0f};

// One dense top-level entry. Mirrors `struct Cell` in brick_common.glsl.
struct Cell {
    int32_t brickSlot     = -1;   // < 0 => empty
    float   emptyDistance = 0.0f; // signed scene distance at cell centre when empty
};
static_assert(sizeof(Cell) == 8, "Cell must match its std430 GLSL counterpart");

// The bump-allocator / diagnostics block read back after the bake. Mirrors the
// Stats SSBO the classify pass writes.
struct BakeStats {
    uint32_t bricksRequested = 0; // slots the classify pass tried to claim
    uint32_t overflowCount   = 0; // claims past kPoolCapacity — must be 0
    uint32_t reserved0       = 0;
    uint32_t reserved1       = 0;
};
static_assert(sizeof(BakeStats) == 16, "BakeStats must match its std430 counterpart");

} // namespace engine::brick
