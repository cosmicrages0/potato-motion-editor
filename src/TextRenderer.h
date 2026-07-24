#pragma once
// =============================================================================
// TextRenderer (Task 5.9) — DirectWrite wrapper for Text layers.
//
// Rasterizes whole strings once into CPU R8 bitmaps, uploads them into
// per-Layer D3D11 R8_UNORM textures. RenderEngine's CompositionRenderer
// draws those as textured quads through a small ps_text_ pixel shader.
//
// Cache-once model: EnsureLayerCache() compares the layer's TextProps hash
// against the last-rasterized hash and only re-rasterizes on miss. Position/
// rotation/scale/opacity/color animate for free via the existing Transform
// system and never invalidate the cache — text is content, motion is
// motion.
//
// Reviewer fixes from pre-go review are locked in:
//   #1 Grayscale rendering (DWRITE_RENDERING_MODE_NATURAL, no ClearType) so
//      the resulting bitmap is a proper single-channel coverage mask.
//   #4 fontFamily fallback: if the requested family isn't installed on the
//      loading machine, RasterizeString silently falls back to "Segoe UI"
//      (always present on Windows 7+). Original TextProps.fontFamily is
//      NOT overwritten so save/reopen on a machine that DOES have the font
//      restores correct rendering.
//   #5 Upload as DXGI_FORMAT_R8_UNORM (single channel, half the RAM of
//      RGBA8) and sample .r in the pixel shader.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <cstddef>

struct Layer;      // fwd — full def in Layer.h
struct TextProps;  // fwd

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    // Non-copyable, movable via ComPtr semantics — safe default.
    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    // Bring up the DirectWrite factory. Idempotent. Returns false only on
    // catastrophic OS failure (missing dwrite.dll — Windows 7 SP1+ has it).
    bool Initialize();
    void Shutdown();
    bool IsReady() const { return factory_ != nullptr; }

    // Populate `out` with every installed font family, sorted A-Z.
    // Called once at startup by RenderEngine so the font picker doesn't
    // hit DirectWrite every frame. Duplicates are removed.
    void EnumerateSystemFonts(std::vector<std::string>& out);

    // Compute a hash of the props that affect rasterization. Position,
    // color, opacity, and scale are excluded — they don't invalidate the
    // bitmap.
    static std::size_t ComputeCacheKey(const TextProps& p);

    // Ensure `layer.textTex` matches the current `layer.textProps`. Compares
    // ComputeCacheKey(props) against layer.textCacheKey; on mismatch,
    // rasterizes + uploads a new texture and updates the hash. On match,
    // no-op — this is the O(1) fast path that runs every render frame.
    //
    // `device` must outlive the layer's texture (i.e. don't call this after
    // the D3D device has been released).
    //
    // Returns true if the layer has (or gained) a usable texture. Returns
    // false only when rasterization itself failed (bad font, zero-sized
    // output, out-of-VRAM). In that case textTex/textSRV are cleared so
    // callers can skip drawing.
    bool EnsureLayerCache(Layer& layer, ID3D11Device* device);

private:
    // The heavy lift: run DirectWrite -> R8 coverage bitmap. Caller owns
    // outPixels (std::vector). outW/outH set to the string's bounding-box
    // dims. Returns false on any failure; outPixels/outW/outH untouched.
    bool RasterizeString(const TextProps& props,
                         std::vector<unsigned char>& outPixels,
                         int& outW, int& outH);

    // GPU upload: build a D3D11 R8_UNORM texture + SRV from a CPU R8 buffer.
    // Returns false on device-creation failure. On success, populates the
    // ComPtrs (auto-releasing any previous contents).
    bool UploadR8Texture(ID3D11Device* device,
                         const unsigned char* pixels, int w, int h,
                         Microsoft::WRL::ComPtr<ID3D11Texture2D>& outTex,
                         Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& outSRV);

    Microsoft::WRL::ComPtr<IDWriteFactory>              factory_;
    Microsoft::WRL::ComPtr<IDWriteGdiInterop>           gdiInterop_;
    Microsoft::WRL::ComPtr<IDWriteRenderingParams>      renderParams_;  // grayscale-mode
    Microsoft::WRL::ComPtr<IDWriteBitmapRenderTarget>   bitmapTarget_;
    // Current bitmap size; grown as needed. Keeps repeated rasterizations
    // from thrashing HBITMAP allocations.
    int  bitmapCurW_ = 0;
    int  bitmapCurH_ = 0;
};
