#include "engine/application.hpp"

#include "engine/embedded_font.hpp"
#include "engine/mat4.hpp"
#include "engine/scene_io.hpp"
#include "engine/ui_theme.hpp"

// imgui.h must precede ImGuizmo.h (the gizmo header relies on it being included).
#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace engine {
namespace {

// Local workgroup size of raymarch_probe.comp.
constexpr uint32_t kWorkgroupSize = 16;

constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) {
    return (value + divisor - 1) / divisor;
}

template <typename T>
std::span<const std::byte> asBytes(const T& value) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&value), sizeof(T));
}

} // namespace

Application::Application(const AppConfig& config)
    // The window must be created for whatever the chosen backend needs: a
    // client-API context cannot be attached after the fact. The engine only
    // forwards the answer — it never interprets it.
    : m_window({.width    = config.width,
                .height   = config.height,
                .title    = config.title,
                .graphics = rhi::windowRequirements(config.backend, config.enableDebug)}),
      m_shaderRoot(config.shaderRoot.string()),
      m_renderer(config.renderer),
      m_debugView(config.debugView),
      m_maxFrames(config.maxFrames),
      m_dumpPath(config.dumpPath),
      m_comparePath(config.comparePath),
      m_pinTime(!config.dumpPath.empty() || !config.comparePath.empty()),
      m_fixedTime(config.fixedTime),
      m_tolerance(config.tolerance),
      m_maxOutlierFraction(config.maxOutlierFraction) {
    const platform::Extent2D framebuffer = m_window.framebufferSize();

    m_device = rhi::createDevice(config.backend,
                                 {
                                     .nativeWindowHandle = m_window.nativeHandle(),
                                     .framebufferSize    = {framebuffer.width, framebuffer.height},
                                     .enableDebug        = config.enableDebug,
                                     .applicationName    = config.title.c_str(),
                                     .shaderRoot         = m_shaderRoot.c_str(),
                                 });

    // The scene lives in a storage buffer that both renderers and the bake read
    // at slot 1. It is allocated at full capacity so the editor can add
    // primitives without reallocating a GPU resource mid-frame; only the active
    // prefix is uploaded and only primitiveCount of it is ever read.
    m_scene =
        config.scenePath.empty() ? buildScene() : loadScene(config.scenePath, &m_animClip);
    if (m_scene.size() > kMaxPrimitives) {
        m_scene.resize(kMaxPrimitives);
    }
    m_sceneBuffer = m_device->createBuffer({
        .size      = static_cast<uint64_t>(kMaxPrimitives) * sizeof(GpuPrimitive),
        .usage     = rhi::BufferUsage::Storage,
        .access    = rhi::MemoryAccess::CpuToGpu,
        .debugName = "scene_primitives",
    });
    uploadScene();

    // Lighting block (slot 5), shared by both marchers. Tiny and host-visible so
    // an editor tweak re-uploads it cheaply; seeded now with the defaults.
    m_lightingBuffer = m_device->createBuffer({
        .size      = sizeof(GpuLighting),
        .usage     = rhi::BufferUsage::Storage,
        .access    = rhi::MemoryAccess::CpuToGpu,
        .debugName = "lighting",
    });
    uploadLighting();

    // --- reference renderer: render target (0) + edit list (1) + lighting (5) -
    m_refShader = m_device->createShader("raymarch");
    constexpr std::array<rhi::BindingDesc, 3> refBindings{
        rhi::BindingDesc{.slot = 0, .type = rhi::BindingType::StorageTexture},
        rhi::BindingDesc{.slot = 1, .type = rhi::BindingType::StorageBuffer},
        rhi::BindingDesc{.slot = 5, .type = rhi::BindingType::StorageBuffer}};
    m_refPipeline = m_device->createComputePipeline({
        .cs               = m_refShader,
        .pushConstantSize = sizeof(SceneUniforms),
        .bindings         = refBindings,
        .debugName        = "raymarch_reference",
    });

    createBrickResources();

    resizeRenderTarget(m_device->swapchainExtent());

    // Scene files live beside the executable, next to the staged shaders.
    m_sceneDir = std::filesystem::path(m_shaderRoot).parent_path() / "scenes";

    // The editor overlay, unless this is a pinned verification run — a UI drawn
    // into the frame would land in the compared image.
    if (config.enableUi && !m_pinTime) {
        initUi();
    }
}

void Application::uploadScene() {
    if (m_scene.size() > kMaxPrimitives) {
        m_scene.resize(kMaxPrimitives); // capacity guard; the editor also blocks Add at the cap
    }
    m_device->updateBuffer(
        m_sceneBuffer,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(m_scene.data()),
                                   m_scene.size() * sizeof(GpuPrimitive)));
}

