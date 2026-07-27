# End-of-day notes — 2026-07-27 (Session 3 → 4 cluster)

Rolling status. Covers the second cluster of the session-3 continuation:
Task 5.14 twirl-down v1 → hotfix → v2 lazy reveal (Commit 19). Ends
with the current commit `4d907f6` shipped CI-green, awaiting user
morning test.

**Doubles as the DeepSeek "where we stand" handoff** — sections
labelled `[DEEPSEEK]` are what to focus on for external review.

---

## `[DEEPSEEK]` One-paragraph tl;dr

Since your last review (Commit 17 design doc `c8f7940` on 2026-07-25),
we shipped the twirl-down v1 (`824eeed`), got real user test feedback,
diagnosed and hotfixed 2 bugs (`fb681e4`), and just landed the lazy-
reveal v2 (`4d907f6`) — the biggest UX rework since Task 5.12 unified
bottom dock. All 5 of your review notes on the v2 design were folded
in (RevealFlags bitmask, constructionDefault snapshot, migration
fallback with `Reveal_Explicit`, `std::string_view` path lookups,
roadmap reshuffle). User tests tomorrow. If green, we move to
Commit 20.

---

## Repo state right now

- **Branch:** `main` on GitHub, clean working tree
- **HEAD:** `4d907f6` feat: lazy property twirl-down v2 (Task 5.15 / Commit 19)
- **CI:** green (Windows-latest MSVC x64 Release, 1.24 MB artifact)
- **Total shipped this cluster:** 4 code commits + 4 docs commits

Recent 10 commits (newest first):

```
4d907f6 feat: lazy property twirl-down v2 (Task 5.15 / Commit 19)
fb681e4 Task 5.14-hotfix: deselect on empty click + freeze pose on stopwatch-off
824eeed Task 5.14: AE-style property twirl-down in timeline (Commit 17)
c8f7940 docs: DESIGN_COMMIT17_AE_TIMELINE_TWIRL.md (v2) + reviewer handoff
ca1fcf0 Quick-win 7: 2x oversample text atlas (fixes 'text blurry when zoomed')
14f83b1 Quick-win 5+6: scale-link no-op resolution + mid-side scale handles
7984e78 Task 5.11-fix-4 (quick-win 4/6): adaptive text-stroke dilation
607fe7e Task 5.13-fix5 + Quick-win 3/6: Drop Shadow 2D gaussian + AE-style bounding box
b335263 Quick-win 2/6: AE-style anchor point crosshair (was cartoonish red dot)
9af3c11 Task 5.13-fix4: fuse DropShadow into single-pass shader (fixes 'shape goes black')
```

---

## What shipped today

### 1. Commit 17 (`824eeed`) — AE-style property twirl-down v1

DeepSeek-reviewed design shipped. Every animated property (Position,
Rotation, Scale, Opacity, Anchor, Size) can now claim its own
horizontal track in the timeline strip via twirl-down. Effects get
similar treatment. Data model additions: `Layer.timelineExpandMask`,
`Layer.expandedEffectIds` (stable effect-IDs per DeepSeek's bitmask-
reorder fix), `Layer.nextEffectId` counter, `DiamondProperty` grows
to include Anchor + Size, `RenderEngine::timelineRows_` member with
`reserve(2048)` for zero heap alloc in the frame loop.

Row model: two-phase (layout pass builds `TimelineRow` list, draw
pass dispatches per Kind). Row heights 18 px headers, 14 px sub-rows.

Net +761 insertions, -308 deletions across 4 files. CI green first
try.

### 2. Hotfix (`fb681e4`) — 2 bugs from user testing v1

**B1 — Selection sticks on empty canvas click.** No deselect handler
existed. Added `SetSelectedId(-1)` when the click hits no layer,
with `MarkForSnapshot()` so Ctrl+Z can restore the selection.
DeepSeek verdict: "correct fix, actually an improvement over AE".

**B2 — Rotation still animates after stopwatch off.** Root cause:
`ToggleStopwatch(t)` cleared keyframes but left `staticValue` at
whatever the property was constructed with (Vec3(0) for rotation).
If the layer's visible rotation was 45° at the moment of toggle-off,
next frame it snapped back to 0. Fix: SNAPSHOT `Evaluate(t)` INTO
`staticValue` BEFORE clearing keyframes. Layer freezes at its
current visible pose. DeepSeek verdict: "exactly correct — matches
AE, no regression risk because Evaluate(t) reads existing keyframes
before they're cleared."

### 3. Commit 19 design doc (`fb681e4` bundled with hotfix)

Full v2 design for lazy property revelation. Sent to DeepSeek for
review. All 6 open questions answered by both my recommendation and
DeepSeek's — unanimous. 5 structural tightenings from DeepSeek
review incorporated into the doc before shipping code:
- `RevealFlags` bitmask (Explicit/Modified/Animated) instead of
  single enum — kills auto-hide/auto-reveal race by design
