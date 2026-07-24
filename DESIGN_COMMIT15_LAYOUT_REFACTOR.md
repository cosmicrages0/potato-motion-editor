# Design Doc — Commit 15 (Task 5.12): Unified Bottom Dock Layout Refactor

**Base commit:** `2f2b7da` (Task 5.11-fix-3: text stroke)
**LOC delta estimate:** +400 / -180
**User-visible change:** Timeline and Graph Editor merged into a single
full-width bottom panel with an internal left/right split. Left pane is
a compact layer outline; right pane toggles between Layer Bars (time
canvas) and Graph Curves via a header button. Right-side Graph Editor
dock is removed.

**No new features.** No Shy, no Track Matte, no Adjustment Layer, no
groups, no inline property editing. Pure UI restructure — every
existing feature keeps working exactly as it does today, just in a
different arrangement.

---

## 1. Why this commit

You've been staring at the Pikimov screenshot for a while. The current
layout has three real problems:

1. **Bottom is split awkwardly** — Timeline gets the left ~half, Graph
   Editor gets the right ~half. Neither is wide enough for real work
   (Timeline strip runs out of horizontal room on comps > 5 seconds,
   Graph Editor is cramped for tangent handles).
2. **Graph Editor is a separate window** — you have to click into it
   to see curves. AE / Pikimov integrate the two so a single mode
   toggle switches the same panel between bars and curves.
3. **Layer outline is buried inside the strip** — the left "name column"
   is a fixed 120 px, no room for the parent picker, blend mode preview,
   or trim/stretch numeric readouts you'll want later.

Fixing the layout now unblocks every feature we've deferred: Shy toggle,
Track Matte pickwhip, Adjustment Layer icon, Motion Blur switch — all
of those want dedicated columns in the outline.

---

## 2. Target layout

```
+--------------------------------------------------------------------------+
| Menu bar                                                                 |
+------------+-----------------------------------+-------------------------+
| Project    | Composition Viewport              | Inspector & Effects     |
| Assets     |                                   | (wider than today —     |
| (25%)      |                                   | ~28%)                   |
| Effects    |                                   |                         |
| Palette    |                                   |                         |
| (tab)      |                                   |                         |
+------------+-----------------------------------+-------------------------+
| Timeline (unified, full width)                                           |
|  ┌── left outline (30%) ──┬── canvas (70%) ─────────────────────────┐   |
|  │ # | Label | Name | Prnt │ [Bars | Graph] toolbar                 │   |
|  │────────────────────────│  <ruler>                                │   |
|  │ 3   ● Text 3   —       │  [bar]--------[keys]------              │   |
|  │ 2   ● Ellipse  —       │  [bar]-----[keys]---                    │   |
|  │ 1   ● Rectangle none   │  [bar]------[keys]------                │   |
|  └────────────────────────┴─────────────────────────────────────────┘   |
+--------------------------------------------------------------------------+
```

- Bottom panel is a **single ImGui window** (`"Timeline"`) that spans
  full viewport width.
- Inside it: `ImGui::BeginTable(..., 2 cols, ImGuiTableFlags_Resizable)`
  or `ImGui::Splitter`-style hand-rolled vertical splitter. **Going with
  the hand-rolled splitter** because ImGui's `BeginTable` has clumsy
  interaction with `SetCursorScreenPos` (which every diamond / bar /
  drag handler uses). A single vertical splitter is 40 LOC and keeps
  the existing pixel-math code paths intact.
- **Left pane** (30% default): compact outline. Columns rendered manually
  with fixed pixel widths — no ImGui table. Header row has column labels.
- **Right pane** (70% default): the existing time-canvas OR the existing
  graph editor, based on `bottomPaneMode` enum. Both already work; we
  just gate which one draws.

---

## 3. Left pane — the outline

Columns (left to right, all rendered on ONE row per layer):

| Col       | Width | Content                                              |
|-----------|-------|------------------------------------------------------|
| `#`       | 24 px | Row number (1-based, reversed: top = highest number).|
| Vis       | 20 px | Eye icon; toggle `layer.isVisible`.                  |
| Color     | 12 px | Filled square of `layer.fillColor`.                  |
| Name      | flex  | Layer name (double-click to rename).                 |
| Type      | 50 px | "Rect" / "Ellip" / "Text" / "Null" / "Cam" (small).  |
| Parent    | 80 px | Existing parent-pickwhip combo, condensed.           |

