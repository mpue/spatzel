#pragma once

// Backend-internal: swapchain ownership, recreation and per-image presentation
// semaphores.

#include "rhi/vulkan/vk_common.hpp"

#include <VkBootstrap.h>

#include <vector>

namespace rhi::vulkan {

class Swapchain {
public:
    Swapchain(vkb::Device& device, VkExtent2D extent);
    ~Swapchain();

    Swapchain(const Swapchain&)            = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Tears down and rebuilds against the new extent, reusing the old
    // swapchain as the creation hint. The caller must have ensured the device
    // is idle.
    void recreate(VkExtent2D extent);

    [[nodiscard]] VkSwapchainKHR handle() const { return m_swapchain.swapchain; }
    [[nodiscard]] VkExtent2D     extent() const { return m_swapchain.extent; }
    [[nodiscard]] VkFormat       format() const { return m_swapchain.image_format; }
    [[nodiscard]] uint32_t       imageCount() const {
        return static_cast<uint32_t>(m_images.size());
    }

    [[nodiscard]] VkImage     image(uint32_t index) const { return m_images[index]; }
    // Colour-attachment view of image `index`, for the ImGui overlay's dynamic
    // rendering pass. The blit path addresses the image directly and does not
    // use these.
    [[nodiscard]] VkImageView imageView(uint32_t index) const { return m_views[index]; }
    [[nodiscard]] VkSemaphore renderFinished(uint32_t index) const {
        return m_renderFinished[index];
    }

    // Layout tracking for the image currently being rendered into. Reset to
    // UNDEFINED at the start of every frame — the previous contents are never
    // read back.
    [[nodiscard]] VkImageLayout& layout(uint32_t index) { return m_layouts[index]; }

private:
    void build(VkExtent2D extent, VkSwapchainKHR oldSwapchain);
    void destroyImageResources();

    vkb::Device&             m_device;
    vkb::Swapchain           m_swapchain{};
    std::vector<VkImage>     m_images;
    std::vector<VkImageView> m_views;
    std::vector<VkSemaphore> m_renderFinished;
    std::vector<VkImageLayout> m_layouts;
};

} // namespace rhi::vulkan
