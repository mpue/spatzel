#pragma once

// The editor panel. It edits the edit list (std::vector<GpuPrimitive>) directly
// — there is no second scene representation. Selection is by list, editing is by
// number field; picking in the viewport and drag gizmos are deliberately out of
// scope. Undo/redo is a plain stack of whole-state snapshots (edit list plus
// animation clip), which a parametric list makes cheap; Ctrl+Z / Ctrl+Y drive it
// from anywhere in the editor, and the Primitives panel has the same two
// buttons.
//
// The panel issues only backend-neutral ImGui:: calls; the render backend that
// puts those on screen lives behind the RHI seam.

#include "engine/animation.hpp"
#include "engine/brick.hpp"
#include "engine/lsystem.hpp"
#include "engine/mat4.hpp"
#include "engine/render_mode.hpp"
#include "engine/scene.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Display-only figures the panel shows. The engine fills these in each frame.
struct EditorStats {
    float       fps            = 0.0f;
    float       frametimeMs    = 0.0f;
    const char* rendererName   = "";
    int         primitiveCount = 0;
    int         maxPrimitives  = 0;
    float       lastBakeMs     = 0.0f;
    bool        haveBake       = false; // a brick re-bake has been timed
};

// The camera, handed to the editor each frame so it can drive the transform
// gizmo (view/projection) and build the pick ray (basis + fov). Assembled by the
// engine from its FlyCamera; the editor knows nothing about how it is produced.
struct ViewportCamera {
    Mat4  view{};
    Mat4  proj{};
    Vec3  position{};
    Vec3  right{};
    Vec3  up{};
    Vec3  forward{};
    float tanHalfFov = 0.0f;
    float aspect     = 1.0f;
};

// What the panel asks the engine to do once its UI is built for the frame.
struct EditorActions {
    bool sceneChanged = false; // edit list mutated this frame -> re-upload + re-bake
    bool bakeMeasure  = false; // the change was committed -> time the resulting re-bake
    bool rebake       = false; // brick structure needs rebuilding, but the scene did
                               // not change (e.g. the grid resolution was retuned)
    bool lightingChanged = false; // lighting edited this frame -> re-upload the light buffer

    // A viewport click asked for a pick: the engine marches this ray and reports
    // the hit primitive back via Editor::setSelected. Origin/dir are world-space.
    bool  pickRequested   = false;
    float pickRayOrigin[3] = {};
    float pickRayDir[3]    = {};
};

// Global render settings the panel edits in place. Exposure and reflection
// samples feed the marcher uniforms directly and need no re-upload or re-bake.
// gridRes is different: it is the resolution the brick field is baked at, so a
// change to it forces a re-bake (signalled via EditorActions::rebake).
struct RenderSettings {
    float exposure          = 1.0f;
    int   reflectionSamples = 4;                       // glossy reflection rays per hit
    int   gridRes           = brick::kDefaultGridRes;  // brick grid cells per axis
};

// The scene lighting, edited in place by the panel. These are the values shared
// by both renderers (they arrive at the shaders in a small storage buffer, not
// as push constants). A change re-uploads that buffer but needs no re-bake — the
// bricks store distance, not shading, so lighting is applied fresh every frame.
// The key light is stored as azimuth/elevation because that is far easier to dial
// than a raw vector; the engine converts it to a direction when it packs the
// buffer. Defaults reproduce the previously hard-coded look exactly.
struct LightingSettings {
    // Directional key light.
    float keyAzimuthDeg   = 37.9f;                  // around the vertical axis
    float keyElevationDeg = 54.5f;                  // above the horizon
    float keyColour[3]    = {1.00f, 0.97f, 0.90f};
    float keyIntensity    = 1.0f;

    // Shadow-casting point light.
    float pointPosition[3] = {2.0f, 4.5f, -2.5f};
    float pointColour[3]   = {1.0f, 0.6f, 0.3f};
    float pointIntensity   = 50.0f;

    // Hemispherical ambient (cheap diffuse-IBL stand-in).
    float ambientSky[3]    = {0.28f, 0.33f, 0.42f};
    float ambientGround[3] = {0.14f, 0.12f, 0.11f};
    float ambientStrength  = 1.0f;