void Application::uploadLighting() {
    constexpr float kPi = 3.14159265358979f;
    const auto      copy3 = [](float (&dst)[4], const float (&src)[3]) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
    };

    // Azimuth/elevation -> a unit direction pointing toward the key light. This
    // convention (x = cos·cos, y = sin(el), z = cos·sin) is what the default
    // angles were chosen to reproduce, so the out-of-the-box look is unchanged.
    const float az = m_lighting.keyAzimuthDeg * (kPi / 180.0f);
    const float el = m_lighting.keyElevationDeg * (kPi / 180.0f);
    const float ce = std::cos(el);

    GpuLighting g{};
    g.keyDir[0] = ce * std::cos(az);
    g.keyDir[1] = std::sin(el);
    g.keyDir[2] = ce * std::sin(az);
    g.keyDir[3] = m_lighting.keyIntensity;
    copy3(g.keyColour, m_lighting.keyColour);

    copy3(g.pointPos, m_lighting.pointPosition);
    g.pointPos[3] = m_lighting.pointIntensity;
    copy3(g.pointColour, m_lighting.pointColour);

    copy3(g.ambientSky, m_lighting.ambientSky);
    g.ambientSky[3] = m_lighting.ambientStrength;
    copy3(g.ambientGround, m_lighting.ambientGround);

    copy3(g.bgHorizon, m_lighting.bgHorizon);
    copy3(g.bgZenith, m_lighting.bgZenith);

    m_device->updateBuffer(m_lightingBuffer, asBytes(g));
}

void Application::createBrickResources() {
    // Dense top-level index, sized for the maximum resolution so retuning the
    // grid never reallocates it. GpuOnly, CopySrc so it can be inspected.
    m_cellsBuffer = m_device->createBuffer({
        .size      = static_cast<uint64_t>(brick::kMaxCellCount) * sizeof(brick::Cell),
        .usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc,
        .access    = rhi::MemoryAccess::GpuOnly,
        .debugName = "brick_cells",
    });
    // Sparse brick pool.
    m_bricksBuffer = m_device->createBuffer({
        .size = static_cast<uint64_t>(brick::kPoolCapacity) * brick::kBrickVoxels * sizeof(float),
        .usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc,
        .access    = rhi::MemoryAccess::GpuOnly,
        .debugName = "brick_pool",
    });
    // The bump allocator / diagnostics block. Device-local: the atomics want it
    // there, it is zeroed on the GPU by the bake (clearBuffer, hence CopyDst),
    // and the readback stages it out through a host copy (hence CopySrc).
    m_statsBuffer = m_device->createBuffer({
        .size  = sizeof(brick::BakeStats),
        .usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst,
        .access    = rhi::MemoryAccess::GpuOnly,
        .debugName = "brick_bake_stats",
    });

    m_classifyShader = m_device->createShader("bake_classify");
    constexpr std::array<rhi::BindingDesc, 3> classifyBindings{
        rhi::BindingDesc{.slot = 1, .type = rhi::BindingType::StorageBuffer}, // scene
        rhi::BindingDesc{.slot = 2, .type = rhi::BindingType::StorageBuffer}, // cells
        rhi::BindingDesc{.slot = 4, .type = rhi::BindingType::StorageBuffer}, // stats
    };
    m_classifyPipeline = m_device->createComputePipeline({
        .cs               = m_classifyShader,
        .pushConstantSize = sizeof(BakeUniforms),
        .bindings         = classifyBindings,
        .debugName        = "bake_classify",
    });

    m_fillShader = m_device->createShader("bake_fill");
    constexpr std::array<rhi::BindingDesc, 3> fillBindings{
        rhi::BindingDesc{.slot = 1, .type = rhi::BindingType::StorageBuffer}, // scene
        rhi::BindingDesc{.slot = 2, .type = rhi::BindingType::StorageBuffer}, // cells
        rhi::BindingDesc{.slot = 3, .type = rhi::BindingType::StorageBuffer}, // bricks
    };
    m_fillPipeline = m_device->createComputePipeline({
        .cs               = m_fillShader,
        .pushConstantSize = sizeof(BakeUniforms),
        .bindings         = fillBindings,
        .debugName        = "bake_fill",
    });

    m_brickShader = m_device->createShader("raymarch_brick");
    constexpr std::array<rhi::BindingDesc, 5> brickBindings{
        rhi::BindingDesc{.slot = 0, .type = rhi::BindingType::StorageTexture}, // output
        rhi::BindingDesc{.slot = 1, .type = rhi::BindingType::StorageBuffer},  // scene
        rhi::BindingDesc{.slot = 2, .type = rhi::BindingType::StorageBuffer},  // cells
        rhi::BindingDesc{.slot = 3, .type = rhi::BindingType::StorageBuffer},  // bricks
        rhi::BindingDesc{.slot = 5, .type = rhi::BindingType::StorageBuffer},  // lighting
    };
    m_brickPipeline = m_device->createComputePipeline({
        .cs               = m_brickShader,
        .pushConstantSize = sizeof(BrickUniforms),
        .bindings         = brickBindings,
        .debugName        = "raymarch_brick",
    });

    // --- viewport picking: edit list (slot 1) -> hit index (slot 6) ---------
    m_pickBuffer = m_device->createBuffer({
        .size      = sizeof(int32_t),
        .usage     = rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc,
        .access    = rhi::MemoryAccess::GpuOnly,
        .debugName = "pick_result",
    });
    m_pickShader = m_device->createShader("pick");
    constexpr std::array<rhi::BindingDesc, 2> pickBindings{
        rhi::BindingDesc{.slot = 1, .type = rhi::BindingType::StorageBuffer},  // scene
        rhi::BindingDesc{.slot = 6, .type = rhi::BindingType::StorageBuffer}}; // result
    m_pickPipeline = m_device->createComputePipeline({
        .cs               = m_pickShader,
        .pushConstantSize = sizeof(PickUniforms),
        .bindings         = pickBindings,
        .debugName        = "pick",
    });
}

