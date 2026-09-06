#pragma once

// Keyframe animation of object transforms. A keyframe is a whole pose (position,
// rotation, dimensions) sampled at a time; a track is the pose keys for one
// object, identified by a stable id carried in GpuPrimitive.control[2] (which the
// shaders ignore). The clip is sampled each frame during playback/scrubbing and
// its interpolated poses are written back into the edit list.

#include "engine/math.hpp"
#include "engine/scene.hpp"

#include <cstdint>
#include <vector>

namespace engine {

// One captured pose at a point in time. `dims` is the primitive's params[4]
// (type-specific sizes); position is xyz only (position[3] is the smooth-union
// blend, left un-animated), rotation is the unit quaternion.
struct PoseKey {
    float time = 0.0f;
    Vec3  position{};
    Quat  rotation{};
    float dims[4] = {};
};

// The pose keys for one object, kept sorted by time.
struct ObjectTrack {
    std::uint32_t        objectId = 0;
    std::vector<PoseKey> keys;
};

struct AnimationClip {
    std::vector<ObjectTrack> tracks;
    float                    duration = 5.0f;
    bool                     linear   = false; // false = smooth (Catmull-Rom/nlerp)
};

// Playback state and the object-id allocator (kept next to the clip so the
// Timeline panel can mint ids when it adds the first keyframe for an object).
struct AnimationState {
    float         time             = 0.0f;
    bool          playing          = false;
    bool          loop             = true;
    float         speed            = 1.0f;
    bool          previewReference = true; // render with the reference path while playing
    std::uint32_t nextId           = 1;
};

// A reserved track id for the camera (it is not a scene primitive, so its track
// simply lives among the object tracks under this sentinel id). Far above any
// primitive id, so it can never collide with one.
inline constexpr std::uint32_t kCameraTrackId = 0xCA33CA33u;

// The camera pose sampled from its track: position, orientation (as a quaternion
// the caller turns into a forward direction), and vertical fov in radians.
struct CameraSample {
    Vec3  position{};
    Quat  orientation{};
    float fov = 0.0f;
};

// --- object identity (GpuPrimitive.control[2]) ------------------------------
[[nodiscard]] std::uint32_t objectIdOf(const GpuPrimitive& p);
void                        setObjectId(GpuPrimitive& p, std::uint32_t id);
// Advance `nextId` past every id currently in the scene, after a wholesale
// replacement (load, L-system regen) so freshly minted ids never collide.
void reseedObjectIds(const std::vector<GpuPrimitive>& scene, AnimationState& state);

// --- track access -----------------------------------------------------------
[[nodiscard]] ObjectTrack*       findTrack(AnimationClip& clip, std::uint32_t objectId);
[[nodiscard]] const ObjectTrack* findTrack(const AnimationClip& clip, std::uint32_t objectId);
// Insert or replace the pose key at `key.time` for `objectId`, keeping the track
// sorted (creating the track if needed).
void setPoseKey(AnimationClip& clip, std::uint32_t objectId, const PoseKey& key);
// Remove the key nearest `time` (within a small epsilon) from the object's track.
void removePoseKeyNear(AnimationClip& clip, std::uint32_t objectId, float time);

// --- sampling ---------------------------------------------------------------
// Write the interpolated pose at `time` into every primitive whose id has a
// track (tracks without a matching primitive are skipped, not an error). The
// camera track is left to sampleCamera. Returns true if any primitive was written.
bool sampleInto(const AnimationClip& clip, float time, std::vector<GpuPrimitive>& scene);

// True if the clip has a non-empty camera track.
[[nodiscard]] bool hasCameraTrack(const AnimationClip& clip);
// Sample the camera track at `time` into `out`. Returns false if there is none.
bool sampleCamera(const AnimationClip& clip, float time, CameraSample& out);

} // namespace engine
