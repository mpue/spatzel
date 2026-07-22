#include "rhi/vulkan/vk_swapchain.hpp"

#include <stdexcept>

namespace rhi::vulkan {

Swapchain::Swapchain(vkb::Device& device, VkExtent2D extent) : m_device(device) {
    build(extent, VK_NULL_HANDLE);
}

Swapchain::~Swapchain() {
    destroyImageResources();
    vkb::destroy_swapchain(m_swapchain);
}

void Swapchain::recreate(VkExtent2D extent) {
    const VkSwapchainKHR old = m_swapchain.swapchain;
    destroyImageResources();

    vkb::Swapchain previous = m_swapchain;
    build(extent, old);
    vkb::destroy_swapchain(previous);
}

void Swapchain::build(VkExtent2D extent, VkSwapchainKHR oldSwapchain) {
    vkb::SwapchainBuilder builder(m_device);
    builder.set_desired_extent(extent.width, extent.height)
        // A UNORM surface, deliberately not the SRGB one vk-bootstrap would
        // pick by default. The seam presents whatever the render target holds,
        // without a colour space conversion; blitting into an sRGB image would
        // silently encode on the way out and make this backend disagree with
        // one that has no such conversion in its present path.
        .set_desired_format(VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                               VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .add_fallback_format(VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM,
                                                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        // FIFO is always supported and keeps the loop from spinning.
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        // The compute result is blitted in, so the image must be a transfer
        // destination as well as a colour attachment.
        .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (oldSwapchain != VK_NULL_HANDLE) {
        builder.set_old_swapchain(oldSwapchain);
    }

    auto result = builder.build();
    if (!result) {
        throw std::runtime_error("swapchain creation failed: " + result.error().message());
    }
    m_swapchain = result.value();

    auto images = m_swapchain.get_images();
    if (!images) {
        throw std::runtime_error("swapchain image query failed: " + images.error().message());
    }
    m_images = images.value();
    m_layouts.assign(m_images.size(), VK_IMAGE_LAYOUT_UNDEFINED);

    // Colour-attachment views, used only by the ImGui overlay's dynamic
    // rendering pass. vkb creates them to match the swapchain format.
    auto views = m_swapchain.get_image_views();
    if (!views) {
        throw std::runtime_error("swapchain image view creation failed: " +
                                 views.error().message());
    }
    m_views = views.value();

    // One presentation semaphore per swapchain image, not per frame in flight:
    // a frame-indexed semaphore can still be pending in the presentation
    // engine when it is reused, which the validation layers reject.
    m_renderFinished.resize(m_images.size());
    const VkSemaphoreCreateInfo semaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    for (VkSemaphore& semaphore : m_renderFinished) {
        FITZEL_CHECK(vkCreateSemaphore(m_device.device, &semaphoreInfo, nullptr, &semaphore));
    }
}

void Swapchain::destroyImageResources() {
    m_swapchain.destroy_image_views(m_views);
    m_views.clear();
    for (VkSemaphore semaphore : m_renderFinished) {
        vkDestroySemaphore(m_device.device, semaphore, nullptr);
    }
    m_renderFinished.clear();
    m_images.clear();
    m_layouts.clear();
}

} // namespace rhi::vulkan