Application::~Application() {
    // UI first, in the one order ImGui's shared context allows: the backend
    // (which idles the GPU internally), then the platform, then the context.
    shutdownUi();

    // No idle wait for the rest: destruction is safe to request at any time, and
    // a backend that can still have work in flight defers the release itself.
    destroyRenderTarget();
    for (rhi::BufferHandle buffer : {m_sceneBuffer, m_cellsBuffer, m_bricksBuffer, m_statsBuffer}) {
        if (rhi::isValid(buffer)) {
            m_device->destroy(buffer);
        }
    }
    for (rhi::PipelineHandle pipeline :
         {m_refPipeline, m_brickPipeline, m_classifyPipeline, m_fillPipeline}) {
        if (rhi::isValid(pipeline)) {
            m_device->destroy(pipeline);
        }
    }
    for (rhi::ShaderHandle shader :
         {m_refShader, m_brickShader, m_classifyShader, m_fillShader}) {
        if (rhi::isValid(shader)) {
            m_device->destroy(shader);
        }
    }
}

void Application::resizeRenderTarget(rhi::Extent2D extent) {
    if (extent.width == 0 || extent.height == 0 || extent == m_targetExtent) {
        return;
    }
    destroyRenderTarget();

    m_renderTarget = m_device->createTexture({
        .width     = extent.width,
        .height    = extent.height,
        .depth     = 1,
        .format    = rhi::Format::RGBA16Float,
        .usage     = rhi::TextureUsage::Storage | rhi::TextureUsage::CopySrc,
        .debugName = "probe_target",
    });
    m_targetExtent = extent;
}

void Application::destroyRenderTarget() {
    if (rhi::isValid(m_renderTarget)) {
        m_device->destroy(m_renderTarget);
        m_renderTarget = rhi::TextureHandle::Invalid;
        m_targetExtent = {};
    }
}

// Runtime switch between the two renderers, and a cycle through the brick
// renderer's debug views. Edge-triggered would need previous-frame state; a
// level check is enough here because a held key just re-selects the same mode.
void Application::handleRendererInput() {
    const platform::InputState& in = m_window.input();
    if (in.isDown(platform::Key::Num1) && m_renderer != RendererMode::Reference) {
        m_renderer = RendererMode::Reference;
        std::fprintf(stderr, "[renderer] reference (brute force)\n");
    }
    if (in.isDown(platform::Key::Num2) && m_renderer != RendererMode::Brick) {
        m_renderer = RendererMode::Brick;
        std::fprintf(stderr, "[renderer] brick\n");
    }
    if (in.isDown(platform::Key::Num3)) {
        // Debounced by requiring release between presses.
        if (!m_debugKeyHeld) {
            m_debugView    = (m_debugView + 1) % 3;
            m_debugKeyHeld = true;
            static const char* names[] = {"shaded", "step heat", "brick tint"};
            std::fprintf(stderr, "[renderer] brick debug view: %s\n", names[m_debugView]);
        }
    } else {
        m_debugKeyHeld = false;
    }
}

