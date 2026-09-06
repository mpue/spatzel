#include "engine/editor.hpp"

#include "engine/editor_internal.hpp"
#include "engine/scene_io.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace engine {

void Editor::buildRendererPanel(RendererMode& renderer, RenderSettings& render,
                                bool brickAvailable, const EditorStats& stats,
                                EditorActions& actions) {
    if (!ImGui::Begin("Renderer")) {
        ImGui::End();
        return;
    }

    ImGui::Text("%.1f FPS  (%.2f ms)", static_cast<double>(stats.fps),
                static_cast<double>(stats.frametimeMs));
    ImGui::Text("Renderer: %s", stats.rendererName);
    if (brickAvailable) {
        const bool brick = renderer == RendererMode::Brick;
        ImGui::BeginDisabled(brick);
        if (ImGui::Button("Brick")) {
            renderer = RendererMode::Brick;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!brick);
        if (ImGui::Button("Reference")) {
            renderer = RendererMode::Reference;
        }
        ImGui::EndDisabled();
        if (stats.haveBake) {
            ImGui::Text("Last re-bake: %.1f ms", static_cast<double>(stats.lastBakeMs));
        } else {
            ImGui::TextDisabled("Last re-bake: -");
        }
    }
    ImGui::Text("Primitives: %d / %d", stats.primitiveCount, stats.maxPrimitives);

    ImGui::Separator();

    // Exposure and reflection-sample count feed the marcher uniforms directly;
    // no re-upload or re-bake, so they need no action flag.
    ImGui::SliderFloat("Exposure", &render.exposure, 0.1f, 8.0f, "%.2f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderInt("Reflection samples", &render.reflectionSamples, 1, 32);

    // Brick grid resolution: only the brick renderer bakes a grid. The engine
    // keeps rendering at the last-baked resolution while the slider is dragged and
    // re-bakes once on release (a coarse->fine sweep costs one re-bake, not one
    // per step).
    if (brickAvailable) {
        ImGui::SliderInt("Grid resolution", &render.gridRes, brick::kMinGridRes,
                         brick::kMaxGridRes, "%d cells/axis");
        render.gridRes = std::clamp(render.gridRes, brick::kMinGridRes, brick::kMaxGridRes);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            actions.rebake = actions.bakeMeasure = true;
        }
    }

    ImGui::End();
}

void Editor::buildPrimitivesPanel(std::vector<GpuPrimitive>& scene, const EditorStats& stats,
                                  EditorActions& actions) {
    if (!ImGui::Begin("Primitives")) {
        ImGui::End();
        return;
    }
    const float em = ImGui::GetFontSize();

    // --- undo / redo -------------------------------------------------------
    // The same two operations the Ctrl+Z / Ctrl+Y shortcuts run; the labels carry
    // the shortcut so it is discoverable from the panel.
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("Undo (Ctrl+Z)")) {
        applyUndo(scene, actions);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redo.empty());
    if (ImGui::Button("Redo (Ctrl+Y)")) {
        applyRedo(scene, actions);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%d undo / %d redo", static_cast<int>(m_undo.size()),
                        static_cast<int>(m_redo.size()));

    ImGui::Separator();

    // --- primitive list ----------------------------------------------------
    if (ImGui::BeginListBox("##primitives", ImVec2(-FLT_MIN, em * 9.0f))) {
        for (int i = 0; i < static_cast<int>(scene.size()); ++i) {
            char label[64];
            std::snprintf(label, sizeof(label), "%d: %s##%d", i,
                          typeLabel(scene[static_cast<size_t>(i)].control[0]), i);
            if (ImGui::Selectable(label, isSelected(i))) {
                // Ctrl-click toggles the row in/out of a multi-selection; a plain
                // click selects just it. Mirrors the viewport pick behaviour.
                if (ImGui::GetIO().KeyCtrl) {
                    toggleSelected(i);
                } else {
                    selectOnly(i);
                }
            }
        }
        ImGui::EndListBox();
    }

    // --- add / delete ------------------------------------------------------
    ImGui::SetNextItemWidth(em * 9.0f);
    ImGui::Combo("##addtype", &m_addType, kTypeNames.data(),
                 static_cast<int>(kTypeNames.size()));
    ImGui::SameLine();
    ImGui::BeginDisabled(stats.primitiveCount >= stats.maxPrimitives);
    if (ImGui::Button("Add")) {
        pushUndo(scene);
        scene.push_back(makeDefault(static_cast<PrimitiveType>(m_addType)));
        selectOnly(static_cast<int>(scene.size()) - 1);
        actions.sceneChanged = actions.bakeMeasure = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_selection.empty());
    if (ImGui::Button("Delete")) {
        pushUndo(scene);
        // Erase every selected primitive. Descending index order keeps the
        // earlier indices valid as later ones are removed.
        std::vector<int> doomed = m_selection;
        std::sort(doomed.begin(), doomed.end(), std::greater<int>());
        doomed.erase(std::unique(doomed.begin(), doomed.end()), doomed.end());
        for (int idx : doomed) {
            if (idx >= 0 && idx < static_cast<int>(scene.size())) {
                scene.erase(scene.begin() + idx);
            }
        }
        m_selection.clear();
        actions.sceneChanged = actions.bakeMeasure = true;
    }
    ImGui::EndDisabled();

    ImGui::End();
}

