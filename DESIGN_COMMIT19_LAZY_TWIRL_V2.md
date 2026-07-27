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

## Design questions — LOCKED (post-DeepSeek review)

All 6 open questions from v1 of this doc are now answered. DeepSeek
endorsed all 6 of my recommendations verbatim. Locked:

| # | Question | Answer |
|---|---|---|
| 1 | What counts as "touched"? | **Option A** — stopwatch ON OR keyframes exist OR value differs from construction default. Matches AE's `UU` behaviour. |
| 2 | How to hide a revealed row? | **Right-click "Hide Property" (Explicit, sticky) + auto-hide only for Modified/Animated reveals when the value returns to default AND no keyframes AND stopwatch off.** |
| 3 | Migration for old .pmge? | **Option B** — reveal anything with keyframes OR non-default staticValue. Old `.pmge` with `Expand_Transform` set in the v1 model → reveal ALL 6 transform props with `RevealFlags::Explicit` (see fallback below). |
| 4 | BlendMode combo style? | **60-px text combo** in the label column, dropdown of BlendMode enum values. Same widget pattern as the removed Parent combo. |
| 5 | Per-property graph icon location? | **Right edge of the label column**, before the diamond track. 12 px wide chart-line glyph via ImDrawList. |
| 6 | SideBySide default split? | **60/40 strip/graph**, persist last-used via SettingsHandler (`bottomPaneGraphSplit`). |

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
// (Commit 22).
//
// Reveal reason is a BITMASK (per DeepSeek review). A property can be
// revealed for multiple reasons simultaneously. Auto-hide only fires
// when the ONLY reason(s) present are Modified or Animated AND the
// value/keyframe state contradicts them. Explicit reveal is STICKY —
// auto-hide is skipped entirely if that bit is set.
enum class RevealFlags : uint8_t {
    None        = 0,
    Explicit    = 1u << 0,  // user right-clicked "Show" or old-file migration
    Modified    = 1u << 1,  // value edited in Inspector (staticValue != default)
    Animated    = 1u << 2,  // has keyframes OR stopwatch on
};
inline uint8_t operator|(RevealFlags a, RevealFlags b) {
    return (uint8_t)a | (uint8_t)b;
}
inline bool operator&(uint8_t a, RevealFlags b) {
    return (a & (uint8_t)b) != 0;
}

struct RevealedProperty {
    std::string path;    // owned string; small and rare updates, fine to allocate
    uint8_t     flags;   // OR'd RevealFlags bits
};

// Reveal semantics:
//   - Toggling stopwatch ON adds Animated bit
//   - Adding a keyframe adds Animated bit
//   - Editing a value in the Inspector adds Modified bit
//   - Right-click Sub-row → "Hide Property" clears ALL bits and drops entry
//   - Right-click Sub-row → "Show in Timeline" adds Explicit bit
//   - Auto-hide per-frame check: if Explicit bit clear AND
//     (Animated bit set but keyframes.empty() && !stopwatchEnabled) AND
//     (Modified bit set but IsAtConstructionDefault()) → drop
//
// Persistence: serialised to .pmge. Old files without the field get a
// migration pass at load time (see migration semantics below).
mutable std::vector<RevealedProperty> revealedProperties;

// Path lookup uses std::string_view to avoid temporary allocations
// when passing string literals. Bounded O(n) where n <= 20 in practice.
bool     IsPropertyRevealed(std::string_view path) const;
uint8_t  GetRevealFlags(std::string_view path) const;
void     RevealProperty(std::string_view path, RevealFlags reason) const;
void     ClearRevealFlag(std::string_view path, RevealFlags reason) const;
void     HideProperty(std::string_view path) const;    // drops entry entirely
```

`std::string` in the vector is a heap-alloc concern per Section 7, but
this is UI state — not touched inside the frame loop's per-pixel or
per-primitive code. Reveal/hide happens at most a few times per user
interaction (DeepSeek confirmed this trade-off is safe). Path *lookups*
during the row-list build use `std::string_view` so passing a literal
like `"transform.position"` doesn't allocate.

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

Add a construction-time default snapshot and a comparison helper:

```cpp
template<typename T>
struct AnimatedProperty {
    T staticValue;
    T constructionDefault;   // Task 5.15: seeded from the constructor's
                             // initial value; NEVER mutated after that.
                             // Used by IsAtConstructionDefault() for the
                             // auto-hide check.
    // ... existing fields ...

    // Constructor stashes the initial value into both slots.
    explicit AnimatedProperty(const T& initial = T{})
        : staticValue(initial), constructionDefault(initial) {}

