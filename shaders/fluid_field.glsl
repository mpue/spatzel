#ifndef FITZEL_FLUID_FIELD_GLSL
#define FITZEL_FLUID_FIELD_GLSL

// The read side of the fluid: what a marcher needs to see water, and nothing
// else. The simulation writes a level set; a level set is a signed distance
// field; a signed distance field is what this renderer already marches. So the
// water joins the scene the same way any other surface would — through a min —
// and needs no second renderer, no marching cubes, no mesh.
//
// Include AFTER sdf_scene.glsl (this file unions with it) and BEFORE
// shading.glsl, whose refraction path marches worldSdf so that glass and water
// refract through the same combined field.

#define FLUID_PARAMS
#define FLUID_PHI_SRC
#include "fluid_common.glsl"
#include "sdf_scene.glsl"

// Distance to the water surface, positive outside.
//
// Inside the domain this is the level set, clamped to a trust band.
// Redistancing runs a fixed handful of iterations per step, so the field is a
// faithful distance near the surface and progressively less faithful away from
// it. Stepping by an unverified large value is exactly how a marcher tunnels
// through a surface, so the far field is capped instead: crossing an empty tank
// costs a few more steps and cannot skip the water.
//
// Outside the domain is the interesting half. The obvious answer — return the
// distance to the domain box, "walk there and ask again" — is wrong, and wrong
// in a way that looks like a rendering bug rather than a reasoning one: that
// value falls to zero at the box, a sphere tracer reads zero as "surface here",
// and the invisible walls of the tank render as a solid object. A step
// instruction and a surface are the same number to a marcher, so the bound has
// to stay positive wherever there is no water.
//
// Two facts give a bound that does. Writing W for the water, B for the box and
// c = clamp(p, B):
//
//   * W lies inside B, so |p - W| >= |p - B| = outside.
//   * phi(c) is the distance from c to W, and |p - c| = outside, so by the
//     triangle inequality |p - W| >= phi(c) - outside.
//
// The larger of the two is still a lower bound, and it is the one that behaves:
// far away the first term dominates and the ray takes big steps; at the wall
// outside vanishes and the second term becomes phi at the wall itself, which is
// positive wherever the water is not actually touching there. Where it IS
// touching, phi(c) <= 0 and the bound collapses to outside — a hit right at the
// wall, which is exactly what water pressed against the side of a tank looks
// like.
//
// The sampler already extends the grid outward by clamping its indices, so
// phi(c) is what a sample at p returns; no separate clamp is needed here.
float fluidDistance(vec3 p) {
    if (gFluid.control.y == 0) {
        return 1.0e9;
    }

    const int   res = gFluid.control.x;
    const float h   = gFluid.origin.w;
    const vec3  lo  = gFluid.origin.xyz;
    const vec3  hi  = lo + vec3(float(res) * h);

    const vec3  q       = max(lo - p, p - hi);
    const float outside = length(max(q, 0.0));

    const float band    = max(gFluid.render.x, 1.0) * h;
    const float sampled = min(fluidSamplePhi(p, lo, h, res) - gFluid.render.y, band);

    if (outside <= 0.0) {
        return sampled;
    }
    return max(sampled - outside, outside);
}

// The trust band, in world units — the value fluidDistance() saturates at.
//
// A saturated distance is safe to STEP by (it under-reports, never over-reports)
// but it is not safe to shade by: a penumbra estimator reading a capped value
// far from any water concludes there is a surface just out of frame and dims
// the light. So the shadow marchers ask for this and drop the water term once
// the reading is at the cap. Returns a value nothing can reach when the fluid is
// off, so the test costs the disabled path nothing.
float fluidBand() {
    if (gFluid.control.y == 0) {
        return 1.0e9;
    }
    return max(gFluid.render.x, 1.0) * gFluid.origin.w;
}

// Gradient of the sampled level set. The sampling offset is half a cell so that
// the difference spans one trilinear cell rather than falling inside one, where
// the gradient of a trilinear patch is piecewise constant and the normal would
// band visibly.
vec3 fluidNormal(vec3 p) {
    const float e = 0.5 * gFluid.origin.w;
    return normalize(vec3(fluidDistance(p + vec3(e, 0.0, 0.0)) -
                              fluidDistance(p - vec3(e, 0.0, 0.0)),
                          fluidDistance(p + vec3(0.0, e, 0.0)) -
                              fluidDistance(p - vec3(0.0, e, 0.0)),
                          fluidDistance(p + vec3(0.0, 0.0, e)) -
                              fluidDistance(p - vec3(0.0, 0.0, e))));
}

// The water's surface material, assembled from the parameter block.
// material.w is the transmission the marchers already understand, so water
// reaches the existing glass path (refraction + Beer-Lambert tint) without a
// second shading model.
vec4 fluidMaterial() {
    return vec4(gFluid.material.x, gFluid.material.y, 0.0, gFluid.water.w);
}

vec3 fluidAlbedo() {
    return gFluid.water.rgb;
}

// The scene and the water, folded into one field. Both are Lipschitz-1 distance
// bounds and a min of two such bounds is another one, so everything the marcher
// assumes about sceneSdf still holds here.
Hit worldSdf(vec3 p, int primitiveCount) {
    Hit result = sceneSdf(p, primitiveCount);

    const float water = fluidDistance(p);
    if (water < result.distance) {
        result.distance = water;
        result.albedo   = fluidAlbedo();
        result.material = fluidMaterial();
    }
    return result;
}

float worldDistance(vec3 p, int primitiveCount) {
    return min(sceneDistance(p, primitiveCount), fluidDistance(p));
}

vec3 worldNormal(vec3 p, int primitiveCount) {
    const float e = 0.0005;
    return normalize(vec3(
        worldDistance(p + vec3(e, 0.0, 0.0), primitiveCount) -
            worldDistance(p - vec3(e, 0.0, 0.0), primitiveCount),
        worldDistance(p + vec3(0.0, e, 0.0), primitiveCount) -
            worldDistance(p - vec3(0.0, e, 0.0), primitiveCount),
        worldDistance(p + vec3(0.0, 0.0, e), primitiveCount) -
            worldDistance(p - vec3(0.0, 0.0, e), primitiveCount)));
}

#endif // FITZEL_FLUID_FIELD_GLSL
