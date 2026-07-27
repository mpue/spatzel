#pragma once

// A small L-system vegetation generator. It rewrites an axiom by a set of
// production rules, then walks the resulting string with a 3D turtle to emit
// ordinary scene primitives — tapered round cones for branches, spheres for
// leaves. The output is a plain std::vector<GpuPrimitive> the editor appends to
// the edit list, so there is no second scene representation: a generated plant
// is just primitives, saved and edited like any other.
//
// Deterministic rewriting plus a seeded angle/length jitter: same config, same
// plant, but a non-zero jitter breaks the mechanical regularity that makes a
// pure L-system read as a fractal rather than as foliage.

#include "engine/math.hpp"
#include "engine/scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// One production: every occurrence of `predecessor` becomes `successor`.
struct LRule {
    char        predecessor = '\0';
    std::string successor;
};

struct LSystemConfig {
    std::string        axiom = "X";
    std::vector<LRule> rules;

    int   iterations    = 4;      // rewriting passes (clamped to a sane ceiling)
    float angleDeg      = 25.0f;  // turn angle for +-&^\/
    float segmentLength = 0.5f;   // world length of one F step
    float lengthTaper   = 0.9f;   // sub-branch length factor per '[' nesting level
    float baseRadius    = 0.08f;  // trunk radius at the root
    float radiusTaper   = 0.82f;  // radius factor carried across each segment
    float leafSize      = 0.12f;  // leaf sphere radius
    bool  leaves        = true;   // emit 'L' leaves
    float tropism       = 0.0f;   // per-segment bend toward gravity (droop)
    std::uint32_t seed  = 1u;     // jitter RNG seed
    float jitter        = 0.0f;   // 0..1 random variation of angle and length

    Vec3 basePosition = {0.0f, 0.0f, 0.0f};   // where the trunk starts
    Vec3 branchColour = {0.45f, 0.30f, 0.16f};
    Vec3 leafColour   = {0.30f, 0.55f, 0.20f};
};

// Rewrite the axiom `iterations` times. Output length is capped, so a rule that
// grows explosively yields a truncated (but valid) string rather than exhausting
// memory.
[[nodiscard]] std::string lsystemExpand(const LSystemConfig& config);

// Walk an expanded string with a 3D turtle and return the primitives it draws.
[[nodiscard]] std::vector<GpuPrimitive> lsystemBuild(const std::string& expanded,
                                                     const LSystemConfig& config);

// How many primitives `expanded` would draw, without building them — for the
// editor's live "≈ N primitives" estimate and its budget guard.
[[nodiscard]] int lsystemPrimitiveCount(const std::string& expanded,
                                        const LSystemConfig& config);

} // namespace engine
