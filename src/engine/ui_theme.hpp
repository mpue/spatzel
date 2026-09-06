#pragma once

// Editor UI chrome kept out of application.cpp: the ImGui colour/metric theme and
// the location of the persisted layout file. Both are pure ImGui/OS concerns with
// no engine state, so they live on their own rather than bloating the app object.

#include <string>

namespace engine {

// Apply the editor's dark theme to the current ImGui style. Sets the *unscaled*
// base metrics and colours; the caller multiplies by the display DPI afterwards
// (ImGui::GetStyle().ScaleAllSizes).
void applyEditorTheme();

// Absolute path where ImGui persists the docking layout (imgui.ini), under the
// user's config directory — %APPDATA%\fitzel on Windows, $XDG_CONFIG_HOME or
// ~/.config/fitzel elsewhere — never the project tree. The directory is created
// if missing. Returns empty if no writable location could be determined, in which
// case the caller should disable persistence.
std::string editorIniPath();

} // namespace engine
