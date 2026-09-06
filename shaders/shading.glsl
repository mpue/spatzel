#ifndef FITZEL_SHADING_GLSL
#define FITZEL_SHADING_GLSL

// The shading model, shared so the reference and brick renderers cannot drift
// apart in how they light a hit. A metallic-roughness PBR model (Cook-Torrance
// GGX) lit by a directional key light and a shadow-casting point light, over a
// hemispherical ambient stand-in for diffuse IBL.
//
// shadeHit returns the *linear HDR* lit colour and nothing else — no background
// fade, no tonemap. That is deliberate: the reference marcher composites a
// glossy reflection on top before the scene is faded into the background and the
// whole thing is tonemapped, so those two steps are the tail of each marcher's
// main(), not part of shadeHit. Keeping the point light's occlusion out of here
// (passed in as `pointShadow`) is what lets the two renderers share this while
// each marches its own shadow ray.

const float kPi = 3.14159265359;

// All lighting is editor-driven now: the values below arrive in a small storage
// buffer at slot 5 rather than being baked into the shader. Both marchers bind
// it (the bake passes do not include this file, so they need no such binding).
// vec4-packed so std430 alignment is unambiguous; the scalar riders (.w) carry
// the light intensities and the ambient strength.
struct LightingParams {
    vec4 keyDir;        // xyz = direction toward the key light, w = intensity
    vec4 keyColour;     // rgb tint, w unused
    vec4 pointPos;      // xyz = world position, w = intensity
    vec4 pointColour;   // rgb tint, w unused
    vec4 ambientSky;    // rgb upper hemisphere, w = ambient strength
    vec4 ambientGround; // rgb lower hemisphere, w unused
    vec4 bgHorizon;     // rgb background looking at the horizon
    vec4 bgZenith;      // rgb background looking straight up
};

#ifdef TARGET_VULKAN
layout(set = 0, binding = 5, std430) readonly buffer Lighting { LightingParams gLight; };
#else
layout(binding = 5, std430) readonly buffer Lighting { LightingParams gLight; };
#endif

vec3 background(vec3 rayDirection) {
    const float t = 0.5 * (rayDirection.y + 1.0);
    return mix(gLight.bgHorizon.rgb, gLight.bgZenith.rgb, clamp(t, 0.0, 1.0));
}

// --- Cook-Torrance terms ----------------------------------------------------

float distributionGGX(vec3 n, vec3 h, float roughness) {
    const float a  = roughness * roughness;
    const float a2 = a * a;
    const float nh = max(dot(n, h), 0.0);
    const float d  = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-7);
}

float geometrySchlickGGX(float nv, float roughness) {
    // Direct-lighting remap of roughness to k.
    const float r = roughness + 1.0;
    const float k = (r * r) / 8.0;
    return nv / (nv * (1.0 - k) + k);
}

