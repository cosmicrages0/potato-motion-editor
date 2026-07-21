# LAYOUT_MAP.md — What The App Should Look Like

> **Purpose:** Give the user (and any remote agent) a precise map of every panel, its intended pixel geometry, its ImGui ID, and every widget inside it. If what you see on screen does not match this document, the difference *is the bug*. Mark up this file — or just send a screenshot — and I will fix the mismatch.

**Version:** As of commit `787328a` (Task 4.5)
**Compiled from:** `src/RenderEngine.cpp` → `RenderAEDockingLayout()`, `DrawViewportCanvas()`, `DrawInspectorPanel()`, `DrawTimelinePanel()`, `DrawTimelineStrip()`, `DrawGraphEditor()`, `DrawProjectAssetsPanel()`

---

## 1. Window (root)

- **SDL window title:** `Potato Motion Graphics Editor - x64`
- **Initial state:** `SDL_WINDOW_MAXIMIZED` — should fill your monitor
- **Fallback size** (if maximize fails or is denied by WM): 90 % of primary display's usable bounds, minimum 800 × 600
- **Resizable:** yes
- **DPI-aware:** `SDL_WINDOW_ALLOW_HIGHDPI` set (may or may not honor it depending on Windows scaling)

### ✅ Expected: Window fills your screen when you launch the .exe
### ❌ Symptom to watch for: Small floating window in the middle of the screen

---

## 2. Dockspace root

Dockspace ID: `MyDockSpace` inside window `MainDockSpace`
Occupies the full window minus the top menu bar. Layout is built **only on first run** (`if DockBuilderGetNode == nullptr`). After that, ImGui saves your layout to `imgui.ini` next to the .exe.

### First-run split diagram (intended)

```
+========================================================================+
| File  Edit  Composition  Layer  Effect  Export                         |  <- Menu bar
+============+==================================+========================+
|            |                                  |                        |
|  Project   |                                  |    Inspector           |
|  Assets    |     Composition Viewport         |    & Effects           |
|  (LEFT)    |     (CENTER, biggest panel)      |    (RIGHT)             |
|            |                                  |                        |
|            |                                  |                        |
|            |                                  |                        |
|            |                                  |                        |
|  ~20% W    |     ~55% W                       |    ~25% W              |
|            |                                  |                        |
+============+=================+================+========================+
|                              |                                         |
|  Timeline (BOTTOM-LEFT)      |  Graph Editor (BOTTOM-RIGHT)            |
|  ~50% W of bottom band       |  ~50% W of bottom band                  |
|                              |                                         |
|  35% of window HEIGHT        |                                         |
+==============================+=========================================+
```

### 🐛 Known problem you have flagged
The docks are not "optimised" — panels may collapse to zero width, or the Composition Viewport can shrink so small that the composition canvas becomes unusable.

**Root cause:** the first-run split uses fractions of `viewport->WorkSize`. If your first launch window is small (before `SDL_WINDOW_MAXIMIZED` takes effect on some window managers), those fractions bake in tiny sizes and `imgui.ini` persists them.

**Fix candidates (need your input):**
- Delete `imgui.ini` on every launch until layout is stable
- Enforce minimum panel sizes with `ImGui::DockBuilderSetNodeSize` per split
- Ship a "Reset Layout" menu item so users can escape a broken layout

---

## 3. Panel: Composition Viewport (the big one — the actual bug source)

**Window title:** `Composition Viewport`
**ImGui window flags:** default (dockable, no menu bar of its own)
**Rendered by:** `RenderEngine::DrawViewportCanvas()`

### What's inside — as of Task 4.5

```
+------------------------------------------------------------------+
| Active Camera [3D View]  FOV=45.0  Pos=(640, 360, -1000)         |  <- HUD text (top-left, ~14px)
| RMB/Alt+RMB: Orbit  MMB/Alt+MMB: Pan  Wheel: Zoom                |  <- Shortcuts hint
|                                                                  |
|  +----------------------------------+                            |
|  | Faint grey rectangle representing|                            |
|  | the 1280x720 composition guide   |                            |  <- Currently uses raw
|  | (only visible if the panel is    |                            |     panel pixels; NOT
|  | bigger than 1280x720 - which     |                            |     centered, NOT scaled
|  | on most screens it is NOT)       |                            |
|  |                                  |                            |
|  |      [Your shapes render here]   |                            |
|  |                                  |                            |
|  +----------------------------------+                            |
|                                                                  |
+------------------------------------------------------------------+
```

### 🐛 THIS IS THE MAIN BUG YOU FLAGGED

The composition viewport currently uses **panel pixels directly as world coordinates**. So:
- A shape at position `(640, 360)` sits 640 pixels right and 360 pixels down **from the panel's top-left corner**, not from the composition center
- If your Composition Viewport panel is 900 pixels wide, world x=640 is at 71% across the panel — visible but not centered
- If your Composition Viewport panel is 500 pixels wide, world x=640 is **off-screen to the right**
- Resizing the panel changes where "world (640, 360)" appears — that is not what you expect from an editor

