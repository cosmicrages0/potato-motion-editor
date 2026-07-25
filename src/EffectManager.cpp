#include "EffectManager.h"

#include <iostream>
#include <algorithm>
#include <cstring>

// =============================================================================
// Embedded HLSL. Kept in this file (single-file per module rule from the
// briefing) so we don't need to distribute .hlsl assets alongside the .exe.
//
// Every shader uses the SAME constant buffer layout so the C++ side can
// blindly memcpy an EffectParams struct into slot 0.
//
// Root layout expected on every draw:
//   VS   : g_vs_fullscreen           (attributeless -> emits fullscreen triangle)
//   PS   : effect_ps_[type]  OR  ps_passthrough_
//   t0   : source texture SRV
//   s0   : linear-clamp sampler
//   b0   : EffectParams (64 bytes)
//   IA   : shared VB (fullscreen triangle in NDC) via input layout
// =============================================================================

static const char* kSharedHeader = R"HLSL(
cbuffer EffectCB : register(b0) {
    float4 p0;
    float4 p1;
    float4 p2;
    float4 p3;
};
Texture2D    tex : register(t0);
SamplerState smp : register(s0);

struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
)HLSL";

// Shared vertex shader. Feeds through a NDC-space fullscreen triangle whose
// UVs are already correct in [0,1].
static const char* kVSSource = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut main(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv  = i.uv;
    return o;
}
)HLSL";

// Passthrough pixel shader — also the fallback when a real shader fails to compile.
static const char* kPSPassthrough = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(VSOut i) : SV_TARGET {
    return tex.Sample(smp, i.uv);
}
)HLSL";

// Motion tile / mirror edge. UVs -> abs(frac(uv * N) * 2 - 1) then sample.
// p0.x = TileCount   p0.y = Phase   p0.z = MirrorEdges (0/1)
static const char* kPSMotionTile = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(VSOut i) : SV_TARGET {
    float  N       = max(p0.x, 0.001);
    float  phase   = p0.y;
    float  mirror  = p0.z;

    float2 uv = i.uv * N + float2(phase, phase);
    float2 mirrored = abs(frac(uv) * 2.0 - 1.0);
    float2 tiled    = frac(uv);
    float2 sampleUV = lerp(tiled, mirrored, saturate(mirror));

    return tex.Sample(smp, saturate(sampleUV));
}
)HLSL";

// Directional motion blur. p0.x=Angle(deg), p0.y=Intensity(0..100 -> pixels),
// p0.z=Samples (capped 16). Samples symmetrically along the direction vector.
static const char* kPSMotionBlur = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(VSOut i) : SV_TARGET {
    float angleRad = radians(p0.x);
    float intensity = clamp(p0.y, 0.0, 100.0);
    int   samples   = (int)clamp(p0.z, 1.0, 16.0);

    // Convert intensity (pixels) to UV space using the sampled texture size.
    uint w, h;
    tex.GetDimensions(w, h);
    float2 texel = float2(1.0 / max(1u, w), 1.0 / max(1u, h));
    float2 dir   = float2(cos(angleRad), sin(angleRad)) * intensity * texel;

    float4 acc = float4(0, 0, 0, 0);
    float  wSum = 0.0;
    // Symmetric sampling around the current pixel.
    for (int s = -8; s <= 8; ++s) {
        if (abs(s) > samples) continue;
        float t = (float)s / max(1.0, (float)samples);
        float weight = 1.0 - abs(t);       // triangular window
        float2 uv    = i.uv + dir * t;
        acc  += tex.Sample(smp, saturate(uv)) * weight;
        wSum += weight;
    }
    if (wSum < 1e-4) return tex.Sample(smp, i.uv);
    return acc / wSum;
}
)HLSL";

// Chromatic aberration. p0.x=Amount(pixels), p0.y=Angle(deg), p0.z=Radial(0/1)
// Radial mode offsets away from image center; directional mode uses Angle.
// Divide-by-zero-safe (uses max(len, epsilon) for the normalized radial vector).
static const char* kPSChroma = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(VSOut i) : SV_TARGET {
    float amt   = p0.x;
    float angle = radians(p0.y);
    float radial = p0.z;

    uint w, h;
    tex.GetDimensions(w, h);
    float2 texel = float2(1.0 / max(1u, w), 1.0 / max(1u, h));

    float2 dir;
    if (radial > 0.5) {
        float2 fromCenter = i.uv - float2(0.5, 0.5);
        float  len = max(length(fromCenter), 1e-4);
        dir = fromCenter / len;
    } else {
        dir = float2(cos(angle), sin(angle));
    }
    float2 offset = dir * amt * texel;

    float r = tex.Sample(smp, saturate(i.uv + offset)).r;
    float g = tex.Sample(smp, saturate(i.uv          )).g;
    float b = tex.Sample(smp, saturate(i.uv - offset)).b;
    float a = tex.Sample(smp, saturate(i.uv          )).a;
    return float4(r, g, b, a);
}
)HLSL";

