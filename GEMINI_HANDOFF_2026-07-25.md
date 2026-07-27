# Gemini review request — Potato Motion Graphics Editor session snapshot

**Date:** 2026-07-25
**Project:** Native Windows x64 C++20 motion graphics editor
**Target hardware:** 4 GB RAM / integrated GPU ("potato" PC constraint)
**Stack:** DirectX 11, SDL2, Dear ImGui (docking branch), FFmpeg (export),
DirectWrite (text), nlohmann/json (isolated in Serialization.cpp)
**Repo:** https://github.com/cosmicrages0/potato-motion-editor
**Current commit:** `ca1fcf0` (branch `main`)

---

## What we need from you

Two asks:

1. **Retrospective cross-check.** Look at what shipped in the last
   ~3 sessions (~15 commits) and tell us if any of the fixes were
   architecturally wrong, likely to regress, or where a better path
   was available. We're specifically nervous about the Task 5.13 bug
   hunt (took 5+ commits to find a rasterizer state issue) and the
   Drop Shadow fused shader.

2. **Design review.** We have a design doc queued for the next commit
   — a big AE-style timeline property twirl-down feature. Read it,
   flag anything wrong or missing, especially:
   - Row-iteration data model
   - Backward-compat with existing keyframe diamond code
   - Anything you'd do differently for the ImGui interaction model

The design doc is included at the end of this file (full text).

---

## Recent session accomplishments (chronological, this cluster)

Working from most recent backward. Every fix landed CI-green
(windows-latest MSVC x64 Release, artifact ~1.23 MB).

### `ca1fcf0` — Quick-win 7: 2x text atlas oversample
User complaint: "the text get blurry when zoomed, looking like ps2
graphics". Root cause: text atlas was rasterized ONCE at requested
fontSize, then sampled by a fixed-size shape quad — bilinear stretch
blur under any zoom or layer scale.

Fix: multiply fontSize by `kTextOversample=2` before
`CreateTextFormat`. Atlas texture is 2x the visual dims. Downstream
(MVP quad size, stroke UV step, gizmo hit-box, `sizePixels`
auto-grow) reads the DIVIDED "visual" dims via ceiling-divide in
`EnsureLayerCache`. 4x VRAM per text layer atlas — negligible
(~28 KB for 72pt headline).

Trade-off known and documented: works up to ~200% effective zoom.
Past that, adaptive re-rasterize would be the real AE fix (parked).

### `14f83b1` — Quick-wins 5+6: scale-link no-op + mid-side scale handles
User feedback interpreted as scale-link bug, on re-read confirmed as
"pure AE behaviour is what I want" (preserve ratio). Existing code
already does this — no change needed, documented in TEST_FEEDBACK
triage as resolved.

Actual code work: added 4 mid-side scale handles (N/E/S/W midpoints)
to selection bounding box. `GizmoMode` enum grew from 6 to 10 values.
`DrawSelectionBox` helper takes an optional `drawMidHandles=true`
param. Hit-test in `HandleGizmoInteraction` adds 4 more `dist(mouse,
midpoint) < kHit` checks. **Elegant discovery:** the existing scale
math has a `startDX=0 -> sx=1.0` short-circuit for the divisor
guard. Placing the mid-side `handleLocalStart` on the anchor's
X-column (N/S) or Y-row (E/W) NATURALLY locks the perpendicular axis
via that guard. Zero new math needed — 4 lines added to the switch.

3D-layer selection quad passes `drawMidHandles=false` (per-axis
basis math for 3D quads not wired yet).

### `7984e78` — Quick-win 4: adaptive text-stroke dilation
User complaint: "text stroke goes wild like its have tile effects
when its more than 12". Root cause: `ps_text_` did an 8-sample outline
dilation at a single ring radius sw. Arc length between adjacent taps
= sw * 2π/8 = sw * 0.785. At sw=15, gap ≈ 12 px — larger than typical
stroke thickness → 8 visible ghost stamps of the letter.

