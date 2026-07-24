#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <vector>

#include "Layer.h"
#include "LayerManager.h"
#include "MathTypes.h"

// -----------------------------------------------------------------------------
// CompositionRenderer (Task 5.0)
//
// The missing piece that turns Tasks 5+6 from "wired but not connected" into
// something users can see. Rasterizes every visible 2D layer into a fixed-size
// composition texture (1920x1080 by default) using a minimal DX11 pipeline:
//   * per-quad transform matrix uploaded via constant buffer
//   * one draw call per shape (potato-PC scale: a few dozen shapes tops)
//   * writes to a caller-owned RTV so RenderEngine can then run the
//     EffectManager ping-pong chain against the result
//
// Why not ImGui? ImDrawList only draws into an ImGui window; we need a real
// offscreen render target so shaders can filter it AND ExportEngine can copy
// it into its staging texture. That means talking to DX11 directly.
//
// Why not per-layer render targets? Same reason EffectManager uses shared
// ping-pong: allocating one RT per layer would burn 33MB * N layers of VRAM.
// This module writes all layers to a single caller-supplied RTV in Z order.
//
// Ellipses are drawn as 48-sample triangle fans built on the CPU each frame
// into a small vertex buffer (uploaded via Map). For potato PCs with <100
// shapes this cost is negligible; if we ever go >1000 shapes we'll switch to
// instanced draws.
// -----------------------------------------------------------------------------

class CompositionRenderer {
public:
    CompositionRenderer();
    ~CompositionRenderer();

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();
    bool IsReady() const { return initialized_; }

    // Render every visible 2D layer into `targetRTV`.
    // 3D layers and Camera / Null special types are skipped here (they have
    // their own paths in RenderEngine). Layers are drawn in timeline order so
    // later layers appear on top.
    // Clears the target to the background color first.
    //
    // Task 5.8: split of the old single `targetW/H` into two roles:
    //   * targetW/H  = D3D11 viewport dims (== the RT's actual pixel count).
    //                  Drives how many pixels the rasterizer fills.
    //   * logicalW/H = "composition dims" used inside BuildShapeMVP to map
    //                  shape positions (in comp pixels) into NDC. Passing
    //                  logicalW = compositionWidth (regardless of RT size)
    //                  makes shape coordinates stable under preview
    //                  downscaling: rasterizer scales to whatever pixel
    //                  count the RT has, but the NDC layout stays comp-
    //                  correct.
    // Backward-compat: when logicalW/H are 0 they default to targetW/H so
    // every pre-5.8 caller keeps rendering exactly as before.
    // Task 5.10: added `compDuration` param so the per-layer visibility
    // gate can resolve the `outPoint = -1` sentinel to "extends to comp
    // end". Defaults to 0.0f for source-compat with pre-5.10 callers:
    // then any negative sentinel is treated as "always visible" (also
    // reproduces pre-5.10 behavior — the layer just draws every frame).
    void RenderLayers(ID3D11RenderTargetView* targetRTV,
                      UINT targetW, UINT targetH,
                      LayerManager& layerManager,
                      const float bgColor[4],
                      UINT logicalW = 0, UINT logicalH = 0,
                      float compDuration = 0.0f);

private:
    bool CreateShaders();
    bool CreateBuffers();
    void ReleaseAll();

