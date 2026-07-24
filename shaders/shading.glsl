#ifndef FITZEL_SHADING_GLSL
#define FITZEL_SHADING_GLSL

// The shading model, shared so the reference and brick renderers cannot drift
// apart in how they light a hit. A directional key light plus a hemispherical
// ambient term, and — added on the 'spiel' branch — a shadow-casting point
// light layered on top.
//
// The point light's *shading* (position, attenuation, its diffuse term) lives
// here so both renderers light it identically. Its *shadow*, however, is a
// scene-marching query, and marching is the one thing the two renderers do
// differently — so the occlusion factor is computed by each renderer's own
// marcher and passed in as `pointShadow`. Only the reference renderer casts a
// real shadow ray; the brick renderer passes 1.0 and shows the point light
// unshadowed. Keeping the query out of shadeHit is what lets the shading model
// stay a single shared function.

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

// Shade a surface hit and fade it into the background near the march limit, so
// the unbounded ground plane does not end in a hard line. `position` is the
// world-space hit; `pointShadow` is the occlusion of the point light along the
// ray to it (1 = lit, 0 = shadowed), computed by the caller's own marcher.
vec3 shadeHit(vec3 albedo, vec3 normal, vec3 position, vec3 rayDirection,
              float travelled, float pointShadow) {
    const float diffuse = max(dot(normal, kLightDirection), 0.0);
    // Hemispherical ambient: sky above, bounce below. Cheap, and it keeps unlit
    // faces from going flat black without any AO term.
    const vec3 ambient = mix(kAmbientGround, kAmbientSky, 0.5 + 0.5 * normal.y);

    // Point light: Lambert term with inverse-square falloff, gated by the
    // shadow factor the caller marched. The distance is measured to the hit, so
    // the same light shapes near and far surfaces consistently.
    const vec3  toLight     = kPointLightPosition - position;
    const float distSq      = max(dot(toLight, toLight), 1e-4);
    const vec3  lightDir    = toLight * inversesqrt(distSq);
    const float pointDiffuse = max(dot(normal, lightDir), 0.0);
    const vec3  point = kPointLightColour * (kPointLightIntensity / distSq) *
                        pointDiffuse * pointShadow;

    vec3 colour = albedo * (ambient + kLightColour * diffuse + point);
    return mix(colour, background(rayDirection), clamp((travelled - 25.0) / 45.0, 0.0, 1.0));
}

#endif // FITZEL_SHADING_GLSL
