// The backend registry. This is the only translation unit outside
// src/rhi/<backend>/ that names concrete backends, and it names them through
// symbol-free headers — no graphics API type reaches this file.
//
// Which backends exist here is a build-time decision (FITZEL_WITH_*); which
// one runs is a runtime decision made by the caller of createDevice.

#include "rhi/rhi.hpp"

#ifdef FITZEL_WITH_VULKAN
#include "rhi/vulkan/vk_backend.hpp"
#endif
#ifdef FITZEL_WITH_OPENGL
#include "rhi/opengl/gl_backend.hpp"
#endif

#include <stdexcept>
#include <string>

namespace rhi {
namespace {

const char* backendName(Backend backend) {
    switch (backend) {
        case Backend::Vulkan: return "vulkan";
        case Backend::OpenGL: return "opengl";
    }
    return "unknown";
}

[[noreturn]] void reportUnavailable(Backend backend) {
    throw std::runtime_error(std::string("rhi: backend '") + backendName(backend) +
                             "' is not linked into this binary");
}

} // namespace

bool isBackendAvailable(Backend backend) {
    switch (backend) {
        case Backend::Vulkan:
#ifdef FITZEL_WITH_VULKAN
            return true;
#else
            return false;
#endif
        case Backend::OpenGL:
#ifdef FITZEL_WITH_OPENGL
            return true;
#else
            return false;
#endif
    }
    return false;
}

WindowRequirements windowRequirements(Backend backend, bool enableDebug) {
    switch (backend) {
        case Backend::Vulkan:
#ifdef FITZEL_WITH_VULKAN
            return vulkan::windowRequirements(enableDebug);
#else
            break;
#endif
        case Backend::OpenGL:
#ifdef FITZEL_WITH_OPENGL
            return opengl::windowRequirements(enableDebug);
#else
            break;
#endif
    }
    reportUnavailable(backend);
}

std::unique_ptr<Device> createDevice(Backend backend, const DeviceCreateInfo& info) {
    switch (backend) {
        case Backend::Vulkan:
#ifdef FITZEL_WITH_VULKAN
            return vulkan::createDevice(info);
#else
            break;
#endif
        case Backend::OpenGL:
#ifdef FITZEL_WITH_OPENGL
            return opengl::createDevice(info);
#else
            break;
#endif
    }
    reportUnavailable(backend);
}

} // namespace rhi
