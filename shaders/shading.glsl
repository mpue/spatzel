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

const vec3 kLightDirection = normalize(vec3(0.45, 0.8, 0.35));
const vec3 kLightColour    = vec3(1.0, 0.97, 0.90);
const vec3 kAmbientSky     = vec3(0.28, 0.33, 0.42);
const vec3 kAmbientGround  = vec3(0.14, 0.12, 0.11);

// Point light: a warm fill above and to one side of the scene, close enough
// that its inverse-square falloff shapes the objects rather than lighting them
// flat. Intensity is folded into the falloff so the colour stays a plain tint.
const vec3  kPointLightPosition  = vec3(2.0, 4.5, -2.5);
const vec3  kPointLightColour    = vec3(1.0, 0.6, 0.3);
const float kPointLightIntensity = 50.0;

vec3 background(vec3 rayDirection) {
    const float t = 0.5 * (rayDirection.y + 1.0);
    return mix(vec3(0.13, 0.14, 0.17), vec3(0.42, 0.52, 0.68), clamp(t, 0.0, 1.0));
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

    // Directional key light.
    vec3 lit = pbrDirect(normal, v, kLightDirection, kLightColour, albedo, f0,
                         roughness, metallic);

    // Point light: inverse-square falloff, gated by the marched shadow factor.
    const vec3  toLight = kPointLightPosition - position;
    const float distSq  = max(dot(toLight, toLight), 1e-4);
    const vec3  lDir     = toLight * inversesqrt(distSq);
    const vec3  radiance = kPointLightColour * (kPointLightIntensity / distSq) * pointShadow;
    lit += pbrDirect(normal, v, lDir, radiance, albedo, f0, roughness, metallic);

    // Hemispherical ambient as a cheap diffuse-IBL term; metals take no diffuse.
    const vec3 ambient = mix(kAmbientGround, kAmbientSky, 0.5 + 0.5 * normal.y) *
                         albedo * (1.0 - metallic);
    lit += ambient;

    // Emission scales the albedo, so a glowing material keeps its own colour.
    lit += albedo * emissive;
    return lit;
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
