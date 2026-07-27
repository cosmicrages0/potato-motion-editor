# Design doc — Commit 17: AE-style property twirl-down in the timeline

**Task ID:** 5.14 (proposed)
**Design-doc-first per workflow.** No code until approved.
**Estimated scope:** ~380 LOC (was 450 in v1; DeepSeek review trimmed inline-editing scope).

**Reviewers consulted:** Gemini + DeepSeek (see `GEMINI_HANDOFF_2026-07-25.md` for
handoff, and the merged consensus below).

## v2 changes from external review

Three real issues flagged by DeepSeek that Gemini missed. All incorporated
into this doc before code lands:

1. **Effect expand-state must be keyed by effect identity, NOT bit position.**
   If the user reorders effects (which they can — Effects panel supports
   drag-reorder), a bitmask indexed by effect slot points at the wrong
   effect after any reorder. Fixed by giving each `Effect` a stable
   `effectId` (auto-increment per layer) and storing expand-state as
   `std::vector<int>` of expanded effect IDs. O(n) lookup but n ≤ 32 —
   trivial.
2. **`std::vector<TimelineRow>` violates zero-heap-alloc-in-frame-loop rule
   (Section 7 of PROJECT_BRIEFING).** Fixed by promoting the row-list to a
   `RenderEngine` member field and calling `.clear()` (which preserves
   capacity) + `.reserve()` once at Initialize to a max reasonable size
   (say 2048 rows = 100+ layers with everything expanded). Never
   reallocates during a frame.
3. **`[unroll]` in the text-stroke shader forces all 42 taps to run.**
   This is a pre-existing perf note from Task 5.11-fix-4, not a Commit 17
   issue, but DeepSeek's review surfaced it. Parked as a follow-up: replace
   with a proper SDF-based stroke (constant cost regardless of width) as
   part of the eventual "text quality follow-up" pass.

## Locked design decisions (from user + reviewer merge)

- Row height: **14 px sub-rows, 18 px header rows.** More info on screen
  without wrecking clickability. DeepSeek recommendation adopted.
- Value readout: **read-only text in v1.** Inline drag-floats deferred to
  a follow-up commit (~200 LOC of scope creep DeepSeek correctly called
  out). Gemini pushed for inline v1 → overruled.
- Twirl icon: **filled triangles ▶/▼** drawn via ImDrawList. Both
  reviewers agreed.
- Auto-expand on select: **none.** Both reviewers agreed. Match AE.
- `fx` chip visibility: **only when the layer has effects.** Both agreed.

## Roadmap (Gemini-proposed, DeepSeek-endorsed)

This commit + 4 follow-ups form the next epic:

| # | Commit | Feature | Rough size |
|---|---|---|---|
| 17 | **THIS** | AE-style property twirl-down + effect bitmask fix | ~380 LOC |
| 18 | Next | Split Layer tool + motion path overlay | ~250 LOC |
| 19 | Next+1 | Separable 2-pass Gaussian blur (unlocks proper DropShadow softness + reusable for Glow, Fast Blur) | ~200 LOC |
| 20 | Next+2 | Effect param animation (each `Effect.params.p*[i]` becomes an `AnimatedProperty<float>` — sub-rows in Commit 17 get their diamond tracks) | ~500 LOC + design doc |
| 21 | Next+3 | Track Mattes + Adjustment Layers | ~400 LOC + design doc |

Commit 20 depends on Commit 17's sub-row infrastructure. Commit 19 is
independent — could ship in parallel if we wanted. Commit 18 items are
mostly independent; grouped because they're both small.



---

## Problem statement

User feedback from the bouncing-ball test session (item 3.1 in the
triage doc):

> "in timeline all the keyframes are one on layer looks stressfull and
> not good when editing it overlaps other cant use"
>
> "i need like ae that when i select any property or effect it shows
> under the name of the layer like a dropdown, then it has options like
> visibility/show, stopwatch to add keyframes"

