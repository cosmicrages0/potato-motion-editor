# Design Doc — Commit 3: Undo/Redo + Keyframe Diamond Editing + Parent Fix

**Base commit:** `0b73fb0` (Task 5.2: Save/Load)
**LOC delta estimate:** +260 / -5
**User-visible change:** three real quality-of-life wins ship together
**Phase 1 completion commit** — after this we're done with foundations and can move to Phase 2 (Bezier per-key, per-layer effect RTs, Composition Settings, text)

---

## 1. What ships in this commit

Three unrelated-but-small fixes bundled because each is a natural completion of an existing subsystem, and none is big enough to justify its own commit.

### 1.1 Undo / Redo (`Ctrl+Z` / `Ctrl+Y`) — snapshot stack piggybacked on Task 5.2's serializer

Gemini + Claude both picked snapshot stack over command pattern. Rationale: the serializer we just built writes ~10-100 KB of JSON per full-scene snapshot. A 50-deep stack of those is single-digit MB — a rounding error on 4GB. Zero per-feature undo/redo code required forever.

**Design:**
```cpp
class UndoStack {
    std::deque<std::string> pastStates;    // JSON strings, most recent at back
    std::deque<std::string> futureStates;  // states that undo() peeled off; redo() puts them back
    size_t maxDepth = 50;

    void RecordSnapshot(const AppState& state);  // pushes current state to pastStates, clears futureStates
    bool Undo(AppState& state);                   // pop pastStates.back into state; push what was current onto futureStates
    bool Redo(AppState& state);                   // symmetric
};
```

**Coalescing rule (critical UX detail):** we do NOT snapshot on every mouse-move during a drag — that would push 60 states/sec onto the stack and make `Ctrl+Z` useless (each undo would rewind one frame of motion). Instead:

- Snapshot **on mouse-up after a drag** (gizmo drag ends, DragFloat3 releases)
- Snapshot **before a structural mutation** (AddLayer, DeleteLayer, SetParent, AddEffect, RemoveEffect)
- Snapshot **before opening a file** (so you can undo the open itself)

Implementation: `RenderEngine` gains a small `MarkForSnapshot()` method. UI code calls it at the right moments. `MarkForSnapshot()` just sets a `pendingSnapshot` bool; the actual snapshot happens at the top of the next frame (once, deduplicated). This keeps hot paths untouched.

**Ctrl+Z / Ctrl+Y wiring:** same pattern as Ctrl+S in `RenderUI()` — check `io.KeyCtrl + IsKeyPressed(ImGuiKey_Z)`.

**Memory ceiling defense:** if snapshotting fails (out of memory, unlikely), we log a warning and skip that snapshot rather than crashing.

### 1.2 Keyframe diamond editing in the timeline strip

Your original ask. Right now diamonds display but you can't select, drag, or delete them. AE-standard interactions:

- **Left-click** a diamond → selects it (highlighted color, shows time in a hover tooltip)
- **Left-click + drag** a selected diamond → slides it along the time ruler
- **Right-click** a diamond → context menu: `Delete Keyframe`
- **Ctrl+click** an empty area of a track row → adds a new keyframe at the clicked time (this is bonus, not user-asked)

**Design:**

Add per-track hit-testing inside `DrawTimelineStrip()`. Each diamond gets an `ImGui::InvisibleButton` at its position; hover/active/click states drive the UX. The keyframe belongs to an `AnimatedProperty<T>` where T varies per row — we need a small type-erased handle so the strip code doesn't care whether it's clicking a position diamond or an opacity diamond:

```cpp
// Internal to DrawTimelineStrip only. Not a public type.
struct DiamondHit {
    int         layerId = -1;
    enum { Position, Rotation, Scale, Opacity } which;
    int         keyIndex = -1;
    float       origTime = 0.0f;
};
```

Two RenderEngine members added:
```cpp
DiamondHit hoveredDiamond;      // updated every frame in DrawTimelineStrip
DiamondHit draggedDiamond;      // set on mouse-down; cleared on mouse-up
bool       diamondDragActive = false;
```

