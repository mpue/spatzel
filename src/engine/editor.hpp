#pragma once

// The editor panel. It edits the edit list (std::vector<GpuPrimitive>) directly
// — there is no second scene representation. Selection is by list, editing is by
// number field; picking in the viewport and drag gizmos are deliberately out of
// scope. Undo/redo is a plain stack of whole-scene snapshots, which a
// parametric list makes cheap.
//
// The panel issues only backend-neutral ImGui:: calls; the render backend that
// puts those on screen lives behind the RHI seam.

#include "engine/lsystem.hpp"
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

// What the panel asks the engine to do once its UI is built for the frame.
struct EditorActions {
    bool sceneChanged = false; // edit list mutated this frame -> re-upload + re-bake
    bool bakeMeasure  = false; // the change was committed -> time the resulting re-bake
};

// Global render settings the panel edits in place. These feed the marcher
// uniforms directly and need no re-upload or re-bake — the next frame reads them.
struct RenderSettings {
    float exposure          = 1.0f;
    int   reflectionSamples = 4; // glossy reflection rays per hit
};

class Editor {
public:
    // Builds the panel for this frame. Mutates `scene` and `renderer` in place:
    // the edit list stays the single source of truth and the editor edits it
    // directly. `sceneDir` is where scene files are saved and where example
    // scenes are listed from.
    EditorActions draw(std::vector<GpuPrimitive>& scene, RendererMode& renderer,
                       RenderSettings& render, bool brickAvailable, const EditorStats& stats,
                       const std::filesystem::path& sceneDir);

private:
    void pushUndo(std::vector<GpuPrimitive> state);
    void clampSelection(const std::vector<GpuPrimitive>& scene);

    int m_selected = -1;

    // Undo/redo history: whole-scene snapshots. Cheap for a parametric list.
    std::vector<std::vector<GpuPrimitive>> m_undo;
    std::vector<std::vector<GpuPrimitive>> m_redo;

    // Scene as it was when the current field edit began, so one drag or typed
    // value produces exactly one undo entry.
    std::vector<GpuPrimitive> m_preEdit;

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

    void        buildLSystemPanel(std::vector<GpuPrimitive>& scene, const EditorStats& stats,
                                  EditorActions& actions);
    LSystemConfig lsysConfig() const; // assemble a config from the UI fields
};

} // namespace engine
