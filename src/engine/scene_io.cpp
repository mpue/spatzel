#include "engine/scene_io.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace engine {
namespace {

using nlohmann::json;

// Enum names in the file are words, not integers: the whole point of the JSON
// form is that a person can read and hand-edit it. The mapping is the file
// format's contract, so it is spelled out here rather than derived from the
// enum's numeric value.
constexpr std::array<std::string_view, 6> kTypeNames{"Sphere",    "Box",     "Torus",
                                                     "Plane",     "RoundCone", "Cylinder"};
constexpr std::array<std::string_view, 4> kOperatorNames{"Union", "SmoothUnion", "Subtract",
                                                         "Intersect"};

std::string typeName(int32_t type) {
    if (type < 0 || type >= static_cast<int32_t>(kTypeNames.size())) {
        throw std::runtime_error("scene: unknown primitive type " + std::to_string(type));
    }
    return std::string(kTypeNames[static_cast<size_t>(type)]);
}

std::string operatorName(int32_t op) {
    if (op < 0 || op >= static_cast<int32_t>(kOperatorNames.size())) {
        throw std::runtime_error("scene: unknown operator " + std::to_string(op));
    }
    return std::string(kOperatorNames[static_cast<size_t>(op)]);
}

int32_t typeFromName(const std::string& name) {
    for (size_t i = 0; i < kTypeNames.size(); ++i) {
        if (name == kTypeNames[i]) {
            return static_cast<int32_t>(i);
        }
    }
    throw std::runtime_error("scene: unknown primitive type '" + name + "'");
}

int32_t operatorFromName(const std::string& name) {
    for (size_t i = 0; i < kOperatorNames.size(); ++i) {
        if (name == kOperatorNames[i]) {
            return static_cast<int32_t>(i);
        }
    }
    throw std::runtime_error("scene: unknown operator '" + name + "'");
}

// Fixed-width float arrays keep the reader strict: a primitive with the wrong
// arity is a malformed file, not a silently truncated one.
template <size_t N>
std::array<float, N> readFloats(const json& node, const char* key) {
    const json& array = node.at(key);
    if (!array.is_array() || array.size() != N) {
        throw std::runtime_error(std::string("scene: '") + key + "' must have " +
                                 std::to_string(N) + " numbers");
    }
    std::array<float, N> out{};
    for (size_t i = 0; i < N; ++i) {
        out[i] = array[i].get<float>();
    }
    return out;
}

} // namespace

