# Undo geht nicht

Status: done
Id: 2026-08-28-002
Created: 2026-08-28T17:54:38.625Z
Tokens: 2674040
InputTokens: 2528970
OutputTokens: 29014
Cost: 2.8505
Context: 35819
ContextLimit: 1000000
ClosedAt: 2026-08-28T18:02:19.093Z
AgentCompletedAt: 2026-08-28T18:02:19.093Z



=== DESCRIPTION ===

++ [ticket] Undo geht nicht
ich habe keine Undo Funktion



=== HISTORY ===

$$> Not logged in · Please run /login

~~> Undo/Redo gibt es jetzt als Tastenkürzel und für Animation mit; vorher lagen die beiden Buttons nur unbeschriftet oben im Primitives-Panel und Keyframe-Änderungen waren gar nicht erfasst.

- `Editor::handleUndoShortcuts` (`src/engine/editor.cpp`) läuft einmal pro Frame in `draw()`: Ctrl+Z = Undo, Ctrl+Y bzw. Ctrl+Shift+Z = Redo. Gesperrt während Texteingabe, laufendem Gizmo-Drag und laufendem Timeline-Drag.
- Die Buttons heißen jetzt "Undo (Ctrl+Z)" / "Redo (Ctrl+Y)" und darunter steht der Füllstand beider Stapel; sie rufen dieselbe Funktion wie die Tastenkürzel auf (`editor_panels.cpp`).
- Ein Undo-Schritt ist jetzt Editliste **plus** `AnimationClip` (`Editor::Snapshot`). Neu erfasst: Add key, Add cam key, Delete key (Button und Entf-Taste), Keyframe-Verschieben im Dope Sheet, Duration und Linear (`editor_timeline.cpp`).
- Beim Szenen-Laden wurde der Snapshot bisher erst nach `loadScene` genommen — das hätte die alte Editliste mit der schon geladenen Animation gepaart. Snapshot wird jetzt vorher genommen und erst nach erfolgreichem Laden abgelegt.

Nicht auf dem Stapel: Lighting, Exposure/Reflection Samples/Grid-Auflösung und der Abspielzustand — das sind Ansichts- statt Szenenänderungen.

Geprüft: Debug und Release bauen sauber (`cmake --build build --config …`, VS-eigenes cmake). Ein Klick-Durchgang in der laufenden App fehlt — hier lohnt der Blick auf Ctrl+Z direkt nach einem Gizmo-Drag und nach einem Keyframe-Verschieben.
