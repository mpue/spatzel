#ifndef FITZEL_FLUID_COMMON_GLSL
#define FITZEL_FLUID_COMMON_GLSL

// The fluid grid, shared by every simulation pass and by the two marchers.
// Mirrored in engine/fluid.hpp — the layouts below and the C++ structs must
// agree byte for byte.
//
// The domain is a CUBE of `res` cells per axis with a single uniform spacing h.
// That is not laziness: a non-uniform spacing turns the pressure Laplacian into
// a different stencil per axis, and the whole point of this first stage is a
// pressure solve simple enough to be obviously right. The brick grid may be
// stretched (its field is static and only ever sampled); this one may not.
//
// Fields:
//   phi        cell-centred signed distance to the water surface, res^3.
//              Negative inside the water. This IS the render representation —
//              the marchers union it with the scene field directly, which is
//              the reason a level set was chosen over a density.
//   pressure   cell-centred, res^3.
//   divergence cell-centred, res^3.
//   solid      cell-centred sample of the SCENE distance field, res^3. Negative
//              inside an obstacle. Obstacles are therefore the edit list
//              itself; there is no second collision representation, in the same
//              way that there is no second scene representation.
//   velocity   a MAC (staggered) grid: u on x-faces, v on y-faces, w on
//              z-faces. All three components share one buffer and one uniform
//              (res+1)^3 lattice, at component offsets 0, S and 2S with
//              S = (res+1)^3. A true MAC grid would need (res+1)*res*res for u;
//              the uniform lattice wastes the slack faces and never reads them,
//              which buys a single index function for all three components.
//
// A pass declares which of these it wants by #defining the matching FLUID_*
// macro before including this file. Declaring a buffer a pipeline does not bind
// is a descriptor-layout mismatch, so the set is opt-in rather than blanket.

// How far outside the water, in cells, velocity is kept meaningful. Inside this
// band the air velocity is extrapolated from the fluid so that a backtrace
// starting just above the surface reads something sensible; beyond it velocity
// is pinned to zero, which is what stops gravity from integrating an unbounded
// downward drift into air the solver never looks at.
const float kFluidVelocityBand = 4.0;

// --- indexing ---------------------------------------------------------------

int fluidCellIndex(ivec3 c, int res) {
    return (c.z * res + c.y) * res + c.x;
}

int fluidFaceStride(int res) {
    const int s = res + 1;
    return s * s * s;
}

int fluidFaceIndex(ivec3 f, int res) {
    const int s = res + 1;
    return (f.z * s + f.y) * s + f.x;
}

// axis: 0 = u (x-faces), 1 = v (y-faces), 2 = w (z-faces).
int fluidVelIndex(int axis, ivec3 f, int res) {
    return axis * fluidFaceStride(res) + fluidFaceIndex(f, res);
}

vec3 fluidCellCentre(vec3 origin, float h, ivec3 c) {
    return origin + (vec3(c) + 0.5) * h;
}

// World position of a face sample. u sits on the low-x face of cell (i,j,k) and
// is cell-centred in the other two axes; v and w rotate that.
vec3 fluidFacePos(int axis, vec3 origin, float h, ivec3 f) {
    vec3 offset = vec3(f) + vec3(0.5);
    offset[axis] = float(f[axis]);
    return origin + offset * h;
}

// Is this face on the outer wall of the domain? The tank has solid walls, so
// the normal velocity there is pinned to zero.
bool fluidWallFace(int axis, ivec3 f, int res) {
    return f[axis] == 0 || f[axis] == res;
}

// Does this face carry a real degree of freedom for `axis`? The shared lattice
// has (res+1)^3 entries per component but u only exists for j, k < res.
bool fluidFaceLive(int axis, ivec3 f, int res) {
    ivec3 limit = ivec3(res - 1);
    limit[axis] = res;
    return all(lessThanEqual(f, limit));
}

// --- push constants ---------------------------------------------------------
//
// One block shape for every simulation pass, so the C++ side has exactly one
// struct to keep in sync. Members are all vec4/ivec4, which makes the Vulkan
// push-constant layout and the OpenGL std140 uniform block agree without any
// padding arithmetic — the same trick the marchers use.

