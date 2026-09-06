#include "engine/animation.hpp"

#include <algorithm>
#include <cmath>

namespace engine {
namespace {

// A pose key at exactly `time` replaces any existing one there (times are keyed
// to a small epsilon so a re-take on the same frame overwrites cleanly).
constexpr float kTimeEps = 1e-4f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

Vec3 lerpVec(Vec3 a, Vec3 b, float s) { return a * (1.0f - s) + b * s; }

// Normalised lerp with the shortest-path sign fix — the same trick the gizmo uses
// (editor_gizmo.cpp). Adequate for keyframe rotation; squad would be the smoother
// upgrade but is deliberately out of scope (see math.hpp's minimalism note).
Quat nlerp(Quat a, Quat b, float s) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const float k = d < 0.0f ? -1.0f : 1.0f;
    Quat        q{a.x * (1.0f - s) + k * b.x * s, a.y * (1.0f - s) + k * b.y * s,
                  a.z * (1.0f - s) + k * b.z * s, a.w * (1.0f - s) + k * b.w * s};
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len > 1e-6f) {
        q.x /= len;
        q.y /= len;
        q.z /= len;
        q.w /= len;
    } else {
        q = Quat{};
    }
    return q;
}

// Non-uniform Catmull-Rom (arbitrary key times) expressed as cubic Hermite. The
// caller passes the endpoint-clamped neighbours (v0=v1 / v3=v2 at the ends), which
// yields one-sided tangents so the curve does not fly past the first/last key.
float catmull(float v0, float v1, float v2, float v3, float t0, float t1, float t2,
              float t3, float t) {
    const float dt = t2 - t1;
    if (dt <= 1e-6f) {
        return v1;
    }
    const float s  = (t - t1) / dt;
    const float m1 = (t2 - t0) > 1e-6f ? (v2 - v0) / (t2 - t0) : 0.0f; // per-unit-time
    const float m2 = (t3 - t1) > 1e-6f ? (v3 - v1) / (t3 - t1) : 0.0f;
    const float s2 = s * s;
    const float s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    return h00 * v1 + h10 * dt * m1 + h01 * v2 + h11 * dt * m2;
}

// Interpolate the pose at `time`. Endpoints are held; times outside the key range
// clamp to the first/last pose.
PoseKey samplePose(const std::vector<PoseKey>& keys, float time, bool linear) {
    if (keys.size() == 1) {
        return keys.front();
    }
    const float t = clampf(time, keys.front().time, keys.back().time);

    // Segment [i, i+1] with keys[i].time <= t.
    size_t i = 0;
    while (i + 2 < keys.size() && keys[i + 1].time <= t) {
        ++i;
    }
    const PoseKey& k1 = keys[i];
    const PoseKey& k2 = keys[i + 1];
    const float    dt = k2.time - k1.time;
    const float    s  = dt > 1e-6f ? (t - k1.time) / dt : 0.0f;

    PoseKey out;
    out.time     = time;
    out.rotation = nlerp(k1.rotation, k2.rotation, s); // rotation is always nlerp

    if (linear || keys.size() == 2) {
        out.position = lerpVec(k1.position, k2.position, s);
        for (int c = 0; c < 4; ++c) {
            out.dims[c] = k1.dims[c] * (1.0f - s) + k2.dims[c] * s;
        }
    } else {
        // Endpoint-clamped neighbours for the tangents.
        const PoseKey& k0 = keys[i > 0 ? i - 1 : i];
        const PoseKey& k3 = keys[i + 2 < keys.size() ? i + 2 : i + 1];
        const float    t0 = k0.time, t1 = k1.time, t2 = k2.time, t3 = k3.time;
        out.position.x = catmull(k0.position.x, k1.position.x, k2.position.x, k3.position.x,
                                 t0, t1, t2, t3, t);
        out.position.y = catmull(k0.position.y, k1.position.y, k2.position.y, k3.position.y,
                                 t0, t1, t2, t3, t);
        out.position.z = catmull(k0.position.z, k1.position.z, k2.position.z, k3.position.z,
                                 t0, t1, t2, t3, t);
        for (int c = 0; c < 4; ++c) {
            out.dims[c] = catmull(k0.dims[c], k1.dims[c], k2.dims[c], k3.dims[c], t0, t1, t2,
                                  t3, t);
        }
    }

    // Dimensions are sizes; a Catmull-Rom overshoot must not drive them negative.
    for (int c = 0; c < 4; ++c) {
        out.dims[c] = std::max(out.dims[c], 0.0f);
    }
    return out;
}

} // namespace

