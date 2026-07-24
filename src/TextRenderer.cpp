#include "TextRenderer.h"
#include "Layer.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>

using Microsoft::WRL::ComPtr;

// =============================================================================
// Construction / lifecycle
// =============================================================================
TextRenderer::TextRenderer() = default;
TextRenderer::~TextRenderer() { Shutdown(); }

bool TextRenderer::Initialize() {
    if (factory_) return true;   // idempotent

    // Shared-mode factory: cheaper than isolated, all DirectWrite consumers
    // in this process share the font cache. Perfect for a single-app editor.
    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                     __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(factory_.GetAddressOf()));
    if (FAILED(hr) || !factory_) {
        std::cerr << "[TextRenderer] DWriteCreateFactory failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    hr = factory_->GetGdiInterop(gdiInterop_.GetAddressOf());
    if (FAILED(hr) || !gdiInterop_) {
        std::cerr << "[TextRenderer] GetGdiInterop failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        Shutdown();
        return false;
    }

    // Reviewer-fix #1: force GRAYSCALE anti-aliasing by zeroing ClearType
    // level. Without this, DirectWrite renders with sub-pixel RGB coverage
    // and the R/G/B channels are per-color-channel opacity — meaningless
    // for an alpha-atlas sample. Grayscale rendering makes R == G == B ==
    // coverage; we then keep only R when uploading to R8_UNORM.
    //   gamma=1.8 is DirectWrite's documented default; we mirror it so text
    //   readability matches what users see in native Win32 apps.
    hr = factory_->CreateCustomRenderingParams(
        1.8f,                            // gamma
        0.0f,                            // enhancedContrast
        0.0f,                            // clearTypeLevel: 0 = pure grayscale
        DWRITE_PIXEL_GEOMETRY_FLAT,
        DWRITE_RENDERING_MODE_NATURAL,   // grayscale AA
        renderParams_.GetAddressOf());
    if (FAILED(hr) || !renderParams_) {
        std::cerr << "[TextRenderer] CreateCustomRenderingParams failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        Shutdown();
        return false;
    }

    // Start with a reasonable bitmap-target size; grown on demand.
    hr = gdiInterop_->CreateBitmapRenderTarget(nullptr, 512, 512,
                                                bitmapTarget_.GetAddressOf());
    if (FAILED(hr) || !bitmapTarget_) {
        std::cerr << "[TextRenderer] CreateBitmapRenderTarget failed hr=0x"
                  << std::hex << hr << std::dec << std::endl;
        Shutdown();
        return false;
    }
    bitmapTarget_->SetPixelsPerDip(1.0f);
    bitmapCurW_ = 512;
    bitmapCurH_ = 512;

    return true;
}

void TextRenderer::Shutdown() {
    bitmapTarget_.Reset();
    renderParams_.Reset();
    gdiInterop_.Reset();
    factory_.Reset();
    bitmapCurW_ = 0;
    bitmapCurH_ = 0;
}

// =============================================================================
// Font enumeration
// =============================================================================
void TextRenderer::EnumerateSystemFonts(std::vector<std::string>& out) {
    out.clear();
    if (!factory_) return;

    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory_->GetSystemFontCollection(collection.GetAddressOf(), FALSE))) {
        return;
    }

    const UINT32 famCount = collection->GetFontFamilyCount();
    out.reserve(famCount);
    for (UINT32 i = 0; i < famCount; ++i) {
        ComPtr<IDWriteFontFamily> fam;
        if (FAILED(collection->GetFontFamily(i, fam.GetAddressOf()))) continue;

        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(fam->GetFamilyNames(names.GetAddressOf()))) continue;

        // Prefer en-us; fall back to index 0 if not present.
        UINT32 idx = 0;
        BOOL   exists = FALSE;
        names->FindLocaleName(L"en-us", &idx, &exists);
        if (!exists) idx = 0;

        UINT32 len = 0;
        if (FAILED(names->GetStringLength(idx, &len)) || len == 0) continue;

        std::wstring wname(len + 1, L'\0');
        if (FAILED(names->GetString(idx, wname.data(), (UINT32)wname.size()))) continue;
        wname.resize(len);

        // UTF-16 -> UTF-8 for our std::string picker labels.
        const int u8len = WideCharToMultiByte(CP_UTF8, 0,
                                              wname.c_str(), (int)wname.size(),
                                              nullptr, 0, nullptr, nullptr);
        if (u8len <= 0) continue;
        std::string u8(u8len, '\0');
        WideCharToMultiByte(CP_UTF8, 0,
                            wname.c_str(), (int)wname.size(),
                            u8.data(), u8len, nullptr, nullptr);
        out.push_back(std::move(u8));
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

