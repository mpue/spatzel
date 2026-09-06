#include "engine/editor.hpp"

#include "engine/editor_internal.hpp"

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder*

#include <algorithm>

namespace engine {

// The panel windows (buildRendererPanel/…), the viewport gizmo (buildGizmo,
// drawSelectionOutlines), the lighting and vegetation windows live in the sibling
// editor_*.cpp files; this file is the core: selection, undo, and the draw()
// orchestration that hosts everything in a dockspace.

Editor::Snapshot Editor::snapshot(const std::vector<GpuPrimitive>& scene) const {
    Snapshot s;
    s.scene = scene;
    if (m_anim != nullptr) {
        s.anim = *m_anim;
    }
    return s;
}

void Editor::pushUndo(Snapshot state) {
    m_undo.push_back(std::move(state));
    if (m_undo.size() > kMaxUndo) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
}

// Undo and redo are the same move in opposite directions, so both go through
// here: the live state is pushed onto the other stack before it is overwritten.
void Editor::restore(std::vector<Snapshot>& from, std::vector<Snapshot>& to,
                     std::vector<GpuPrimitive>& scene, EditorActions& actions) {
    if (from.empty()) {
        return;
    }
    to.push_back(snapshot(scene));
    Snapshot state = std::move(from.back());
    from.pop_back();
    scene = std::move(state.scene);
    if (m_anim != nullptr) {
        *m_anim = std::move(state.anim);
    }
    clampSelection(scene);
    m_tlSelTime          = -1.0f; // the keyframe it pointed at may be gone
    actions.sceneChanged = actions.bakeMeasure = true;
}

void Editor::applyUndo(std::vector<GpuPrimitive>& scene, EditorActions& actions) {
    restore(m_undo, m_redo, scene, actions);
}

void Editor::applyRedo(std::vector<GpuPrimitive>& scene, EditorActions& actions) {
    restore(m_redo, m_undo, scene, actions);
}

void Editor::handleUndoShortcuts(std::vector<GpuPrimitive>& scene, EditorActions& actions) {
    const ImGuiIO& io = ImGui::GetIO();
    // Not while typing (Ctrl+Z belongs to the text field then), and not mid-drag:
    // a gizmo or keyframe drag is still assembling its own single undo entry.
    if (io.WantTextInput || m_gizmoUsing || m_tlDrag != 0 || !io.KeyCtrl) {
        return;
    }
    const bool z = ImGui::IsKeyPressed(ImGuiKey_Z, false);
    if (z && !io.KeyShift) {
        applyUndo(scene, actions);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (z && io.KeyShift)) {
        applyRedo(scene, actions);
    }
}

void Editor::clampSelection(const std::vector<GpuPrimitive>& scene) {
    // Drop any selected index that ran past the end of a shrunken scene; the rest
    // (including an empty selection) is left as-is.
    const int count = static_cast<int>(scene.size());
    m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(),
                                     [count](int i) { return i < 0 || i >= count; }),
                      m_selection.end());
}

bool Editor::isSelected(int index) const {
    return std::find(m_selection.begin(), m_selection.end(), index) != m_selection.end();
}

void Editor::selectOnly(int index) {
    m_selection.assign(1, index);
}

void Editor::toggleSelected(int index) {
    const auto it = std::find(m_selection.begin(), m_selection.end(), index);
    if (it != m_selection.end()) {
        m_selection.erase(it);
    } else {
        m_selection.push_back(index); // becomes the new primary
    }
}

void Editor::applyPick(int index) {
    if (index < 0) {
        // Miss: a plain click clears the selection; a Ctrl-click leaves it be (so
        // a stray click while building a multi-selection does not wipe it).
        if (!m_pendingPickAdditive) {
            m_selection.clear();
        }
        return;
    }
    if (m_pendingPickAdditive) {
        toggleSelected(index);
    } else {
        selectOnly(index);
    }
}

// The default docking layout, built once when no saved layout exists: a left
// column (Primitives over Properties), a right column (Renderer, Lighting, then
// Vegetation+Scene tabbed), and the viewport in the transparent centre.
void Editor::setupDockLayout(unsigned int dockId) {
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace |
                                          ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockId, ImGui::GetMainViewport()->Size);

    ImGuiID centre = dockId;

    // A full-width strip across the very bottom for the Timeline (split first so
    // it spans the whole width, Blender-style), then the left/right columns above.
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.26f, nullptr,
                                                       &centre);
    const ImGuiID left   = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.22f, nullptr,
                                                       &centre);
    const ImGuiID right  = ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.30f, nullptr,
                                                       &centre);

    ImGuiID       leftBottom = 0;
    const ImGuiID leftTop = ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.45f, nullptr,
                                                        &leftBottom);

    ImGuiID       rightRest = 0;
    const ImGuiID rightTop  = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.32f, nullptr,
                                                          &rightRest);
    ImGuiID       rightBottom = 0;
    const ImGuiID rightMid = ImGui::DockBuilderSplitNode(rightRest, ImGuiDir_Up, 0.55f, nullptr,
                                                         &rightBottom);

    ImGui::DockBuilderDockWindow("Primitives", leftTop);
    ImGui::DockBuilderDockWindow("Properties", leftBottom);
    ImGui::DockBuilderDockWindow("Renderer", rightTop);
    ImGui::DockBuilderDockWindow("Lighting", rightMid);
    ImGui::DockBuilderDockWindow("Vegetation", rightBottom);
    ImGui::DockBuilderDockWindow("Scene", rightBottom); // tabbed with Vegetation
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    ImGui::DockBuilderFinish(dockId);
}

EditorActions Editor::draw(std::vector<GpuPrimitive>& scene, RendererMode& renderer,
                           RenderSettings& render, LightingSettings& lighting, AnimationClip& anim,
                           AnimationState& animState, const ViewportCamera& camera,
                           bool brickAvailable, const EditorStats& stats,
                           const std::filesystem::path& sceneDir) {
    EditorActions actions;
    m_anim = &anim; // what a snapshot taken inside a panel captures beside the scene
    clampSelection(scene);

    // The viewport gizmo and click-picking run first (into the background draw
    // list, over the image), so the gizmo owns the mouse when hovered and a click
    // on empty space falls through to a pick.
    buildGizmo(scene, camera, actions);

    // Ctrl+Z / Ctrl+Y anywhere in the editor. After the gizmo, so a drag ending
    // this frame has already committed its own entry, and before the panels, so
    // they build their widgets from the restored state.
    handleUndoShortcuts(scene, actions);

    // Dock host over the whole viewport; the central node is transparent so the
    // 3D image shows through and clicks there reach the picker.
    const ImGuiID dockId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                                                        ImGuiDockNodeFlags_PassthruCentralNode);
    if (!m_dockInit) {
        m_dockInit                = true;
        const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        // Build the default layout only when the ini restored nothing.
        if (node == nullptr || node->IsEmpty()) {
            setupDockLayout(dockId);
        }
    }

    buildRendererPanel(renderer, render, brickAvailable, stats, actions);
    buildPrimitivesPanel(scene, stats, actions);
    buildPropertiesPanel(scene, actions);
    buildLightingPanel(lighting, actions);
    buildLSystemPanel(scene, stats, actions);
    buildTimelinePanel(scene, anim, animState, camera);
    buildScenePanel(scene, anim, actions, sceneDir);

    return actions;
}

} // namespace engine