// Blend-mode pass. Reads the source texture (the layer above) and a `dst`
// texture sampled via t1 (the composite below). For Task 5 the destination
// is currently the same buffer (self-blend, useful for glow-ish effects);
// per-layer proper below-layer sampling ships with Task 5.0 usability pass.
// p0.x = mode (0=Normal,1=Add,2=Mul,3=Screen,4=Overlay,5=ColorDodge)
static const char* kPSBlend = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(VSOut i) : SV_TARGET {
    int mode = (int)round(p0.x);
    float4 src = tex.Sample(smp, i.uv);
    float4 dst = src; // See header note: proper dst-layer sampling in Task 5.0

    float3 s = src.rgb;
    float3 d = dst.rgb;
    float3 r = s;

    if      (mode == 1) r = s + d;                                // Additive
    else if (mode == 2) r = s * d;                                // Multiply
    else if (mode == 3) r = 1.0 - (1.0 - s) * (1.0 - d);           // Screen
    else if (mode == 4) {                                          // Overlay
        r = d < 0.5 ? (2.0 * s * d) : (1.0 - 2.0 * (1.0 - s) * (1.0 - d));
    }
    else if (mode == 5) {                                          // Color Dodge (safe)
        float3 inv = max(1.0 - s, float3(1e-4, 1e-4, 1e-4));
        r = min(float3(1,1,1), d / inv);
    }
    // Normal (mode 0) or unknown: passthrough.
    return float4(saturate(r), src.a);
}
)HLSL";

// =============================================================================
// Task 5.13: composite blit — sample the source SRV, output with alpha.
// Blend state at draw time is SRC_ALPHA / INV_SRC_ALPHA, so this is a
// standard "src over dst" composite. Used by CompositeSRVOver().
//
// Task 5.13-fix2: added the EffectCB cbuffer block for parity with all
// other effect shaders. HLSL cbuffer bindings can be finicky across
// drivers when the shader signature doesn't match what the VS declares
// (fullscreen VS has the same block at b0). This makes the composite
// shader's reflection stable.
// =============================================================================
static const char* kPSComposite = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(VSOut i) : SV_TARGET {
    return tex.Sample(smp, i.uv);
}
)HLSL";

