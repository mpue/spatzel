#pragma once

// The Vulkan backend's entry points, as seen by the factory.
//
// This header is deliberately free of Vulkan symbols: it is the only file
// under src/rhi/vulkan/ that anything outside the backend includes.

#include "rhi/rhi.hpp"

#include <memory>

namespace rhi::vulkan {

[[nodiscard]] WindowRequirements     windowRequirements(bool enableDebug);
[[nodiscard]] std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info);

} // namespace rhi::vulkan
