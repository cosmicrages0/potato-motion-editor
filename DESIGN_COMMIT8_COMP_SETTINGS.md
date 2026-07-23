# Design Doc — Commit 8 (Task 5.6): Composition Settings Modal + Reset Layout

**Base commit:** `91cd59d` (Task 5.4-fix-3, all Graph Editor work landed)
**LOC delta estimate:** +160 / -20
**User-visible change:** Composition → Settings... now opens a modal where
you can change resolution, duration, frame rate, and background color.
View → Reset Layout puts every panel back where it started.

---

## 1. Why this commit

- **Resolution is hard-coded to 1920×1080.** Comp RT is allocated at that
  size in `Initialize()`. Users can't make a 1080×1920 vertical
  composition, a 1080p square, a 4K piece for a TV wall, or a 640×360
  low-res proxy for potato-laptop testing.
- **Background color is hard-coded** to `{0.08, 0.08, 0.10, 1.0}` in
  RenderEngine.cpp line 1735. Users can't do a white-background comp for
  a logo animation, a green-screen comp for keying, or a transparent
  comp for later compositing.
- **Duration lives on a slider in the Timeline** panel (works, but not
  discoverable). Belongs also in the settings modal.
- **Frame rate has no home** anywhere in the UI — it lives only inside
  `ExportEngine::Settings.fps` and applies only at export time. A comp's
  "working FPS" should also drive the timeline ruler tick spacing.
- **Layout is destructible.** ImGui's docking lets users drag panels
  anywhere. There's no way to recover the default AE-style layout short
  of deleting `imgui.ini` and restarting.

Both fixes are small, obvious, and unblock a lot of real use cases.

---

## 2. Data model changes

`AppState` (serialized) gains:

```cpp
struct AppState {
    // Existing:
    LayerManager*        layerManager = nullptr;
    Camera*              camera       = nullptr;
    AnimationEngine*     animEngine   = nullptr;
    int   compositionWidth  = 1920;
    int   compositionHeight = 1080;
    int   cameraStyleInt    = 0;
    bool  show3DFeatures    = false;

    // NEW:
    int   compositionFps    = 30;             // 24/30/60 typical
    float bgColor[4]        = {0.08f, 0.08f, 0.10f, 1.0f};
};
```

`RenderEngine` gains matching members:

```cpp
int   compositionFps = 30;
float bgColor[4]     = {0.08f, 0.08f, 0.10f, 1.0f};   // replaces hard-coded {0.08,...}
// Modal state:
bool  showCompSettingsModal = false;
// Staging fields — edits go here first so Cancel actually cancels.
int   pendingCompW    = 1920;
int   pendingCompH    = 1080;
int   pendingCompFps  = 30;
float pendingCompDur  = 5.0f;   // seconds; mirrors animEngine.duration
float pendingBgColor[4] = {0.08f, 0.08f, 0.10f, 1.0f};
```

Duration stays authoritative on `animEngine.duration` — the modal writes
back to it on OK.

---

## 3. Serialization (Task 5.2 schema addition)

`.pmge` `composition` block gains two fields:

```json
"composition": {
  "width": 1920,
  "height": 1080,
  "cameraStyle": "AfterEffects",
  "show3DFeatures": false,
  "fps": 30,                                       // NEW
  "bgColor": [0.08, 0.08, 0.10, 1.0]               // NEW (RGBA 0..1)
}
```

Missing fields default to `fps=30`, `bgColor={0.08,0.08,0.10,1.0}` for
backward compat. All Task 5.2-5.4 files load unchanged.

---

## 4. UI: Settings modal

Opens from `Composition → Settings...` menu (existing stub gets wired).

```
┌─ Composition Settings ─────────────────────────┐
│                                                │
│  Resolution                                    │
│    Width       [ 1920 ]  px                    │
│    Height      [ 1080 ]  px                    │
│    [Preset ▼] 1920×1080  •  1080×1920 (vert)   │
│                            1080×1080 (square)  │
│                            3840×2160 (4K)      │
│                            640×360 (proxy)     │
│                                                │
│  Timing                                        │
│    Duration    [ 5.00  ]  seconds              │
│    Frame Rate  [ 30 ▼ ]   (24 / 30 / 60)       │
│                                                │
│  Appearance                                    │
│    Background [ ■ ]  RGB color picker          │
│                                                │
│         [ Cancel ]              [ Apply ]      │
└────────────────────────────────────────────────┘
```

Behavior:

- Opens with current values populated into `pendingCompW/H/Fps/Dur/BgColor`.
- **Apply**:
  1. `MarkForSnapshot()` (undo captures the pre-change state)
  2. Copies pending fields → real fields
  3. If W/H changed, calls `ReleaseCompositionRT()` + `CreateCompositionRT()`,
     also resizes `EffectManager` ping-pong RTs
  4. Writes `animEngine.duration = pendingCompDur`
  5. Closes the modal
