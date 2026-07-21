# End of Day Notes — Potato Motion Graphics Editor

**Date:** 2026-07-22
**Session tag:** Tasks 1 → 6 shipped (skeleton complete)
**Final commit on `main`:** `8f04c9b`
**Final artifact:** `MotionGraphicsEditor-x64-Windows` — 1.05 MB

> This is an **opinion doc**, not a plan. Everything below is my honest take on where we are, what worries me, and what I'd want to see. Any of it can be argued with. We'll turn the parts you agree with into a real plan tomorrow.

---

## 1. What we actually shipped today (fast summary)

| # | Task | What landed | Ship state |
|---|---|---|---|
| 1 | Bootstrap | SDL2 window, DX11 device (with WARP + feature-level fallback), ImGui docking, AE-style 5-panel first-run layout | ✅ Works |
| 2 | Slingshot Bezier | `AnimationEngine` + `BezierCurve` with intentionally unclamped Y for overshoot, interactive Graph Editor with draggable P1/P2 handles, live playhead | ✅ Works |
| 3 | Layer hierarchy | `Layer` + `LayerManager` with stable-ID parenting, cycle detection, per-frame world-matrix cache, 2D bounding-box gizmos (4 corner scale + 1 move), Timeline row table, Inspector Transform section | ✅ Works, ⚠️ gizmo math is bad |
| 4 | 3D camera | `Mat4` + LookAtLH + PerspectiveFovLH, `Camera` class with LookAt & free-look modes, `ShapeType::Camera` layer that drives the view, RMB-orbit / MMB-pan / wheel-zoom, camera icon + look-at line gizmo | ✅ Works |
| 4.5 | Polish + docs | Maximized window, shape-spawn helper, `Null` layer type, AE/Alight camera style toggle, real keyframe primitives (`PropertyTrack`), timeline strip with playhead + colored diamonds, `PROJECT_BRIEFING.md`, `LAYOUT_MAP.md` | ⚠️ Look-only for keyframes — see below |
| 5 | HLSL effect stack | `Effect` POD + `EffectManager` with runtime `D3DCompile`, 4 shaders (Motion Tile / Motion Blur / Chroma Aberration / Blend Modes), 2 ping-pong RTs (not per-layer), Effects Palette + Effect Controls panels, `[fx]` badge on Timeline | ⚠️ Data-complete but effects don't visually apply yet |
| 6 | FFmpeg export + Alight XML | `ExportEngine` with `_popen` binary-mode pipe, DX11 staging readback (row-by-row `fwrite`), Render Queue panel with progress bar + ETA + Cancel, `AlightXmlImporter` (no XML lib dependency), File menu import | ⚠️ Pipeline complete but exports a solid color gradient, not the actual scene |

The **skeleton is complete.** Every module in the original spec exists, compiles, and runs on x64 Windows.

---

## 2. My honest views (unfiltered)

### 2.1 What went genuinely well
- **The stable-ID architecture in `LayerManager`** was the single best decision. It means Task 7 keyframes, Task 5.0 effect wiring, and any future undo/redo can all reference layers by `id` without caring about vector reordering. If we had used indices we'd be having a bad time already.
- **The `NOMINMAX` lesson from Task 3** made every subsequent Windows-header commit green on first try. That kind of "learn once, prevent forever" is exactly what the briefing doc is for.
- **The defensive shader-compilation fallback in Task 5.** A broken HLSL edit currently prints a `[EffectManager] PS compile failed:` line and drops to passthrough. The editor NEVER crashes on a shader. That's a design habit that pays off constantly.
- **The `_popen("wb")` binary-mode catch in Task 6.** This bug is silent (produces garbage MP4s), catchable only by watching output byte-by-byte, and would have wasted a full day of debugging if I'd shipped it with text mode.
- **CI-driven development.** 8 pushes, 7 green, 1 red (fixed in 4 minutes). No local Windows environment needed. `windows-latest` is our test bench.

### 2.2 What I'm most uncomfortable about

**#1 — The screenshot review changed my opinion of the whole build.**
Your screenshot showed a scale field reading 0.976 while a big orange circle was on screen, keyframe diamonds nowhere to be seen, and label text clipped to gibberish. I had been telling you "Task 4.5 shipped" while actually shipping "Task 4.5 exists but does nothing useful from a user's chair." I still find that hard to sit with. The lesson: **never call a task "done" until you have a screenshot of it working from the user's POV**, not just a green CI build. A green build only proves the compiler doesn't hate you. It doesn't prove the software is any good.

**#2 — Task 5 and Task 6 are not-actually-shipped without Task 5.0.**
Both scope caveats are documented, but let's not pretend. Right now:
- Task 5's Effects Palette shows real effects, but the shapes on screen aren't filtered.
- Task 6's Render Queue exports a real MP4, but its content is a solid color, not your scene.

They are both **"pipe wired, faucet not connected."** The user-facing value comes from Task 5.0 hooking them up. From your seat, Tasks 5 and 6 are essentially IOUs until then.