    // Upload the given constant-buffer contents for one shape and issue one draw.
    // Task 5.7: DrawRect and DrawEllipse now share ps_shape_sdf_ and take
    // stroke color / stroke width / corner radius. Passing zero for the two
    // scalars reproduces the pre-5.7 hard-edged unstroked look.
    // Task 5.8: draw helpers take logicalW/H (drive MVP) separately from
    // targetW/H (drive viewport). All internal callers now come from
    // RenderLayers which resolves the two dim pairs once at the top.
    void DrawRect   (const Mat3& worldMatrix, const Vec2& size,
                     unsigned int fillColor, unsigned int strokeColor,
                     float strokeWidth, float cornerRadius,
                     UINT logicalW, UINT logicalH);
    void DrawEllipse(const Mat3& worldMatrix, const Vec2& size,
                     unsigned int fillColor, unsigned int strokeColor,
                     float strokeWidth,
                     UINT logicalW, UINT logicalH);
    void DrawNullMarker(const Mat3& worldMatrix, const Vec2& size,
                        UINT logicalW, UINT logicalH);
    // Task 5.9: text sprite. Samples layer's cached R8 atlas via SRV, tints
    // by fillColor. Size is the R8 atlas dims in pixels (drives the quad's
    // world-space extent); world matrix places it in comp space just like
    // any other shape.
    //
    // Task 5.11-fix: optional stroke via 8-sample outline dilation in
    // ps_text_. Pass strokeWidth = 0 to reproduce the pre-fix fill-only
    // behavior; anything > 0 draws a stroke ring of `strokeColor` around
    // every glyph edge, `strokeWidth` pixels thick.
    void DrawText(const Mat3& worldMatrix, const Vec2& size,
                  unsigned int fillColor, unsigned int strokeColor,
                  float strokeWidth,
                  ID3D11ShaderResourceView* atlasSRV,
                  UINT logicalW, UINT logicalH);

    // Constant buffer layout (16-byte aligned; must match HLSL).
    // Task 5.7: added stroke[4] + params2[4] for stroke color + halfExtent.
    // params layout: (shape_type, strokeWidth_px, cornerRadius_px, unused)
    // params2 layout: (halfExtentX_px, halfExtentY_px, unused, unused)
    struct ShapeCB {
        float mvp[16];      // 4x4 row-major MVP for this shape (64 B)
        float color[4];     // fill RGBA 0..1
        float stroke[4];    // stroke RGBA 0..1  (Task 5.7)
        float params[4];    // (type, strokeWidth, cornerRadius, unused)
        float params2[4];   // (halfExtentX, halfExtentY, unused, unused)  (Task 5.7)
    };
    static_assert(sizeof(ShapeCB) == (16 + 4 + 4 + 4 + 4) * 4, "ShapeCB size drift");

    // Non-owning
    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // Owned pipeline state
    // Task 5.7: ps_shape_sdf_ replaces the split ps_rect_ / ps_ellipse_.
    // One SDF-based pixel shader now handles fill + stroke + rounded corners
    // for both rect (params.x=0) and ellipse (params.x=1). The Null marker
    // keeps its own shader — it's an editor-only glyph, not a shape.
    ID3D11VertexShader*   vs_             = nullptr;
    ID3D11PixelShader*    ps_shape_sdf_   = nullptr;
    ID3D11PixelShader*    ps_null_        = nullptr;
    // Task 5.9: text pixel shader + linear sampler. Text sprites are cached
    // R8_UNORM atlases uploaded by TextRenderer; this PS samples .r as
    // coverage and tints by the fill color.
    ID3D11PixelShader*    ps_text_        = nullptr;
    ID3D11SamplerState*   samp_linear_    = nullptr;
    ID3D11InputLayout*    layout_      = nullptr;
    ID3D11Buffer*         cb_shape_    = nullptr;   // dynamic, sizeof(ShapeCB)
    ID3D11Buffer*         vb_quad_     = nullptr;   // 4 verts covering [-0.5,+0.5]
    ID3D11Buffer*         ib_quad_     = nullptr;   // 6 indices (two tris)
    ID3D11BlendState*     blend_alpha_ = nullptr;
    // Task 5.10: per-layer blend states, indexed by (int)BlendMode.
    // Precomputed at Initialize; released in ReleaseAll. Table lookup
    // at draw time: OMSetBlendState(blend_modes_[(int)layer.blend]).
    // Falls back to blend_alpha_ (Normal) for out-of-range values.
    static constexpr int kBlendModeCount = 6; // matches Effect.h enum size
    ID3D11BlendState*     blend_modes_[kBlendModeCount] = {};

    bool initialized_ = false;
};
