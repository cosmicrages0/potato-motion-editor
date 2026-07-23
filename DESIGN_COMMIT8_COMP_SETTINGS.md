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

## 10. Pre-go review adjustments (locked)

Reviewer flagged four issues before go. All applied to this design.

**#1 W/H clamp** was `[16, 8192]`. Raised floor to `[64, 8192]` — 16×16
breaks the letterbox math (division by tiny numbers) and produces an
unusable panel. 64 keeps geometry sane, 8192 keeps iGPU alive.

**#2 Snapshot timing bug** — this is a real latent bug, not just an
Apply-modal issue. `MarkForSnapshot()` sets a flag consumed at top of
next frame via `FlushPendingSnapshot`. For continuous drags this
captures pre-drag state correctly (mouse-down flags, mutation happens
across later frames, next frame's snapshot = pre-drag). For **atomic
ops in a single frame** (Apply button, "Delete Keyframe", "Set to
Bezier"), the mutation happens in-frame; the snapshot at frame N+1
captures the POST-mutation state → Ctrl+Z becomes a no-op.

Fix: rework `MarkForSnapshot()` to push **synchronously** the first
time it's called each frame, with a per-frame-number guard for
coalescing. Continuous drags still coalesce (all frame-N marks pre-
drag collapse into one), atomic ops now snapshot pre-mutation
correctly.

Code shape:
```cpp
uint64_t lastSnapshotFrame = 0;   // new member
uint64_t currentFrameNumber = 0;  // bumped at start of BeginFrame

void MarkForSnapshot() {
    if (currentFrameNumber != 0 && currentFrameNumber == lastSnapshotFrame)
        return;  // already snapshotted this frame — coalesce
    AppState st{}; BuildAppState(st);
    undoStack.PushSnapshot(st);
    lastSnapshotFrame = currentFrameNumber;
}
```

`FlushPendingSnapshot()` becomes a no-op and its call site is removed
(or kept as dead code with a comment for one commit, then removed
next). The `pendingSnapshot` bool goes away.

This fixes Apply-modal undo AND retroactively fixes Delete Keyframe,
Set to Bezier, and every other atomic op that ships MarkForSnapshot.

**#3 FPS custom-value preservation** — Combo is 24/30/60 only. If a
loaded .pmge has `fps=25` or `fps=29.97`, previous design would snap
it to 30 on next save → silently destroys user's setting.

Fix: on modal open, if `compositionFps` is not in `[24, 30, 60]`, show
a leading "Custom: X fps" entry in the combo. Selecting a preset
replaces the value normally. Not selecting = keep the custom value
intact. On save, we always write whatever `compositionFps` holds (no
snap in Serialization.cpp).

**#4 Staging init** — already right in the design (section 2 lists
pendingBgColor etc. and section 4 says "opens with current values
populated"). Adding a defensive one-liner comment in the code that the
copy from live→pending happens at modal-open time so a future refactor
can't forget it.

## 11. Reset Layout — no confirmation, fire-and-forget

Matches Blender. `View → Reset Layout` immediately nukes the dock node
and re-runs the initial builder next frame. Not undoable (layout state
isn't in AppState). If users want an escape, they can drag panels back
manually.

## 12. Timeline FPS ticks — deferred to next commit

Ruler drawing has heuristics that break at extreme FPS. Ship the modal
first, timeline sync follow-up. Export queue default FPS = comp FPS
ships THIS commit (one line).

## 13. Go

All fixes above merged into the plan. Executing single commit.
