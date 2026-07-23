# End-of-day notes — 2026-07-24

## What shipped today (chronological)

All commits landed green on `main`. Every one had a design doc committed
first per workflow, most had a pre-go review pass.

### `05bfb40` — Task 5.4: first-pass Graph Editor + per-key Bezier
- `AnimatedProperty<T>` gained tangent fields (`inTangent`, `outTangent`,
  `incomingMode`, `outgoingMode`)
- New `EvaluateBezierSegment` with Newton-Raphson time-inversion
- Graph Editor rewritten: mode toggle, property picker, draggable
  tangent handles, right-click menu (Linear/Bezier/Hold/Delete)
- Serialization: optional `it/ot/iv/ov/im/om` fields per keyframe
- **User feedback:** all tests passed BUT this used Lottie-style raw
  (time_offset, value_offset) tangents, not AE-native (speed, influence)

### `5510244` — Task 5.4-fix: AE-accurate rework
Massive rework based on the "how AE actually works" spec you dropped:
- Storage now stores `inSpeed`/`outSpeed` (T-space units/sec) +
  `inInfluence`/`outInfluence` (0..100%) per side
- **Five interp modes**: Linear / Bezier / **ContinuousBezier** /
  **AutoBezier** / Hold
- Value graph draws X=red, Y=green, Z=blue simultaneously (AE default)
- Speed graph is ONE magnitude curve = sqrt(dx² + dy² [+ dz²])/dt
- Handle drag: free 2D default, Shift = influence-only, Alt = speed-only
- ContinuousBezier mirrors handles on drag; AutoBezier recomputes
  tangents from neighbor slopes each frame and locks handle drag
- Legacy 5.4 `.pmge` files auto-convert on load

### `2a79dcc` — Task 5.4-fix-2: Speed graph editable + UX fixes
- Speed graph handles now draggable per your fix spec
- Auto-expand Y bounds when handle is pulled past the current panel edge
  (fixes "handle goes out of dock")
- Top-left "value / speed" label moved inside plot area (fixes overlap)
- Removed "F9" from menu label — your laptop's F-lock was intercepting
  it as a screen-lock key. Menu-only now.

### `91cd59d` — Task 5.4-fix-3: Speed graph hit-testing
- Bug: keys were unclickable in Speed mode. Root cause: hit-test always
  used value-space Y, but Speed-mode keys are drawn at the sampled
  speed curve height. Dispatched by mode. Larger hit radius (12→18 px)
  + white ring in Speed mode so handles at speed=0 (sitting on the X
  axis) are still grabbable.

### `79171ea` — Task 5.6: Composition Settings modal + Reset Layout
- `Composition → Settings...` (was a stub) now opens a modal:
  resolution (W/H + preset combo), duration, frame rate (24/30/60 +
  custom-value preservation), background color picker
- `View → Reset Layout` recovers the default AE-style dock
- **Snapshot timing bug fixed (retroactive)** — MarkForSnapshot now
  pushes synchronously the first time it's called per frame via a
  frame-counter guard. Old model deferred to next frame, capturing
  post-mutation state → Ctrl+Z was a no-op for atomic ops like Delete
  Keyframe, Set to Bezier. This fixes them all in Task 5.4 too.

### `6e60be5` — Task 5.6-fix: Delete key routes to keyframes
- When the playhead is snapped to a highlighted diamond, hitting Delete
  now removes just that keyframe (across all 6 animated Transform
  properties), not the layer. Falls back to layer-delete when no keys
  are near the playhead. Time epsilon adapts to comp duration.

### `9a4c137` — Task 5.7: Shape strokes + rounded corners
- Every shape gains `strokeColor` + `strokeWidth`; rectangles gain
  `cornerRadius`. Consolidated `ps_rect_` + `ps_ellipse_` into a
  single SDF-based pixel shader with fill + stroke + rounded corners
  in one draw call.
- Both edges anti-aliased via smoothstep (no jagged silhouette at low
  zoom). Corner radius clamped both in the UI slider and in-shader
  (belt + braces).
- Inspector: new `Stroke` + `Corners` headers. Backward-compat: old
  `.pmge` files default to no stroke + sharp corners.