**Delete-by-right-click:** when active, use `AnimatedProperty<T>::RemoveKeyAt(time)` which already exists (added in Task 5.1). Wrapped in `MarkForSnapshot()` so undo works.

**Drag-to-move:** on mouse-move while `diamondDragActive`, compute the new time from mouse X, replace the keyframe by removing the old and inserting at new time. On mouse-up, `MarkForSnapshot()`.

**Bonus: Ctrl+click empty track to add key.** Uses `AnimatedProperty::SetValue(t, prop.Evaluate(t))` — samples the current value at that time (which for an unanimated property is `staticValue`, for an animated one is the interpolated value) and stamps it. Auto-enables the stopwatch if it wasn't on.

### 1.3 Parent dropdown fix (the 1-line bug)

Found while debugging your parenting complaint: the Timeline table's Name column has `Selectable(..., ImGuiSelectableFlags_SpanAllColumns)` which steals every click across the entire row, including clicks on the Parent dropdown. Fix: add `ImGuiSelectableFlags_AllowOverlap` so the Combo receives its own clicks.

**One-line change:**
```cpp
if (ImGui::Selectable(label, isSelected,
    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
```

Plus `ImGui::SetNextItemAllowOverlap()` before the Combo call so its hit-rect wins the overlap. Two lines total.

---

## 2. Files changing

```
src/UndoStack.h            NEW      ~55 lines  — snapshot stack + AppState (de)serialize helpers
src/UndoStack.cpp          NEW      ~90 lines  — reuses Serialization.cpp helpers via string round-trip
src/Serialization.h        MODIFIED +6 lines   — expose SaveProjectToString / LoadProjectFromString
src/Serialization.cpp      MODIFIED +30 lines  — refactor Save/Load to route through the string versions
src/RenderEngine.h         MODIFIED +12 lines  — UndoStack member, DiamondHit types, pendingSnapshot bool
src/RenderEngine.cpp       MODIFIED +180 lines — Ctrl+Z/Y shortcuts, MarkForSnapshot placement,
                                                  DrawTimelineStrip diamond hit-test + drag + right-click menu,
                                                  parent-dropdown AllowOverlap fix
```

Total: **~260 new LOC**, ~5 lines removed. Two new files that don't touch existing modules structurally.

---

## 3. The `AppState` round-trip trick (why this is clean)

`UndoStack` doesn't need its own serializer — it reuses Task 5.2's. New helpers on `Serialization.h`:

```cpp
bool SaveProjectToString(const AppState& state, std::string& outJson, std::string* outError = nullptr);
bool LoadProjectFromString(AppState& state, const std::string& inJson, std::string* outError = nullptr);
```

The existing `SaveProject(path)` becomes `SaveProjectToString + write to file`. The existing `LoadProject(path)` becomes `read file + LoadProjectFromString`. Net-zero behavior change, but now `UndoStack::RecordSnapshot` can call `SaveProjectToString` to get a `std::string`, push it onto the deque, and `Undo`/`Redo` can call `LoadProjectFromString` to restore.

**Cost per snapshot:** ~5-50 KB for a typical scene. 50-deep stack = 250 KB-2.5 MB. Trivial.

---

## 4. When exactly does a snapshot happen?

This is the most important detail because "when to record" is what determines whether Ctrl+Z feels right. Here's the exact list:

**BEFORE** each of these mutations, `MarkForSnapshot()` is called:
- `layerManager.AddLayer(...)` — from menu, palette button, or drag-import
- `layerManager.DeleteLayerById(...)` — from Delete key, menu, or Timeline button
- `layerManager.SetParent(...)` — from Parent dropdown
- `layer.AddEffect(...)` / `layer.RemoveEffectById(...)` / `layer.MoveEffect(...)`
- `AnimatedProperty::ToggleStopwatch(...)` (stopwatch on/off is a structural change)
- Loading a project (`LoadProject`)