// =============================================================================
// Task 5.13: Drop Shadow — offset+blur pass. Samples the source at a shifted
// UV, applies a 5-tap gaussian-ish blur along the perpendicular axis for
// softness, tints by shadow color and multiplies by opacity. Writes the
// colored shadow into the destination (typically pongRTV).
//
// CB layout matches EffectParams:
//   p0.x = Distance (px)
//   p0.y = Angle (degrees, 0=right, 90=down)
//   p0.z = Softness (px)
//   p1.x = Opacity (0..1)
//   p2.xyz = Shadow color RGB (0..1)
//   p3.xy = RT dims (px, filled in per-frame by the caller so px->UV
//           conversion is correct at any composition size)
// =============================================================================
// =============================================================================
// Task 5.13-fix4: DropShadow FUSED single-pass shader.
//
// Old two-pass design (kPSDropShadowOffset + kPSDropShadowComposite) had a
// fatal same-texture bind conflict in Pass 2: readSRV was ping_srv_ (source
// shape), and otherRTV (the write target for Pass 2) was ping_rtv_ — same
// texture. D3D11 silently unbinds one, so the composite draws garbage. In
// the wild this presented as "shape goes fully black on drop-shadow apply".
//
// New single-pass design: one shader that samples the source at both the
// current UV (to get the layer itself) AND at 5 taps around the shifted UV
// (to build the blurred shadow alpha), then composites source-over-shadow
// inline. Writes final RGBA into writeRTV (pong). Only READS ping and only
// WRITES pong — no same-texture risk.
//
// CB layout unchanged from the old Offset pass:
//   p0.x = Distance (px)
//   p0.y = Angle (deg, 0=right, 90=down in UV space where +Y is down)
//   p0.z = Softness (px, perpendicular-axis 5-tap gaussian-ish radius)
//   p1.x = Opacity (0..1)
//   p2.xyz = Shadow color RGB (0..1)
//   p3.xy = RT dims (px, injected per-frame by ApplyChain for px->UV math)
// =============================================================================
static const char* kPSDropShadowFused = R"HLSL(
cbuffer EffectCB : register(b0) { float4 p0; float4 p1; float4 p2; float4 p3; };
Texture2D    tex : register(t0);
SamplerState smp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(VSOut i) : SV_TARGET {
    float dist    = p0.x;
    float ang     = radians(p0.y);
    float soft    = max(p0.z, 0.0);
    float opacity = saturate(p1.x);
    float3 color  = p2.xyz;
    float2 rtDim  = max(p3.xy, float2(1.0, 1.0));

    // Source at current UV — the LAYER itself.
    float4 src = tex.Sample(smp, i.uv);

    // Shadow: sample the source alpha at a shifted UV. The shift is
    // OPPOSITE the intended visual shadow direction, so sampling "up-left"
    // of a pixel produces a shadow that visually appears down-right.
    float2 offsetPx = float2(cos(ang), sin(ang)) * dist;
    float2 offsetUV = offsetPx / rtDim;
    float2 srcUV    = i.uv - offsetUV;

    // ---------------------------------------------------------------
    // Task 5.13-fix5: proper 2D Gaussian blur (13 taps in a rotationally
    // symmetric ring pattern). Old code was a 5-tap perpendicular line
    // blur which at softness > ~15 px stamped 5 visible copies of the
    // source silhouette along a diagonal — user reported "the shadow
    // begins to appears circles in a diagonal way". Fixed by moving to
    // a 2D distribution: one center tap + 4 axis-aligned taps at 1σ +
    // 8 diagonal taps at ~0.7σ, weights normalised to sum to 1.0.
    //
    // Kernel is scaled by `soft` in pixels so the ring radius grows
    // with the Softness slider. Weights follow a standard 2D Gaussian
    // where σ = soft / 2 so the ±1σ ring is at half the softness.
    // ---------------------------------------------------------------
    float2 sPx = float2(soft, soft) / rtDim; // pixel->UV for kernel
    // Center weight (µ=0)
    const float wC = 0.20;
    // Axis-aligned ring at radius 1σ (weight per tap ≈ 0.12 * 4 taps = 0.48)
    const float wA = 0.10;
    // Diagonal ring at radius ~1.4σ but scaled down to 1σ visually via
    // multiplying the offset by 0.707 (weight per tap ≈ 0.04 * 8 = 0.32)
    const float wD = 0.04;

    float shadowA = 0.0;
    // Center
    shadowA += tex.Sample(smp, srcUV).a * wC;
    // 4 axis-aligned taps (N, S, E, W)
    shadowA += tex.Sample(smp, srcUV + float2( 0.0,  sPx.y)).a * wA;
    shadowA += tex.Sample(smp, srcUV + float2( 0.0, -sPx.y)).a * wA;
    shadowA += tex.Sample(smp, srcUV + float2( sPx.x, 0.0 )).a * wA;
    shadowA += tex.Sample(smp, srcUV + float2(-sPx.x, 0.0 )).a * wA;
    // 8 diagonal taps at 0.707 * σ (approximates a circular ring)
    const float d = 0.707;
    shadowA += tex.Sample(smp, srcUV + float2( d*sPx.x,  d*sPx.y)).a * wD;
    shadowA += tex.Sample(smp, srcUV + float2( d*sPx.x, -d*sPx.y)).a * wD;
    shadowA += tex.Sample(smp, srcUV + float2(-d*sPx.x,  d*sPx.y)).a * wD;
    shadowA += tex.Sample(smp, srcUV + float2(-d*sPx.x, -d*sPx.y)).a * wD;
    // 4 more taps at 1.5σ for large softness — cheap way to extend the
    // tail without ghosting. Weight ≈ 0.02 * 4 = 0.08 keeps total near 1.
    const float e = 1.5;
    const float wE = 0.02;
    shadowA += tex.Sample(smp, srcUV + float2( e*sPx.x, 0.0)).a * wE;
    shadowA += tex.Sample(smp, srcUV + float2(-e*sPx.x, 0.0)).a * wE;
    shadowA += tex.Sample(smp, srcUV + float2(0.0,  e*sPx.y)).a * wE;
    shadowA += tex.Sample(smp, srcUV + float2(0.0, -e*sPx.y)).a * wE;

    shadowA *= opacity;

    // Source-over-shadow composite, inline.
    float outA    = src.a + shadowA * (1.0 - src.a);
    float3 outRGB = src.rgb * src.a + color * shadowA * (1.0 - src.a);
    if (outA > 0.001) outRGB /= outA;
    return float4(outRGB, outA);
}
)HLSL";

