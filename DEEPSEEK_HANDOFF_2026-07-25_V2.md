# DeepSeek review request — Potato Motion Editor, Commit 19 design

**Context:** you reviewed our Commit 17 design (AE-style property twirl-
down) two sessions ago. Your feedback caught the effect-bitmask reorder
bug, the `TimelineRow` heap-alloc issue, and pushed back correctly on
scope creep (inline drag-floats). All incorporated; Commit 17 shipped
green.

**What happened next:** the user tested Commit 17 in the actual editor.
The twirl-down works but is **too eager** — twirling Transform open
shows all 6 property sub-rows regardless of which ones the user has
touched. Same for effect params. User feedback (paraphrased):

> "if i want only scale only that will come in the below timeline and
> then if i need position it will come under the scale property"
> "the option should come when it is used"
> "only those parameter will show down in the timeline whose is clicked
> or changed"

Also asked for inline editable values (reversing your earlier deferral —
now explicitly requested), label column reorder with a blend-mode combo,
per-property graph icon, and a new side-by-side layout mode where the
timeline strip and graph editor are visible simultaneously.

We're calling this Commit 19 (lazy twirl v2). Original roadmap
(17→18 split-layer → 19 blur → 20 fx-anim → 21 mattes) slides
one right; the immediate follow-up (Commit 18) is redirected to
lazy-reveal because half-shipping the twirl-down UX would leave the
user in a worse spot than pre-Commit-17.

---

## What we need from you

Two asks:

### 1. Retrospective on 2 real bugs from Commit 17 test

**B1 — selection sticks after clicking empty canvas.** No deselect
handler. Trivial fix: added `layerManager.SetSelectedId(-1)` when
click hits no layer. Snapshot on the deselect for Ctrl+Z. Shipped
in hotfix `fb681e4`.

**B2 — rotation still animates after stopwatch off.**
`ToggleStopwatch(t)` was clearing keyframes but leaving `staticValue`
at whatever the property was constructed with (Vec3(0) for rotation),
so if the layer's visible rotation at the moment of toggle-off was
say 45°, it snapped back to 0° on next frame. User perceived this
as "rotation still happening" because during playback the SetValue
calls from a stale Inspector drag path may have been keyframing
during a race window.

Fix shipped in `fb681e4`: `ToggleStopwatch` now snapshots
`Evaluate(t)` INTO `staticValue` BEFORE clearing keyframes. Layer
freezes at its visible pose on toggle-off.

**Please confirm** this is the right fix. Any edge case where
snapshotting the evaluated value into staticValue would cause
regression? (E.g., property with staticValue that intentionally
differed from the initial keyframe? Doesn't happen in current
code but structural review welcome.)

### 2. Design review on Commit 19 (lazy twirl v2)

Full design doc follows below. **6 explicit open questions** at
"Design questions still open" section — please weigh in on each with
your rec. Also flag anything I missed structurally, especially:

- The `revealedProperties: std::vector<std::string>` approach —
  string keys per property path. Alternative is enum-based, but
  effect params make that untenable (each layer has arbitrary
  effect IDs).
- The auto-reveal / auto-hide race in the row-list build.
- Migration semantics for old `.pmge` files that don't have
  `revealedProperties`.
- The new `bottomPaneMode::SideBySide` layout — splitter fraction
  persistence, keyboard shortcut cycling (Shift+F3 goes Bars →
  Graph → SideBySide → Bars now).
- Inline drag-float during playback — my plan is to add
  `inspectorValueDragActive` to the `anyDragActive` freeze-clock
  check. Any better alternative?

Be direct. Push back on scope creep, flag latent bugs, tell me if
the roadmap reshuffle is a mistake.

---

## Reference: what shipped since your last review

CI-green commits between your Commit 17 review and this one:
- `edef091` docs (session-2 EOD + feedback triage + header refresh)
- `7a98acc` Drop Shadow default params bump
- `9af3c11` **Drop Shadow fused shader** (fixes 'shape goes black' —
  same-texture SRV+RTV bind in 2-pass composite, collapsed to
  single-pass. Zero same-texture risk.)
- `b335263` AE anchor crosshair (cosmetic)
- `607fe7e` 2D gaussian shadow (13-tap ring, replaces 5-tap
  perpendicular line that stamped diagonal ghosts) + AE bounding
  box style (shadow-sandwich outline)