// =============================================================================
// Cache key
// =============================================================================
std::size_t TextRenderer::ComputeCacheKey(const TextProps& p) {
    // Cheap std::hash combine. Collisions would just cause one skipped
    // re-rasterize per session — harmless for a UX cache.
    auto mix = [](std::size_t seed, std::size_t v) {
        return seed ^ (v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
    };
    std::size_t h = 0;
    h = mix(h, std::hash<std::string>{}(p.text));
    h = mix(h, std::hash<std::string>{}(p.fontFamily));
    // Cast float to int-bits so bit-exact-equal floats produce equal hashes
    // (regular float hash on x87 legacy compilers can differ; safest to
    // reinterpret bytes).
    unsigned int sizeBits = 0;
    std::memcpy(&sizeBits, &p.fontSize, sizeof(sizeBits));
    h = mix(h, std::hash<unsigned int>{}(sizeBits));
    h = mix(h, std::hash<int>{}(p.fontWeight));
    h = mix(h, std::hash<int>{}((int)p.italic));
    h = mix(h, std::hash<int>{}(p.alignment));
    return h;
}

// =============================================================================
// Rasterize a string into an R8 coverage bitmap
// =============================================================================
bool TextRenderer::RasterizeString(const TextProps& props,
                                    std::vector<unsigned char>& outPixels,
                                    int& outW, int& outH) {
    if (!factory_ || !gdiInterop_ || !renderParams_ || !bitmapTarget_) return false;
    if (props.text.empty()) {
        // 1x1 transparent pixel keeps the sprite draw well-defined without
        // a degenerate texture.
        outPixels.assign(1, 0);
        outW = 1; outH = 1;
        return true;
    }

    // UTF-8 -> UTF-16 for DirectWrite.
    const int wlen = MultiByteToWideChar(CP_UTF8, 0,
                                          props.text.c_str(), (int)props.text.size(),
                                          nullptr, 0);
    if (wlen <= 0) return false;
    std::wstring wtext(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        props.text.c_str(), (int)props.text.size(),
                        wtext.data(), wlen);

    // Look up the requested font family. Reviewer-fix #4: if the requested
    // family isn't installed, fall back to "Segoe UI" (always on Windows
    // 7+). We DO NOT overwrite props.fontFamily — save/reopen on a machine
    // that has the font restores correct rendering.
    ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory_->GetSystemFontCollection(collection.GetAddressOf(), FALSE))) {
        return false;
    }
    std::wstring wfamily;
    {
        const int flen = MultiByteToWideChar(CP_UTF8, 0,
                                             props.fontFamily.c_str(), (int)props.fontFamily.size(),
                                             nullptr, 0);
        if (flen > 0) {
            wfamily.resize(flen);
            MultiByteToWideChar(CP_UTF8, 0,
                                props.fontFamily.c_str(), (int)props.fontFamily.size(),
                                wfamily.data(), flen);
        }
    }
    UINT32 famIdx = 0;
    BOOL   famExists = FALSE;
    if (!wfamily.empty()) {
        collection->FindFamilyName(wfamily.c_str(), &famIdx, &famExists);
    }
    if (!famExists) {
        std::cerr << "[TextRenderer] Font '" << props.fontFamily
                  << "' not found; falling back to Segoe UI" << std::endl;
        collection->FindFamilyName(L"Segoe UI", &famIdx, &famExists);
        if (!famExists) return false;   // no Segoe UI either — Windows too old
    }

    // Build a TextFormat with the chosen family + size + weight + style.
    const DWRITE_FONT_WEIGHT weight = (DWRITE_FONT_WEIGHT)std::clamp(props.fontWeight, 100, 900);
    const DWRITE_FONT_STYLE  style  = props.italic
                                         ? DWRITE_FONT_STYLE_ITALIC
                                         : DWRITE_FONT_STYLE_NORMAL;

    // We need the actual family name for CreateTextFormat (it looks it up
    // by string, not index — quirk of the API). Pull it back out.
    std::wstring actualFamily;
    {
        ComPtr<IDWriteFontFamily> fam;
        if (SUCCEEDED(collection->GetFontFamily(famIdx, fam.GetAddressOf()))) {
            ComPtr<IDWriteLocalizedStrings> names;
            if (SUCCEEDED(fam->GetFamilyNames(names.GetAddressOf())) && names->GetCount() > 0) {
                UINT32 len = 0;
                if (SUCCEEDED(names->GetStringLength(0, &len)) && len > 0) {
                    actualFamily.resize(len + 1);
                    if (SUCCEEDED(names->GetString(0, actualFamily.data(), (UINT32)actualFamily.size()))) {
                        actualFamily.resize(len);
                    } else {
                        actualFamily.clear();
                    }
                }
            }
        }
    }
    if (actualFamily.empty()) actualFamily = L"Segoe UI";

    ComPtr<IDWriteTextFormat> format;
    HRESULT hr = factory_->CreateTextFormat(
        actualFamily.c_str(),
        collection.Get(),
        weight, style, DWRITE_FONT_STRETCH_NORMAL,
        props.fontSize,
        L"en-us",
        format.GetAddressOf());
    if (FAILED(hr) || !format) return false;

    switch (props.alignment) {
        case 1: format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);   break;
        case 2: format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING); break;
        default: format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); break;
    }

    // Measure first with a very wide layout box so we know the true bounds.
    // Then create the final layout at the measured size so DrawTextLayout
    // fills the bitmap exactly. Add ~4px padding on each side so glyph
    // overhang (italics, script hooks) doesn't clip.
    const float kPad = 4.0f;
    ComPtr<IDWriteTextLayout> layout;
    hr = factory_->CreateTextLayout(wtext.c_str(), (UINT32)wtext.size(),
                                     format.Get(),
                                     4096.0f, 4096.0f,
                                     layout.GetAddressOf());
    if (FAILED(hr) || !layout) return false;

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return false;

    const int wPix = std::max(1, (int)std::ceil(metrics.widthIncludingTrailingWhitespace + 2 * kPad));
    const int hPix = std::max(1, (int)std::ceil(metrics.height + 2 * kPad));

    // Resize the bitmap-render-target if the string doesn't fit. IDWrite
    // returns E_INVALIDARG on shrink attempts; we only ever grow.
    if (wPix > bitmapCurW_ || hPix > bitmapCurH_) {
        const UINT newW = (UINT)std::max(wPix, bitmapCurW_);
        const UINT newH = (UINT)std::max(hPix, bitmapCurH_);
        if (FAILED(bitmapTarget_->Resize(newW, newH))) return false;
        bitmapCurW_ = (int)newW;
        bitmapCurH_ = (int)newH;
    }

    // Clear the bitmap area we're about to draw into (GDI DIB doesn't
    // zero itself between draws). Get the HDC + underlying HBITMAP,
    // BitBlt-fill the region with black, then let DrawTextLayout render
    // grayscale coverage on top.
    HDC hdc = bitmapTarget_->GetMemoryDC();
    if (!hdc) return false;

    // Grab the DIB backing to zero it. GetCurrentObject(hdc, OBJ_BITMAP)
    // gives us the HBITMAP; then GetDIBits with lpvBits=nullptr fills
    // BITMAPINFO for size query — but for a FAST clear we just PatBlt
    // black over the used sub-rect.
    RECT clearRc = { 0, 0, wPix, hPix };
    HBRUSH blackBrush = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdc, &clearRc, blackBrush);

    // Draw the text layout at (kPad, kPad) with white color; grayscale AA
    // gives us a black->white coverage ramp we can lift as R8.
    hr = bitmapTarget_->DrawTextLayout(kPad, kPad,
                                        layout.Get(),
                                        renderParams_.Get(),
                                        RGB(255, 255, 255));
    if (FAILED(hr)) return false;

    // Extract the DIB backing pixels. GetCurrentObject on the HDC returns
    // the HBITMAP; GetObject fills a BITMAP struct with a pointer to the
    // pixel bits (bmBits) because IDWriteBitmapRenderTarget wraps a
    // DIBSection which is directly memory-mapped.
    HBITMAP hbmp = (HBITMAP)GetCurrentObject(hdc, OBJ_BITMAP);
    if (!hbmp) return false;
    DIBSECTION dib{};
    if (GetObject(hbmp, sizeof(dib), &dib) == 0 || dib.dsBm.bmBits == nullptr) {
        return false;
    }
    // The DIB is 32-bit BGRA. We copy the RED channel (which equals G
    // and B under grayscale mode) into a compact R8 buffer sized to the
    // string's bounding box.
    const int bmpStride = dib.dsBm.bmWidthBytes; // typically 4 * bmWidth
    const unsigned char* srcBase = (const unsigned char*)dib.dsBm.bmBits;
    outPixels.assign((size_t)wPix * (size_t)hPix, 0);
    for (int y = 0; y < hPix; ++y) {
        const unsigned char* srcRow = srcBase + (size_t)y * (size_t)bmpStride;
        unsigned char*       dstRow = outPixels.data() + (size_t)y * (size_t)wPix;
        for (int x = 0; x < wPix; ++x) {
            // BGRA: index 2 is R, but under grayscale R==G==B, so it doesn't
            // matter which we pick. Use index 0 (B) to save one add. All
            // three are equal.
            dstRow[x] = srcRow[x * 4 + 0];
        }
    }
    outW = wPix;
    outH = hPix;
    return true;
}