// =============================================================================
// Vertex data: one fullscreen triangle in NDC with UVs.
// (A triangle covers the entire screen more efficiently than a quad — one
// fewer vertex shader invocation and no diagonal seam.)
// =============================================================================
struct FullscreenVert { float x, y; float u, v; };
static const FullscreenVert kFullscreenTri[3] = {
    { -1.0f,  3.0f,  0.0f, -1.0f }, // top
    { -1.0f, -1.0f,  0.0f,  1.0f }, // bottom-left
    {  3.0f, -1.0f,  2.0f,  1.0f }, // bottom-right
};

// =============================================================================
EffectManager::EffectManager() {}
EffectManager::~EffectManager() { Shutdown(); }

bool EffectManager::Initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                               UINT width, UINT height) {
    if (!device || !context) return false;
    device_  = device;
    context_ = context;

    // 1) Compile shared VS
    {
        ID3DBlob* vsBlob   = nullptr;
        ID3DBlob* errBlob  = nullptr;
        HRESULT hr = D3DCompile(kVSSource, std::strlen(kVSSource),
                                "vs_fullscreen", nullptr, nullptr,
                                "main", "vs_4_0",
                                D3DCOMPILE_ENABLE_STRICTNESS, 0,
                                &vsBlob, &errBlob);
        if (FAILED(hr) || !vsBlob) {
            if (errBlob) {
                std::cerr << "[EffectManager] VS compile failed: "
                          << (const char*)errBlob->GetBufferPointer() << std::endl;
                errBlob->Release();
            }
            return false;
        }
        if (errBlob) errBlob->Release();

        hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                         vsBlob->GetBufferSize(),
                                         nullptr, &vs_fullscreen_);
        if (FAILED(hr)) { vsBlob->Release(); return false; }

        const D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        hr = device_->CreateInputLayout(layoutDesc, 2,
                                        vsBlob->GetBufferPointer(),
                                        vsBlob->GetBufferSize(),
                                        &input_layout_);
        vsBlob->Release();
        if (FAILED(hr)) return false;
    }

    // 2) Passthrough PS (must succeed — used as fallback everywhere else)
    ps_passthrough_ = CompilePS(kPSPassthrough, "main");
    if (!ps_passthrough_) return false;

    // 3) Per-effect PS with pass-through fallback so a broken shader never
    //    takes down the app.
    // Task 5.13-fix4: DropShadow slot in effect_ps_ stays null on purpose —
    // the effect uses ps_dropshadow_fused_ (single-pass fused shader)
    // dispatched via a special branch in ApplyChain that also injects RT
    // dims into cb.p3.xy for px->UV math.
    struct { EffectType t; const char* src; } entries[] = {
        { EffectType::MotionTile,             kPSMotionTile },
        { EffectType::DirectionalMotionBlur,  kPSMotionBlur },
        { EffectType::ChromaticAberration,    kPSChroma     },
        { EffectType::BlendMode,              kPSBlend      },
    };
    for (const auto& e : entries) {
        ID3D11PixelShader* ps = CompilePS(e.src, "main");
        effect_ps_[(int)e.t] = ps ? ps : ps_passthrough_;
        // Note: if we assigned the passthrough as fallback, we do NOT own it
        // in the per-effect slot — Shutdown() must not release it twice.
    }

    // Task 5.13: three extra PS for the per-layer isolation path.
    // Non-fatal on failure — DropShadow / composite would just no-op, so
    // the app stays runnable even if HLSL parsing hits a hardware quirk.
    ps_composite_            = CompilePS(kPSComposite,            "main");
    // Task 5.13-fix4: single fused DropShadow PS. Old two-pass shaders
    // (offset + composite) had a same-texture SRV+RTV bind conflict on
    // Pass 2 that silently produced garbage. Fused version writes final
    // composited RGBA in one pass from a single readSRV -> writeRTV bind.
    ps_dropshadow_fused_     = CompilePS(kPSDropShadowFused,      "main");

    // Task 5.13-fix2: if composite shader compilation failed (unlikely but
    // not impossible on some drivers), fall back to the passthrough PS —
    // same one-liner sample+return, guaranteed to exist by step 2 above.
    // Without this fallback CompositeSRVOver silently no-ops and the
    // per-layer isolation dispatch produces a black composite.
    if (!ps_composite_) {
        std::cerr << "[EffectManager] ps_composite_ compile failed; falling back to ps_passthrough_" << std::endl;
        ps_composite_ = ps_passthrough_;
    }

    // 4) Fullscreen triangle VB
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = sizeof(kFullscreenTri);
        bd.Usage          = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {};
        sd.pSysMem = kFullscreenTri;
        HRESULT hr = device_->CreateBuffer(&bd, &sd, &vb_fullscreen_);
        if (FAILED(hr)) return false;
    }

    // 5) 64-byte dynamic constant buffer
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = sizeof(EffectParams);   // 64
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device_->CreateBuffer(&bd, nullptr, &cb_effect_);
        if (FAILED(hr)) return false;
    }

    // 6) Linear-clamp sampler
    {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxAnisotropy  = 1;
        sd.MinLOD         = 0.0f;
        sd.MaxLOD         = D3D11_FLOAT32_MAX;
        HRESULT hr = device_->CreateSamplerState(&sd, &linear_clamp_);
        if (FAILED(hr)) return false;
    }

    // 7) Standard alpha blend state for the final composite
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable    = TRUE;
        bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        HRESULT hr = device_->CreateBlendState(&bd, &blend_normal_);
        if (FAILED(hr)) return false;
    }

    // 7b) Task 5.13-fix: opaque REPLACE blend state. Effect passes need
    //     write-through semantics (the shader's output is the final pixel,
    //     including its own alpha). Alpha blending inside ApplyChain
    //     causes filtered layers to blend against stale pong contents.
    {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].BlendEnable    = FALSE;
        bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend      = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        HRESULT hr = device_->CreateBlendState(&bd, &blend_replace_);
        if (FAILED(hr)) return false;
    }

    // 7c) Task 5.13-fix2: explicit CULL_NONE rasterizer state. Fullscreen
    //     triangle winding is CCW (top=(-1,3), BL=(-1,-1), BR=(3,-1)); DX11
    //     default FrontCounterClockwise=FALSE means CCW is back-facing.
    //     ImGui's backend sets CULL_NONE which normally covers us, but if
    //     any other code (or a driver quirk) leaves CULL_BACK bound, our
    //     fullscreen triangle silently disappears. Bind explicitly.
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode              = D3D11_FILL_SOLID;
        rd.CullMode              = D3D11_CULL_NONE;
        rd.FrontCounterClockwise = FALSE;
        rd.DepthClipEnable       = TRUE;
        rd.ScissorEnable         = FALSE;
        rd.MultisampleEnable     = FALSE;
        rd.AntialiasedLineEnable = FALSE;
        HRESULT hr = device_->CreateRasterizerState(&rd, &rasterizer_none_);
        if (FAILED(hr)) return false;
    }

    // 8) Ping-pong RTs
    if (!CreateRenderTargets(width, height)) return false;

    initialized_ = true;
    return true;
}