    // Background gradient (also what smooth surfaces reflect).
    float bgHorizon[3] = {0.13f, 0.14f, 0.17f};
    float bgZenith[3]  = {0.42f, 0.52f, 0.68f};
};

class Editor {
public:
    // Builds the panel for this frame. Mutates `scene` and `renderer` in place:
    // the edit list stays the single source of truth and the editor edits it
    // directly. `sceneDir` is where scene files are saved and where example
    // scenes are listed from.
    EditorActions draw(std::vector<GpuPrimitive>& scene, RendererMode& renderer,
                       RenderSettings& render, LightingSettings& lighting, AnimationClip& anim,
                       AnimationState& animState, const ViewportCamera& camera, bool brickAvailable,
                       const EditorStats& stats, const std::filesystem::path& sceneDir);

    // Called by the engine after a pick pass resolves, with the hit primitive
    // index (or -1 for a miss). Whether it replaces the selection or toggles the
    // primitive into it depends on the modifier held when the click was issued
    // (captured in m_pendingPickAdditive). Bounds are re-checked on the next draw.
    void applyPick(int index);

private:
    // One undo step: everything an edit can touch. The animation clip travels
    // with the edit list because a load, an L-system generate or a keyframe edit
    // changes both, and restoring only one half would leave tracks describing
    // objects that are no longer there.
    struct Snapshot {
        std::vector<GpuPrimitive> scene;
        AnimationClip             anim;
    };

