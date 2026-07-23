# Design Doc — Commit 9 (Task 5.7): Shape Strokes + Rounded Corners

**Base commit:** `6e60be5` (Task 5.6-fix: Delete-key routes to keyframes)
**LOC delta estimate:** +180 / -15
**User-visible change:** Every shape gains stroke color + stroke width.
Rectangles gain a corner radius. Both are per-layer, editable in the
Inspector, and rendered by the GPU pixel shader — no CPU-side geometry
tessellation.

---

## 1. Why this commit

Every shape today is a hard-cornered, unstroked filled polygon. That's
enough to test the animation engine but nowhere close to what real motion
graphics need. Two 200-LOC gaps close a huge use-case delta:

- **Strokes** unlock outlined shapes, ring shapes, callouts, buttons.
- **Rounded corners** unlock modern-UI-style cards, tags, pills — the
  most common shape in every social media / product motion graphic
  shipped in the last 10 years.

Both features are perfectly suited to signed-distance-field (SDF) pixel
shaders — cheaper than tessellation, sharp at any zoom, animatable
without allocating new geometry per frame.

---

## 2. Data model changes

`Layer` (in `src/Layer.h`) gains three fields:

```cpp
struct Layer {
    // ... existing ...
    unsigned int fillColor    = 0xFFCCCC00;

    // Task 5.7: stroke + corner radius. Strokes are drawn INSIDE the shape
    // boundary (matches Figma / AE default) so a 0-alpha fill + non-zero
    // stroke width produces a proper outlined shape. cornerRadius is only
    // meaningful for Rectangles (ignored on Ellipse / Null / Camera).
    unsigned int strokeColor  = 0xFF000000; // ABGR; default opaque black
    float        strokeWidth  = 0.0f;       // pixels; 0 => no stroke
    float        cornerRadius = 0.0f;       // pixels; 0 => sharp corners
};
```

Defaults preserve current behavior: strokeWidth=0 means no stroke,
cornerRadius=0 means sharp corners → old scenes look identical.

**Not animated yet.** Making these AnimatedProperty<T> is possible but
adds serialization + inspector template complexity. Ship the plain
fields first; animate in a follow-up when a user actually asks (nobody
animates stroke color in practice — it's a static styling choice 99%
of the time).

---

## 3. Shader rework — one PS handles both features

Currently CompositionRenderer has two shape shaders (`ps_rect_`,
`ps_ellipse_`) plus a null-marker. Corner radius + stroke both need SDF
math, so we consolidate:

**New `ps_shape_sdf_`** replaces `ps_rect_` and `ps_ellipse_`. Selection
happens via `params.x`:

- `params.x == 0.0`: rectangle SDF (rounded corners when `params.z > 0`)
- `params.x == 1.0`: ellipse SDF (params.z ignored)

**Constant buffer additions** (all fit in existing spare slots — no new CB):

```
params.x = shape type (0=rect, 1=ellipse)     [existing]
params.y = stroke width in pixels             [NEW]
params.z = corner radius in pixels            [NEW]
params.w = shape half-extent max              [NEW; used for pixel-space SDF]

stroke   = float4 stroke color (RGBA 0..1)    [NEW CB slot — 16 bytes]
```

New `ShapeCB` layout:

```cpp
struct ShapeCB {
    float mvp[16];      // 64B
    float color[4];     // fill RGBA
    float stroke[4];    // stroke RGBA
    float params[4];    // (type, strokeWidth, cornerRadius, halfExtentMax)
};
// = 64 + 16 + 16 + 16 = 112 bytes, 16-byte aligned. Was 96. Still fits any HW.
```

### The SDF (HLSL sketch)