void Editor::buildPropertiesPanel(std::vector<GpuPrimitive>& scene, EditorActions& actions) {
    if (!ImGui::Begin("Properties")) {
        ImGui::End();
        return;
    }

    // Marks a live value change this frame, and — separately — a committed edit
    // (a finished drag or a typed-and-confirmed value). Snapshots the pre-edit
    // scene when a field is first touched so one edit session is one undo entry.
    auto track = [&]() {
        if (ImGui::IsItemActivated()) {
            m_preEdit = snapshot(scene);
        }
        if (ImGui::IsItemEdited()) {
            actions.sceneChanged = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            pushUndo(m_preEdit);
            actions.bakeMeasure = true;
        }
    };

    // --- gizmo mode --------------------------------------------------------
    // Mirrors the viewport gizmo's operation. Radio buttons plus the T/R/Y keys
    // (W/E/R are taken by the fly camera). buildGizmo reads m_gizmoMode.
    ImGui::TextUnformatted("Gizmo");
    ImGui::SameLine();
    ImGui::RadioButton("Move (T)", &m_gizmoMode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Rotate (R)", &m_gizmoMode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Dims (Y)", &m_gizmoMode, 2);
    if (m_selection.size() > 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%d selected)", static_cast<int>(m_selection.size()));
    }

    ImGui::Separator();

    // The panel edits the primary (last-selected) primitive; the gizmo moves the
    // whole selection.
    const int sel = primary();
    if (sel >= 0 && sel < static_cast<int>(scene.size())) {
        GpuPrimitive& p = scene[static_cast<size_t>(sel)];

        // Type. Changing it resets the parameters to that type's defaults, since
        // the params mean different things per type.
        int type = p.control[0];
        if (ImGui::Combo("Type", &type, kTypeNames.data(), static_cast<int>(kTypeNames.size())) &&
            type != p.control[0]) {
            pushUndo(scene);
            GpuPrimitive fresh = makeDefault(static_cast<PrimitiveType>(type));
            // Keep placement and material; reset only the type-specific params.
            std::copy(std::begin(p.position), std::end(p.position), std::begin(fresh.position));
            std::copy(std::begin(p.rotation), std::end(p.rotation), std::begin(fresh.rotation));
            std::copy(std::begin(p.albedo), std::end(p.albedo), std::begin(fresh.albedo));
            std::copy(std::begin(p.material), std::end(p.material), std::begin(fresh.material));
            fresh.control[1]     = p.control[1];
            fresh.control[2]     = p.control[2]; // preserve the animation object id
            p                    = fresh;
            actions.sceneChanged = actions.bakeMeasure = true;
        }

        int op = p.control[1];
        if (ImGui::Combo("Operator", &op, kOperatorNames.data(),
                         static_cast<int>(kOperatorNames.size())) &&
            op != p.control[1]) {
            pushUndo(scene);
            p.control[1]         = op;
            actions.sceneChanged = actions.bakeMeasure = true;
        }

        ImGui::DragFloat3("Position", p.position, 0.01f);
        track();

        ImGui::DragFloat4("Rotation", p.rotation, 0.01f);
        normaliseQuat(p.rotation); // the shader assumes a unit quaternion
        track();

        if (p.control[1] == static_cast<int32_t>(Operator::SmoothUnion)) {
            ImGui::DragFloat("Blend", &p.position[3], 0.005f, 0.0f, 4.0f);
            track();
        }

        // Type-specific parameters.
        switch (static_cast<PrimitiveType>(p.control[0])) {
            case PrimitiveType::Sphere:
                ImGui::DragFloat("Radius", &p.params[0], 0.01f, 0.0f, 100.0f);
                track();
                break;
            case PrimitiveType::Box:
                ImGui::DragFloat3("Half extents", p.params, 0.01f, 0.0f, 100.0f);
                track();
                ImGui::DragFloat("Rounding", &p.params[3], 0.005f, 0.0f, 100.0f);
                track();
                break;
            case PrimitiveType::Torus:
                ImGui::DragFloat("Major radius", &p.params[0], 0.01f, 0.0f, 100.0f);
                track();
                ImGui::DragFloat("Minor radius", &p.params[1], 0.01f, 0.0f, 100.0f);
                track();
                break;
            case PrimitiveType::Plane:
                ImGui::DragFloat3("Normal", p.params, 0.01f);
                track();
                ImGui::DragFloat("Offset", &p.params[3], 0.01f);
                track();
                break;
            case PrimitiveType::RoundCone:
                ImGui::DragFloat("Height", &p.params[0], 0.01f, 0.0f, 100.0f);
                track();
                ImGui::DragFloat("Base radius", &p.params[1], 0.005f, 0.0f, 100.0f);
                track();
                ImGui::DragFloat("Tip radius", &p.params[2], 0.005f, 0.0f, 100.0f);
                track();
                break;
            case PrimitiveType::Cylinder:
                ImGui::DragFloat("Radius", &p.params[0], 0.01f, 0.0f, 100.0f);
                track();
                ImGui::DragFloat("Half-height", &p.params[1], 0.01f, 0.0f, 100.0f);
                track();
                break;
        }

        ImGui::ColorEdit3("Albedo", p.albedo);
        track();

        // Metallic-roughness PBR material.
        ImGui::SliderFloat("Roughness", &p.material[0], 0.0f, 1.0f);
        track();
        ImGui::SliderFloat("Metallic", &p.material[1], 0.0f, 1.0f);
        track();
        ImGui::DragFloat("Emissive", &p.material[2], 0.02f, 0.0f, 20.0f);
        track();
        // Glass transmission: 0 opaque, 1 clear. The albedo doubles as the glass
        // tint (Beer-Lambert absorption through the object).
        ImGui::SliderFloat("Transmission", &p.material[3], 0.0f, 1.0f);
        track();
    } else {
        ImGui::TextDisabled("No primitive selected");
    }

    ImGui::End();
}

