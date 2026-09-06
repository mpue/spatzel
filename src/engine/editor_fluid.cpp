#include "engine/editor.hpp"

#include <imgui.h>

#include <algorithm>

namespace engine {

// The Fluid panel. Two halves: the knobs, and the numbers that say what the
// knobs bought.
//
// The numbers are the point. A liquid solver has several places where "looks
// about right" and "is right" come apart — a pressure solve that stopped short
// still looks like water, a level set quietly losing volume still looks like
// water — so the residual divergence and the volume drift are shown next to the
// controls that cause them, rather than left to be discovered by staring at the
// image. Same argument as the brick renderer's comparison against the
// reference: a claim with a number behind it.
void Editor::buildFluidPanel(fluid::Settings& f, const EditorStats& stats,
                             EditorActions& actions) {
    if (!ImGui::Begin("Fluid")) {
        ImGui::End();
        return;
    }

    // A changed domain moves every cell centre, so the obstacle field — a sample
    // of the scene distance field at those centres — is stale.
    const auto markDomain = [&]() {
        if (ImGui::IsItemEdited()) {
            actions.fluidDomainMoved = true;
        }
    };

    ImGui::Checkbox("Enabled", &f.enabled);
    ImGui::SameLine();
    if (ImGui::Button("Reset water")) {
        actions.fluidReset = true;
    }
    ImGui::SetItemTooltip("Re-seed the level set from the dam box and pool level,\n"
                          "zero the velocity, and re-baseline the volume figure.");

    if (!f.enabled) {
        ImGui::TextDisabled("The solver allocates nothing while this is off.");
    }

    ImGui::SeparatorText("Domain");
    // Cubic and uniformly spaced on purpose: one cell size on all three axes is
    // what keeps the pressure stencil the same in every direction.
    if (ImGui::DragFloat3("Corner", &f.origin.x, 0.02f, -50.0f, 50.0f, "%.2f")) {
        actions.fluidDomainMoved = true;
    }
    ImGui::DragFloat("Size", &f.size, 0.05f, 0.5f, 40.0f, "%.2f");
    markDomain();
    ImGui::SetItemTooltip("Edge length of the tank, world units. The domain is a cube.");

    int res = f.res;
    if (ImGui::SliderInt("Resolution", &res, fluid::kMinRes, fluid::kMaxRes)) {
        // Changing the resolution reinterprets every index in every field, so
        // the solver re-seeds rather than resampling. Snapped to a multiple of
        // the workgroup size so the dispatch covers the grid exactly.
        f.res = std::clamp((res / 4) * 4, fluid::kMinRes, fluid::kMaxRes);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%.3f/cell)", static_cast<double>(f.cellSize()));

    ImGui::SeparatorText("Integration");
    ImGui::DragFloat("Timestep", &f.timestep, 0.0002f, 1.0f / 600.0f, 1.0f / 30.0f, "%.4f s");
    ImGui::SetItemTooltip("Fixed, not derived from the frame rate: a solver whose step\n"
                          "follows the frame rate produces a result that does too.");
    ImGui::SliderInt("Max substeps", &f.maxSubsteps, 1, 8);
    ImGui::DragFloat("Gravity", &f.gravity, 0.05f, -40.0f, 40.0f, "%.2f m/s^2");

    ImGui::SeparatorText("Solver");
    ImGui::SliderInt("Pressure sweeps", &f.pressureSweeps, 1, 240);
    ImGui::SetItemTooltip("Red-black Gauss-Seidel. One sweep moves information one cell,\n"
                          "so a tank `resolution` cells deep needs at least that many\n"
                          "before the bottom knows the surface is there. The residual it\n"
                          "actually reached is reported below.");
    if (f.pressureSweeps < f.res) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "< res");
    }
    ImGui::SliderInt("Extrapolation sweeps", &f.extrapolateSweeps, 0, 8);
    ImGui::SetItemTooltip("How far the fluid velocity reaches into the air, in cells.\n"
                          "Too few and the free surface goes flat.");
    ImGui::SliderInt("Redistancing", &f.reinitIterations, 0, 12);
    ImGui::SetItemTooltip("Restores the level set to a distance function. The renderer\n"
                          "sphere-traces this field, so zero here is not an option.");

    ImGui::SeparatorText("Seed");
    if (ImGui::DragFloat3("Dam min", &f.seedMin.x, 0.02f, -50.0f, 50.0f, "%.2f")) {
        actions.fluidReset = true;
    }
    if (ImGui::DragFloat3("Dam max", &f.seedMax.x, 0.02f, -50.0f, 50.0f, "%.2f")) {
        actions.fluidReset = true;
    }
    if (ImGui::DragFloat("Pool level", &f.poolLevel, 0.01f, -50.0f, 50.0f, "%.2f")) {
        actions.fluidReset = true;
    }
    ImGui::SetItemTooltip("Still water below this height, unioned with the dam box.");

    ImGui::SeparatorText("Appearance");
    ImGui::ColorEdit3("Tint", f.colour,
                      ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
    ImGui::SetItemTooltip("Beer-Lambert absorption colour: what a long path through\n"
                          "the water leaves of the light behind it.");
    ImGui::SliderFloat("Transmission", &f.transmission, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Roughness", &f.roughness, 0.0f, 0.4f, "%.3f");
    ImGui::SliderFloat("Index of refraction", &f.ior, 1.0f, 2.0f, "%.2f");
    ImGui::DragFloat("Surface offset", &f.surfaceOffset, 0.002f, -0.5f, 0.5f, "%.3f");
    ImGui::SetItemTooltip("Pushes the rendered surface out or in. A blunt counter to\n"
                          "the level set's volume loss — it moves the picture, not\n"
                          "the simulation.");
    ImGui::DragFloat("Trust band", &f.trustBandCells, 0.25f, 1.0f, 64.0f, "%.1f cells");
    ImGui::SetItemTooltip("How far the marchers trust the level set as a distance.\n"
                          "Lower is safer and costs march steps; higher is faster and\n"
                          "risks stepping through the surface.");

    ImGui::SeparatorText("Diagnostics");
    if (!stats.fluid.valid) {
        ImGui::TextDisabled("No measurement yet.");
    } else {
        ImGui::Text("Volume %.4f m^3", static_cast<double>(stats.fluid.volume));

        const float drift = stats.fluid.volumeDrift();
        // Level-set advection loses volume. A few percent over a run is the
        // method behaving as documented; tens of percent means the timestep or
        // the resolution is asking too much of it.
        const bool  heavy = drift < -0.10f;
        ImGui::TextColored(heavy ? ImVec4(0.95f, 0.65f, 0.25f, 1.0f)
                                 : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                           "Volume drift %+.1f %%", static_cast<double>(drift * 100.0f));
        ImGui::SetItemTooltip("Against the volume right after the last reset. Grid-only\n"
                              "advection of a level set loses water; particles (FLIP) are\n"
                              "the fix, and this grid is what they would sit on.");

        // The residual is quoted against a rate the cell size makes meaningful:
        // a divergence of d empties a cell in 1/d seconds, so d * h is a speed
        // and comparing it to the fastest velocity in the field is the scale-free
        // way to ask whether the solve converged.
        const float scale = f.cellSize() > 0.0f ? stats.fluid.maxDivergence * f.cellSize() : 0.0f;
        const float ratio = stats.fluid.maxSpeed > 1e-4f ? scale / stats.fluid.maxSpeed : 0.0f;
        const bool  loose = ratio > 0.05f;
        ImGui::TextColored(loose ? ImVec4(0.95f, 0.65f, 0.25f, 1.0f)
                                 : ImGui::GetStyleColorVec4(ImGuiCol_Text),
                           "Residual divergence %.4f 1/s  (%.1f %% of max speed)",
                           static_cast<double>(stats.fluid.maxDivergence),
                           static_cast<double>(ratio * 100.0f));
        ImGui::SetItemTooltip("What the Jacobi iteration count above actually achieved,\n"
                              "measured after the last projection of the frame. Raise the\n"
                              "iteration count until this stops mattering.");

        ImGui::Text("Max speed %.2f m/s", static_cast<double>(stats.fluid.maxSpeed));
    }

    ImGui::End();
}

} // namespace engine
