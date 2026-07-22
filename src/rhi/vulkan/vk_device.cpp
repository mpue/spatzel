#include "rhi/vulkan/vk_device.hpp"

// volk.h (via vk_common.hpp) must be included first: GLFW only declares
// glfwCreateWindowSurface once the Vulkan types are visible.
#include <GLFW/glfw3.h>

// ImGui's Vulkan render backend. IMGUI_IMPL_VULKAN_USE_VOLK (set in CMake) makes
// it resolve entry points through volk, the same loader the rest of this backend
// uses. This is the only place ImGui's Vulkan symbols appear — below the seam.
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace rhi::vulkan {
namespace {

VkBool32 VKAPI_PTR debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                 VkDebugUtilsMessageTypeFlagsEXT,
                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                 void*) {
    const char* label = "info";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        label = "error";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        label = "warning";
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) != 0) {
        return VK_FALSE;
    }
    std::fprintf(stderr, "[vulkan %s] %s\n", label,
                 data->pMessage != nullptr ? data->pMessage : "<no message>");
    return VK_FALSE;
}

std::vector<uint32_t> loadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open SPIR-V module: " + path.string());
    }

    const std::streamsize byteCount = file.tellg();
    if (byteCount <= 0 || byteCount % sizeof(uint32_t) != 0) {
        throw std::runtime_error("not a SPIR-V module (bad size): " + path.string());
    }

    std::vector<uint32_t> words(static_cast<size_t>(byteCount) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), byteCount);
    if (!file) {
        throw std::runtime_error("short read on SPIR-V module: " + path.string());
    }
    return words;
}

