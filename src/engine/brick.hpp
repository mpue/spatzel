#pragma once

// The sparse SDF brick structure, C++ side. Every constant and layout here
// mirrors shaders/brick_common.glsl and must agree with it byte for byte — the
// bake shaders read and write these buffers with the GLSL definitions, the
// engine allocates and inspects them with these.
//
// v1 is deliberately conservative: a dense top-level index over a bounded AABB,
// and a fixed brick pool. The grid resolution is runtime-tunable (within a
// compile-time ceiling the buffers are sized for), but there is still no
// hierarchy, no streaming, and no dynamic allocation — see the non-goals in
// ARCHITECTURE.md.

#include "engine/math.hpp"

#include <cstdint>

namespace engine::brick {

// --- structure constants (mirror brick_common.glsl) ------------------------
// The top-level grid resolution is no longer fixed: it is a runtime value the
// editor drives, passed to every bake/march pass as push-constant data (the
// GLSL side reads it too, so brick_common.glsl no longer hard-codes it). Only
// the *ceiling* is a compile-time constant, because the cell buffer is sized
// once for the worst case and the live resolution just uses a prefix of it.
inline constexpr int32_t kDefaultGridRes = 64;  // cells per axis at startup
inline constexpr int32_t kMinGridRes     = 16;  // editor slider floor
inline constexpr int32_t kMaxGridRes     = 128; // editor slider ceiling; buffers sized for this

inline constexpr int32_t kBrickInterior = 8;                                      // interior voxels per axis
inline constexpr int32_t kBrickApron    = 1;                                      // ghost voxels each side
inline constexpr int32_t kBrickSize     = kBrickInterior + 2 * kBrickApron;       // 10
inline constexpr int32_t kBrickVoxels   = kBrickSize * kBrickSize * kBrickSize;   // 1000

// The cell buffer is allocated for the maximum resolution; a run at a lower
// resolution addresses only the leading gridRes^3 entries. Sizing for the
// ceiling means changing resolution never reallocates a GPU buffer.
inline constexpr int64_t kMaxCellCount =
    static_cast<int64_t>(kMaxGridRes) * kMaxGridRes * kMaxGridRes; // 2,097,152

// Number of live cells at a given resolution — the occupancy-stat denominator.
constexpr int64_t cellCount(int32_t gridRes) {
    return static_cast<int64_t>(gridRes) * gridRes * gridRes;
}

// Brick pool capacity in slots. cellCount() is the absolute ceiling, but only
// surface-adjacent cells take a slot, and that count grows with roughly the
// square of the resolution. The fixed scene bakes to ~16k occupied cells at the
// default 64^3 and ~67k at the 128^3 ceiling — the ground plane dominates,
// because the conservative occupancy test uses the cell's 3D diagonal and a
// flat plane still claims several vertical layers. Sized to clear the ceiling
// on the sample scenes with headroom; overflow is still counted and reported as
// a hard failure rather than silently truncating the field, so an unusually
// dense scene at max resolution surfaces a warning instead of corrupting output.
inline constexpr int32_t kPoolCapacity = 98304; // 98304 * 1000 floats = ~375 MiB

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
