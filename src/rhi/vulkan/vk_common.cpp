// This translation unit also hosts the VulkanMemoryAllocator implementation.
// VMA resolves its entry points through the pointers volk loaded, which is why
// VMA_STATIC_VULKAN_FUNCTIONS is off (see the target's compile definitions).
#define VMA_IMPLEMENTATION

// VMA's implementation lives in this translation unit, so it inherits our
// warning level. It is third-party code and must not be able to fail the build.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include "rhi/vulkan/vk_common.hpp"

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <stdexcept>
#include <string>

namespace rhi::vulkan {
namespace {

const char* resultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        default:                                return "VkResult";
    }
}

} // namespace

void checkResult(VkResult result, const char* expression) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(expression) + " failed: " + resultName(result) +
                                 " (" + std::to_string(static_cast<int>(result)) + ")");
    }
}

VkFormat toVkFormat(Format format) {
    switch (format) {
        case Format::RGBA8Unorm:  return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8Unorm:  return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::RGBA16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::Undefined:   break;
    }
    return VK_FORMAT_UNDEFINED;
}

VkBufferUsageFlags toVkBufferUsage(BufferUsage usage) {
    VkBufferUsageFlags flags = 0;
    if (any(usage & BufferUsage::Storage))     flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (any(usage & BufferUsage::Uniform))     flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (any(usage & BufferUsage::CopySrc))     flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (any(usage & BufferUsage::CopyDst))     flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return flags;
}

VkImageUsageFlags toVkImageUsage(TextureUsage usage) {
    VkImageUsageFlags flags = 0;
    if (any(usage & TextureUsage::Storage))     flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (any(usage & TextureUsage::Sampled))     flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (any(usage & TextureUsage::CopySrc))     flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (any(usage & TextureUsage::CopyDst))     flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return flags;
}

VkDescriptorType toVkDescriptorType(BindingType type) {
    switch (type) {
        case BindingType::StorageTexture: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        case BindingType::StorageBuffer:  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        case BindingType::UniformBuffer:  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
}

void setDebugName(VkDevice device, uint64_t handle, VkObjectType type, const char* name) {
    if (name == nullptr || vkSetDebugUtilsObjectNameEXT == nullptr) {
        return;
    }
    const VkDebugUtilsObjectNameInfoEXT info{
        .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
        .pNext        = nullptr,
        .objectType   = type,
        .objectHandle = handle,
        .pObjectName  = name,
    };
    vkSetDebugUtilsObjectNameEXT(device, &info);
}

} // namespace rhi::vulkan
