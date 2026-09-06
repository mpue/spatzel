#pragma once

// The fluid grid, C++ side. Every constant and layout here mirrors
// shaders/fluid_common.glsl and must agree with it byte for byte — the solver
// passes read and write these buffers with the GLSL definitions, the engine
// allocates and inspects them with these.
//
// What this is
// ------------
// An Eulerian liquid on a staggered (MAC) grid whose surface is a level set:
// advection, a Jacobi pressure projection, velocity extrapolation into the air
// and redistancing, all in compute, all in storage buffers.
//
// The level set is not an implementation detail — it is the reason this shape
// of solver was chosen. The renderer marches signed distance fields; a level
// set IS a signed distance field; so the water reaches the screen through the
// same union the rest of the scene goes through, with no meshing step, no
// screen-space surface reconstruction and no second renderer. See
// shaders/fluid_field.glsl.
//
// What it is not, yet
// -------------------
// Grid-only advection of a level set loses volume, visibly, over a long run.
// That is inherent to the method and not a bug to be tuned away; the fix is
// particles (FLIP/PIC) carrying the velocity and the surface, which is the
// natural next stage and which this grid is exactly the substrate for. The
// volume drift is measured and reported every frame rather than hidden, in the
// same spirit as the brick renderer's comparison against the reference: the
// honest number is on screen.

#include "engine/math.hpp"

#include <cstdint>

namespace engine::fluid {

// --- structure constants (mirror fluid_common.glsl) ------------------------

inline constexpr int32_t kDefaultRes = 64;  // cells per axis at startup
inline constexpr int32_t kMinRes     = 16;  // editor slider floor
inline constexpr int32_t kMaxRes     = 128; // editor slider ceiling; buffers sized for this

// Cell-centred fields (phi, pressure, divergence, solid) are res^3. The
// velocity components share one uniform (res+1)^3 lattice at three offsets; see
// the header comment in fluid_common.glsl for why the slack faces are worth it.
constexpr int64_t cellCount(int32_t res) {
    return static_cast<int64_t>(res) * res * res;
}
constexpr int64_t faceCount(int32_t res) {
    const int64_t s = res + 1;
    return s * s * s;
}

inline constexpr int64_t kMaxCellCount = cellCount(kMaxRes); // 2,097,152
inline constexpr int64_t kMaxFaceCount = faceCount(kMaxRes); // 2,146,689

// Total device memory the solver holds once it is switched on, at the ceiling:
// two velocity buffers (3 components each), two level sets, two pressures, one
// divergence, one obstacle field. Roughly 93 MiB at res 128, 12 MiB at 64 —
// but allocated for the ceiling either way, so moving the resolution slider
// never reallocates a GPU buffer. Nothing is allocated at all until the fluid
// is first switched on: an engine run that never touches water pays nothing.
inline constexpr int64_t kResidentFloats =
    2 * 3 * kMaxFaceCount + 2 * kMaxCellCount + 3 * kMaxCellCount;

// --- GPU layouts -----------------------------------------------------------

// The render-side parameter block (slot 8). Read by both marchers; written by
// the engine whenever a fluid setting changes. All vec4-sized members, so
// std430 and std140 agree and no padding arithmetic is needed.
struct alignas(16) GpuParams {
    float   origin[4]   = {};                       // xyz = domain minimum, w = cell size
    int32_t control[4]  = {};                       // x = res, y = enabled
    float   water[4]    = {};                       // rgb = tint, w = transmission
    float   material[4] = {};                       // x = roughness, y = metallic, z = ior
    float   render[4]   = {};                       // x = trust band (cells), y = surface offset
};
static_assert(sizeof(GpuParams) == 80, "GpuParams must match its GLSL counterpart");

// The push-constant block every solver pass takes. One shape for all of them:
// the passes differ in which fields they touch, not in what they need told.
struct alignas(16) GpuPush {
    float   domain[4]  = {}; // xyz = domain minimum, w = cell size
    int32_t control[4] = {}; // x = res, y = primitiveCount
    float   step[4]    = {}; // x = dt, y = gravity
    float   seedMin[4] = {}; // xyz = seed box minimum, w = still-water level
    float   seedMax[4] = {}; // xyz = seed box maximum
};
static_assert(sizeof(GpuPush) == 80, "fluid push constants must stay under 128 bytes");

// The diagnostics block the stats pass fills with atomics, in fixed point
// because the RHI has no reduction primitive and adding one would put a
// synchronisation model in the seam. Scales mirror fluid_stats.comp.
struct GpuStats {
    uint32_t volumeMilli    = 0; // water volume in thousandths of a cell
    uint32_t divergenceMicro = 0; // max |divergence| in millionths, 1/s
    uint32_t speedMilli     = 0; // max |velocity| component in thousandths
    uint32_t reserved       = 0;
};
static_assert(sizeof(GpuStats) == 16, "GpuStats must match its std430 counterpart");

// --- settings --------------------------------------------------------------

// Everything the editor panel drives, in world units. The domain is a cube:
// a uniform cell spacing is what keeps the pressure stencil the same on all
// three axes, and that simplicity is the point of this first stage.
struct Settings {
    bool  enabled = false;
    int   res     = kDefaultRes;
    Vec3  origin{-3.0f, 0.0f, -3.0f}; // minimum corner of the tank
    float size    = 6.0f;             // edge length of the cube, world units

