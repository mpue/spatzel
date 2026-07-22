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
#include <string_view>
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

enum class Backend : uint8_t { Vulkan, OpenGL };

// True if this binary was linked with an implementation of `backend`.
[[nodiscard]] bool isBackendAvailable(Backend backend);

// ---------------------------------------------------------------------------
// Window requirements
//
// Some APIs need the window to be created a particular way — a client-API
// context has to exist before the device does, and cannot be attached
// afterwards. So the backend states what it needs and the platform layer
// executes it. The platform layer never learns which backend asked.
// ---------------------------------------------------------------------------
enum class ClientApi : uint8_t {
    None,       // the backend creates its own presentation surface
    OpenGLCore, // the window must own a core-profile context of the given version
};

struct WindowRequirements {
    ClientApi api          = ClientApi::None;
    uint32_t  majorVersion = 0;
    uint32_t  minorVersion = 0;
    bool      debugContext = false;
};

// Must be answerable before a device — or even a window — exists.
[[nodiscard]] WindowRequirements windowRequirements(Backend backend, bool enableDebug);

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
    CopySrc     = 1u << 2,
    CopyDst     = 1u << 3,
};

enum class TextureUsage : uint32_t {
    None        = 0,
    Storage     = 1u << 0,
    Sampled     = 1u << 1,
    CopySrc     = 1u << 2,
    CopyDst     = 1u << 3,
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

// Texel (0, 0) is the TOP-LEFT corner of a texture, and of the presented
// image. This is a contract, not an observation: a backend whose presentation
// surface disagrees flips on its own side of the seam. Without it, two
// backends running the same shader differ by a vertical mirror.
inline constexpr bool kTopLeftOrigin = true;

// Presentation applies no colour space conversion: whatever a render target
// holds is what reaches the screen. A backend whose natural presentation
// surface would encode on the way out must choose one that does not.
// Like the origin above, this only shows up when a second backend disagrees.
inline constexpr bool kPresentsUnconverted = true;

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

    // Zero a buffer's bytes. The one write a bump-allocated pass needs before
    // its atomics run, and one the interface could not otherwise express
    // without a host round-trip that would pin the buffer in host-visible
    // memory. Ordered like a dispatch: a dispatch recorded after it sees the
    // cleared bytes (see dispatch()).
    virtual void clearBuffer(BufferHandle buffer)                      = 0;

    // Dispatches are ordered against each other: everything a dispatch writes
    // to a storage texture or a storage buffer — and every preceding
    // clearBuffer — is visible to every dispatch recorded after it, and to a
    // subsequent blitToSwapchain or readback.
    //
    // Stated because a multi-pass algorithm depends on it and cannot express it
    // — barriers are deliberately absent from this interface, so the guarantee
    // has to live in the contract instead. Ordering only, never a
    // synchronisation primitive the caller can reach.
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
    // Enables whatever diagnostics the backend offers — validation layers,
    // debug message callbacks, object naming.
    bool        enableDebug        = false;
    const char* applicationName    = "fitzel";
    // Root directory holding the compiled shader variants. Each backend picks
    // its own subdirectory; the layout below this path is a backend detail.
    const char* shaderRoot         = "shaders";
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

    // --- Resources ---------------------------------------------------------
    //
    // Destruction is always safe to request: a backend that can still have the
    // resource in flight defers the actual release itself. There is
    // deliberately no way to ask the device to go idle — that is a
    // synchronisation model, and synchronisation models differ per backend.
    [[nodiscard]] virtual TextureHandle  createTexture(const TextureDesc& desc)          = 0;
    [[nodiscard]] virtual BufferHandle   createBuffer(const BufferDesc& desc)            = 0;
    [[nodiscard]] virtual PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;

    // Shaders are referenced by logical name, never by compiled bytes: the
    // same GLSL yields different binaries per backend, and choosing between
    // them is not a decision the engine can make correctly.
    [[nodiscard]] virtual ShaderHandle createShader(std::string_view logicalName) = 0;

    // Host-visible buffers only (MemoryAccess::CpuToGpu).
    virtual void updateBuffer(BufferHandle buffer, std::span<const std::byte> data,
                              uint64_t offset = 0) = 0;

    // Blocking read of a texture's contents as linear RGBA floats, row-major
    // from the top-left texel. `out` must hold width * height * 4 values.
    // Intended for verification, not for a rendering path.
    virtual void readTexture(TextureHandle texture, std::span<float> out) = 0;

    // Blocking read of raw buffer bytes. The buffer must have been created with
    // BufferUsage::CopySrc. Like readTexture this is a verification path — it
    // may stall the device — and exists so that what a compute pass wrote into
    // a buffer can be inspected rather than only believed.
    virtual void readBuffer(BufferHandle buffer, std::span<std::byte> out,
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