- `7984e78` Adaptive text stroke (concentric-ring dilation with
  `[unroll]` — you flagged this compiles all 42 taps regardless
  of runtime `sw`; noted as tech debt, real fix is SDF-based
  stroke, parked)
- `14f83b1` Scale link no-op resolution + 4 mid-side scale handles
- `ca1fcf0` 2x text atlas oversample (fixes zoom blur, works up
  to ~200% zoom; adaptive re-rasterize deferred)
- `c8f7940` Commit 17 design doc v2 (your fixes incorporated)
- `824eeed` **Commit 17 ships** — AE-style property twirl-down
  with all your v2 fixes
- `fb681e4` **hotfix** B1 + B2 (this section's retro)

Current commit `fb681e4`, CI green, artifact ~1.24 MB.

---

## Full design doc

# Design doc — Commit 19: Lazy property revelation (twirl-down v2)

**Task ID:** 5.15 (proposed)
**Design-doc-first per workflow.** Reviewers: user + DeepSeek before code.
**Estimated scope:** ~500 LOC across `Layer.h`, `RenderEngine.h`,
`RenderEngine.cpp`, minor `Serialization.cpp`.
**Roadmap position:** slides into the Commit 18 slot; original 17→21
plan shifts +1.

---

## Why this exists

Task 5.14 (Commit 17) shipped the twirl-down but the sub-row model is
**eager** — twirling Transform open dumps all 6 property rows in the
user's face whether they need them or not. Same for Effects (all 5 Drop
Shadow params always visible when the effect is expanded).

User feedback from session-3 (see `TEST_FEEDBACK_2026-07-25_TWIRL.md`)
converged on one theme: **only properties I've actually touched should
appear in the timeline.** This matches AE's real behavior — properties
are lazily revealed as they're keyframed or modified.

Verbatim user requests (paraphrased for the design):
- "if i want only scale only that will come in the below timeline and
  then if i need position it will come under the scale property"
- "the option should come when it is used"
- "only those parameter will show down in the timeline whose is clicked
  or changed"
- "clicking the value users can type specific value"
- Timeline label column reorder: `[eye][arrow][blend][name][fx]`
- Graph editor per-property affordance + side-by-side layout mode

Grouping all of this into one commit because the changes are inter-
dependent — you can't add per-property graph icons if the sub-rows
themselves are reorganised, you can't do inline editing without a
per-sub-row hit-region model, and lazy reveal changes the row-layout
pass upstream.

---

## Non-goals

- **fill/stroke/color as AnimatedProperty** — separate task (Commit
  20-ish). Structural. Deserves its own design.
- **Effect param animation** — same reason (Commit 20 per the locked
  roadmap).
- **`U` / `UU` AE-style shortcuts** — the "show only animated" and
  "show only modified" hotkeys. Nice-to-have; add once the lazy-reveal
  model is stable and users start asking for the shortcut.
- **Motion path overlay** — was in Commit 18 slot; slides to Commit 20.
- **Split Layer tool** — was in Commit 18 slot; slides to Commit 20.

---

## Locked design decisions

The user gave clear direction on these already. Not up for further
review:

- **Lazy reveal model** — properties appear as they're touched, disappear
  via right-click → Hide (or stopwatch off + never touched again)
- **Inline editable values** — clicking the value readout turns it into
  a drag-float; DeepSeek's earlier "defer to follow-up" concern is
  now moot because the user explicitly asked for it
- **Label column reorder:** `[eye 18px][twirl 12px][blend combo
  60px][name flex][fx chip 22px]`. **Parent combo REMOVED from strip**
  — user's list didn't include it, and it takes column real-estate
  that blend mode needs. Parent stays available in the Inspector.
- **Per-property graph icon** — appears in the sub-row's right margin
  only when that property has been touched (stopwatch on or has
  keyframes). Click = open graph editor scoped to that property.
- **Graph layout modes:**
  - `Full` — current behavior; graph replaces the timeline strip
  - `SideBySide` — timeline strip on left half, graph on right half
    with a splitter. New mode added to `bottomPaneMode` enum.

---

## Design questions still open (for user + DeepSeek review)