#ifdef FLUID_PUSH
#ifdef TARGET_VULKAN
layout(push_constant) uniform FluidUniforms {
#else
layout(binding = 0, std140) uniform FluidUniforms {
#endif
    vec4  domain;  // xyz = domain minimum corner, w = cell size h
    ivec4 control; // x = res, y = primitiveCount, z = spare, w = flags
    vec4  step;    // x = dt, y = gravity (applied to v), z, w = spare
    vec4  seedMin; // xyz = seed box minimum, w = still-water level (y)
    vec4  seedMax; // xyz = seed box maximum, w = spare
} u;
#endif

// --- resource declarations --------------------------------------------------

#ifdef TARGET_VULKAN
#define FLUID_SET layout(set = 0,
#else
#define FLUID_SET layout(
#endif

#ifdef FLUID_PARAMS
// The render-side parameter block. The marchers are already at 124 of their 128
// push-constant bytes, so the fluid's own parameters travel in a buffer instead
// of in that block — which also means enabling the fluid costs the marchers one
// binding and no layout churn.
struct FluidParams {
    vec4  origin;   // xyz = domain minimum corner, w = cell size h
    ivec4 control;  // x = res, y = enabled, z, w = spare
    vec4  water;    // rgb = albedo / Beer-Lambert tint, w = transmission
    vec4  material; // x = roughness, y = metallic, z = index of refraction, w = spare
    vec4  render;   // x = trust band in cells, y = surface offset, z, w = spare
};
FLUID_SET binding = 8, std430) readonly buffer FluidParamsBlock { FluidParams gFluid; };
#endif

#ifdef FLUID_VEL_SRC
FLUID_SET binding = 9, std430) readonly buffer FluidVelSrc { float gVelSrc[]; };
#endif
#ifdef FLUID_VEL_DST
FLUID_SET binding = 10, std430) writeonly buffer FluidVelDst { float gVelDst[]; };
#endif
#ifdef FLUID_PHI_SRC
FLUID_SET binding = 11, std430) readonly buffer FluidPhiSrc { float gPhiSrc[]; };
#endif
#ifdef FLUID_PHI_DST
FLUID_SET binding = 12, std430) writeonly buffer FluidPhiDst { float gPhiDst[]; };
#endif
#ifdef FLUID_PRESSURE
// Read-write and single-buffered, unlike every other field here. The pressure
// solve is a red-black Gauss-Seidel sweep: the two colours are separate
// dispatches, and within one dispatch a cell only ever reads its six
// neighbours, which all carry the other colour. So there is no read-write
// hazard to double-buffer against, and updating in place is exactly what makes
// it Gauss-Seidel rather than Jacobi — each colour sees the other colour's
// answer from this sweep instead of from the last one.
FLUID_SET binding = 13, std430) buffer FluidPressureBlock { float gPressure[]; };
#endif
#ifdef FLUID_DIVERGENCE_R
FLUID_SET binding = 15, std430) readonly buffer FluidDivergenceR { float gDivergence[]; };
#endif
#ifdef FLUID_DIVERGENCE_W
FLUID_SET binding = 15, std430) writeonly buffer FluidDivergenceW { float gDivergenceOut[]; };
#endif
#ifdef FLUID_SOLID_R
FLUID_SET binding = 16, std430) readonly buffer FluidSolidR { float gSolid[]; };
#endif
#ifdef FLUID_SOLID_RW
// Read-write for the bake alone, which needs the previous frame's obstacle
// field to see how far the obstacle moved. Each invocation touches only its own
// cell, so reading the old value and writing the new one in the same dispatch is
// not a hazard.
FLUID_SET binding = 16, std430) buffer FluidSolidRW { float gSolid[]; };
#endif
#ifdef FLUID_SOLID_SPEED_R
FLUID_SET binding = 18, std430) readonly buffer FluidSolidSpeedR { float gSolidSpeed[]; };
#endif
#ifdef FLUID_SOLID_SPEED_W
FLUID_SET binding = 18, std430) writeonly buffer FluidSolidSpeedW { float gSolidSpeedOut[]; };
#endif
#ifdef FLUID_STATS
FLUID_SET binding = 17, std430) buffer FluidStatsBlock { uint gStats[]; };
#endif

// --- field access -----------------------------------------------------------
//
// Everything below reads only the buffers its own guard declared, so a pass
// that did not ask for a field cannot accidentally reach it.

