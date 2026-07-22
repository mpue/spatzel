#include "engine/editor.hpp"

#include "engine/scene_io.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <exception>
#include <system_error>

namespace engine {
namespace {

constexpr std::array<const char*, 4> kTypeNames{"Sphere", "Box", "Torus", "Plane"};
constexpr std::array<const char*, 4> kOperatorNames{"Union", "SmoothUnion", "Subtract",
                                                    "Intersect"};

constexpr size_t kMaxUndo = 128;

const char* typeLabel(int32_t type) {
    return (type >= 0 && type < 4) ? kTypeNames[static_cast<size_t>(type)] : "?";
}

GpuPrimitive makeDefault(PrimitiveType type) {
    GpuPrimitive p{};              // rotation defaults to identity, albedo to grey
    p.position[1] = 1.0f;          // lifted off the ground so it is visible
    p.albedo[0]   = 0.80f;
    p.albedo[1]   = 0.70f;
    p.albedo[2]   = 0.45f;
    p.control[0]  = static_cast<int32_t>(type);
    p.control[1]  = static_cast<int32_t>(Operator::Union);
    switch (type) {
        case PrimitiveType::Sphere:
            p.params[0] = 0.5f;
            break;
        case PrimitiveType::Box:
            p.params[0] = p.params[1] = p.params[2] = 0.5f;
            p.params[3] = 0.05f;
            break;
        case PrimitiveType::Torus:
            p.params[0] = 0.6f;
            p.params[1] = 0.2f;
            break;
        case PrimitiveType::Plane:
            p.position[1] = 0.0f;
            p.params[1]   = 1.0f; // unit normal pointing up
            break;
    }
    return p;
}

void normaliseQuat(float q[4]) {
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len > 1e-6f) {
        for (int i = 0; i < 4; ++i) {
            q[i] /= len;
        }
    } else {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
    }
}

} // namespace

void Editor::pushUndo(std::vector<GpuPrimitive> state) {
    m_undo.push_back(std::move(state));
    if (m_undo.size() > kMaxUndo) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
}

void Editor::clampSelection(const std::vector<GpuPrimitive>& scene) {
    if (scene.empty()) {
        m_selected = -1;
    } else {
        m_selected = std::clamp(m_selected, 0, static_cast<int>(scene.size()) - 1);
    }
}

