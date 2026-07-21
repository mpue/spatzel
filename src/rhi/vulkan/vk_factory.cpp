// Backend-side half of the factory. rhi::createDevice lives in
// src/rhi/factory.cpp and dispatches here.

#include "rhi/vulkan/vk_backend.hpp"
#include "rhi/vulkan/vk_device.hpp"

namespace rhi::vulkan {

WindowRequirements windowRequirements(bool) {
    // Vulkan owns its presentation surface, so the window must be created
    // without any client API attached to it.
    return WindowRequirements{.api = ClientApi::None};
}

std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info) {
    return std::make_unique<VulkanDevice>(info);
}

} // namespace rhi::vulkan