1. **What counts as "touched"?**
   - Option A: stopwatch on OR keyframes exist OR value ever changed
     from construction default
   - Option B: stopwatch on OR keyframes exist (stricter — a one-time
     edit doesn't reveal)
   - Option C: user explicitly right-clicks the property in Inspector →
     "Show in Timeline" (fully explicit — no automatic reveal)

   **My recommendation: Option A.** Matches AE's `UU` semantics.
   Option B leaves users confused why their edit doesn't show up.
   Option C is the safest but requires learning a hidden gesture.

2. **How do users HIDE a revealed row?**
   - Right-click the sub-row → "Hide Property" menu item?
   - Turn stopwatch off AND clear keyframes → auto-hide next frame?
   - Never — once revealed, always shown until layer is collapsed?

   **My recommendation: right-click menu with "Hide Property" AND
   auto-hide when the property returns to its construction default
   AND has no keyframes AND stopwatch is off.** Best of both.

3. **Migration path for existing projects?**
   - Old `.pmge` files loaded fresh should show which properties?
   - Option A: All properties currently animated (has keyframes)
   - Option B: All properties currently touched (differs from default
     OR animated)
   - Option C: Nothing revealed by default; user opens what they want

   **My recommendation: Option B on first load, then track from there.**

4. **How does Blend Mode combo behave in the label column?**
   - Full dropdown (like the current Parent combo)?
   - Icon-only that opens the dropdown on click?
   - Text label showing current mode with a chevron?

   **My recommendation: 60-px wide text combo showing current mode
   name, opens the standard BlendMode enum dropdown.** Matches the
   Parent combo pattern from Task 5.12b — user knows how to use it.

5. **Where does the per-property graph icon go?**
   - Right edge of the sub-row's label column (before the diamond
     track starts)
   - Right edge of the WHOLE strip (after the diamond track ends)
   - Not in the strip — only in the Inspector next to each property

   **My recommendation: right edge of the label column, ~12 px wide,
   dimmed when property is untouched, bright + clickable when
   touched.** Matches user's "just like fx is toggle" model.

6. **Graph SideBySide default width split?**
   - 50/50 timeline strip / graph
   - 60/40 favoring strip
   - Persist last-used split via SettingsHandler like the label|track
     splitter

   **My recommendation: default 60/40, persist to imgui.ini.** Users
   spend more time in the strip than the graph editor.

---

## Data model changes

### `Layer.h`

Add a set of "revealed property IDs" per layer. NOT a bitmask this time —
we already learned from DeepSeek that vector indices are unstable when
properties get added over time (Anchor/Size were added in Task 5.14
already). Use a stable string-based key so future properties (fill,
stroke, effect params, whatever) can join without breaking anything.

```cpp
// Task 5.15: lazy property revelation. Which properties are currently
// "revealed" in the timeline sub-rows. Keyed by property path so we can
// address anything: "transform.position", "transform.rotation",
// "transform.scale", "transform.opacity", "transform.anchor",
// "transform.size", "fill.color" (future), "effect:<effectId>:distance"
// (Commit 20).
//
// Reveal semantics:
//   - Toggling stopwatch on adds to the set
//   - Adding a keyframe adds to the set
//   - Editing a value in the Inspector adds to the set
//   - Right-click Sub-row → "Hide Property" removes from the set
//   - Auto-hide (per-frame check): property is at construction default
//     AND has no keyframes AND stopwatch is off AND not in the set
//     because of an explicit user reveal → drop
//
// Persistence: serialised to .pmge so reopened projects show the same
// property list. Loaded projects from BEFORE this task get an implicit
// reveal for anything animated or non-default (see migration below).
mutable std::vector<std::string> revealedProperties;

bool IsPropertyRevealed(const char* path) const;
void RevealProperty(const char* path) const;    // idempotent, no-op if already revealed
void HideProperty(const char* path) const;       // idempotent
```

`std::string` in the vector is a heap-allocation concern per Section 7,
but this is UI-state — not touched inside the frame loop's per-pixel or
per-primitive code. Reveal/hide happens at most a few times per user
interaction. Fine.

### `RenderEngine.h`

Extend `bottomPaneMode`:

```cpp
enum class BottomPaneMode : int {
    Bars       = 0,   // current default
    Graph      = 1,   // graph replaces strip
    SideBySide = 2,   // Task 5.15: strip left, graph right
};
```