#ifdef FLUID_SOLID_R
// Cells outside the domain read as their nearest in-domain neighbour: the walls
// are solid anyway, so the clamp never invents free space.
float fluidSolidAt(ivec3 c, int res) {
    return gSolid[fluidCellIndex(clamp(c, ivec3(0), ivec3(res - 1)), res)];
}
bool fluidSolidCell(ivec3 c, int res) {
    return fluidSolidAt(c, res) < 0.0;
}
// A face is closed if it lies on the tank wall or if either cell it separates
// is inside an obstacle. First order and deliberately so — a fractional face
// area is the obvious next refinement, not a correctness fix.
bool fluidFaceClosed(int axis, ivec3 f, int res) {
    if (fluidWallFace(axis, f, res)) {
        return true;
    }
    ivec3 lo = f;
    lo[axis] -= 1;
    return fluidSolidCell(lo, res) || fluidSolidCell(f, res);
}
#endif

#if defined(FLUID_SOLID_R) && defined(FLUID_SOLID_SPEED_R)
// The velocity a closed face imposes on the fluid — zero for a stationary
// obstacle, and the obstacle's own velocity along this face's axis when it
// moves. This is the whole of the moving-obstacle coupling: every pass that
// used to write a hard zero at a closed face writes this instead.
//
// The obstacle's velocity is never told to the solver. It is *read off the
// obstacle field's own motion*: a level set transported by a velocity satisfies
// phi_t + v . grad(phi) = 0, so on a field that is a distance function
// (|grad phi| = 1) the surface's normal speed is simply -phi_t. The bake stores
// that scalar per cell; the direction comes from the gradient here.
//
// Only the normal component is recoverable that way — a sphere spinning in place
// has phi_t = 0 everywhere — and only the normal component is wanted: the
// boundary condition here is free-slip, constraining u . n and leaving the
// tangential flow alone. A no-slip wall that drags water around with it would
// need the tangential velocity too, and that genuinely would need per-primitive
// motion data.
float fluidClosedFaceVelocity(int axis, ivec3 f, int res) {
    if (fluidWallFace(axis, f, res)) {
        return 0.0; // the tank itself never moves
    }

    ivec3 lo = f;
    lo[axis] -= 1;

    // The obstacle half of the pair: the face is closed, so at least one of the
    // two cells is inside the obstacle, and that one's field describes the
    // surface that is moving.
    const ivec3 c = fluidSolidCell(lo, res) ? lo : f;

    const float speed = gSolidSpeed[fluidCellIndex(clamp(c, ivec3(0), ivec3(res - 1)), res)];
    if (speed == 0.0) {
        return 0.0; // stationary obstacle: the old hard zero, reached cheaply
    }

    // The surface normal, from the gradient of the obstacle field. The 1/(2h)
    // of the central difference cancels in the normalisation, so it is left out.
    vec3 g;
    for (int a = 0; a < 3; ++a) {
        ivec3 gh = c;
        ivec3 gl = c;
        gh[a] += 1;
        gl[a] -= 1;
        g[a] = fluidSolidAt(gh, res) - fluidSolidAt(gl, res);
    }

    const float len = length(g);
    if (len < 1.0e-6) {
        return 0.0; // no usable normal — do not invent a direction
    }
    return speed * (g[axis] / len);
}
#endif

#ifdef FLUID_PHI_SRC
float fluidPhiAt(ivec3 c, int res) {
    return gPhiSrc[fluidCellIndex(clamp(c, ivec3(0), ivec3(res - 1)), res)];
}

// Trilinear read of the cell-centred level set. Sample points are cell centres,
// so the lattice is offset by half a cell; the clamp extends the boundary cells
// outward, which is what keeps a read at the very wall in range.
float fluidSamplePhi(vec3 p, vec3 origin, float h, int res) {
    const vec3  g = (p - origin) / h - 0.5;
    const vec3  b = floor(g);
    const vec3  t = clamp(g - b, 0.0, 1.0);
    const ivec3 c = ivec3(b);

    const float c000 = fluidPhiAt(c + ivec3(0, 0, 0), res);
    const float c100 = fluidPhiAt(c + ivec3(1, 0, 0), res);
    const float c010 = fluidPhiAt(c + ivec3(0, 1, 0), res);
    const float c110 = fluidPhiAt(c + ivec3(1, 1, 0), res);
    const float c001 = fluidPhiAt(c + ivec3(0, 0, 1), res);
    const float c101 = fluidPhiAt(c + ivec3(1, 0, 1), res);
    const float c011 = fluidPhiAt(c + ivec3(0, 1, 1), res);
    const float c111 = fluidPhiAt(c + ivec3(1, 1, 1), res);

    return mix(mix(mix(c000, c100, t.x), mix(c010, c110, t.x), t.y),
               mix(mix(c001, c101, t.x), mix(c011, c111, t.x), t.y), t.z);
}

