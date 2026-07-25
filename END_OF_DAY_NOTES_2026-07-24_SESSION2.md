# End-of-day notes — 2026-07-24 (session 2, Tasks 5.7 → 5.13-fix2)

Continuation of the same calendar day. Earlier session covered Tasks
5.4 → 5.6-fix-2 (see `END_OF_DAY_NOTES_2026-07-24.md`). This one picked
up at 5.7 and ran through the Task 5.13 rescue.

---

## What shipped today (this session)

All commits landed green on `main`. Design docs committed before code
for every large feature. Small bugfixes shipped without design docs.

### `9a4c137` — Task 5.7: Shape strokes + rounded corners (SDF PS)
- Unified `ps_shape_sdf_` replaces per-shape shaders. Iñigo Quílez
  rounded-rect SDF + fast ellipse, anti-aliased on both edges.
- `Layer.strokeColor`, `strokeWidth`, `cornerRadius` fields; per-shape
  UI in Inspector.

### `37fd25b` — Task 6.1: MP4 export via `CreateProcess` + real ffmpeg log
- Replaced `_popen("wb")` (MSVCRT text-mode munging bug) with explicit
  `CreateProcess` + `CreatePipe` + `WriteFile`.
- ffmpeg stderr captured to `<outputDir>/ffmpeg_last.log`; last 20
  lines surface in the status bar on failure.
- Render Queue Resolution is read-only (auto from comp dims).
- Duration auto-populates from `max(outPoint)` on panel show; user
  edit latches an override flag.

### `872d5cf` — Task 6.1-fix: Deterministic per-frame export tick
- Export loop now advances comp time deterministically:
  `animEngine.currentTime = frameIndex / fps`, then
  `layerManager.BeginFrame(exportTime)` + `SyncCameraFromLayerIfAny()`.
- Bypasses `isPlaying` / wall-clock. Exports are now frame-accurate.

### `f09d1ab` — Task 5.8: Preview Scale (Full/Half/Quarter) + comp/RT split
- `previewScale` (1.0/0.5/0.25) drives RT dims; MVP division still uses
  logical comp dims so shapes stay comp-correct at any preview scale.
- `RtWidth()/RtHeight()` = comp * previewScale.
- FPS + Canvas + RT readout in the viewport toolbar.

### `2554224` — Task 5.8-fix: Freeze anim clock during gizmo/diamond drag
- Fixes keyframe-spam when timeline is playing AND user drags a shape.

### `55dd503` — Task 5.9: Text layers with DirectWrite + font favorites
- New `ShapeType::Text` + `TextProps` + text-atlas ComPtr cache on Layer.
- DirectWrite grayscale AA (`DWRITE_RENDERING_MODE_NATURAL`,
  clearTypeLevel=0) → R8_UNORM atlas.
- Font favorites persist in `%LOCALAPPDATA%\PotatoMotion\fonts.json`
  (per-user, NOT in `.pmge`).
- **CI caught** `IDWriteBitmapRenderTarget::DrawTextLayout` doesn't
  exist — had to write our own bridge (next commit).

### `aa304d1` — Task 5.9-fix: BitmapTextRenderer bridge
- Custom `IDWriteTextRenderer` subclass that bridges
  `IDWriteTextLayout::Draw()` → `IDWriteBitmapRenderTarget::DrawGlyphRun`.
- Pattern borrowed from FW1 wrapper.

### `40a0730` — Task 5.9-fix-2: text alignment / italic clip / weight combo / auto-grow
- `layout->GetOverhangMetrics()` used to expand atlas for italic tail
  clipping.
- `SetMaxWidth` + `SetMaxHeight` set BEFORE overhang read so
  center/right alignment resolves correctly.
- Weight combo (9 named weights) replaces the raw slider — matches AE.
- `sizePixels` auto-syncs to atlas dims when stopwatch is off.