- **Cancel**: closes without applying anything
- **Presets dropdown** just fills the W/H fields — user still hits Apply
- Width / Height clamped to `[16, 8192]` so users can't allocate a
  gigabyte GPU texture by accident
- FPS is a combo, not a free-form input (24/30/60 only). Enforces the
  common motion-graphics rates; if a user wants 25 or 29.97 later we
  extend the combo. Keeps the modal simple.

---

## 5. UI: Reset Layout menu item

New `View → Reset Layout`. Click:

1. Call `ImGui::DockBuilderRemoveNode(dockspace_id)` to nuke current layout
2. Set `pendingResetLayout = true`
3. Next frame's `RenderAEDockingLayout()` detects the flag AND that the
   dock node is gone, so it re-runs the initial layout build (the code
   already there — currently only fires on first-ever launch because
   `imgui.ini` re-loads previous state)

Existing `if (ImGui::DockBuilderGetNode(dockspace_id) == nullptr) { ...
build initial layout ... }` block covers step 3 — we just have to make
the null-check pass by removing the node.

Small trap: dock IDs get baked into `imgui.ini`. Reset Layout needs to
happen on the frame AFTER the current one's dock queries so we don't
tear down mid-frame. Use a `pendingResetLayout` flag consumed at the top
of `RenderAEDockingLayout()`.

---

## 6. Background color wiring

Currently at line 1735:

```cpp
const float bg[4] = { 0.08f, 0.08f, 0.10f, 1.0f };
compRenderer.RenderLayers(compRTV, ..., bg);
```

Change to:

```cpp
compRenderer.RenderLayers(compRTV, ..., bgColor);
```

That's it. `CompositionRenderer::RenderLayers` already accepts `const
float bgColor[4]` and uses it for `ClearRenderTargetView`.

---

## 7. Frame rate wiring

`compositionFps` drives:

1. **Timeline ruler tick spacing** — DrawTimelineStrip currently draws
   ticks every 0.1s. Change to `1.0 / compositionFps` intervals so ticks
   align to frames. Nice-to-have; skip if it complicates the strip too
   much for this commit.
2. **Snap-to-frame on scrub** — later commit. Not this one.
3. **Export default** — `pendingExport.fps = compositionFps` on comp
   settings apply, so Render Queue starts with the right value.

For this commit I ship (3) only. Timeline tick spacing gets a follow-up
(risk: my ruler drawing code has heuristics that break at very low FPS).

---

## 8. Files changing

```
src/RenderEngine.h        MODIFIED  +12    modal state + comp fields
src/RenderEngine.cpp      MODIFIED  +140/-15  modal draw, menu wiring,
                                              bgColor use, reset layout
src/Serialization.h       MODIFIED  +2     AppState gains fps + bgColor
src/Serialization.cpp     MODIFIED  +15    read/write new fields with defaults
DESIGN_COMMIT8_COMP_SETTINGS.md  NEW  this file
```

Net ~+160 LOC. Binary impact negligible (~+2 KB after LTCG).

---

## 9. Test plan

1. Load a Task 5.4 `.pmge` — new fields default cleanly, playback unchanged.
2. Composition → Settings... → change W to 720, H to 1280, Apply → the
   viewport letterbox now shows a vertical canvas.
3. Change background to bright green, Apply → viewport background is green.
4. Change Duration to 10s, Apply → timeline ruler + Composition Clock
   both show 10s.
5. Change FPS to 60, Apply → Render Queue's FPS combo now defaults to 60.
6. Save → close → reopen → all changes persisted.
7. Ctrl+Z after Apply → all changes reverted (single snapshot).
8. Drag Inspector panel to the top-left corner. View → Reset Layout →
   Inspector snaps back to the right side. Every other panel too.
9. Cancel button on the modal → no changes applied, no undo entry.

---

## 10. Questions before I execute

**Q1.** For W/H clamps I picked `[16, 8192]`. AE goes down to 4 and up
to 30720. 8192 keeps us safe on integrated GPUs (a 30720×17280 comp RT
is ~2 GB and would immediately crash a potato). Accept the tighter
limit, or want wider?

**My rec: keep [16, 8192].** Integrated GPUs choke past 8K anyway.

**Q2.** Reset Layout — I want a small confirmation (`"Reset all panel
positions?"` yes/no popup) or fire-and-forget? Fire-and-forget matches
Blender; confirmation matches Photoshop.

**My rec: fire-and-forget** with Ctrl+Z as escape (though undo of layout
state isn't wired yet — Reset Layout is not undoable in this commit).

**Q3.** Timeline tick spacing driven by FPS — bundle in this commit or
defer to a follow-up?

**My rec: defer.** Ruler drawing has some heuristics that need care.
Ship the modal first, timeline sync next commit.

Say **"go single commit"** to execute all three my-rec defaults, or
tell me to adjust.