### ✅ How it SHOULD work (this is what Task 5.0 will fix)

```
+------------------------------------------------------------------+
| Active Camera... FOV... Pos...                                   |
|                                                                  |
|            +-------------------------------------+               |
|            |                                     |               |
|            |     LOCKED 1280 x 720 CANVAS        |               |
|  dark      |     (or whatever comp resolution    |    dark       |
|  bars      |     the user picked)                |    bars       |
|            |                                     |               |
|  aka       |     * always centered in panel      |    aka        |
|  "letterbox|     * uniformly scaled to fit       |    "letterbox"|
|            |     * shapes at (640,360) always    |               |
|            |       appear at DEAD CENTER         |               |
|            |     * gray checkerboard behind      |               |
|            |                                     |               |
|            +-------------------------------------+               |
|                                                                  |
| Zoom: 42%   Cursor: (312, 187 comp)                              |  <- Bottom-left HUD
+------------------------------------------------------------------+
```

**In this fixed model:**
- The composition has a fixed logical resolution (default 1280×720, user-pickable)
- It's rendered into a rectangle that's **always centered in the viewport panel**
- Uniform scale so the whole comp fits with letterbox bars on the wide axis
- Every world→screen conversion goes through a single transform, so shapes at (640, 360) are **always at dead center** regardless of panel size
- Zoom-in/zoom-out actions scale that transform, not the world

---

## 4. Panel: Project Assets

**Window title:** `Project Assets`
**Rendered by:** `RenderEngine::DrawProjectAssetsPanel()`

```
+----------------------------------+
| Media Library                    |
| ----                             |
| • (Media import lands in Task 6) |
|                                  |
| Quick Add                        |
| [+ Rectangle] [+ Ellipse]        |
|                                  |
| (large empty area below)         |
+----------------------------------+
```

**Status:** Placeholder. The Quick Add buttons work (they route through `SpawnShapeAtViewportCenter`). Media import is Task 6.

---

## 5. Panel: Inspector & Effects

**Window title:** `Inspector & Effects`
**Rendered by:** `RenderEngine::DrawInspectorPanel()`

### When a layer is selected

```
+------------------------------------------+
| Name: [Rectangle 3___________________]   |
| Layer ID: 3   Parent ID: -1              |
|                                          |
| v Transform                              |
|   [K] Position (x,y,z)   [640] [360] [0] |  <- K button lights up if keyframes exist
|   [K] Rotation (deg)     [0]   [0]   [0] |
|   [K] Scale              [1]   [1]   [1] |
|       Anchor (0..1)      [0.5] [0.5]     |
|       Size (px)          [200] [120]     |
|       Size = base authoring pixels...    |  <- Helper text
|   [K] Opacity            [========1.00]  |
|       [ ] Stick to Camera (Alight HUD)   |  <- disabled unless Alight mode
|                                          |
| v Fill                                   |
|   Color [██]                             |
|                                          |
| v Composition Clock                      |
|   [Play] [Reset]  [x] Loop               |
|   Duration (s)  [==========1.00]         |
|   Time (s): 0.334                        |
|                                          |
| v Slingshot Bezier Handles               |
|   P1 (control) [0.25] [1.25]             |
|   P2 (control) [0.50] [0.90]             |
|   (help text about overshoot...)         |
|                                          |
| v Camera Properties                      |
|   (Driven by Camera layer #N, or None)   |
|   Cam Position [640] [360] [-1000]       |
|   Cam Target   [640] [360] [0]           |
|   [ ] Use LookAt Target                  |
|   FOV (vertical, deg) [====45.0]         |
|   Near / Far Z  [1.0] [10000.0]          |
|   [Reset Camera]                         |
+------------------------------------------+
```

### 🐛 Problems you may notice
- **Every property crammed into one panel** — should split into tabs (Transform / Effects / Bezier / Camera)
- **`Slingshot Bezier Handles` and `Composition Clock` show even when a shape is selected** — they belong to the global clock, not the layer, but appear inside the per-layer inspector, which is confusing
- **`Camera Properties` shows for every selected layer**, even if it's a rectangle — should only show when a Camera layer is selected

### When no layer is selected
```
+------------------------------------------+
| No layer selected. Add one from the      |
| Timeline.                                |
+------------------------------------------+
```

---

## 6. Panel: Timeline

**Window title:** `Timeline`
**Rendered by:** `RenderEngine::DrawTimelinePanel()` (which internally calls `DrawTimelineStrip()`)