// Half-precision floats only appear here, in the readback path: the storage
// image is RGBA16F and the seam speaks plain floats.
float halfToFloat(uint16_t bits) {
    const uint32_t sign     = static_cast<uint32_t>(bits >> 15) << 31;
    const uint32_t exponent = (bits >> 10) & 0x1Fu;
    const uint32_t mantissa = bits & 0x3FFu;

    uint32_t result = 0;
    if (exponent == 0) {
        if (mantissa != 0) {
            // Subnormal: renormalise into a regular single-precision value.
            uint32_t shifted  = mantissa;
            uint32_t exponent32 = 127 - 15 + 1;
            while ((shifted & 0x400u) == 0) {
                shifted <<= 1;
                --exponent32;
            }
            shifted &= 0x3FFu;
            result = (exponent32 << 23) | (shifted << 13);
        }
    } else if (exponent == 0x1Fu) {
        result = (0xFFu << 23) | (mantissa << 13);
    } else {
        result = ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    result |= sign;

    float value = 0.0f;
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

void initialiseLoaderOnce() {
    static const bool initialised = [] {
        checkResult(volkInitialize(), "volkInitialize");
        return true;
    }();
    (void) initialised;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
VulkanDevice::VulkanDevice(const DeviceCreateInfo& info) : m_commandList(*this) {
    m_windowExtent = {info.framebufferSize.width, info.framebufferSize.height};
    // The layout below shaderRoot is this backend's business alone.
    m_shaderDirectory = std::filesystem::path(info.shaderRoot) / "vulkan";

    initialiseLoaderOnce();
    createInstance(info);
    createSurface(info.nativeWindowHandle);
    selectDeviceAndQueue();
    createAllocator();
    m_swapchain = std::make_unique<Swapchain>(m_device, m_windowExtent);
    createFrames();
}

void VulkanDevice::createInstance(const DeviceCreateInfo& info) {
    uint32_t           extensionCount = 0;
    const char* const* extensions     = glfwGetRequiredInstanceExtensions(&extensionCount);
    if (extensions == nullptr) {
        throw std::runtime_error("glfwGetRequiredInstanceExtensions failed — no Vulkan loader?");
    }

    vkb::InstanceBuilder builder(vkGetInstanceProcAddr);
    builder.set_app_name(info.applicationName)
        .set_engine_name("fitzel")
        .require_api_version(1, 3, 0)
        .enable_extensions(extensionCount, extensions);

    if (info.enableDebug) {
        builder.request_validation_layers(true).set_debug_callback(&debugCallback);
    }

    auto result = builder.build();
    if (!result) {
        throw std::runtime_error("Vulkan instance creation failed: " + result.error().message());
    }
    m_instance = result.value();
    volkLoadInstanceOnly(m_instance.instance);
}

void VulkanDevice::createSurface(void* nativeWindowHandle) {
    if (nativeWindowHandle == nullptr) {
        throw std::runtime_error("rhi: DeviceCreateInfo::nativeWindowHandle is null");
    }
    auto* window = static_cast<GLFWwindow*>(nativeWindowHandle);
    FITZEL_CHECK(glfwCreateWindowSurface(m_instance.instance, window, nullptr, &m_surface));
}

void VulkanDevice::selectDeviceAndQueue() {
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.synchronization2 = VK_TRUE;
    // The ImGui overlay draws with dynamic rendering (no render pass object), so
    // the feature has to be enabled on the device. The compute + blit render
    // path itself uses neither, but the seam now offers the overlay.
    features13.dynamicRendering  = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(m_instance);
    auto selected = selector.set_surface(m_surface)
                        .set_minimum_version(1, 3)
                        .set_required_features_13(features13)
                        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
                        .select();
    if (!selected) {
        throw std::runtime_error("no suitable Vulkan device: " + selected.error().message());
    }
    m_physicalDevice = selected.value();

    auto device = vkb::DeviceBuilder(m_physicalDevice).build();
    if (!device) {
        throw std::runtime_error("Vulkan device creation failed: " + device.error().message());
    }
    m_device = device.value();
    volkLoadDevice(m_device.device);

    auto queue = m_device.get_queue(vkb::QueueType::graphics);
    auto index = m_device.get_queue_index(vkb::QueueType::graphics);
    if (!queue || !index) {
        throw std::runtime_error("no graphics queue available");
    }
    m_queue       = queue.value();
    m_queueFamily = index.value();

    std::fprintf(stderr, "[rhi] Vulkan backend on %s\n", m_physicalDevice.name.c_str());
}

void VulkanDevice::createAllocator() {
    // volk owns the entry points, so VMA is handed the two it needs to resolve
    // everything else itself.
    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr   = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo info{};
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance         = m_instance.instance;
    info.physicalDevice   = m_physicalDevice.physical_device;
    info.device           = m_device.device;
    info.pVulkanFunctions = &functions;

    FITZEL_CHECK(vmaCreateAllocator(&info, &m_allocator));
}

void VulkanDevice::createFrames() {
    const VkCommandPoolCreateInfo poolInfo{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = m_queueFamily,
    };
    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    const VkSemaphoreCreateInfo semaphoreInfo{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = nullptr, .flags = 0};

    // Sized for the handful of descriptors a milestone-1 compute pass needs;
    // the pool is reset wholesale at the start of every frame.
    const VkDescriptorPoolSize poolSizes[]{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16},
    };
    const VkDescriptorPoolCreateInfo descriptorPoolInfo{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext         = nullptr,
        .flags         = 0,
        .maxSets       = 16,
        .poolSizeCount = static_cast<uint32_t>(std::size(poolSizes)),
        .pPoolSizes    = poolSizes,
    };

    for (Frame& frame : m_frames) {
        FITZEL_CHECK(vkCreateCommandPool(m_device.device, &poolInfo, nullptr, &frame.commandPool));

        const VkCommandBufferAllocateInfo allocInfo{
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext              = nullptr,
            .commandPool        = frame.commandPool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        FITZEL_CHECK(vkAllocateCommandBuffers(m_device.device, &allocInfo, &frame.commandBuffer));
        FITZEL_CHECK(vkCreateFence(m_device.device, &fenceInfo, nullptr, &frame.inFlight));
        FITZEL_CHECK(
            vkCreateSemaphore(m_device.device, &semaphoreInfo, nullptr, &frame.imageAvailable));
        FITZEL_CHECK(vkCreateDescriptorPool(m_device.device, &descriptorPoolInfo, nullptr,
                                            &frame.descriptorPool));
    }
}

// ---------------------------------------------------------------------------
// Teardown
// ---------------------------------------------------------------------------
VulkanDevice::~VulkanDevice() {
    if (m_device.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device.device);
    }

    shutdownUi();

    // Anything the application forgot to destroy is cleaned up here so the
    // process exits without leaking device memory. Deferred deletions first:
    // the GPU is idle, so nothing is still in flight.
    for (PendingDeletion& pending : m_deletionQueue) {
        pending.deleter();
    }
    m_deletionQueue.clear();

    m_pipelines.forEachAlive([&](Pipeline& pipeline) {
        vkDestroyPipeline(m_device.device, pipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(m_device.device, pipeline.layout, nullptr);
        if (pipeline.setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device.device, pipeline.setLayout, nullptr);
        }
    });
    m_shaders.forEachAlive([&](Shader& shader) {
        vkDestroyShaderModule(m_device.device, shader.module, nullptr);
    });
    m_textures.forEachAlive([&](Texture& texture) {
        vkDestroyImageView(m_device.device, texture.view, nullptr);
        vmaDestroyImage(m_allocator, texture.image, texture.allocation);
    });
    m_buffers.forEachAlive([&](Buffer& buffer) {
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    });

    for (Frame& frame : m_frames) {
        if (frame.descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(m_device.device, frame.descriptorPool, nullptr);
        }
        if (frame.imageAvailable != VK_NULL_HANDLE) {
            vkDestroySemaphore(m_device.device, frame.imageAvailable, nullptr);
        }
        if (frame.inFlight != VK_NULL_HANDLE) {
            vkDestroyFence(m_device.device, frame.inFlight, nullptr);
        }
        if (frame.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(m_device.device, frame.commandPool, nullptr);
        }
    }

    m_swapchain.reset();

    if (m_allocator != VK_NULL_HANDLE) {
        vmaDestroyAllocator(m_allocator);
    }
    if (m_device.device != VK_NULL_HANDLE) {
        vkb::destroy_device(m_device);
    }
    if (m_surface != VK_NULL_HANDLE) {
        vkb::destroy_surface(m_instance, m_surface);
    }
    if (m_instance.instance != VK_NULL_HANDLE) {
        vkb::destroy_instance(m_instance);
    }
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------
CommandList& VulkanDevice::beginFrame() {
    if (m_frameActive) {
        throw std::runtime_error("rhi: beginFrame called while a frame is already recording");
    }

    recreateSwapchainIfNeeded();

    Frame& frame = m_frames[m_frameIndex];
    FITZEL_CHECK(vkWaitForFences(m_device.device, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    collectGarbage();

    VkResult acquired = vkAcquireNextImageKHR(m_device.device, m_swapchain->handle(), UINT64_MAX,
                                              frame.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        // The semaphore is untouched when acquisition fails, so it is safe to
        // reuse it for the retry.
        m_swapchainDirty = true;
        recreateSwapchainIfNeeded();
        acquired = vkAcquireNextImageKHR(m_device.device, m_swapchain->handle(), UINT64_MAX,
                                         frame.imageAvailable, VK_NULL_HANDLE, &m_imageIndex);
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        checkResult(acquired, "vkAcquireNextImageKHR");
    }

    FITZEL_CHECK(vkResetFences(m_device.device, 1, &frame.inFlight));
    FITZEL_CHECK(vkResetCommandPool(m_device.device, frame.commandPool, 0));
    FITZEL_CHECK(vkResetDescriptorPool(m_device.device, frame.descriptorPool, 0));

    const VkCommandBufferBeginInfo beginInfo{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    FITZEL_CHECK(vkBeginCommandBuffer(frame.commandBuffer, &beginInfo));

    // Previous contents are never read, so the image starts undefined again.
    m_swapchain->layout(m_imageIndex) = VK_IMAGE_LAYOUT_UNDEFINED;
    m_swapchainStage                  = VK_PIPELINE_STAGE_2_NONE;
    m_swapchainAccess                 = VK_ACCESS_2_NONE;

    m_commandList.reset(frame.commandBuffer, frame.descriptorPool);
    m_frameActive = true;
    return m_commandList;
}

void VulkanDevice::endFrame() {
    if (!m_frameActive) {
        throw std::runtime_error("rhi: endFrame called without a matching beginFrame");
    }
    Frame& frame = m_frames[m_frameIndex];

    // The presentation engine needs PRESENT_SRC; the semaphore signal provides
    // the execution dependency, so no destination stage/access is required.
    transitionSwapchainImage(frame.commandBuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE);
    FITZEL_CHECK(vkEndCommandBuffer(frame.commandBuffer));

    const VkSemaphoreSubmitInfo waitInfo{
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = frame.imageAvailable,
        .value       = 0,
        .stageMask   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    const VkSemaphoreSubmitInfo signalInfo{
        .sType       = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .pNext       = nullptr,
        .semaphore   = m_swapchain->renderFinished(m_imageIndex),
        .value       = 0,
        .stageMask   = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .deviceIndex = 0,
    };
    const VkCommandBufferSubmitInfo commandInfo{
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = nullptr,
        .commandBuffer = frame.commandBuffer,
        .deviceMask    = 0,
    };
    const VkSubmitInfo2 submitInfo{
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext                    = nullptr,
        .flags                    = 0,
        .waitSemaphoreInfoCount   = 1,
        .pWaitSemaphoreInfos      = &waitInfo,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &commandInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signalInfo,
    };
    FITZEL_CHECK(vkQueueSubmit2(m_queue, 1, &submitInfo, frame.inFlight));

    const VkSwapchainKHR swapchain     = m_swapchain->handle();
    const VkSemaphore    waitSemaphore = m_swapchain->renderFinished(m_imageIndex);
    const VkPresentInfoKHR presentInfo{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext              = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = &waitSemaphore,
        .swapchainCount     = 1,
        .pSwapchains        = &swapchain,
        .pImageIndices      = &m_imageIndex,
        .pResults           = nullptr,
    };
    const VkResult presented = vkQueuePresentKHR(m_queue, &presentInfo);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        m_swapchainDirty = true;
    } else {
        checkResult(presented, "vkQueuePresentKHR");
    }

    ++m_frameCounter;
    m_frameIndex  = (m_frameIndex + 1) % kFramesInFlight;
    m_frameActive = false;
}

void VulkanDevice::onResize(uint32_t width, uint32_t height) {
    m_windowExtent   = {width, height};
    m_swapchainDirty = true;
}

Extent2D VulkanDevice::swapchainExtent() const {
    const VkExtent2D extent = m_swapchain->extent();
    return {extent.width, extent.height};
}

void VulkanDevice::recreateSwapchainIfNeeded() {
    if (!m_swapchainDirty || m_windowExtent.width == 0 || m_windowExtent.height == 0) {
        return;
    }
    FITZEL_CHECK(vkDeviceWaitIdle(m_device.device));
    m_swapchain->recreate(m_windowExtent);
    m_swapchainDirty = false;
}

void VulkanDevice::collectGarbage() {
    size_t write = 0;
    for (size_t read = 0; read < m_deletionQueue.size(); ++read) {
        if (m_deletionQueue[read].retireAfter <= m_frameCounter) {
            m_deletionQueue[read].deleter();
        } else {
            // Guard the self-assignment: when no earlier entry has been retired,
            // write == read and `x = std::move(x)` would run. libc++ empties a
            // self-move-assigned std::function (it destroys the target before
            // reading the source), so the deleter would be lost and invoked
            // empty on a later frame — std::bad_function_call. This is the
            // resize crash: the render target's deferred destroy is the usual
            // producer here.
            if (write != read) {
                m_deletionQueue[write] = std::move(m_deletionQueue[read]);
            }
            ++write;
        }
    }
    m_deletionQueue.resize(write);
}

void VulkanDevice::defer(std::function<void()> deleter) {
    m_deletionQueue.push_back({m_frameCounter + kFramesInFlight, std::move(deleter)});
}

// ---------------------------------------------------------------------------
// Barriers — backend-internal by design
// ---------------------------------------------------------------------------
void VulkanDevice::transitionTexture(VkCommandBuffer cmd, Texture& texture, VkImageLayout layout,
                                     VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    // Emitted unconditionally: even when the layout is unchanged, consecutive
    // uses of the same image need a write-after-write dependency.
    const VkImageMemoryBarrier2 barrier{
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = texture.stage,
        .srcAccessMask       = texture.access,
        .dstStageMask        = stage,
        .dstAccessMask       = access,
        .oldLayout           = texture.layout,
        .newLayout           = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = texture.image,
        .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependency{
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                    = nullptr,
        .dependencyFlags          = 0,
        .memoryBarrierCount       = 0,
        .pMemoryBarriers          = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers    = nullptr,
        .imageMemoryBarrierCount  = 1,
        .pImageMemoryBarriers     = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);

    texture.layout = layout;
    texture.stage  = stage;
    texture.access = access;
}

void VulkanDevice::transitionSwapchainImage(VkCommandBuffer cmd, VkImageLayout layout,
                                            VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    VkImageLayout& current = m_swapchain->layout(m_imageIndex);
    const VkImageMemoryBarrier2 barrier{
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .pNext               = nullptr,
        .srcStageMask        = m_swapchainStage,
        .srcAccessMask       = m_swapchainAccess,
        .dstStageMask        = stage,
        .dstAccessMask       = access,
        .oldLayout           = current,
        .newLayout           = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = m_swapchain->image(m_imageIndex),
        .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    const VkDependencyInfo dependency{
        .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .pNext                    = nullptr,
        .dependencyFlags          = 0,
        .memoryBarrierCount       = 0,
        .pMemoryBarriers          = nullptr,
        .bufferMemoryBarrierCount = 0,
        .pBufferMemoryBarriers    = nullptr,
        .imageMemoryBarrierCount  = 1,
        .pImageMemoryBarriers     = &barrier,
    };
    vkCmdPipelineBarrier2(cmd, &dependency);

    current           = layout;
    m_swapchainStage  = stage;
    m_swapchainAccess = access;
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.depth != 1) {
        throw std::runtime_error("rhi: only 2D textures with depth == 1 are supported");
    }

    Texture texture;
    texture.extent = {desc.width, desc.height, 1};
    texture.format = toVkFormat(desc.format);
    if (texture.format == VK_FORMAT_UNDEFINED) {
        throw std::runtime_error("rhi: createTexture with an undefined format");
    }

    const VkImageCreateInfo imageInfo{
        .sType                 = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .imageType             = VK_IMAGE_TYPE_2D,
        .format                = texture.format,
        .extent                = texture.extent,
        .mipLevels             = 1,
        .arrayLayers           = 1,
        .samples               = VK_SAMPLE_COUNT_1_BIT,
        .tiling                = VK_IMAGE_TILING_OPTIMAL,
        .usage                 = toVkImageUsage(desc.usage),
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
        .initialLayout         = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage    = VMA_MEMORY_USAGE_AUTO;
    allocInfo.priority = 1.0f;

    FITZEL_CHECK(vmaCreateImage(m_allocator, &imageInfo, &allocInfo, &texture.image,
                                &texture.allocation, nullptr));

    const VkImageViewCreateInfo viewInfo{
        .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = 0,
        .image            = texture.image,
        .viewType         = VK_IMAGE_VIEW_TYPE_2D,
        .format           = texture.format,
        .components       = {},
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    FITZEL_CHECK(vkCreateImageView(m_device.device, &viewInfo, nullptr, &texture.view));

    setDebugName(m_device.device, reinterpret_cast<uint64_t>(texture.image), VK_OBJECT_TYPE_IMAGE,
                 desc.debugName);
    return m_textures.insert(texture);
}

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        throw std::runtime_error("rhi: createBuffer with size 0");
    }

    Buffer buffer;
    buffer.size        = desc.size;
    buffer.hostVisible = desc.access != MemoryAccess::GpuOnly;

    const VkBufferCreateInfo bufferInfo{
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = desc.size,
        .usage                 = toVkBufferUsage(desc.usage),
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
    };

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    switch (desc.access) {
        case MemoryAccess::GpuOnly:
            break;
        case MemoryAccess::CpuToGpu:
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                              VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryAccess::GpuToCpu:
            allocInfo.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }

    FITZEL_CHECK(vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &buffer.buffer,
                                 &buffer.allocation, &buffer.info));

    setDebugName(m_device.device, reinterpret_cast<uint64_t>(buffer.buffer), VK_OBJECT_TYPE_BUFFER,
                 desc.debugName);
    return m_buffers.insert(buffer);
}

ShaderHandle VulkanDevice::createShader(std::string_view logicalName) {
    // Only compute exists so far, hence the fixed stage suffix. The engine
    // never sees any of this — it asked for "raymarch_probe".
    const std::filesystem::path path =
        m_shaderDirectory / (std::string(logicalName) + ".comp.spv");
    const std::vector<uint32_t> spirv = loadSpirv(path);

    const VkShaderModuleCreateInfo info{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext    = nullptr,
        .flags    = 0,
        .codeSize = spirv.size() * sizeof(uint32_t),
        .pCode    = spirv.data(),
    };

    Shader shader;
    FITZEL_CHECK(vkCreateShaderModule(m_device.device, &info, nullptr, &shader.module));
    return m_shaders.insert(shader);
}

PipelineHandle VulkanDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    const Shader& shader = m_shaders.get(desc.cs);

    Pipeline pipeline;
    pipeline.pushConstantSize = desc.pushConstantSize;
    pipeline.hasBindings      = !desc.bindings.empty();

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(desc.bindings.size());
    for (const BindingDesc& binding : desc.bindings) {
        bindings.push_back(VkDescriptorSetLayoutBinding{
            .binding            = binding.slot,
            .descriptorType     = toVkDescriptorType(binding.type),
            .descriptorCount    = 1,
            .stageFlags         = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        });
    }

    if (!bindings.empty()) {
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext        = nullptr,
            .flags        = 0,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings    = bindings.data(),
        };
        FITZEL_CHECK(vkCreateDescriptorSetLayout(m_device.device, &layoutInfo, nullptr,
                                                 &pipeline.setLayout));
    }

    const VkPushConstantRange pushRange{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = desc.pushConstantSize,
    };
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext                  = nullptr,
        .flags                  = 0,
        .setLayoutCount         = pipeline.setLayout != VK_NULL_HANDLE ? 1u : 0u,
        .pSetLayouts            = pipeline.setLayout != VK_NULL_HANDLE ? &pipeline.setLayout
                                                                      : nullptr,
        .pushConstantRangeCount = desc.pushConstantSize > 0 ? 1u : 0u,
        .pPushConstantRanges    = desc.pushConstantSize > 0 ? &pushRange : nullptr,
    };
    FITZEL_CHECK(
        vkCreatePipelineLayout(m_device.device, &pipelineLayoutInfo, nullptr, &pipeline.layout));

    const VkComputePipelineCreateInfo pipelineInfo{
        .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext  = nullptr,
        .flags  = 0,
        .stage  = VkPipelineShaderStageCreateInfo{
             .sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .pNext               = nullptr,
             .flags               = 0,
             .stage               = VK_SHADER_STAGE_COMPUTE_BIT,
             .module              = shader.module,
             .pName               = "main",
             .pSpecializationInfo = nullptr,
        },
        .layout             = pipeline.layout,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex  = -1,
    };
    FITZEL_CHECK(vkCreateComputePipelines(m_device.device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                          nullptr, &pipeline.pipeline));

    setDebugName(m_device.device, reinterpret_cast<uint64_t>(pipeline.pipeline),
                 VK_OBJECT_TYPE_PIPELINE, desc.debugName);
    return m_pipelines.insert(pipeline);
}

void VulkanDevice::updateBuffer(BufferHandle handle, std::span<const std::byte> data,
                                uint64_t offset) {
    Buffer& buffer = m_buffers.get(handle);
    if (!buffer.hostVisible) {
        throw std::runtime_error("rhi: updateBuffer requires a host-visible buffer");
    }
    if (offset + data.size() > buffer.size) {
        throw std::runtime_error("rhi: updateBuffer would write past the end of the buffer");
    }
    if (buffer.info.pMappedData == nullptr) {
        throw std::runtime_error("rhi: buffer is not persistently mapped");
    }

    auto* destination = static_cast<std::byte*>(buffer.info.pMappedData) + offset;
    std::memcpy(destination, data.data(), data.size());
    FITZEL_CHECK(vmaFlushAllocation(m_allocator, buffer.allocation, offset, data.size()));
}

void VulkanDevice::submitBlocking(const std::function<void(VkCommandBuffer)>& record) {
    const VkCommandPoolCreateInfo poolInfo{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = m_queueFamily,
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    FITZEL_CHECK(vkCreateCommandPool(m_device.device, &poolInfo, nullptr, &pool));

    const VkCommandBufferAllocateInfo allocInfo{
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext              = nullptr,
        .commandPool        = pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    FITZEL_CHECK(vkAllocateCommandBuffers(m_device.device, &allocInfo, &cmd));

    const VkCommandBufferBeginInfo beginInfo{
        .sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext            = nullptr,
        .flags            = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    FITZEL_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
    record(cmd);
    FITZEL_CHECK(vkEndCommandBuffer(cmd));

    const VkCommandBufferSubmitInfo commandInfo{
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = nullptr,
        .commandBuffer = cmd,
        .deviceMask    = 0,
    };
    const VkSubmitInfo2 submitInfo{
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext                    = nullptr,
        .flags                    = 0,
        .waitSemaphoreInfoCount   = 0,
        .pWaitSemaphoreInfos      = nullptr,
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &commandInfo,
        .signalSemaphoreInfoCount = 0,
        .pSignalSemaphoreInfos    = nullptr,
    };

    const VkFenceCreateInfo fenceInfo{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    VkFence fence = VK_NULL_HANDLE;
    FITZEL_CHECK(vkCreateFence(m_device.device, &fenceInfo, nullptr, &fence));

    FITZEL_CHECK(vkQueueSubmit2(m_queue, 1, &submitInfo, fence));
    FITZEL_CHECK(vkWaitForFences(m_device.device, 1, &fence, VK_TRUE, UINT64_MAX));

    vkDestroyFence(m_device.device, fence, nullptr);
    vkDestroyCommandPool(m_device.device, pool, nullptr);
}

void VulkanDevice::readTexture(TextureHandle handle, std::span<float> out) {
    Texture& texture = m_textures.get(handle);
    if (texture.format != VK_FORMAT_R16G16B16A16_SFLOAT) {
        throw std::runtime_error("rhi: readTexture only supports RGBA16Float for now");
    }

    const size_t texelCount = static_cast<size_t>(texture.extent.width) * texture.extent.height;
    if (out.size() < texelCount * 4) {
        throw std::runtime_error("rhi: readTexture destination is too small");
    }

    // Everything the frame path may still be doing with this image has to
    // finish before it can be copied out.
    FITZEL_CHECK(vkDeviceWaitIdle(m_device.device));

    const VkDeviceSize byteCount = texelCount * 4 * sizeof(uint16_t);
    const VkBufferCreateInfo bufferInfo{
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = byteCount,
        .usage                 = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
    };
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer          staging     = VK_NULL_HANDLE;
    VmaAllocation     allocation  = VK_NULL_HANDLE;
    VmaAllocationInfo allocated{};
    FITZEL_CHECK(
        vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &staging, &allocation, &allocated));

    submitBlocking([&](VkCommandBuffer cmd) {
        transitionTexture(cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        const VkBufferImageCopy region{
            .bufferOffset      = 0,
            .bufferRowLength   = 0,
            .bufferImageHeight = 0,
            .imageSubresource  = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset       = {0, 0, 0},
            .imageExtent       = texture.extent,
        };
        vkCmdCopyImageToBuffer(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                               &region);
    });

    FITZEL_CHECK(vmaInvalidateAllocation(m_allocator, allocation, 0, byteCount));
    const auto* halves = static_cast<const uint16_t*>(allocated.pMappedData);
    for (size_t i = 0; i < texelCount * 4; ++i) {
        out[i] = halfToFloat(halves[i]);
    }

    vmaDestroyBuffer(m_allocator, staging, allocation);
}

void VulkanDevice::readBuffer(BufferHandle handle, std::span<std::byte> out, uint64_t offset) {
    const Buffer& buffer = m_buffers.get(handle);
    if (offset + out.size() > buffer.size) {
        throw std::runtime_error("rhi: readBuffer would read past the end of the buffer");
    }
    if (out.empty()) {
        return;
    }

    // Same reasoning as readTexture: whatever the frame path may still be doing
    // with this buffer has to finish before it can be copied out.
    FITZEL_CHECK(vkDeviceWaitIdle(m_device.device));

    const VkDeviceSize       byteCount = out.size();
    const VkBufferCreateInfo bufferInfo{
        .sType                 = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext                 = nullptr,
        .flags                 = 0,
        .size                  = byteCount,
        .usage                 = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode           = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices   = nullptr,
    };
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer          staging    = VK_NULL_HANDLE;
    VmaAllocation     allocation = VK_NULL_HANDLE;
    VmaAllocationInfo allocated{};
    FITZEL_CHECK(
        vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &staging, &allocation, &allocated));

    submitBlocking([&](VkCommandBuffer cmd) {
        const VkBufferCopy region{.srcOffset = offset, .dstOffset = 0, .size = byteCount};
        vkCmdCopyBuffer(cmd, buffer.buffer, staging, 1, &region);
    });

    FITZEL_CHECK(vmaInvalidateAllocation(m_allocator, allocation, 0, byteCount));
    std::memcpy(out.data(), allocated.pMappedData, out.size());

    vmaDestroyBuffer(m_allocator, staging, allocation);
}

void VulkanDevice::destroy(TextureHandle handle) {
    const Texture texture = m_textures.remove(handle);
    defer([this, texture] {
        vkDestroyImageView(m_device.device, texture.view, nullptr);
        vmaDestroyImage(m_allocator, texture.image, texture.allocation);
    });
}

void VulkanDevice::destroy(BufferHandle handle) {
    const Buffer buffer = m_buffers.remove(handle);
    defer([this, buffer] { vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation); });
}

void VulkanDevice::destroy(ShaderHandle handle) {
    const Shader shader = m_shaders.remove(handle);
    defer([this, shader] { vkDestroyShaderModule(m_device.device, shader.module, nullptr); });
}

void VulkanDevice::destroy(PipelineHandle handle) {
    const Pipeline pipeline = m_pipelines.remove(handle);
    defer([this, pipeline] {
        vkDestroyPipeline(m_device.device, pipeline.pipeline, nullptr);
        vkDestroyPipelineLayout(m_device.device, pipeline.layout, nullptr);
        if (pipeline.setLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(m_device.device, pipeline.setLayout, nullptr);
        }
    });
}

// ---------------------------------------------------------------------------
// Debug UI overlay
//
// The ImGui context and every ImGui:: call live in the engine; this backend
// only owns the Vulkan render backend. It draws with dynamic rendering (the
// device already requires Vulkan 1.3), so there is no render pass or
// framebuffer to manage — the overlay attaches directly to the swapchain image
// view in CommandList::endUiFrame.
// ---------------------------------------------------------------------------
bool VulkanDevice::initUi() {
    // ImGui manages its font (and any user texture) descriptors from its own
    // pool, freeing sets as textures come and go — hence FREE_DESCRIPTOR_SET.
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    const VkDescriptorPoolCreateInfo poolInfo{
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext         = nullptr,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = 8,
        .poolSizeCount = 1,
        .pPoolSizes    = &poolSize,
    };
    FITZEL_CHECK(
        vkCreateDescriptorPool(m_device.device, &poolInfo, nullptr, &m_uiDescriptorPool));

    const VkFormat colorFormat = m_swapchain->format();
    VkPipelineRenderingCreateInfo renderingInfo{
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .pNext                   = nullptr,
        .viewMask                = 0,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &colorFormat,
        .depthAttachmentFormat   = VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = VK_FORMAT_UNDEFINED,
    };

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance                    = m_instance.instance;
    initInfo.PhysicalDevice              = m_physicalDevice.physical_device;
    initInfo.Device                      = m_device.device;
    initInfo.QueueFamily                 = m_queueFamily;
    initInfo.Queue                       = m_queue;
    initInfo.DescriptorPool              = m_uiDescriptorPool;
    initInfo.MinImageCount               = m_swapchain->imageCount();
    initInfo.ImageCount                  = m_swapchain->imageCount();
    initInfo.MSAASamples                 = VK_SAMPLE_COUNT_1_BIT;
    initInfo.UseDynamicRendering         = true;
    initInfo.PipelineRenderingCreateInfo = renderingInfo;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        vkDestroyDescriptorPool(m_device.device, m_uiDescriptorPool, nullptr);
        m_uiDescriptorPool = VK_NULL_HANDLE;
        return false;
    }
    m_uiInitialised = true;
    return true;
}

void VulkanDevice::beginUiFrame() {
    if (m_uiInitialised) {
        ImGui_ImplVulkan_NewFrame();
    }
}

void VulkanDevice::shutdownUi() {
    if (!m_uiInitialised) {
        return;
    }
    // ImGui's device objects may still be referenced by frames in flight, so
    // idle first. Safe to call from either the engine's ordered teardown or the
    // destructor; the flag makes the second call a no-op.
    vkDeviceWaitIdle(m_device.device);
    ImGui_ImplVulkan_Shutdown();
    vkDestroyDescriptorPool(m_device.device, m_uiDescriptorPool, nullptr);
    m_uiDescriptorPool = VK_NULL_HANDLE;
    m_uiInitialised    = false;
}

} // namespace rhi::vulkan