**#3 — The Composition Viewport is not a canvas, it's just a `ImDrawList`.**
This is the root cause of many bugs at once:
- Shapes not centered → panel-pixel is world-pixel
- Scale gizmo math is non-linear near anchor → because there's no proper world↔screen transform, we compute deltas in the wrong space
- No zoom/pan of the composition → same reason
- Effects don't apply → because ImGui draws to swap chain, we can't intercept
- Export shows wrong content → same reason

Fixing the canvas properly (rendering the scene into a texture, drawing that texture inside the viewport panel) fixes 5-6 things at once. This is the highest-leverage single change we could make.

**#4 — The keyframe UI I shipped in 4.5 doesn't teach itself.**
The K diamond button is small, subtly colored, and the "click it" workflow is not discoverable. Real AE users right-click a stopwatch icon. Even Alight Motion has a big pulsing keyframe button. Ours is a 12-pixel square that lights up slightly when there's data. A new user has ~0% chance of finding it.

**#5 — I keep making the Inspector do too many jobs.**
It shows: layer name, Transform, Fill, Composition Clock (global!), Slingshot Bezier (global!), Camera Properties (global-ish!). That's five different "levels" of state crammed into one scroll. AE keeps these strictly separated (Effect Controls window is separate; Composition Settings is a modal; Global preferences are elsewhere). We're going to hurt users if we don't split this up.

### 2.3 Small things that bug me
- **Timeline strip label column** is 140px hardcoded. Truncates any name longer than ~14 chars.
- **HUD text in the viewport** overlaps the composition guide rectangle at small panel sizes.
- **Slingshot toggle** in the Timeline is a great debug feature but doesn't belong in the main UI once real per-property keyframes work.
- **No visual feedback when adding an effect.** You click "Motion Tile" in the palette, the [fx] badge appears in the Timeline row — but nothing else changes. Should probably auto-open the Effect Controls panel and scroll to the new effect.
- **Camera menu items don't say which mode is active** except by an inconspicuous checkmark. AE uses a bold indicator.
- **`import.xml` hardcoded path** for Alight import is placeholder-ish. Should be a real file picker.
- **No status bar at the bottom.** AE has one; it's underrated for showing "current tool, cursor position in comp coords, current selection".

---

## 3. What we need to do to look like AE (opinion, not plan)

Ordered by "how much closer to AE this single change gets us" not by effort.

### 3.1 The five changes that would move the needle the most
1. **Fixed centered composition canvas rendered into a texture.**
   This unlocks: shape centering, scale gizmo correctness, per-layer effects, real-scene export, canvas zoom/pan, and a proper coordinate system. **One change, five features unblocked.** By far the highest ROI.
2. **Tabbed Inspector: Transform / Effect Controls / Masks / Expressions.**
   AE's key insight is that different property groups belong on different tabs. We're currently a single vertical scroll. Splitting the panel eliminates 90% of "wait, where do I find X" confusion.
3. **Right-click stopwatch = toggle keyframes for property.**
   The AE mental model. Enable the stopwatch → any change to that property auto-creates a keyframe at the current time. Disable it → all keyframes for that property are removed. Way more discoverable than our K button.
4. **Timeline: proper track lanes with expandable properties.**
   Right now each layer is one row. In AE, click the ▶ next to a layer to expand its properties (Position, Rotation, Scale, Opacity + any Effects) into their own sub-rows, each with its own keyframe track. This is where the whole app comes to life for animators.
5. **Composition settings modal.**
   AE's `Cmd/Ctrl-K`. Sets resolution, duration, frame rate, background color for the whole comp. Currently these are scattered (`compositionWidth` on the engine, `duration` on the animEngine, no BG color anywhere).

### 3.2 Medium-impact adds
- **Layer solo / lock icons in Timeline** (the data exists in `Layer`, no UI)
- **`[3D]` badge like `[fx]` badge** for 3D layers (partially exists as a checkbox)
- **Numeric readouts during gizmo drag** ("+42px, +8px" floating near cursor)
- **Snap-to-composition-center by default** when dragging shapes
- **Layer color labels** — small colored square next to layer name, artist-picked, for quick visual grouping
- **`Ctrl+D` = duplicate selected layer** (was in the Task 3 briefing, never implemented)
- **Right-click a layer in Timeline** → context menu (Rename, Duplicate, Delete, Precompose, Break Parenting)
- **Anchor point drag tool (Y key)** so users can move the anchor separately from position
- **Global "Enable Motion Blur" toggle** in Composition Settings — cheap once directional blur shader exists
- **"Reset Layout" menu item** so users can escape a broken `imgui.ini`
- **Save/load `.pmge` JSON files** so work survives closing the app
- **Ctrl+Z / Ctrl+Y undo/redo** — command pattern on top of a `LayerManager` snapshot stack

