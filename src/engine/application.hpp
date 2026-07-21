#pragma once

// Engine layer. Sees rhi.hpp and the platform layer — never a backend.

#include "platform/window.hpp"
#include "rhi/rhi.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

struct AppConfig {
    uint32_t              width            = 1280;
    uint32_t              height           = 720;
    std::string           title            = "fitzel";
    rhi::Backend          backend          = rhi::Backend::Vulkan;
    bool                  enableValidation = false;
    std::filesystem::path shaderDirectory  = "shaders";
    // Stop after this many presented frames; 0 runs until the window closes.
    // Exists so an automated run can exercise the full startup/shutdown path.
    uint64_t              maxFrames        = 0;
};

// Owns the window, the device and the milestone-1 compute probe pass.
class Application {
public:
    explicit Application(const AppConfig& config);
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    void run();

private:
    void renderFrame();
    void resizeRenderTarget(rhi::Extent2D extent);
    void destroyRenderTarget();

    // Mirrors the push constant block in shaders/raymarch_probe.comp.
    struct ProbePushConstants {
        float resolution[2] = {0.0f, 0.0f};
        float time          = 0.0f;
    };

    platform::Window             m_window;
    std::unique_ptr<rhi::Device> m_device;

    rhi::ShaderHandle   m_shader       = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_pipeline     = rhi::PipelineHandle::Invalid;
    rhi::TextureHandle  m_renderTarget = rhi::TextureHandle::Invalid;
    rhi::Extent2D       m_targetExtent = {};

    uint64_t m_maxFrames    = 0;
    uint64_t m_framesDrawn  = 0;
};

} // namespace engine