Currently every animated property's keyframe diamonds stack onto ONE
horizontal row per layer. A layer with animated Position + Scale +
Rotation + Opacity + squash-stretch = 6+ diamonds crowded into an 18-px
row. Overlapping diamonds are unclickable, and it's impossible to see
at a glance which property has what keyframes.

AE's fix: each layer has a **twirly `>` next to its name**. Click it,
the layer expands vertically into per-property sub-rows. Each sub-row
gets its own name, its own stopwatch icon, its own keyframe track.
Multi-hierarchy (Transform > Position, Transform > Scale, ..., Effects >
Drop Shadow > Distance, ...).

This is the single biggest UX unlock left on the backlog. It's blocking
serious animation work.

---

## Non-goals (explicit, protect scope)

- **Per-effect-parameter keyframing** — that's item 2.1 in the triage,
  needs its own architectural commit. This doc lays the UI foundation
  (each effect gets an expandable row) but each effect parameter
  displays as static text for now, not animated. When effect-param
  animation ships later, it drops into the same twirl UI with no
  further UI work needed.
- **Alight-Motion per-segment curves in Bars mode** (triage item 1.4).
  Separate task.
- **Column show/hide bar / A/V toggle columns / Shy / Solo / Lock**
  from the AE 2025 UI brief. Those need their own design doc.
- **Layer groups (parent-child twirl of layers themselves)**. Different
  feature — this doc is about property twirl WITHIN a layer.
- **Preserving imgui.ini state** for per-layer twirl-open bool. First
  cut: expanded state lives in `Layer` in-memory, resets on project
  reload. Add persistence in a follow-up (~10 LOC).

---

## Design

### Data model changes

`src/Effect.h`:

```cpp
struct Effect {
    // ... existing fields ...

    // Task 5.14 (v2 fix): stable identity for expand-state tracking.
    // Auto-assigned on effect creation from a per-layer counter — NOT
    // an index into the vector. Persists through Effects-panel reorder
    // so the timeline strip's expanded-parameter-list state follows the
    // effect across reorders, not the slot the effect used to occupy.
    // Not serialised — expand state is transient.
    int effectId = -1;  // -1 = unassigned; helper assigns on push
};
```

`src/Layer.h`:

```cpp
struct Layer {
    // ... existing fields ...

    // Task 5.14: which sub-sections are twirled open in the timeline
    // strip. Bit flags for cheap add-more-categories later
    // (Masks, Text properties, etc.). In-memory only for now — not
    // serialised. Twirl state resets to collapsed on project reload
    // (persistence to imgui.ini deferred to a follow-up).
    enum ExpandFlag : uint32_t {
        Expand_Transform = 1 << 0,  // Position/Rotation/Scale/Anchor/Size/Opacity
        Expand_Effects   = 1 << 1,  // list of Effect entries
        Expand_Text      = 1 << 2,  // TextProps (only for Text layers)
    };
    mutable uint32_t timelineExpandMask = 0;

    // Task 5.14 (v2 fix): which effect entries have their parameter
    // sub-rows expanded. Stored as a list of effect IDs (NOT indices)
    // so Effects-panel reorder doesn't scramble the expand state.
    // O(n) lookup on IsEffectExpanded() but n <= 32 (ApplyChain cap).
    mutable std::vector<int> expandedEffectIds;

    // Task 5.14 (v2 fix): per-layer effect ID counter. Increments on
    // every AddEffect / MakeXxx factory call, giving each effect a
    // unique-within-this-layer ID for expand-state and future features
    // (undo of param edits, effect-scoped keyframes in Commit 20).
    mutable int nextEffectId = 0;

    // Small helpers on Layer:
    bool IsEffectExpanded(int effectId) const;
    void ToggleEffectExpand(int effectId);
    int  AssignEffectId();  // ++nextEffectId, returns old value
};
```

Where an `Effect` is pushed onto `layer.effects`, we call
`layer.AssignEffectId()` first and stamp it into `Effect.effectId`. The
existing `MakeMotionTile / MakeChromatic / MakeBlend / MakeDropShadow /
MakeMotionBlur` factories don't touch this — the CALLER (the Effects
panel Add-Effect button) does it after construction. Clean separation.