    bool IsAtConstructionDefault() const;
};
```

**Critical caveat (DeepSeek review):** `Transform::scale` is
constructed as `AnimatedProperty<Vec3>{Vec3(1.0f, 1.0f, 1.0f)}` —
i.e. its construction default is (1,1,1), NOT the generic Vec3 default
`(0,0,0)`. Same for anchor `(0.5, 0.5)`, sizePixels
`(200, 120)`, opacity `1.0`, etc. The whole point of storing
`constructionDefault` at CONSTRUCTION time is that each property snapshots
the value the caller passed in — so `IsAtConstructionDefault()` returns
correct answers WITHOUT hardcoding per-property defaults elsewhere.

Implementation of `IsAtConstructionDefault` is a per-T `==` check.
For Vec2/Vec3 the equality is exact; for float it's `std::fabs(diff)
< 1e-6f` slop. Fine grained enough that "user dragged to 0.9999
back to 1.0" reads as "at default" (auto-hide fires).

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

**Migration on load (old files without `revealedProperties` field):**
- Every layer runs a `MigrateRevealedProperties()` pass:
  1. For each transform property (position/rotation/scale/anchor/size/
     opacity), if `keyframes.empty() == false` OR `!IsAtConstructionDefault()`:
     add to `revealedProperties` with `RevealFlags::Modified | Animated`
     (whichever apply — both if both).
  2. **DeepSeek's fallback (critical):** if the layer had
     `Expand_Transform` bit set in the v1 `timelineExpandMask`, reveal
     ALL 6 transform properties with `RevealFlags::Explicit`. This
     preserves the "everything visible" state the user chose in v1
     — and because Explicit is sticky, auto-hide won't surprise
     them by dropping properties they'd expected to see.
  3. Same for `Expand_Effects`: reveal all currently-expanded effects'
     header rows explicitly.
- Anything else stays hidden.

This means old projects reopen with a familiar timeline view (only
properties they'd actually animated / edited, PLUS any properties they'd
explicitly kept revealed in the v1 UI).

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
| Auto-hide races with auto-reveal (property hides one frame, reveals next) | **DeepSeek-approved fix:** `RevealFlags` as a bitmask (Explicit / Modified / Animated). Auto-hide check skips ENTIRELY if `Explicit` bit is set. For Modified/Animated bits, auto-hide only fires when the corresponding condition contradicts (Modified but at default value; Animated but no keys and stopwatch off). No frame-to-frame flicker possible. |
| Inline drag-float during playback spams keyframes | Freeze anim clock while any sub-row drag-float is active (same pattern as gizmo/diamond drag; add flag to `anyDragActive` in BeginFrame). |
| Migration on old files reveals nothing | Migration checks BOTH keyframes AND "differs from default" so any user-modified project shows familiar layout. Fallback: if `revealedProperties` is empty after migration AND `timelineExpandMask & Expand_Transform` was set (user had it expanded in Task 5.14), reveal all 6 properties (mirrors old behaviour). |
| BlendMode combo in label column takes too much space on narrow panels | If `labelW < 200` px, hide the combo. Blend still editable in Inspector. Same conditional pattern as the Parent combo removal in Task 5.12b. |
| String comparisons in `IsPropertyRevealed` on a hot path | Bounded: 6 transform props + ~5 params × 3 effects = ~20 entries per layer max. Lookup uses `std::string_view` (per DeepSeek) so passing `"transform.position"` doesn't allocate a temporary. Comparison is `.size() +` char-by-char — trivial at this scale. If profiling ever shows it hot (unlikely), switch to a `uint32_t` FNV hash of the path. |

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

## Sign-off (v2 — post-DeepSeek review)

Full review from DeepSeek incorporated. Changes from v1 of this doc:

- **`RevealFlags` as a bitmask** (Explicit / Modified / Animated)
  instead of single-value enum. Auto-hide skips entirely if Explicit
  bit set — kills the reveal/hide race dead.
- **Scale-default caveat** added: `constructionDefault` must be
  snapshotted at construction time (not hardcoded per-type) because
  Transform properties have non-identity defaults (Scale=1, Anchor=0.5,
  Size=200/120, Opacity=1).
- **Migration fallback** for old `.pmge` files uses
  `RevealFlags::Explicit` on all 6 transform props when the v1
  `Expand_Transform` bit was set — prevents auto-hide from surprising
  users on reopen.
- **`std::string_view` for path lookups** to avoid temporary
  allocations from `const char*` literals.
- **Roadmap reshuffle validated** — DeepSeek called it "the only
  right move" (fixing the wheel before installing a new engine).

All 6 open design questions locked with unanimous user + DeepSeek
recommendations.

**Verdict: Green light. Go code.** — DeepSeek

**Ready to implement.** Awaiting user "go single commit" or equivalent.

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