bool Application::run() {
    while (!m_window.shouldClose()) {
        m_window.pollEvents();

        if (m_window.input().isDown(platform::Key::Escape)) {
            m_window.requestClose();
            continue;
        }

        if (m_window.consumeResized()) {
            const platform::Extent2D size = m_window.framebufferSize();
            m_device->onResize(size.width, size.height);
        }

        if (m_window.isMinimised()) {
            // Nothing to present into; idle instead of spinning.
            m_window.waitEvents();
            m_lastFrameTime = platform::timeSeconds();
            continue;
        }

        const double now = platform::timeSeconds();
        // Clamped so a stall — a breakpoint, a swapchain rebuild — cannot
        // teleport the camera across the scene on the frame after it.
        const float deltaSeconds =
            std::clamp(static_cast<float>(now - m_lastFrameTime), 0.0f, 0.1f);
        m_lastFrameTime = now;
        // Exponential smoothing so the FPS readout does not flicker.
        m_smoothedFrametime = m_smoothedFrametime > 0.0f
                                  ? m_smoothedFrametime + 0.1f * (deltaSeconds - m_smoothedFrametime)
                                  : deltaSeconds;

        // When the pointer or keyboard is over the panel, the editor owns the
        // input — do not also fly the camera or fire the renderer hotkeys.
        const bool uiCaptures =
            m_uiEnabled && (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard);

        // A pinned run has to be reproducible, and a free-flying camera is
        // not. Same reasoning as the pinned clock. Renderer switching is frozen
        // too, so a comparison run renders exactly the requested path.
        // While a camera track is playing back, it owns the camera — free-fly
        // input is suppressed so it does not fight the animation.
        const bool cameraDriven = m_animState.playing && hasCameraTrack(m_animClip);
        if (!m_pinTime && !uiCaptures && !cameraDriven) {
            m_camera.update(m_window.input(), deltaSeconds);
            handleRendererInput();
        }

        // Advance the animation playhead, but freeze it while the user is mid-edit
        // (a gizmo drag or an active field) so playback neither fights the edit nor
        // scrambles the pre-edit undo snapshot. Queried from the previous frame's
        // ImGui state, which is exactly what we want here.
        const bool editing =
            m_uiEnabled && (ImGuizmo::IsUsing() || ImGui::IsAnyItemActive());
        if (m_animState.playing && !editing && m_animClip.duration > 0.0f) {
            m_animState.time += deltaSeconds * m_animState.speed;
            if (m_animState.time >= m_animClip.duration) {
                if (m_animState.loop) {
                    m_animState.time = std::fmod(m_animState.time, m_animClip.duration);
                } else {
                    m_animState.time    = m_animClip.duration;
                    m_animState.playing = false;
                }
            }
        }

        renderFrame();
        ++m_framesDrawn;

        if (m_maxFrames != 0 && m_framesDrawn >= m_maxFrames) {
            m_window.requestClose();
        }
    }

    if (m_framesDrawn == 0) {
        return true;
    }
    if (!m_dumpPath.empty()) {
        writeDump(m_dumpPath);
    }
    if (!m_comparePath.empty()) {
        return compareAgainst(m_comparePath);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Editor UI
//
// The engine owns the ImGui context and issues only backend-neutral ImGui::
// calls; the backend (behind the seam) owns the render backend, the platform
// layer the GLFW input backend. A backend without an overlay reports so from
// initUi and the engine simply runs without one.
// ---------------------------------------------------------------------------
void Application::initUi() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();

    // Dockable panels. Multi-viewport stays off, so the platform/render backends
    // need no viewport support.
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Persist the docking layout in the user's config dir (never the project). The
    // string must outlive the context, so it lives on the app; ImGui keeps only
    // the pointer. Empty path -> no persistence.
    m_iniPath      = editorIniPath();
    io.IniFilename = m_iniPath.empty() ? nullptr : m_iniPath.c_str();

    // Everything scales off the monitor's DPI so the panels stay the same
    // apparent size on a 96-DPI laptop and a 4K/200% display alike.
    const float dpi = m_window.contentScale();

    applyEditorTheme();
    ImGui::GetStyle().ScaleAllSizes(dpi);

    // Karla, embedded so there is no runtime file dependency, rasterised at the
    // DPI-scaled pixel size for crisp text at any density. FontDataOwnedByAtlas
    // is false: the glyph bytes are a static array ImGui must not try to free.
    ImFontConfig fontCfg;
    fontCfg.FontDataOwnedByAtlas = false;
    fontCfg.OversampleH          = 2;
    fontCfg.OversampleV          = 2;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kKarlaRegularTtf),
                                   static_cast<int>(kKarlaRegularTtfSize),
                                   std::round(16.0f * dpi), &fontCfg);

    if (!m_device->initUi()) {
        // This backend has no overlay (the OpenGL backend today). Run without.
        ImGui::DestroyContext();
        m_uiEnabled = false;
        return;
    }
    m_window.initUi();
    m_uiEnabled = true;
}

void Application::shutdownUi() {
    if (!m_uiEnabled) {
        return;
    }
    m_device->shutdownUi(); // idles the GPU, then tears down the render backend
    m_window.shutdownUi();
    ImGui::DestroyContext();
    m_uiEnabled = false;
}

