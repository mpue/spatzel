#include "engine/ui_theme.hpp"

#include <imgui.h>

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace engine {

// A hand-tuned dark theme for the editor: soft near-black panels, a single warm
// amber accent, and generously rounded corners with roomy padding so the panels
// read as a modern tool rather than the stock ImGui grey. All metrics are the
// unscaled base values; the caller multiplies them by the display's DPI scale.
void applyEditorTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 8.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 5.0f;
    s.PopupRounding     = 6.0f;
    s.GrabRounding      = 5.0f;
    s.TabRounding       = 5.0f;
    s.ScrollbarRounding = 6.0f;

    s.WindowPadding    = ImVec2(14.0f, 12.0f);
    s.FramePadding     = ImVec2(10.0f, 6.0f);
    s.ItemSpacing      = ImVec2(10.0f, 8.0f);
    s.ItemInnerSpacing = ImVec2(8.0f, 6.0f);
    s.ScrollbarSize    = 13.0f;
    s.GrabMinSize      = 11.0f;

    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize  = 0.0f;
    s.WindowTitleAlign = ImVec2(0.02f, 0.5f);

    const ImVec4 accent     = ImVec4(0.96f, 0.62f, 0.20f, 1.00f); // warm amber
    const ImVec4 accentDim  = ImVec4(0.96f, 0.62f, 0.20f, 0.42f);
    const ImVec4 accentSoft = ImVec4(0.96f, 0.62f, 0.20f, 0.22f);

    ImVec4* c                          = s.Colors;
    c[ImGuiCol_Text]                   = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]           = ImVec4(0.48f, 0.49f, 0.54f, 1.00f);
    c[ImGuiCol_WindowBg]               = ImVec4(0.09f, 0.09f, 0.11f, 0.98f);
    c[ImGuiCol_ChildBg]                = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]                = ImVec4(0.11f, 0.11f, 0.13f, 0.98f);
    c[ImGuiCol_Border]                 = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_FrameBg]                = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]         = ImVec4(0.22f, 0.22f, 0.26f, 1.00f);
    c[ImGuiCol_FrameBgActive]          = ImVec4(0.26f, 0.26f, 0.31f, 1.00f);
    c[ImGuiCol_TitleBg]                = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]          = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]       = ImVec4(0.07f, 0.07f, 0.09f, 0.80f);
    c[ImGuiCol_MenuBarBg]              = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_ScrollbarBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]          = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]   = ImVec4(0.36f, 0.36f, 0.41f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]    = accent;
    c[ImGuiCol_CheckMark]              = accent;
    c[ImGuiCol_SliderGrab]             = accent;
    c[ImGuiCol_SliderGrabActive]       = ImVec4(1.00f, 0.72f, 0.34f, 1.00f);
    c[ImGuiCol_Button]                 = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered]          = ImVec4(0.27f, 0.27f, 0.32f, 1.00f);
    c[ImGuiCol_ButtonActive]           = accentDim;
    c[ImGuiCol_Header]                 = accentSoft;
    c[ImGuiCol_HeaderHovered]          = accentDim;
    c[ImGuiCol_HeaderActive]           = accentDim;
    c[ImGuiCol_Separator]              = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    c[ImGuiCol_SeparatorHovered]       = accentDim;
    c[ImGuiCol_SeparatorActive]        = accent;
    c[ImGuiCol_ResizeGrip]             = ImVec4(1.00f, 1.00f, 1.00f, 0.06f);
    c[ImGuiCol_ResizeGripHovered]      = accentDim;
    c[ImGuiCol_ResizeGripActive]       = accent;
    c[ImGuiCol_Tab]                    = ImVec4(0.14f, 0.14f, 0.17f, 1.00f);
    c[ImGuiCol_TabHovered]             = accentDim;
    c[ImGuiCol_TabSelected]            = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_TabDimmed]              = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]      = ImVec4(0.16f, 0.16f, 0.19f, 1.00f);
    c[ImGuiCol_TextSelectedBg]         = accentDim;
    c[ImGuiCol_NavCursor]              = accent;
    // Docking chrome.
    c[ImGuiCol_DockingPreview]         = accentDim;
    c[ImGuiCol_DockingEmptyBg]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
}

std::string editorIniPath() {
    namespace fs = std::filesystem;

    // Pick the platform config root, preferring an explicit override.
    fs::path root;
    if (const char* appdata = std::getenv("APPDATA")) { // Windows
        root = appdata;
    } else if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        root = xdg;
    } else if (const char* home = std::getenv("HOME")) {
        root = fs::path(home) / ".config";
    } else {
        return {}; // no writable location; caller disables persistence
    }

    const fs::path dir = root / "fitzel";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return {};
    }
    return (dir / "imgui.ini").string();
}

} // namespace engine
