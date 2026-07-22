#pragma once

// Backend-internal: the Vulkan implementation of rhi::Device and
// rhi::CommandList.

#include "rhi/vulkan/vk_common.hpp"
#include "rhi/vulkan/vk_resources.hpp"
#include "rhi/vulkan/vk_swapchain.hpp"

#include <VkBootstrap.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace rhi::vulkan {

class VulkanDevice;

// ---------------------------------------------------------------------------
// Command list
//
// Bindings are recorded lazily: bindStorageTexture/bindStorageBuffer only note
// what should be visible to the next dispatch. dispatch() then inserts the
// required barriers, allocates a descriptor set from the frame's pool, writes
// it and binds it. That is what keeps barriers and descriptors out of the
// public interface.
// ---------------------------------------------------------------------------
class VulkanCommandList final : public CommandList {
public:
    explicit VulkanCommandList(VulkanDevice& device) : m_device(device) {}

    void reset(VkCommandBuffer cmd, VkDescriptorPool descriptorPool);

    void bindComputePipeline(PipelineHandle pipeline) override;
    void pushConstants(std::span<const std::byte> data) override;
    void bindStorageTexture(uint32_t slot, TextureHandle texture) override;
    void bindStorageBuffer(uint32_t slot, BufferHandle buffer) override;
    void clearBuffer(BufferHandle buffer) override;
    void dispatch(uint32_t gx, uint32_t gy, uint32_t gz) override;
    void blitToSwapchain(TextureHandle src) override;

private:
    struct PendingBinding {
        uint32_t      slot    = 0;
        BindingType   type    = BindingType::StorageTexture;
        TextureHandle texture = TextureHandle::Invalid;
        BufferHandle  buffer  = BufferHandle::Invalid;
    };

    void setBinding(PendingBinding binding);
    void flushBindings();

    VulkanDevice&               m_device;
    VkCommandBuffer             m_cmd            = VK_NULL_HANDLE;
    VkDescriptorPool            m_descriptorPool = VK_NULL_HANDLE;
    PipelineHandle              m_pipeline       = PipelineHandle::Invalid;
    std::vector<PendingBinding> m_bindings;
    bool                        m_bindingsDirty  = false;
};

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(const DeviceCreateInfo& info);
    ~VulkanDevice() override;

    CommandList& beginFrame() override;
    void         endFrame() override;
    void         onResize(uint32_t width, uint32_t height) override;
    Extent2D     swapchainExtent() const override;

    TextureHandle  createTexture(const TextureDesc& desc) override;
    BufferHandle   createBuffer(const BufferDesc& desc) override;
    ShaderHandle   createShader(std::string_view logicalName) override;
    PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;

    void updateBuffer(BufferHandle handle, std::span<const std::byte> data,
                      uint64_t offset) override;
    void readTexture(TextureHandle handle, std::span<float> out) override;
    void readBuffer(BufferHandle handle, std::span<std::byte> out, uint64_t offset) override;

    void destroy(TextureHandle handle) override;
    void destroy(BufferHandle handle) override;
    void destroy(ShaderHandle handle) override;
    void destroy(PipelineHandle handle) override;

private:
    friend class VulkanCommandList;

    struct Frame {
        VkCommandPool    commandPool    = VK_NULL_HANDLE;
        VkCommandBuffer  commandBuffer  = VK_NULL_HANDLE;
        VkFence          inFlight       = VK_NULL_HANDLE;
        VkSemaphore      imageAvailable = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    };

    void createInstance(const DeviceCreateInfo& info);
    void createSurface(void* nativeWindowHandle);
    void selectDeviceAndQueue();
    void createAllocator();
    void createFrames();

    void recreateSwapchainIfNeeded();
    void collectGarbage();
    void defer(std::function<void()> deleter);

    // Runs a short command buffer and blocks until it has completed. Used only
    // by the readback paths; the frame path never needs it.
    void submitBlocking(const std::function<void(VkCommandBuffer)>& record);

    // Barrier helpers — the whole reason transitions never surface in rhi.hpp.
    void transitionTexture(VkCommandBuffer cmd, Texture& texture, VkImageLayout layout,
                           VkPipelineStageFlags2 stage, VkAccessFlags2 access);
    void transitionSwapchainImage(VkCommandBuffer cmd, VkImageLayout layout,
                                  VkPipelineStageFlags2 stage, VkAccessFlags2 access);

    vkb::Instance       m_instance{};
    vkb::PhysicalDevice m_physicalDevice{};
    vkb::Device         m_device{};
    VkSurfaceKHR        m_surface     = VK_NULL_HANDLE;
    VkQueue             m_queue       = VK_NULL_HANDLE;
    uint32_t            m_queueFamily = 0;
    VmaAllocator        m_allocator   = VK_NULL_HANDLE;

    std::unique_ptr<Swapchain>          m_swapchain;
    std::array<Frame, kFramesInFlight>  m_frames{};
    VulkanCommandList                   m_commandList;

    Pool<Texture, TextureHandle>   m_textures;
    Pool<Buffer, BufferHandle>     m_buffers;
    Pool<Shader, ShaderHandle>     m_shaders;
    Pool<Pipeline, PipelineHandle> m_pipelines;

    struct PendingDeletion {
        uint64_t              retireAfter = 0;
        std::function<void()> deleter;
    };
    std::vector<PendingDeletion> m_deletionQueue;

    // <shaderRoot>/vulkan — this backend's own SPIR-V variants.
    std::filesystem::path m_shaderDirectory;

    // Producer state of the swapchain image currently being written, so the
    // pre-present barrier knows what it has to wait for.
    VkPipelineStageFlags2 m_swapchainStage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        m_swapchainAccess = VK_ACCESS_2_NONE;

    VkExtent2D m_windowExtent    = {};
    uint64_t   m_frameCounter    = 0;
    uint32_t   m_frameIndex      = 0;
    uint32_t   m_imageIndex      = 0;
    bool       m_frameActive     = false;
    bool       m_swapchainDirty  = false;
};

} // namespace rhi::vulkan
