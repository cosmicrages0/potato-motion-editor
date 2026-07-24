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

#include "Effect.h"

// -----------------------------------------------------------------------------
// EffectManager: owns compiled shader blobs, ping-pong render targets, a
// shared constant buffer, a fullscreen-triangle VB, and knows how to run a
// chain of Effects against a source texture, producing a destination texture.
//
// Memory strategy (see PROJECT_BRIEFING.md Section 9.5, potato-PC constraint):
//   * EXACTLY TWO render targets at composition resolution, ping-ponged.
//     Cost = 2 * (W * H * 4) bytes ~= 16 MB @ 1080p regardless of layer count.
//     If we instead allocated one RT per layer, a 30-layer scene at 1080p
//     would use ~250 MB VRAM — hard fail on integrated Intel HD.
//   * All shaders compiled once in Initialize(). If compilation fails, we
//     fall back to a pass-through shader so the editor NEVER crashes on a
//     shader bug.
//   * Constant buffer is a single 64-byte upload per Effect, reused across
//     the whole frame.
//
// Task 5 scope note:
//   For Task 5 we ship the pipeline against the swap-chain back buffer as a
//   post-comp effect (per-composition effects, not per-layer). Per-layer
//   effect application requires each layer to render into a texture rather
//   than directly into an ImDrawList. That layer-texture pipeline is
//   scheduled for the deferred Task 5.0 Usability Pass.
// -----------------------------------------------------------------------------

class EffectManager {
public:
    EffectManager();
    ~EffectManager();

    // Compile every built-in effect shader, create the ping-pong RTs, the
    // constant buffer, the fullscreen triangle VB, and the linear sampler.
    // Returns false only on unrecoverable init errors (e.g. no d3dcompiler).
    // Shader compile errors are non-fatal: the offending effect gets the
    // passthrough shader as a fallback.
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                    UINT width, UINT height);

    // Resize the internal ping-pong RTs. Called from RenderEngine when the
    // main window resizes or when the user picks a new composition resolution.
    void Resize(UINT width, UINT height);

    void Shutdown();

    // Apply an ordered chain of effects to a source SRV, writing the final
    // composited result into `destinationRTV`.  If `effects` is empty, this
    // performs a passthrough copy (still useful to establish the frame).
    // Returns false if the pipeline is not initialised or if `effects` is
    // null/oversized (defensive).
    bool ApplyChain(ID3D11ShaderResourceView* sourceSRV,
                    ID3D11RenderTargetView*   destinationRTV,
                    const std::vector<Effect>& effects);

    // Task 5.13: composite an SRV over an RTV via SRC_ALPHA / INV_SRC_ALPHA
    // blend. Used by the per-layer isolation dispatch to blit a filtered
    // layer (from pingSRV) over the shared compRTV. Reads the source at
    // full RT size — caller must ensure the source and destination have
    // the same dims (they always do when both are our ping/pong RTs).
    // Returns false if not initialized or if either handle is null.
    bool CompositeSRVOver(ID3D11ShaderResourceView* srcSRV,
                          ID3D11RenderTargetView*   dstRTV);

    // Direct access for the RenderEngine to blit intermediate results into
    // its own render target if it needs to. Both may be null before Initialize.
    ID3D11ShaderResourceView* GetPingSRV()   const { return ping_srv_; }
    ID3D11RenderTargetView*   GetPingRTV()   const { return ping_rtv_; }
    ID3D11ShaderResourceView* GetPongSRV()   const { return pong_srv_; }
    ID3D11RenderTargetView*   GetPongRTV()   const { return pong_rtv_; }

    bool IsReady() const { return initialized_; }
    UINT Width()   const { return width_; }
    UINT Height()  const { return height_; }

private:
    bool CreateRenderTargets(UINT width, UINT height);
    void ReleaseRenderTargets();

    // Compile a single pixel shader from an HLSL source string. On failure
    // the returned pointer is null; the caller substitutes the passthrough PS.
    ID3D11PixelShader* CompilePS(const char* hlslSrc, const char* entry);

    // Bind a passthrough VS + given PS + shared VB/sampler and draw the
    // fullscreen triangle. Assumes the RTV/SRV binding is done by the caller.
    void DrawFullscreenPass(ID3D11PixelShader* ps,
                            ID3D11ShaderResourceView* srcSRV);

    // Non-owning
    ID3D11Device*        device_  = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    // Ping-pong render targets (owned)
    ID3D11Texture2D*         ping_tex_ = nullptr;
    ID3D11RenderTargetView*  ping_rtv_ = nullptr;
    ID3D11ShaderResourceView* ping_srv_ = nullptr;
    ID3D11Texture2D*         pong_tex_ = nullptr;
    ID3D11RenderTargetView*  pong_rtv_ = nullptr;
    ID3D11ShaderResourceView* pong_srv_ = nullptr;

    // Shared pipeline objects (owned)
    ID3D11VertexShader*   vs_fullscreen_ = nullptr;
    ID3D11InputLayout*    input_layout_  = nullptr;
    ID3D11Buffer*         vb_fullscreen_ = nullptr;
    ID3D11Buffer*         cb_effect_     = nullptr; // 64 bytes, EffectParams
    ID3D11SamplerState*   linear_clamp_  = nullptr;
    ID3D11BlendState*     blend_normal_  = nullptr;
    // Task 5.13-fix: opaque REPLACE blend state (BlendEnable=FALSE).
    // Bound at the top of ApplyChain so every effect pass overwrites the
    // pool RT pixel-for-pixel, ignoring whatever alpha state the shape
    // rasterizer left behind. Without this, effect passes inherit the
    // SRC_ALPHA/INV_SRC_ALPHA state from CompositionRenderer and blend
    // their output against uninitialized-or-stale pong contents, causing
    // filtered layers to vanish or show ghosts of previous layers.
    ID3D11BlendState*     blend_replace_ = nullptr;

    // One pixel shader per EffectType. Populated in Initialize().
    // Index = (int)EffectType. Slot may be null if compile failed — in that
    // case the passthrough shader is used instead. Zero-init via {} scales
    // automatically when EffectType::COUNT grows.
    ID3D11PixelShader* effect_ps_[(int)EffectType::COUNT] = {};
    ID3D11PixelShader* ps_passthrough_ = nullptr;

    // Task 5.13: extra shaders for the per-layer isolation path.
    //   ps_composite_ — plain SRV-over-RTV blit via alpha blend. Used to
    //                   composite an isolated layer's post-effect result
    //                   (in pingRTV) over the shared compRTV.
    //   ps_dropshadow_offset_    — samples source with an offset+blur,
    //                              tints by shadow color+opacity, writes
    //                              a colored shadow into the destination.
    //   ps_dropshadow_composite_ — samples TWO SRVs (blurred shadow at t0,
    //                              original source at t1) and composites
    //                              source-over-shadow into the destination.
    ID3D11PixelShader* ps_composite_             = nullptr;
    ID3D11PixelShader* ps_dropshadow_offset_     = nullptr;
    ID3D11PixelShader* ps_dropshadow_composite_  = nullptr;

    UINT width_       = 0;
    UINT height_      = 0;
    bool initialized_ = false;
};
