#pragma once

// Backend-internal: handle -> Vulkan object mapping.

#include "rhi/handle_pool.hpp"
#include "rhi/vulkan/vk_common.hpp"

#include <cstdint>

namespace rhi::vulkan {

using rhi::detail::Pool;

// ---------------------------------------------------------------------------
// Resource payloads
// ---------------------------------------------------------------------------
struct Texture {
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkExtent3D    extent     = {};
    VkFormat      format     = VK_FORMAT_UNDEFINED;

    // Tracked resource state, updated by the command list as it inserts
    // barriers. Never visible above the seam.
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access = VK_ACCESS_2_NONE;
};

struct Buffer {
    VkBuffer          buffer     = VK_NULL_HANDLE;
    VmaAllocation     allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info       = {};
    VkDeviceSize      size       = 0;
    bool              hostVisible = false;
};

struct Shader {
    VkShaderModule module = VK_NULL_HANDLE;
};

struct Pipeline {
    VkPipeline            pipeline         = VK_NULL_HANDLE;
    VkPipelineLayout      layout           = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout        = VK_NULL_HANDLE;
    uint32_t              pushConstantSize = 0;
    bool                  hasBindings      = false;
};

} // namespace rhi::vulkan
