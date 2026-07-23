# Design Doc — Commit 11 (Task 5.6-fix-2): Undo preserves layer selection

**Base commit:** `872d5cf` (Task 6.1-fix: export animations)
**LOC delta estimate:** +18 / -2
**User-visible change:** Ctrl+Z on the currently-selected layer no
longer jumps you back to the first (bottom) layer. Selection is
preserved across undo/redo, save, and load.

---

## 1. The bug

User reported: `ellipse over rectangle → click ellipse → tweak something
in Inspector → Ctrl+Z → selection jumps back to Rectangle`.

Root cause is a two-part omission in Serialization:

1. **`SelectedLayerId` is never written to JSON.** `AppStateToJson` /
   `WriteLayer` skip it entirely. So every snapshot the UndoStack takes
   loses which layer was active.

2. **On load, `LayerManager::Clear()` resets `selectedLayerId = -1`.**
   Then `JsonToAppState` finishes with this compensator:
   ```cpp
   if (state.layerManager->GetSelectedId() < 0 && !Layers().empty()) {
       state.layerManager->SetSelectedId(Layers().front().id);
   }
   ```
   Which forces selection to the bottom-most layer (Rectangle). Every
   undo restore triggers this path → selection resets.

Task 5.6 made `MarkForSnapshot` synchronous (fixed the atomic-op undo
bug). This selection bug survived because it lives in the JSON
save/load path, not the snapshot timing path.

---

## 2. Fix

### 2a. Serialize the field

`AppStateToJson` gains one line:

```cpp
outRoot["selectedLayerId"] = state.layerManager->GetSelectedId();
```

Written at the root, not inside `composition` — selection is editor
state, not a composition property. AE / Blender / Figma all handle it
similarly (editor state at root, comp/scene properties nested).

### 2b. Restore the field before the fallback

`JsonToAppState` reads the saved value AFTER layers are populated,
BEFORE the "no selection → snap to front" compensator:

```cpp
if (root.contains("selectedLayerId")) {
    const int savedSel = root["selectedLayerId"].get<int>();
    // Only apply if the layer still exists (defensive: a corrupt file
    // or a mid-migration schema change might reference a deleted layer).
    if (savedSel >= 0 && state.layerManager->GetLayerById(savedSel) != nullptr) {
        state.layerManager->SetSelectedId(savedSel);
    }
}
// Existing "snap to front" fallback runs only if the above didn't set
// anything, i.e. the file didn't have selectedLayerId or the referenced
// layer was deleted since save.
if (state.layerManager->GetSelectedId() < 0 && !state.layerManager->Layers().empty()) {
    state.layerManager->SetSelectedId(state.layerManager->Layers().front().id);
}
```

### 2c. Backward compat

Pre-fix `.pmge` files have no `selectedLayerId` field → the front-layer
fallback still fires. Old files load identically. Zero-cost migration.

---

## 3. Undo/redo behavior after the fix

Every `MarkForSnapshot()` call now captures the ACTIVE selection along
with the rest of AppState. `Undo` restores it. `Redo` restores it.

Corner cases:

- **Undoing a layer creation** — the new layer is gone from the vector
  after Undo. If it was selected at the snapshot moment, restored
  selection references an ID that no longer exists → the guard in
  step 2b's `GetLayerById()` check skips the restore → fallback picks
  the front layer. Correct behavior: undoing "create + select" should
  drop selection too.

- **Undoing a layer deletion** — the deleted layer is restored to the
  vector. Its ID was captured in the snapshot's selection. Restore
  works cleanly, user's selection returns to what they had.

- **Ctrl+Z after clicking a different layer** — clicking a layer
  doesn't call MarkForSnapshot in the current code (verified via
  `grep MarkForSnapshot src/RenderEngine.cpp | grep -i click`), so
  pure selection changes are NOT undoable. That's intentional —
  matches AE (undo is for scene mutations, not view state). This
  commit doesn't change that. It ONLY fixes the "selection resets to
  layer 0" side-effect of undoing an actual mutation.

---

## 4. Files changing

```
src/Serialization.cpp  MODIFIED  +12 / -1   Save + restore selectedLayerId
                                              with GetLayerById existence
                                              guard
DESIGN_COMMIT11_UNDO_SELECTION.md  NEW  this file
```

That's it. Net ~+12 LOC. No header changes. No binary size impact.

---

## 5. Test plan

1. Load any comp with 2+ layers. Select the top (Ellipse). Tweak its
   fillColor. Ctrl+Z → **selection stays on Ellipse**.
2. Delete the top layer. Ctrl+Z → Ellipse returns AND is selected
   again.
3. Add a new layer → it's auto-selected → Ctrl+Z → creation reverts,
   selection falls back to whatever was selected before (or the front
   layer if history's exhausted).
4. Save file → close → reopen → the layer you had selected is still
   selected.
5. Old .pmge files (pre-fix) still load: no crash, front-layer
   fallback fires, no selection-lost prompt.

---

## 6. Go

No open questions. Executing single commit.
