#pragma once

// Scene serialisation. The edit list is the single source of truth, so this is
// just that list written to and read from a human-readable JSON file — the
// point being that an interesting arrangement can be captured and restored
// exactly. There is no second representation: load returns the same
// std::vector<GpuPrimitive> the renderer already consumes.

#include "engine/scene.hpp"

#include <filesystem>
#include <vector>

namespace engine {

// Writes the edit list to `path` as JSON. Throws std::runtime_error on failure.
void saveScene(const std::filesystem::path& path, const std::vector<GpuPrimitive>& scene);

// Reads a scene JSON written by saveScene. Throws std::runtime_error if the
// file is missing or malformed.
[[nodiscard]] std::vector<GpuPrimitive> loadScene(const std::filesystem::path& path);

} // namespace engine