### 3.3 Big adds (each is a milestone in itself)
- **Text layers** via DirectWrite — the single most-missed feature for real motion graphics work
- **Shape strokes and rounded rectangle corners** — trivial data-wise, high visual impact
- **Sub-compositions / pre-comp** — one `Layer` whose content is another `LayerManager`
- **Real masks and track mattes** — needs the render-to-texture pipeline first, then a vector path editor
- **Motion tracking (point tracker)** — this is the AE feature that makes people gasp
- **Basic expressions** — `pick whip` between properties, then `wiggle(2, 30)` — closest thing to magic in AE

### 3.4 Things I'd say "no" to on a potato PC
- **Real GPU 3D with shadows and lights.** Our fake-3D projected quads are correct for the target hardware. Going real 3D means a depth buffer, shadow maps, and dropping DX9 fallback — trades away too much.
- **Live 4K preview.** The proxy pipeline exists for a reason. Editing at 720p and exporting at 4K is the correct compromise.
- **Roto brush / content-aware fill.** These are AI-adjacent, GPU-hungry, and not what a potato editor should promise.
- **Cinema 4D Lite integration.** Not our lane.

---

## 4. Architectural regrets and things I'd redesign

1. **The global comp clock in `AnimationEngine` should have been a `CompositionClock` singleton owned by `RenderEngine`, not co-owned by `Layer::SampleTracks`.** Right now the "one Bezier drives selected scale" hack and the real `PropertyTrack` sampling both read `animEngine.currentTime`, which means comp time and easing curve are conflated. Task 7 will need to untangle this.
2. **`Effect` should have a `parameters` variant (or a small std::any-ish thing), not fixed float4s.** The fixed 64-byte block works for the 4 effects we ship, but text-color effects, gradient ramps, and mask feathering will each want different storage. I chose expedience.
3. **The `Layer::SampleTracks(t)` pattern mutates `transform` in place**, which means anything that reads `transform.position` after `SampleTracks` gets the sampled value, not the authored value. That's fine for rendering but confusing for gizmo hit-testing during scrub. AE keeps "authored" and "evaluated" values separate. We should too.
4. **Two separate `Mat3` and `Mat4` types** with duplicated helpers. If I did it again I'd have just one `Mat4` used for everything, with the 2D gizmo path using a lifted-to-4x4 version. Two types = two places to fix any bug.
5. **The `ShapeType::Camera` layer is a special case in `SyncCameraFromLayerIfAny`.** Should be a proper polymorphic "layer behavior" — but polymorphism in C++ with value-typed layers is awkward. Might be worth revisiting when Text/Path layers arrive.

---

## 5. What I'd want to see next if I were you

If I were sitting in your chair, my priority order (again, opinion, not plan):

**Absolute first: Task 5.0 Usability Pass.** Nothing else matters until:
- Shapes appear where you click, at the right size
- The composition canvas is a fixed rectangle centered in its panel
- Effects visually apply to shapes
- Export produces MP4 files of your actual scene, not color gradients

Because right now the app is a nice technical demo but you can't make anything with it.

**Second: Text layers + save/load.** These two combined turn the app from "toy" into "you can start a project on Monday and open it on Tuesday and add a title." Everything else — motion tracking, expressions, sub-comps — is on top of a working save/load foundation.

**Third: Real per-property keyframes with Bezier easing per keyframe (Task 7).** Once you can `Ctrl+Z` (which requires save/load's serialization work anyway) and once you can add text, real animation authoring becomes tractable.

**Then everything else in whatever order feels most fun to you.** The order past that is a matter of taste and what you want to be able to demo next.

---

## 6. What I'd want from Gemini / any next agent

- Read `PROJECT_BRIEFING.md` end-to-end first. Especially Sections 7 (coding rules) and 12 (history of failed builds — the two mistakes we've already made and don't need to make again).
- Read `LAYOUT_MAP.md`. Especially Section 3, the composition viewport root-cause diagram.
- Take a screenshot **before** claiming any UI-visible change is done. If you can't take one, ask the user to.
- Never introduce a `// TODO`. Ship a working stub with documented behavior instead.
- If you introduce a new `#include <windows.h>`, put `NOMINMAX` and `WIN32_LEAN_AND_MEAN` above it. Every time. No exceptions.

---

## 7. One-paragraph gut check

We built a functional skeleton of a native Windows motion graphics editor with 3D camera, HLSL shader pipeline, and FFmpeg export in one session, at a 1 MB binary size, running on integrated GPUs — that's genuinely rare. But we also over-claimed usability at Task 4.5 and shipped two "wired but not connected" milestones at 5 and 6. **The next session's job is to turn the skeleton into a body: fixed canvas, effects that filter, exports that export your scene, and a UI that stops truncating labels.** After that we can start adding organs (text, save/load, real keyframe UX). It's a 1-2 session job before the app becomes something you can actually use, and probably 5-8 more sessions before it looks unmistakably AE-like. Very doable.

---

*Not a plan. An opinion doc. Argue with any of it and we'll build the real plan together tomorrow.*