// =============================================================================
// GPU upload
// =============================================================================
bool TextRenderer::UploadR8Texture(ID3D11Device* device,
                                    const unsigned char* pixels, int w, int h,
                                    ComPtr<ID3D11Texture2D>& outTex,
                                    ComPtr<ID3D11ShaderResourceView>& outSRV) {
    if (!device || !pixels || w <= 0 || h <= 0) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width          = (UINT)w;
    td.Height         = (UINT)h;
    td.MipLevels      = 1;
    td.ArraySize      = 1;
    td.Format         = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage          = D3D11_USAGE_IMMUTABLE;
    td.BindFlags      = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem     = pixels;
    init.SysMemPitch = (UINT)w;   // R8 = 1 byte per pixel, no padding

    // Reset any prior contents. ComPtr::Reset releases the old ref.
    outTex.Reset();
    outSRV.Reset();

    HRESULT hr = device->CreateTexture2D(&td, &init, outTex.GetAddressOf());
    if (FAILED(hr) || !outTex) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format              = DXGI_FORMAT_R8_UNORM;
    sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(outTex.Get(), &sd, outSRV.GetAddressOf());
    if (FAILED(hr) || !outSRV) {
        outTex.Reset();
        return false;
    }
    return true;
}

// =============================================================================
// The one method the render loop actually calls
// =============================================================================
bool TextRenderer::EnsureLayerCache(Layer& layer, ID3D11Device* device) {
    if (!device || !IsReady()) return false;
    const std::size_t key = ComputeCacheKey(layer.textProps);
    if (layer.textSRV && layer.textCacheKey == key) return true; // fast path

    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (!RasterizeString(layer.textProps, pixels, w, h)) {
        layer.textTex.Reset();
        layer.textSRV.Reset();
        layer.textTexW    = 0;
        layer.textTexH    = 0;
        // Leave cacheKey stale so a future prop change (or a retry) fires
        // another RasterizeString attempt.
        return false;
    }
    if (!UploadR8Texture(device, pixels.data(), w, h,
                         layer.textTex, layer.textSRV)) {
        layer.textTex.Reset();
        layer.textSRV.Reset();
        layer.textTexW = 0;
        layer.textTexH = 0;
        return false;
    }
    layer.textTexW     = w;
    layer.textTexH     = h;
    layer.textCacheKey = key;
    return true;
}
