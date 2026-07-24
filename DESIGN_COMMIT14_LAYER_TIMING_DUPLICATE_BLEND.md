# Design Doc — Commit 14 (Task 5.10): Layer trim + duplicate + blend + duration polish

**Base commit:** `40a0730` (Task 5.9-fix-2: text alignment / italic / weight / bounds)
**LOC delta estimate:** +250 / -12
**User-visible change:** Timeline finally feels like AE. Each layer row now
has a trim bar (drag edges to set in/out points, drag middle to slip both).
Ctrl+D duplicates the selected layer. Inspector gains a Blend Mode
dropdown that composites the layer over layers below using proper
Photoshop-style blend math. Composition Clock's duration field is
now double-click-to-type. Export duration auto-populates from the last
visible layer's out-point instead of a hardcoded default.

Six adjustments from user's pre-doc review are locked in.

---

## 1. Why this commit

All layers currently live from `t=0` to `t=compDuration`. You can't:
- trim a text intro to hit at 0.5s and disappear at 2s
- overlap two variants of the same shape on consecutive rows
- glow a highlight layer over content below via additive blend
- duplicate anything (Ctrl+D just navigates the menu bar in the current build)

These are 90% of AE workflow. Without them the tool can't produce a
real motion graphics piece — it can only animate a static composition.

Also two small user asks from the same message:
- Duration slider forces you to drag. Click-to-type is faster.
- Export duration is a separate hardcoded field. Should follow the
  actual timeline extent.

---

## 2. Data model (locked adjustments applied)

### `Layer` gains three fields

```cpp
struct Layer {
    // ... existing ...

    // Task 5.10: in/out point in COMPOSITION time (seconds).
    // -1.0f on outPoint = sentinel "extends to comp end". Resolved at
    // render time as (outPoint < 0 ? animEngine.duration : outPoint).
    // Old .pmge files missing these fields default to (0, -1) => same
    // as pre-5.10 behavior (visible for the entire comp).
    float inPoint  =  0.0f;
    float outPoint = -1.0f;

    // Task 5.10: per-layer blend mode. Reuses the SAME enum that
    // Effect.h defines for the composition-wide BlendMode effect, so
    // there's one BlendMode type in the codebase (adjustment #2).
    // 6 modes: Normal / Additive / Multiply / Screen / Overlay / ColorDodge.
    BlendMode blend = BlendMode::Normal;
};
```

