# Potato Motion Graphics Editor — Full Project Briefing

> **Audience for this document:** any CLI LLM agent (Claude, Gemini, GPT, Grok, etc.) picking up work on the repo. Read this end-to-end **before writing any code**. It is the source of truth for what has been built, what is broken, what AE / Alight Motion do that we don't yet do, and what should come next.

---

## 1. Project Identity

| Field | Value |
|---|---|
| **Repository** | https://github.com/cosmicrages0/potato-motion-editor.git |
| **Branch** | `main` (single-branch workflow, no PRs) |
| **Language** | C++20, MSVC on `windows-latest` GitHub Actions |
| **Target OS** | Windows x64 only |
| **Windowing** | SDL2 (fetched via CMake FetchContent, tag `release-2.28.5`) |
| **UI** | Dear ImGui, `docking` branch (fetched via CMake FetchContent) |
| **Renderer** | Direct3D 11 with feature-level fallback to 10.1 / 10.0 / 9.3, and a WARP software-rasterizer safety net |
| **Math** | Custom header-only Vec2/Vec3/Vec4/Mat3/Mat4 in `src/MathTypes.h`. **Left-handed** coordinate system (matches D3D native) |
| **Build** | CMake 3.20+, glob `src/*.cpp` `src/*.h`. GitHub Actions workflow `.github/workflows/build.yml` produces `MotionGraphicsEditor-x64-Windows` artifact (~1 MB) |
| **Design constraint** | Must run on dual-core / 4 GB RAM / integrated-GPU "potato" machines. Zero heap allocations inside the frame render loop; all buffers pre-allocated in init |

---

## 2. Source Tree (as of end of Task 4)

```
potato-motion-editor/
├── CMakeLists.txt              # SDL2 + ImGui docking FetchContent, WIN32 exe
├── SOFTWARE_SPEC.md            # Original spec (Task 1-6 outline)
├── PROJECT_BRIEFING.md         # <-- this file
├── .github/workflows/build.yml # MSVC x64 Release, uploads .exe artifact
└── src/
    ├── main.cpp                # Entry: creates RenderEngine, runs main loop
    ├── MathTypes.h             # Vec2/3/4, Mat3, Mat4 (LH, vertical FOV)
    ├── AnimationEngine.h/.cpp  # Global comp clock + slingshot Bezier curve
    ├── Layer.h                 # Transform (Vec3 pos/rot/scale), Layer struct
    ├── LayerManager.h/.cpp     # Stable-id vector, cycle-safe parenting,
    │                           #   2D and 3D world-matrix caches
    ├── Camera.h/.cpp           # Single-node 3D camera (LookAt + free-look)
    ├── RenderEngine.h/.cpp     # Everything else: DX11 init, ImGui panels,
    │                           #   viewport render, gizmos, camera controls
    └── (Task 5 will add) EffectManager.h/.cpp, Shaders.hlsl
    └── (Task 6 will add) ExportEngine.h/.cpp
```

---

## 3. What We Have Built — Task by Task

### ✅ Task 1 — Bootstrapping (done)
- SDL2 window + DX11 device + ImGui docking dockspace
- `docking` branch of ImGui, viewport enable flags
- Menu bar (File/Edit/Composition/Layer/Effect/Export — items are stubs except Layer)
- After-Effects-style 5-panel first-run dock layout: **Project Assets** (left) · **Composition Viewport** (center) · **Inspector & Effects** (right) · **Timeline** (bottom-left) · **Graph Editor** (bottom-right)

### ✅ Task 2 — Slingshot Bezier Engine (done)
- `BezierCurve` in `AnimationEngine.h` implementing `B(t) = (1-t)^3·P0 + 3(1-t)^2·t·P1 + 3(1-t)·t^2·P2 + t^3·P3`
- **P1.y and P2.y are intentionally unclamped** — the entire point is to allow overshoot > 100 % and rebound < 0 %
- Interactive graph editor panel: drag P1/P2 handles, live playhead
- One global `AnimationEngine` = one global comp clock (Play/Pause/Reset/Loop/Duration)

