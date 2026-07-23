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

    // Render every visible 2D layer into `targetRTV` at (targetW x targetH).
    // 3D layers and Camera / Null special types are skipped here (they have
    // their own paths in RenderEngine). Layers are drawn in timeline order so
    // later layers appear on top.
    // Clears the target to the background color first.
    void RenderLayers(ID3D11RenderTargetView* targetRTV,
                      UINT targetW, UINT targetH,
                      LayerManager& layerManager,
                      const float bgColor[4]);

private:
    bool CreateShaders();
    bool CreateBuffers();
    void ReleaseAll();

    // Upload the given constant-buffer contents for one shape and issue one draw.
    // Task 5.7: DrawRect and DrawEllipse now share ps_shape_sdf_ and take
    // stroke color / stroke width / corner radius. Passing zero for the two
    // scalars reproduces the pre-5.7 hard-edged unstroked look.
    void DrawRect   (const Mat3& worldMatrix, const Vec2& size,
                     unsigned int fillColor, unsigned int strokeColor,
                     float strokeWidth, float cornerRadius,
                     UINT targetW, UINT targetH);
    void DrawEllipse(const Mat3& worldMatrix, const Vec2& size,
                     unsigned int fillColor, unsigned int strokeColor,
                     float strokeWidth,
                     UINT targetW, UINT targetH);
    void DrawNullMarker(const Mat3& worldMatrix, const Vec2& size,
                        UINT targetW, UINT targetH);

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
    ID3D11InputLayout*    layout_      = nullptr;
    ID3D11Buffer*         cb_shape_    = nullptr;   // dynamic, sizeof(ShapeCB)
    ID3D11Buffer*         vb_quad_     = nullptr;   // 4 verts covering [-0.5,+0.5]
    ID3D11Buffer*         ib_quad_     = nullptr;   // 6 indices (two tris)
    ID3D11BlendState*     blend_alpha_ = nullptr;

    bool initialized_ = false;
};