void Editor::buildScenePanel(std::vector<GpuPrimitive>& scene, AnimationClip& anim,
                             fluid::Settings& fluidSettings, EditorActions& actions,
                             const std::filesystem::path& sceneDir) {
    if (!ImGui::Begin("Scene")) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Scene file");
    ImGui::InputText("##filename", m_fileName, sizeof(m_fileName));

    const auto resolve = [&](const char* name) {
        std::filesystem::path p(name);
        return p.is_absolute() ? p : sceneDir / p;
    };

    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        try {
            saveScene(resolve(m_fileName), scene, &anim, &fluidSettings);
            m_status = std::string("Saved ") + m_fileName;
        } catch (const std::exception& e) {
            m_status = std::string("Save failed: ") + e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        try {
            // Snapshot before the load: loadScene writes the clip in place, so a
            // snapshot taken afterwards would pair the old scene with the new
            // animation. Pushed only once the load actually succeeded.
            Snapshot before = snapshot(scene);
            std::vector<GpuPrimitive> loaded =
                loadScene(resolve(m_fileName), &anim, &fluidSettings);
            pushUndo(std::move(before));
            scene = std::move(loaded);
            clampSelection(scene);
            actions.sceneChanged = actions.bakeMeasure = true;
            // The file may carry a different tank: re-voxelise the obstacles and
            // re-seed, so a loaded dam break starts from its own initial state
            // rather than from whatever was sloshing a moment ago.
            actions.fluidDomainMoved = actions.fluidReset = true;
            m_status = std::string("Loaded ") + m_fileName;
        } catch (const std::exception& e) {
            m_status = std::string("Load failed: ") + e.what();
        }
    }

    // Example scenes: every .json in the executable's scenes/ directory, one
    // click to load.
    if (ImGui::TreeNode("Examples")) {
        std::error_code ec;
        std::filesystem::directory_iterator it(sceneDir, ec);
        if (ec) {
            ImGui::TextDisabled("(%s)", sceneDir.string().c_str());
        } else {
            for (const auto& entry : it) {
                if (entry.path().extension() != ".json") {
                    continue;
                }
                const std::string name = entry.path().filename().string();
                if (ImGui::Button(name.c_str())) {
                    try {
                        Snapshot before = snapshot(scene); // see the Load button above
                        std::vector<GpuPrimitive> loaded =
                            loadScene(entry.path(), &anim, &fluidSettings);
                        pushUndo(std::move(before));
                        scene = std::move(loaded);
                        clampSelection(scene);
                        actions.sceneChanged = actions.bakeMeasure = true;
                        actions.fluidDomainMoved = actions.fluidReset = true;
                        std::snprintf(m_fileName, sizeof(m_fileName), "%s", name.c_str());
                        m_status = "Loaded " + name;
                    } catch (const std::exception& e) {
                        m_status = std::string("Load failed: ") + e.what();
                    }
                }
            }
        }
        ImGui::TreePop();
    }

    if (!m_status.empty()) {
        ImGui::TextWrapped("%s", m_status.c_str());
    }

    ImGui::End();
}

} // namespace engine