```hlsl
// UV is [0,1]. Convert to centered coords in PIXEL space so stroke width
// and corner radius interpret naturally.
float2 halfExtent = float2(params.w * uv.zw); // (see below)
float2 p          = (uv - 0.5) * 2.0 * halfExtent;  // pixel coords centered

float d;
if (params.x < 0.5) {
    // Rounded rectangle SDF (Iñigo Quílez):
    //   d = length(max(|p| - (halfExtent - r), 0)) - r
    float r = params.z;
    float2 q = abs(p) - halfExtent + r;
    d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
} else {
    // Ellipse SDF: exact form is expensive; use the classic approx that
    // matches close to circle. For non-circular we use an iterative fit,
    // but 1 iter is enough for stroke rendering.
    float2 e = halfExtent;
    d = length(p / e) - 1.0;
    d *= min(e.x, e.y);  // convert normalized d back to pixels (approx)
}

// Fill vs stroke composition:
float aa = 1.0;                          // 1 pixel of anti-alias
if (d > 0.0) discard;                    // outside the shape
float insideDepth = -d;                  // pixels from edge, positive

float4 result = color;                   // fill
if (params.y > 0.0) {
    float sw = params.y;
    // stroke covers the outermost `sw` pixels of the shape.
    float strokeMix = smoothstep(sw - aa, sw + aa, insideDepth);
    // strokeMix == 0 at edge (stroke color), 1 deeper in (fill color)
    result = lerp(stroke, color, strokeMix);
}
return result;
```

The `halfExtent` term needs the shape's world-space size in pixels; the
existing `BuildShapeMVP` bakes size into the MVP but doesn't expose it
to the PS. I pass it via `params.w` set from the pre-MVP size, and use
UV to reconstruct pixel-space coordinates.

Actually cleaner: pass full `halfExtent.xy` in a fresh CB slot. Let me
allocate `params2.xy` for that so I don't force `.w` to serve two masters:

```cpp
struct ShapeCB {
    float mvp[16];
    float color[4];
    float stroke[4];
    float params[4];   // (type, strokeWidth, cornerRadius, unused)
    float params2[4];  // (halfExtentX, halfExtentY, unused, unused)
};
```

Total 128 bytes. Still 16-aligned. Zero risk.

### Anti-aliasing at edges

`smoothstep(sw - aa, sw + aa, insideDepth)` gives a 2-pixel-wide soft
transition between fill and stroke. `d > 0 discard` still hard-cuts the
outside — that's fine because DX11 MSAA isn't enabled on the comp RT
and adding fwidth-based AA doubles the shader length. For potato
hardware, discard + smoothstep on the stroke boundary is good enough
and matches ImGui's own anti-aliasing style.

---

## 4. Inspector UI

Under the existing `Fill` collapsing header, add a new `Stroke` header
and (for rectangles) a `Corners` header:

```
▼ Fill
    Color         [■ picker]

▼ Stroke                                        (Task 5.7 NEW)
    Color         [■ picker]
    Width         [ 0.0 ] px                    slider 0..64

▼ Corners       (Rectangle only)                (Task 5.7 NEW)
    Radius        [ 0.0 ] px                    slider 0..min(w,h)/2
```

Both use standard `ImGui::ColorEdit4` + `ImGui::SliderFloat`.

`MarkForSnapshot()` fires on `IsItemActivated()` for each control so
Ctrl+Z rewinds each stroke/corner tweak cleanly.

Corners header hides itself for non-rectangles (they're meaningless on
ellipses/nulls/cameras).

---

## 5. Serialization (Task 5.2 schema extension)

Layer JSON gains three optional fields:

```json
{
  "id": 2, "type": "Rectangle", "fillColor": "0xFF00B4FF",
  "strokeColor":  "0xFF000000",              // NEW
  "strokeWidth":  2.0,                       // NEW
  "cornerRadius": 12.0,                      // NEW
  "transform": { ... }
}
```

Missing fields default to `0xFF000000` / `0.0` / `0.0` so all existing
`.pmge` files load unchanged.

---

## 6. Files changing

```
src/Layer.h                MODIFIED  +6 / -0    three new members
src/CompositionRenderer.h  MODIFIED  +5 / -3    ShapeCB grows;
                                                 DrawRect/DrawEllipse
                                                 signatures gain stroke +
                                                 corner params
src/CompositionRenderer.cpp MODIFIED +80 / -25  new consolidated SDF PS;
                                                 old ps_rect_/ps_ellipse_
                                                 replaced; DrawRect &
                                                 DrawEllipse forward the
                                                 new fields; RenderLayers
                                                 reads them off each layer
src/RenderEngine.cpp       MODIFIED  +40        Inspector controls; also
                                                 forward stroke/corner to
                                                 the legacy ImGui path
                                                 (nice-to-have, not
                                                 mission-critical)
src/Serialization.cpp      MODIFIED  +15        three optional fields
DESIGN_COMMIT9_STROKES_CORNERS.md  NEW  this file
```