Add the side-by-side splitter fraction:

```cpp
float bottomPaneGraphSplit = 0.60f;  // strip width fraction in SideBySide mode
```

Persist via existing SettingsHandler from Task 5.12.

### `AnimatedProperty.h`

No structural change. Add helpers:

```cpp
// True if the current staticValue differs from construction default
// AND there are no keyframes. Used by the auto-hide check.
bool IsAtConstructionDefault() const;
```

Needs a `constructionDefault` field stored at initialization. Or we
compare against a well-known-default per T (identity for Vec3
position/rotation, ones for Vec3 scale, etc.). Trickier for arbitrary
T. **Simplest:** store construction-time default at property
construction; expose `IsAtConstructionDefault()`. +8 bytes per property.
Negligible.

---

## Row layout changes

`DrawTimelineStrip`'s row-list pass extended:

```cpp
// For each layer L (top row = last vector index):
//   emit LayerHeader row
//   if Expand_Transform bit set OR any transform property is revealed:
//     for each transform property in { position, rotation, scale,
//                                       opacity, anchor, size }:
//       if L.IsPropertyRevealed("transform.<name>"):
//         emit TransformProp row
//   if Expand_Effects bit set:
//     for each effect e:
//       emit EffectHeader row
//       if L.IsEffectExpanded(e.id):
//         for each param p in e:
//           if L.IsPropertyRevealed("effect:<e.id>:<paramName>"):
//             emit EffectParam row
```

Row rendering (per kind) is 90% preserved from Task 5.14. Key changes:

- **TransformProp row** gets:
  - Inline editable value (drag-float, right-aligned in label column)
  - Per-property graph-icon at the far-right of label column (12 px,
    dimmed if `!isRevealed`)
  - Right-click menu → "Hide Property", "Reset to Default"
- **LayerHeader row** gets the column reorder:
  `[eye 18][twirl 12][blend 60][name flex][fx 22]`
  - Parent combo REMOVED
  - Blend combo (60 px) reads/writes `layer.blend` directly (already
    an `AnimatedProperty<int>` — wait, is it? Check current code.
    Likely just a `BlendMode` enum field, not animated. Fine — this
    combo just sets the static field for now)

### Inline value editing spec

Sub-row layout for editable value:
```
[stopwatch 12px][gap 4px][property name  ][value drag-float 70px][gap 6px][graph icon 12px]
                                        ^                       ^
                                     labelX0                labelX1
                                     (rest goes to diamond track)
```

Drag-float widget is an `ImGui::DragFloat` (scalar) or `DragFloat2` /
`DragFloat3` depending on the property type. Small speed factor
(~0.5) so scrubbing is precise. `ImGui::IsItemDeactivatedAfterEdit`
fires the undo snapshot at the end of the drag (not per-frame).

**Editing during playback:** freeze the clock like we do for gizmo/
diamond drag. Update `anyDragActive` check in BeginFrame to include
`inspectorValueDragActive` (new flag set/cleared inside the sub-row
drag-float lifecycle).

### Right-click sub-row menu

Standard ImGui popup:
- **Hide Property** — removes from `revealedProperties`. If the
  property has keyframes, they stay (just hidden from the strip).
- **Reset to Default** — sets `staticValue` back to construction
  default, clears keyframes, turns off stopwatch. Then re-checks
  auto-hide.
- **Open in Graph** — opens the graph editor scoped to this property
  (switches `bottomPaneMode` to Graph or SideBySide depending on
  current setting; sets a new `graphFocusedProperty` field for the
  editor to render).

---

## Auto-reveal hooks

Every place that touches a property must reveal it. Points to touch:

1. **Stopwatch toggle** in the timeline sub-row → reveal on ON, keep
   revealed on OFF (visibility persists even without keys, until
   explicit Hide)
2. **`SetValue` from the Inspector's DragFloat3 for position / rotation
   / scale / opacity / anchor / size** → reveal (already routes through
   SetValue; add a `layer.RevealProperty("transform.<name>")` call
   after the SetValue if the value actually changed)
3. **Inline drag-float in the sub-row itself** → already revealed (the
   sub-row wouldn't be visible otherwise, but the drag might create
   the first keyframe → snapshot for undo as usual)
4. **Import from JSON** (Serialization) → migration path (see below)