### `37fd25b` — Task 6.1: Export rewrite — CreateProcess + real ffmpeg log
Fixed the "encoder died?" ghost:
- Replaced `_popen("wb")` with `CreateProcess` + `CreatePipe` +
  `WriteFile`. Guaranteed binary-clean (no MSVCRT text-mode munging).
- ffmpeg's stderr now captured to `<output_dir>/ffmpeg_last.log`. On
  failure, `status_.errorMsg` includes the last 2 KB of the log so
  users see the real error, not the generic "encoder died?" line.
- Render Queue Width/Height fields removed — export now always
  matches Composition Settings. Kills the portrait-comp vs
  landscape-preset mismatch bug that was likely the pipe killer.

### `872d5cf` — Task 6.1-fix: Export animations
- Bug: rectangle didn't animate in the exported MP4. Root cause: pump
  never advanced `animEngine.currentTime`; every frame was identical.
- Fixed: comp time now derived deterministically from
  `frameIndex / fps` and forced into `animEngine.currentTime` +
  `layerManager.BeginFrame(exportTime)` before each render. Frame-
  accurate regardless of isPlaying or wall-clock speed.
- Bonus: export pump now honors the user's bgColor (was still using
  the hard-coded pre-5.6 dark gray).

### `1c5edd6` — Task 5.6-fix-2: Undo preserves layer selection
- Bug: pressing Ctrl+Z on the top layer (Ellipse) reset selection to
  the bottom layer (Rectangle).
- Root cause: `selectedLayerId` was never serialized, so every undo
  snapshot lost it; on restore, `Clear()` set it to -1 and the front-
  layer fallback grabbed layer[0].
- Fixed: `AppStateToJson` writes `selectedLayerId` at root level;
  `JsonToAppState` restores it before the fallback fires, with a
  `GetLayerById` existence guard for undo-of-creation edge cases.
- Backward-compat: missing field on old `.pmge` files → front-layer
  fallback still fires.

## What still bugs me (todo tomorrow)

**Preview vs Composition resolution split (Task 5.8).** Called out in
Task 5.7's reviewer note. When you lower comp resolution the shapes
don't visually scale in the viewport correctly — gizmo/hit-test
coordinates go out of sync with the shrunken comp RT. Proper fix:
independent Preview Scale (Full/Half/Quarter) that keeps comp
dimensions locked but downsamples the RT.

**Ellipse stroke on extreme aspect ratios.** Fast SDF approximation
gives slightly variable stroke width around perimeter of e.g. a
100×20 oval. Iterative ellipse SDF is a ~10-LOC follow-up.

**Timeline FPS-driven tick spacing.** Deferred from Task 5.6 design.
Right now ruler uses fixed 0.1s intervals; should snap to frames at
1/fps intervals so 30 fps and 60 fps comps look distinct.

**Snap-to-frame scrub.** Also deferred from 5.6. Playhead drag should
snap to nearest frame boundary when comp fps > 0.

## Bigger themes shipped

1. **Snapshot timing model was subtly wrong.** The atomic-op undo bug
   in 5.6 was a latent problem going back to Task 5.3 that never
   surfaced because early atomic ops (right-click Delete Keyframe)
   were rare test paths. The Comp Settings modal made it obvious.
   Retroactively fixed for everyone.

2. **AE-accurate storage matters.** First pass at graph editor
   (Lottie-style tangents) shipped and tested green — then you
   pointed out it wasn't how AE actually works. Second pass
   completely reworked the storage, added 3 new interp modes, and
   made the animations feel professional. Lesson: "tests pass" ≠
   "correct model." Domain expertise catches things test suites miss.

3. **Real ffmpeg error surface is worth the CreateProcess complexity.**
   Two hours of "why is the pipe closing?" evaporated once we could
   read what ffmpeg was actually saying. The log-file path is
   permanent infrastructure — every future export failure will be
   diagnosable in seconds.

## Sleep well. Repo state:
- Branch: `main`
- Last commit: `1c5edd6` (Task 5.6-fix-2)
- Artifact: ~1.16 MB
- All CI runs green today
- Design docs added: 5 (COMMIT6-11)
- PAT status: used many times today, scrubbed after each push,
  **please revoke it in GitHub Settings when you get up**