- `constructionDefault` snapshotted at property construction so
  `IsAtConstructionDefault()` handles non-identity defaults (Scale=1,
  Anchor=0.5, Size=200/120, Opacity=1) without special-casing
- Migration fallback for old `.pmge` files uses `Reveal_Explicit`
  when the v1 `Expand_Transform` bit was set — prevents auto-hide
  from surprising users on reopen
- `std::string_view` for path lookups so `"transform.position"`
  literals don't allocate temporaries
- Roadmap reshuffle validated as "the only right move" —
  original 5-commit roadmap shifts one right, lazy-reveal wedges
  in as Commit 19

### 4. Commit 19 (`4d907f6`) — lazy property twirl-down v2

The design shipped verbatim. ~600 net LOC across 5 source files.
Everything the user asked for in session-3 feedback:

- **Lazy reveal** — properties appear as sub-rows only when
  touched (stopwatch on OR value changed from default OR
  right-click Show)
- **Auto-hide** — property returns to default + no keys + stopwatch
  off + not Explicit-flagged → row disappears next frame
- **Explicit is sticky** — right-click "Show" bit persists through
  value changes, kills the race
- **Inline drag-float** on every revealed sub-row's value
- **`inspectorValueDragActive` freeze-clock** so playback doesn't
  spam keyframes during a sub-row drag
- **Right-click sub-row menu** — Hide Property, Reset to Default
- **Label column reorder** — `[eye 18][twirl 14][blend combo 60]
  [name flex][fx chip 22]` (Parent combo removed from strip)
- **Per-property graph icon** — 3-segment chart glyph, dim/bright
  based on stopwatch state, click switches to SideBySide mode
- **BottomPaneMode::SideBySide** — new third mode splits bottom dock
  horizontally, timeline strip left + graph editor right, 6-px
  splitter, splitter fraction persists in-memory
- **Shift+F3 cycles** Bars → Graph → SideBySide → Bars
- **Serialization** — `revealedProperties` field on Layer, migration
  auto-reveals for old files based on animated/modified state

### 5. Docs shipped

- `DESIGN_COMMIT17_AE_TIMELINE_TWIRL.md` — v1 design (v2 with review
  notes shipped in `c8f7940`)
