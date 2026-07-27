#pragma once

// Engine layer. Sees rhi.hpp and the platform layer — never a backend.

#include "engine/brick.hpp"
#include "engine/camera.hpp"
#include "engine/editor.hpp"
#include "engine/render_mode.hpp"
#include "engine/scene.hpp"
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

    RendererMode          renderer    = RendererMode::Brick;
    // 0 = shaded, 1 = step-count heat, 2 = brick/empty tint. Brick renderer only.
    int32_t               debugView   = 0;
    // Show the Dear ImGui editor panel. Forced off for pinned verification runs
    // (--dump / --compare) so the UI never lands in a compared image.
    bool                  enableUi    = true;
    // Load this scene file at startup instead of the built-in scene. Lets a
    // saved arrangement be reproduced headlessly, e.g. for --dump / --compare.
    std::filesystem::path scenePath;

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
    void uploadScene();

    void initUi();
    void shutdownUi();
    void buildUi(); // issues the editor's ImGui:: calls and reacts to its actions

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
        float   exposure          = 1.0f;
        int32_t reflectionSamples = 4;   // glossy reflection rays per hit
        float   padding           = 0.0f;
    };
    static_assert(sizeof(SceneUniforms) == 88, "push constant block must stay under 128 bytes");

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
        float   exposure          = 1.0f;
        int32_t reflectionSamples = 4;
    };
    static_assert(sizeof(BrickUniforms) == 120, "brick push constants must stay under 128 bytes");

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

    // The edit list — the single source of truth — and its GPU mirror. The
    // buffer is allocated at kMaxPrimitives capacity so the editor can add
    // primitives without reallocating it; only the active prefix is uploaded
    // and only primitiveCount of it is evaluated.
    // Raised from 256 for the L-system vegetation generator: a tree easily runs
    // to hundreds of segments. The reference renderer slows linearly (it
    // evaluates every primitive per march step), but the brick renderer bakes
    // once; the editor's generator shows a live count and blocks a generate that
    // would overflow this budget.
    static constexpr uint32_t     kMaxPrimitives = 2048;
    std::vector<GpuPrimitive>     m_scene;
    rhi::BufferHandle             m_sceneBuffer = rhi::BufferHandle::Invalid;

    // The baked structure: dense top-level index, sparse brick pool, and the
    // bump-allocator / diagnostics block.
    rhi::BufferHandle m_cellsBuffer  = rhi::BufferHandle::Invalid;
    rhi::BufferHandle m_bricksBuffer = rhi::BufferHandle::Invalid;
    rhi::BufferHandle m_statsBuffer  = rhi::BufferHandle::Invalid;
    bool              m_needBake     = true;  // record the bake on the next frame
    bool              m_bakePending  = false; // stats/time not yet read back
    bool              m_measureBake  = true;  // time the next re-bake (first bake + committed edits)
    float             m_lastBakeMs   = 0.0f;
    bool              m_haveBake     = false; // a re-bake has been timed at least once
    double            m_bakeStart    = 0.0;   // wall clock at the timed re-bake's submit

    RendererMode   m_renderer     = RendererMode::Brick;
    int32_t        m_debugView    = 0;
    bool           m_debugKeyHeld = false;
    RenderSettings m_render;       // exposure + reflection samples, edited in the panel

    // Editor UI.
    Editor                m_editor;
    bool                  m_uiEnabled = false;
    std::filesystem::path m_sceneDir; // where scenes are saved/loaded from
    float                 m_smoothedFrametime = 0.0f; // for a steady FPS readout

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
