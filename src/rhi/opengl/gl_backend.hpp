#pragma once

// The OpenGL backend's entry points, as seen by the factory.
//
// This header is deliberately free of GL symbols: it is the only file under
// src/rhi/opengl/ that anything outside the backend includes.

#include "rhi/rhi.hpp"

#include <memory>

namespace rhi::opengl {

[[nodiscard]] WindowRequirements      windowRequirements(bool enableDebug);
[[nodiscard]] std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);

} // namespace rhi::opengl