float geometrySmith(vec3 n, vec3 v, vec3 l, float roughness) {
    return geometrySchlickGGX(max(dot(n, v), 0.0), roughness) *
           geometrySchlickGGX(max(dot(n, l), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (vec3(1.0) - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// Dielectrics reflect a flat 4%; metals tint the reflection with their albedo.
vec3 materialF0(vec3 albedo, float metallic) {
    return mix(vec3(0.04), albedo, metallic);
}

// --- glossy reflection sampling (shared by both marchers) -------------------

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// Jitter the mirror direction `r` inside a cone whose width grows with
// roughness, so a rough surface blurs its reflection. Sampled in the tangent
// plane of `r`.
vec3 jitterDirection(vec3 r, float roughness, vec2 xi) {
    const vec3  up = abs(r.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    const vec3  t  = normalize(cross(up, r));
    const vec3  b  = cross(r, t);
    const float ang = 6.2831853 * xi.x;
    const float rad = roughness * roughness * xi.y; // roughness^2 spread
    return normalize(r + (cos(ang) * t + sin(ang) * b) * rad);
}

// One analytic light's Cook-Torrance contribution.
vec3 pbrDirect(vec3 n, vec3 v, vec3 l, vec3 radiance, vec3 albedo, vec3 f0,
               float roughness, float metallic) {
    const float nl = max(dot(n, l), 0.0);
    if (nl <= 0.0) {
        return vec3(0.0);
    }
    const vec3  h = normalize(v + l);
    const float d = distributionGGX(n, h, roughness);
    const float g = geometrySmith(n, v, l, roughness);
    const vec3  f = fresnelSchlick(max(dot(h, v), 0.0), f0);

    const vec3 specular = (d * g * f) / max(4.0 * max(dot(n, v), 0.0) * nl, 1e-4);
    // Energy left for diffuse after reflection, and metals have no diffuse.
    const vec3 kd = (vec3(1.0) - f) * (1.0 - metallic);
    return (kd * albedo / kPi + specular) * radiance * nl;
}

// Direct + ambient-diffuse + emission, in linear HDR. `rayDirection` is the
// incident (eye→hit) direction; `material` is (roughness, metallic, emissive, _).
// The environment/scene specular (the reflection) is added by the caller.
vec3 shadeHit(vec3 albedo, vec3 normal, vec3 position, vec3 rayDirection,
              vec4 material, float pointShadow) {
    const float roughness = clamp(material.x, 0.04, 1.0);
    const float metallic  = clamp(material.y, 0.0, 1.0);
    const float emissive  = material.z;

    const vec3 v  = -rayDirection;
    const vec3 f0 = materialF0(albedo, metallic);

    // Directional key light. Intensity rides in keyDir.w, so a zeroed intensity
    // turns the key light off cleanly.
    const vec3 keyRadiance = gLight.keyColour.rgb * gLight.keyDir.w;
    vec3 lit = pbrDirect(normal, v, normalize(gLight.keyDir.xyz), keyRadiance, albedo, f0,
                         roughness, metallic);

    // Point light: inverse-square falloff, gated by the marched shadow factor.
    const vec3  toLight = gLight.pointPos.xyz - position;
    const float distSq  = max(dot(toLight, toLight), 1e-4);
    const vec3  lDir     = toLight * inversesqrt(distSq);
    const vec3  radiance = gLight.pointColour.rgb * (gLight.pointPos.w / distSq) * pointShadow;
    lit += pbrDirect(normal, v, lDir, radiance, albedo, f0, roughness, metallic);

    // Hemispherical ambient as a cheap diffuse-IBL term; metals take no diffuse.
    // ambientSky.w is a single strength multiplier over both hemisphere tints.
    const vec3 ambient = mix(gLight.ambientGround.rgb, gLight.ambientSky.rgb,
                             0.5 + 0.5 * normal.y) *
                         gLight.ambientSky.w * albedo * (1.0 - metallic);
    lit += ambient;

    // Emission scales the albedo, so a glowing material keeps its own colour.
    lit += albedo * emissive;
    return lit;
}

// --- glass (refraction) -----------------------------------------------------
//
// Glass is a secondary effect, so both renderers share one implementation that
// marches the exact scene SDF (the brick marcher pays a little edit-list cost on
// glass pixels only, which are rare). It is a two-interface model — refract in,
// cross the object, refract out — with a Fresnel-weighted environment reflection
// and Beer-Lambert absorption tinting the transmitted light by the albedo. No
// recursive internal bounces: what lies beyond the glass is shaded once, without
// its own shadows or reflections, which is plenty convincing and bounds the cost.

const float kGlassIor = 1.5; // typical crown glass

// One scene bounce: march the edit list, shade the first hit (direct + ambient,
// no shadow ray, no further reflection) or return the sky. This is what the eye
// sees looking along `rd` — through the glass, or in its mirror reflection.
vec3 sceneSampleOnce(vec3 ro, vec3 rd, int primitiveCount) {
    float t = 0.0;
    for (int i = 0; i < 96; ++i) {
        const vec3  p   = ro + rd * t;
        const Hit   h   = sceneSdf(p, primitiveCount);
        const float eps = 0.0006 * max(t, 1.0);
        if (h.distance < eps) {
            const vec3 n = sceneNormal(p, primitiveCount);
            return shadeHit(h.albedo, n, p, rd, h.material, 1.0);
        }
        t += h.distance;
        if (t > 60.0) {
            break;
        }
    }
    return background(rd);
}

// The glass appearance at a front hit: `pos`/`normal` the surface, `rd` the eye
// ray, `tint` the glass colour (its albedo). Returns linear HDR.
vec3 traceGlass(vec3 pos, vec3 rd, vec3 normal, vec3 tint, int primitiveCount) {
    const float f0   = 0.04; // ((ior-1)/(ior+1))^2 for ior 1.5
    const float fres = f0 + (1.0 - f0) * pow(1.0 - max(dot(-rd, normal), 0.0), 5.0);

    // Environment reflection off the front face.
    const vec3 reflected = sceneSampleOnce(pos + normal * 0.02, reflect(rd, normal),
                                           primitiveCount);

    // Refract into the glass. (refract returns 0 on total internal reflection,
    // which cannot happen entering a denser medium, but guard anyway.)
    const vec3 rin = refract(rd, normal, 1.0 / kGlassIor);
    vec3       refracted;
    if (dot(rin, rin) < 1e-6) {
        refracted = reflected;
    } else {
        // March inside the object (scene SDF is negative there) until the far
        // surface, accumulating the path length for absorption.
        vec3  p      = pos + rin * 0.02;
        float inside = 0.0;
        for (int i = 0; i < 48; ++i) {
            const float d = sceneSdf(p, primitiveCount).distance;
            if (d > -0.002) {
                break; // reached the exit surface
            }
            const float s = max(-d, 0.01);
            p += rin * s;
            inside += s;
        }
        // Refract back out to air. The exit normal points out of the glass, so
        // it is flipped to sit on the incident (glass) side for refract().
        const vec3 exitN = sceneNormal(p, primitiveCount);
        vec3       rout  = refract(rin, -exitN, kGlassIor);
        if (dot(rout, rout) < 1e-6) {
            rout = reflect(rin, -exitN); // total internal reflection at the exit
        }
        refracted = sceneSampleOnce(p + rout * 0.02, rout, primitiveCount);

        // Beer-Lambert: a longer path through coloured glass absorbs more of the
        // complementary light. Clear (white) glass leaves the colour untouched.
        const vec3 absorb = exp(-(vec3(1.0) - tint) * inside * 1.5);
        refracted *= absorb;
    }

    return mix(refracted, reflected, fres);
}

// --- output transform -------------------------------------------------------

// ACES filmic approximation (Narkowicz), then the sRGB OETF. The presented image
// is UNORM and unconverted (see ARCHITECTURE leak #7), so the encode has to
// happen here for the window to show a correct, tone-mapped result.
vec3 tonemap(vec3 hdr, float exposure) {
    vec3 x = hdr * exposure;
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    x = clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
    return mix(1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, x * 12.92,
               vec3(lessThan(x, vec3(0.0031308))));
}

// Fade a shaded surface into the background near the march limit, so the
// unbounded ground plane does not end in a hard line. Applied after reflection
// compositing, before tonemapping.
vec3 fadeToBackground(vec3 colour, vec3 rayDirection, float travelled) {
    return mix(colour, background(rayDirection), clamp((travelled - 25.0) / 45.0, 0.0, 1.0));
}

#endif // FITZEL_SHADING_GLSL
