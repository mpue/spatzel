// Backend-side half of the factory. rhi::createDevice lives in
// src/rhi/factory.cpp and dispatches here.

#include "rhi/opengl/gl_backend.hpp"
#include "rhi/opengl/gl_device.hpp"

namespace rhi::opengl {

WindowRequirements windowRequirements(bool enableDebug) {
    // Unlike Vulkan, the context is inseparable from the window: it must exist
    // from creation, and a debug context cannot be requested afterwards.
    return WindowRequirements{
        .api          = ClientApi::OpenGLCore,
        .majorVersion = 4,
        .minorVersion = 6,
        .debugContext = enableDebug,
    };
}

std::unique_ptr<Device> createDevice(const DeviceCreateInfo& info) {
    return std::make_unique<GlDevice>(info);
}

} // namespace rhi::opengl