Row height = 22 px (2 px taller than today's 18 to fit the columns).
Row background = existing selection tint + drag-in-flight tint.

**Interaction:**
- Row is one big drag-handle for reorder (same as today, just wider).
- Double-click name → rename (`InputText`). Enter or click-away commits.
- Eye toggle: existing pattern from `Layer.isVisible` — click flips.
- Color square: click to open Fill color picker inline.

**What we DON'T do in this commit:**
- Shy / Solo / Lock icons (deferred — they need feature commits first)
- Twirly to expand into per-property rows (that's a data-model change)
- Blend mode preview icon (`N` / `A` / etc.) — later polish

---

## 4. Right pane — mode-switched canvas

Header toolbar (persistent across modes):
```
[Bars] [Graph]  |  ⏮ ▶ ⏭ ⏹  |  duration input  |  fps readout
```

- `bottomPaneMode` = enum `{Bars = 0, Graph = 1}`, defaults Bars.
- **Bars mode** — the existing time canvas (ruler + trim bars + keyframe
  diamonds + playhead). All existing code paths from `DrawTimelineStrip`
  minus the label column (which now lives in the left pane).
- **Graph mode** — the existing `DrawGraphEditor` content. When it draws
  here it looks IDENTICAL to today's separate Graph Editor window —
  the module doesn't care what container it's in.

Switching modes just gates which body runs. Same layer selection, same
playhead, same undo history.

Shortcut: `Shift+F3` toggles the mode (AE-standard).

The old `"Graph Editor"` dock window is REMOVED. All the graph editor
code stays — it just gets called from inside the Timeline panel now.

---

## 5. Splitter between the two panes

Hand-rolled. `float bottomPaneSplitFrac = 0.30f` in RenderEngine.
Splitter is a 4 px invisible drag handle. When dragged, `bottomPaneSplitFrac`
updates. Clamped `[0.15, 0.60]`. NOT persisted to `.pmge` (editor state
like preview scale). Loads default 0.30 always.

---

## 6. Files changing

```
src/RenderEngine.h     +8      bottomPaneMode enum + splitFrac +
                                  rename-buffer state
src/RenderEngine.cpp  +250/-160 dock builder change (remove Graph
                                  Editor dock); DrawTimelinePanel
                                  rewrite: splitter + left outline
                                  columns + mode toggle; strip label
                                  column removal from DrawTimelineStrip
                                  (its label rendering moves into the
                                  outline); Shift+F3 shortcut handler
DESIGN_COMMIT15_LAYOUT_REFACTOR.md  NEW  this file
```

Net ~+100 LOC. Binary size negligible.

---

## 7. What survives untouched

Everything animation, rendering, export, serialization, undo, save/load
is untouched. Layer data model unchanged. Every existing `.pmge` file
loads and renders identically. The only visible change is layout.

Explicitly preserved:
- Trim bar drag (left/right handles + middle-slip)
- Keyframe diamond drag + right-click context menu
- Playhead scrub
- Reorder-by-drag on the layer name (now on the whole left row)
- Ctrl+D duplicate, Delete key semantics
- Graph editor value/speed modes, tangent handle drag, right-click menu

---

## 8. Test plan

1. Old .pmge loads → all layers, keyframes, tangents, effects intact
   → visual layout different but every feature works
2. Bottom panel spans full width; drag the splitter bar left/right;
   clamps at 15% and 60%
3. Left outline: # column shows row index (top row = highest number).
   Click a layer name to select. Double-click to rename. Drag any part
   of the row to reorder.
4. Right pane defaults to Bars mode. Click Graph button in header →
   graph editor draws in the same panel; Bars button switches back.
   Shift+F3 also toggles.
5. All existing Bars-mode interactions work (trim bars, diamonds,
   playhead scrub).
6. Graph-mode: mode toggle + property picker + tangent handle drag +
   right-click context menu all work as before.
7. The old right-side Graph Editor dock does NOT appear on first launch
   or after "View → Reset Layout."
8. Save → close → reopen — layer order, keyframes, everything survive.
   Splitter position doesn't persist (matches preview scale convention).
9. Ctrl+Z after any layer / keyframe mutation reverts cleanly (snapshot
   pipeline unchanged).
10. Export MP4 → renders correctly (this touches ZERO of the export
    path; safety-check only).

---

## 9. Explicitly deferred (do not scope-creep this commit)

Every one of these needs its own commit:

- **Pill-shaped layer bars** in the right pane canvas — cosmetic tint
  change; safer once the outline is settled
- **Inline property editing on the outline** (x/y under layer name) —
  needs per-property row expansion (twirly), which is a data-model touch
- **Layer groups / parent tree** — needs `Layer.groupChildren` field
- **Shy / Solo / Lock icons in the outline** — need feature bits
- **Blend mode preview icon (N / A / etc.)** in the outline — polish
- **Column show/hide toggle bar** at the bottom of the outline (Pikimov
  has one for A/V, Switches, Modes clusters) — needs the column data
  first

---

## 10. Go

No open questions. Executing single commit.