    [[nodiscard]] Snapshot snapshot(const std::vector<GpuPrimitive>& scene) const;
    void pushUndo(Snapshot state);
    void pushUndo(const std::vector<GpuPrimitive>& scene) { pushUndo(snapshot(scene)); }
    // Move one step along the history: pop `from`, push what is live onto `to`.
    // A no-op on an empty stack.
    void restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to,
                 std::vector<GpuPrimitive>& scene, EditorActions& actions);
    void applyUndo(std::vector<GpuPrimitive>& scene, EditorActions& actions);
    void applyRedo(std::vector<GpuPrimitive>& scene, EditorActions& actions);
    // Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z, handled once per frame for the whole editor.
    void handleUndoShortcuts(std::vector<GpuPrimitive>& scene, EditorActions& actions);
    void clampSelection(const std::vector<GpuPrimitive>& scene);

    // The current selection: primitive indices, in the order they were added, so
    // the last is the "primary" (the one whose fields the panel edits). Empty
    // means nothing selected. A gizmo drag transforms the whole set.
    std::vector<int> m_selection;
    [[nodiscard]] int  primary() const {
        return m_selection.empty() ? -1 : m_selection.back();
    }
    [[nodiscard]] bool isSelected(int index) const;
    void               selectOnly(int index); // clear, then select just this one
    void               toggleSelected(int index);

    // Whether the pick request in flight should toggle (Ctrl held) or replace the
    // selection. Captured when the click is issued, consumed in applyPick.
    bool m_pendingPickAdditive = false;

    // Undo/redo history: whole-state snapshots. Cheap for a parametric list.
    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;

    // State as it was when the current edit began, so one drag or typed value
    // produces exactly one undo entry.
    Snapshot m_preEdit;

    // The clip being edited this frame, so a snapshot taken deep inside a panel
    // can capture it without every panel having to take it as a parameter. Set
    // at the top of draw(), never used outside it.
    AnimationClip* m_anim = nullptr;

    int         m_addType = 0;               // PrimitiveType for the Add control
    char        m_fileName[128] = "scene.json"; // save/load target (relative to sceneDir)
    std::string m_status;                    // last save/load result, shown in the panel

    // --- Vegetation (L-system) generator ---------------------------------
    // The panel edits text/number fields; a LSystemConfig is assembled from
    // them on demand for the live estimate and for Generate. Rules are held as
    // fixed char buffers so ImGui::InputText can write them directly.
    static constexpr int kLsysMaxRules = 6;
    struct LSystemUi {
        char  axiom[128] = "X";
        char  rulePred[kLsysMaxRules][2] = {"X", "F", "", "", "", ""};
        char  ruleSucc[kLsysMaxRules][192] = {"F+[[X]-X]-F[-FX]+X", "FF", "", "", "", ""};
        int   iterations    = 3;
        float angleDeg      = 25.0f;
        float segmentLength = 0.28f;
        float lengthTaper   = 0.90f;
        float baseRadius    = 0.06f;
        float radiusTaper   = 0.74f;
        float leafSize      = 0.09f;
        bool  leaves        = true;
        float tropism       = 0.04f;
        int   seed          = 1;
        float jitter        = 0.22f;
        float basePos[3]    = {0.0f, 0.0f, 0.0f};
        float branchColour[3] = {0.42f, 0.28f, 0.15f};
        float leafColour[3]   = {0.28f, 0.55f, 0.20f};
        int   preset          = 1; // index into the preset table; 0 = Custom
    };
    LSystemUi m_lsys;

    // Dockspace host layout (built once when no saved layout is restored).
    void setupDockLayout(unsigned int dockId); // unsigned int == ImGuiID
    bool m_dockInit = false;

    // The individual dockable panel windows. Each is one ImGui::Begin/End; they
    // live in editor_panels.cpp, editor_lighting.cpp and editor_lsystem.cpp.
    void buildRendererPanel(RendererMode& renderer, RenderSettings& render, bool brickAvailable,
                            const EditorStats& stats, EditorActions& actions);
    void buildPrimitivesPanel(std::vector<GpuPrimitive>& scene, const EditorStats& stats,
                              EditorActions& actions);
    void buildPropertiesPanel(std::vector<GpuPrimitive>& scene, EditorActions& actions);
    void buildScenePanel(std::vector<GpuPrimitive>& scene, AnimationClip& anim,
                         EditorActions& actions, const std::filesystem::path& sceneDir);
    void buildLSystemPanel(std::vector<GpuPrimitive>& scene, const EditorStats& stats,
                           EditorActions& actions);
    void buildLightingPanel(LightingSettings& lighting, EditorActions& actions);
    void buildTimelinePanel(std::vector<GpuPrimitive>& scene, AnimationClip& anim,
                            AnimationState& animState, const ViewportCamera& camera);
    LSystemConfig lsysConfig() const; // assemble a config from the UI fields

    // The transform gizmo over the viewport: draws the handles at the selection
    // pivot, applies a drag to every selected primitive, hooks the undo/re-bake
    // edges, and turns a click on empty space into a pick request.
    void buildGizmo(std::vector<GpuPrimitive>& scene, const ViewportCamera& camera,
                    EditorActions& actions);
    // A highlight box around each selected primitive, drawn in the viewport.
    void drawSelectionOutlines(const std::vector<GpuPrimitive>& scene,
                               const ViewportCamera& camera) const;

    // --- dope-sheet (Timeline) view state --------------------------------
    float         m_tlPxPerSec  = 0.0f;   // horizontal zoom; 0 => fit to duration
    float         m_tlViewStart = 0.0f;   // time at the left edge of the track area
    std::uint32_t m_tlSelObject = 0;      // selected keyframe's object id (0 = none)
    float         m_tlSelTime   = -1.0f;  // selected keyframe's time (-1 = none)
    int           m_tlDrag      = 0;      // 0 = none, 1 = dragging a key, 2 = scrubbing
    float         m_tlDragStart = -1.0f;  // time the dragged key started at (-1 = none)

    // Transform mode of the gizmo: 0 = translate, 1 = rotate, 2 = dimensions.
    int  m_gizmoMode  = 0;
    bool m_gizmoUsing = false; // true while a gizmo drag is in progress
    // The live matrix ImGuizmo manipulates during a drag, held across frames so
    // ImGuizmo's own drag state stays consistent (and a dimensions drag reads its
    // accumulated scale off it).
    Mat4 m_gizmoMatrix{};
    // The pivot (selection centroid) captured at drag start, so a rotate orbits a
    // fixed point rather than a centroid that shifts as the objects move.
    Vec3 m_gizmoPivot{};
    // Each selected primitive's params, snapshotted at drag start (parallel to
    // m_selection), so a dimensions drag scales relative to where it began.
    std::vector<std::array<float, 4>> m_dimStart;
};

} // namespace engine
