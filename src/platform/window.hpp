#pragma once

// Platform layer — windowing, input and timing. Contains no graphics API code
// of any kind; the RHI backend receives only an opaque native handle from here.
//
// It does depend on rhi.hpp, for one reason: a client-API context has to exist
// from the moment the window is created and cannot be attached later. The
// backend states its requirements, this layer executes them, and neither side
// learns anything about the other.

#include "rhi/rhi.hpp"

#include <cstdint>
#include <string>

namespace platform {

struct Extent2D {
    uint32_t width  = 0;
    uint32_t height = 0;

    friend bool operator==(const Extent2D&, const Extent2D&) = default;
};

struct WindowDesc {
    uint32_t                width  = 1280;
    uint32_t                height = 720;
    std::string             title  = "fitzel";
    rhi::WindowRequirements graphics{};
};

enum class Key {
    Escape,
};

// RAII window. Owns the GLFW library initialisation for as long as any window
// lives (single-window is all milestone 1 needs, but the refcount keeps the
// lifetime rule honest).
class Window {
public:
    explicit Window(const WindowDesc& desc);
    ~Window();

    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&)                 = delete;
    Window& operator=(Window&&)      = delete;

    void pollEvents();
    // Blocks until an event arrives. Used to idle cheaply while minimised.
    void waitEvents() const;

    [[nodiscard]] bool shouldClose() const;
    void requestClose();

    [[nodiscard]] bool isKeyDown(Key key) const;

    // Size of the drawable surface in pixels (not screen coordinates).
    [[nodiscard]] Extent2D framebufferSize() const;
    [[nodiscard]] bool     isMinimised() const;

    // True exactly once per framebuffer size change; consumes the flag.
    [[nodiscard]] bool consumeResized();

    // Opaque handle for the graphics backend. The concrete type is a platform
    // detail the backend agrees on out-of-band; nothing above the RHI seam may
    // dereference it.
    [[nodiscard]] void* nativeHandle() const { return m_handle; }

private:
    void* m_handle  = nullptr;
    bool  m_resized = false;
};

// Monotonic seconds since process start.
[[nodiscard]] double timeSeconds();

} // namespace platform
