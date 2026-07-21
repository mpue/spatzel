#pragma once

// Engine layer. Sees rhi.hpp and the platform layer — never a backend.

#include "platform/window.hpp"
#include "rhi/rhi.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

struct AppConfig {
    uint32_t              width       = 1280;
    uint32_t              height      = 720;
    std::string           title       = "fitzel";
    rhi::Backend          backend     = rhi::Backend::Vulkan;
    bool                  enableDebug = false;
    // Root of the compiled shader tree. What lives below it is decided by the
    // active backend, not here.
    std::filesystem::path shaderRoot  = "shaders";
    // Stop after this many presented frames; 0 runs until the window closes.
    // Exists so an automated run can exercise the full startup/shutdown path.
    uint64_t              maxFrames   = 0;

    // Verification. When a dump or comparison is requested the animation clock
    // is pinned, otherwise two runs could never agree.
    std::filesystem::path dumpPath;
    std::filesystem::path comparePath;
    float                 fixedTime = 1.0f;
    float                 tolerance = 1.0f / 255.0f;
};

// Owns the window, the device and the milestone-1 compute probe pass.
class Application {
public:
    explicit Application(const AppConfig& config);
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    // Returns false if a requested pixel comparison failed.
    [[nodiscard]] bool run();

private:
    void renderFrame();
    void resizeRenderTarget(rhi::Extent2D extent);
    void destroyRenderTarget();

    void               writeDump(const std::filesystem::path& path);
    [[nodiscard]] bool compareAgainst(const std::filesystem::path& path);

    // Mirrors the push constant block in shaders/raymarch_probe.comp.
    struct ProbePushConstants {
        float resolution[2] = {0.0f, 0.0f};
        float time          = 0.0f;
    };

    platform::Window             m_window;
    std::string                  m_shaderRoot;
    std::unique_ptr<rhi::Device> m_device;

    rhi::ShaderHandle   m_shader       = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_pipeline     = rhi::PipelineHandle::Invalid;
    rhi::TextureHandle  m_renderTarget = rhi::TextureHandle::Invalid;
    rhi::Extent2D       m_targetExtent = {};

    uint64_t m_maxFrames   = 0;
    uint64_t m_framesDrawn = 0;

    std::filesystem::path m_dumpPath;
    std::filesystem::path m_comparePath;
    bool                  m_pinTime   = false;
    float                 m_fixedTime = 1.0f;
    float                 m_tolerance = 1.0f / 255.0f;
};

} // namespace engine