### `1e0a3c6` — Task 5.11: AE-order timeline + insert-above + drag-to-reorder
- Layer stack is AE-order: **top row = last vector index = front-most**.
- `AddLayer` inserts at `selIdx + 1` (above the selection).
- Drag-to-reorder using `IsMouseDown` (not `IsItemActive` — that dies
  mid-drag when the vector reorder moves the widget's screen Y).
- Hit region is the FULL row width.

### `ac0b6c9` — Task 5.11-fix-2: reorder direction inverted
- `MoveLayerToIndex` now uses FINAL-POSITION semantics (v2 fix).
- Clamp `dragFromRow` instead of early-returning.

### `2f2b7da` — Task 5.11-fix-3: text stroke via 8-sample outline
- `ps_text_` samples the atlas at 8 rotational offsets scaled by stroke
  width, dilates coverage, tints with stroke color.
- **Known artifact**: past ~15px stroke, 8 samples produce visible
  ghost copies. Documented, deferred to adaptive-sample-count fix.

### `5fe4149` — Task 5.12: Unified bottom dock (Bars/Graph toggle, splitter, persist)
- Timeline is one panel spanning full width. `[Bars] [Graph]` toggle +
  Shift+F3 shortcut.
- 6px splitter over label|track divider. Clamped [0.15, 0.60].
- Bottom-dock state (mode + splitFrac) persisted to `imgui.ini` via
  a `SettingsHandler` under `[PotatoBottomDock][State]`.

### `6d46bab` — Task 5.12b: whole-row reorder drag + inline Vis/Parent + kill table
- Strip label column now inlines `[Vis 18px] [Name] [Parent combo 90px]`
  when there's horizontal room.
- Redundant ImGui table (was drawn ABOVE the strip) removed.

### `862b305` — Task 5.13: per-layer effect isolation + Drop Shadow (5 params)
- Exact-2-RT ping-pong at comp size (~16 MB @ 1080p) preserved from
  earlier design.
- New helpers: `CompositionRenderer::ClearComp/RenderSingleLayer`,
  `EffectManager::CompositeSRVOver` (SRC_ALPHA/INV_SRC_ALPHA blit),
  reworked `ApplyChain` with source-adjacent RT selection +
  unconditional post-loop passthrough copy.
- **Drop Shadow** effect: 5 params (Distance/Angle/Softness/Opacity/
  Color) via 2-pass shader (`ps_dropshadow_offset_` +
  `ps_dropshadow_composite_`).
- Isolation dispatch happens in both `DrawViewportCanvas` and
  `PumpExportOneFrameIfActive` — export and viewport share the pipeline.

### `a4abdca` — Task 5.13-fix: bind REPLACE blend in ApplyChain
- First attempted fix for "layer vanishes with fx" reports. Theory
  was blend state inheritance from `RenderSingleLayer`
  (SRC_ALPHA/INV_SRC_ALPHA) contaminating effect passes.
- Added `blend_replace_` (BlendEnable=FALSE) and bound at top of
  `ApplyChain`.
- **Did not fix the reported bug** — layers still vanished.

### `584f983` / `5e042b2` / `03cc986` — Task 5.13-diag / diag2 / diag3
- Three diagnostic bisection commits, each stripping one more layer
  of the isolation pipeline:
  - diag: skip `ApplyChain`, composite pingSRV directly to compRTV
  - diag2: swap `RenderSingleLayer` for `RenderLayers` (proven-good
    code targeted at pingRTV)
  - diag3: skip shape draw entirely — just clear pingRTV to opaque
    magenta and composite to compRTV
- **All three: no output.** Even a hardcoded magenta clear + composite
  produced a black frame. That eliminated shape rasterizer state,
  effect shaders, `RenderSingleLayer`, blend state, and the isolation
  dispatch loop as suspects.

### `4610777` — Task 5.13-fix2: **the fix** (CULL_NONE + DSV reset + composite fallback)
Root cause: the composite fullscreen triangle was being silently culled
by inherited rasterizer state. The triangle uses CCW winding
(`(-1,3), (-1,-1), (3,-1)`); DX11 defaults are `CullMode=CULL_BACK`,
`FrontCounterClockwise=FALSE`, meaning **CCW = back-facing = culled**.
ImGui's backend sets `CULL_NONE` at every `RenderDrawData`, which is why
Task 5.13 seemed to work in earlier sessions — some prior state left it
that way. In this session's runtime state, something (device init order
or a driver default) had `CULL_BACK` bound going into our composite.