**AFTER** each of these interactions completes, `MarkForSnapshot()` is called (recording the resulting state):
- Gizmo drag ends (mouse released after drag) — one snapshot per drag, not one per frame
- Inspector `DragFloat3` / `SliderFloat` widget deactivates (`ImGui::IsItemDeactivatedAfterEdit()`)
- Keyframe diamond drag ends
- Right-click delete-keyframe context menu action

**NEVER** snapshot for:
- Playhead scrubbing (no state change; only currentTime moved)
- Composition Clock Play/Pause (session state, not project state)
- Panel resize / dock rearrange (imgui.ini handles)
- Window resize / camera orbit (camera moves ARE snapshotted, but only if a Camera layer exists and its position was mutated)

The `MarkForSnapshot` → actually-snapshot-at-next-frame-top pattern means multiple calls in a single frame coalesce to one snapshot. If the user does something that triggers 3 marks in one frame (e.g. clicking a button that adds a layer, sets its parent, and adds an effect), you get ONE undo-able state.

---

## 5. Edge cases

| Case | Behavior |
|---|---|
| Undo with empty past stack | No-op, no error, status banner "Nothing to undo" |
| Redo with empty future stack | Same |
| Undo, then modify anything | `futureStates` is cleared (standard undo semantics — you can't redo past a diverging edit) |
| Snapshot fails to serialize (unlikely) | Log warning, don't crash, past stack unchanged |
| 51st snapshot | Oldest state dropped, newest appended |
| Undo restores a state whose composition size differs from the current one | RenderEngine detects the mismatch and recreates compRT/effect ping-pong (same code path as Load) |
| Delete a keyframe from a track that had only one key | Track goes empty; stopwatch stays lit (property is animatable but static); matches AE |
| Drag a keyframe past another key's time | Reorders; both remain (SetKey uses time-tolerance to avoid duplicates) |
| Drag a keyframe past the composition duration | Clamped to duration (silently); user can extend duration if they want more range |
| Right-click a diamond, but no context menu opens | Should ONLY happen if the diamond is not actually under the cursor — hit rect is 12px square around center |

---

## 6. Test plan

Manual pass I'll run before committing:

1. Add a rectangle, verify it appears. `Ctrl+Z` — rectangle disappears. `Ctrl+Y` — reappears
2. Enable position stopwatch, add 3 keyframes at different times, verify diamonds visible
3. Left-click a diamond → highlighted. Drag it → slides along ruler. Release → snapshot taken (verified with next Ctrl+Z reverting the move)
4. Right-click a diamond → context menu → Delete Keyframe → diamond gone. `Ctrl+Z` → diamond back
5. `Ctrl+click` an empty spot on a track row → new keyframe appears at that time
6. Timeline: click a layer's Parent dropdown → dropdown OPENS (currently doesn't). Select a parent → parenting works. Verify cycle detection still grays out invalid parents
7. Full sequence: add 2 layers, parent one to the other, animate the parent's position with 2 keys, `Ctrl+S` → save. Close app. Reopen, load. `Ctrl+Z` `Ctrl+Z` `Ctrl+Z` — should unwind to earlier states saved during the current session (not before the load; loading itself was a snapshot so undo returns to pre-load state)
8. Every test in `TEST_CHECKLIST.md` should still pass identically

---

## 7. What deliberately does NOT change in this commit

- Bezier easing per keyframe (Phase 2 — the graph editor stops being a showpiece THEN)
- Per-layer effect RT pool (Phase 2)
- Composition Settings modal (Phase 2)
- Text layers, sub-comps, masks (Phase 3+)
- Command-pattern undo (Gemini + Claude both said snapshot is better for us; revisit only if snapshots ever become >100MB, which requires embedded images)

---

## 8. Question(s) before I execute

None. This is straightforward integration of pieces already designed. If you spot something I got wrong, tell me. Otherwise say **"go single commit"** and I execute.

If you want, I can also split it into 3 micro-commits (undo, then diamonds, then parent) — but they share so much of the same test cycle (each needs "verify nothing broke elsewhere") that one commit is genuinely simpler.