void Application::buildUi() {
    EditorStats stats;
    stats.fps            = m_smoothedFrametime > 0.0f ? 1.0f / m_smoothedFrametime : 0.0f;
    stats.frametimeMs    = m_smoothedFrametime * 1000.0f;
    stats.rendererName   = m_renderer == RendererMode::Brick ? "brick" : "reference (brute force)";
    stats.primitiveCount = static_cast<int>(m_scene.size());
    stats.maxPrimitives  = static_cast<int>(kMaxPrimitives);
    stats.lastBakeMs     = m_lastBakeMs;
    stats.haveBake       = m_haveBake;

    // The camera the editor needs for the gizmo (view/projection) and the pick
    // ray (basis + fov). View/projection are reconstructed to match the marcher's
    // own ray generation exactly, so the gizmo lands on the rendered object.
    const Vec3  position = m_camera.position();
    const Vec3  right    = m_camera.right();
    const Vec3  up       = m_camera.up();
    const Vec3  forward  = m_camera.forward();
    const float aspect   = static_cast<float>(m_targetExtent.width) /
                         static_cast<float>(m_targetExtent.height);
    const float fov = m_camera.verticalFovRadians();

    ViewportCamera vpCam;
    vpCam.view       = viewFromBasis(right, up, forward, position);
    vpCam.proj       = perspective(fov, aspect, 0.05f, 500.0f);
    vpCam.position   = position;
    vpCam.right      = right;
    vpCam.up         = up;
    vpCam.forward    = forward;
    vpCam.tanHalfFov = std::tan(fov * 0.5f);
    vpCam.aspect     = aspect;

    const EditorActions actions =
        m_editor.draw(m_scene, m_renderer, m_render, m_lighting, m_animClip, m_animState, vpCam,
                      /*brickAvailable=*/true, stats, m_sceneDir);

    if (actions.pickRequested) {
        // Stage the ray; the pick pass is recorded into this frame and read back
        // after present (a click is rare, so the readback stall is fine).
        m_pickUniforms = PickUniforms{};
        for (int i = 0; i < 3; ++i) {
            m_pickUniforms.rayOrigin[i] = actions.pickRayOrigin[i];
            m_pickUniforms.rayDir[i]    = actions.pickRayDir[i];
        }
        m_pickUniforms.control[0] = static_cast<int32_t>(m_scene.size());
        m_pickPending             = true;
    }

    if (actions.sceneChanged || actions.bakeMeasure) {
        // The edit list is the single source of truth; push it to the GPU and
        // ask for a re-bake. The reference renderer needs nothing more — it
        // re-reads the list every frame.
        uploadScene();
        m_needBake = true;
    }
    if (actions.rebake) {
        // The structure needs rebuilding but the edit list did not change (a
        // grid-resolution retune): re-bake without re-uploading the scene.
        m_needBake = true;
    }
    if (actions.lightingChanged) {
        // Lighting lives in its own buffer the marchers read every frame; just
        // re-upload it. No re-bake — the bricks hold distance, not shading.
        uploadLighting();
    }
    if (actions.bakeMeasure) {
        // A committed edit (a finished drag, an add/delete, undo/redo, a load):
        // time the resulting re-bake so the panel can report it. Live drag
        // frames re-bake too, but unmeasured, so dragging stays smooth.
        m_measureBake = true;
    }
}

