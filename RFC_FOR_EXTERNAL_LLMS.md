# RFC — How Would You Take Potato Motion Editor Toward AE Parity?

> **You are being asked for a strategic architecture proposal, not a code patch.**
> Read this document, look at the repo, and answer the questions at the end. Concrete plans win over vague ones. Opinions with tradeoffs win over opinions without.

---

## 1. What this repository is

- **Repo:** https://github.com/cosmicrages0/potato-motion-editor
- **What it is:** a native Windows x64 motion graphics editor, ~1.05 MB binary, targeting **potato-class hardware** (dual-core CPU, 4 GB RAM, integrated GPU, DirectX 9/10/11 feature-level fallback).
- **Stack:** C++20, MSVC on `windows-latest` GitHub Actions, DirectX 11 (with WARP software-rasterizer fallback), SDL2 for windowing/input, Dear ImGui (`docking` branch) for UI, FFmpeg via `_popen` for export.
- **Goal:** be as close to Adobe After Effects and Alight Motion as a 1 MB binary running on a 2015 laptop can plausibly get.

**Before answering, please read (in this order):**
1. `SOFTWARE_SPEC.md` — the original technical spec that seeded the project
2. `PROJECT_BRIEFING.md` — the living architectural summary; every task's design decisions are documented here
3. `LAYOUT_MAP.md` — the intended UI layout and per-panel widget list
4. `END_OF_DAY_NOTES.md` — the previous agent's honest post-mortem of Tasks 1-6, including regrets

Skimming these takes ~15 minutes. Skipping them will produce a bad proposal.

---

## 2. Current state — what works, what's broken, what's missing

### 2.1 What is actually shipped and working (verify by pulling the artifact from the latest GitHub Actions run)

| Subsystem | Status |
|---|---|
| SDL2 window, DX11 device (with WARP fallback), ImGui docking, 5-panel AE-style layout | ✅ Works |
| `Layer` + `LayerManager` with stable IDs, parent/child hierarchy, cycle-safe reparenting, per-frame world-matrix memoization | ✅ Works |
| 2D bounding-box gizmos (4 corner scale, 1 move) with rotation-aware hit-testing via inverse affine | ✅ Works, math is stable but not linear-feeling |
| Full 3D camera pipeline (LookAt + free-look, vertical FOV, orbit/pan/zoom) with `ShapeType::Camera` layer that drives the view | ✅ Works |
| Runtime HLSL compiler + 4 pixel shaders (Motion Tile, Directional Motion Blur, Chromatic Aberration, Blend Modes) with defensive passthrough fallback | ✅ Works |
| Fixed 1920×1080 composition RT with letterbox display via `ImGui::AddImage` | ✅ Works |
| Effect chain applied to composition (all enabled effects across all layers combine) | ✅ Works |
| FFmpeg direct-stream MP4 export via `_popen("wb")`, 1-frame-in-RAM guarantee (~33 MB @ 4K) | ✅ Works IF ffmpeg is installed |
| AE-style stopwatch keyframes (click stopwatch → any value change auto-keys at playhead time) | ✅ Works, linear interpolation only |
| Alight Motion XML curve importer | ✅ Works |
| Null Object layer type | ✅ Works |
| Camera style toggle (AE mode vs Alight mode with `stickToCamera` HUD attachment) | ✅ Works |
| Slingshot Bezier easing engine (unclamped Y for overshoot) | ✅ Works, but only drives one global curve |

### 2.2 What's known-broken or missing

**Bugs the user has hit and reported:**
- Gizmo scale math is non-linear near the anchor point (tiny mouse moves cause huge scale jumps)
- Timeline strip keyframe diamonds show but can't be dragged, right-clicked, or deleted
- Inspector labels used to clip; fixed by tabs, but tabs are still cramped on narrow screens
- FFmpeg "encoder died" if ffmpeg.exe isn't in PATH (mitigated by a new Test FFmpeg button, but the error UX during a failed export is still bad)
- Effect chain is composition-wide, not per-layer isolated
- 3D layer selection doesn't have on-canvas transform handles (Inspector-only editing)

