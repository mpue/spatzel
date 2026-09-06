# Project knowledge (fitzel)

Shared notes for every ticket in this project. Keep this index short; split
detail into `knowledge/<topic>.md` and link it here.

## Build

- No `cmake` on PATH. Use the VS-bundled one:
  `C:\Program Files\Microsoft Visual Studio\18\Insiders\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`
  Generator is `Visual Studio 18 2026`, multi-config, build tree in `build/`.
  `& $cmake --build build --config Debug --target fitzel` (also `Release`).
  `build.ps1` / `build.cmd` are the wrappers the project ships.
- Pre-existing warning C5054 at `src/engine/editor.cpp:123` (mixed ImGui enum
  `|`). Not a regression; ignore it.
- Running the app needs a real window/GPU, so agent sessions verify by building
  and reading code, not by clicking.

## Architecture

- The edit list (`std::vector<GpuPrimitive>`, `src/engine/scene.hpp`) is the
  single source of truth. No second scene representation, no scale transform —
  the editor edits per-type dimension params instead.
- `Editor::draw` mutates the scene, the `AnimationClip` and the settings structs
  in place and returns an `EditorActions` telling the engine what to re-upload
  or re-bake (`application.cpp`, around the `m_editor.draw` call).
- The editor is split by panel: `editor.cpp` (core/selection/undo/dockspace),
  `editor_panels.cpp`, `editor_gizmo.cpp`, `editor_timeline.cpp`,
  `editor_lighting.cpp`, `editor_lsystem.cpp`; helpers in
  `editor_internal.hpp`.

## Topics

- [Editor undo/redo](knowledge/editor-undo.md) — snapshot stack, what is covered
  and what deliberately is not.