void Application::renderFrame() {
    // Build the editor UI before recording: an edit this frame takes effect this
    // frame (buffer upload and, for the brick path, a re-bake below).
    if (m_uiEnabled) {
        m_window.beginUiFrame();
        m_device->beginUiFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame(); // must follow NewFrame; the gizmo is built in buildUi
        buildUi();
        ImGui::Render();
    }

    // Animation playback/scrubbing writes the interpolated pose into the edit list.
    // Outside the UI guard so a headless --dump of an animation still moves. Reuses
    // the normal upload+bake path, unmeasured — like a live gizmo drag — and only
    // when the playhead actually moved (so a paused clip costs nothing).
    if (m_animState.playing || m_animState.time != m_lastAnimTime) {
        if (sampleInto(m_animClip, m_animState.time, m_scene)) {
            uploadScene();
            m_needBake = true;
        }
        // The camera track drives the camera directly (no scene upload/bake — the
        // camera only feeds the marcher uniforms, read fresh every frame).
        CameraSample cam;
        if (sampleCamera(m_animClip, m_animState.time, cam)) {
            const Vec3 fwd = quatRotate(cam.orientation, Vec3{0.0f, 0.0f, -1.0f});
            m_camera.setPose(cam.position, fwd, cam.fov);
        }
        m_lastAnimTime = m_animState.time;
    }

    rhi::CommandList& cmd = m_device->beginFrame();

    // beginFrame is what actually rebuilds the swapchain, so the authoritative
    // surface size is only known now. Creating and destroying resources during
    // recording is fine: destruction is deferred past the frames in flight.
    resizeRenderTarget(m_device->swapchainExtent());

    // The bake is recorded into the frame ahead of the marcher; the
    // dispatch-ordering guarantee makes the marcher in this same command list
    // see what it wrote. Re-baked on every scene change (full re-bake, no
    // incremental path). Stats are read back — which stalls the device — only
    // when a re-bake is being timed (the first one and each committed edit), so
    // dragging a value re-bakes every frame without a stall.
    // While playing, optionally preview through the reference renderer, which
    // re-reads the edit list every frame and needs no bake — so per-frame
    // animation stays smooth instead of re-baking the whole brick grid each frame.
    // The pending bake is deferred until playback stops and the brick view returns.
    const bool         previewRef = m_animState.playing && m_animState.previewReference;
    const RendererMode eff = previewRef ? RendererMode::Reference : m_renderer;

    if (m_needBake && !previewRef) {
        recordBake(cmd);
        m_needBake = false;
        if (m_measureBake) {
            m_bakePending = true;
            m_measureBake = false;
        }
    }

    if (eff == RendererMode::Reference) {
        recordReference(cmd);
    } else {
        recordBrick(cmd);
    }

    // Picking is independent of the image: it marches the click ray against the
    // same edit list and writes a single index, read back after present.
    if (m_pickPending) {
        recordPick(cmd);
    }

    cmd.blitToSwapchain(m_renderTarget);

    // The overlay draws last, on top of the presented image.
    if (m_uiEnabled) {
        cmd.endUiFrame();
    }

    const double submitTime = platform::timeSeconds();
    m_device->endFrame();

    if (m_bakePending) {
        // readBuffer waits for the submitted frame to complete, so the span from
        // submit to here is the re-bake frame's GPU cost (bake + render + the
        // readback stall). Reported as the re-bake time.
        reportBakeStats();
        m_lastBakeMs  = static_cast<float>((platform::timeSeconds() - submitTime) * 1000.0);
        m_haveBake    = true;
        m_bakePending = false;
    }

    if (m_pickPending) {
        // The pick pass wrote the hit index this frame; read it back (blocks on
        // the frame like the bake stats do) and hand it to the editor. -1 on a
        // miss deselects.
        int32_t index = -1;
        m_device->readBuffer(
            m_pickBuffer,
            std::span<std::byte>(reinterpret_cast<std::byte*>(&index), sizeof(index)));
        m_editor.applyPick(index);
        m_pickPending = false;
    }
}

Application::SceneUniforms Application::cameraUniforms() const {
    const Vec3  position = m_camera.position();
    const Vec3  right    = m_camera.right();
    const Vec3  up       = m_camera.up();
    const Vec3  forward  = m_camera.forward();
    const float aspect   = static_cast<float>(m_targetExtent.width) /
                         static_cast<float>(m_targetExtent.height);

    return SceneUniforms{
        .cameraPosition = {position.x, position.y, position.z,
                           std::tan(m_camera.verticalFovRadians() * 0.5f)},
        .cameraRight    = {right.x, right.y, right.z, aspect},
        .cameraUp       = {up.x, up.y, up.z,
                           m_pinTime ? m_fixedTime : static_cast<float>(platform::timeSeconds())},
        .cameraForward  = {forward.x, forward.y, forward.z, 0.0f},
        .resolution     = {static_cast<float>(m_targetExtent.width),
                           static_cast<float>(m_targetExtent.height)},
        .primitiveCount = static_cast<int32_t>(m_scene.size()),
        .exposure          = m_render.exposure,
        .reflectionSamples = m_render.reflectionSamples,
    };
}

void Application::recordReference(rhi::CommandList& cmd) {
    const SceneUniforms uniforms = cameraUniforms();
    cmd.bindComputePipeline(m_refPipeline);
    cmd.bindStorageTexture(0, m_renderTarget);
    cmd.bindStorageBuffer(1, m_sceneBuffer);
    cmd.bindStorageBuffer(5, m_lightingBuffer);
    cmd.pushConstants(asBytes(uniforms));
    cmd.dispatch(divideRoundUp(m_targetExtent.width, kWorkgroupSize),
                 divideRoundUp(m_targetExtent.height, kWorkgroupSize), 1);
}

