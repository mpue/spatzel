#include "engine/editor.hpp"

#include <imgui.h>

#include <array>
#include <cfloat>
#include <cstdio>
#include <string>

namespace engine {

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
    if (!ImGui::Begin("Vegetation")) {
        ImGui::End();
        return;
    }

    const float em = ImGui::GetFontSize();

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
        ImGui::SetNextItemWidth(em * 1.6f);
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
    ImGui::SetNextItemWidth(em * 5.6f);
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

    ImGui::End();
}

} // namespace engine
