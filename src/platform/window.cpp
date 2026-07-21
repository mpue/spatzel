#include "platform/window.hpp"

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
        case Key::Escape: return GLFW_KEY_ESCAPE;
    }
    return GLFW_KEY_UNKNOWN;
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

void Window::pollEvents() { glfwPollEvents(); }

void Window::waitEvents() const { glfwWaitEvents(); }

bool Window::shouldClose() const {
    return glfwWindowShouldClose(asGlfw(m_handle)) == GLFW_TRUE;
}

void Window::requestClose() { glfwSetWindowShouldClose(asGlfw(m_handle), GLFW_TRUE); }

bool Window::isKeyDown(Key key) const {
    return glfwGetKey(asGlfw(m_handle), toGlfwKey(key)) == GLFW_PRESS;
}

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
