#pragma once

// Engine layer. Sees rhi.hpp and the platform layer — never a backend.

#include "engine/animation.hpp"
#include "engine/brick.hpp"
#include "engine/camera.hpp"
#include "engine/editor.hpp"
#include "engine/fluid_sim.hpp"
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
    // Switch the water on at startup (--fluid). Off by default: the solver
    // allocates nothing and costs nothing until it is enabled.
    bool                  fluid       = false;
    // Start the animation clip playing. Without it a headless run holds the
    // playhead at zero, so nothing moves and an animated scene dumps its rest
    // pose. Looping, so a long run keeps producing motion.
    bool                  play        = false;
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
    // Seconds the simulated world advances this frame: the wall clock normally,
    // a fixed step when the run is pinned. Everything that integrates — the
    // animation playhead, the obstacle velocity the water feels, the fluid
    // substeps — reads it from here, so a pinned run is a function of the frame
    // index and two runs of it agree.
    [[nodiscard]] float simulationDelta() const;
    void resizeRenderTarget(rhi::Extent2D extent);
    void destroyRenderTarget();

    void createBrickResources();
    void recordReference(rhi::CommandList& cmd);
    void recordBrick(rhi::CommandList& cmd);
    void recordBake(rhi::CommandList& cmd);
    void recordPick(rhi::CommandList& cmd);
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
        int32_t gridRes           = brick::kDefaultGridRes; // must match the value the bake used
    };
    static_assert(sizeof(BrickUniforms) == 124, "brick push constants must stay under 128 bytes");

    // Mirrors the push constant block in the bake shaders.
    struct BakeUniforms {
        float   aabbMin[4] = {};
        float   aabbMax[4] = {};
        int32_t control[4] = {}; // x = primitiveCount, y = pool capacity
    };
    static_assert(sizeof(BakeUniforms) == 48, "bake push constants must stay under 128 bytes");

    // Mirrors the push constant block in pick.comp: the ray to trace and the
    // primitive count. The result (the hit primitive index, or -1) comes back in
    // m_pickBuffer.
    struct PickUniforms {
        float   rayOrigin[4] = {}; // xyz
        float   rayDir[4]    = {}; // xyz, normalised
        int32_t control[4]   = {}; // x = primitiveCount
    };
    static_assert(sizeof(PickUniforms) == 48, "pick push constants must stay under 128 bytes");

    // The lighting block both marchers read from a storage buffer (slot 5).
    // vec4-packed to match the std430 layout of LightingParams in shading.glsl
    // byte for byte; the scalar riders in .w carry the light intensities and the
    // ambient strength. Assembled from m_lighting whenever the lighting changes.
    struct GpuLighting {
        float keyDir[4]        = {}; // xyz direction toward the light, w = intensity
        float keyColour[4]     = {};
        float pointPos[4]      = {}; // xyz world position, w = intensity
        float pointColour[4]   = {};
        float ambientSky[4]    = {}; // rgb, w = ambient strength
        float ambientGround[4] = {};
        float bgHorizon[4]     = {};
        float bgZenith[4]      = {};
    };
    static_assert(sizeof(GpuLighting) == 128,
                  "lighting buffer must match the std430 block in shading.glsl");

    [[nodiscard]] SceneUniforms cameraUniforms() const;
    void                        uploadLighting();

    platform::Window             m_window;
    std::string                  m_shaderRoot;
    std::string                  m_iniPath; // ImGui layout file; ImGui holds the pointer
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

    // Viewport picking: a one-invocation pass that marches the click ray and
    // writes the hit primitive index (or -1) for readback.
    rhi::ShaderHandle   m_pickShader   = rhi::ShaderHandle::Invalid;
    rhi::PipelineHandle m_pickPipeline = rhi::PipelineHandle::Invalid;
    rhi::BufferHandle   m_pickBuffer   = rhi::BufferHandle::Invalid;
    bool                m_pickPending  = false; // a pick was requested this frame
    PickUniforms        m_pickUniforms{};       // the ray to trace when it is

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

    // The grid resolution the cell buffer currently holds. Set at bake time; the
    // marcher reads *this*, not the editor's pending value, so the cell indexing
    // always matches the bake even while the resolution slider is mid-drag.
    int32_t           m_bakedGridRes = brick::kDefaultGridRes;

    RendererMode   m_renderer     = RendererMode::Brick;
    int32_t        m_debugView    = 0;
    bool           m_debugKeyHeld = false;
    RenderSettings m_render;       // exposure + reflection samples, edited in the panel

    // Lighting, edited in the panel and mirrored into a small storage buffer the
    // marchers read (slot 5). No re-bake on change — bricks store distance, not
    // shading, so the light buffer is consulted fresh every frame.
    LightingSettings  m_lighting;
    rhi::BufferHandle m_lightingBuffer = rhi::BufferHandle::Invalid;

    // Keyframe animation: the clip (per-object pose tracks) and playback state,
    // both edited by the Timeline panel. The playhead is advanced in the main
    // loop and the sampled pose written into m_scene each frame while playing or
    // scrubbing. m_lastAnimTime gates re-sampling to when the time actually moved.
    AnimationClip  m_animClip;
    AnimationState m_animState;
    float          m_lastAnimTime = -1.0f;

    // Editor UI.
    Editor                m_editor;
    bool                  m_uiEnabled = false;
    std::filesystem::path m_sceneDir; // where scenes are saved/loaded from
    float                 m_smoothedFrametime = 0.0f; // for a steady FPS readout

    // The water. Owns its field buffers and its nine compute passes; the engine
    // owns only the settings, when to step, and where the two buffers the
    // marchers read are bound (slots 8 and 11). Nothing is allocated until the
    // fluid is switched on.
    FluidSim        m_fluid;
    fluid::Settings m_fluidSettings;
    fluid::Stats    m_fluidStats;
    // Frames since the last diagnostics readback. The readback stalls the
    // device, exactly like the brick bake's, so it runs on an interval rather
    // than every frame — the figures move slowly enough for that to be honest.
    uint32_t        m_fluidStatsAge = 0;

    FlyCamera m_camera;
    double    m_lastFrameTime = 0.0;
    // The frame delta the solver is stepped with, captured by the main loop.
    float     m_frameDelta    = 0.0f;

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