    // Integration. The step is fixed rather than derived from the frame time:
    // a solver whose timestep follows the frame rate is a solver whose result
    // follows the frame rate, and a pinned-time verification run could then
    // never reproduce anything.
    float timestep    = 1.0f / 120.0f;
    int   maxSubsteps = 2;       // per frame; the clock is caught up, never overtaken
    float gravity     = -9.81f;

    // Solver iteration counts. The pressure solve is red-black Gauss-Seidel;
    // one sweep is two dispatches and moves information one cell, so a column
    // `res` cells deep needs at least that many sweeps before the bottom knows
    // the surface exists. This is the knob that trades residual divergence for
    // frame time, and the residual it actually achieves is measured
    // (Stats::maxDivergence) rather than assumed.
    int pressureSweeps    = 60;
    int extrapolateSweeps = 4; // how far velocity reaches into the air, in cells
    int reinitIterations  = 4; // redistancing sweeps per step

    // Seeding: a block of water (the dam) unioned with a still pool below
    // `poolLevel`. Both are exact distance functions, so the seeded field needs
    // no redistancing to start.
    Vec3  seedMin{-3.0f, 0.0f, -3.0f};
    Vec3  seedMax{-1.2f, 4.0f, 3.0f};
    float poolLevel = 0.3f;

    // Appearance. The water is a transmissive surface with an index of its own;
    // it reaches the screen through the same path glass does.
    float colour[3]    = {0.62f, 0.82f, 0.88f};
    float transmission = 0.90f;
    float roughness    = 0.03f;
    float ior          = 1.33f;

    // How far, in cells, the marchers trust the level set as a distance before
    // saturating it. See fluidDistance() in fluid_field.glsl: a saturated
    // reading costs a few extra march steps and cannot tunnel through water.
    float trustBandCells = 8.0f;
    // Pushes the rendered surface out (positive) or in (negative), in world
    // units. A blunt but effective counter to the level set's volume loss.
    float surfaceOffset = 0.0f;

    [[nodiscard]] float cellSize() const { return size / static_cast<float>(res); }
};

// What the diagnostics readback produces, in world units.
struct Stats {
    bool  valid          = false;
    float volume         = 0.0f; // m^3, smeared-Heaviside estimate
    float referenceVolume = 0.0f; // the same measure right after the last seed
    float maxDivergence  = 0.0f; // 1/s, after the last projection of the frame
    float maxSpeed       = 0.0f; // world units/s

    // Volume drift since the seed, as a fraction. Negative means water has been
    // lost — which it will have been; see the header note.
    [[nodiscard]] float volumeDrift() const {
        return referenceVolume > 0.0f ? (volume - referenceVolume) / referenceVolume : 0.0f;
    }
};

} // namespace engine::fluid