std::uint32_t objectIdOf(const GpuPrimitive& p) {
    return static_cast<std::uint32_t>(p.control[2]);
}

void setObjectId(GpuPrimitive& p, std::uint32_t id) {
    p.control[2] = static_cast<std::int32_t>(id);
}

void reseedObjectIds(const std::vector<GpuPrimitive>& scene, AnimationState& state) {
    std::uint32_t maxId = 0;
    for (const GpuPrimitive& p : scene) {
        maxId = std::max(maxId, objectIdOf(p));
    }
    state.nextId = maxId + 1;
}

ObjectTrack* findTrack(AnimationClip& clip, std::uint32_t objectId) {
    for (ObjectTrack& t : clip.tracks) {
        if (t.objectId == objectId) {
            return &t;
        }
    }
    return nullptr;
}

const ObjectTrack* findTrack(const AnimationClip& clip, std::uint32_t objectId) {
    for (const ObjectTrack& t : clip.tracks) {
        if (t.objectId == objectId) {
            return &t;
        }
    }
    return nullptr;
}

void setPoseKey(AnimationClip& clip, std::uint32_t objectId, const PoseKey& key) {
    ObjectTrack* track = findTrack(clip, objectId);
    if (track == nullptr) {
        clip.tracks.push_back(ObjectTrack{objectId, {}});
        track = &clip.tracks.back();
    }
    for (PoseKey& k : track->keys) {
        if (std::fabs(k.time - key.time) <= kTimeEps) {
            k = key; // re-take at the same time
            return;
        }
    }
    track->keys.push_back(key);
    std::sort(track->keys.begin(), track->keys.end(),
              [](const PoseKey& a, const PoseKey& b) { return a.time < b.time; });
}

void removePoseKeyNear(AnimationClip& clip, std::uint32_t objectId, float time) {
    ObjectTrack* track = findTrack(clip, objectId);
    if (track == nullptr) {
        return;
    }
    auto it = std::min_element(track->keys.begin(), track->keys.end(),
                               [time](const PoseKey& a, const PoseKey& b) {
                                   return std::fabs(a.time - time) < std::fabs(b.time - time);
                               });
    if (it != track->keys.end() && std::fabs(it->time - time) <= 0.05f) {
        track->keys.erase(it);
    }
}

bool hasCameraTrack(const AnimationClip& clip) {
    const ObjectTrack* tr = findTrack(clip, kCameraTrackId);
    return tr != nullptr && !tr->keys.empty();
}

bool sampleCamera(const AnimationClip& clip, float time, CameraSample& out) {
    const ObjectTrack* tr = findTrack(clip, kCameraTrackId);
    if (tr == nullptr || tr->keys.empty()) {
        return false;
    }
    const PoseKey pose = samplePose(tr->keys, time, clip.linear);
    out.position       = pose.position;
    out.orientation    = pose.rotation; // captured from the camera basis
    out.fov            = pose.dims[0];
    return true;
}

bool sampleInto(const AnimationClip& clip, float time, std::vector<GpuPrimitive>& scene) {
    bool wrote = false;
    for (const ObjectTrack& track : clip.tracks) {
        if (track.keys.empty()) {
            continue;
        }
        GpuPrimitive* prim = nullptr;
        for (GpuPrimitive& p : scene) {
            if (objectIdOf(p) == track.objectId) {
                prim = &p;
                break;
            }
        }
        if (prim == nullptr) {
            continue; // orphaned track (object deleted) — skipped, not an error
        }

        const PoseKey pose = samplePose(track.keys, time, clip.linear);
        prim->position[0]  = pose.position.x;
        prim->position[1]  = pose.position.y;
        prim->position[2]  = pose.position.z; // position[3] (blend) is not animated
        prim->rotation[0]  = pose.rotation.x;
        prim->rotation[1]  = pose.rotation.y;
        prim->rotation[2]  = pose.rotation.z;
        prim->rotation[3]  = pose.rotation.w;
        for (int c = 0; c < 4; ++c) {
            prim->params[c] = pose.dims[c];
        }
        wrote = true;
    }
    return wrote;
}

} // namespace engine