Fix: concentric-ring adaptive sampling:
- sw ≤ 8: 12 taps (8 outer + 4 inner)
- sw ≤ 20: 24 taps (8 outer + 8 outer-offset + 4 inner + optional mid)
- sw > 20: 42 taps (8 outer + 8 outer-offset + 8 mid + 4 inner)

Uses HLSL `[unroll]` on the loops. Extra rings gated behind runtime
`if (sw > 8.0)` / `if (sw > 16.0)` branches. This commit's CI run
originally cancelled due to runner queue timeout — re-verified green
on the next push.

### `607fe7e` — Task 5.13-fix5 + Quick-win 3: 2D gaussian shadow + AE bounding box
User complaint on Drop Shadow: "increased softness the shadow begins
to appears circles in a diagonal way". Same root pattern as text
stroke: 5-tap PERPENDICULAR line blur stamped 5 copies of source
silhouette diagonally past ~15 px softness.

Fix: 13-tap 2D Gaussian ring pattern inside the fused DropShadow
shader. 1 center tap (w=0.20) + 4 axis-aligned at 1σ (w=0.10 each) +
8 diagonal at 0.707σ (w=0.04 each) + 4 axis at 1.5σ tail (w=0.02
each). Weights sum to exactly 1.00, rotationally symmetric.

**Known follow-up:** user later confirmed still not truly blurry at
softness > 30. Real fix is proper separable 2-pass Gaussian (~1-2 hr
commit), parked.

Also in this commit: unified selection bounding box style. Old
cyan+yellow outline + corner squares replaced with black-shadow +
white-core sandwich matching the anchor crosshair (see next). New
`DrawSelectionBox` helper applied to all 3 draw sites (2D shape gizmos,
2D selection overlay, 3D projected quad).

### `b335263` — Quick-win 2: AE-style anchor crosshair
User feedback: "the anchor point is looking like cartoonish, the anchor
point is big needs to be very small tiny dot". Old: 12-px filled red
circle. New: crosshair marker via new `DrawAnchorMarker` helper — two
12-px thin lines with black-shadow + white-core sandwich, plus a
1.5-px core dot with matching outline for precise clicking. Drag
hit-test radius unchanged (~14 px).

Both draw sites (`DrawSelectionGizmos` and `DrawViewportCanvas`
overlay) share the helper.

### `9af3c11` — Task 5.13-fix4: fuse DropShadow into single-pass shader
**Real bug fix, not cosmetic.** User: "still taking over the shape
color and going black". Root cause: DropShadow's Pass 2 (composite)
had a same-texture SRV+RTV bind conflict. Trace:

- `ApplyChain` enters DropShadow branch with `readSRV=ping_srv_`,
  sets up `writeRTV=pong_rtv_`, `writeSRV=pong_srv_`,
  `otherRTV=ping_rtv_`, `otherSRV=ping_srv_`
- Pass 1 (offset+blur): reads `readSRV=ping`, writes `writeRTV=pong`.
  SAFE (different textures).
- Pass 2 (composite): reads `writeSRV=pong (t0)` AND `readSRV=ping
  (t1)`, writes to `otherRTV=ping_rtv_`.
- **`otherRTV=ping_rtv_` and `readSRV=ping_srv_` back the SAME
  TEXTURE.** D3D11 silently unbinds one → composite output is
  undefined/zero → `CompositeSRVOver` blits transparent over compRTV
  → layer appears as bg color / partially-tinted black.

Fix: fuse both passes into ONE shader. `kPSDropShadowFused` samples
source at current UV (the layer itself) AND at 5 taps around the
shifted UV (blurred shadow alpha), then composites source-over-shadow
inline. Only READS ping, only WRITES pong — no same-texture risk
possible.

`ApplyChain` DropShadow branch collapses from ~55 lines of custom
two-pass draw (with its own IASet/PSSet sequence that ALSO missed
the `rasterizer_none_` + DSS-reset binds from Task 5.13-fix2 —
implicit bug fix) to ~15 lines using standard `DrawFullscreenPass`.

