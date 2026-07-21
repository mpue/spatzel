// The bridge between the backend-agnostic interface and this backend. Linking
// this translation unit is what makes rhi::Backend::Vulkan available; nothing
// above the seam includes anything from src/rhi/vulkan/.

#include "rhi/vulkan/vk_device.hpp"

#include <stdexcept>

namespace rhi {

std::unique_ptr<Device> createDevice(Backend backend, const DeviceCreateInfo& info) {
    switch (backend) {
        case Backend::Vulkan:
            return std::make_unique<vulkan::VulkanDevice>(info);
    }
    throw std::runtime_error("rhi: requested backend is not linked into this binary");
}

} // namespace rhi