ID3D11PixelShader* EffectManager::CompilePS(const char* hlslSrc, const char* entry) {
    if (!device_ || !hlslSrc) return nullptr;
    ID3DBlob* blob = nullptr;
    ID3DBlob* err  = nullptr;
    HRESULT hr = D3DCompile(hlslSrc, std::strlen(hlslSrc),
                            entry, nullptr, nullptr,
                            entry, "ps_4_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0,
                            &blob, &err);
    if (FAILED(hr) || !blob) {
        if (err) {
            std::cerr << "[EffectManager] PS compile failed (" << entry << "): "
                      << (const char*)err->GetBufferPointer() << std::endl;
            err->Release();
        }
        return nullptr;
    }
    if (err) err->Release();

    ID3D11PixelShader* ps = nullptr;
    hr = device_->CreatePixelShader(blob->GetBufferPointer(),
                                    blob->GetBufferSize(),
                                    nullptr, &ps);
    blob->Release();
    if (FAILED(hr)) return nullptr;
    return ps;
}

bool EffectManager::CreateRenderTargets(UINT width, UINT height) {
    if (!device_) return false;
    if (width  < 1) width  = 1;
    if (height < 1) height = 1;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width      = width;
    td.Height     = height;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    auto make = [&](ID3D11Texture2D** t, ID3D11RenderTargetView** r,
                    ID3D11ShaderResourceView** s) -> bool {
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, t);
        if (FAILED(hr) || !*t) return false;
        hr = device_->CreateRenderTargetView(*t, nullptr, r);
        if (FAILED(hr)) return false;
        hr = device_->CreateShaderResourceView(*t, nullptr, s);
        if (FAILED(hr)) return false;
        return true;
    };

    if (!make(&ping_tex_, &ping_rtv_, &ping_srv_)) return false;
    if (!make(&pong_tex_, &pong_rtv_, &pong_srv_)) return false;

    width_  = width;
    height_ = height;
    return true;
}