Net −104 LOC / +79 LOC.

### `7a98acc` — Quick-win 1: Drop Shadow default params
Trivial factory helper param bump. D=5→20, S=3→8, O=0.6→0.75. Angle
already at 135° (AE default).

### `edef091` — Docs: session-2 EOD notes + feedback triage + header refresh
- `END_OF_DAY_NOTES_2026-07-24_SESSION2.md` — full log of Tasks
  5.7 → 5.13-fix2, effect parity vs AE table, lessons learned
- `TEST_FEEDBACK_2026-07-24_BOUNCING_BALL.md` — 17-item triage
  from live user test session (BUG/MISSING/UX/POLISH tags)
- `src/EffectManager.h` header refresh with load-bearing warning:
  "Never remove `rasterizer_none_` + `OMSetDepthStencilState(nullptr, 0)`
  from `DrawFullscreenPass` without proving no code path leaves
  CULL_BACK bound"

### `4610777` — Task 5.13-fix2: **the** Task 5.13 rescue
This was the smoking-gun session. Task 5.13's per-layer effect
isolation shipped in `862b305` seemingly fine. User tested: filtered
layers vanished with any effect enabled. Three "fix" attempts and
three diagnostic bisections failed:
- `a4abdca` blend-state REPLACE bind (wrong theory)
- `584f983` diag: skip ApplyChain — still vanished
- `5e042b2` diag: use `RenderLayers` in place of `RenderSingleLayer`
  — still vanished
- `03cc986` diag: hardcoded magenta clear + composite — still black

Real root cause (found by careful reading of ImGui backend defaults):
The composite fullscreen triangle vertices `(-1,3), (-1,-1), (3,-1)`
have CCW winding. DX11 defaults are `CullMode=CULL_BACK`,
`FrontCounterClockwise=FALSE` → CCW is back-facing → triangle
silently culled. ImGui's backend sets `CULL_NONE` at every
`RenderDrawData` call which is why Task 5.13 seemed to work in some
test runs, but pre-frame state depended on driver + init order.

Fix bundle (~15 LOC):
- `rasterizer_none_` state (CULL_NONE, DepthClipEnable=TRUE) created
  in `Initialize`, bound at top of `DrawFullscreenPass`
- `OMSetDepthStencilState(nullptr, 0)` in `DrawFullscreenPass` —
  removes inherited depth-test as a variable
- `ps_composite_` fallback to `ps_passthrough_` on compile failure
  (identical HLSL, guaranteed to exist)
- Added `cbuffer EffectCB` block to `kPSComposite` for signature
  parity with the fullscreen VS

Lesson learned: DX11 Release builds with zero debug output cost ~5x
more debugging time. Wiring `ID3D11InfoQueue` + `OutputDebugString`
is now backlog item #12 in the triage doc, specifically because of
this hunt.

---

## Architecture as of `ca1fcf0`

### Rendering pipeline

**`CompositionRenderer`** — DX11 shape rasterizer. Owns `ps_shape_sdf_`
(Iñigo Quílez rounded rect + fast ellipse, anti-aliased both edges
via fwidth), `ps_text_` (adaptive multi-ring outline for stroke,
Task 5.11-fix-4), `ps_null_`. Task 5.13 factored out `ClearComp` and
`RenderSingleLayer` helpers for the per-layer isolation path.
`RenderLayers` (the batch path) preserved for backward compat.

**`EffectManager`** — Exactly 2 ping-pong RTs at composition
resolution. Cost = 2 * W * H * 4 bytes ~= 16 MB @ 1080p regardless of
layer count. Shaders: `ps_passthrough_`, per-effect PS (MotionTile /
DirectionalMotionBlur / ChromaticAberration / BlendMode), plus Task
5.13 additions: `ps_composite_`, `ps_dropshadow_fused_` (post-5.13-fix4).
`ApplyChain` reworked in Task 5.13 with source-adjacent RT selection +
unconditional post-loop passthrough copy.

