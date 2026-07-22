#pragma once

// Backend-internal: the OpenGL implementation of rhi::Device and
// rhi::CommandList.
//
// GL is an immediate-mode, single-context API: there is no command buffer to
// record into, no queue to submit to and no image layout to track. The
// "command list" therefore issues calls as they arrive, and beginFrame /
// endFrame reduce to bookkeeping plus a buffer swap. That asymmetry with
// Vulkan is exactly what the seam is there to absorb.

#include "rhi/handle_pool.hpp"
#include "rhi/rhi.hpp"

#include <glad/gl.h>

#include <cstddef>
#include <filesystem>
#include <string_view>

namespace rhi::opengl {

using rhi::detail::Pool;

struct GlTexture {
    GLuint   texture        = 0;
    uint32_t width          = 0;
    uint32_t height         = 0;
    GLenum   internalFormat = 0;
};

struct GlBuffer {
    GLuint      buffer      = 0;
    GLsizeiptr  size        = 0;
    bool        hostVisible = false;
};

struct GlShader {
    GLuint shader = 0;
};

struct GlPipeline {
    GLuint   program          = 0;
    GLuint   pushConstantUbo  = 0;
    uint32_t pushConstantSize = 0;
};

class GlDevice;

class GlCommandList final : public CommandList {
public:
    explicit GlCommandList(GlDevice& device) : m_device(device) {}

    void bindComputePipeline(PipelineHandle pipeline) override;
    void pushConstants(std::span<const std::byte> data) override;
    void bindStorageTexture(uint32_t slot, TextureHandle texture) override;
    void bindStorageBuffer(uint32_t slot, BufferHandle buffer) override;
    void clearBuffer(BufferHandle buffer) override;
    void dispatch(uint32_t gx, uint32_t gy, uint32_t gz) override;
    void blitToSwapchain(TextureHandle src) override;
    void endUiFrame() override {} // no UI on this backend

private:
    GlDevice&      m_device;
    PipelineHandle m_pipeline = PipelineHandle::Invalid;
};

class GlDevice final : public Device {
public:
    explicit GlDevice(const DeviceCreateInfo& info);
    ~GlDevice() override;

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

    // No ImGui overlay on this backend yet: report unavailable and the engine
    // runs without one. The GL render backend would be a small addition, but
    // the milestone only requires the overlay on Vulkan.
    [[nodiscard]] bool initUi() override { return false; }
    void               beginUiFrame() override {}
    void               shutdownUi() override {}

private:
    friend class GlCommandList;

    void* m_window = nullptr; // GLFWwindow*, owned by the platform layer

    GlCommandList m_commandList;
    GLuint        m_blitFramebuffer = 0;

    Pool<GlTexture, TextureHandle>   m_textures;
    Pool<GlBuffer, BufferHandle>     m_buffers;
    Pool<GlShader, ShaderHandle>     m_shaders;
    Pool<GlPipeline, PipelineHandle> m_pipelines;

    std::filesystem::path m_shaderDirectory;
    Extent2D              m_extent{};
    bool                  m_frameActive = false;
};

} // namespace rhi::opengl