**Features that don't exist yet at all:**
- **Undo/redo.** `Ctrl+Z` does nothing. Every user mistake is permanent.
- **Save/load.** Closing the app loses everything. No `.pmge` file format exists.
- **Text layers.** No DirectWrite integration.
- **Shape stroke, rounded corners.** Fill only.
- **Sub-compositions / pre-comp / nesting.**
- **Masks, track mattes.**
- **Right-click context menus anywhere** (Timeline rows, canvas, keyframe diamonds).
- **Snapping** to layer edges / composition center / other layers.
- **Duplicate layer** (Ctrl+D).
- **Composition Settings modal** (Cmd/Ctrl-K equivalent). Composition size is hardcoded to 1920×1080 in code.
- **Bezier easing per keyframe.** Everything interpolates linearly; the global slingshot curve is decorative for the graph editor and doesn't drive real per-keyframe easing.
- **Anchor point drag tool** (AE's Y key).
- **Layer solo/lock icons in Timeline** (data exists in `Layer` struct, no UI).
- **Motion tracking.**
- **Expressions / property linking.**
- **Real DPI awareness.** SDL flag is set but rendering doesn't scale.
- **Reset Layout menu item.** A broken `imgui.ini` currently traps the user.
- **Adjustment layers.**
- **Text/shape rendering beyond rectangles and ellipses** (Custom Path is a stub).

### 2.3 Architectural regrets from the previous session (do NOT repeat these mistakes)

1. **The global comp clock in `AnimationEngine` is co-owned by `Layer::SampleTracks`** — reading `animEngine.currentTime` from Layer is a coupling smell. Comp time should be a parameter, not global state.
2. **`Effect` uses a fixed 64-byte float4 struct** — expedient but wrong for text color, gradients, mask feathering. A variant/any type would scale better.
3. **`Layer::SampleTracks(t)` mutates `transform` in place** — gizmo hit-test after sampling sees animated values, not authored values. Should keep "authored" and "evaluated" separate.
4. **Two matrix types (`Mat3` for 2D, `Mat4` for 3D)** — duplicated helpers, bug surface × 2.
5. **`ShapeType::Camera` is a special case in `SyncCameraFromLayerIfAny`** — polymorphism-lite hack. Would be cleaner with a proper layer-behavior trait.
6. **Effect chain is composition-wide, not per-layer.** Documented as intentional for CapCut-model simplicity but users may expect AE-model per-layer effects.

### 2.4 Hard constraints (non-negotiable)

- **Runs on a 2015-era dual-core laptop with 4 GB RAM and Intel HD Graphics.** No hard DX11-only features, no CUDA, no NVIDIA-only extensions.
- **Zero heap allocations in the frame render loop.** All buffers allocated in `Initialize()` or on structural mutation only.
- **Binary size < 5 MB.** Currently 1.05 MB. Any proposal that requires a large dependency (Qt, JUCE, WebView2, Skia) is DOA unless justified.
- **Single-file architecture per module.** `Foo.h` + `Foo.cpp`, no sprawling headers, no template-only libraries in the hot path.
- **Windows x64 only.** No cross-platform proposals please — the whole rendering stack is DX11.
- **Must survive `<windows.h>` include** — every new file that includes it needs `NOMINMAX` and `WIN32_LEAN_AND_MEAN` defined first.

---

## 3. The strategic question we need external input on

There are **~15 remaining features** between where we are now and something that could reasonably be called "After Effects on a potato PC." We know how to implement each of them in isolation. What we DON'T know is:

**In what order should we implement them, grouped into what phases, following what architectural spine?**

Three plausible directions we've considered:

### Direction A: "Foundations first" (stable-boring)
1. Undo/redo command pattern (2 days)
2. Save/load `.pmge` JSON (2 days)
3. Composition Settings modal + Reset Layout menu (1 day)
4. Right-click context menus everywhere (1 day)
5. Real timeline: expandable per-property tracks, draggable keyframes (3 days)
6. Bezier easing per keyframe (2 days)
7. Then features: text, sub-comps, masks, tracking, expressions

**Pro:** app becomes a real editor before it becomes a feature-rich toy. Users can build up a project without losing it.
**Con:** takes ~2 weeks before there's any visible "wow" feature to demo.

### Direction B: "Feature demos first" (marketing-driven)
1. Text layers via DirectWrite (3 days)
2. Real per-layer effect passes (2 days)
3. Shape strokes + rounded corners (1 day)
4. More shader effects: glow, drop shadow, gradient, ramp (4 days)
5. Motion tracking point-tracker (7 days)
6. Then foundations: undo, save/load, timeline overhaul

**Pro:** every commit is a demo-able "look what it can do now!"
**Con:** users will build real projects, hit an unrecoverable bug, and lose everything because there's no save/load. Reputationally risky.

### Direction C: "Fix what's broken, then Foundation" (defensive)
1. Rewrite gizmo scale math (non-linear near anchor bug)
2. Draggable keyframe diamonds in the timeline
3. Per-layer isolated effect passes
4. Composition Settings modal
5. Then Direction A phase 1

**Pro:** stops the bleeding on user-facing bugs before adding surface area.
**Con:** slowest of the three; nothing "new" for many commits.

**None of these may be right.** We want you to propose your own direction, name it, and defend it.

---

## 4. Questions we need you to answer

Answer each with a short, specific reply. Vague answers ("just do good engineering") are useless.

### 4.1 Direction and phasing
**Q1.** Do you prefer Direction A, B, or C above — or propose your own Direction D? Give the ordered task list, grouped into 2-4 phases, with a rough time estimate per phase.

**Q2.** Of the ~15 missing features listed in Section 2.2, which **three** would you build FIRST regardless of direction? Why those three specifically?

**Q3.** Which of the ~15 missing features would you deprioritize the LONGEST, or drop entirely? Why?

### 4.2 Architecture calls that are hard to reverse later
**Q4.** **Undo/redo:** command pattern with per-op undo, or full-state snapshot stack? Justify against the "4 GB RAM potato PC" constraint.

**Q5.** **Save/load format:** hand-rolled JSON, RapidJSON, nlohmann/json, `msgpack`, `protobuf`, or something else? What are the tradeoffs for a 1 MB binary?

**Q6.** **Per-property keyframe storage:** the current `std::optional<PropertyTrack>` on 4 fields of `Layer` works but doesn't scale to per-effect params. Do you keep it or introduce a generic `AnimatedProperty<T>` template?

**Q7.** **Per-layer effect passes:** requires each layer to render into its own transient RT before composite. On a potato PC with 30 layers × 1080p that's ~250 MB VRAM if naive. What's your allocation strategy? (Pool? Atlas? On-demand alloc-and-free per frame?)

**Q8.** **Text rendering:** DirectWrite directly, `stb_truetype`, or ImGui's built-in font atlas? Which handles right-to-left, ligatures, and per-character animation best on the potato budget?

**Q9.** **Threading:** the export loop currently blocks the main thread and pumps ImGui between frames. Is that acceptable long-term, or do we need real threading (deferred contexts, worker pool, message queue)? If real threading, what's the sync architecture?

### 4.3 UX / product calls
**Q10.** AE vs Alight vs CapCut — which mental model should this app COMMIT to as its primary identity? (We currently try to serve both AE and Alight; some argue this makes it worse at both.)

**Q11.** The user has repeatedly said "I can't test this properly." What TWO changes to the app itself would most improve testability for a solo user working alone at a laptop, no external QA?

**Q12.** Is right-click context menu everywhere worth prioritizing, or is it a nice-to-have?

**Q13.** Should we ship a bundled `ffmpeg.exe` inside the project so users don't hit the "encoder died" trap, accepting the binary bloat (~80 MB)? Or stay lean and rely on user installation with the current Test FFmpeg button?

### 4.4 Meta questions
**Q14.** What's ONE thing in the current codebase you would refactor before adding any new feature? Not "make it prettier" — an actual architectural change with a payoff.

**Q15.** What would you WARN us against doing? What is the trap we're likely to fall into if we don't hear it now?

**Q16.** If you had to name the ONE feature that would make an artist go "OK this is real software now" — what would it be? Justify.

---

## 5. Response format we want

Please respond in this structure so the human can compare your proposal side-by-side with 2-3 other LLMs' proposals:

```
## Direction (name it)
One paragraph.

## Ordered phase list
Phase 1 (X days): tasks
Phase 2 (Y days): tasks
Phase 3 (Z days): tasks

## Answers to Q1-Q16
Q1. ...
Q2. ...
...
Q16. ...

## The one thing I disagree most with the previous agent on
One paragraph.

## Concrete first commit I would ship
File(s) I'd touch, what I'd change, why. No code snippets — just the plan.
```

---

## 6. Things NOT to do in your response

- **Don't propose rewriting from scratch.** The user has ~30 committed hours in this codebase. A rewrite proposal will be ignored.
- **Don't propose adding Qt / JUCE / any GUI framework.** ImGui is locked in.
- **Don't propose making it cross-platform.** Windows-only is a hard constraint.
- **Don't propose adding a scripting language / node graph / WebAssembly.** Not now, maybe never.
- **Don't recommend "professional QA process" or "hire testers."** Solo dev, no budget, no team.
- **Don't say "it depends" without stating what it depends on and picking a default.**
- **Don't write any code.** This is an RFC, not a PR. Prose only.
- **Don't be diplomatic.** The user explicitly wants opinionated answers so they can pick between differing strategies. Blend-in-the-middle proposals waste the exercise.

---

## 7. Who is asking

A solo indie developer, working in short sessions, on a Windows PC, with a limited-battery laptop, using CLI LLM agents for the actual coding. They have shipped Tasks 1-6 in one long session, hit the wall on "I can't animate," realized the previous agent (me) was too reactive, and now wants to hear 2-3 external opinions before committing to a direction for the remaining weeks of work.

Their frustration signal: **"lets stop implementing one by one."** They are tired of tactical fixes and want strategic clarity.

If your answer is helpful, they will pick a direction and hand your document to the next coding agent as the plan. If your answer is generic AI mush, they will move on.

---

## 8. Deadline

None formally. But every day this document sits unanswered is a day the user isn't building. Prefer a good answer today over a perfect one next week.

---

## 9. Addendum: Real user test findings (added after commit `b818be1`)

Between the original RFC and now, the user ran a 47-test checklist against the shipped build. Here's the ground truth so your proposal isn't answering the wrong questions.

### 9.1 What ACTUALLY works end-to-end (verified by real testing, not by CI green)

- Window maximize, dock layout, 5-panel view ✅
- All shape-add buttons (Rect / Ellipse / Null; Camera hidden by default now) ✅
- Layer selection, delete, parent dropdown with cycle-detection greyed-out ✅
- Bounding box appears around selected 2D layer with 4 corner + 1 center handles ✅
- Inspector tabs `[Transform] [Effect Controls] [Global]` don't overlap or leak ✅
- **AE-style stopwatch keyframe workflow WORKS end-to-end** — user quote: *"animation is working no doubt, i can set keyframe and forward in timeline and then change the value the second key appears, it overall good for now"* ✅
- Timeline strip scrub, keyframe diamonds display ✅
- imgui.ini persistence across restarts ✅

### 9.2 What LOOKED like it worked in code but DOESN'T on screen

- **Effects visually apply to shapes** ⚠️ User quote: *"no effects gets apply on the shape i mean it shows the parameter even i changed, nothing is applied on shape"* — the Effect Controls panel accepts input, the [FX ON] HUD appears, but the actual shape pixels don't visibly change. Strongly suspect RTV/SRV aliasing: `EffectManager::ApplyChain(compSRV, compRTV, ...)` reads from `compSRV` while `compRTV` is the output; D3D will silently unbind the SRV when its underlying texture is bound as an RTV, so the chain reads from a null SRV and writes garbage / no-op back into compTexture. Needs an intermediate ping-pong texture or a defensive copy before the chain runs.

- **Graph Editor is decorative** ⚠️ User quote: *"the animation is control by the graph editor the curve its feels like a showpiece"* — the Slingshot Bezier P1/P2 handles are draggable and the curve renders and the playhead moves along it, but no keyframe interpolation actually uses that curve. All `PropertyTrack::Evaluate` does linear interpolation. The graph editor is disconnected from every real animation.

- **FFmpeg export** ⚠️ User has explicitly deprioritized this. Reads "encoder died". Almost certainly ffmpeg not in PATH. Test FFmpeg button was moved to top of Render Queue as "Step 1 (click me FIRST)" but user hasn't yet installed ffmpeg to verify the underlying pipe.

### 9.3 What was actually broken in the code (root cause + user-visible symptom)

Fixed in commits leading up to `b818be1`:
- **Matrix transpose bug** in `CompositionRenderer::BuildShapeMVP`. HLSL float4x4 defaults to column-major storage; C++ was writing row-major. Result: dragging the position value moved the shape in the wrong direction while the CPU-drawn bounding box moved correctly. User's exact description: *"the shape is watching the box"*.
- **Stopwatch ID collision** in ImGui. All four Transform-tab stopwatch buttons rendered with the same label (`"( )"` or `"(*)"`), so ImGui hashed them to the same ID, triggered debug assertion popup on hover, and could route clicks to the wrong track. Fixed in the same commit as this RFC update.
- **Tooltip crash** — separate fix, replaced BeginTooltip/EndTooltip pair with SetTooltip single-call.

### 9.4 Two specific hard problems the user wants outside help on

**Problem A — RTV/SRV aliasing in the effect chain.**
The current architecture:
```
CompositionRenderer::RenderLayers(compRTV, ...)    // fills compTexture
effectManager.ApplyChain(compSRV, compRTV, ...)    // reads compSRV, writes compRTV
```
`compSRV` and `compRTV` are two views of the SAME `compTexture`. D3D11 will not let a resource be bound simultaneously as both an SRV and an RTV. The chain silently gets a null SRV in slot 0 for its first pass and produces a black or no-op output that overwrites `compTexture`. Ping-pong buffers exist inside `EffectManager` but they only ping-pong between EACH OTHER, not between the source and them.

**Question A:** What's the cleanest fix that keeps the "1080p ping-pong" VRAM budget?
Candidate approaches:
- **A1:** Blit compTexture to `effectManager.ping_tex_` first, run chain from ping to compRTV
- **A2:** Add a third "composite" texture on the RenderEngine side and always double-buffer
- **A3:** Redesign the chain to always start reading from an internal texture and end writing to the caller's RTV

Which do you recommend, given the "one comp render into a texture, then filter it in-place" flow that Task 5.0 committed to?

**Problem B — Graph editor disconnected from real animation.**
Right now `AnimationEngine::currentCurve` (the slingshot Bezier) drives NOTHING except the graph editor's own preview line. `PropertyTrack::Evaluate` does linear interp between keyframes.

**Question B:** What's the minimum change to make the graph curve MEAN something without a huge rewrite? Options:
- **B1:** Global default easing — treat `AnimationEngine::currentCurve` as the easing applied to EVERY keyframe segment. 1-hour change, not AE-accurate.
- **B2:** Per-keyframe Bezier — every `Keyframe` stores its own `outHandle` and `inHandle` (like AE). Graph editor shows/edits the curve between the currently-selected keyframe and the next. ~1-day change, AE-accurate.
- **B3:** Per-property default easing — each `PropertyTrack` stores one Bezier that applies to all its segments. Middle-ground.

Which is the right first step and why?

### 9.5 What the user is NOT asking

Explicitly deprioritized:
- Export polish (comes back after ffmpeg install verified)
- 3D features (hidden in UI via View menu; user is "2D first")
- Anything that requires a UI rewrite

### 9.6 One-paragraph gut-check request

The user is a solo dev, one long session in, has just discovered that visible fidelity on the canvas is the difference between "impressive tech demo" and "usable tool." They can author animation now (huge) but effects don't visibly render (bad) and the graph editor is a lie (medium). If your original RFC answer would change based on these test findings, please revise it. If not, please explicitly say your original answer stands.

---

*Repo: https://github.com/cosmicrages0/potato-motion-editor*
*Latest commit at time of writing: `b818be1` (Task 5.0-b, plus the ID-collision fix in the commit that adds this addendum)*
*This RFC lives at `/RFC_FOR_EXTERNAL_LLMS.md` in the repo root.*
*Section 9 addendum added after user's 47-test checklist run.*