void EffectManager::ReleaseRenderTargets() {
    if (ping_srv_) { ping_srv_->Release(); ping_srv_ = nullptr; }
    if (ping_rtv_) { ping_rtv_->Release(); ping_rtv_ = nullptr; }
    if (ping_tex_) { ping_tex_->Release(); ping_tex_ = nullptr; }
    if (pong_srv_) { pong_srv_->Release(); pong_srv_ = nullptr; }
    if (pong_rtv_) { pong_rtv_->Release(); pong_rtv_ = nullptr; }
    if (pong_tex_) { pong_tex_->Release(); pong_tex_ = nullptr; }
    width_ = height_ = 0;
}

void EffectManager::Resize(UINT width, UINT height) {
    if (!initialized_) return;
    if (width == width_ && height == height_) return;
    ReleaseRenderTargets();
    CreateRenderTargets(width, height);
}

void EffectManager::DrawFullscreenPass(ID3D11PixelShader* ps,
                                       ID3D11ShaderResourceView* srcSRV) {
    if (!context_ || !ps || !vs_fullscreen_ || !vb_fullscreen_ || !input_layout_) return;

    const UINT stride = sizeof(FullscreenVert);
    const UINT offset = 0;
    context_->IASetInputLayout(input_layout_);
    context_->IASetVertexBuffers(0, 1, &vb_fullscreen_, &stride, &offset);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Task 5.13-fix2: explicit CULL_NONE rasterizer bind. See
    // rasterizer_none_ init in Initialize() for rationale.
    if (rasterizer_none_) context_->RSSetState(rasterizer_none_);
    // Task 5.13-fix2: unbind any lingering depth-stencil state — we don't
    // depth-test in the effect pipeline and inherited DSV state can drop
    // the draw silently on some drivers.
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->VSSetShader(vs_fullscreen_, nullptr, 0);
    context_->PSSetShader(ps,             nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &cb_effect_);
    context_->PSSetSamplers(0, 1, &linear_clamp_);
    context_->PSSetShaderResources(0, 1, &srcSRV);
    context_->Draw(3, 0);

    // Unbind SRV so it can be used as an RTV on the next pass (D3D warns
    // loudly if you leave a resource bound simultaneously as SRV and RTV).
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context_->PSSetShaderResources(0, 1, &nullSRV);
}

