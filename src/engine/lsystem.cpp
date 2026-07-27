#include "engine/lsystem.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace engine {
namespace {

constexpr float  kPi          = 3.14159265358979323846f;
constexpr size_t kMaxExpanded = 200000; // guard against exponential blow-up
constexpr int    kMaxIters    = 12;
constexpr float  kMinRadius   = 0.004f;

// Local turtle frame convention: heading is local +Y (a segment grows along it),
// left is local +X, up is local +Z. The orientation quaternion maps this local
// frame into world space, so it is exactly the round cone's rotation.
GpuPrimitive makeSegment(Vec3 base, Quat orient, float height, float r0, float r1, Vec3 colour) {
    GpuPrimitive p{};
    p.position[0] = base.x; p.position[1] = base.y; p.position[2] = base.z;
    p.rotation[0] = orient.x; p.rotation[1] = orient.y; p.rotation[2] = orient.z;
    p.rotation[3] = orient.w;
    p.params[0]   = height; // see scene.hpp: RoundCone params = height, base r, tip r
    p.params[1]   = r0;
    p.params[2]   = r1;
    p.albedo[0]   = colour.x; p.albedo[1] = colour.y; p.albedo[2] = colour.z;
    p.control[0]  = static_cast<int32_t>(PrimitiveType::RoundCone);
    p.control[1]  = static_cast<int32_t>(Operator::Union);
    return p;
}

GpuPrimitive makeLeaf(Vec3 pos, float radius, Vec3 colour) {
    GpuPrimitive p{};
    p.position[0] = pos.x; p.position[1] = pos.y; p.position[2] = pos.z;
    p.rotation[3] = 1.0f;
    p.params[0]   = radius;
    p.albedo[0]   = colour.x; p.albedo[1] = colour.y; p.albedo[2] = colour.z;
    p.control[0]  = static_cast<int32_t>(PrimitiveType::Sphere);
    p.control[1]  = static_cast<int32_t>(Operator::Union);
    return p;
}

struct Turtle {
    Vec3  position;
    Quat  orientation;
    float length = 0.0f;
};

} // namespace

std::string lsystemExpand(const LSystemConfig& config) {
    std::string current = config.axiom;
    const int   iters   = std::clamp(config.iterations, 0, kMaxIters);

    for (int it = 0; it < iters; ++it) {
        std::string next;
        next.reserve(current.size() * 2);
        for (const char c : current) {
            const std::string* replacement = nullptr;
            for (const LRule& r : config.rules) {
                if (r.predecessor == c) {
                    replacement = &r.successor;
                    break;
                }
            }
            if (replacement) {
                next += *replacement;
            } else {
                next += c;
            }
            if (next.size() > kMaxExpanded) {
                return next; // truncate rather than blow up
            }
        }
        current = std::move(next);
    }
    return current;
}

int lsystemPrimitiveCount(const std::string& expanded, const LSystemConfig& config) {
    // Mirrors the draw/leaf decisions in lsystemBuild so the editor's estimate is
    // exact: one primitive per drawn segment, plus a leaf at every twig tip (a
    // branch that ends with a segment and no further sub-branch) and every
    // explicit 'L'.
    int  count    = 0;
    bool justDrew = false;
    for (const char c : expanded) {
        switch (c) {
            case 'F':
            case 'G': ++count; justDrew = true; break;
            case 'f': justDrew = false; break;
            case '[': justDrew = false; break;
            case ']':
                if (config.leaves && justDrew) { ++count; }
                justDrew = false;
                break;
            case 'L':
                if (config.leaves) { ++count; }
                break;
            default: break; // turns keep the current tip state
        }
    }
    if (config.leaves && justDrew) { ++count; }
    return count;
}