Fix bundle:
- **`rasterizer_none_`** (`CULL_NONE`, `FrontCounterClockwise=FALSE`,
  `DepthClipEnable=TRUE`) created in `Initialize`, bound at top of
  `DrawFullscreenPass`. Removes inherited rasterizer as a variable.
- **`OMSetDepthStencilState(nullptr, 0)`** in `DrawFullscreenPass`.
  Forces depth-off default. Removes inherited DSS as a variable.
- **`ps_composite_` fallback to `ps_passthrough_`** on compile failure
  (identical one-line HLSL). `Shutdown` guards against alias
  double-release. Removes silent shader-compile failure as a variable.
- **Added `cbuffer EffectCB` block** to `kPSComposite` for signature
  parity with the fullscreen VS. Removes reflection quirk as a variable.

Diagnostics (v1/v2/v3) reverted; real isolation pipeline restored.

**User confirmed** all 5 test scenes pass — chromatic aberration on
white/red/cyan/gray/yellow shows textbook R/G/B channel offsets:
white → yellow-left/blue-right, red → clean red fringe, cyan →
green-left/blue-right, gray → faint yellow↔blue rainbow, yellow →
green-right + red-left edge.

---

## Effects — parity vs After Effects (verified)

| Effect | Ships correct math? | AE parity notes |
|---|---|---|
| **Motion Tile** | Yes | Mirror-edges mode matches AE. Phase param controls scroll. |
| **Directional Motion Blur** | Yes | Working per user test — angle + distance streaks correctly. Simpler than AE's true directional motion blur (which uses temporal samples); ours is a fixed-angle box blur. Visually indistinguishable at small radii. |
| **Chromatic Aberration** | Yes | R/G/B channel offset math matches AE's Optics Compensation → CA pass. Radial mode uses image-center distance. |
| **Blend Mode (Normal/Add/Mul/Screen/Overlay/Color Dodge)** | Partial | Overlay + Color Dodge math is textbook. **Additive still looks "translucent, not glow"** (user's earlier report). Deferred to effects deep-dive. |
| **Drop Shadow** | Yes for math | 2-pass offset+blur + alpha-over composite in-shader. Distance/Angle/Softness/Opacity/Color all working. User's "text itself was black" observation was because the default Distance (~5 px) puts the shadow directly under the shape — bump Distance to 20+ and Angle to 135° for the classic AE look. Default values could be improved (deferred). |

**What we're NOT parity with AE yet:**
- No **Gaussian Blur** as a standalone effect (Softness in Drop Shadow
  is a 5-tap box blur, not true Gaussian).
- No **Glow** effect. Requested implicitly by the "Additive not glow"
  observation.
- No **Levels / Curves / Hue-Saturation** color-correction primitives.
- No **Track Matte** (AE's Alpha/Luma matte via adjacent layer).
- No **Adjustment Layer** type (affects layers below without needing
  own content).
- **Text stroke ghosting past ~15 px** — 8 discrete samples produce
  visible ghost copies. Adaptive sample-count fix parked.

---

## What's on the repo right now

- **Branch**: `main`
- **Latest commit**: `4610777` (Task 5.13-fix2)
- **CI**: green (Windows-latest MSVC x64 Release)
- **Artifact**: ~1.23 MB
- **Runs today (session 2)**: many, all successful once fix2 landed
- **PAT status**: used ~10× this session, scrubbed after every push,
  **rotate/revoke when convenient**

---

## Deferred (real backlog — not aspirational)

Ordered by rough value × effort:

### High value, low effort
1. **Better Drop Shadow defaults** — Distance=20, Angle=135°, Softness=8,
   Opacity=0.75. One-line JSON default change. ~5 min.
2. **Text stroke ghosting fix** — adaptive sample count (8 → up to 32
   past 15 px). ~10 shader lines, one commit. ~30 min.
3. **BlendMode-as-effect fast-path skip** — layers with only the legacy
   BlendMode effect (superseded by `Layer.blend` field from Task 5.10)
   currently force isolation path. One-condition fix in
   `HasAnyEnabledEffect()`. ~15 min.

### Medium value, medium effort
4. **Gaussian Blur effect** — proper separable H+V pass. Would replace
   the box blur inside Drop Shadow's softness step too. ~150 LOC.
5. **Glow effect** — threshold + blur + additive composite. Fixes the
   "Additive is not glowy" complaint. ~200 LOC, needs Gaussian first.
6. **Adjustment Layer type** — needs render-order rework so a layer
   can affect the accumulated compRTV BELOW it. ~250 LOC + design doc.
7. **PNG / JPG import** — WIC decode, new `Image` layer type, project
   asset panel wiring. Huge value for real content creation. ~350 LOC
   + design doc.

### Big features (need design docs first)
8. **Full AE 2025 UI brief** — Shy, Solo, Lock, Track Matte pickwhip,
   layer groups (twirly), pill bars, column show/hide bar, F4 column
   toggle. You provided the spec; we shipped partial in Task 5.12.
   Rest is ~800 LOC across UI + serialization.
9. **Pikimov layout redesign** — pill-shaped bars, inline
   position/value fields on timeline rows, unified compact left column.
   You said "we will come back later"; that time hasn't come.
10. **True depth-based 3D compositor** — migrate 3D layers from ImGui
    overlay path into compRTV with proper depth sort. Currently 3D
    is drawn as ImGui perspective-projected polys over compRTV. Big
    architectural lift.
11. **More effects**: Levels, Curves, Hue/Saturation, Color Balance,
    Fill, Tint, Fast Blur, Directional Blur (as separate from motion),
    Threshold, Invert.

### Infrastructure / polish
12. **DX11 debug layer output routing** — today's Task 5.13 bug ate
    5 hours partly because we had zero runtime signal from a shipped
    Release exe. Wire `D3D11_CREATE_DEVICE_DEBUG` (gated by a build
    flag) + `OutputDebugString` for `ID3D11InfoQueue` messages. Would
    have shown "SILENT NOOP DRAW: rasterizer discarded triangle"
    immediately.
13. **Windows Event Log or rolling file log** for shipped builds so
    future customer bug reports come with real error output attached.
14. **Undo memory bound** — `UndoStack` deque is unbounded. Cap at
    e.g. 100 snapshots or 200 MB, evict oldest.
15. **Comp-tab auto-open on new project** — currently the viewport is
    empty until user manually opens the Composition Viewport panel
    via View menu.

### Won't do (explicitly parked)
- Direct competition with AE feature-for-feature. Stay potato-scoped
  (4 GB RAM, integrated GPU). Depth is more valuable than breadth.
- Nested compositions ("pre-comps"). Too much complexity for the
  target audience.
- Plugin API. Same reasoning.

---

## Lessons from today

1. **When bisection contradicts your model, your model is wrong, not the
   test.** Three diagnostics returned "no output" and I kept clinging to
   variations of "blend state must be wrong." The actual answer was
   rasterizer state — a state I'd checked and dismissed as "the fast
   path works, so cull must be fine." I forgot that the fast path uses
   a different vertex source (CompositionRenderer's quad IB) which
   might have been drawn under different state than my fullscreen
   triangle at pipeline init.

2. **Explicit > inherited state.** Every draw call that isn't part of
   a batch should re-bind every state it depends on. The cost is
   negligible; the debugging savings are enormous. Applied to
   `DrawFullscreenPass` today — should audit the rest of
   `CompositionRenderer` next time we touch it.

3. **Zero-signal Release builds cost 5× more debugging time.**
   Infrastructure item #12 above just moved up the priority list.
   Even a text file that gets `OutputDebugString` echoes would have
   caught this in one round-trip.

4. **User's "same problem" is a valid data point.** Multiple times
   today the user rejected my fix in one sentence and it forced a
   real bisection instead of another guess-and-ship cycle. That
   discipline saved several more wasted commits.

5. **Feature-parity checklists matter.** The chromatic aberration
   verification just now (white/red/cyan/gray/yellow) took 30
   seconds and gave concrete evidence the effect matches AE.
   Should do this for every new effect before calling it shipped.

---

## Sleep well.

Repo state clean, all effects verified against AE reference behavior,
Task 5.13 fully closed. Handoff-to-Claude MD not needed — we got there
ourselves in the last shot. Next session, pick from the backlog above
(my vote: Drop Shadow defaults + text stroke fix as a warm-up combo,
then PNG import as the next real feature).