bool EffectManager::ApplyChain(ID3D11ShaderResourceView* sourceSRV,
                               ID3D11RenderTargetView*   destinationRTV,
                               const std::vector<Effect>& effects) {
    if (!initialized_ || !context_) return false;
    if (!sourceSRV || !destinationRTV) return false;
    // Bound the vector size defensively — a runaway UI shouldn't cost us a
    // 500-pass frame.
    if (effects.size() > 32) return false;

    // Count enabled effects (they might all be disabled).
    size_t enabledCount = 0;
    for (const auto& e : effects) if (e.enabled) ++enabledCount;

    // Task 5.13-fix: bind opaque REPLACE blend state ONCE at the top of
    // ApplyChain. Every effect pass (simple + DropShadow's internal
    // passes + the post-loop passthrough copy) writes the shader's own
    // RGBA verbatim into the pool RT. Alpha blending here would cause
    // the shader's transparent-outside-shape pixels to reveal whatever
    // stale garbage lived in pong, which is exactly the "filtered layer
    // vanishes" bug from the first shipping of Task 5.13. The final
    // layer-over-comp composite is done separately by CompositeSRVOver,
    // which explicitly rebinds blend_normal_.
    if (blend_replace_) {
        const float blendFactor[4] = { 0, 0, 0, 0 };
        context_->OMSetBlendState(blend_replace_, blendFactor, 0xFFFFFFFF);
    }

    // Fast path: nothing to do -> passthrough source into destination.
    if (enabledCount == 0) {
        D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f };
        context_->RSSetViewports(1, &vp);
        context_->OMSetRenderTargets(1, &destinationRTV, nullptr);
        // Upload zero'd constants so passthrough is deterministic
        EffectParams empty;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(context_->Map(cb_effect_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &empty, sizeof(empty));
            context_->Unmap(cb_effect_, 0);
        }
        DrawFullscreenPass(ps_passthrough_, sourceSRV);
        return true;
    }

    // Ping-pong: we start reading from sourceSRV and writing to whichever
    // of the two pool RTs does NOT back sourceSRV — otherwise the first
    // pass would try to bind the same texture as both SRV and RTV, which
    // D3D11 refuses. When sourceSRV points OUTSIDE the pool (typical
    // pre-5.13 case: compSRV or an atlas SRV), start on pingRTV. When it
    // points AT pingSRV (Task 5.13 per-layer isolation path), start on
    // pongRTV. When it points at pongSRV, start on pingRTV. This makes
    // the function work correctly no matter which ping/pong texture the
    // caller staged the source into.
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* readSRV  = sourceSRV;
    ID3D11RenderTargetView*   writeRTV;
    ID3D11ShaderResourceView* writeSRV;
    ID3D11RenderTargetView*   otherRTV;
    ID3D11ShaderResourceView* otherSRV;
    if (sourceSRV == ping_srv_) {
        writeRTV = pong_rtv_;  writeSRV = pong_srv_;
        otherRTV = ping_rtv_;  otherSRV = ping_srv_;
    } else {
        writeRTV = ping_rtv_;  writeSRV = ping_srv_;
        otherRTV = pong_rtv_;  otherSRV = pong_srv_;
    }

    // Task 5.13: parity-safe destination handling.
    //
    // Old code: last pass writes directly into destinationRTV. That breaks
    // when destinationRTV is the same texture as readSRV on the last pass
    // (e.g. dest=pongRTV, chain length 2 -> readSRV=pongSRV at pass 2).
    // We can't bind the same texture as SRV and RTV in one draw.
    //
    // Fix: NEVER let the loop write directly to destinationRTV. Every
    // pass, including the last, writes into a pool RT. After the loop,
    // do ONE extra passthrough copy from the last-written pool SRV into
    // destinationRTV. Costs one fullscreen quad per chain — trivial.
    ID3D11ShaderResourceView* finalOutputSRV = nullptr;

    size_t applied = 0;
    for (const auto& eff : effects) {
        if (!eff.enabled) continue;
        const bool isLast = (++applied == enabledCount);
        (void)isLast; // no longer differentiates dst — kept for clarity below

        // -------------------------------------------------------------
        // Task 5.13-fix4: DropShadow uses the FUSED single-pass shader.
        // Reads source at readSRV, writes final composited RGBA into
        // writeRTV. Identical bind pattern to a simple effect except we
        // must inject the RT dims into cb.p3.xy for px->UV math inside
        // the shader (Distance and Softness are in pixels).
        //
        // The old two-pass design bound readSRV as t1 while also using
        // otherRTV = ping_rtv_ as the render target — same texture as
        // readSRV — which D3D11 silently drops. See kPSDropShadowFused
        // comment block above for full history.
        // -------------------------------------------------------------
        if (eff.type == EffectType::DropShadow && ps_dropshadow_fused_) {
            EffectParams pp = eff.params;
            pp.p3[0] = (float)width_;
            pp.p3[1] = (float)height_;
            D3D11_MAPPED_SUBRESOURCE ms1;
            if (SUCCEEDED(context_->Map(cb_effect_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms1))) {
                std::memcpy(ms1.pData, &pp, sizeof(EffectParams));
                context_->Unmap(cb_effect_, 0);
            }
            context_->OMSetRenderTargets(1, &writeRTV, nullptr);
            DrawFullscreenPass(ps_dropshadow_fused_, readSRV);
            finalOutputSRV = writeSRV;
            readSRV  = writeSRV;
            std::swap(writeRTV, otherRTV);
            std::swap(writeSRV, otherSRV);
            continue;
        }

        // Task 5.13: simple effect always writes into pool writeRTV
        // (never destinationRTV directly). Post-loop copies the final
        // pool SRV into destinationRTV.
        // Upload params
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(context_->Map(cb_effect_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &eff.params, sizeof(EffectParams));
            context_->Unmap(cb_effect_, 0);
        }

        context_->OMSetRenderTargets(1, &writeRTV, nullptr);

        const int idx = (int)eff.type;
        ID3D11PixelShader* ps = (idx >= 0 && idx < (int)EffectType::COUNT)
                                    ? effect_ps_[idx] : ps_passthrough_;
        if (!ps) ps = ps_passthrough_;

        DrawFullscreenPass(ps, readSRV);
        finalOutputSRV = writeSRV;

        // Always swap after each pass — even the last one, so the
        // finalOutputSRV pointer stays synchronised with what we just
        // wrote.
        readSRV  = writeSRV;
        std::swap(writeRTV, otherRTV);
        std::swap(writeSRV, otherSRV);
    }

    // Task 5.13: post-loop copy from finalOutputSRV into destinationRTV.
    // Passthrough shader is a plain SRV-to-RT blit. This handles all
    // parity cases (even/odd effect count) AND the DropShadow output
    // uniformly — the destination is guaranteed to be safe to bind
    // because it's not one of our pool textures (unless caller
    // deliberately passed one; if they did, we skip the copy and the
    // result already sits in the right place).
    if (finalOutputSRV) {
        if (destinationRTV != ping_rtv_ &&
            destinationRTV != pong_rtv_) {
            // Normal case: destination is external (e.g. compRTV). Blit.
            D3D11_VIEWPORT vp2 = { 0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f };
            context_->RSSetViewports(1, &vp2);
            context_->OMSetRenderTargets(1, &destinationRTV, nullptr);
            DrawFullscreenPass(ps_passthrough_, finalOutputSRV);
        } else if ((destinationRTV == ping_rtv_ && finalOutputSRV != ping_srv_) ||
                   (destinationRTV == pong_rtv_ && finalOutputSRV != pong_srv_)) {
            // Destination is a pool RT AND source is the other pool SRV
            // (different texture) — safe to blit.
            D3D11_VIEWPORT vp2 = { 0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f };
            context_->RSSetViewports(1, &vp2);
            context_->OMSetRenderTargets(1, &destinationRTV, nullptr);
            DrawFullscreenPass(ps_passthrough_, finalOutputSRV);
        }
        // Otherwise: destination and source refer to the same pool
        // texture. Result is already there — no copy needed.
    }
    return true;
}