**Backward-compat for old .pmge files:** effects loaded from JSON have
`effectId = -1`. On first frame after load, we walk `layer.effects` and
assign fresh IDs via `AssignEffectId()`. `expandedEffectIds` is always
empty on load (transient), so no ID collision possible.

### Row model

The strip today draws N rows where N = layer count, and every layer
gets exactly ONE 18-px row. New model: layer count → row COUNT is
variable, computed each frame:

- 1 row for the LAYER HEADER (name, Vis, Parent — matches today's row)
- If `Expand_Transform` bit set: 6 sub-rows (Position, Rotation, Scale,
  Anchor, Size, Opacity), one per AnimatedProperty
- If `Expand_Effects` bit set: 1 sub-row per effect header. If that
  effect's bit in `effectExpandMask` is also set: N more sub-rows for
  its parameters (Drop Shadow has 5: Distance/Angle/Softness/Opacity/
  Color; Chromatic has 3; etc.)
- If `Expand_Text` bit set (Text layers only): 1 sub-row per TextProps
  field (font, size, weight, italic, alignment, stroke width, stroke
  color, fill color). Most aren't animated yet — display as static
  editable text with a disabled stopwatch icon (visual polish, no
  functionality).

Row heights are non-uniform (v2 change per DeepSeek):
- Layer HEADER rows: `kHeaderRowH = 18.0f` (matches current strip)
- Sub-rows (Transform, Effect header, Effect param): `kSubRowH = 14.0f`
  — fits ~28% more information on screen without wrecking diamond
  clickability. Diamond radius is 5 px; a 14 px row with the diamond
  centered leaves 4 px above/below for hit-test slop.

A layer with everything expanded and 2 effects with 3 params each is
`18 + 6*14 + 14 + 2*14 + 3*14 + 3*14 = 18 + 84 + 14 + 28 + 42 + 42 = 228 px`.
(vs 288 with uniform 18 px rows.) Fine for typical use; scroll-region
wraps.

### Sub-row visual style

**Header row** (top of each layer group) — matches today's row EXACTLY:
- `[Vis 18px] [name 4px pad] [Parent combo 90px if room]`
- **NEW:** small twirl `>` / `v` icon at the very left, before Vis
  (12 px wide). Click to toggle `Expand_Transform` bit.
- **NEW:** to the right of the Parent combo, small `fx` chip. Click
  to toggle `Expand_Effects` bit. Only visible if the layer HAS
  effects (or hidden but always clickable if we want "add effect
  from timeline" — probably later).

**Transform sub-row** (Position, etc.) — 14 px tall:
- Indent left edge by 24 px (twirl + Vis indentation)
- `[Stopwatch 12px] [property name (Position, etc.)] [value text 80px]`
  where "value text" shows the CURRENT evaluated value like `(320.5,
  180.0)` in a smaller font, dim color.
- **v2: read-only in v1** — click the property name jumps focus to the
  Inspector's matching field for editing (nice-to-have; skip if it
  costs scope). Inline drag-float editing deferred to a follow-up
  commit (~200 LOC, per DeepSeek's scope call).
- Track column: same X range as today (labelW → stripW - 6). Draws
  only THIS property's diamonds in THIS property's color.

**Effect header sub-row** (Drop Shadow, Chromatic, etc.) — 14 px tall:
- Indented 24 px from strip origin
- `[Twirl 10px] [Enable checkbox 12px] [Effect name]`
- Small `X` at end to remove
- No keyframe track (the effect itself isn't a scalar)

**Effect parameter sub-row** (Distance, Angle, ...) — 14 px tall:
- Indented 48 px (twirl + effect-level indent)
- `[Stopwatch disabled-look 12px] [Param name] [value 80px]`
- Track column reserved but empty for now (until Commit 20 lands)

**Visual accents:**
- Header rows get the existing `IM_COL32(50, 50, 80, 200)` selection
  tint when selected
- Transform sub-rows get a slightly darker bg tint so they visually
  group under their layer
- Effect header rows get a subtle blue/purple tint so effects stand
  out from Transform
- Effect parameter rows get an even darker tint

### Row iteration + hit-test rework

The current `DrawTimelineStrip` has a single `for (size_t rowI = 0;
rowI < nLayers; ++rowI)` loop. Change to a two-phase model:

1. **Row layout pass:** walk layers in AE-order, populate a member
   vector on `RenderEngine`:
   ```cpp
   struct TimelineRow {
       int   layerId;
       enum Kind : uint8_t {
           LayerHeader,     // header row for the layer group
           TransformProp,   // Position/Rotation/Scale/Anchor/Size/Opacity
           EffectHeader,    // effect entry name + enable + remove
           EffectParam,     // one row per effect param (Distance, Softness, ...)
       } kind;
       uint8_t subIndex;    // DiamondProperty for TransformProp,
                            //   effect vector index for EffectHeader,
                            //   param index (0..3) for EffectParam
       uint8_t rowH;        // 18 for headers, 14 for sub-rows
       float   y0;          // computed screen Y
   };

   // On RenderEngine class:
   std::vector<TimelineRow> timelineRows_;    // scratch, reused each frame
   ```

   **v2 fix (DeepSeek):** promote from stack-local to a member field.
   Initialize with `timelineRows_.reserve(2048)` in `RenderEngine::
   Initialize()`. In `DrawTimelineStrip` we call
   `timelineRows_.clear()` at the top (preserves capacity — no
   heap alloc). 2048 rows accommodates 100 layers × 20 sub-rows each,
   which is beyond any realistic use. Enforces the Section 7 rule
   "zero heap alloc in the frame loop."

2. **Draw pass:** iterate rows, dispatch on `Kind` to the right draw
   function. Diamond drawing collapses to a single template call per
   row per animated property.

This keeps the code linear and adds each row-type as its own function:
- `DrawLayerHeaderRow(Layer&, const TimelineRow&)`
- `DrawTransformPropRow(Layer&, const TimelineRow&, int propIdx)`
- `DrawEffectHeaderRow(Layer&, const TimelineRow&, int effectIdx)`
- `DrawEffectParamRow(Layer&, const TimelineRow&, int effectIdx, int paramIdx)`

### Twirl click → toggle bit

Left-click a twirl icon → toggle the appropriate bit in
`timelineExpandMask` or `effectExpandMask`. `MarkForSnapshot()` first
so Ctrl+Z restores the previous expanded state (small quality-of-life
detail — worth it because reordering-with-expanded state is a common
edit flow).

Twirl icon draws as two triangles: right-pointing `▶` when collapsed,
down-pointing `▼` when expanded. Both are 8-px filled triangles via
ImDrawList — no font glyph needed (avoids the same font-dependency
issue we hit with the eye/chain icons).

### Drag-to-reorder scope

Reorder drag remains LAYER-level (grab layer header row → move whole
layer group). Sub-rows within an expanded group can't be reordered
(Position row always precedes Scale row per AE convention). The
existing reorder-drag code moves layers by their vector index; that
still works, we just need to skip the drag hit-test on sub-rows.

Concretely: `InvisibleButton("##layerReorder", ...)` only lives on
`Kind==LayerHeader` rows. Sub-rows install NO reorder button; if user
grabs a sub-row, nothing happens.

### Keyframe diamond routing

Today's `drawAndHitKeys` template lambda takes an `AnimatedProperty&`
+ a `DiamondProperty` enum + a color. Under the new model, we call it
once per TransformProp sub-row with the specific property. The
`DiamondProperty` enum grows to include the two currently-missing ones:

```cpp
enum class DiamondProperty : int {
    Position = 0, Rotation = 1, Scale = 2, Opacity = 3,
    Anchor   = 4, Size     = 5,  // Task 5.14: previously untracked
};
```

Serialization stays untouched — DiamondProperty is UI-only, not on
disk. The `RemoveKeyAt` dispatch switch in the diamond context menu
(line 1370 area) gets 2 new cases.

### Column widths recap

Same as today:
- `labelW` = `stripW * bottomPaneSplitFrac`, clamped [autoMin,
  stripW*0.60]
- `trackX0 = labelW + 4`
- Splitter drag unchanged

Within `labelW`, indent is applied on sub-rows only. Header rows
keep the current `[Vis][name][parent]` layout at zero indent.

### Rulers, playhead, trim bars

Unchanged. Trim bars still live on the layer header row (not on
sub-rows). Playhead spans the full track height, all rows.

### Selection semantics

Clicking a layer HEADER row = select that layer (as today). Clicking
a sub-row also selects the parent layer but ALSO sets a
`selectedProperty` marker (new field, in-memory) so the Inspector can
scroll/highlight the corresponding property. Nice-to-have; skip if it
costs too much scope.

### Backward compatibility

- **Old `.pmge` files load unchanged** — no serialisation touched
- **Twirl state is transient** — reloading a project starts with
  everything collapsed. Persistence to `imgui.ini` deferred.
- **Existing keybindings + shortcuts unchanged** — Shift+F3 still
  toggles Graph mode, Delete still removes near-playhead keyframes
- **Existing diamond drag/context-menu code reused verbatim** — we
  just call it from per-property sub-rows instead of the merged
  layer row

---

## Implementation plan (order)

1. **Add fields to `Layer.h`**: `timelineExpandMask`, `effectExpandMask`.
   `DiamondProperty` enum extended with Anchor + Size.
2. **Build the `TimelineRow` list** at the top of
   `DrawTimelineStrip`. Compute total height for scroll region.
3. **Draw layer header row** — 90% code reuse from today. Add the
   left-margin twirl icon and click handler.
4. **Draw transform sub-rows** — factor out `drawAndHitKeys` call per
   property. Add stopwatch icon draw (12-px watch glyph, filled when
   ON) + property name + value readout. Wire stopwatch click to
   `layer.transform.<prop>.ToggleStopwatch(t)` with `MarkForSnapshot`.
5. **Draw effect header rows** — enable checkbox + name + `X` remove
   button + twirl for parameter expansion.
6. **Draw effect parameter rows** — read-only for now, value text
   from `Effect.params.p*[i]` interpreted per effect type.
7. **Update diamond context-menu switch** for Anchor + Size cases.
8. **Update reorder-drag** to only fire on layer header rows.
9. **Manual test matrix** — 5+ layer scene with mixed
   Transform/Effects expansion. Verify diamond hit-test, splitter
   drag, trim bar drag, Shift+F3 into Graph mode + back.
10. **Debug panel readout** — show "expanded layers: N, total rows: M"
    for user + my own sanity.

---

## Estimated LOC breakdown

| File | Add | Del | Net |
|---|---|---|---|
| `src/Layer.h` | 12 | 0 | +12 |
| `src/RenderEngine.h` | 15 | 2 | +13 |
| `src/RenderEngine.cpp` | 380 | 60 | +320 |
| `DESIGN_COMMIT17_AE_TIMELINE_TWIRL.md` | (this doc) | | |
| **Total** | ~410 | ~62 | **+350** |

Fits within the "big feature single commit" size we've done before
(Task 5.12b was ~250 LOC).

---

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Row iteration bug — off-by-one in Y math | `TimelineRow.y0` computed once, all draws use it. Debug panel shows rowCount and per-layer sub-count. |
| Diamond hit-test breaks when rows shift | Diamonds are drawn PER-ROW with rowYc from the row's y0. Same math as today. No coupling to layer index. |
| Reorder drag grabs a sub-row and confuses | Explicit `if (row.kind == LayerHeader)` gate on the InvisibleButton install. Sub-rows have zero interactive widgets besides the stopwatch. |
| Twirl click races with layer-select click | Twirl is 12 px on the far left. Rest of the header row still triggers select. Careful ID stack (`##twirl_<layerId>`) so it wins hit-test in its zone. |
| Perf on 30+ layer scenes | Row list rebuild is O(N + expanded sub-counts). Even 100 layers with everything expanded = ~500 rows = negligible. Diamond hit-test is O(keys per property per expanded row), same as today. No frame-budget concern. |
| ImGui ID collisions | Use `PushID((int)(0x70000000 \| layer.id * 100 + subIdx))` per sub-row. Existing IDs use 0x7A-0x7F prefixes; 0x70-0x79 free. |
| Text-layer stopwatch on TextProps that don't have AnimatedProperty wrappers | Skip Text sub-rows in v1. Only add when we actually animate text (future task). Removes an entire failure mode from the commit. |

---

## Explicitly deferred to follow-ups

- **Persistence of expand state to `imgui.ini`** (~10 LOC via existing
  `SettingsHandler` from Task 5.12)
- **Text property sub-rows** (Text layers don't animate TextProps yet)
- **Effect param animation** (item 2.1 triage — architectural)
- **Alight-Motion per-segment curves in Bars mode** (item 1.4)
- **Sub-row selection propagates to Inspector focus** (nice-to-have)
- **Column widths per sub-row-kind customisation** (right now all
  sub-rows share the same labelW)
- **Text-stroke shader optimisation** — the current adaptive multi-ring
  dilation (`ps_text_`, Task 5.11-fix-4) uses `[unroll]` on the
  per-ring loops. HLSL `[unroll]` fully expands the loop at compile
  time, so the shader bytecode always contains all 42 taps even when
  `sw < 8` and the runtime `if` short-circuits most of them. On
  integrated Intel HD at 1080p this is measurable overhead. Real fix:
  signed-distance-field stroke — constant cost regardless of stroke
  width, and perfect quality (no ghosting, ever). Pre-existing tech
  debt, unrelated to Commit 17, flagged by DeepSeek review so it
  doesn't get forgotten.

---

## Sign-off (v2)

All 5 review questions from v1 are now **LOCKED** based on user +
Gemini + DeepSeek merged review:

| Question | Answer | Source |
|---|---|---|
| Row heights | 18 px headers, **14 px sub-rows** | DeepSeek |
| Value readout | **Read-only text in v1**; inline editing = follow-up commit | DeepSeek |
| Twirl icon | Filled triangle ▶/▼ via ImDrawList | Both agreed |
| Auto-expand on select | **None** (fully collapsed by default) | Both agreed |
| `fx` chip visibility | **Only when layer has effects** | Both agreed |

Plus 2 additional fixes from DeepSeek review:
- Effect expand state keyed by stable `effectId`, not vector index
- `timelineRows_` promoted to member field with reserved capacity —
  no heap alloc in the frame loop

## Implementation-time checklist (DeepSeek review, do NOT skip)

Three things to verify while coding to avoid known-bad shortcuts:

1. **Effect ID migration path.** When deserialising an `Effect` from
   JSON, if `effectId` field is missing (old `.pmge` files), assign a
   fresh one via `layer.AssignEffectId()` immediately after push.
   Serialise `effectId` on save so it survives reload.
   Load-time migration is MANDATORY — without it, expand-state
   scrambles the first time a user reopens an old project with
   effects.

2. **`timelineRows_` capacity handling.**
   - `timelineRows_.reserve(2048)` in `RenderEngine::Initialize()`.
   - `timelineRows_.clear()` at the top of every `DrawTimelineStrip()`.
   - NEVER call `shrink_to_fit()` — that reallocates.
   - Assert `size() <= 2048` in debug builds; if we ever hit it, bump
     the reserve constant (100+ layers with everything expanded
     realistically caps around 1500).

3. **Row Y-math.** Precise formula to avoid off-by-one:
   - `rulerH = 22.0f` (unchanged from today)
   - Header row Y = `rulerH + (sum of prior rows' heights)`
   - Sub-row Y = same formula; height comes from the row struct's
     `rowH` field (14 for sub-rows, 18 for headers)
   - **v2 note:** DeepSeek proposed an extra 4-px gap between header
     and its sub-rows. Skipped for v1 — the twirl icon on the header
     row is a strong enough visual separator, and the extra gap
     complicates hit-test math. Add later if user feedback asks.

**Ready to code.** Awaiting user "go single commit" or equivalent.