void Application::recordBrick(rhi::CommandList& cmd) {
    const SceneUniforms cam = cameraUniforms();
    BrickUniforms       uniforms{};
    std::memcpy(uniforms.cameraPosition, cam.cameraPosition, sizeof(cam.cameraPosition));
    std::memcpy(uniforms.cameraRight, cam.cameraRight, sizeof(cam.cameraRight));
    std::memcpy(uniforms.cameraUp, cam.cameraUp, sizeof(cam.cameraUp));
    std::memcpy(uniforms.cameraForward, cam.cameraForward, sizeof(cam.cameraForward));
    uniforms.resolution[0]  = cam.resolution[0];
    uniforms.resolution[1]  = cam.resolution[1];
    uniforms.primitiveCount = cam.primitiveCount;
    uniforms.debugMode      = m_debugView;
    uniforms.aabbMin[0]     = brick::kAabbMin.x;
    uniforms.aabbMin[1]     = brick::kAabbMin.y;
    uniforms.aabbMin[2]     = brick::kAabbMin.z;
    uniforms.aabbMax[0]     = brick::kAabbMax.x;
    uniforms.aabbMax[1]     = brick::kAabbMax.y;
    uniforms.aabbMax[2]     = brick::kAabbMax.z;
    uniforms.exposure          = cam.exposure;
    uniforms.reflectionSamples = cam.reflectionSamples;
    // The resolution the cells were baked at — not the editor's pending value —
    // so the marcher's cell indexing matches the buffer contents exactly.
    uniforms.gridRes           = m_bakedGridRes;

    cmd.bindComputePipeline(m_brickPipeline);
    cmd.bindStorageTexture(0, m_renderTarget);
    cmd.bindStorageBuffer(1, m_sceneBuffer);
    cmd.bindStorageBuffer(2, m_cellsBuffer);
    cmd.bindStorageBuffer(3, m_bricksBuffer);
    cmd.bindStorageBuffer(5, m_lightingBuffer);
    cmd.pushConstants(asBytes(uniforms));
    cmd.dispatch(divideRoundUp(m_targetExtent.width, kWorkgroupSize),
                 divideRoundUp(m_targetExtent.height, kWorkgroupSize), 1);
}

void Application::recordBake(rhi::CommandList& cmd) {
    BakeUniforms uniforms{};
    uniforms.aabbMin[0] = brick::kAabbMin.x;
    uniforms.aabbMin[1] = brick::kAabbMin.y;
    uniforms.aabbMin[2] = brick::kAabbMin.z;
    uniforms.aabbMax[0] = brick::kAabbMax.x;
    uniforms.aabbMax[1] = brick::kAabbMax.y;
    uniforms.aabbMax[2] = brick::kAabbMax.z;
    uniforms.control[0] = static_cast<int32_t>(m_scene.size());
    uniforms.control[1] = brick::kPoolCapacity;

    // Bake at the editor's current resolution and remember it: the marcher keys
    // its cell indexing off m_bakedGridRes, so the two can never disagree.
    const int32_t gridRes = std::clamp(m_render.gridRes, brick::kMinGridRes, brick::kMaxGridRes);
    uniforms.control[2]   = gridRes;
    m_bakedGridRes        = gridRes;

    // Zero the bump counter / diagnostics before the atomics touch it. The
    // ordering guarantee makes it visible to the classify dispatch below.
    cmd.clearBuffer(m_statsBuffer);

    // Pass 1: classify each cell and bump-allocate brick slots.
    cmd.bindComputePipeline(m_classifyPipeline);
    cmd.bindStorageBuffer(1, m_sceneBuffer);
    cmd.bindStorageBuffer(2, m_cellsBuffer);
    cmd.bindStorageBuffer(4, m_statsBuffer);
    cmd.pushConstants(asBytes(uniforms));
    cmd.dispatch(divideRoundUp(static_cast<uint32_t>(gridRes), 4),
                 divideRoundUp(static_cast<uint32_t>(gridRes), 4),
                 divideRoundUp(static_cast<uint32_t>(gridRes), 4));

    // Pass 2: fill each occupied cell's brick. One workgroup per cell, one
    // thread per voxel; empty cells early-out. Ordering is the seam's promise.
    cmd.bindComputePipeline(m_fillPipeline);
    cmd.bindStorageBuffer(1, m_sceneBuffer);
    cmd.bindStorageBuffer(2, m_cellsBuffer);
    cmd.bindStorageBuffer(3, m_bricksBuffer);
    cmd.pushConstants(asBytes(uniforms));
    cmd.dispatch(static_cast<uint32_t>(gridRes), static_cast<uint32_t>(gridRes),
                 static_cast<uint32_t>(gridRes));
}

void Application::recordPick(rhi::CommandList& cmd) {
    cmd.bindComputePipeline(m_pickPipeline);
    cmd.bindStorageBuffer(1, m_sceneBuffer);
    cmd.bindStorageBuffer(6, m_pickBuffer);
    cmd.pushConstants(asBytes(m_pickUniforms));
    cmd.dispatch(1, 1, 1);
}

