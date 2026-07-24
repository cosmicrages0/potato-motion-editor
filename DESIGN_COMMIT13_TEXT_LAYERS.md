# Design Doc — Commit 13 (Task 5.9): Text Layers with DirectWrite

**Base commit:** `2554224` (Task 5.8-fix: freeze clock during drag)
**LOC delta estimate:** +720 / -15
**User-visible change:** New "Text" layer type. Font picker with system-
font enumeration, favorites (starred fonts pinned to the top),
size / color / alignment / weight controls, editable string. Animates
via the same AnimatedProperty pattern as every other layer.

This is the "oh, this is a real motion graphics editor" commit. 60% of
motion graphics is text.

---

## 1. Rendering strategy — cache-once, sample-many

Windows text APIs are a maze. DirectWrite is the modern one but its
"native" Direct2D backend requires a D3D10.1 device shared to D3D11 via
DXGI keyed mutex — a lot of glue for what we need. Skip that.

**Chosen path (proven by many editors, e.g. FW1 wrapper):**

1. Use `IDWriteBitmapRenderTarget` to rasterize the whole text string
   once into a CPU RGBA bitmap. DirectWrite handles font shaping,
   kerning, cleartype AA, ligatures — all the hard typography stuff.
2. Upload that bitmap into a per-layer `ID3D11Texture2D`.
3. Draw the texture as a quad through a new tiny pixel shader
   `ps_text_` that samples the alpha and tints by the layer's text
   color.
4. Cache invalidation: re-rasterize only when text string / font
   family / font size / weight changes. Position / rotation / scale /
   color changes just re-use the existing bitmap — cheap.

Why cache-once instead of every frame:
- A 500 px × 100 px text bitmap = 200 KB RAM.
- On potato hardware, per-frame DirectWrite rasterization would eat
  ~2ms per text layer. 10 text layers = 20ms per frame → dropped below
  60fps.
- Cache-once is O(1) per frame after the first (just draw a quad).
  Only invalidates on text edit which is rare in playback loop.
- Rebuild cost hits during typing in the Inspector — user's already
  focused there, ~5ms rasterization is invisible.

---

## 2. Data model

### `Layer.type` gains `ShapeType::Text`

```cpp
enum class ShapeType : int {
    Rectangle  = 0,
    Ellipse    = 1,
    CustomPath = 2,
    Camera     = 3,
    Null       = 4,
    Text       = 5,   // NEW
};
```

### `Layer` gains a `TextProps` sub-struct

```cpp
struct TextProps {
    std::string text          = "Text";
    std::string fontFamily    = "Segoe UI";
    float       fontSize      = 72.0f;        // pixels (matches AE convention)
    int         fontWeight    = 400;          // 100..900 DirectWrite scale
    bool        italic        = false;
    int         alignment     = 0;            // 0=left, 1=center, 2=right
    // Fill color reuses Layer.fillColor (already there, no new field).
    // Stroke reuses Layer.strokeColor + strokeWidth (already there —
    // stroke on text becomes a pixel-shader outline. Cost = free.)
};

struct Layer {
    ...existing...
    TextProps textProps;   // only meaningful when type == Text
};
```

**Not animated in this commit:**  `text` string, `fontFamily`,
`fontWeight`, `italic`, `alignment` — these are content, not motion.
Font size stays static too (animate scale.x/y instead, which the SDF
handles naturally). Position / rotation / scale / opacity / fillColor
already animate via the existing AnimatedProperty system — free win.

### `Layer` cache fields (not serialized)

```cpp
// TextRenderer caches these; layer owns them for lifetime + tear-down.
// mutable so const paths (Evaluate) don't lie about "not mutating layer."
mutable ID3D11Texture2D*         textTex      = nullptr;
mutable ID3D11ShaderResourceView* textSRV     = nullptr;
mutable int                       textTexW    = 0;
mutable int                       textTexH    = 0;
// Cache-key hash of the props that affect rasterization. Compared each
// frame; mismatch triggers re-rasterize.
mutable size_t                    textCacheKey = 0;
```