**Keyframe times stay in COMP time** (adjustment #3 locked). Trimming the
left edge (`inPoint`) only CLIPS visibility — it does NOT shift keyframe
times. This preserves every existing `.pmge` file's animation exactly.
The AE "slip animation with the layer" gesture is a separate action
(Alt-drag the bar, future commit) — this commit ships the safer
clip-only behavior.

### Compat rules

- Missing `inPoint` on read → `0.0`
- Missing `outPoint` on read → `-1.0` (sentinel = "comp end")
- Missing `blend` on read → `BlendMode::Normal`
- On write: emit `inPoint` only when != 0; emit `outPoint` only when >= 0;
  emit `blend` only when != Normal. Keeps files short and byte-identical
  for untrimmed unblended layers.

---

## 3. Rendering — visibility check + blend state per layer

`CompositionRenderer::RenderLayers` gets a per-layer visibility gate at
the top of the loop:

```cpp
const float t = layerManager.CurrentCompTime();
for (auto& layer : layerManager.Layers()) {
    // Existing visibility guards
    if (!layer.isVisible) continue;
    if (layer.is3D)       continue;
    if (layer.type == ShapeType::Camera) continue;

    // Task 5.10: in/out visibility gate
    const float out = (layer.outPoint < 0.0f) ? compDuration : layer.outPoint;
    if (t < layer.inPoint || t > out) continue;

    // Task 5.10: bind the layer's blend state for this draw
    context_->OMSetBlendState(blendStates_[(int)layer.blend], ...);

    // ... existing per-shape switch (DrawRect / DrawEllipse / DrawText etc.)
}
// After the loop: restore Normal so downstream ImGui / effect chain
// doesn't inherit a stray blend state.
context_->OMSetBlendState(blend_normal_, ...);
```

The visibility check runs BEFORE keyframe evaluation, so trimmed-out
layers cost zero eval time.

**Comp duration source**: `layerManager.CurrentCompTime()` is set from
`animEngine.currentTime` in `BeginFrame`. Comp duration itself lives on
`animEngine.duration`. `CompositionRenderer` doesn't have a direct
handle to that — so `RenderLayers` gets a new `float compDuration`
parameter alongside the existing `logicalW/H` pair. Defaults to 0 for
back-compat (then the sentinel check falls through to "always visible",
which matches the pre-5.10 behavior).

**Export path** (`PumpExportOneFrameIfActive`) uses the same call with
`compDuration = animEngine.duration`. Same visibility gate; trimmed-out
layers don't render in the MP4. Test #7 in the plan verifies this.

---

## 4. Blend states — six states, precomputed at Init

Six ID3D11BlendState objects, one per BlendMode value. Created once in
`CreateBuffers`, released in `ReleaseAll`. Table lookup at draw time:

| Mode       | SrcBlend         | DestBlend       | Notes |
|------------|------------------|-----------------|-------|
| Normal     | SRC_ALPHA        | INV_SRC_ALPHA   | Standard premul-ish alpha |
| Additive   | SRC_ALPHA        | ONE             | Glow / highlight |
| Multiply   | DEST_COLOR       | ZERO            | Darkening / tint |
| Screen     | INV_DEST_COLOR   | ONE             | **Photoshop screen** (adjustment #5 locked) |
| Overlay    | SRC_ALPHA        | INV_SRC_ALPHA   | Approximation — true overlay needs a shader; fallback to Normal for v1 |
| ColorDodge | ONE              | INV_SRC_COLOR   | Rough approx; true dodge is a shader too |

Overlay/ColorDodge fall back to reasonable fixed-function approximations.
True per-channel math needs a pixel shader; deferred to a follow-up.
Users get honest visual feedback: pick "Overlay" and see the fallback,
not silent no-op.

**Interaction with existing Effect-based BlendMode**: `Effect.h`'s
`BlendMode` effect is composition-wide (applies to the whole comp RT
via a shader pass). Per-layer blend is per-DRAW (applies as this shape
is rasterized). They compose — the effect runs after all layers are on
compRTV. If a layer has both, both apply (layer-blend first at draw,
comp-blend later during effect chain). Documented as expected behavior.

---

## 5. Duplicate

`Ctrl+D` triggers `LayerManager::DuplicateLayer(id)`:

```cpp
int LayerManager::DuplicateLayer(int srcId) {
    const Layer* src = FindLayerById(srcId);
    if (!src) return -1;
    Layer copy = *src;           // shallow ok — ComPtr atlases AddRef safely
    copy.id   = nextId++;
    copy.name = src->name + " copy";
    // Adjustment #4 LOCKED: DO NOT shift inPoint/outPoint. AE
    // duplicates in place; users can drag if they want offset.
    // Insert immediately AFTER the original in the layer stack (higher z-order).
    auto it = std::find_if(layers.begin(), layers.end(),
        [srcId](const Layer& L){ return L.id == srcId; });
    if (it == layers.end()) layers.push_back(std::move(copy));
    else                    layers.insert(it + 1, std::move(copy));
    RebuildIndex();
    selectedLayerId = copy.id;
    return copy.id;
}
```

Ctrl+D handler in RenderEngine's SDL_KEYDOWN block:
```cpp
if ((event.key.keysym.mod & KMOD_CTRL) && k == SDLK_d) {
    if (layerManager.GetSelectedId() != -1) {
        MarkForSnapshot();
        layerManager.DuplicateLayer(layerManager.GetSelectedId());
    }
}
```

Snapshot BEFORE mutation per the Task 5.6 sync-snapshot convention.

**ComPtr shallow-copy safety**: `Layer` has `ComPtr<ID3D11Texture2D>
textTex` from Task 5.9. Copy-assigning a ComPtr AddRefs — the atlas is
shared between src and copy until one of them re-rasterizes on next
frame (cache-key match, no-op). Zero risk of dangling. On Save/Load
the atlases don't persist anyway (rebuilt on load from TextProps).

---

## 6. Timeline UI — trim bars

Each layer row in `DrawTimelineStrip` gains a bar:
- x0 = `TimeToX(layer.inPoint)`
- x1 = `TimeToX(layer.outPoint < 0 ? animEngine.duration : layer.outPoint)`
- Height = row height minus 4 px (leaves space for keyframe diamonds)
- Color = layer's fillColor at 40% alpha (see the shape's color at a
  glance)
- Border = 1 px white when selected

Three interaction zones per bar (invisible buttons):
1. **Left edge** (6 px wide) — drag = update `inPoint` only.
   Cursor `ImGuiMouseCursor_ResizeEW`. Clamped `[0, outPoint - 0.01]`.
2. **Right edge** (6 px wide) — drag = update `outPoint` only. Cursor
   same. Clamped `[inPoint + 0.01, compDuration]`. On drag,
   `outPoint = -1` sentinel is materialized to the real time so the
   drag has something to compare against.
3. **Middle** (bar body) — drag = slip both in/out together (same
   delta). Cursor `ImGuiMouseCursor_ResizeAll`. Clamps so neither end
   crosses `[0, compDuration]`.

All three fire `MarkForSnapshot()` on `IsItemActivated()` so Ctrl+Z
rewinds each trim/slip cleanly (per Task 5.6 sync-snapshot convention).

Bars sit BEHIND the existing keyframe diamonds so scrubbing and
diamond-clicking still work exactly as before — the diamond hit-tests
run first in the row's UI loop.

---

## 7. Duration polish (bundled user requests)

**7a. Composition Clock duration field — double-click-to-type.**
Current:
```cpp
ImGui::SliderFloat("Duration (s)", &animEngine.duration, 0.1f, 60.0f);
```
Replace with `SliderFloat` + `ImGuiSliderFlags_AlwaysClamp` and add
`ImGui::DragFloat`-style click-to-type via `ImGui::InputFloat` as a
paired widget below. Simpler: use `ImGui::SliderFloat` with the
`ImGuiSliderFlags_AlwaysClamp` flag — Ctrl+click on any ImGui slider
converts to text input by default. Already works. **User probably
missed that Ctrl+click works.** Bundle a "Duration:" `InputFloat` next
to the slider so it's discoverable without the modifier trick. Both
widgets bind to the same float.

Also do the same treatment for the Timeline panel's Duration slider
(line 666: `SliderFloat("Duration (s)##tl", ...)` → adds a paired
`InputFloat` next to it).

**7b. Export duration auto-populates from timeline extent.**
`pendingExportSeconds` is currently a separate float that defaults to
5.0. On modal open (or first frame if we skip a modal), compute:
```cpp
float autoExtent = 0.0f;
for (const auto& L : layerManager.Layers()) {
    if (!L.isVisible) continue;
    const float end = (L.outPoint < 0.0f) ? animEngine.duration : L.outPoint;
    autoExtent = std::max(autoExtent, end);
}
if (autoExtent < 0.1f) autoExtent = animEngine.duration; // fallback
pendingExportSeconds = std::min(autoExtent, animEngine.duration);
```
Fires whenever the Render Queue panel becomes visible. User can still
override the value in the Duration field manually. If they do, we
DON'T re-auto on next panel-open (respect user override) — track a
`exportDurationUserOverridden` bool.

---

## 8. Files changing

```
src/Layer.h                  +8    inPoint / outPoint / blend fields
src/LayerManager.h           +2    DuplicateLayer signature
src/LayerManager.cpp         +30   DuplicateLayer implementation
src/CompositionRenderer.h    +12   6 blend states, compDuration param
src/CompositionRenderer.cpp  +80   blend state creation/release/select,
                                    visibility gate at top of loop
src/RenderEngine.h           +2    exportDurationUserOverridden bool
src/RenderEngine.cpp         +140  Ctrl+D handler, timeline trim bars,
                                    inspector Blend combo, duration
                                    input paired with slider, export
                                    auto-duration on panel open,
                                    RenderLayers calls updated to pass
                                    compDuration
src/Serialization.cpp        +18   inPoint / outPoint / blend round-trip
                                    with defaults
DESIGN_COMMIT14_LAYER_TIMING_DUPLICATE_BLEND.md  NEW  this file
```

Net ~+280 LOC. Binary impact ~+3 KB.

---

## 9. Test plan

1. Add rect at t=0, in=0 out=2, comp 10s → only visible 0-2s.
2. Drag rect's right handle to 5s → visible to 5s.
3. Drag bar middle so it starts at 2s → visible 2-5s. **Local animation
   still evaluates at comp time** (adjustment #3 lock verified — no key
   shift when trimming).
4. Ctrl+D on the rect → new layer at same in/out, immediately after
   original in the stack, auto-selected. Both selectable
   independently.
5. Add a text layer over the rect. Inspector → Blend → Additive → text
   glows over the rect below.
6. Same test with Screen → highlights lift the rect without blowing
   out (INV_DEST_COLOR/ONE math, adjustment #5).
7. Save .pmge with trim + blend + duplicate → reopen → exactly the same
   layers/times/blend modes.
8. Old .pmge (pre-5.10) → loads with inPoint=0, outPoint=-1
   (auto-extends), blend=Normal → visually identical to before.
9. Export → trimmed-out layers don't render in the MP4 (verify frame
   at t=6s in the test-1 scene shows nothing but bg).
10. Duration field in Composition Clock and Timeline: click into the
    InputFloat and type "8.5", press Enter → duration = 8.5s.
11. Open Render Queue with a comp whose last outPoint is 3.2s and
    comp duration 10s → Duration field pre-fills to 3.2. Type 5.0 →
    it stays at 5.0 on subsequent Render Queue re-opens (user
    override respected).
12. Ctrl+Z after every trim / duplicate / blend change → cleanly
    reverts (sync-snapshot fires BEFORE mutation per Task 5.6
    convention).

---

## 10. Locked adjustments recap

1. `outPoint = -1.0f` sentinel, resolved at render time.
2. Reuse `BlendMode` from `Effect.h` — one enum, six values.
3. Keyframe times stay in COMP time. Trimming inPoint only clips
   visibility. Zero breakage for existing .pmge files.
4. Duplicate at exact same in/out; insert immediately after original.
5. Screen = `SrcBlend=INV_DEST_COLOR, DestBlend=ONE`.
6. Bundle duration InputFloat + export auto-duration in this commit.

## 11. Go

No open questions. Executing single commit.
