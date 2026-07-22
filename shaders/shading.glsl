#ifndef FITZEL_SHADING_GLSL
#define FITZEL_SHADING_GLSL

// The shading model, shared so the reference and brick renderers cannot drift
// apart in how they light a hit. One directional light plus a hemispherical
// ambient term; no shadows, no ambient occlusion — neither is needed to
// validate the accelerated path, and adding them here would only make the two
// renderers' agreement harder to reason about.

const vec3 kLightDirection = normalize(vec3(0.45, 0.8, 0.35));
const vec3 kLightColour    = vec3(1.0, 0.97, 0.90);
const vec3 kAmbientSky     = vec3(0.28, 0.33, 0.42);
const vec3 kAmbientGround  = vec3(0.14, 0.12, 0.11);

vec3 background(vec3 rayDirection) {
    const float t = 0.5 * (rayDirection.y + 1.0);
    return mix(vec3(0.13, 0.14, 0.17), vec3(0.42, 0.52, 0.68), clamp(t, 0.0, 1.0));
}

// Shade a surface hit and fade it into the background near the march limit, so
// the unbounded ground plane does not end in a hard line.
vec3 shadeHit(vec3 albedo, vec3 normal, vec3 rayDirection, float travelled) {
    const float diffuse = max(dot(normal, kLightDirection), 0.0);
    // Hemispherical ambient: sky above, bounce below. Cheap, and it keeps unlit
    // faces from going flat black without any AO term.
    const vec3 ambient = mix(kAmbientGround, kAmbientSky, 0.5 + 0.5 * normal.y);

    vec3 colour = albedo * (ambient + kLightColour * diffuse);
    return mix(colour, background(rayDirection), clamp((travelled - 25.0) / 45.0, 0.0, 1.0));
}

#endif // FITZEL_SHADING_GLSL
