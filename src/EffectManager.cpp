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

    // Ping-pong: we start reading from sourceSRV and writing to ping_rtv_.
    // After the first pass, subsequent passes read the previous output SRV.
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)width_, (float)height_, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* readSRV = sourceSRV;
    ID3D11RenderTargetView*   writeRTV = ping_rtv_;
    ID3D11ShaderResourceView* writeSRV = ping_srv_;
    ID3D11RenderTargetView*   otherRTV = pong_rtv_;
    ID3D11ShaderResourceView* otherSRV = pong_srv_;

    size_t applied = 0;
    for (const auto& eff : effects) {
        if (!eff.enabled) continue;
        const bool isLast = (++applied == enabledCount);

        ID3D11RenderTargetView* dst = isLast ? destinationRTV : writeRTV;

        // Upload params
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(context_->Map(cb_effect_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &eff.params, sizeof(EffectParams));
            context_->Unmap(cb_effect_, 0);
        }

        context_->OMSetRenderTargets(1, &dst, nullptr);

        const int idx = (int)eff.type;
        ID3D11PixelShader* ps = (idx >= 0 && idx < (int)EffectType::COUNT)
                                    ? effect_ps_[idx] : ps_passthrough_;
        if (!ps) ps = ps_passthrough_;

        DrawFullscreenPass(ps, readSRV);

        if (!isLast) {
            // Next pass reads what we just wrote.
            readSRV  = writeSRV;
            std::swap(writeRTV, otherRTV);
            std::swap(writeSRV, otherSRV);
        }
    }
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
    if (ps_passthrough_) { ps_passthrough_->Release(); ps_passthrough_ = nullptr; }
    if (vs_fullscreen_)  { vs_fullscreen_->Release();  vs_fullscreen_  = nullptr; }
    if (input_layout_)   { input_layout_->Release();   input_layout_   = nullptr; }
    if (vb_fullscreen_)  { vb_fullscreen_->Release();  vb_fullscreen_  = nullptr; }
    if (cb_effect_)      { cb_effect_->Release();      cb_effect_      = nullptr; }
    if (linear_clamp_)   { linear_clamp_->Release();   linear_clamp_   = nullptr; }
    if (blend_normal_)   { blend_normal_->Release();   blend_normal_   = nullptr; }
    ReleaseRenderTargets();
    initialized_ = false;
    device_ = nullptr;
    context_ = nullptr;
}