### ✅ Task 3 — Layer Hierarchy & 2D Gizmos (done)
- `Transform` uses `Vec3` position / rotation / scale (Z-ready even in 2D)
- `Mat3` for 2D affine with closed-form `InverseAffine()` for rotation-aware hit-testing
- `LayerManager`:
  - Stores layers by value; **`parentId` refers to `Layer.id` not vector index** (delete-safe)
  - `idToIndex` hash cache rebuilt on structural mutations only
  - Per-frame world-matrix memoization (`frameMatrixCache`)
  - `WouldCreateCycle()` walks the ancestor chain with a `visited` set + depth cap
  - Deleting a parent **orphans** children (does not cascade — matches AE)
- Timeline table: visibility · 3D flag · name · type · parent dropdown (cycles grayed out)
- Inspector: Position/Rotation/Scale Vec3, anchor, size, opacity, color
- Viewport: green bounding box + 4 yellow corner scale handles + red center move handle; click empty area to select layer under mouse
- **Correct matrix order** `world = parent * local` (column-vector convention)
- Delete/Backspace keyboard shortcut

### ✅ Task 4 — 3D Camera & Perspective Pipeline (done)
- `Mat4` with `LookAtLH`, `PerspectiveFovLH` (vertical FOV), defensive clamps on every input
- `Camera` class: `position`, `target`, `rotation`, `up`, `fov`, `nearZ`, `farZ`, `useTargetMode`
- **Dual camera model:**
  1. Always-present global `Camera` (fallback)
  2. Optional `ShapeType::Camera` layer whose transform drives the global camera every frame in `SyncCameraFromLayerIfAny()`