void Application::reportBakeStats() {
    brick::BakeStats stats{};
    m_device->readBuffer(m_statsBuffer,
                         std::span<std::byte>(reinterpret_cast<std::byte*>(&stats), sizeof(stats)));

    const int64_t cellCount = brick::cellCount(m_bakedGridRes);
    const double  occupancy =
        100.0 * static_cast<double>(stats.bricksRequested) / static_cast<double>(cellCount);
    std::fprintf(stderr,
                 "[bake] %u/%lld cells occupied (%.1f%%) at %d^3, pool capacity %d, overflow %u\n",
                 stats.bricksRequested, static_cast<long long>(cellCount), occupancy,
                 m_bakedGridRes, brick::kPoolCapacity, stats.overflowCount);
    if (stats.overflowCount != 0) {
        std::fprintf(stderr,
                     "[bake] WARNING: brick pool overflowed by %u slots — raise kPoolCapacity or "
                     "shrink the AABB; the brick field is incomplete.\n",
                     stats.overflowCount);
    }
}

// ---------------------------------------------------------------------------
// Verification
//
// The dump is a deliberately dumb container: magic, extent, linear RGBA
// floats. It exists so two backends can be diffed against each other, and as
// the starting point for golden-image tests later.
// ---------------------------------------------------------------------------
namespace {

constexpr char kDumpMagic[4] = {'F', 'Z', 'L', 'D'};

struct DumpHeader {
    char     magic[4]{};
    uint32_t width  = 0;
    uint32_t height = 0;
};

} // namespace

void Application::writeDump(const std::filesystem::path& path) {
    std::vector<float> pixels(static_cast<size_t>(m_targetExtent.width) * m_targetExtent.height * 4);
    m_device->readTexture(m_renderTarget, pixels);

    DumpHeader header{};
    std::memcpy(header.magic, kDumpMagic, sizeof(kDumpMagic));
    header.width  = m_targetExtent.width;
    header.height = m_targetExtent.height;

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw std::runtime_error("cannot write dump: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(pixels.data()),
               static_cast<std::streamsize>(pixels.size() * sizeof(float)));
    if (!file) {
        throw std::runtime_error("short write on dump: " + path.string());
    }
    std::fprintf(stderr, "[engine] wrote %ux%u dump to %s\n", header.width, header.height,
                 path.string().c_str());
}

bool Application::compareAgainst(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot read reference: " + path.string());
    }

    DumpHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!file || std::memcmp(header.magic, kDumpMagic, sizeof(kDumpMagic)) != 0) {
        throw std::runtime_error("not a fitzel dump: " + path.string());
    }
    if (header.width != m_targetExtent.width || header.height != m_targetExtent.height) {
        std::fprintf(stderr, "[compare] FAIL: reference is %ux%u, this run is %ux%u\n",
                     header.width, header.height, m_targetExtent.width, m_targetExtent.height);
        return false;
    }

    const size_t       count = static_cast<size_t>(header.width) * header.height * 4;
    std::vector<float> reference(count);
    file.read(reinterpret_cast<char*>(reference.data()),
              static_cast<std::streamsize>(count * sizeof(float)));
    if (!file) {
        throw std::runtime_error("truncated reference: " + path.string());
    }

    std::vector<float> actual(count);
    m_device->readTexture(m_renderTarget, actual);

    float  maxDiff  = 0.0f;
    double sumDiff  = 0.0;
    size_t overCount = 0;
    for (size_t i = 0; i < count; ++i) {
        const float diff = std::abs(actual[i] - reference[i]);
        maxDiff = std::max(maxDiff, diff);
        sumDiff += diff;
        overCount += diff > m_tolerance ? 1 : 0;
    }

    // Two acceptance criteria. The cross-backend test wants max error within
    // tolerance — every component agrees. The reference/brick A/B cannot meet
    // that: a curved silhouette shifts by a sub-pixel under the trilinear field
    // and the infinite ground plane leaves the finite AABB, so a small set of
    // components legitimately exceed tolerance while the image as a whole
    // matches. maxOutlierFraction names how large that set may be; at its
    // default of 0 the strict max test is unchanged.
    const double outlierFraction = static_cast<double>(overCount) / static_cast<double>(count);
    const bool   passed = (m_maxOutlierFraction > 0.0f)
                              ? outlierFraction <= static_cast<double>(m_maxOutlierFraction)
                              : maxDiff <= m_tolerance;
    std::fprintf(stderr,
                 "[compare] %s — max %.6f, mean %.6f, %zu/%zu (%.4f%%) components over tolerance "
                 "%.6f, outlier budget %.4f%%\n",
                 passed ? "PASS" : "FAIL", static_cast<double>(maxDiff), sumDiff / double(count),
                 overCount, count, 100.0 * outlierFraction, static_cast<double>(m_tolerance),
                 100.0 * static_cast<double>(m_maxOutlierFraction));
    return passed;
}

} // namespace engine