**`RenderEngine::DrawViewportCanvas`** dispatches per-layer:
- Layer without effects → `RenderSingleLayer(layer, compRTV, ...)`
  (fast batching path)
- Layer with effects → `ClearComp(pingRTV, transparent)` →
  `RenderSingleLayer(layer, pingRTV, ...)` → `ApplyChain(pingSRV,
  pongRTV, perLayer)` → `CompositeSRVOver(pongSRV, compRTV)` (alpha
  over-composite via `blend_normal_`)

Same dispatch runs in `PumpExportOneFrameIfActive` for MP4 export
via `CreateProcess + CreatePipe + WriteFile` to ffmpeg (Task 6.1).

### Text rendering (Task 5.9 + Quick-win 7)

DirectWrite grayscale AA (`DWRITE_RENDERING_MODE_NATURAL`,
clearTypeLevel=0) → R8_UNORM atlas. Custom `BitmapTextRenderer`
(IDWriteTextRenderer subclass) bridges `IDWriteTextLayout::Draw()` →
`IDWriteBitmapRenderTarget::DrawGlyphRun()` (the bitmap RT type
doesn't expose `DrawTextLayout` — CI caught that during Task 5.9).

Cache-once model: `TextRenderer::EnsureLayerCache(layer, device)`
hashes TextProps → compares against `Layer::textCacheKey` → skips
re-rasterize if match. Font-family fallback to "Segoe UI" if
requested font not installed; `TextProps.fontFamily` never overwritten.

Post-Quick-win 7: atlas is `2x` oversampled. `layer.textTexW/H`
holds the VISUAL dims (atlas dims / 2, ceiling divide).

### Timeline UI (Task 5.12)

Bottom dock = single Timeline panel, full width. `[Bars] [Graph]`
toggle + Shift+F3. Bottom dock state (mode + `bottomPaneSplitFrac`)
persisted to `imgui.ini` via a `SettingsHandler` under
`[PotatoBottomDock][State]`.

Bars mode = timeline strip with:
- Ruler + playhead
- Per-layer row: `[Vis 18px] [Name flex] [Parent combo 90px if room]`
  (Task 5.12b inlined these from a redundant table above)
- Full-row drag-to-reorder (Task 5.12b) using `IsMouseDown` lifetime
  (not `IsItemActive` which dies mid-drag when the vector reorder
  moves the widget's screen Y — Task 5.11-fix)
- Trim bars behind keyframe diamonds
- Keyframe diamonds for Position/Rotation/Scale/Opacity via
  `drawAndHitKeys` template lambda

**Big pending UX pain (see design doc below):** every animated
property's keyframes stack onto ONE row per layer. Overlapping
diamonds are unclickable. Users can't tell which property has what
keys. Design doc for AE-style twirl-down at the end of this file
addresses this.

### AnimatedProperty (Task 5.4-fix, AE-native)

`AnimatedProperty<T>` with `Keyframe<T>` holding `inSpeed/outSpeed`
(T-space units/sec), `inInfluence/outInfluence` (0..100%),
`incomingMode/outgoingMode` (5 InterpModes: Linear/Bezier/
ContinuousBezier/AutoBezier/Hold), `roving` bool.

`Evaluate()` dispatches:
- Hold → short-circuit
- Both-Linear → Lerp fast path
- Else → BuildBezierSegment + SolveBezierU (Newton-Raphson) +
  EvalBezierValueAtU

Speed graph shows sqrt(dx² + dy² [+ dz²])/dt. Value graph draws
X=red, Y=green, Z=blue overlaid.

### Undo model

`UndoStack` = deque<string> JSON snapshots (nlohmann). Task 5.6
sync-snapshot: `MarkForSnapshot()` pushes synchronously the first
call per frame (guarded by `currentFrameNumber == lastSnapshotFrame`).
Continuous drags coalesce. Structural mutations + atomic ops both
work. Layer selection persists in JSON via `outRoot["selectedLayerId"]`
(Task 5.6-fix-2).

### Coding rules (from PROJECT_BRIEFING.md §7)

- No `// TODO` — ship working stubs
- Zero heap alloc in the frame loop
- Defensive C++20 (all pointer / HRESULT / divide checks)
- `#define NOMINMAX` and `#define WIN32_LEAN_AND_MEAN` before every
  `<windows.h>`
- Stable `Layer.id` — NEVER use vector index for identity
- Left-handed coords everywhere (DX11 native)
- Vertical FOV in degrees
- Design-doc-first for large features; small bugfixes can skip

---

## Effect parity vs AE (verified by user visual test)

| Effect | Correct math? | Notes |
|---|---|---|
| Motion Tile | Yes | Mirror-edges mode matches AE. |
| Directional Motion Blur | Yes | Simpler than AE's temporal-sample blur; ours is fixed-angle box. Visually equivalent at small radii. Also currently STATIC — no per-frame parameter animation yet. |
| Chromatic Aberration | Yes | R/G/B channel offset math matches AE. Radial mode uses image-center distance. User verified: white→yellow-left/blue-right, red→red fringe only, cyan→green-left/blue-right, gray→faint rainbow, yellow→green fringe + red edge. |
| Blend Mode (Normal/Add/Mul/Screen/Overlay/ColorDodge) | Partial | Overlay + ColorDodge textbook. Additive still reads "translucent, not glow" — user's earlier report. Deferred to effects deep-dive. |
| Drop Shadow | Math OK, softness quality limited | Post 5.13-fix4 the shape stays visible + shadow renders. Softness > 30 still not truly gaussian-smooth. Fused single-pass shader avoids same-texture bind bug. |

**Not-yet-shipped effects** (backlog): Gaussian Blur (standalone),
Glow, Levels/Curves/Hue-Saturation, Track Matte, Adjustment Layer.

---

## Live backlog (from TEST_FEEDBACK triage)

**Quick wins done this cluster:** 7 of the 7 quick-wins bundle
(including Drop Shadow fused-shader bug fix along the way).

**Pending medium tasks:**
- Drop Shadow proper separable Gaussian (~1-2 hr)
- Split Layer tool (Ctrl+Shift+D style)
- Motion path overlay (trace anchor's animated path on canvas)
- Edge flicker during drag (needs repro)
- Speed graph handle hit-test with many keyframes (needs bouncing-ball
  repro from user's video)
- Per-shape AA modes (Crisp / Smooth / HighQuality)

**Big architectural, needs design doc:**
- **AE-style property twirl-down timeline** (design doc at end of
  this file — awaiting user sign-off on 5 review questions)
- Effect param animation (each `Effect.params.p*[i]` becomes
  `AnimatedProperty<float>`, ~1 day)
- Alight-Motion per-segment curves in Bars mode
- Full AE 2025 UI brief (Shy / Solo / Lock / Track Matte pickwhip /
  layer groups / pill bars / column show-hide bar)

**Infrastructure debt:**
- DX11 debug layer routing (`ID3D11InfoQueue` → file log) — would
  have saved 5 commits on the Task 5.13 hunt
- Undo memory bound
- Persistence of expanded-state in timeline (once twirl-down ships)

---

## Design doc for next commit

**Full text of `DESIGN_COMMIT17_AE_TIMELINE_TWIRL.md` below.** Not yet
committed to repo — awaiting user answers on the 5 review questions
at the end.

---

# Design doc — Commit 17: AE-style property twirl-down in the timeline

**Task ID:** 5.14 (proposed)
**Design-doc-first per workflow.** No code until approved.
**Estimated scope:** ~450 LOC across `RenderEngine.h`, `RenderEngine.cpp`, `imgui.ini` handler. One commit.

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

`src/Layer.h`:

```cpp
struct Layer {
    // ... existing fields ...

    // Task 5.14: which sub-sections are twirled open in the timeline
    // strip. In-memory only for now; not serialised. Bit flags so we
    // can add more categories cheaply (Masks, Text properties, etc.).
    enum ExpandFlag : uint32_t {
        Expand_Transform = 1 << 0,  // Position/Rotation/Scale/Anchor/Size/Opacity
        Expand_Effects   = 1 << 1,  // list of Effect entries
        Expand_Text      = 1 << 2,  // TextProps (only for Text layers)
    };
    mutable uint32_t timelineExpandMask = 0;
    // Per-effect-index expand bit (which effect entries are showing
    // their parameter list). Stored as a bitmask limited to 32 effects
    // per layer — matches the ApplyChain vector cap.
    mutable uint32_t effectExpandMask   = 0;
};
```

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

Row height stays at `rowH = 18.0f`. A layer with everything expanded
and 2 effects with 3 params each is `1 + 6 + 1 + 2 + 3 + 3 = 16` rows =
288 px. Fine for typical use; scroll-region wraps.

### Sub-row visual style

**Header row** (top of each layer group) — matches today's row EXACTLY:
- `[Vis 18px] [name 4px pad] [Parent combo 90px if room]`
- **NEW:** small twirl `>` / `v` icon at the very left, before Vis
  (12 px wide). Click to toggle `Expand_Transform` bit.
- **NEW:** to the right of the Parent combo, small `fx` chip. Click
  to toggle `Expand_Effects` bit. Only visible if the layer HAS
  effects (or hidden but always clickable if we want "add effect
  from timeline" — probably later).

**Transform sub-row** (Position, etc.):
- Indent left edge by 24 px (twirl + Vis indentation)
- `[Stopwatch 14px] [property name (Position, etc.)] [value text 80px]`
  where "value text" shows the CURRENT evaluated value like `(320.5,
  180.0)` in a smaller font, dim color — read-only in the strip; edit
  in Inspector.
- Track column: same X range as today (labelW → stripW - 6). Draws
  only THIS property's diamonds in THIS property's color.

**Effect header sub-row** (Drop Shadow, Chromatic, etc.):
- Indented 24 px from strip origin
- `[Twirl > 12px] [Enable checkbox 14px] [Effect name]`
- Small `X` at end to remove
- No keyframe track (the effect itself isn't a scalar)

**Effect parameter sub-row** (Distance, Angle, ...):
- Indented 48 px (twirl + effect-level indent)
- `[Stopwatch disabled-look 14px] [Param name] [value 80px]`
- Track column reserved but empty for now (until effect-param anim ships)

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

1. **Row layout pass:** walk layers in AE-order, produce a
   `std::vector<TimelineRow>` where
   ```cpp
   struct TimelineRow {
       int   layerId;
       enum Kind { LayerHeader, TransformProp, EffectHeader, EffectParam, TextProp } kind;
       int   subIndex;   // property index for TransformProp,
                          // effect index for EffectHeader/EffectParam,
                          // param index for EffectParam within its effect
       float y0;         // computed screen Y
   };
   ```
   Total rows = sum over layers of (1 + expanded sub-counts).
   Total height = rowCount * rowH + rulerH.

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

---

## Ready-check for user

**Please review this design and confirm before I ship.** Specifically:

1. **Row heights** — sub-rows same 18 px as header rows? Or thinner
   (14 px) to fit more on screen?
2. **Value readout in sub-rows** — should it be editable inline (small
   drag-float widget) or read-only text? Editable is closer to AE 2025
   but doubles the widget count per row.
3. **Twirl icon style** — filled triangle (`▶`/`▼`)? Text glyph (`>`/`v`)?
   Chevron (`>`/`⌄`)?
4. **What to auto-expand on select** — nothing (collapsed by default,
   user opens what they want)? Or auto-expand Transform when a layer is
   selected? AE auto-expands only what you explicitly clicked.
5. **`fx` chip visibility** — always draw it on the header row (even
   when the layer has no effects), or only when effects exist?

Once these 5 are locked, I ship one commit implementing this doc.