- Two-pass viewport render (Alight-style, not AE's run-based):
  1. All 2D layers in timeline order
  2. All 3D layers depth-sorted by view-space Z, far → near
- Behind-camera clip: any 3D quad with `w ≤ 0` on any vertex is skipped this frame
- Camera controls over the viewport: **RMB = orbit**, **MMB = pan**, **wheel = dolly**
- Camera icon + look-at line drawn when Camera layer selected
- Inspector "Camera Properties" section with Reset button

---

## 4. Honest State of the App (as of Task 4)

**I have overstated the usability in past updates. Here is the truth:**

### What actually works well
- Compiles and runs on Windows x64
- Docking layout persists between runs
- You *can* add layers, parent them, animate the slingshot curve, and see it drive the selected layer's scale
- Camera orbit/pan/zoom feels responsive

### What is broken or missing that stops it being a real editor

| Bug / Gap | User impact | Priority |
|---|---|---|
| **Window doesn't fit the screen on launch** | Hard-coded 1600 × 900, no DPI scaling, no "maximize on start" | 🔴 Critical |
| **New shapes spawn at world (640, 360) not the visible canvas center** | On a small viewport panel the shape spawns off-screen and looks like nothing happened | 🔴 Critical |
| **Scale gizmo doesn't feel right** | Uniform corner-scale, no aspect-lock, no mid-edge handles, no numeric feedback during drag; `Size (px)` and `Scale` are two separate concepts confused into one | 🔴 Critical |
| **No keyframe UI** | The slingshot Bezier is a *global* comp clock applied to the *selected* layer's scale only. You cannot set a keyframe on `Position at t=0.5s` yet. `PropertyTrack` is a stub | 🔴 Critical |
| **Timeline strip is missing** | The "Timeline" panel is just a layer list. There is no time ruler, no playhead you can drag, no keyframe diamonds, no per-layer track bars showing in/out points | 🔴 Critical |
| **Camera can't be parented to a Null/other layer, and layers can't be parented TO the camera** | The parenting dropdown lets you pick a Camera as parent syntactically but there is no "Null Object" layer type yet, so real Alight/AE camera rigs are impossible | 🟡 High |
| **No Null Object layer type** | AE-style parenting rigs need an invisible transform-only layer to parent to | 🟡 High |
| **No undo/redo** | Ctrl+Z does nothing. One misclick wipes state | 🟡 High |
| **No save/load** | Closing the app loses everything | 🟡 High |
| **Rotation gizmo missing on canvas** | You can only type rotation numerically in the Inspector | 🟡 High |
| **No mid-edge handles for non-uniform scale** | Only 4 corners exist | 🟠 Medium |
| **3D layers have no on-canvas transform handles** | Must edit numerically in Inspector | 🟠 Medium |
| **No snap-to-grid, no rulers, no guides** | Precise composition is guesswork | 🟠 Medium |
| **No text layer** | Can't add typography | 🟠 Medium |
| **No image / video import** | Can't use real media | 🟠 Medium (blocker for real use, but scheduled for Task 6) |
| **Slingshot curve is the ONLY easing** | Real editors have Linear / Ease In / Ease Out / Custom per-keyframe | 🟠 Medium |
| **CustomPath shape type is a stub** | Draws a placeholder circle | 🟢 Low |
| **3D layer click-to-select doesn't work** | 2D layers only; 3D layers can only be selected via the Timeline | 🟢 Low |
| **No layer duplicate (Ctrl+D)** | Announced in Task 3 briefing but never implemented | 🟢 Low |
| **No masking / mattes / track mattes** | Would need to be scheduled | 🟢 Low |
| **HUD text overlaps the composition guide rectangle at small viewport sizes** | Cosmetic | 🟢 Low |

---

## 5. What After Effects Has That We Don't

Grouped so it's clear what is achievable on a potato PC and what would need real engineering.

### 5.1 Achievable on potato hardware (should end up on our roadmap)
- **Null Object layers** — invisible transform-only parents (1-day task)
- **Adjustment layers** — apply effects to all layers below (~2 days once shaders land)
- **Text layers** — even basic system-font text via `DirectWrite` (~3 days)
- **Shape layer strokes and fills** — separate stroke width, stroke color (~1 day)
- **Rounded rectangle corner radius** (~1 hour)
- **Per-property keyframes** — the `PropertyTrack` stub in `Layer.h` was left there for exactly this. Would replace the current "one global clock drives selected scale" hack (~3 days)
- **Keyframe interpolation modes** — Linear / Bezier / Hold, per keyframe (~1 day on top of tracks)
- **Timeline strip with playhead, time ruler, keyframe diamonds, in/out points** (~3 days)
- **Undo/redo** — command pattern with a stack (~2 days)
- **Project save/load** — JSON serialization of the layer tree + curves (~2 days)
- **Snapping** — to composition center, layer edges, other layers (~1 day)
- **Solo, Lock, Shy** flags on layers — data is already there in `Layer`, just no UI (~2 hours)
- **Track mattes** — Alpha / Luma matte from layer above (~2 days with shaders)
- **Motion blur global toggle** (~1 day once directional blur shader exists)
- **Global Effects palette / Effect Controls panel** — right now Effects menu items are stubs (~2 days)
- **Sub-composition (pre-comp / nesting)** — a Layer whose "content" is another LayerManager (~4 days)
- **Basic mask paths** — vector shapes used as clipping paths (~4 days)
- **Anchor point tool (Y key in AE)** — drag anchor separately from position (~1 day)

### 5.2 Achievable but expensive
- **Expressions / scripting** — a JavaScript-lite interpreter for property-linking (~2 weeks minimum)
- **Puppet warp / mesh deformation** (~2 weeks)
- **3D shadows and lights** (~1 week; needs proper 3D rasterizer, not our polyline hack)
- **Roto brush / rotoscoping** (~1 month)
- **Content-aware fill** (definitely not for a potato PC)
- **Motion tracking** (~2 weeks for point tracking; face/planar tracking is a full product)

### 5.3 Not achievable on potato hardware and we should not attempt
- **Real GPU-accelerated 3D layers with proper depth buffer, shadow maps, and lights** (we render 3D via projected polylines, not real rasterization)
- **Cinema 4D Lite integration**
- **Live real-time preview at 4K** (we can only *export* 4K per the design)

### 5.4 Roadmap after user's screenshot review (locked ordering)

The user has explicitly chosen skeleton-first ordering. Do NOT reorder these without asking.

| # | Milestone | Rough effort | Why it matters |
|---|---|---|---|
| **5** | **HLSL Pixel Shader Stack** (motion tile, motion blur, chroma aberration, blend modes) | 4-5 days | Adds the visual "wow" — the reason a motion graphics editor exists |
| **6** | **FFmpeg Proxy + 4K Export Engine** | 4-5 days | The point at which the app can produce a deliverable file |
| **5.0** | **Deferred Usability Pass** — fixed centered composition canvas at 1920×1080, tabbed Inspector, wider timeline labels, corner-scale gizmo rewrite, rotation gizmo, Reset Layout menu, proper DPI. See Section 9.5 for the full list | 4 days | Turns the skeleton into something the user can actually operate |
| **7** | **Bezier easing per keyframe + curve editor tied to real tracks** (right now keys are linear-only, and the graph editor drives only the global demo Bezier) | 3 days | Real animation quality |
| **8** | **Null Object layers + camera rig UX** — already partially done in 4.5 but needs to be re-verified after 5.0 fixes layout | 1 day | Confirms parenting rigs work end-to-end |
| **9** | **Undo/redo + project save/load (JSON `.pmge` file)** | 4 days | Without these it's not a real tool |
| **10** | **Text layers + shape strokes + rounded corners** | 4 days | The minimum content variety needed for a real motion graphic |
| **11** | **Sub-compositions (pre-comp / nesting)** | 4 days | Lets users build complex scenes from smaller pieces |
| **12** | **Masks + track mattes** | 4 days | Essential for compositing |
| **13** | **Snapping, rulers, guides, safe zones** | 2 days | Precision authoring |
| **14** | **Motion tracking (point tracker)** | 10 days | The most-requested advanced feature |
| **15** | **Basic expressions (link property A to property B via math)** | 10-14 days | The AE feature that separates "toy" from "pro" |

---

## 6. Alight Motion vs After Effects — Which Are We?

We are **both**, and the user should be able to pick per-project.

| Behavior | AE style | Alight style | Our current default |
|---|---|---|---|
| 2D vs 3D layer ordering | Runs of 3D layers sorted by depth, 2D layers act as opaque barriers | All 2D layers under all 3D layers; 3D sorted by depth | **Alight** (simpler) |
| Camera parenting | Camera is a Layer; can parent to any Null | Camera is a fixed rig; layers attach TO camera | **AE-style syntactic parenting works but no Null layers yet** |
| Comp clock | Per-comp, per-layer keyframes on every property | Global scene time, per-layer keyframes | **Global comp clock only** (single-Bezier hack) |
| Timeline | Full track strip with keyframe diamonds | Simplified strip | **Neither yet** |
| 3D layers | Real 3D with shadows/lights | Faux-3D projected quads | **Faux-3D projected quads** (correct for potato PC) |

**Decision going forward:** wherever it's cheap, give the user a **toggle** in the composition settings for "AE mode" vs "Alight mode". Where it's expensive, ship one and document the other as future work.

---

## 7. Strict Coding Rules (must be honored by any agent)

1. **No `// TODO` or partial code.** If a feature isn't finished, ship a working stub whose behavior is documented, not a syntactic placeholder.
2. **Single-file per module.** `Foo.h` + `Foo.cpp`, class definition inside the header. No sprawling headers.
3. **Zero-allocation in the frame loop.** All GPU buffers, ImGui strings, vectors sized in `Initialize()` or on structural mutation only. `thread_local` scratch vectors are OK if cleared not shrunk.
4. **Defensive C++20.**
   - Every pointer check: `if (!ptr) return;`
   - Every division has a `> epsilon` guard
   - Every DirectX call is `HRESULT`-checked
   - Every user input to camera / projection math is clamped to a safe range
5. **`#define NOMINMAX` and `#define WIN32_LEAN_AND_MEAN` BEFORE `#include <windows.h>`.** MSVC will otherwise define `min`/`max` as macros and break `std::clamp`.
6. **Stable IDs, never vector indices.** `Layer.id` is the only cross-frame handle. Selection, parenting, and effect-attachments all use `id`.
7. **Left-handed coordinate system everywhere.** Matches D3D native; saves a Z flip at Task 6 export time.
8. **Vertical FOV, degrees, in every camera formula.** Never horizontal, never radians in a public interface.
9. **After any structural mutation** (add/delete/reorder/reparent layer) — rebuild the id→index cache and clear per-frame matrix caches.

---

## 8. Build / CI Workflow

The workflow at `.github/workflows/build.yml` runs on every push to `main`:
1. `actions/checkout@v4`
2. `ilammy/msvc-dev-cmd@v1` with `arch: x64`
3. `cmake -B build -S . -DCMAKE_BUILD_TYPE=Release`
4. `cmake --build build --config Release --parallel`
5. Copy every `*.exe` and `*.dll` from `build/` to `release_pack/`
6. Upload `release_pack/` as artifact `MotionGraphicsEditor-x64-Windows`

**Push directly to `main`.** The CI is the smoke test. If it goes red, the fix is immediate — do not leave `main` broken.

**When it goes red:** without repo admin credentials the log is not directly readable via public API. Rely on: (a) the failed step name from the `actions/jobs/{id}` endpoint (it lists steps with pass/fail per step), and (b) careful re-reading of the diff for the classic MSVC gotchas: `NOMINMAX`, `<windows.h>` collisions, missing `<vector>` / `<optional>` / `<cstdio>` includes, and `size_t` vs `int` overload mismatches.

---

## 9. Immediate Next Work: Task 4.5 Polish (approved by the user)

Before Task 5 (shaders), fix these specific issues the user has flagged:

1. **Window sizing:** open maximized (or fill 90 % of the primary display), honor DPI scale
2. **Shape spawn position:** new shapes appear at the **currently-visible viewport center**, not the world (640, 360)
3. **Scale/size clarity:** separate the concepts. `Size` = base authoring pixels (rarely changes). `Scale` = animation multiplier. Show both, but drag gizmos affect `Scale` only unless a modifier key is held
4. **Basic keyframe primitives:** on a property, click the ◆ diamond to add a keyframe at the current time. Time-based evaluation replaces the "one Bezier for selected scale" hack for at least Position and Scale
5. **Timeline strip:** add a time ruler across the top, a draggable playhead, and keyframe diamonds on each layer row
6. **Null Object layer type:** new `ShapeType::Null` that renders as an X marker only; can be parented to and can be a parent
7. **Camera-parenting UX:** dropdown in Composition Settings — "Camera Style: [After Effects (Camera is a layer, parent freely)] [Alight Motion (layers attach TO camera as HUD)]". Under Alight mode, layers with a `stickToCamera` flag inherit the camera's view transform for a HUD effect

After 4.5 lands, proceed to Task 5 shaders.

---

## 9.5 State After Task 4.5 (post user screenshot review)

The user tested the 4.5 build and shipped a screenshot. Ship succeeded but the review was blunt and correct: **"4.5 polished is only look not functional."**

### What's cosmetically present but functionally broken
- **Composition Viewport is not centered.** Confirmed: the panel uses panel-pixel coordinates as world coordinates directly, so the 1280×720 composition guide rectangle floats wherever the panel size happens to place it (in the user's screenshot it's in the bottom-right of a mostly-empty black panel). Every shape spawns relative to the panel's top-left corner, not the composition center. LAYOUT_MAP.md Section 3 has the diagrams.
- **Inspector `Scale` field is fighting the Slingshot demo.** The "Slingshot -> Selected Scale" checkbox is on by default, and the global Bezier silently overwrites `transform.scale.x/y` every frame in `BeginFrame()`. So the user types a number into the Scale field and it gets stomped on the next frame. Confusing.
- **Inspector labels clip.** Panel width isn't reserved for the K button + label combo, so `Position` → `Positi`, `Rotation` → `Rotati`, `Opacity` → `Opacit`, `Scale` → `Scal`.
- **Keyframe diamonds don't appear in the timeline strip** after clicking the K button. Either the K click doesn't reach the code path or the diamond drawing is off-row.
- **"Slingshot Bezier Handles" and "Composition Clock" sections show in the per-layer Inspector**, even though they are global-clock properties. Belongs in a separate "Global" tab or the Comp menu.
- **Timeline strip label column is 140px fixed**, which truncates layer names.

### User's decision (locked in, don't re-litigate)

The user chose **skeleton-first**: proceed with Task 5 shaders and Task 6 export before any usability rewrite. Rationale: the underlying data model is right; the UX wiring can be fixed in one clean pass once the whole feature surface is present. Attempting to polish before all the pieces are there wastes work when Task 5/6 will add more properties that need the same UX pattern anyway.

**Concrete calls from the user:**
- **Composition default resolution:** 1920×1080 (not 1280×720). Fixed centered canvas becomes part of the post-Task-6 usability pass, not now.
- **Slingshot demo checkbox:** keep it for backwards compatibility, but **default OFF** and move out of the main Timeline strip into a debug/demo submenu so it stops fighting the Scale field.
- **Ordering:** Task 5 → Task 6 → Task 5.0 Usability Pass (deferred) → Task 7 Real Keyframe Easing → Task 8 Undo/Save.

### Task 5.0 Usability Pass — deferred but scheduled

To be executed AFTER Task 6, in one pass, fixing:
1. Centered composition canvas at fixed resolution with letterbox bars
2. Proper world↔screen transform (one function, one direction)
3. Corner-scale gizmo redone with linear mouse-delta math (current version is non-linear near anchor)
4. Rotation gizmo on canvas
5. Inspector layout with tabs (Transform / Effects / Global) so per-layer and global properties don't mix
6. Timeline strip: zoom, scroll, wider label column, keyframe context menu (right-click row to add key at playhead)
7. "Reset Layout" menu item so users can escape a broken imgui.ini
8. Real DPI awareness

---


## 10. Task 5 Preview (for planning only, do not start yet)

- HLSL shader stack under `src/shaders/`:
  - `motion_tile.hlsl` — UV wrap: `abs(frac(uv * N) * 2.0 - 1.0)`
  - `motion_blur.hlsl` — directional pixel sampling along velocity
  - `chroma_aberration.hlsl` — distance-based RGB channel offset
  - `blend_modes.hlsl` — Additive / Multiply / Screen / Overlay / Color Dodge
- `EffectManager` owns compiled shader blobs (compiled once in `Initialize()`), applies them to layer render targets between the composite pass and the final blit
- Effects palette panel; Effect Controls panel becomes real (currently a menu stub)
- Global "Enable Motion Blur" toggle in Composition Settings

---

## 11. Task 6 Preview (for planning only, do not start yet)

- `ExportEngine` with the proxy/full swap:
  - `EditMode` = 1280 × 720 MJPEG proxies for lag-free scrubbing
  - `ExportMode` = swap media handles to 4K originals
- FFmpeg via `_popen` piping a single BGRA frame at a time to `ffmpeg` — never > 1 frame of 4K in RAM (~33 MB)
- Render Queue panel

---

## 12. History of Failed Builds (for institutional memory)

| Commit | Failure | Root cause | Fix |
|---|---|---|---|
| 3c52405 and 20 prior | MSVC unresolved everything | Stray `a` before `"imgui.h"` in `RenderEngine.h` | Remove the `a` |
| b243396 (Task 3) | MSVC: `std::clamp` won't compile | `<windows.h>` defines `min`/`max` as macros | `#define NOMINMAX` before `#include <windows.h>` |
| — | — | — | — |

Both fixes are now in the codebase permanently. Any regression on either would show up as the same symptom.

---

## 13. Security Notes for Agents

- User has pasted two Personal Access Tokens into chat so far. Both should now be revoked by the user. If you need to push, ask the user for a **fresh, fine-grained, single-repo, 1-day-expiry** PAT.
- **Never** hard-code a token in a source file, a CMake file, or a workflow YAML.
- After using a pushed token, immediately `git remote set-url origin` back to the plain HTTPS URL and confirm no `github_pat_` string appears in the workspace: `grep -rn github_pat_ /home/user`.
- Do not use `git config credential.helper store`.

---

*End of briefing. If you are an agent starting fresh: read Sections 1-4 to understand the code, Section 4 (bugs) and Section 5 (gap) to understand what is missing, and Section 7 (rules) to understand how to write code that ships. Then look at what the user asks for and cross-reference against Section 5's roadmap.*
