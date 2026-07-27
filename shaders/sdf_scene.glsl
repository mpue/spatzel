#ifndef FITZEL_SDF_SCENE_GLSL
#define FITZEL_SDF_SCENE_GLSL

// The scene evaluation, shared by every pass that needs it.
//
// The reference raymarcher and the brick bake must not merely agree — they
// must be the same code, or "the brick path reproduces the reference" degrades
// into "two implementations of the same idea happen to agree today". So this
// file holds the edit list, the primitive distance functions, the operators and
// the fold over the list, and nothing else includes a second copy.
//
// It also carries the storage buffer declaration, because that is the one part
// that genuinely differs between targets. Every consumer binds the edit list at
// slot 1.
//
// The primitive count is a parameter rather than a global: each pass carries it
// in its own push constant block, and the shape of that block is the pass's
// business.

// Mirrors engine::GpuPrimitive. Every member is a 16-byte slot so std430 and
// std140 agree and the C++ struct needs no padding arithmetic.
struct Primitive {
    vec4  position; // xyz, w = smooth-union blend radius
    vec4  rotation; // unit quaternion
    vec4  params;   // per type
    vec4  albedo;   // rgb
    ivec4 control;  // x = type, y = operator
};

const int kTypeSphere    = 0;
const int kTypeBox       = 1;
const int kTypeTorus     = 2;
const int kTypePlane     = 3;
const int kTypeRoundCone = 4;

const int kOpUnion       = 0;
const int kOpSmoothUnion = 1;
const int kOpSubtract    = 2;
const int kOpIntersect   = 3;

#ifdef TARGET_VULKAN
layout(set = 0, binding = 1, std430) readonly buffer Scene {
    Primitive primitives[];
} scene;
#else
layout(binding = 1, std430) readonly buffer Scene {
    Primitive primitives[];
} scene;
#endif

// --- primitives -------------------------------------------------------------

// Rotate p by the inverse of a unit quaternion, i.e. take the world-space
// sample point into the primitive's local frame.
vec3 rotateInverse(vec3 p, vec4 q) {
    const vec3 conjugate = -q.xyz;
    const vec3 t = 2.0 * cross(conjugate, p);
    return p + q.w * t + cross(conjugate, t);
}

float sdSphere(vec3 p, float radius) {
    return length(p) - radius;
}

float sdRoundBox(vec3 p, vec3 halfExtents, float rounding) {
    const vec3 q = abs(p) - halfExtents + rounding;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - rounding;
}

float sdTorus(vec3 p, float major, float minor) {
    const vec2 q = vec2(length(p.xz) - major, p.y);
    return length(q) - minor;
}

float sdPlane(vec3 p, vec3 normal, float offset) {
    return dot(p, normal) + offset;
}

// Tapered capsule (IQ's round cone), base at the local origin and tip at
// (0, h, 0): radius r0 at the base, r1 at the tip. An exact distance function
// and 1-Lipschitz, so it is safe for both sphere tracing and the brick
// occupancy test. This is the branch/trunk primitive the L-system emits.
float sdRoundCone(vec3 p, float r0, float r1, float h) {
    const vec2 q = vec2(length(p.xz), p.y);

    const float b = (r0 - r1) / h;
    const float a = sqrt(max(1.0 - b * b, 0.0));
    const float k = dot(q, vec2(-b, a));

    if (k < 0.0)     return length(q) - r0;
    if (k > a * h)   return length(q - vec2(0.0, h)) - r1;
    return dot(q, vec2(a, b)) - r0;
}

