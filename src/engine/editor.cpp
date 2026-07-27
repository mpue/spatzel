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

constexpr std::array<const char*, 5> kTypeNames{"Sphere", "Box", "Torus", "Plane", "Round Cone"};
constexpr std::array<const char*, 4> kOperatorNames{"Union", "SmoothUnion", "Subtract",
                                                    "Intersect"};

constexpr size_t kMaxUndo = 128;

const char* typeLabel(int32_t type) {
    return (type >= 0 && type < static_cast<int32_t>(kTypeNames.size()))
               ? kTypeNames[static_cast<size_t>(type)]
               : "?";
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
        case PrimitiveType::RoundCone:
            p.params[0] = 1.0f;  // height
            p.params[1] = 0.15f; // base radius
            p.params[2] = 0.08f; // tip radius
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
            std::copy(std::begin(p.material), std::end(p.material), std::begin(fresh.material));
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
            case PrimitiveType::RoundCone:
                ImGui::DragFloat("Height", &p.params[0], 0.01f, 0.0f, 100.0f);
                track();
                ImGui::DragFloat("Base radius", &p.params[1], 0.005f, 0.0f, 100.0f);
                track();
                ImGui::DragFloat("Tip radius", &p.params[2], 0.005f, 0.0f, 100.0f);
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
    } else {
        ImGui::TextDisabled("No primitive selected");
    }

    ImGui::Separator();

    // --- vegetation generator ---------------------------------------------
    buildLSystemPanel(scene, stats, actions);

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

LSystemConfig Editor::lsysConfig() const {
    LSystemConfig c;
    c.axiom = m_lsys.axiom;
    for (int i = 0; i < kLsysMaxRules; ++i) {
        if (m_lsys.rulePred[i][0] != '\0' && m_lsys.ruleSucc[i][0] != '\0') {
            c.rules.push_back(LRule{m_lsys.rulePred[i][0], m_lsys.ruleSucc[i]});
        }
    }
    c.iterations    = m_lsys.iterations;
    c.angleDeg      = m_lsys.angleDeg;
    c.segmentLength = m_lsys.segmentLength;
    c.lengthTaper   = m_lsys.lengthTaper;
    c.baseRadius    = m_lsys.baseRadius;
    c.radiusTaper   = m_lsys.radiusTaper;
    c.leafSize      = m_lsys.leafSize;
    c.leaves        = m_lsys.leaves;
    c.tropism       = m_lsys.tropism;
    c.seed          = static_cast<std::uint32_t>(m_lsys.seed);
    c.jitter        = m_lsys.jitter;
    c.basePosition  = {m_lsys.basePos[0], m_lsys.basePos[1], m_lsys.basePos[2]};
    c.branchColour  = {m_lsys.branchColour[0], m_lsys.branchColour[1], m_lsys.branchColour[2]};
    c.leafColour    = {m_lsys.leafColour[0], m_lsys.leafColour[1], m_lsys.leafColour[2]};
    return c;
}