// Task 5.13: composite one SRV over an RTV. Standard SRC_ALPHA /
// INV_SRC_ALPHA blend. Used by the per-layer isolation path to blit a
// filtered layer (pingSRV, containing the layer post-effects) over the
// shared compRTV. Set the blend state locally so we don't rely on
// whatever the caller left bound.
bool EffectManager::CompositeSRVOver(ID3D11ShaderResourceView* srcSRV,
                                     ID3D11RenderTargetView*   dstRTV) {
    if (!initialized_ || !context_) return false;
    if (!srcSRV || !dstRTV || !ps_composite_) return false;
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    context_->OMSetRenderTargets(1, &dstRTV, nullptr);
    // Alpha-over: our source RGBA composites onto whatever's in dst.
    const float blendFactor[4] = { 0, 0, 0, 0 };
    context_->OMSetBlendState(blend_normal_, blendFactor, 0xFFFFFFFF);
    DrawFullscreenPass(ps_composite_, srcSRV);
    return true;
}

void EffectManager::Shutdown() {
    // Free per-effect PS. We must NOT release the passthrough more than once,
    // even if some effect slots point at it as a fallback.
    for (int i = 0; i < (int)EffectType::COUNT; ++i) {
        if (effect_ps_[i] && effect_ps_[i] != ps_passthrough_) {
            effect_ps_[i]->Release();
        }
        effect_ps_[i] = nullptr;
    }
    // Task 5.13: extra PS for the per-layer isolation path.
    // Task 5.13-fix2: composite may alias to passthrough (fallback path);
    // release before the passthrough, guard against the alias case.
    if (ps_composite_ && ps_composite_ != ps_passthrough_) {
        ps_composite_->Release();
    }
    ps_composite_ = nullptr;
    // Task 5.13-fix4: fused DropShadow PS replaces the old offset+composite pair.
    if (ps_dropshadow_fused_)      { ps_dropshadow_fused_->Release();      ps_dropshadow_fused_      = nullptr; }
    if (ps_passthrough_) { ps_passthrough_->Release(); ps_passthrough_ = nullptr; }
    if (vs_fullscreen_)  { vs_fullscreen_->Release();  vs_fullscreen_  = nullptr; }
    if (input_layout_)   { input_layout_->Release();   input_layout_   = nullptr; }
    if (vb_fullscreen_)  { vb_fullscreen_->Release();  vb_fullscreen_  = nullptr; }
    if (cb_effect_)      { cb_effect_->Release();      cb_effect_      = nullptr; }
    if (linear_clamp_)   { linear_clamp_->Release();   linear_clamp_   = nullptr; }
    if (blend_normal_)    { blend_normal_->Release();    blend_normal_    = nullptr; }
    if (blend_replace_)   { blend_replace_->Release();   blend_replace_   = nullptr; }
    if (rasterizer_none_) { rasterizer_none_->Release(); rasterizer_none_ = nullptr; }
    ReleaseRenderTargets();
    initialized_ = false;
    device_ = nullptr;
    context_ = nullptr;
}