std::vector<GpuPrimitive> lsystemBuild(const std::string& expanded, const LSystemConfig& config) {
    std::vector<GpuPrimitive> out;

    const float angle = config.angleDeg * kPi / 180.0f;

    std::mt19937                          rng(config.seed);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    const auto jitter = [&](float amount) -> float {
        return config.jitter > 0.0f ? unit(rng) * amount * config.jitter : 0.0f;
    };

    // Compose a turn about a local axis: right-multiplying rotates in the
    // turtle's own frame, which is what turtle turns mean.
    const auto turn = [](Quat q, Vec3 localAxis, float radians) {
        return quatMul(q, quatFromAxisAngle(localAxis, radians));
    };

    Turtle t;
    t.position    = config.basePosition;
    t.orientation = Quat{};             // identity → heading points +Y (up)
    t.length      = config.segmentLength;

    std::vector<Turtle> stack;

    const auto leaf = [&](Vec3 at) {
        out.push_back(makeLeaf(at, std::max(kMinRadius, config.leafSize), config.leafColour));
    };

    // A twig tip is a segment that ends a branch with no sub-branch after it.
    // Tracking that here means the Leaves toggle works for any grammar, without
    // the grammar having to spell out where leaves go.
    bool justDrew = false;

    for (const char c : expanded) {
        switch (c) {
            case 'F':
            case 'G': {
                // Optional droop: bend the heading toward gravity before drawing.
                if (config.tropism != 0.0f) {
                    const Vec3  heading = quatRotate(t.orientation, Vec3{0.0f, 1.0f, 0.0f});
                    const Vec3  axis    = cross(heading, Vec3{0.0f, -1.0f, 0.0f});
                    const float s       = length(axis);
                    if (s > 1e-5f) {
                        t.orientation = quatMul(quatFromAxisAngle(axis, config.tropism * s),
                                                t.orientation);
                    }
                }

                const Vec3  heading = quatRotate(t.orientation, Vec3{0.0f, 1.0f, 0.0f});
                const float len     = std::max(0.001f, t.length * (1.0f + jitter(1.0f)));

                // Radius is set by branch depth (the '[' nesting level), not by how
                // many segments have been drawn: the trunk stays a trunk, and each
                // level of branching steps thinner. Radius is uniform within a
                // level so consecutive capsules meet seamlessly — the thinning is
                // a step at each fork, where it reads as a branch, not a bead on a
                // straight run.
                const int   depth  = static_cast<int>(stack.size());
                const float radius = std::max(kMinRadius,
                                              config.baseRadius * std::pow(config.radiusTaper,
                                                                           static_cast<float>(depth)));

                out.push_back(makeSegment(t.position, t.orientation, len, radius, radius,
                                          config.branchColour));
                t.position += heading * len;
                justDrew = true;
                break;
            }
            case 'f': {
                const Vec3 heading = quatRotate(t.orientation, Vec3{0.0f, 1.0f, 0.0f});
                t.position += heading * t.length;
                justDrew = false;
                break;
            }
            case '+': t.orientation = turn(t.orientation, {0.0f, 0.0f, 1.0f},  angle + jitter(angle)); break;
            case '-': t.orientation = turn(t.orientation, {0.0f, 0.0f, 1.0f}, -angle + jitter(angle)); break;
            case '&': t.orientation = turn(t.orientation, {1.0f, 0.0f, 0.0f},  angle + jitter(angle)); break;
            case '^': t.orientation = turn(t.orientation, {1.0f, 0.0f, 0.0f}, -angle + jitter(angle)); break;
            case '\\': t.orientation = turn(t.orientation, {0.0f, 1.0f, 0.0f},  angle); break;
            case '/':  t.orientation = turn(t.orientation, {0.0f, 1.0f, 0.0f}, -angle); break;
            case '|':  t.orientation = turn(t.orientation, {0.0f, 0.0f, 1.0f}, kPi);    break;
            case '[':
                stack.push_back(t);
                t.length *= config.lengthTaper; // sub-branches are shorter
                justDrew = false;               // thinner is handled by depth in 'F'
                break;
            case ']':
                if (config.leaves && justDrew) {
                    leaf(t.position); // this branch ended in a twig — cap it with a leaf
                }
                if (!stack.empty()) {
                    t = stack.back();
                    stack.pop_back();
                }
                justDrew = false;
                break;
            case 'L':
                if (config.leaves) {
                    leaf(t.position); // explicit leaf marker
                }
                break;
            default:
                break; // X, A, B, … : rewrite drivers with no geometry
        }
    }
    // A tip at the very end of the string (the trunk's own top) gets a leaf too.
    if (config.leaves && justDrew) {
        leaf(t.position);
    }
    return out;
}

} // namespace engine
