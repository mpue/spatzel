#pragma once

// rhi.hpp — the single source of truth for the render-hardware-interface seam.
//
// BACKEND-AGNOSTIC. No graphics-API symbol may appear in this file or in any
// file that includes it. Resources are referenced exclusively through opaque
// handles; barriers, layout transitions, descriptor management and
// synchronisation are backend internals and are deliberately absent from this
// interface.
//
// Failures are reported by throwing std::runtime_error.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace rhi {

// ---------------------------------------------------------------------------
// Handles
//
// Opaque, trivially copyable, comparable. A handle is only meaningful to the
// Device that produced it. Invalid == 0 so a default-constructed handle is
// always invalid.
// ---------------------------------------------------------------------------
enum class BufferHandle   : uint32_t { Invalid = 0 };
enum class TextureHandle  : uint32_t { Invalid = 0 };
enum class ShaderHandle   : uint32_t { Invalid = 0 };
enum class PipelineHandle : uint32_t { Invalid = 0 };

template <typename H>
[[nodiscard]] constexpr bool isValid(H handle) noexcept {
    return handle != H::Invalid;
}

enum class Backend : uint8_t { Vulkan };

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------
enum class Format : uint8_t {
    Undefined = 0,
    RGBA8Unorm,
    BGRA8Unorm,
    RGBA16Float,
    RGBA32Float,
};

enum class MemoryAccess : uint8_t {
    GpuOnly,  // device-local, not host visible
    CpuToGpu, // host visible, written by the CPU, read by the GPU
    GpuToCpu, // host visible, written by the GPU, read back by the CPU
};

enum class BufferUsage : uint32_t {
    None        = 0,
    Storage     = 1u << 0,
    Uniform     = 1u << 1,
    TransferSrc = 1u << 2,
    TransferDst = 1u << 3,
};

enum class TextureUsage : uint32_t {
    None        = 0,
    Storage     = 1u << 0,
    Sampled     = 1u << 1,
    TransferSrc = 1u << 2,
    TransferDst = 1u << 3,
};

// Opt-in bitmask operators.
template <typename E> struct IsFlags : std::false_type {};
template <> struct IsFlags<BufferUsage>  : std::true_type {};
template <> struct IsFlags<TextureUsage> : std::true_type {};

template <typename E, typename = std::enable_if_t<IsFlags<E>::value>>
[[nodiscard]] constexpr E operator|(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));
}

template <typename E, typename = std::enable_if_t<IsFlags<E>::value>>
[[nodiscard]] constexpr E operator&(E a, E b) noexcept {
    using U = std::underlying_type_t<E>;
    return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));
}

template <typename E, typename = std::enable_if_t<IsFlags<E>::value>>
[[nodiscard]] constexpr bool any(E value) noexcept {
    return static_cast<std::underlying_type_t<E>>(value) != 0;
}

// ---------------------------------------------------------------------------
// Descriptors
// ---------------------------------------------------------------------------
struct Extent2D {
    uint32_t width  = 0;
    uint32_t height = 0;

    friend bool operator==(const Extent2D&, const Extent2D&) = default;
};

struct BufferDesc {
    uint64_t     size      = 0;
    BufferUsage  usage     = BufferUsage::None;
    MemoryAccess access    = MemoryAccess::GpuOnly;
    const char*  debugName = nullptr;
};

struct TextureDesc {
    uint32_t     width     = 0;
    uint32_t     height    = 0;
    uint32_t     depth     = 1;
    Format       format    = Format::Undefined;
    TextureUsage usage     = TextureUsage::None;
    const char*  debugName = nullptr;
};

enum class BindingType : uint8_t {
    StorageTexture,
    StorageBuffer,
    UniformBuffer,
};

// One entry per resource the shader declares in descriptor set 0. Declared
// explicitly rather than reflected out of the shader binary so the seam stays
// free of any SPIR-V reflection dependency.
struct BindingDesc {
    uint32_t    slot = 0;
    BindingType type = BindingType::StorageTexture;
};

struct ComputePipelineDesc {
    ShaderHandle                cs               = ShaderHandle::Invalid;
    uint32_t                    pushConstantSize = 0;
    std::span<const BindingDesc> bindings        = {};
    const char*                 debugName        = nullptr;
};

// ---------------------------------------------------------------------------
// Command recording
// ---------------------------------------------------------------------------
class CommandList {
public:
    virtual ~CommandList() = default;

    CommandList(const CommandList&)            = delete;
    CommandList& operator=(const CommandList&) = delete;

    virtual void bindComputePipeline(PipelineHandle pipeline)          = 0;
    virtual void pushConstants(std::span<const std::byte> data)        = 0;
    virtual void bindStorageTexture(uint32_t slot, TextureHandle tex)  = 0;
    virtual void bindStorageBuffer(uint32_t slot, BufferHandle buffer) = 0;
    virtual void dispatch(uint32_t gx, uint32_t gy, uint32_t gz)       = 0;

    // Copies `src` into the image that will be presented this frame, scaling
    // if the extents differ. All required resource state transitions are the
    // backend's business.
    virtual void blitToSwapchain(TextureHandle src) = 0;

protected:
    CommandList() = default;
};

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
struct DeviceCreateInfo {
    // Opaque platform window handle (platform::Window::nativeHandle()). The
    // backend knows how to turn this into a presentation surface; nothing
    // above the seam may interpret it.
    void*       nativeWindowHandle = nullptr;
    Extent2D    framebufferSize    = {};
    bool        enableValidation   = false;
    const char* applicationName    = "fitzel";
};

class Device {
public:
    virtual ~Device() = default;

    Device(const Device&)            = delete;
    Device& operator=(const Device&) = delete;

    // --- Frame lifecycle ---------------------------------------------------
    // beginFrame acquires a presentable image and returns a command list that
    // is valid until the matching endFrame. endFrame submits and presents.
    virtual CommandList& beginFrame()                        = 0;
    virtual void         endFrame()                          = 0;
    virtual void         onResize(uint32_t width, uint32_t height) = 0;

    // Extent of the presentation surface, i.e. the natural size for a
    // full-screen render target.
    [[nodiscard]] virtual Extent2D swapchainExtent() const = 0;

    // Blocks until the device is idle. Required before destroying resources
    // that may still be referenced by in-flight work.
    virtual void waitIdle() = 0;

    // --- Resources ---------------------------------------------------------
    [[nodiscard]] virtual TextureHandle  createTexture(const TextureDesc& desc)          = 0;
    [[nodiscard]] virtual BufferHandle   createBuffer(const BufferDesc& desc)            = 0;
    [[nodiscard]] virtual ShaderHandle   createShader(std::span<const uint32_t> spirv)   = 0;
    [[nodiscard]] virtual PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;

    // Host-visible buffers only (MemoryAccess::CpuToGpu).
    virtual void updateBuffer(BufferHandle buffer, std::span<const std::byte> data,
                              uint64_t offset = 0) = 0;

    virtual void destroy(TextureHandle handle)  = 0;
    virtual void destroy(BufferHandle handle)   = 0;
    virtual void destroy(ShaderHandle handle)   = 0;
    virtual void destroy(PipelineHandle handle) = 0;

protected:
    Device() = default;
};

// The one and only place in engine-level code where a backend is named.
[[nodiscard]] std::unique_ptr<Device> createDevice(Backend backend,
                                                   const DeviceCreateInfo& info);

} // namespace rhi