---

## 3. `TextRenderer` — new module

New files: `src/TextRenderer.h` + `.cpp`.

### Responsibilities

- Own the DirectWrite factory + a shared `IDWriteBitmapRenderTarget`.
- `RasterizeString(text, fontFamily, size, weight, italic, alignment,
  outBitmap, outW, outH)` — returns a heap-allocated RGBA byte array
  sized to the smallest rect that fits the rendered string.
- `EnumerateSystemFonts(std::vector<std::string>& out)` — populate the
  font picker. Called once at startup; cached in a `RenderEngine`
  member so the picker dropdown doesn't hit DirectWrite every frame.
- `ComputeCacheKey(props)` — hash text + font + size + weight +
  italic + alignment for invalidation compares. Position/color don't
  affect the bitmap so they're excluded.
- `EnsureLayerCache(layer, device)` — checks the layer's cached hash
  vs current props; if different, rasterizes + uploads a new texture.

### Non-goals

- No per-glyph animation ("write-on" effect). Later.
- No rich text (mixed fonts/sizes in one string). Later.
- No path text (text along a curve). Later.
- No emoji color glyphs — DirectWrite handles this natively via COLR
  but the pixel shader treats the atlas as alpha-only. If a user drops
  emoji in, they'll see monochrome silhouettes. Acceptable v1.

---

## 4. Font favorites — persistent, cross-project

Font list can be 300+ entries on Windows. Favorites pin a user's
frequently-used subset to the top.

**Storage:** `%LOCALAPPDATA%\PotatoMotion\fonts.json`. Auto-created
on first favorite. Format:

```json
{
  "favorites": ["Segoe UI", "Impact", "Arial Black", "Comic Sans MS"]
}
```

- Loaded once at RenderEngine::Initialize into a `std::set<std::string>
  favoriteFonts` member.
- Written on every favorite toggle (small file, fine to rewrite whole
  thing — cost is one Win32 file write).
- If the file's malformed / missing, we start with an empty set. No
  crash, no error dialog — silent.

**Not in the .pmge project file.** Favorites are per-user, not per-
project. A .pmge that hard-codes "Impact" for a text layer loads fine
on any machine that has Impact; the machine's own favorites list is
independent.

**Picker UI:**

```
[Font ▼]
  ★ Segoe UI          <- favorites section, sorted alphabetically
  ★ Impact
  ★ Arial Black
  ─────────────
    Arial             <- all system fonts, sorted alphabetically
    Cambria
    Comic Sans MS
    Consolas
    ...
```

Each row has a small ☆/★ toggle on the right. Clicking it flips the
favorite state and rewrites the JSON. Selecting the row picks the
font.

---

## 5. Pixel shader — one new PS, tiny

`ps_text_` in `CompositionRenderer.cpp`. Samples the text atlas SRV
(alpha channel), tints by the fill color, does the same stroke +
outer-AA logic as `ps_shape_sdf_` for consistency.

```hlsl
Texture2D    texAtlas : register(t0);
SamplerState samLinear : register(s0);

float4 main(VSOut i) : SV_TARGET {
    float alpha = texAtlas.Sample(samLinear, i.uv).a;
    if (alpha < 0.001) discard;   // majority of the quad is empty
    float4 result = color;
    result.a *= alpha;
    return result;
}
```

Stroke on text = shipping a follow-up if requested. For v1, text is
fill-only. Users who need outlined text can duplicate the layer and
offset it — the classic film-title trick.

Font size in the DirectWrite side, layer scale in the SDF side:
- DirectWrite rasterizes at `fontSize` px so the bitmap resolution
  matches the on-screen size at Scale = 1.
- The Transform.scale property still scales the quad, so animating
  scale.x from 0 → 1 gives the classic pop-in effect without
  re-rasterization.