```
+---------------------------------------------------------------------------+
| [+ Rect] [+ Ellipse] [+ Null] [+ Camera] [Delete Selected]                |
|   [x] Slingshot -> Selected Scale                                         |
| ------                                                                    |
|                                                                           |
|  +-----------------------------------------------------------------+      |
|  | 0.00s     0.25s     0.50s     0.75s     1.00s                    |  <- Ruler
|  |  |   .   .   |   .   .   |   .   .   |   .   .   |               |     with ticks
|  | ================ Background Rect ============================|===  <- Playhead
|  |     ◆         ◆                                                |    <- Position keys
|  | ================ Bouncing Ball ==============================|===
|  |          ◆         ◆                                          |
|  +-----------------------------------------------------------------+
|                                                                           |
| ------                                                                    |
| +----+----+---------------+----------+------------+                       |
| |Vis | 3D | Name          | Type     | Parent     |                       |
| +----+----+---------------+----------+------------+                       |
| |[X] |[ ] | Background R… | Rectangle| (none)     |                       |
| |[X] |[ ] | Bouncing Ball | Ellipse  | (none)     |                       |
| +----+----+---------------+----------+------------+                       |
+---------------------------------------------------------------------------+
```

### 🐛 Problems you flagged
- **"i cant see keyframes to add"** — the K buttons in Inspector do work, but the timeline strip diamonds show them AFTER the fact. There's no "click this timeline row to add a keyframe" workflow yet.
- **"timeline too"** — the strip only shows 1.00s max (the current comp duration). You probably want a scrollable, zoomable timeline.
- **Playhead scrubbing does work** but is invisible if the strip is very narrow.

---

## 7. Panel: Graph Editor

**Window title:** `Graph Editor`
**Rendered by:** `RenderEngine::DrawGraphEditor()`

```
+--------------------------------------------+
| Interactive Slingshot / Overshoot Curve    |
| (P1 & P2 handles can exceed 100%)          |
|                                            |
|  100% ————————————————————————             |
|         ╱⋮                                 |  <- Orange = P1 handle
|        ╱ ⋮                                 |     Cyan = the Bezier curve
|       ╱ ⋮⋮                                 |     Red vertical line = playhead
|      ╱  ⋮⋮                                 |
|     ╱   ⋮⋮                                 |
|   ╱    ⋮⋮                                  |
|  0% ————————————————————————               |
|                                            |
+--------------------------------------------+
```

**Status:** Works. Drag P1/P2 to shape the curve. Red playhead moves with the comp clock. **But** it drives only one global Bezier — every keyframe uses linear interpolation right now. Task 7 will make each keyframe have its own Bezier.

---

## 8. Every ImGui window ID (for debugging)

If you see a stray window / panel with no title, cross-reference it here.

| Window title | Purpose | Source function |
|---|---|---|
| `MainDockSpace` | Invisible container for the dockspace | `RenderAEDockingLayout` |
| `Project Assets` | Media library placeholder | `DrawProjectAssetsPanel` |
| `Composition Viewport` | The big canvas | `DrawViewportCanvas` |
| `Inspector & Effects` | Selected layer's properties | `DrawInspectorPanel` |
| `Timeline` | Layer list + timeline strip | `DrawTimelinePanel` + `DrawTimelineStrip` |
| `Graph Editor` | Bezier curve editor | `DrawGraphEditor` |

If you dock or float any of these, they can be restored via the ImGui default (`Ctrl+Tab` cycles windows in some ImGui builds).

---

## 9. Confirmed bugs from your feedback

| # | You said | Confirmed root cause | Fix priority |
|---|---|---|---|
| 1 | "when i click shapes it doesn't appear in middle of the canvas" | Panel-pixel-as-world; canvas isn't a fixed centered rectangle | 🔴 #1 |
| 2 | "when i open the program it doesn't fit to my screen" | `SDL_WINDOW_MAXIMIZED` set but some WMs ignore it; `imgui.ini` may persist a small layout from prior run | 🔴 #2 |
| 3 | "the shapes doesn't have scale and size functions properly" | Corner drag scales by mouse-delta / anchor-distance ratio which is wildly non-linear near the anchor; edge/mid handles missing; no shift-for-uniform | 🔴 #3 |
| 4 | "i can't see keyframes to add" | K buttons exist but are visually indistinct; no right-click-to-add on the timeline strip | 🔴 #4 |
| 5 | "time line too" | Timeline strip is fixed to 0…duration only; can't scroll, can't zoom, no in/out drag | 🟠 #5 |
| 6 | "docks are not being optimised" | First-run split doesn't enforce minimum panel widths; `imgui.ini` locks in bad layouts | 🟠 #6 |
| 7 | "resolution of layer also matters" | No composition resolution setting; whole app assumes 1280×720 in a couple of places | 🟠 #7 |
| 8 | "4.5 polished is only look not functional" | Keyframes sample correctly but the workflow to *set them* isn't discoverable | 🔴 #8 |

---

## 10. Your homework (please help me fix this properly)

Send me **any of the following** in your next message and I'll build a targeted fix:

1. **A screenshot of the app right after launch.** I want to see the initial window size and dock layout.
2. **A screenshot after clicking "+ Rect".** I want to see where the shape actually appeared vs where you expected it.
3. **A screenshot after trying to drag a corner scale handle.** I want to see what the shape does.
4. **Written annotations on this file.** Any line where you say "no, mine looks different: it's actually X" is gold.

Reply in any form — even a phone photo of your monitor works. Once I know exactly what's wrong on YOUR screen, I can ship a Task 5.0 Usability Pass that actually fixes it instead of guessing.