void saveScene(const std::filesystem::path& path, const std::vector<GpuPrimitive>& scene,
               const AnimationClip* anim, const fluid::Settings* fluidSettings) {
    json primitives = json::array();
    for (const GpuPrimitive& p : scene) {
        primitives.push_back(json{
            {"type", typeName(p.control[0])},
            {"operator", operatorName(p.control[1])},
            {"id", objectIdOf(p)}, // stable animation object id (0 = none)
            {"position", {p.position[0], p.position[1], p.position[2]}},
            {"rotation", {p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]}},
            {"blend", p.position[3]},
            {"params", {p.params[0], p.params[1], p.params[2], p.params[3]}},
            {"albedo", {p.albedo[0], p.albedo[1], p.albedo[2]}},
            {"roughness", p.material[0]},
            {"metallic", p.material[1]},
            {"emissive", p.material[2]},
            {"transmission", p.material[3]},
        });
    }
    json document{{"version", 4}, {"primitives", std::move(primitives)}};

    // Keyframe tracks, keyed by object id. Only non-empty tracks are written.
    if (anim != nullptr) {
        json tracks = json::array();
        for (const ObjectTrack& tr : anim->tracks) {
            if (tr.keys.empty()) {
                continue;
            }
            json keys = json::array();
            for (const PoseKey& k : tr.keys) {
                keys.push_back(json{
                    {"time", k.time},
                    {"position", {k.position.x, k.position.y, k.position.z}},
                    {"rotation", {k.rotation.x, k.rotation.y, k.rotation.z, k.rotation.w}},
                    {"dims", {k.dims[0], k.dims[1], k.dims[2], k.dims[3]}},
                });
            }
            tracks.push_back(json{{"objectId", tr.objectId}, {"keys", std::move(keys)}});
        }
        if (!tracks.empty()) {
            document["animation"] = json{{"duration", anim->duration},
                                         {"linear", anim->linear},
                                         {"tracks", std::move(tracks)}};
        }
    }

    // The water. Written whole rather than as a diff against the defaults: a
    // scene file is meant to be readable and hand-editable, and a partial block
    // would leave the reader guessing which knobs are in play.
    if (fluidSettings != nullptr && fluidSettings->enabled) {
        const fluid::Settings& f = *fluidSettings;
        document["fluid"] = json{
            {"enabled", f.enabled},
            {"resolution", f.res},
            {"origin", {f.origin.x, f.origin.y, f.origin.z}},
            {"size", f.size},
            {"timestep", f.timestep},
            {"maxSubsteps", f.maxSubsteps},
            {"gravity", f.gravity},
            {"pressureSweeps", f.pressureSweeps},
            {"extrapolateSweeps", f.extrapolateSweeps},
            {"reinitIterations", f.reinitIterations},
            {"seedMin", {f.seedMin.x, f.seedMin.y, f.seedMin.z}},
            {"seedMax", {f.seedMax.x, f.seedMax.y, f.seedMax.z}},
            {"poolLevel", f.poolLevel},
            {"colour", {f.colour[0], f.colour[1], f.colour[2]}},
            {"transmission", f.transmission},
            {"roughness", f.roughness},
            {"ior", f.ior},
            {"trustBandCells", f.trustBandCells},
            {"surfaceOffset", f.surfaceOffset},
        };
    }

    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        throw std::runtime_error("scene: cannot write " + path.string());
    }
    file << document.dump(2) << '\n';
    if (!file) {
        throw std::runtime_error("scene: short write on " + path.string());
    }
}

