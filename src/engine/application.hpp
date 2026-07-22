#pragma once

// Engine layer. Sees rhi.hpp and the platform layer — never a backend.

#include "engine/brick.hpp"
#include "engine/camera.hpp"
#include "engine/scene.hpp"
#include "platform/window.hpp"
#include "rhi/rhi.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace engine {

// Which renderer produces the frame. The reference is the brute-force marcher
// that defines correct; the brick renderer is the accelerated path validated
// against it. Switchable at runtime.
enum class RendererMode {
    Brick,     // accelerated: marches the baked brick structure
    Reference, // brute force: evaluates the whole edit list per step
};

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

    RendererMode          renderer    = RendererMode::Brick;
    // 0 = shaded, 1 = step-count heat, 2 = brick/empty tint. Brick renderer only.
    int32_t               debugView   = 0;

    // Verification. When a dump or comparison is requested the animation clock
    // is pinned, otherwise two runs could never agree.
    std::filesystem::path dumpPath;
    std::filesystem::path comparePath;
    float                 fixedTime = 1.0f;
    float                 tolerance = 1.0f / 255.0f;
    // Fraction of components allowed to exceed `tolerance` before --compare
    // fails. The reference/brick A/B differs at curved silhouettes (trilinear
    // filter error) and at the AABB ground boundary — a handful of pixels, not
    // a max-error story — so the acceptance test is an outlier count, not a max.
    // Default 0 keeps the strict cross-backend behaviour unchanged.
    float                 maxOutlierFraction = 0.0f;
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

    void createBrickResources();
    void recordReference(rhi::CommandList& cmd);
    void recordBrick(rhi::CommandList& cmd);
    void recordBake(rhi::CommandList& cmd);
    void reportBakeStats();
    void handleRendererInput();

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

    // Mirrors the push constant block in raymarch_brick.comp: the camera block
    // above plus the debug view selector and the grid AABB.
    struct BrickUniforms {
        float   cameraPosition[4] = {};
        float   cameraRight[4]    = {};
        float   cameraUp[4]       = {};
        float   cameraForward[4]  = {};
        float   resolution[2]     = {};
        int32_t primitiveCount    = 0;
        int32_t debugMode         = 0;
        float   aabbMin[4]        = {};
        float   aabbMax[4]        = {};
    };
    static_assert(sizeof(BrickUniforms) == 112, "brick push constants must stay under 128 bytes");

    // Mirrors the push constant block in the bake shaders.
    struct BakeUniforms {
        float   aabbMin[4] = {};
        float   aabbMax[4] = {};
        int32_t control[4] = {}; // x = primitiveCount, y = pool capacity
    };
    static_assert(sizeof(BakeUniforms) == 48, "bake push constants must stay under 128 bytes");

    [[nodiscard]] SceneUniforms cameraUniforms() const;

    platform::Window             m_window;
    std::string                  m_shaderRoot;
    std::unique_ptr<rhi::Device> m_device;

    // Reference renderer (brute-force marcher over the edit list).
    rhi::ShaderHandle   m_refShader   = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_refPipeline = rhi::PipelineHandle::Invalid;

    // Brick renderer and its bake passes.
    rhi::ShaderHandle   m_brickShader      = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_brickPipeline    = rhi::PipelineHandle::Invalid;
    rhi::ShaderHandle   m_classifyShader   = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_classifyPipeline = rhi::PipelineHandle::Invalid;
    rhi::ShaderHandle   m_fillShader       = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_fillPipeline     = rhi::PipelineHandle::Invalid;

    rhi::TextureHandle  m_renderTarget = rhi::TextureHandle::Invalid;
    rhi::Extent2D       m_targetExtent = {};

    // The edit list and its GPU mirror. Uploaded once: the scene is static for
    // this milestone, and re-uploading is what an editor would add, not the
    // renderer.
    std::vector<GpuPrimitive> m_scene;
    rhi::BufferHandle         m_sceneBuffer = rhi::BufferHandle::Invalid;

    // The baked structure: dense top-level index, sparse brick pool, and the
    // bump-allocator / diagnostics block.
    rhi::BufferHandle m_cellsBuffer  = rhi::BufferHandle::Invalid;
    rhi::BufferHandle m_bricksBuffer = rhi::BufferHandle::Invalid;
    rhi::BufferHandle m_statsBuffer  = rhi::BufferHandle::Invalid;
    bool              m_needBake     = true; // record the bake on the next frame
    bool              m_bakePending  = false; // stats not yet read back

    RendererMode m_renderer     = RendererMode::Brick;
    int32_t      m_debugView    = 0;
    bool         m_debugKeyHeld = false;

    FlyCamera m_camera;
    double    m_lastFrameTime = 0.0;

    uint64_t m_maxFrames   = 0;
    uint64_t m_framesDrawn = 0;

    std::filesystem::path m_dumpPath;
    std::filesystem::path m_comparePath;
    bool                  m_pinTime            = false;
    float                 m_fixedTime          = 1.0f;
    float                 m_tolerance          = 1.0f / 255.0f;
    float                 m_maxOutlierFraction = 0.0f;
};

} // namespace engine
