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
    void DrawRect   (const Mat3& worldMatrix, const Vec2& size, unsigned int color,
                     UINT targetW, UINT targetH);
    void DrawEllipse(const Mat3& worldMatrix, const Vec2& size, unsigned int color,
                     UINT targetW, UINT targetH);
    void DrawNullMarker(const Mat3& worldMatrix, const Vec2& size,
                        UINT targetW, UINT targetH);

    // Constant buffer layout (16-byte aligned; must match HLSL):
    struct ShapeCB {
        float mvp[16];   // 4x4 row-major MVP for this shape
        float color[4];  // RGBA in 0..1
        float params[4]; // .x = shape type hint (0=rect, 1=ellipse), .y/z/w reserved
    };
    static_assert(sizeof(ShapeCB) == (16 + 4 + 4) * 4, "ShapeCB size drift");

    // Non-owning
    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // Owned pipeline state
    ID3D11VertexShader*   vs_          = nullptr;
    ID3D11PixelShader*    ps_rect_     = nullptr;
    ID3D11PixelShader*    ps_ellipse_  = nullptr;
    ID3D11PixelShader*    ps_null_     = nullptr;
    ID3D11InputLayout*    layout_      = nullptr;
    ID3D11Buffer*         cb_shape_    = nullptr;   // dynamic, sizeof(ShapeCB)
    ID3D11Buffer*         vb_quad_     = nullptr;   // 4 verts covering [-0.5,+0.5]
    ID3D11Buffer*         ib_quad_     = nullptr;   // 6 indices (two tris)
    ID3D11BlendState*     blend_alpha_ = nullptr;

    bool initialized_ = false;
};
