#pragma once

// Backend-internal. This header — and everything else under src/rhi/vulkan/ —
// is the only place in the project allowed to see Vulkan.

#include <volk.h>

#include <vk_mem_alloc.h>

#include "rhi/rhi.hpp"

#include <cstdint>

namespace rhi::vulkan {

// Throws std::runtime_error with the failing expression and result code.
void checkResult(VkResult result, const char* expression);

#define FITZEL_CHECK(expr) ::rhi::vulkan::checkResult((expr), #expr)

[[nodiscard]] VkFormat           toVkFormat(Format format);
[[nodiscard]] VkBufferUsageFlags toVkBufferUsage(BufferUsage usage);
[[nodiscard]] VkImageUsageFlags  toVkImageUsage(TextureUsage usage);
[[nodiscard]] VkDescriptorType   toVkDescriptorType(BindingType type);

// Optional debug-name tagging; a no-op when VK_EXT_debug_utils is unavailable.
void setDebugName(VkDevice device, uint64_t handle, VkObjectType type, const char* name);

// Number of frames the CPU may run ahead of the GPU.
inline constexpr uint32_t kFramesInFlight = 2;

// Descriptor sets one pool can hand out. A dispatch whose bindings changed takes
// one, so this is a batch size, not a budget: a frame that needs more gets
// another pool (see VulkanCommandList::allocateDescriptorSet).
inline constexpr uint32_t kDescriptorPoolSets = 256;

} // namespace rhi::vulkan
