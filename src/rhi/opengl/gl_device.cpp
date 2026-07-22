#include "rhi/opengl/gl_device.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace rhi::opengl {
namespace {

void GLAD_API_PTR debugCallback(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei,
                                const GLchar* message, const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
        return;
    }
    const char* label = type == GL_DEBUG_TYPE_ERROR ? "error" : "warning";
    std::fprintf(stderr, "[opengl %s] (%u) %s\n", label, id,
                 message != nullptr ? message : "<no message>");
}

std::vector<char> readBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("cannot open SPIR-V module: " + path.string());
    }
    const std::streamsize byteCount = file.tellg();
    if (byteCount <= 0) {
        throw std::runtime_error("empty SPIR-V module: " + path.string());
    }
    std::vector<char> bytes(static_cast<size_t>(byteCount));
    file.seekg(0);
    file.read(bytes.data(), byteCount);
    if (!file) {
        throw std::runtime_error("short read on SPIR-V module: " + path.string());
    }
    return bytes;
}

GLenum toGlInternalFormat(Format format) {
    switch (format) {
        case Format::RGBA8Unorm:  return GL_RGBA8;
        case Format::BGRA8Unorm:  return GL_RGBA8; // GL has no BGRA storage format
        case Format::RGBA16Float: return GL_RGBA16F;
        case Format::RGBA32Float: return GL_RGBA32F;
        case Format::Undefined:   break;
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
GlDevice::GlDevice(const DeviceCreateInfo& info) : m_commandList(*this) {
    if (info.nativeWindowHandle == nullptr) {
        throw std::runtime_error("rhi: DeviceCreateInfo::nativeWindowHandle is null");
    }
    m_window          = info.nativeWindowHandle;
    m_extent          = info.framebufferSize;
    m_shaderDirectory = std::filesystem::path(info.shaderRoot) / "opengl";

    auto* window = static_cast<GLFWwindow*>(m_window);
    // The context was created with the window — see rhi::windowRequirements.
    // All this backend has to do is adopt it on this thread.
    glfwMakeContextCurrent(window);
    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0) {
        throw std::runtime_error("failed to load OpenGL 4.6 core entry points");
    }
    glfwSwapInterval(1);

    if (info.enableDebug) {
        glEnable(GL_DEBUG_OUTPUT);
        // Synchronous so a message names the call that produced it.
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(&debugCallback, nullptr);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
    }

    if (GLAD_GL_ARB_gl_spirv == 0) {
        throw std::runtime_error("GL_ARB_gl_spirv is required but not supported by this driver");
    }

    glCreateFramebuffers(1, &m_blitFramebuffer);

    // Reported so "no GL errors" is a claim that can be checked rather than
    // assumed: without a debug context the callback would never fire.
    GLint contextFlags = 0;
    glGetIntegerv(GL_CONTEXT_FLAGS, &contextFlags);
    const bool debugContext = (contextFlags & GL_CONTEXT_FLAG_DEBUG_BIT) != 0;

    const GLubyte* renderer = glGetString(GL_RENDERER);
    std::fprintf(stderr, "[rhi] OpenGL backend on %s (debug context: %s)\n",
                 renderer != nullptr ? reinterpret_cast<const char*>(renderer) : "<unknown>",
                 debugContext ? "yes" : "no");
}

GlDevice::~GlDevice() {
    // GL destruction is immediate: the driver keeps objects alive until the
    // commands referencing them have retired. There is nothing to defer, which
    // is why the seam has no waitIdle for the engine to call.
    m_pipelines.forEachAlive([](GlPipeline& pipeline) {
        glDeleteProgram(pipeline.program);
        if (pipeline.pushConstantUbo != 0) {
            glDeleteBuffers(1, &pipeline.pushConstantUbo);
        }
    });
    m_shaders.forEachAlive([](GlShader& shader) { glDeleteShader(shader.shader); });
    m_textures.forEachAlive([](GlTexture& texture) { glDeleteTextures(1, &texture.texture); });
    m_buffers.forEachAlive([](GlBuffer& buffer) { glDeleteBuffers(1, &buffer.buffer); });

    if (m_blitFramebuffer != 0) {
        glDeleteFramebuffers(1, &m_blitFramebuffer);
    }
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------
CommandList& GlDevice::beginFrame() {
    if (m_frameActive) {
        throw std::runtime_error("rhi: beginFrame called while a frame is already recording");
    }
    // No image to acquire and no swapchain to rebuild: the default framebuffer
    // follows the window on its own. Resizing is pure bookkeeping here.
    m_frameActive = true;
    return m_commandList;
}

void GlDevice::endFrame() {
    if (!m_frameActive) {
        throw std::runtime_error("rhi: endFrame called without a matching beginFrame");
    }
    glfwSwapBuffers(static_cast<GLFWwindow*>(m_window));
    m_frameActive = false;
}

void GlDevice::onResize(uint32_t width, uint32_t height) { m_extent = {width, height}; }

Extent2D GlDevice::swapchainExtent() const { return m_extent; }

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------
TextureHandle GlDevice::createTexture(const TextureDesc& desc) {
    if (desc.width == 0 || desc.height == 0 || desc.depth != 1) {
        throw std::runtime_error("rhi: only 2D textures with depth == 1 are supported");
    }
    const GLenum internalFormat = toGlInternalFormat(desc.format);
    if (internalFormat == 0) {
        throw std::runtime_error("rhi: createTexture with an undefined format");
    }

    GlTexture texture;
    texture.width          = desc.width;
    texture.height         = desc.height;
    texture.internalFormat = internalFormat;

    glCreateTextures(GL_TEXTURE_2D, 1, &texture.texture);
    glTextureStorage2D(texture.texture, 1, internalFormat, static_cast<GLsizei>(desc.width),
                       static_cast<GLsizei>(desc.height));
    if (desc.debugName != nullptr) {
        glObjectLabel(GL_TEXTURE, texture.texture, -1, desc.debugName);
    }
    return m_textures.insert(texture);
}

BufferHandle GlDevice::createBuffer(const BufferDesc& desc) {
    if (desc.size == 0) {
        throw std::runtime_error("rhi: createBuffer with size 0");
    }

    GlBuffer buffer;
    buffer.size        = static_cast<GLsizeiptr>(desc.size);
    buffer.hostVisible = desc.access != MemoryAccess::GpuOnly;

    GLbitfield flags = 0;
    if (desc.access == MemoryAccess::CpuToGpu) {
        flags = GL_DYNAMIC_STORAGE_BIT;
    } else if (desc.access == MemoryAccess::GpuToCpu) {
        flags = GL_MAP_READ_BIT;
    }

    glCreateBuffers(1, &buffer.buffer);
    glNamedBufferStorage(buffer.buffer, buffer.size, nullptr, flags);
    if (desc.debugName != nullptr) {
        glObjectLabel(GL_BUFFER, buffer.buffer, -1, desc.debugName);
    }
    return m_buffers.insert(buffer);
}

ShaderHandle GlDevice::createShader(std::string_view logicalName) {
    // Same logical name as every other backend; the variant below this
    // directory is the backend's own business.
    const std::filesystem::path path =
        m_shaderDirectory / (std::string(logicalName) + ".comp.spv");
    const std::vector<char> spirv = readBinary(path);

    GlShader shader;
    shader.shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderBinary(1, &shader.shader, GL_SHADER_BINARY_FORMAT_SPIR_V, spirv.data(),
                   static_cast<GLsizei>(spirv.size()));
    glSpecializeShader(shader.shader, "main", 0, nullptr, nullptr);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader.shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        GLint length = 0;
        glGetShaderiv(shader.shader, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(length > 0 ? length : 1), '\0');
        glGetShaderInfoLog(shader.shader, length, nullptr, log.data());
        glDeleteShader(shader.shader);
        throw std::runtime_error("specialising " + path.string() + " failed: " + log);
    }
    return m_shaders.insert(shader);
}

PipelineHandle GlDevice::createComputePipeline(const ComputePipelineDesc& desc) {
    const GlShader& shader = m_shaders.get(desc.cs);

    GlPipeline pipeline;
    pipeline.pushConstantSize = desc.pushConstantSize;
    pipeline.program          = glCreateProgram();
    glAttachShader(pipeline.program, shader.shader);
    glLinkProgram(pipeline.program);
    glDetachShader(pipeline.program, shader.shader);

    GLint linked = GL_FALSE;
    glGetProgramiv(pipeline.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint length = 0;
        glGetProgramiv(pipeline.program, GL_INFO_LOG_LENGTH, &length);
        std::string log(static_cast<size_t>(length > 0 ? length : 1), '\0');
        glGetProgramInfoLog(pipeline.program, length, nullptr, log.data());
        glDeleteProgram(pipeline.program);
        throw std::runtime_error("linking compute program failed: " + log);
    }

    if (desc.pushConstantSize > 0) {
        // GL has no push constants. The same byte blob goes into a uniform
        // block instead; std140 lays it out identically for this shader.
        // Rounded up to 16 because that is the std140 block granularity.
        const GLsizeiptr size = (desc.pushConstantSize + 15) / 16 * 16;
        glCreateBuffers(1, &pipeline.pushConstantUbo);
        glNamedBufferStorage(pipeline.pushConstantUbo, size, nullptr, GL_DYNAMIC_STORAGE_BIT);
    }

    if (desc.debugName != nullptr) {
        glObjectLabel(GL_PROGRAM, pipeline.program, -1, desc.debugName);
    }
    return m_pipelines.insert(pipeline);
}

void GlDevice::updateBuffer(BufferHandle handle, std::span<const std::byte> data, uint64_t offset) {
    GlBuffer& buffer = m_buffers.get(handle);
    if (!buffer.hostVisible) {
        throw std::runtime_error("rhi: updateBuffer requires a host-visible buffer");
    }
    if (static_cast<GLsizeiptr>(offset + data.size()) > buffer.size) {
        throw std::runtime_error("rhi: updateBuffer would write past the end of the buffer");
    }
    glNamedBufferSubData(buffer.buffer, static_cast<GLintptr>(offset),
                         static_cast<GLsizeiptr>(data.size()), data.data());
}

void GlDevice::readTexture(TextureHandle handle, std::span<float> out) {
    const GlTexture& texture = m_textures.get(handle);

    const size_t texelCount = static_cast<size_t>(texture.width) * texture.height;
    if (out.size() < texelCount * 4) {
        throw std::runtime_error("rhi: readTexture destination is too small");
    }

    // Anything the compute pass wrote has to be visible to the readback path.
    glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT | GL_PIXEL_BUFFER_BARRIER_BIT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTextureImage(texture.texture, 0, GL_RGBA, GL_FLOAT,
                      static_cast<GLsizei>(texelCount * 4 * sizeof(float)), out.data());
}

void GlDevice::readBuffer(BufferHandle handle, std::span<std::byte> out, uint64_t offset) {
    const GlBuffer& buffer = m_buffers.get(handle);
    if (offset + out.size() > static_cast<uint64_t>(buffer.size)) {
        throw std::runtime_error("rhi: readBuffer would read past the end of the buffer");
    }
    if (out.empty()) {
        return;
    }

    // Anything a compute pass wrote has to be visible to the client read.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);

    // Reading a device-local buffer straight to the host — glGetNamedBufferSubData
    // or a direct map — migrates it out of video memory, which the driver flags
    // as a performance warning and which would also thrash a buffer the render
    // path still uses. So stage it exactly as the Vulkan backend does: a
    // GPU-side copy into a transient host-readable buffer (the source stays in
    // video), then map that. Genuinely host-visible sources are mapped directly.
    if (buffer.hostVisible) {
        const void* mapped = glMapNamedBufferRange(buffer.buffer, static_cast<GLintptr>(offset),
                                                   static_cast<GLsizeiptr>(out.size()),
                                                   GL_MAP_READ_BIT);
        if (mapped == nullptr) {
            throw std::runtime_error("rhi: readBuffer failed to map a host-visible buffer");
        }
        std::memcpy(out.data(), mapped, out.size());
        glUnmapNamedBuffer(buffer.buffer);
        return;
    }

    GLuint staging = 0;
    glCreateBuffers(1, &staging);
    // GL_CLIENT_STORAGE_BIT asks the driver to keep the staging buffer in host
    // memory from the start, so the GPU copy lands there directly and the map
    // reads host memory — no video->host migration, and no performance warning.
    glNamedBufferStorage(staging, static_cast<GLsizeiptr>(out.size()), nullptr,
                         GL_MAP_READ_BIT | GL_CLIENT_STORAGE_BIT);
    glCopyNamedBufferSubData(buffer.buffer, staging, static_cast<GLintptr>(offset), 0,
                             static_cast<GLsizeiptr>(out.size()));
    const void* mapped =
        glMapNamedBufferRange(staging, 0, static_cast<GLsizeiptr>(out.size()), GL_MAP_READ_BIT);
    if (mapped == nullptr) {
        glDeleteBuffers(1, &staging);
        throw std::runtime_error("rhi: readBuffer failed to map its staging buffer");
    }
    std::memcpy(out.data(), mapped, out.size());
    glUnmapNamedBuffer(staging);
    glDeleteBuffers(1, &staging);
}

void GlDevice::destroy(TextureHandle handle) {
    const GlTexture texture = m_textures.remove(handle);
    glDeleteTextures(1, &texture.texture);
}

void GlDevice::destroy(BufferHandle handle) {
    const GlBuffer buffer = m_buffers.remove(handle);
    glDeleteBuffers(1, &buffer.buffer);
}

void GlDevice::destroy(ShaderHandle handle) {
    const GlShader shader = m_shaders.remove(handle);
    glDeleteShader(shader.shader);
}

void GlDevice::destroy(PipelineHandle handle) {
    const GlPipeline pipeline = m_pipelines.remove(handle);
    glDeleteProgram(pipeline.program);
    if (pipeline.pushConstantUbo != 0) {
        glDeleteBuffers(1, &pipeline.pushConstantUbo);
    }
}

} // namespace rhi::opengl