std::vector<GpuPrimitive> loadScene(const std::filesystem::path& path, AnimationClip* anim,
                                    fluid::Settings* fluidSettings) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("scene: cannot read " + path.string());
    }

    json document;
    try {
        file >> document;
    } catch (const json::exception& e) {
        throw std::runtime_error("scene: not valid JSON in " + path.string() + ": " + e.what());
    }

    const auto& primitives = document.at("primitives");
    if (!primitives.is_array()) {
        throw std::runtime_error("scene: 'primitives' must be an array in " + path.string());
    }

    std::vector<GpuPrimitive> scene;
    scene.reserve(primitives.size());
    for (const json& node : primitives) {
        GpuPrimitive p{};
        const auto position = readFloats<3>(node, "position");
        const auto rotation = readFloats<4>(node, "rotation");
        const auto params   = readFloats<4>(node, "params");
        const auto albedo   = readFloats<3>(node, "albedo");

        p.position[0] = position[0];
        p.position[1] = position[1];
        p.position[2] = position[2];
        p.position[3] = node.at("blend").get<float>();
        p.rotation[0] = rotation[0];
        p.rotation[1] = rotation[1];
        p.rotation[2] = rotation[2];
        p.rotation[3] = rotation[3];
        p.params[0]   = params[0];
        p.params[1]   = params[1];
        p.params[2]   = params[2];
        p.params[3]   = params[3];
        p.albedo[0]   = albedo[0];
        p.albedo[1]   = albedo[1];
        p.albedo[2]   = albedo[2];
        // Material fields arrived in schema version 2; scenes that predate them
        // (examples, generated plants) keep the struct's matte defaults.
        p.material[0] = node.value("roughness", p.material[0]);
        p.material[1] = node.value("metallic", p.material[1]);
        p.material[2] = node.value("emissive", p.material[2]);
        p.material[3] = node.value("transmission", p.material[3]); // optional; 0 = opaque
        p.control[0]  = typeFromName(node.at("type").get<std::string>());
        p.control[1]  = operatorFromName(node.at("operator").get<std::string>());
        p.control[2]  = static_cast<int32_t>(node.value("id", 0u)); // animation id (v3+)
        scene.push_back(p);
    }

    // Animation clip (schema v3+). Absent -> the caller keeps an empty clip.
    if (anim != nullptr) {
        anim->tracks.clear();
        if (document.contains("animation")) {
            const auto& a = document.at("animation");
            anim->duration = a.value("duration", anim->duration);
            anim->linear   = a.value("linear", anim->linear);
            if (a.contains("tracks")) {
                for (const json& tnode : a.at("tracks")) {
                    ObjectTrack tr;
                    tr.objectId = tnode.at("objectId").get<std::uint32_t>();
                    for (const json& knode : tnode.at("keys")) {
                        PoseKey    k;
                        const auto pos = readFloats<3>(knode, "position");
                        const auto rot = readFloats<4>(knode, "rotation");
                        const auto dim = readFloats<4>(knode, "dims");
                        k.time     = knode.at("time").get<float>();
                        k.position = {pos[0], pos[1], pos[2]};
                        k.rotation = {rot[0], rot[1], rot[2], rot[3]};
                        for (int c = 0; c < 4; ++c) {
                            k.dims[c] = dim[c];
                        }
                        tr.keys.push_back(k);
                    }
                    anim->tracks.push_back(std::move(tr));
                }
            }
        }
    }
    // The water (schema v4+). Absent -> the caller keeps whatever it had, so an
    // older file loaded into a running editor leaves the tank alone rather than
    // silently resetting it to the defaults.
    if (fluidSettings != nullptr && document.contains("fluid")) {
        const auto&     f = document.at("fluid");
        fluid::Settings s = *fluidSettings;

        s.enabled           = f.value("enabled", s.enabled);
        s.res               = f.value("resolution", s.res);
        s.size              = f.value("size", s.size);
        s.timestep          = f.value("timestep", s.timestep);
        s.maxSubsteps       = f.value("maxSubsteps", s.maxSubsteps);
        s.gravity           = f.value("gravity", s.gravity);
        s.pressureSweeps    = f.value("pressureSweeps", s.pressureSweeps);
        s.extrapolateSweeps = f.value("extrapolateSweeps", s.extrapolateSweeps);
        s.reinitIterations  = f.value("reinitIterations", s.reinitIterations);
        s.poolLevel         = f.value("poolLevel", s.poolLevel);
        s.transmission      = f.value("transmission", s.transmission);
        s.roughness         = f.value("roughness", s.roughness);
        s.ior               = f.value("ior", s.ior);
        s.trustBandCells    = f.value("trustBandCells", s.trustBandCells);
        s.surfaceOffset     = f.value("surfaceOffset", s.surfaceOffset);

        if (f.contains("origin")) {
            const auto v = readFloats<3>(f, "origin");
            s.origin      = {v[0], v[1], v[2]};
        }
        if (f.contains("seedMin")) {
            const auto v = readFloats<3>(f, "seedMin");
            s.seedMin     = {v[0], v[1], v[2]};
        }
        if (f.contains("seedMax")) {
            const auto v = readFloats<3>(f, "seedMax");
            s.seedMax     = {v[0], v[1], v[2]};
        }
        if (f.contains("colour")) {
            const auto v = readFloats<3>(f, "colour");
            for (int c = 0; c < 3; ++c) {
                s.colour[c] = v[c];
            }
        }

        // Clamped on the way in: a hand-edited file is exactly where an
        // out-of-range resolution or a zero timestep would come from, and the
        // solver would divide by it.
        s.res         = std::clamp(s.res, fluid::kMinRes, fluid::kMaxRes);
        s.size        = std::max(s.size, 0.01f);
        s.timestep    = std::clamp(s.timestep, 1.0e-5f, 1.0f);
        s.maxSubsteps = std::clamp(s.maxSubsteps, 1, 64);

        *fluidSettings = s;
    }

    return scene;
}

} // namespace engine