Net ~+165 LOC. Binary impact ~+3-5 KB (new SDF shader ~500 chars
compressed).

---

## 7. Risks / considerations

**Ellipse SDF non-circular precision.** The `length(p/e) - 1.0` approx
is exact for circles; ovals get slightly warped stroke width. For
stroke widths under ~10% of the shorter radius the error is invisible.
If a user cranks a stroke to 40 px on a 100×20 ellipse we'll get
noticeable stroke-width variation around the perimeter. Fix (later
commit): switch to the iterative ellipse SDF (Iñigo Quílez has one, ~10
LOC, one Newton step). Not worth the code today.

**Rounded corners on rotated rectangles.** SDF is in the shape's LOCAL
pixel space (pre-MVP), so rotation Just Works — corners stay circular
regardless of rotation angle. Free win.

**Stroke width in ScaledPixel vs WorldPixel space.** I pick
**local-pixel space**: stroke of 4px means 4px in the shape's local
coordinate frame, so if you scale the layer 2x the stroke also renders
2x wider on screen. Matches Figma/AE. If users want scale-independent
strokes, that's a checkbox in a future commit.

**MSAA / smoothstep at zoom.** At very small zoom levels a 1px
stroke may alias. The smoothstep gives 2 pixels of soft transition
which mitigates this. Acceptable for a v1.

---

## 8. Test plan

1. Load a Task 5.6 `.pmge` — everything looks identical (defaults zero
   the new fields).
2. Add a new Rectangle → open Inspector → set stroke width to 4, stroke
   color to red → viewport shows the rectangle with a red 4-pixel
   inset border.
3. Set corner radius to 20 → corners round smoothly.
4. Set fill alpha to 0, keep stroke width 4 → **hollow outlined
   rectangle**. This is the "outline-only" use case.
5. Rotate the rectangle 30° via gizmo → corners remain circular, stroke
   width remains constant around the perimeter.
6. Scale the rectangle 2x → stroke visually doubles (local-pixel
   semantics).
7. Same on an Ellipse — Corners header is hidden, but Stroke works
   (ring shape).
8. Ctrl+Z after every tweak — reverts cleanly (MarkForSnapshot on
   IsItemActivated).
9. Save → close → reopen → all three fields round-trip.
10. Export to MP4 → strokes render correctly in the output (uses the
    same shader path).

---

## 10. Pre-go review adjustments (locked)

**#1 Outer edge AA** — the design's `discard` at `d > 0` gives a hard 1-px
jagged edge at low zoom. Fix: replace the discard with an outer-edge
alpha smoothstep. Pixel goes fully transparent 1 px outside the SDF,
fully opaque 1 px inside, blends across the 2-px window.

Revised HLSL (final):

```hlsl
// d < 0 = inside, d > 0 = outside, in pixels.
float aa = 1.0;                         // 1-pixel soft edge
float outerAlpha = 1.0 - smoothstep(-aa, aa, d);
if (outerAlpha <= 0.001) discard;       // still discard fully-outside frags

float4 result = color;                  // fill
if (params.y > 0.0) {                   // stroke width in pixels
    float sw = params.y;
    float strokeMix = smoothstep(sw - aa, sw + aa, -d); // -d = pixels inside
    result = lerp(stroke, color, strokeMix);
}
result.a *= outerAlpha;                 // fade the outer 1 px band
return result;
```

**#2 Corner clamp — both UI AND shader** —

UI: `ImGui::SliderFloat("Radius", &r, 0.0f, std::min(w,h) * 0.5f);`
so the slider max moves with the current size.

Shader: `float r = min(params.z, min(halfExtent.x, halfExtent.y));`
right before feeding it to the SDF. Belt-and-braces so a corrupt file or
mid-animation size change can't push the SDF into the pinched/inverted
regime.

**#3, #4 already correct.** Design's final CB layout uses `params2.xy`
for halfExtent (no `params.w` hack). Stroke color uses the same
`UnpackABGRToRGBAf` helper as fillColor — no channel-swap surprise.

**Preview vs Composition split** noted for a follow-up (Task 5.8). Every
comment about "shape goes invisible at low res" traces to the fact that
we're rendering directly at composition resolution and letterbox-scaling
in ImGui. A dedicated preview scale would let users work at 1/2 or 1/4
res for perf without changing the export resolution. Not this commit.

## 11. Go

All fixes above merged into the plan. Executing single commit.
