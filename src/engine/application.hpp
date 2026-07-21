#pragma once

// Engine layer. Sees rhi.hpp and the platform layer — never a backend.

#include "engine/camera.hpp"
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

    // Mirrors the push constant block in shaders/raymarch.comp.
    //
    // Every vector is a vec4 with a scalar tucked into .w. std140 aligns a
    // vec3 to 16 bytes anyway, so this costs nothing and removes any chance of
    // the two shader variants disagreeing about offsets.
    struct SceneUniforms {
        float   cameraPosition[4] = {}; // xyz, w = tan(fovY / 2)
        float   cameraRight[4]    = {}; // xyz, w = aspect ratio
        float   cameraUp[4]       = {}; // xyz, w = time
        float   cameraForward[4]  = {}; // xyz
        float   resolution[2]     = {};
        int32_t primitiveCount    = 0;
        float   padding           = 0.0f;
    };
    static_assert(sizeof(SceneUniforms) == 80, "push constant block must stay under 128 bytes");

    platform::Window             m_window;
    std::string                  m_shaderRoot;
    std::unique_ptr<rhi::Device> m_device;

    rhi::ShaderHandle   m_shader       = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_pipeline     = rhi::PipelineHandle::Invalid;
    rhi::TextureHandle  m_renderTarget = rhi::TextureHandle::Invalid;
    rhi::Extent2D       m_targetExtent = {};

    FlyCamera m_camera;
    double    m_lastFrameTime = 0.0;

    uint64_t m_maxFrames   = 0;
    uint64_t m_framesDrawn = 0;

    std::filesystem::path m_dumpPath;
    std::filesystem::path m_comparePath;
    bool                  m_pinTime   = false;
    float                 m_fixedTime = 1.0f;
    float                 m_tolerance = 1.0f / 255.0f;
};

} // namespace engine