- Going way beyond scale = 1 will look pixelated. Fix: user bumps
  fontSize. Documented tradeoff.

---

## 6. Files changing

```
NEW  src/TextRenderer.h          ~80 LOC   DirectWrite wrapper +
                                            cache key + enumerate + rasterize
NEW  src/TextRenderer.cpp        ~250 LOC  the implementation

MOD  src/Layer.h                 +25 / -0  ShapeType::Text + TextProps
                                            + cache fields
MOD  src/LayerManager.cpp        +8        AddTextLayer helper (default
                                            props); DeleteLayer releases
                                            the cached texture too
MOD  src/CompositionRenderer.h   +8        ps_text_ + DrawText
MOD  src/CompositionRenderer.cpp +130      new PS source; DrawText
                                            method; RenderLayers switch
                                            gains Text case
MOD  src/RenderEngine.h          +10       fontEnumeration cache +
                                            favoriteFonts set + text
                                            renderer instance
MOD  src/RenderEngine.cpp        +180      Layer menu 'New Text';
                                            Inspector 'Text' collapsing
                                            header; font picker popup
                                            with favorites; +Text
                                            button in Timeline strip;
                                            init/shutdown wiring
MOD  src/Serialization.cpp       +25       Text type string + TextProps
                                            round-trip (backward compat)
MOD  CMakeLists.txt              +2        link Dwrite.lib
NEW  DESIGN_COMMIT13_TEXT_LAYERS.md  this file
```

Net ~+720 LOC. Binary impact ~+40 KB (DirectWrite import stubs +
TextRenderer object code).

---

## 7. Backward compat

Every .pmge from before Task 5.9 works unchanged:

- Loader falls back to `type = Rectangle` on unknown strings (existing
  behavior).
- Missing `textProps` field on a non-Text layer = ignored.
- Serialization only writes `textProps` when `type == Text`.

Old files load, new files with text layers require Task 5.9 build to
render correctly — pre-5.9 build would show the layer as a Rectangle
with the text layer's fillColor. Acceptable v1 forward-compat.

---

## 8. Test plan

1. Layer menu → New Text → text layer appears center-canvas with
   default "Text" string in Segoe UI 72px.
2. Inspector Text header → edit string → viewport updates within a
   frame or two (rasterize + upload latency).
3. Font picker → scroll → click "Impact" → text re-rasterizes with
   Impact. Star Impact → it moves to the favorites section at the top.
4. Reopen the app → favorites persist (fonts.json survived).
5. Set fontSize to 200 → text gets huge and stays crisp (re-raster
   at high res). Set scale.x to 3 → text gets 3x wider but blurry
   (documented; user should bump fontSize instead).
6. Set position keyframes at t=0 and t=1 → text tweens smoothly.
7. Ctrl+Z after any text edit → reverts cleanly.
8. Save → close → reopen → text layer restores with the right font,
   string, size, color.
9. Delete text layer → texture is released, no leak (verify via
   Debug panel VRAM counter or Task Manager).
10. Export MP4 → text renders in the exported video.

---

## 9. Considered and rejected

- **Freetype + STB image writer.** Would work cross-platform, but
  we're Windows-only and DirectWrite is already there. Adding a
  ~800 KB Freetype dependency for zero portability win is silly.
- **Per-glyph atlas + per-string kerning table.** Faster for changing
  strings (typewriter effect) but 3x the LOC. Wait for a user to ask.
- **ImGui text.** Only supports one bitmap font. Not motion-graphics
  quality. Non-starter.
- **Full D2D interop.** ~200 LOC of DXGI keyed mutex, D3D10.1 device,
  surface sharing. All of that pain to avoid one CPU→GPU upload per
  text-change. Bad tradeoff.

---

## 11. Pre-go review adjustments (locked)

All four reviewer fixes applied to the plan below. Numbered per the
review comment for traceability.

