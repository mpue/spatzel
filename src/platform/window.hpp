#pragma once

// Platform layer — windowing, input and timing. Contains no graphics API code
// of any kind; the RHI backend receives only an opaque native handle from here.
//
// It does depend on rhi.hpp, for one reason: a client-API context has to exist
// from the moment the window is created and cannot be attached later. The
// backend states its requirements, this layer executes them, and neither side
// learns anything about the other.

#include "rhi/rhi.hpp"

#include <array>
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

// Only the keys something actually asks about. Growing this enum is the price
// of keeping GLFW's key codes out of every layer above.
enum class Key : uint8_t {
    Escape,
    W,
    A,
    S,
    D,
    Q,
    E,
    LeftShift,
    Count,
};

enum class MouseButton : uint8_t {
    Left,
    Right,
    Count,
};

// A snapshot of the input devices, refreshed by Window::pollEvents. Consumers
// read state rather than subscribing to events: everything driven by input so
// far is continuous (movement, look), not discrete.
class InputState {
public:
    [[nodiscard]] bool isDown(Key key) const {
        return m_keys[static_cast<size_t>(key)];
    }
    [[nodiscard]] bool isDown(MouseButton button) const {
        return m_buttons[static_cast<size_t>(button)];
    }

    // Cursor movement in pixels since the previous poll. Zero unless the
    // cursor is currently captured, so a consumer never sees a jump when the
    // capture starts or ends.
    [[nodiscard]] double cursorDeltaX() const { return m_cursorDeltaX; }
    [[nodiscard]] double cursorDeltaY() const { return m_cursorDeltaY; }
    [[nodiscard]] bool   isCursorCaptured() const { return m_cursorCaptured; }

private:
    friend class Window;

    std::array<bool, static_cast<size_t>(Key::Count)>         m_keys{};
    std::array<bool, static_cast<size_t>(MouseButton::Count)> m_buttons{};
    double m_cursorDeltaX  = 0.0;
    double m_cursorDeltaY  = 0.0;
    bool   m_cursorCaptured = false;
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

    [[nodiscard]] const InputState& input() const { return m_input; }

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
    void updateInput();
    void setCursorCaptured(bool captured);

    void*      m_handle  = nullptr;
    bool       m_resized = false;
    InputState m_input{};
    double     m_lastCursorX = 0.0;
    double     m_lastCursorY = 0.0;
};

// Monotonic seconds since process start.
[[nodiscard]] double timeSeconds();

} // namespace platform
