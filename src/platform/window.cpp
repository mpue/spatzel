#include "platform/window.hpp"

#include <backends/imgui_impl_glfw.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace platform {
namespace {

int g_glfwRefCount = 0;

// GLFW invokes this from C code, so it must not throw. Failures are reported
// through the return values of the calls themselves.
void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, description);
}

GLFWwindow* asGlfw(void* handle) { return static_cast<GLFWwindow*>(handle); }

int toGlfwKey(Key key) {
    switch (key) {
        case Key::Escape:    return GLFW_KEY_ESCAPE;
        case Key::W:         return GLFW_KEY_W;
        case Key::A:         return GLFW_KEY_A;
        case Key::S:         return GLFW_KEY_S;
        case Key::D:         return GLFW_KEY_D;
        case Key::Q:         return GLFW_KEY_Q;
        case Key::E:         return GLFW_KEY_E;
        case Key::LeftShift: return GLFW_KEY_LEFT_SHIFT;
        case Key::Num1:      return GLFW_KEY_1;
        case Key::Num2:      return GLFW_KEY_2;
        case Key::Num3:      return GLFW_KEY_3;
        case Key::Count:     break;
    }
    return GLFW_KEY_UNKNOWN;
}

int toGlfwMouseButton(MouseButton button) {
    switch (button) {
        case MouseButton::Left:  return GLFW_MOUSE_BUTTON_LEFT;
        case MouseButton::Right: return GLFW_MOUSE_BUTTON_RIGHT;
        case MouseButton::Count: break;
    }
    return GLFW_MOUSE_BUTTON_LAST + 1;
}

} // namespace

Window::Window(const WindowDesc& desc) {
    if (g_glfwRefCount == 0) {
        glfwSetErrorCallback(&glfwErrorCallback);
        if (glfwInit() != GLFW_TRUE) {
            throw std::runtime_error("glfwInit failed");
        }
    }
    ++g_glfwRefCount;

    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // Executed on the backend's behalf. This layer does not know, and must not
    // know, which backend asked for it.
    switch (desc.graphics.api) {
        case rhi::ClientApi::None:
            // The backend creates its own presentation surface.
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            break;
        case rhi::ClientApi::OpenGLCore:
            glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,
                           static_cast<int>(desc.graphics.majorVersion));
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,
                           static_cast<int>(desc.graphics.minorVersion));
            glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT,
                           desc.graphics.debugContext ? GLFW_TRUE : GLFW_FALSE);
            break;
    }

    GLFWwindow* handle = glfwCreateWindow(static_cast<int>(desc.width),
                                          static_cast<int>(desc.height),
                                          desc.title.c_str(), nullptr, nullptr);
    if (handle == nullptr) {
        if (--g_glfwRefCount == 0) {
            glfwTerminate();
        }
        throw std::runtime_error("glfwCreateWindow failed");
    }

    m_handle = handle;
    glfwSetWindowUserPointer(handle, this);
    glfwSetFramebufferSizeCallback(handle, [](GLFWwindow* w, int, int) {
        static_cast<Window*>(glfwGetWindowUserPointer(w))->m_resized = true;
    });
}

Window::~Window() {
    if (m_handle != nullptr) {
        glfwDestroyWindow(asGlfw(m_handle));
        m_handle = nullptr;
    }
    if (--g_glfwRefCount == 0) {
        glfwTerminate();
    }
}

void Window::pollEvents() {
    glfwPollEvents();
    updateInput();
}

void Window::updateInput() {
    GLFWwindow* handle = asGlfw(m_handle);

    for (size_t i = 0; i < static_cast<size_t>(Key::Count); ++i) {
        const int code = toGlfwKey(static_cast<Key>(i));
        m_input.m_keys[i] = code != GLFW_KEY_UNKNOWN &&
                            glfwGetKey(handle, code) == GLFW_PRESS;
    }
    for (size_t i = 0; i < static_cast<size_t>(MouseButton::Count); ++i) {
        const int code = toGlfwMouseButton(static_cast<MouseButton>(i));
        m_input.m_buttons[i] = glfwGetMouseButton(handle, code) == GLFW_PRESS;
    }

    // Look is a hold-to-engage gesture: the cursor is only captured while the
    // right button is down, so the window stays resizable and alt-tab keeps
    // working without any special handling.
    const bool wantCapture = m_input.isDown(MouseButton::Right);
    if (wantCapture != m_input.m_cursorCaptured) {
        setCursorCaptured(wantCapture);
    }

    double x = 0.0;
    double y = 0.0;
    glfwGetCursorPos(handle, &x, &y);
    if (m_input.m_cursorCaptured) {
        m_input.m_cursorDeltaX = x - m_lastCursorX;
        m_input.m_cursorDeltaY = y - m_lastCursorY;
    } else {
        m_input.m_cursorDeltaX = 0.0;
        m_input.m_cursorDeltaY = 0.0;
    }
    m_lastCursorX = x;
    m_lastCursorY = y;
}

void Window::setCursorCaptured(bool captured) {
    GLFWwindow* handle = asGlfw(m_handle);
    glfwSetInputMode(handle, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported() == GLFW_TRUE) {
        // Unaccelerated motion while looking around; the desktop pointer keeps
        // its acceleration when the capture ends.
        glfwSetInputMode(handle, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    }
    m_input.m_cursorCaptured = captured;

    // Re-seed the reference position so the first frame after a transition
    // reports no movement instead of the jump GLFW just introduced.
    glfwGetCursorPos(handle, &m_lastCursorX, &m_lastCursorY);
    m_input.m_cursorDeltaX = 0.0;
    m_input.m_cursorDeltaY = 0.0;
}

void Window::initUi() {
    // install_callbacks = true: ImGui installs its own GLFW callbacks and
    // chains this window's existing framebuffer-size callback, so both keep
    // firing. InitForOther works for any render backend — the client-API value
    // only matters to the multi-viewport path, which is not enabled.
    ImGui_ImplGlfw_InitForOther(asGlfw(m_handle), true);
}

void Window::beginUiFrame() { ImGui_ImplGlfw_NewFrame(); }

void Window::shutdownUi() { ImGui_ImplGlfw_Shutdown(); }

void Window::waitEvents() const { glfwWaitEvents(); }

bool Window::shouldClose() const {
    return glfwWindowShouldClose(asGlfw(m_handle)) == GLFW_TRUE;
}

void Window::requestClose() { glfwSetWindowShouldClose(asGlfw(m_handle), GLFW_TRUE); }

Extent2D Window::framebufferSize() const {
    int w = 0;
    int h = 0;
    glfwGetFramebufferSize(asGlfw(m_handle), &w, &h);
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

bool Window::isMinimised() const {
    const Extent2D size = framebufferSize();
    return size.width == 0 || size.height == 0;
}

float Window::contentScale() const {
    float sx = 1.0f;
    float sy = 1.0f;
    glfwGetWindowContentScale(asGlfw(m_handle), &sx, &sy);
    const float scale = sx > sy ? sx : sy;
    return scale > 1.0f ? scale : 1.0f;
}

bool Window::consumeResized() {
    const bool resized = m_resized;
    m_resized = false;
    return resized;
}

double timeSeconds() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

} // namespace platform
