# Design Doc — Commit 16 (Task 5.13): Per-Layer Effect RT Pool + Drop Shadow

**Base commit:** `6d46bab` (Task 5.12b — layout refactor)
**LOC delta estimate:** +380 / -60
**User-visible change:** Adding an effect to a layer now affects **only
that layer**, not the whole composition. Adds "Drop Shadow" — the most-
requested real effect — to demonstrate isolation working. Every existing
effect (Motion Tile, Motion Blur, Chromatic Aberration, Blend Mode)
also becomes per-layer.

---

## 1. Why this commit

Every basic motion graphics tool has this. Currently `EffectManager::
ApplyChain` runs ONCE against the whole `compRTV`, pooling every enabled
effect on every visible layer into one combined chain. Consequence: add
"Chromatic Aberration" to your text layer and the rectangle behind it
also gets fringed — user's obvious intent broken.

The RFC / Gemini already prescribed the architecture (see
`RFC_FOR_EXTERNAL_LLMS.md` responses): EXACTLY 2 render targets at comp
resolution, ping-ponged, shared across ALL layers per frame. The infra
is already there in `EffectManager` (`ping_rtv_` / `pong_rtv_`) — we
just call `ApplyChain` wrong.

Also: **batching-by-default** (Gemini's addition). Layers with NO
effects skip the ping-pong dance entirely and draw straight to
`compRTV`. Only layers WITH effects pay the cost. On typical projects
that's 10-20% of layers, so this stays fast on potato.

---

## 2. Architecture

### Current (broken) flow

```
compRTV = clear(bgColor)
for each layer:
    CompositionRenderer draws layer directly into compRTV
if (anyLayerHasEffects):
    combined = [every effect from every layer]
    ApplyChain(compSRV, compRTV, combined)  // <-- applies to WHOLE comp
```

### New flow (per-layer isolation)

```
compRTV = clear(bgColor)
for each layer in draw order:
    if (layer has no enabled effects):
        // Fast path: draw straight into compRTV (batching-by-default)
        CompositionRenderer draws layer into compRTV
    else:
        // Slow path: isolation
        1. Clear pingRTV to transparent
        2. CompositionRenderer draws ONLY this layer into pingRTV
        3. ApplyChain(pingSRV, pingRTV, layer.effects)
           - chain writes into pongRTV
           - if more effects, swap ping/pong
           - final pass writes back into pingRTV
        4. Composite pingSRV over compRTV via alpha blend
```

Cost per layer with effects = 1 layer draw + N effect passes + 1
composite blit. Zero VRAM growth beyond the existing 2 ping-pong RTs.

### Composite pass

Step 4 needs a new tiny pass shader — sample pingSRV with alpha, blend
into compRTV. Call it `ps_composite_`. Same fullscreen-triangle VB the
effect chain uses. Two lines of HLSL:

```hlsl
Texture2D    src : register(t0);
SamplerState samp: register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(VSOut i) : SV_TARGET {
    return src.Sample(samp, i.uv);  // straight sample; blend state
                                      // does the src.a-over-dest math
}
```

The `blend_alpha_` state we already have (`SRC_ALPHA / INV_SRC_ALPHA`)
does the right thing.

### Layer-level render helper

`CompositionRenderer::RenderLayers` currently loops over all visible
layers internally. We need a way to render ONE layer at a time.

Two options:

- **(a)** Add a public `CompositionRenderer::RenderSingleLayer(layer,
  targetRTV, ...)` method. Explicit, easy to test. My rec.
- **(b)** Give `RenderLayers` a per-layer callback where the caller
  decides destination RT. More flexible, harder to reason about.

Going with **(a)**. New method mirrors the existing per-shape dispatch
logic (Rect/Ellipse/Text/Null cases) but skips the outer for-loop.
Signature:

```cpp
void RenderSingleLayer(Layer& layer, ID3D11RenderTargetView* targetRTV,
                       UINT targetW, UINT targetH,
                       LayerManager& lm,
                       const float clearColor[4],  // typically transparent
                       UINT logicalW, UINT logicalH);
```

Clears the RT to `clearColor` before drawing (transparent for isolation
mode; the actual bgColor for full-scene mode). `RenderLayers` becomes
a thin wrapper that calls `RenderSingleLayer` for each layer in order
into the same `compRTV`.

Actually cleaner: keep `RenderLayers` unchanged, add
`RenderSingleLayer` as a new method that internally sets up viewport +
pipeline state + calls the existing per-shape draw. Zero risk to the
existing fast path.

### Where the new dispatch lives

`RenderEngine::DrawViewportCanvas` currently:
1. Calls `compRenderer.RenderLayers(...)` (draws all layers into compRTV)
2. Applies composition-wide effect chain if `anyLayerHasEffects`

New logic replaces both:

```cpp
compRenderer.ClearComp(compRTV, bgColor);
for each layer in draw order:
    if layer has no enabled effects:
        compRenderer.RenderSingleLayer(layer, compRTV, ...);
    else:
        // Isolation pass
        const float transparent[4] = {0,0,0,0};
        compRenderer.RenderSingleLayer(layer, pingRTV, ..., transparent);
        effectManager.ApplyChain(pingSRV, pingRTV, layer.effects);
        // Composite ping (containing filtered layer) over compRTV
        compRenderer.CompositeOver(pingSRV, compRTV);
```

`ClearComp` and `CompositeOver` are small new methods on
`CompositionRenderer` so the RT dance stays in one module.

---

## 3. Drop Shadow — the new effect

### What users see

Inspector Effect Controls tab, under a Drop Shadow effect entry:
- Distance (px) — shadow offset
- Angle (deg) — direction of the offset
- Softness (px) — Gaussian-ish blur radius
- Opacity (%) — shadow alpha
- Color — RGB shadow color

### How it renders

Drop shadow is TWO passes:
1. Sample the source texture with an OFFSET UV to make a colored shadow
   shape at the desired distance/angle. Blur it a bit (using a poor-
   man's 5-tap gaussian to keep cost bounded). Multiply by shadow color
   + opacity.
2. Composite the ORIGINAL source on top of the shadow (source appears
   in front, shadow behind).

Both passes fit in the existing ping-pong: pass 1 writes shadow into
pongRTV, pass 2 samples both source and shadow (needing TWO SRVs in
the shader) and composites into pingRTV.

**Complication**: existing effect chain assumes 1 SRV per pass. Drop
Shadow needs 2 SRVs (the shadow texture from the previous pass AND
the ORIGINAL source before any effect ran).

**Cleanest solution**: Drop Shadow is a "compound effect" that
internally does its own two-pass ping-pong. It grabs the ORIGINAL src
SRV at ApplyChain entry, blurs+offsets it into pingRTV, then samples
BOTH pingRTV and the original into pongRTV via a 2-texture shader.

That means Drop Shadow's PS binds TWO textures (t0=blurred shadow,
t1=original) and the CB carries color + opacity. New shader:
`ps_dropshadow_composite_`.

### Params

Reuses existing `EffectParams` struct — no data model change:

```
DropShadow: p0.x = Distance   p0.y = Angle(deg)  p0.z = Softness
            p1.x = Opacity(0-1)
            p2.xyz = Shadow color RGB
```

Factory `Effect::MakeDropShadow()` with sensible defaults (Distance=5,
Angle=135°, Softness=3, Opacity=0.6, Color=black).

### Cost

- 1 offset+blur pass (5-tap kernel = 5 texture samples per pixel)
- 1 composite pass (2 texture samples per pixel)

Well under the budget of a typical Chromatic Aberration (3+ samples
already). Won't tank potato performance.

---

## 4. Files changing

```
src/Effect.h                    +25   DropShadow enum entry + factory
src/EffectManager.h             +8    ApplyChain doc note; DropShadow
                                        needs internal state for the
                                        two-pass; no signature change
src/EffectManager.cpp          +150   new ps_dropshadow_ shaders (offset
                                        + composite); DropShadow branch
                                        in ApplyChain; new
                                        ps_composite_ for the RT-over-RT
                                        blit; handles the 2-texture bind
src/CompositionRenderer.h      +12    RenderSingleLayer + ClearComp +
                                        CompositeOver signatures
src/CompositionRenderer.cpp    +80    RenderSingleLayer impl (refactored
                                        from RenderLayers per-layer body);
                                        ClearComp / CompositeOver impls
src/RenderEngine.cpp           +60/-40  DrawViewportCanvas rewrite for
                                        per-layer dispatch; export pump
                                        gets the same treatment for
                                        parity
src/Serialization.cpp          +8     DropShadow enum <-> string
src/RenderEngine.cpp           +25    Effect menu 'Add Drop Shadow'
                                        entry; Effect Controls panel
                                        Drop Shadow sliders
DESIGN_COMMIT16_PER_LAYER_FX_DROPSHADOW.md   NEW  this file
```

Net ~+320 LOC. Binary impact ~+8 KB (two new PS shaders).

---

## 5. Backward compat

`.pmge` files with existing effects load unchanged. Old files with
Chromatic Aberration + Motion Blur bundled onto the "top" layer used
to affect the whole comp; after this commit they only affect that one
layer. Users may notice their old scenes look different — that's the
CORRECT behavior; the old behavior was a bug.

Documented in the commit body so users know why their old comps might
look tighter after the update.

---

## 6. Test plan

1. Add rectangle + ellipse. Add Chromatic Aberration to the ellipse
   only. Only the ellipse's edges fringe; rectangle stays crisp.
2. Add Drop Shadow to text. Shadow appears below-and-right of the
   text at 45°, soft-edged, semi-transparent. Rectangle behind the
   text is NOT shadowed.
3. Layer with no effects renders at the fast path (verify via Debug
   panel FPS — should match previous performance on the same scene).
4. Layer with 3 stacked effects (Motion Blur → Chromatic → Drop Shadow)
   applies them in order. Each keeps its slider working.
5. Undo/redo across add-effect, remove-effect, param-slider works
   (existing MarkForSnapshot paths unchanged).
6. Save + reload — Drop Shadow persists with all params intact.
7. Export MP4 — same visual as viewport.
8. Old .pmge with legacy comp-wide effects loads; effects now only
   apply to the layer that owns them.

---

## 7. Deferred (do not scope-creep)

- **Animatable effect params** — needs promoting EffectParams floats
  to AnimatedProperty<float>. Own commit.
- **More effects** — Gaussian Blur, Glow, Levels, Curves. Each own
  commit; DropShadow ships as the proof-of-isolation.
- **Effect presets** — save an effect stack as a template. Own commit.
- **Effect reordering via drag** — currently only add/remove buttons.
  Nice-to-have polish.

---

## 8. Go

No open questions. Executing single commit.