Auto-HIDE happens per-frame in the row layout pass:

```cpp
for (each revealed transform property):
    auto& prop = get_property_by_path(layer, path);
    if (!prop.HasStopwatch() && prop.keyframes.empty() &&
        prop.IsAtConstructionDefault()) {
        layer.HideProperty(path);
    }
```

O(6) per layer per frame — negligible.

---

## Serialization

`.pmge` gets a new optional field per layer:

```json
{
    "layer": {
        ...
        "revealedProperties": ["transform.position", "transform.scale"]
    }
}
```

**Migration on load (old files without the field):**
- Compute the set at load time using the auto-reveal criteria
- Any transform property that HAS keyframes → reveal
- Any transform property whose current staticValue differs from
  construction default → reveal
- Anything else stays hidden

This means old projects reopen with a familiar timeline view (only
properties they'd actually animated / edited).

Comparing this to Task 5.14's default: Task 5.14 revealed ALL
properties on twirl-open regardless of touch state. Task 5.15
reveals only touched ones. **Both models coexist during migration:**
we don't drop the twirl bits (`timelineExpandMask`), we just reinterpret
them. Twirl OPEN means "show revealed props"; twirl CLOSED means "show
none, just merged diamonds on the header row." Backward-compat clean.

---

## Graph editor rework

### Layout modes

`bottomPaneMode` gains a third value: `SideBySide`. UI:

- **Bars mode:** timeline strip full-width (unchanged)
- **Graph mode:** graph editor full-width (unchanged from Task 5.12)
- **SideBySide mode:** vertical splitter divides bottom dock; timeline
  strip on left, graph on right. Splitter fraction persists via
  SettingsHandler (`bottomPaneGraphSplit`, default 0.60).

Toolbar toggle already has `[Bars][Graph]` from Task 5.12. Add a third
`[Side]` button. Shift+F3 cycles through the three modes instead of
just toggling Bars↔Graph.

### Per-property graph icon

At the right end of each revealed TransformProp sub-row's label column:

- 12-px square with a chart-icon glyph (small line-graph shape drawn
  via ImDrawList — 3 line segments forming an arc, like the AE graph
  icon).
- Dimmed color when property is untouched (`!isRevealed` — but
  actually since we only draw the row when revealed, this state is
  always "touched" in practice; keep the visual dim/bright toggle for
  visual weight indicating "keyframes present" vs "just modified")
- Click → sets `graphFocusedProperty = LayerIdPropertyPath{layerId,
  path}` and switches `bottomPaneMode` to `Graph` if it was `Bars`, or
  leaves it in `SideBySide` if that's the current mode.
- The graph editor reads `graphFocusedProperty` and renders that
  property's curve. If null, falls back to current behaviour
  (currently-selected layer's Position, or the property dropdown).

### Graph editor's own toolbar

Add a small breadcrumb: `<Layer name> > <Property name>` so user knows
what they're editing. Click the layer part → jumps focus to that layer
in the timeline.

---

## Implementation plan (order)

1. **Add data model bits** (`Layer.revealedProperties`,
   `Layer.IsPropertyRevealed / RevealProperty / HideProperty`,
   `AnimatedProperty.IsAtConstructionDefault` + construction default
   snapshot)
2. **Serialization** — read/write `revealedProperties`, migration for
   old files
3. **Auto-reveal hooks** — Inspector DragFloat3 sites, stopwatch
   toggle, etc.
4. **Auto-hide check** — per-layer, per-frame in the row-list build
5. **Row-list build changes** — filter TransformProp rows by
   `IsPropertyRevealed`
6. **Label column reorder** — swap eye/twirl order, add BlendMode
   combo, remove Parent combo
7. **Inline drag-float value editing** — replace read-only text on
   TransformProp rows; wire freeze-clock + undo
8. **Right-click sub-row menu** — Hide / Reset / Open in Graph
9. **Per-property graph icon** — draw + click handler
10. **BottomPaneMode SideBySide** — add enum value, implement layout,
    add toolbar `[Side]` button, persist split
11. **Graph focused-property** — new field, graph editor reads it,
    breadcrumb bar
12. **Manual test matrix** (see below)

---

## Estimated LOC

| File | Add | Del | Net |
|---|---|---|---|
| `src/Layer.h` | 30 | 5 | +25 |
| `src/AnimatedProperty.h` | 15 | 0 | +15 |
| `src/RenderEngine.h` | 20 | 3 | +17 |
| `src/RenderEngine.cpp` | 400 | 60 | +340 |
| `src/Serialization.cpp` | 30 | 0 | +30 |
| **Total** | ~495 | ~68 | **+427** |

Same magnitude as Commit 17. Fits within our "single commit big-feature"
size budget.

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Auto-hide races with auto-reveal (property hides one frame, reveals next) | Auto-hide only runs if the property is NOT in the explicit-reveal set from a user gesture. Track a `revealReason` (Enum: Explicit / Animated / Modified) — Explicit is sticky, others follow the value state. |
| Inline drag-float during playback spams keyframes | Freeze anim clock while any sub-row drag-float is active (same pattern as gizmo/diamond drag; add flag to `anyDragActive` in BeginFrame). |
| Migration on old files reveals nothing | Migration checks BOTH keyframes AND "differs from default" so any user-modified project shows familiar layout. Fallback: if `revealedProperties` is empty after migration AND `timelineExpandMask & Expand_Transform` was set (user had it expanded in Task 5.14), reveal all 6 properties (mirrors old behaviour). |
| BlendMode combo in label column takes too much space on narrow panels | If `labelW < 200` px, hide the combo. Blend still editable in Inspector. Same conditional pattern as the Parent combo removal in Task 5.12b. |
| String comparisons in `IsPropertyRevealed` on a hot path | Bounded: 6 transform props + ~5 params × 3 effects = ~20 entries per layer max. `std::string` compare on that tiny list is fine per frame. If profiling shows it hot, switch to `uint32_t` hash of the path. |

---

## Test matrix

Post-CI green, run through:

1. **Fresh scene:** add a rect. Twirl Transform open. **Expect: NO sub-rows** (all properties at default).
2. **Set Position keyframe:** stopwatch on for Position, add key. **Expect: Position sub-row appears immediately.**
3. **Set Scale value in Inspector (no keyframe):** drag scale to 1.5. **Expect: Scale sub-row appears (auto-reveal on edit).**
4. **Reset scale to 1.0 without stopwatch:** **Expect: Scale sub-row auto-hides.**
5. **Right-click Position sub-row → Hide Property.** **Expect: sub-row disappears even though keyframes remain.**
6. **Inline edit a value:** click the value text on Position sub-row → drag. Value changes, keyframe at current time updates. **Playback continues to work.**
7. **Playback + inline edit:** start playback, drag inline value. **Playback should freeze during drag; no keyframe spam.**
8. **BlendMode combo:** change blend to Additive from the label column. Layer renders additively. Persists on save/reload.
9. **Parent combo missing from strip:** confirmed. Parent editable in Inspector only.
10. **Per-property graph icon:** click on Position's graph icon → graph editor opens focused on Position. Breadcrumb shows `Rect1 > Position`.
11. **SideBySide mode:** toolbar `[Side]` button → timeline shrinks to 60% width, graph appears on right 40%. Drag splitter → both resize.
12. **Save/reload:** everything above persists (revealed set, blend mode, side-by-side split).
13. **Old .pmge file:** load a project made with Task 5.14. Sub-rows appear for properties that had keyframes or non-default values.

---

## Sign-off requirements

Before code:
- [ ] User approves 6 open design questions
- [ ] DeepSeek review of full doc (send for second-opinion pass; catch
      edge cases I missed like the reveal-hide race and Explicit
      reveal-reason)
- [ ] Any final tweaks folded in

Then one commit implements everything above.

---

## What's NOT in this commit (explicitly)

- fill / stroke color as AnimatedProperty
- effect param animation
- Split Layer tool
- Motion path overlay
- `U` / `UU` AE shortcuts
- Adjustment Layer type
- Track Mattes

Original roadmap now:
- 17 (shipped): AE twirl-down v1
- 18 (skipped, folded here): Split Layer + motion path
- **19 (THIS): Lazy twirl-down v2 + graph rework**
- 20: Split Layer + motion path overlay (from old 18)
- 21: Separable 2-pass Gaussian blur (from old 19)
- 22: Effect param animation (from old 20)
- 23: Track Mattes + Adjustment Layers (from old 21)