void Editor::buildLSystemPanel(std::vector<GpuPrimitive>& scene, const EditorStats& stats,
                               EditorActions& actions) {
    if (!ImGui::CollapsingHeader("Vegetation (L-System)")) {
        return;
    }

    const auto clearRules = [&]() {
        for (int i = 0; i < kLsysMaxRules; ++i) {
            m_lsys.rulePred[i][0] = '\0';
            m_lsys.ruleSucc[i][0] = '\0';
        }
    };
    const auto setRule = [&](int i, const char* pred, const char* succ) {
        std::snprintf(m_lsys.rulePred[i], sizeof(m_lsys.rulePred[i]), "%s", pred);
        std::snprintf(m_lsys.ruleSucc[i], sizeof(m_lsys.ruleSucc[i]), "%s", succ);
    };

    // Presets seed the whole config; "Custom" leaves the fields as they are so a
    // user can dial in their own grammar without a preset overwriting it.
    static constexpr std::array<const char*, 4> kPresetNames{"Custom", "Plant", "Bush", "Tree (3D)"};
    if (ImGui::Combo("Preset", &m_lsys.preset, kPresetNames.data(),
                     static_cast<int>(kPresetNames.size()))) {
        switch (m_lsys.preset) {
            case 1: // Plant — the classic planar fractal plant, softened by jitter
                std::snprintf(m_lsys.axiom, sizeof(m_lsys.axiom), "%s", "X");
                clearRules();
                setRule(0, "X", "F+[[X]-X]-F[-FX]+X");
                setRule(1, "F", "FF");
                m_lsys.iterations = 3;   m_lsys.angleDeg = 25.0f;
                m_lsys.segmentLength = 0.28f; m_lsys.lengthTaper = 0.90f;
                m_lsys.baseRadius = 0.06f; m_lsys.radiusTaper = 0.74f;
                m_lsys.leafSize = 0.09f; m_lsys.leaves = true;
                m_lsys.tropism = 0.04f;  m_lsys.jitter = 0.22f;
                break;
            case 2: // Bush — a rounder, denser plant. Grows ~8x per pass, so it
                    // stays at 3 iterations to fit the primitive budget.
                std::snprintf(m_lsys.axiom, sizeof(m_lsys.axiom), "%s", "F");
                clearRules();
                setRule(0, "F", "FF-[-F+F+F]+[+F-F-F]");
                m_lsys.iterations = 3;   m_lsys.angleDeg = 22.0f;
                m_lsys.segmentLength = 0.22f; m_lsys.lengthTaper = 0.88f;
                m_lsys.baseRadius = 0.06f; m_lsys.radiusTaper = 0.78f;
                m_lsys.leafSize = 0.11f; m_lsys.leaves = true;
                m_lsys.tropism = 0.03f;  m_lsys.jitter = 0.30f;
                break;
            case 3: // Tree (3D) — pitched sub-branches rolled apart into 3D.
                    // Leaves are placed automatically at the twig tips.
                std::snprintf(m_lsys.axiom, sizeof(m_lsys.axiom), "%s", "A");
                clearRules();
                setRule(0, "A", "F[&FA]/////[&FA]///////[&FA]");
                setRule(1, "F", "FF");
                m_lsys.iterations = 4;   m_lsys.angleDeg = 26.0f;
                m_lsys.segmentLength = 0.26f; m_lsys.lengthTaper = 0.90f;
                m_lsys.baseRadius = 0.16f; m_lsys.radiusTaper = 0.72f;
                m_lsys.leafSize = 0.16f; m_lsys.leaves = true;
                m_lsys.tropism = 0.05f;  m_lsys.jitter = 0.24f;
                break;
            default:
                break; // Custom
        }
    }

    ImGui::InputText("Axiom", m_lsys.axiom, sizeof(m_lsys.axiom));
    ImGui::TextDisabled("Rules (predecessor -> successor)");
    for (int i = 0; i < kLsysMaxRules; ++i) {
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(24.0f);
        if (ImGui::InputText("##pred", m_lsys.rulePred[i], sizeof(m_lsys.rulePred[i]))) {
            m_lsys.preset = 0;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("->");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##succ", m_lsys.ruleSucc[i], sizeof(m_lsys.ruleSucc[i]))) {
            m_lsys.preset = 0;
        }
        ImGui::PopID();
    }

    ImGui::SliderInt("Iterations", &m_lsys.iterations, 0, 8);
    ImGui::SliderFloat("Angle", &m_lsys.angleDeg, 0.0f, 90.0f, "%.1f deg");
    ImGui::DragFloat("Segment len", &m_lsys.segmentLength, 0.005f, 0.01f, 5.0f);
    ImGui::SliderFloat("Length taper", &m_lsys.lengthTaper, 0.40f, 1.0f);
    ImGui::DragFloat("Base radius", &m_lsys.baseRadius, 0.002f, 0.005f, 2.0f);
    ImGui::SliderFloat("Radius taper", &m_lsys.radiusTaper, 0.50f, 1.0f);
    ImGui::SliderFloat("Tropism", &m_lsys.tropism, -0.30f, 0.30f);
    ImGui::Checkbox("Leaves", &m_lsys.leaves);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("Leaf size", &m_lsys.leafSize, 0.005f, 0.01f, 1.0f);
    ImGui::DragInt("Seed", &m_lsys.seed, 0.1f, 0, 100000);
    ImGui::SliderFloat("Jitter", &m_lsys.jitter, 0.0f, 1.0f);
    ImGui::DragFloat3("Base pos", m_lsys.basePos, 0.02f);
    ImGui::ColorEdit3("Branch col", m_lsys.branchColour);
    ImGui::ColorEdit3("Leaf col", m_lsys.leafColour);

    // Live estimate: expand the grammar (string-only, cheap) and count the draw
    // symbols, so the primitive cost is visible before committing to Generate.
    const LSystemConfig cfg      = lsysConfig();
    const std::string   expanded = lsystemExpand(cfg);
    const int           estimate = lsystemPrimitiveCount(expanded, cfg);
    const int           freeSlots = stats.maxPrimitives - stats.primitiveCount;
    const bool          overflow = estimate > freeSlots;

    if (overflow) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                           "~ %d primitives (only %d free)", estimate, freeSlots);
    } else {
        ImGui::Text("~ %d primitives (%d free)", estimate, freeSlots);
    }

    // Same capacity guard the Add button uses — never let a bulk insert be
    // silently truncated by uploadScene.
    ImGui::BeginDisabled(overflow || estimate == 0);
    if (ImGui::Button("Generate")) {
        pushUndo(scene);
        const std::vector<GpuPrimitive> plant = lsystemBuild(expanded, cfg);
        scene.insert(scene.end(), plant.begin(), plant.end());
        clampSelection(scene);
        actions.sceneChanged = actions.bakeMeasure = true;
        m_status = "Generated " + std::to_string(plant.size()) + " primitives";
    }
    ImGui::EndDisabled();
}

} // namespace engine