**#1 Grayscale AA (critical).** `IDWriteBitmapRenderTarget::DrawGlyphRun`
defaults to `DWRITE_RENDERING_MODE_DEFAULT` which produces ClearType
sub-pixel RGB coverage — the R/G/B channels represent per-color-channel
opacity for the pixel, NOT a proper alpha value. Sampling `.a` from
that in a shader gives nonsense.

Fix: create an `IDWriteRenderingParams` with
`DWRITE_RENDERING_MODE_ALIASED` or set the bitmap-render-target's
pixel-snapping / anti-aliasing mode to grayscale. Concretely:

```cpp
IDWriteRenderingParams* rp = nullptr;
factory->CreateCustomRenderingParams(
    /*gamma*/ 1.0f, /*enhancedContrast*/ 0.0f, /*clearTypeLevel*/ 0.0f,
    DWRITE_PIXEL_GEOMETRY_FLAT,
    DWRITE_RENDERING_MODE_NATURAL,   // grayscale-AA, single-channel coverage
    &rp);
bitmapRT->SetPixelsPerDip(1.0f);
```

Then when DrawGlyphRun renders, R = G = B = coverage. The bitmap
returned by `GetMemoryDC() -> GetDIBits()` is a proper single-channel
mask (all three channels equal per pixel).

**#2 Fill-only v1 (doc consistency).** Section 2's line "Stroke reuses
Layer.strokeColor + strokeWidth — cost = free" was aspirational and
wrong: alpha-atlas sampling can produce a stroked LOOK via distance-
field techniques, but only from an SDF text pass. Bitmap alpha alone
cannot draw an outline. Correcting the design to explicitly say
**fill only in v1** — matches Section 5's ps_text_ shader shape.
Stroke deferred with the other polish items (would need an SDF text
pipeline or a separate outline pre-pass).

**#3 ComPtr for owned COM handles.** Using `mutable ID3D11Texture2D*
textTex` is a leak waiting to happen: DeleteLayer needs to remember to
release; device-reset needs a full sweep; RVO/copy semantics on Layer
struct are landmines. Switch to
`Microsoft::WRL::ComPtr<ID3D11Texture2D>` / `ComPtr<ID3D11ShaderResourceView>`
which auto-releases on Layer destruction, on assign, on `.Reset()`
during re-rasterize. `<wrl/client.h>` is a header-only Windows SDK
dependency — free. Layer becomes safely copyable/movable by default.

**#4 fontFamily fallback.** If the .pmge file names a font not present
on the loading machine (e.g. project made on a Mac-user's fork using
"SF Pro Display" opened on a Windows box), `IDWriteFontCollection::
FindFamilyName` returns index-out-of-range. Rasterize would either
crash or produce empty output. Fix: `TextRenderer::RasterizeString`
first calls `FindFamilyName`; on miss, silently falls back to
"Segoe UI" (always present on Windows 7+). Logs one line to stderr so
the user can trace what happened. Original `fontFamily` in TextProps
is NOT overwritten — save/reopen on the machine with the real font
restores correct rendering.

**#5 Sample .r not .a (follows from #1).** Two clean options:
  a. Rasterize into R8G8B8A8, all channels equal, sample any channel.
     Wastes 4× texture memory.
  b. Upload as **DXGI_FORMAT_R8_UNORM**, sample `.r`. Halves texture
     memory AND matches the actual data (one channel of coverage).

**Going with (b)** because it's cheaper and clearer. `ps_text_` becomes:

```hlsl
float coverage = texAtlas.Sample(samLinear, i.uv).r;
if (coverage < 0.001) discard;
float4 result = color;
result.a *= coverage;
return result;
```

TextRenderer post-processes the DirectWrite RGBA bitmap to a single-
channel R8 buffer before upload — trivial per-pixel copy, fast even
at 4K text sizes.

## 12. Go

All four reviewer fixes merged. Executing single commit.