// The level set at a face, as the mean of the two cells it separates. A smooth
// measure of how far a face is from the water — what the extrapolation band and
// the gravity band are cut against.
float fluidPhiAtFace(int axis, ivec3 f, int res) {
    ivec3 lo = f;
    lo[axis] -= 1;
    return 0.5 * (fluidPhiAt(lo, res) + fluidPhiAt(f, res));
}
#endif

#if defined(FLUID_PHI_SRC) && defined(FLUID_SOLID_R)
// A cell carrying a pressure unknown: water, and not inside an obstacle.
bool fluidIsWater(ivec3 c, int res) {
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, ivec3(res)))) {
        return false;
    }
    return !fluidSolidCell(c, res) && fluidPhiAt(c, res) < 0.0;
}

// Does this face carry a velocity the projection actually solved for?
//
// The distinction matters more than it looks. A face between a water cell and
// an air cell IS solved — it is part of the free-surface boundary condition —
// even though the level set averaged across it comes out positive. Deciding
// validity from the averaged phi instead would let the extrapolation pass
// overwrite half the surface velocities the projection had just made
// divergence-free, and the water would quietly stop conserving volume.
bool fluidFaceSolved(int axis, ivec3 f, int res) {
    ivec3 lo = f;
    lo[axis] -= 1;
    return fluidIsWater(lo, res) || fluidIsWater(f, res);
}
#endif

#ifdef FLUID_VEL_SRC
float fluidVelAt(int axis, ivec3 f, int res) {
    return gVelSrc[fluidVelIndex(axis, clamp(f, ivec3(0), ivec3(res)), res)];
}

// Trilinear read of one velocity component at an arbitrary world point. The
// component's lattice is staggered by half a cell in the two axes it is NOT
// aligned with, which is exactly the offset undone here.
float fluidSampleVelocity(int axis, vec3 p, vec3 origin, float h, int res) {
    vec3 g = (p - origin) / h - 0.5;
    g[axis] += 0.5;

    const vec3  b = floor(g);
    const vec3  t = clamp(g - b, 0.0, 1.0);
    const ivec3 c = ivec3(b);

    const float c000 = fluidVelAt(axis, c + ivec3(0, 0, 0), res);
    const float c100 = fluidVelAt(axis, c + ivec3(1, 0, 0), res);
    const float c010 = fluidVelAt(axis, c + ivec3(0, 1, 0), res);
    const float c110 = fluidVelAt(axis, c + ivec3(1, 1, 0), res);
    const float c001 = fluidVelAt(axis, c + ivec3(0, 0, 1), res);
    const float c101 = fluidVelAt(axis, c + ivec3(1, 0, 1), res);
    const float c011 = fluidVelAt(axis, c + ivec3(0, 1, 1), res);
    const float c111 = fluidVelAt(axis, c + ivec3(1, 1, 1), res);

    return mix(mix(mix(c000, c100, t.x), mix(c010, c110, t.x), t.y),
               mix(mix(c001, c101, t.x), mix(c011, c111, t.x), t.y), t.z);
}

vec3 fluidSampleVelocity3(vec3 p, vec3 origin, float h, int res) {
    return vec3(fluidSampleVelocity(0, p, origin, h, res),
                fluidSampleVelocity(1, p, origin, h, res),
                fluidSampleVelocity(2, p, origin, h, res));
}

// Second-order backtrace (midpoint / RK2). Semi-Lagrangian advection is
// unconditionally stable whatever the step, so the timestep is fixed and the
// only question left is how much the trajectory bends inside it.
vec3 fluidBacktrace(vec3 p, float dt, vec3 origin, float h, int res) {
    const vec3 v0  = fluidSampleVelocity3(p, origin, h, res);
    const vec3 mid = p - 0.5 * dt * v0;
    return p - dt * fluidSampleVelocity3(mid, origin, h, res);
}
#endif

#endif // FITZEL_FLUID_COMMON_GLSL
