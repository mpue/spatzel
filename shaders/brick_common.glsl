#ifndef FITZEL_BRICK_COMMON_GLSL
#define FITZEL_BRICK_COMMON_GLSL

// The sparse brick structure, shared between the bake passes and the brick
// marcher. Mirrored in engine/brick.hpp — the constants below and the Cell
// layout must agree with the C++ side byte for byte.
//
// The scene AABB is deliberately NOT a constant here: it travels in each pass's
// push-constant block so it can be retuned (a wider ground footprint, a taller
// box) without recompiling a shader.

const int kGridRes       = 64;                                   // dense top-level cells per axis
const int kBrickInterior = 8;                                    // interior voxels per axis
const int kBrickApron    = 1;                                    // ghost voxels each side
const int kBrickSize     = kBrickInterior + 2 * kBrickApron;     // 10
const int kBrickVoxels   = kBrickSize * kBrickSize * kBrickSize; // 1000

// One dense top-level entry. brickSlot < 0 marks a cell the bake proved holds
// no surface; emptyDistance is then the signed scene distance at the cell
// centre, the basis for conservative empty-space skipping. For an occupied cell
// brickSlot indexes the brick pool and emptyDistance is unused.
struct Cell {
    int   brickSlot;
    float emptyDistance;
};

int cellLinearIndex(ivec3 c) {
    return (c.z * kGridRes + c.y) * kGridRes + c.x;
}

int voxelLinearIndex(ivec3 v) {
    return (v.z * kBrickSize + v.y) * kBrickSize + v.x;
}

// Per-axis size of a top-level cell. Non-uniform on purpose: the ground plane
// is linear, so trilinear reproduces it exactly however coarse the cell, which
// lets the horizontal axes be stretched to cover more ground without error,
// while the vertical axis stays fine enough for the objects.
vec3 cellSizeOf(vec3 aabbMin, vec3 aabbMax) {
    return (aabbMax - aabbMin) / float(kGridRes);
}

// World position sampled by voxel `v` (each component 0..kBrickSize-1) of the
// brick owning the cell with minimum corner cellMin and per-axis voxel spacing
// `step`. Voxels are cell-centred: interior voxel 0 sits half a step inside
// cellMin, so voxel v is at cellMin + (v - kBrickApron + 0.5) * step. The two
// apron voxels straddle the cell boundary, which is exactly what lets a
// trilinear read anywhere inside the cell reach only samples this brick owns.
vec3 voxelWorldPos(vec3 cellMin, vec3 step, ivec3 v) {
    return cellMin + (vec3(v) - float(kBrickApron) + 0.5) * step;
}

#endif // FITZEL_BRICK_COMMON_GLSL
