# Design Doc — Commit 12 (Task 5.8): Preview vs Composition Resolution Split

**Base commit:** `508deba` (yesterday's Task 5.6-fix-2)
**LOC delta estimate:** +130 / -25
**User-visible change:** New viewport toolbar dropdown: **Preview Scale
(Full / Half / Quarter)**. Composition dimensions stay locked at whatever
you set in Comp Settings; the internal render target is downsampled by
the preview scale for perf. Fixes the "shapes go invisible at low res"
bug — because low res is no longer implemented by shrinking the
composition itself.

---

## 1. The bug we're fixing

When the user lowers "composition resolution" in the current build,
`compositionWidth/Height` shrink AND the compRT shrinks with them. But
shapes are authored in composition-pixel coordinates — a shape at
(960, 540) in a 1920×1080 comp becomes off-screen in a 640×360 comp
because `BuildShapeMVP` divides by target dims to produce NDC. The
shape doesn't go invisible; it flies off to NDC.x = 3.0 or wherever.
Symptom: user says "shapes go invisible."

The real design fix: **decouple "how big is the canvas the user is
drawing on" from "how many pixels do we allocate to display it."**
Composition dims are the AUTHORING space. Preview scale is a
performance knob that trades RT resolution for speed WITHOUT touching
any coordinate system the user cares about.

AE does this. Alight does this. Every serious motion graphics tool
does this. We shipped without it because Task 5 was busy nailing down
the animation model.

---

## 2. Architecture split

### Terms

- **Composition dimensions** (`compositionWidth`, `compositionHeight`):
  the user-authored canvas size. Everything in a `.pmge` file — shape
  positions, sizes, camera framing — lives in this coordinate space.
  Set via Composition Settings modal. Unchanged by this commit.

- **Preview scale** (`previewScale`, float in {1.0, 0.5, 0.25}): a
  performance multiplier applied ONLY to the render target allocation
  and the D3D11 viewport dims. NOT persisted in `.pmge` (editor state,
  not scene state, like the graph editor's mode toggle). Default 1.0.

- **RT dimensions** (`compTextureWidth`, `compTextureHeight`):
  `compositionWidth * previewScale`, `compositionHeight * previewScale`.
  Rebuilt whenever either input changes.

### The critical invariant

**All coordinate math is in composition pixels, always.** Shape
positions, gizmo hit-tests, ScreenToCanvas — all unchanged. The ONLY
thing the RT dims affect is:

1. `D3D11_VIEWPORT` in `CompositionRenderer::RenderLayers` — matches
   RT dims so the rasterizer fills the allocated pixels.
2. RT texture allocation size.
3. The letterbox draws the SRV upscaled by the ImGui `AddImage` call
   (automatic — texture minification/magnification is a hardware
   sample the ImGui backend does for free).

`BuildShapeMVP` gets a NEW `logicalW/H` pair for MVP division. The
existing `targetW/H` becomes viewport dims. When we set `logicalW/H
= compositionWidth/Height` and `targetW/H = compRT dims`, the shape
positions map to NDC correctly (comp-relative), then the rasterizer
scales to whatever pixel count the RT has. Bit-exact identical output
at Full scale; smooth downsampled output at Half/Quarter.

### Why this doesn't break existing code

- `ScreenToCanvas` maps letterbox screen pixels → composition pixels.
  Currently it uses `compTextureWidth/Height`. Change to use
  `compositionWidth/Height` — this is what the callers actually want.
  All hit-test / gizmo math continues to work in composition coords.
- `CompositionRenderer::RenderLayers` gets new optional params
  `logicalW, logicalH` defaulting to `targetW, targetH` so existing
  callers (there aren't many — RenderEngine viewport pass and export
  pump) continue to compile untouched.
- Export path: passes `logicalW/H = compositionWidth/Height` and
  `targetW/H = export dims`. Since export dims already equal comp
  dims after Task 6.1 (comp-driven export), this is a no-op there too.

---

## 3. UI

Viewport panel gets a small toolbar at the top:

```
Preview: [Full ▼]   FPS: 60   Canvas: 1080×1920
```

Preview dropdown: `Full`, `Half`, `Quarter`. Three options only — more
would be feature creep and each option doubles VRAM footprint variance.

FPS reads `1.0f / deltaTime` averaged over the last 30 frames (dead
simple, no allocation). Shows the perf benefit of dropping to Half.

Canvas is a text-only readout of `compositionWidth × compositionHeight`,
matches what Comp Settings shows. Just a reminder so users don't
confuse preview scale with composition size.

Changing the dropdown triggers `ReleaseCompositionRT()` +
`CreateCompositionRT(newW, newH)`. Same code path as changing comp
dims in the Comp Settings modal — battle-tested.

---

## 4. Files changing

```
src/CompositionRenderer.h    MODIFIED  +6 / -3    RenderLayers gains
                                                    logicalW, logicalH
                                                    with defaults
src/CompositionRenderer.cpp  MODIFIED  +15 / -8   BuildShapeMVP gets
                                                    logicalW/H for
                                                    NDC math; viewport
                                                    still uses target
src/RenderEngine.h           MODIFIED  +8         previewScale +
                                                    showPreviewFps + fps
                                                    smoothing ring
src/RenderEngine.cpp         MODIFIED  +90 / -12  toolbar; RT rebuild
                                                    on preview change;
                                                    ScreenToCanvas uses
                                                    composition dims;
                                                    Comp Settings modal
                                                    resize path also
                                                    respects previewScale;
                                                    RenderLayers calls
                                                    updated to pass
                                                    logicalW/H
DESIGN_COMMIT12_PREVIEW_SCALE.md  NEW  this file
```

Net ~+100 LOC. Binary impact negligible.

**Not touched:** Serialization. Preview scale is editor state, not
scene state, so `.pmge` files don't carry it. Matches how comp view
zoom, panel dock positions, etc. aren't in the file.

---

## 5. What this does NOT do (deferred)

- **Per-layer effect RT pool (Task 5.5).** Still on the todo. Preview
  scale changes RT allocation size, which happens to match one thing
  Gemini's exact-2-RT constraint would need to know about — but the
  two commits are independent. 5.5 is next after this if you want.

- **Preview scale animated between frames.** Would let a comp play
  back at Quarter for scrub perf and jump to Full on pause. Nice
  polish, not shipping yet — needs care to avoid RT thrash.

- **Auto-preview based on RT size.** Could auto-drop to Half above 4K
  and Quarter above 8K to protect the potato GPU. Deferred — user's
  explicit choice via the dropdown is fine for v1.

---

## 6. Test plan

1. Set comp to 1920×1080. Add rectangle at (960, 540). Preview =
   Full. Rectangle appears center-canvas.
2. Switch preview to Half. Rectangle STILL appears center-canvas at
   the same visual size (letterbox upscales the RT). FPS counter
   goes up.
3. Switch to Quarter. Same visual, more FPS.
4. Set comp to 4K (3840×2160). Preview = Full → likely sluggish on
   potato. Preview = Quarter → snappy. Both look correct.
5. Comp Settings → change comp to 1080×1920 (portrait) while preview
   is Half. RT rebuilds correctly. Aspect ratio letterboxes right.
6. Export → still runs at full comp resolution (preview scale doesn't
   affect export path).
7. Gizmo drag / click-to-select works identically at all preview
   scales — coordinate space is composition pixels regardless.
8. Save `.pmge` at preview Quarter → reopen → preview defaults to
   Full (editor state, not persisted). Comp dims survive.

---

## 7. Go

No open questions. Preview scale = editor state, three options only,
default Full. Executing single commit.