EditorActions Editor::draw(std::vector<GpuPrimitive>& scene, RendererMode& renderer,
                           bool brickAvailable, const EditorStats& stats,
                           const std::filesystem::path& sceneDir) {
    EditorActions actions;
    clampSelection(scene);

    // Marks a live value change this frame, and — separately — a committed edit
    // (a finished drag or a typed-and-confirmed value), which is what an undo
    // entry and a timed re-bake key off. Snapshots the pre-edit scene when a
    // field is first touched so one edit session is one undo entry.
    auto track = [&]() {
        if (ImGui::IsItemActivated()) {
            m_preEdit = scene;
        }
        if (ImGui::IsItemEdited()) {
            actions.sceneChanged = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            pushUndo(m_preEdit);
            actions.bakeMeasure = true;
        }
    };

    ImGui::SetNextWindowSize(ImVec2(340, 640), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene Editor");

    // --- status / info -----------------------------------------------------
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

    // --- undo / redo -------------------------------------------------------
    ImGui::BeginDisabled(m_undo.empty());
    if (ImGui::Button("Undo")) {
        m_redo.push_back(scene);
        scene = std::move(m_undo.back());
        m_undo.pop_back();
        clampSelection(scene);
        actions.sceneChanged = actions.bakeMeasure = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_redo.empty());
    if (ImGui::Button("Redo")) {
        m_undo.push_back(scene);
        scene = std::move(m_redo.back());
        m_redo.pop_back();
        clampSelection(scene);
        actions.sceneChanged = actions.bakeMeasure = true;
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- primitive list ----------------------------------------------------
    ImGui::TextUnformatted("Primitives");
    if (ImGui::BeginListBox("##primitives", ImVec2(-FLT_MIN, 140))) {
        for (int i = 0; i < static_cast<int>(scene.size()); ++i) {
            char label[64];
            std::snprintf(label, sizeof(label), "%d: %s##%d", i,
                          typeLabel(scene[static_cast<size_t>(i)].control[0]), i);
            if (ImGui::Selectable(label, m_selected == i)) {
                m_selected = i;
            }
        }
        ImGui::EndListBox();
    }

    // --- add / delete ------------------------------------------------------
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("##addtype", &m_addType, kTypeNames.data(),
                 static_cast<int>(kTypeNames.size()));
    ImGui::SameLine();
    ImGui::BeginDisabled(stats.primitiveCount >= stats.maxPrimitives);
    if (ImGui::Button("Add")) {
        pushUndo(scene);
        scene.push_back(makeDefault(static_cast<PrimitiveType>(m_addType)));
        m_selected           = static_cast<int>(scene.size()) - 1;
        actions.sceneChanged = actions.bakeMeasure = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_selected < 0);
    if (ImGui::Button("Delete")) {
        pushUndo(scene);
        scene.erase(scene.begin() + m_selected);
        clampSelection(scene);
        actions.sceneChanged = actions.bakeMeasure = true;
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- selected primitive ------------------------------------------------
    if (m_selected >= 0 && m_selected < static_cast<int>(scene.size())) {
        GpuPrimitive& p = scene[static_cast<size_t>(m_selected)];

        // Type. Changing it resets the parameters to that type's defaults,
        // since the params mean different things per type.
        int type = p.control[0];
        if (ImGui::Combo("Type", &type, kTypeNames.data(), static_cast<int>(kTypeNames.size())) &&
            type != p.control[0]) {
            pushUndo(scene);
            GpuPrimitive fresh = makeDefault(static_cast<PrimitiveType>(type));
            // Keep placement and material; reset only the type-specific params.
            std::copy(std::begin(p.position), std::end(p.position), std::begin(fresh.position));
            std::copy(std::begin(p.rotation), std::end(p.rotation), std::begin(fresh.rotation));
            std::copy(std::begin(p.albedo), std::end(p.albedo), std::begin(fresh.albedo));
            fresh.control[1] = p.control[1];
            p                = fresh;
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
        }

        ImGui::ColorEdit3("Albedo", p.albedo);
        track();
    } else {
        ImGui::TextDisabled("No primitive selected");
    }

    ImGui::Separator();

    // --- save / load -------------------------------------------------------
    ImGui::TextUnformatted("Scene file");
    ImGui::InputText("##filename", m_fileName, sizeof(m_fileName));

    const auto resolve = [&](const char* name) {
        std::filesystem::path p(name);
        return p.is_absolute() ? p : sceneDir / p;
    };

    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        try {
            saveScene(resolve(m_fileName), scene);
            m_status = std::string("Saved ") + m_fileName;
        } catch (const std::exception& e) {
            m_status = std::string("Save failed: ") + e.what();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        try {
            std::vector<GpuPrimitive> loaded = loadScene(resolve(m_fileName));
            pushUndo(scene);
            scene = std::move(loaded);
            clampSelection(scene);
            actions.sceneChanged = actions.bakeMeasure = true;
            m_status = std::string("Loaded ") + m_fileName;
        } catch (const std::exception& e) {
            m_status = std::string("Load failed: ") + e.what();
        }
    }

    // Example scenes: every .json beside the executable's scenes/ directory,
    // one click to load. This is where the committed sample scenes surface.
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
                        std::vector<GpuPrimitive> loaded = loadScene(entry.path());
                        pushUndo(scene);
                        scene = std::move(loaded);
                        clampSelection(scene);
                        actions.sceneChanged = actions.bakeMeasure = true;
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
    return actions;
}

} // namespace engine