float evaluatePrimitive(vec3 worldPoint, Primitive prim) {
    // A plane is defined in world space; everything else is defined in its own
    // frame and reached by undoing the transform. There is no scale term,
    // because a non-uniform scale would break the distance metric that sphere
    // tracing relies on.
    if (prim.control.x == kTypePlane) {
        return sdPlane(worldPoint, normalize(prim.params.xyz), prim.params.w);
    }

    const vec3 local = rotateInverse(worldPoint - prim.position.xyz, prim.rotation);

    if (prim.control.x == kTypeSphere) {
        return sdSphere(local, prim.params.x);
    }
    if (prim.control.x == kTypeBox) {
        return sdRoundBox(local, prim.params.xyz, prim.params.w);
    }
    if (prim.control.x == kTypeTorus) {
        return sdTorus(local, prim.params.x, prim.params.y);
    }
    if (prim.control.x == kTypeRoundCone) {
        // params: x = height, y = base radius, z = tip radius.
        return sdRoundCone(local, prim.params.y, prim.params.z, prim.params.x);
    }
    return 1e9;
}

// --- operators --------------------------------------------------------------

// Polynomial smooth minimum. Returns the blended distance in .x and the mix
// factor in .y so the albedo can follow the same blend — which is what makes
// the transition zone legible rather than only visible in silhouette.
vec2 smoothUnion(float a, float b, float k) {
    if (k <= 0.0) {
        return vec2(min(a, b), a < b ? 0.0 : 1.0);
    }
    const float h = clamp(0.5 + 0.5 * (a - b) / k, 0.0, 1.0);
    return vec2(mix(a, b, h) - k * h * (1.0 - h), h);
}

// --- scene ------------------------------------------------------------------

struct Hit {
    float distance;
    vec3  albedo;
};

// Folds the edit list into a single field, left to right: each primitive
// combines with everything accumulated before it. The list is the CSG order —
// a Subtract carves out what precedes it — which is why there is no separate
// tree here; the flat list is the tree.
//
// min, max and the polynomial smin are all Lipschitz-continuous with constant
// 1, and negating an argument preserves that, so every operator below keeps the
// result a conservative distance bound everywhere — which is exactly what both
// sphere tracing and the bake's occupancy test rely on. The CSG operators
// (max-based) can under-estimate distance near a seam, which only ever makes
// tracing take a smaller, still-safe step.
Hit sceneSdf(vec3 p, int primitiveCount) {
    Hit result;
    result.distance = 1e9;
    result.albedo   = vec3(0.8);

    for (int i = 0; i < primitiveCount; ++i) {
        const Primitive prim = scene.primitives[i];
        const float     d    = evaluatePrimitive(p, prim);
        const int       op   = prim.control.y;

        if (op == kOpSmoothUnion) {
            const vec2 blended = smoothUnion(result.distance, d, prim.position.w);
            result.albedo   = mix(result.albedo, prim.albedo.rgb, blended.y);
            result.distance = blended.x;
        } else if (op == kOpSubtract) {
            // Carve this primitive out of the accumulated shape. The newly
            // exposed walls keep the accumulated material — the cutter paints
            // nothing.
            result.distance = max(result.distance, -d);
        } else if (op == kOpIntersect) {
            // Keep only what is also inside this primitive. The binding surface
            // is the farther of the two, so that one's material shows.
            if (d > result.distance) {
                result.albedo = prim.albedo.rgb;
            }
            result.distance = max(result.distance, d);
        } else { // kOpUnion
            if (d < result.distance) {
                result.albedo = prim.albedo.rgb;
            }
            result.distance = min(result.distance, d);
        }
    }
    return result;
}

float sceneDistance(vec3 p, int primitiveCount) {
    return sceneSdf(p, primitiveCount).distance;
}

// Central differences. Six evaluations of the whole list per shaded pixel,
// which is exactly the cost the reference renderer is supposed to pay.
vec3 sceneNormal(vec3 p, int primitiveCount) {
    const float h = 0.0005;
    return normalize(vec3(
        sceneDistance(p + vec3(h, 0.0, 0.0), primitiveCount) -
            sceneDistance(p - vec3(h, 0.0, 0.0), primitiveCount),
        sceneDistance(p + vec3(0.0, h, 0.0), primitiveCount) -
            sceneDistance(p - vec3(0.0, h, 0.0), primitiveCount),
        sceneDistance(p + vec3(0.0, 0.0, h), primitiveCount) -
            sceneDistance(p - vec3(0.0, 0.0, h), primitiveCount)));
}

#endif // FITZEL_SDF_SCENE_GLSL