- `DESIGN_COMMIT19_LAZY_TWIRL_V2.md` — v2 design (locked with
  DeepSeek's 5 fixes)
- `DEEPSEEK_HANDOFF_2026-07-25_V2.md` — the review paper trail
- `TEST_FEEDBACK_2026-07-25_TWIRL.md` — triage of user's 10-item
  feedback batch from testing v1
- Updated `TEST_FEEDBACK_2026-07-24_BOUNCING_BALL.md` with resolved
  items marked

---

## `[DEEPSEEK]` Architecture snapshot as of `4d907f6`

For your context — if there's an earlier codebase mental model, this
is where we've drifted since your last look.

### Data model

**`AnimatedProperty<T>`:**
- Fields: `staticValue`, `keyframes`, `stopwatchEnabled`,
  `constructionDefault` (new, Task 5.15)
- Constructor stashes initial value into both `staticValue` AND
  `constructionDefault`
- `ToggleStopwatch(t)` on OFF: `staticValue = Evaluate(t)` before
  `keyframes.clear()` — freezes pose
- `IsAtConstructionDefault()` uses non-template `AP_ValuesEqual`
  overloads (float/Vec2/Vec3) with 1e-6f slop — avoids polluting
  MathTypes.h with a global `operator==`

**`Layer`:**
- Existing: id, parentId, transform (6 AnimatedProperty fields),
  fillColor/strokeColor/strokeWidth/cornerRadius, TextProps,
  effects vector, inPoint/outPoint/blend
- Added (Task 5.14): `timelineExpandMask` (Transform/Effects bits),
  `expandedEffectIds` (vector<int>, keyed by stable effect IDs),
  `nextEffectId` counter, `IsEffectExpanded`/`ToggleEffectExpand`
- Added (Task 5.15): `revealedProperties` (vector<RevealedProperty>
  where RevealedProperty = {string path, uint8 flags}), enum
  `RevealFlags { None=0, Explicit=1, Modified=2, Animated=4 }`,
  helpers `IsPropertyRevealed / GetRevealFlags / RevealProperty /
  ClearRevealFlag / HideProperty` (all take `std::string_view`),
  private `AutoExpandGroupForPath` called from `RevealProperty`

**`Effect`:**
- POD with `int id` (stable per-layer), `EffectType`, `bool enabled`,
  `EffectParams` (four float4 slots), `displayName`
- Factories (`MakeMotionTile/DropShadow/etc.`) don't touch id —
  caller assigns via `Layer::AddEffect` which auto-increments

### Rendering pipeline

- `CompositionRenderer` — DX11 shape rasterizer, `ps_shape_sdf_`
  (Iñigo Quílez rounded rect + fast ellipse with fwidth AA),
  `ps_text_` (adaptive multi-ring dilation for stroke, Task 5.11-fix-4),
  `ps_null_`. `ClearComp`/`RenderSingleLayer` helpers for per-layer
  isolation path.
- `EffectManager` — exactly 2 ping-pong RTs (~16 MB @ 1080p),
  `ps_dropshadow_fused_` single-pass fused DropShadow (Task 5.13-fix4).
  `ApplyChain` with source-adjacent RT selection + post-loop
  passthrough copy. `DrawFullscreenPass` explicitly binds
  `rasterizer_none_` + `OMSetDepthStencilState(nullptr, 0)` —
  do NOT remove these without proving no upstream code leaves
  `CULL_BACK` bound (see Task 5.13-fix2 5-hour bug hunt).
- `RenderEngine::DrawViewportCanvas` per-layer dispatch: layers
  without effects → fast path direct to compRTV; layers with
  effects → isolation path (clear pingRTV → RenderSingleLayer to
  pingRTV → ApplyChain(pingSRV, pongRTV) → CompositeSRVOver(pongSRV,
  compRTV)).

### Timeline UI

- **Task 5.12 base:** unified bottom dock, `[Bars][Graph]` toggle,
  6-px splitter over label|track divider, dock state persists to
  imgui.ini via `SettingsHandler` under `[PotatoBottomDock][State]`
- **Task 5.14 twirl v1:** two-phase row-list model
  (`std::vector<TimelineRow>` on RenderEngine, `reserve(2048)`),
  layer-header rows + transform sub-rows + effect header rows,
  variable heights (18/14 px), reorder-drag only on header rows
- **Task 5.15 twirl v2:** lazy reveal filter, inline drag-float
  editing, label column reorder with blend combo + parent removed,
  per-property graph icon, `BottomPaneMode::SideBySide` third mode
  with strip-left/graph-right splitter (60/40 default, persists
  in-memory)

### `[DEEPSEEK]` Coding invariants (unchanged from PROJECT_BRIEFING §7)

- No `// TODO` — ship working stubs
- Zero heap alloc in the frame loop (enforced: `timelineRows_`
  reserved, `revealedProperties` mutations happen at user-interaction
  cadence not per-frame)
- Defensive C++20 (all pointer / HRESULT / divide checks)
- `#define NOMINMAX` and `#define WIN32_LEAN_AND_MEAN` before every
  `<windows.h>`
- Stable `Layer.id` — NEVER use vector index for identity
- Stable `Effect.id` per-layer (Task 5.14) — NEVER use vector index
- Left-handed coords (DX11 native)
- Vertical FOV in degrees
- Design-doc-first for large features; small bugfixes can skip

---

## `[DEEPSEEK]` What we're asking user to test tomorrow

If any of these fail, I iterate. If all pass, we proceed to
Commit 20.

**Core lazy-reveal model:**
1. Fresh Rect → twirl Transform open → NO sub-rows appear
2. Turn Position stopwatch ON → Position sub-row appears
3. Turn stopwatch OFF (no other change) → sub-row auto-hides
4. Drag Scale to 1.5 in Inspector → Scale sub-row appears
5. Reset Scale to 1.0 → auto-hides
6. Right-click revealed sub-row → "Hide Property" → row disappears
7. Right-click → "Reset to Default" → clears keys + hides row

**Label column reorder:**
8. `[eye][twirl][blend combo][name][fx chip]` — Parent combo gone
9. Blend combo changes layer blend mode correctly

**Inline editing:**
10. Click any sub-row's value → drag-float widget → scrubs value
    + freezes playback during drag (no keyframe spam)

**Per-property graph icon:**
11. Chart glyph at right edge of label column, dim when
    stopwatch off, bright when on
12. Click → switches to SideBySide mode

**SideBySide layout:**
13. Shift+F3 cycles through 3 modes
14. Toolbar `[Side]` button — same effect
15. Draggable splitter in the middle

**Backward compat:**
16. Save + reopen — sub-rows and reveal state persist
17. Load old (pre-Commit 19) .pmge — sub-rows appear for touched
    properties via migration, no properties disappear

**Regression check (Task 5.14 features):**
18. Effect twirl shows effect headers (param sub-rows deferred to
    Commit 22)
19. Layer reorder drag still works from header rows only
20. Diamond drag / context menu still works on revealed sub-rows

---

## Roadmap — remaining commits

Locked, DeepSeek-endorsed order. Each is expected to be one commit
unless design-doc-worthy scope emerges.

| # | Commit | What | Rough LOC |
|---|---|---|---|
| 17 | ✅ shipped `824eeed` | AE twirl-down v1 | +761 |
| 18 | ✅ shipped `fb681e4` | Deselect + freeze-pose hotfix | +289 |
| 19 | ✅ shipped `4d907f6` | Lazy twirl v2 + graph rework | +690 |
| **20** | **NEXT** | **Split Layer tool + Motion Path overlay** | ~250 |
| 21 | after 20 | Separable 2-pass Gaussian blur (fixes DropShadow softness, foundation for Glow) | ~200 |
| 22 | after 21 | **Effect param animation** — every `Effect.params.p*[i]` becomes `AnimatedProperty<float>`, timeline sub-rows wire in, fill/stroke/color also gain AnimatedProperty | ~500 + design doc |
| 23 | after 22 | Track Mattes + Adjustment Layers | ~400 + design doc |

Nice-to-haves parked (not roadmap gating):
- Persistence of `BottomPaneMode` + `bottomPaneGraphSplit` to
  imgui.ini (~15 LOC via existing SettingsHandler)
- `DrawGraphEditor` honoring `graphFocusedLayerId/Property` fields +
  breadcrumb bar (~30 LOC)
- `U` / `UU` AE-style shortcuts for "show animated" / "show modified"
- DX11 debug layer routing (`ID3D11InfoQueue` → OutputDebugString +
  file log) — would have saved the Task 5.13 5-hour bug hunt;
  worth doing before shipping v1.0
- Undo memory cap (deque currently unbounded)
- Full AE 2025 UI brief: Shy / Solo / Lock icons, Track Matte
  pickwhip, layer groups (twirly), pill bars, column show/hide bar,
  F4 column toggle

Explicitly won't do:
- Direct AE feature-parity race — stay potato-scoped
  (4 GB RAM / integrated GPU)
- Nested compositions ("pre-comps") — too much complexity
- Plugin API — same reasoning

---

## `[DEEPSEEK]` What we specifically want you to review tomorrow

Once user tests `4d907f6` and reports results, we'll send you
another handoff. Two anticipated questions:

**If tests pass:**
- Commit 20 (Split Layer + Motion Path) design doc — you review
  the split-layer semantics (does Split Layer preserve effects on
  both halves? separate ids? undo transactions?) and the motion-
  path sampling strategy (Bezier-arc-length vs uniform-time
  parameterization; polyline dot spacing; hit-test for scrubbing
  by clicking a dot).

**If tests fail:**
- Whatever specific bug the user hits. Likely candidates given
  the size of Commit 19:
  - The inline drag-float might have hit-test collision with the
    diamond track (the value widget's InvisibleButton runs across
    the same X range as the leftmost diamond)
  - The right-click context menu might conflict with the diamond
    right-click menu (both open at the mouse position)
  - The `AutoExpandGroupForPath` might cause a "twirl closed
    then any Inspector edit re-opens it" surprise
  - SideBySide split of DrawGraphEditor might have missing
    scroll-context state (I ran it inside a child window but
    didn't audit GraphEditor for parent-window assumptions)

---

## Lessons carried forward from this cluster

1. **Design docs paid off again.** Task 5.13 was a 5-hour hunt for a
   silently-culled fullscreen triangle. Task 5.14 shipped clean first
   try because DeepSeek caught the effect-bitmask reorder bug in the
   design doc. Task 5.15 (Commit 19) shipped clean first try because
   DeepSeek caught 5 more structural issues (bitmask races, Scale
   default, migration fallback, string_view, roadmap validation) —
   all before code was written.

2. **User feedback iteration cycle is tight.** Ship v1 → user tests
   → real feedback batch → triage doc → design doc → external review
   → ship v2. Two full round-trips this session, both green.

3. **Explicit > inherited state (still).** The Task 5.13-fix2
   `rasterizer_none_` + `OMSetDepthStencilState(nullptr, 0)` invariant
   is holding up. Zero regressions since.

4. **Path A over Path B was right.** We paused the original 5-commit
   roadmap (Split Layer next) to fix the twirl-down UX before it
   solidified. If we'd shipped Split Layer on top of a half-baked
   twirl model, the fix would have been 2× harder later. DeepSeek
   validated: "fix the wheel before installing a new engine."

5. **Small bugs during big-feature test sessions.** User's session-3
   feedback (10 items) had 2 real bugs + 8 UX changes. Splitting
   them into a fast hotfix + a design-heavy v2 kept the review flow
   clean instead of merging everything into one giant commit.

---

## Sleep well.

Repo state clean, CI green, docs current, design paper trail complete.
Tomorrow: user tests `4d907f6`. If green → Commit 20 (Split Layer +
Motion Path). If bugs → iterate + hotfix.

PAT status: used ~15 times this cluster, scrubbed after every push,
rotate when convenient.
