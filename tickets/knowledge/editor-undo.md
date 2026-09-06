# Editor undo/redo

Implemented in `src/engine/editor.cpp` (`snapshot`, `pushUndo`, `restore`,
`applyUndo`, `applyRedo`, `handleUndoShortcuts`); declared in `editor.hpp`.

## Model

- `Editor::Snapshot` = whole edit list + whole `AnimationClip`. Both travel
  together: a scene load, an L-system generate and a keyframe edit all change
  both, and restoring only one half leaves tracks describing objects that are
  no longer there.
- Two stacks (`m_undo`, `m_redo`), capped at `kMaxUndo` (128, in
  `editor_internal.hpp`). `pushUndo` clears the redo stack.
- `m_anim` is a bare pointer to the clip, set at the top of `draw()` each frame,
  so a snapshot taken deep inside a panel can capture the clip without every
  panel signature growing an `AnimationClip&`.
- `restore()` is undo and redo in one: push the live state onto the other stack,
  pop the wanted one, clamp the selection, drop the timeline key selection,
  and set `sceneChanged | bakeMeasure` so the engine re-uploads and re-bakes.

## Conventions at the call sites

- Instant actions (add, delete, type/operator change, generate, load, add/delete
  keyframe): `pushUndo(scene)` **before** the mutation.
- Drags and typed fields: `m_preEdit = snapshot(scene)` on
  `ImGui::IsItemActivated()`, `pushUndo(m_preEdit)` on
  `IsItemDeactivatedAfterEdit()` — one entry per edit session, not per frame.
- `ImGui::Checkbox` writes the bool before returning, so the "Linear" toggle in
  the timeline flips it back, snapshots, and flips it again.
- `loadScene(path, &anim)` writes the clip in place, so the snapshot has to be
  taken **before** the call and pushed only after it succeeded — otherwise the
  entry pairs the old scene with the newly loaded animation.

## Shortcuts

`handleUndoShortcuts` runs once per frame in `draw()`, after `buildGizmo` (so a
drag ending this frame has committed its own entry) and before the panels (so
they build from the restored state). Ctrl+Z undo, Ctrl+Y or Ctrl+Shift+Z redo.
Suppressed while `io.WantTextInput`, while a gizmo drag is running
(`m_gizmoUsing`) and while a timeline drag is running (`m_tlDrag != 0`).

## Deliberately not on the stack

- `LightingSettings` and `RenderSettings` (exposure, reflection samples, grid
  resolution) — view/lighting settings, not scene edits.
- `AnimationState` (playhead, playing, `nextId`) — playback state. An undone
  "Add key" does not roll `nextId` back; ids simply skip a value.
- During playback `sampleInto` overwrites animated poses every frame, so undoing
  a transform while the clip plays has no visible effect.
