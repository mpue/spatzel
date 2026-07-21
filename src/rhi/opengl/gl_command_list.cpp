#include "rhi/opengl/gl_device.hpp"

#include <stdexcept>

namespace rhi::opengl {

void GlCommandList::bindComputePipeline(PipelineHandle handle) {
    const GlPipeline& pipeline = m_device.m_pipelines.get(handle);
    glUseProgram(pipeline.program);
    m_pipeline = handle;
}

void GlCommandList::pushConstants(std::span<const std::byte> data) {
    const GlPipeline& pipeline = m_device.m_pipelines.get(m_pipeline);
    if (data.size() > pipeline.pushConstantSize) {
        throw std::runtime_error("rhi: pushConstants exceeds the pipeline's push constant size");
    }
    glNamedBufferSubData(pipeline.pushConstantUbo, 0, static_cast<GLsizeiptr>(data.size()),
                         data.data());
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, pipeline.pushConstantUbo);
}

void GlCommandList::bindStorageTexture(uint32_t slot, TextureHandle handle) {
    const GlTexture& texture = m_device.m_textures.get(handle);
    glBindImageTexture(slot, texture.texture, 0, GL_FALSE, 0, GL_READ_WRITE,
                       texture.internalFormat);
}

void GlCommandList::bindStorageBuffer(uint32_t slot, BufferHandle handle) {
    const GlBuffer& buffer = m_device.m_buffers.get(handle);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, buffer.buffer);
}

void GlCommandList::dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    // GL's equivalent of the Vulkan backend's pre-dispatch barrier: make any
    // earlier access to these images and buffers complete before the shader
    // touches them. The storage-buffer bit is what makes the dispatch-ordering
    // guarantee in rhi.hpp true for a multi-pass algorithm.
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    glDispatchCompute(gx, gy, gz);
    // And the post-dispatch half, so the blit and any readback see the writes.
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                    GL_BUFFER_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT |
                    GL_TEXTURE_UPDATE_BARRIER_BIT);
}

void GlCommandList::blitToSwapchain(TextureHandle src) {
    const GlTexture& texture = m_device.m_textures.get(src);

    glNamedFramebufferTexture(m_device.m_blitFramebuffer, GL_COLOR_ATTACHMENT0, texture.texture, 0);
    glNamedFramebufferReadBuffer(m_device.m_blitFramebuffer, GL_COLOR_ATTACHMENT0);

    const Extent2D target = m_device.m_extent;

    // The seam says texel (0, 0) is top-left. GL's default framebuffer puts
    // y = 0 at the bottom, so the destination rectangle is inverted here.
    // Without this the two backends differ by a vertical mirror.
    glBlitNamedFramebuffer(m_device.m_blitFramebuffer, 0,
                           0, 0, static_cast<GLint>(texture.width),
                           static_cast<GLint>(texture.height),
                           0, static_cast<GLint>(target.height),
                           static_cast<GLint>(target.width), 0,
                           GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

} // namespace rhi::opengl
