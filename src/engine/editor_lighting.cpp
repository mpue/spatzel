#include "engine/editor.hpp"

#include <imgui.h>

namespace engine {

void Editor::buildLightingPanel(LightingSettings& l, EditorActions& actions) {
    if (!ImGui::Begin("Lighting")) {
        ImGui::End();
        return;
    }

    // Any widget touched this frame re-uploads the (tiny) light buffer. Live, so
    // dragging a slider or scrubbing a colour updates the image immediately.
    const auto mark = [&]() {
        if (ImGui::IsItemEdited()) {
            actions.lightingChanged = true;
        }
    };

    constexpr auto kPicker = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float;

    ImGui::SeparatorText("Key light (directional)");
    ImGui::SliderFloat("Azimuth", &l.keyAzimuthDeg, -180.0f, 180.0f, "%.1f deg");
    mark();
    ImGui::SliderFloat("Elevation", &l.keyElevationDeg, 0.0f, 90.0f, "%.1f deg");
    mark();
    ImGui::ColorEdit3("Key colour", l.keyColour, kPicker);
    mark();
    ImGui::DragFloat("Key intensity", &l.keyIntensity, 0.02f, 0.0f, 20.0f, "%.2f");
    mark();

    ImGui::SeparatorText("Point light");
    ImGui::DragFloat3("Position", l.pointPosition, 0.02f);
    mark();
    ImGui::ColorEdit3("Point colour", l.pointColour, kPicker);
    mark();
    ImGui::DragFloat("Point intensity", &l.pointIntensity, 0.5f, 0.0f, 2000.0f, "%.1f",
                     ImGuiSliderFlags_Logarithmic);
    mark();

    ImGui::SeparatorText("Ambient (hemisphere)");
    ImGui::ColorEdit3("Sky", l.ambientSky, kPicker);
    mark();
    ImGui::ColorEdit3("Ground", l.ambientGround, kPicker);
    mark();
    ImGui::SliderFloat("Ambient strength", &l.ambientStrength, 0.0f, 4.0f, "%.2f");
    mark();

    ImGui::SeparatorText("Background");
    ImGui::ColorEdit3("Horizon", l.bgHorizon, kPicker);
    mark();
    ImGui::ColorEdit3("Zenith", l.bgZenith, kPicker);
    mark();

    ImGui::End();
}

} // namespace engine
