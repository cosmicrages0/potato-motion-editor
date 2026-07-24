#include "CompositionRenderer.h"

#include <iostream>
#include <cstring>
#include <cmath>
#include <algorithm>

// =============================================================================
// HLSL. Kept inline (single-file per module rule from PROJECT_BRIEFING).
// One VS reused across all shape types; separate PS per shape so ellipse/null
// specific math (SDF, X marker) live where they belong.
//
// Constant buffer layout must match ShapeCB in the header EXACTLY.
// =============================================================================

static const char* kVSSrc = R"HLSL(
cbuffer ShapeCB : register(b0) {
    float4x4 mvp;
    float4   color;
    float4   params;
};
struct VSIn  { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };

VSOut main(VSIn i) {
    VSOut o;
    // pos is in unit-quad space (-0.5..+0.5). mvp scales / rotates / translates
    // to clip space directly.
    float4 world = mul(float4(i.pos, 0.0, 1.0), mvp);
    o.pos = world;
    o.uv = i.uv;
    o.color = color;
    return o;
}
)HLSL";

// Task 5.7: consolidated shape SDF pixel shader.
//
// Handles both Rectangle (params.x < 0.5) and Ellipse (params.x >= 0.5),
// with fill + optional stroke + rounded corners (rect only).
//
//   params.x   = shape type (0 = rect, 1 = ellipse)
//   params.y   = stroke width in pixels (0 = no stroke)
//   params.z   = corner radius in pixels (rect only)
//   params2.xy = half-extent in pixels (matches the shape's world-pixel size)
//
// Both edges are anti-aliased via smoothstep over a 2-pixel window (aa = 1).
// Outer edge is faded with alpha instead of hard `discard` so the shape
// silhouette is smooth at low zoom (pre-go review #1).
//
// Corner radius is clamped in-shader to min(halfExtent.x, halfExtent.y)
// so a corrupt file or mid-animation size change can't push the SDF into
// the pinched/inverted regime (pre-go review #2 belt-and-braces).
//
// Rounded-rect SDF is Iñigo Quílez's classic formulation. Ellipse SDF is
// the fast `length(p/e) - 1` approximation — sharp for circles, slightly
// distorted for extreme aspect ratios (documented tradeoff in design
// section 7). Good enough for stroke widths well under the shorter radius.
static const char* kPSShapeSDFSrc = R"HLSL(
cbuffer ShapeCB : register(b0) {
    float4x4 mvp;
    float4   color;   // fill  RGBA
    float4   stroke;  // stroke RGBA
    float4   params;  // (type, strokeWidth, cornerRadius, unused)
    float4   params2; // (halfExtentX, halfExtentY, unused, unused)
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };

float4 main(VSOut i) : SV_TARGET {
    // Convert UV (0..1) to centered pixel-space coordinates so all
    // subsequent SDF math is in real pixels.
    float2 halfExtent = params2.xy;
    float2 p          = (i.uv - 0.5) * 2.0 * halfExtent;

    float d;
    if (params.x < 0.5) {
        // Rounded rectangle SDF. Clamp radius to what fits.
        float r  = min(params.z, min(halfExtent.x, halfExtent.y));
        r        = max(r, 0.0);
        float2 q = abs(p) - halfExtent + r;
        d        = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    } else {
        // Fast ellipse SDF approximation.
        float2 e = halfExtent;
        d = length(p / e) - 1.0;
        d *= min(e.x, e.y);  // rough conversion back to pixels
    }

    // Outer edge anti-aliasing: fade alpha across a 2-pixel window.
    float aa = 1.0;
    float outerAlpha = 1.0 - smoothstep(-aa, aa, d);
    if (outerAlpha <= 0.001) discard;

    float4 result = color;   // fill baseline
    float sw = params.y;
    if (sw > 0.0) {
        // -d is how many pixels INSIDE the shape we are (>=0 when inside).
        // strokeMix goes 0 near the edge (stroke) -> 1 deeper in (fill),
        // with a 2-pixel soft transition so the stroke/fill boundary AAs.
        float strokeMix = smoothstep(sw - aa, sw + aa, -d);
        result = lerp(stroke, color, strokeMix);
    }
    result.a *= outerAlpha;
    return result;
}
)HLSL";

static const char* kPSNullSrc = R"HLSL(
// Null marker: draw an X inside the quad. UV is [0,1]. Keep bright and thin.
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
float4 main(VSOut i) : SV_TARGET {
    float2 uv = i.uv;
    // Two diagonal bars ~2% thick, plus a subtle box outline.
    float d1 = abs(uv.x - uv.y);
    float d2 = abs(uv.x + uv.y - 1.0);
    float bar = min(d1, d2);
    if (bar > 0.03) {
        // Box outline
        float border = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
        if (border > 0.02) discard;
    }
    return float4(0.75, 0.75, 0.75, 1.0);
}
)HLSL";

// Unit quad centered on origin, UVs 0..1 across the face.
struct QVert { float x, y; float u, v; };
static const QVert kQuadVerts[4] = {
    { -0.5f, -0.5f, 0.0f, 0.0f },
    {  0.5f, -0.5f, 1.0f, 0.0f },
    {  0.5f,  0.5f, 1.0f, 1.0f },
    { -0.5f,  0.5f, 0.0f, 1.0f },
};
static const unsigned short kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };

// =============================================================================
CompositionRenderer::CompositionRenderer() {}
CompositionRenderer::~CompositionRenderer() { Shutdown(); }

static ID3DBlob* CompileHLSL(const char* src, const char* entry, const char* profile) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* err  = nullptr;
    HRESULT hr = D3DCompile(src, std::strlen(src), entry, nullptr, nullptr,
                            entry, profile,
                            D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            std::cerr << "[CompositionRenderer] Compile failed (" << profile
                      << "/" << entry << "): "
                      << (const char*)err->GetBufferPointer() << std::endl;
            err->Release();
        }
        if (blob) blob->Release();
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

bool CompositionRenderer::CreateShaders() {
    // Vertex shader + input layout
    ID3DBlob* vsBlob = CompileHLSL(kVSSrc, "main", "vs_4_0");
    if (!vsBlob) return false;
    HRESULT hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(),
                                             vsBlob->GetBufferSize(),
                                             nullptr, &vs_);
    if (FAILED(hr)) { vsBlob->Release(); return false; }

    const D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
          D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device_->CreateInputLayout(ied, 2,
                                    vsBlob->GetBufferPointer(),
                                    vsBlob->GetBufferSize(),
                                    &layout_);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    // Pixel shaders per shape family
    auto makePS = [&](const char* src, ID3D11PixelShader** out) -> bool {
        ID3DBlob* b = CompileHLSL(src, "main", "ps_4_0");
        if (!b) return false;
        HRESULT h = device_->CreatePixelShader(b->GetBufferPointer(),
                                               b->GetBufferSize(),
                                               nullptr, out);
        b->Release();
        return SUCCEEDED(h);
    };
    // Task 5.7: one SDF pixel shader for both Rect and Ellipse.
    if (!makePS(kPSShapeSDFSrc, &ps_shape_sdf_)) return false;
    if (!makePS(kPSNullSrc,     &ps_null_))      return false;
    return true;
}

bool CompositionRenderer::CreateBuffers() {
    // Constant buffer (dynamic, updated per shape)
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth      = sizeof(ShapeCB);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_shape_))) return false;
    }
    // Immutable quad VB
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(kQuadVerts);
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = kQuadVerts;
        if (FAILED(device_->CreateBuffer(&bd, &sd, &vb_quad_))) return false;
    }
    // Immutable quad IB
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(kQuadIndices);
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd = {}; sd.pSysMem = kQuadIndices;
        if (FAILED(device_->CreateBuffer(&bd, &sd, &ib_quad_))) return false;
    }
    // Standard alpha blend
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
        if (FAILED(device_->CreateBlendState(&bd, &blend_alpha_))) return false;
    }
    return true;
}

bool CompositionRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;
    device_  = device;
    context_ = context;
    if (!CreateShaders()) return false;
    if (!CreateBuffers()) return false;
    initialized_ = true;
    return true;
}

void CompositionRenderer::ReleaseAll() {
    if (blend_alpha_) { blend_alpha_->Release(); blend_alpha_ = nullptr; }
    if (ib_quad_)     { ib_quad_->Release();     ib_quad_     = nullptr; }
    if (vb_quad_)     { vb_quad_->Release();     vb_quad_     = nullptr; }
    if (cb_shape_)    { cb_shape_->Release();    cb_shape_    = nullptr; }
    if (layout_)      { layout_->Release();      layout_      = nullptr; }
    if (ps_null_)      { ps_null_->Release();      ps_null_      = nullptr; }
    // Task 5.7: single consolidated SDF pixel shader replaced the old
    // rect + ellipse pair. Both former pointers are gone from the header.
    if (ps_shape_sdf_) { ps_shape_sdf_->Release(); ps_shape_sdf_ = nullptr; }
    if (vs_)          { vs_->Release();          vs_          = nullptr; }
}

void CompositionRenderer::Shutdown() {
    ReleaseAll();
    device_ = nullptr;
    context_ = nullptr;
    initialized_ = false;
}

// -----------------------------------------------------------------------------
// Build MVP for a shape:
//   Layer world matrix is in COMPOSITION PIXEL coords (0..W, 0..H top-left).
//   We need clip space (-1..+1, +1..-1 top-left).
//
// The unit quad is centered on origin sized 1x1, so we first scale by the
// layer's size, then apply the layer's world matrix (which already includes
// scale/rotate/translate + anchor offset done in Layer::ToLocalMatrix). Then
// we convert composition pixels to NDC.
// -----------------------------------------------------------------------------
static void BuildShapeMVP(const Mat3& world, const Vec2& size,
                          UINT targetW, UINT targetH, float outRow[16]) {
    // We want: clip = P * world * S * quadVert
    //   where S scales the unit quad from [-0.5, +0.5] to layer local (0..w, 0..h)
    //   AND translates the origin (so the quad's origin sits at (w/2, h/2), matching
    //   the layer's local top-left-at-origin convention).
    //
    // Local vertex v_local in [-0.5,+0.5]^2 -> layer-local (0..w, 0..h):
    //     v = (v_local + 0.5) * size
    //
    // Then world matrix transforms to composition-pixel space:
    //     v_comp = world * v
    //
    // Then map pixels to NDC:
    //     ndc.x =  (v_comp.x / W) * 2 - 1
    //     ndc.y = -(v_comp.y / H) * 2 + 1
    //
    // We fold all of this into a single 4x4. Because size, world, and the
    // ortho projection all commute nicely (all affine 2D), we can just
    // compute the full 3x3 transform in pixel space and then apply ortho.

    // Step 1: quad -> layer local (scale + translate)
    // Represented as a 3x3:
    //   [sx  0  tx]     sx = size.x, tx = size.x * 0.5
    //   [ 0 sy  ty]     sy = size.y, ty = size.y * 0.5
    //   [ 0  0   1]
    Mat3 quadToLocal;
    quadToLocal.m[0][0] = size.x;  quadToLocal.m[0][2] = size.x * 0.5f;
    quadToLocal.m[1][1] = size.y;  quadToLocal.m[1][2] = size.y * 0.5f;

    // Step 2: layer local -> composition pixels
    Mat3 quadToComp = world * quadToLocal;

    // Step 3: composition pixels -> NDC (baked into row-major 4x4).
    // For a vertex v = (x, y, 0, 1) in comp-pixel space:
    //   ndc.x =  (a x + b y + c) * (2/W) - 1
    //   ndc.y = -(d x + e y + f) * (2/H) + 1
    // where (a,b,c) is row 0 of quadToComp and (d,e,f) is row 1.
    const float invW = (targetW > 0) ? (2.0f / (float)targetW) : 0.0f;
    const float invH = (targetH > 0) ? (2.0f / (float)targetH) : 0.0f;

    const float a = quadToComp.m[0][0], b = quadToComp.m[0][1], c = quadToComp.m[0][2];
    const float d = quadToComp.m[1][0], e = quadToComp.m[1][1], f = quadToComp.m[1][2];

    // HLSL is mul(v, mvp) with row vectors in our VS => mvp is row-major
    // and each output component is a row (rows are treated as columns of the
    // transposed transform). Actually with mul(v, m) HLSL treats v as row and
    // m as row-major, computing v * m. So m[i][j] is (row i, col j) and:
    //   out.x = v.x*m[0][0] + v.y*m[1][0] + v.z*m[2][0] + v.w*m[3][0]
    // We want: out.x =  a*v.x + b*v.y + 0*v.z + c*v.w, then scaled by invW - 1*v.w
    //          = (a*invW)*v.x + (b*invW)*v.y + 0 + (c*invW - 1)*v.w
    // So the first COLUMN (m[.][0]) is (a*invW, b*invW, 0, c*invW - 1).
    //
    // For out.y: -(d*v.x + e*v.y + f*v.w)*invH + v.w
    //          = (-d*invH)*v.x + (-e*invH)*v.y + 0 + (-f*invH + 1)*v.w
    // Column 1 is (-d*invH, -e*invH, 0, -f*invH + 1).
    //
    // out.z = 0 (we don't care about depth in 2D)
    // out.w = 1 (passthrough)
    float mvp[4][4] = {0};
    // Column 0
    mvp[0][0] =  a * invW;
    mvp[1][0] =  b * invW;
    mvp[3][0] =  c * invW - 1.0f;
    // Column 1
    mvp[0][1] = -d * invH;
    mvp[1][1] = -e * invH;
    mvp[3][1] = -f * invH + 1.0f;
    // Column 2
    mvp[2][2] = 1.0f;
    // Column 3 (w passthrough)
    mvp[3][3] = 1.0f;

    // Flatten for the CB upload.
    //
    // BUG-FIX: HLSL float4x4 defaults to COLUMN-MAJOR storage. If we upload
    // rows sequentially it would end up transposed inside the shader,
    // producing a mirrored/rotated transform where dragging position moved
    // the shape in the wrong direction (this was the "shape watches the
    // bounding box move" bug in tests 3.2 / 5 / 10). We fix by writing the
    // matrix TRANSPOSED to the buffer: element (r,c) goes to slot [c*4 + r].
    for (int r = 0; r < 4; ++r)
        for (int col = 0; col < 4; ++col)
            outRow[col * 4 + r] = mvp[r][col];
}

static void UnpackABGRToRGBAf(unsigned int abgr, float out[4]) {
    // IM_COL32 packs as (a<<24)|(b<<16)|(g<<8)|(r), matching D3D R8G8B8A8_UNORM
    // byte order (R lowest). Our shader wants RGBA in 0..1.
    out[0] = ((abgr >>  0) & 0xFF) / 255.0f;
    out[1] = ((abgr >>  8) & 0xFF) / 255.0f;
    out[2] = ((abgr >> 16) & 0xFF) / 255.0f;
    out[3] = ((abgr >> 24) & 0xFF) / 255.0f;
}

// Task 5.7: DrawRect + DrawEllipse now share ps_shape_sdf_. Both take
// stroke color + stroke width; DrawRect also takes corner radius (ignored
// by the ellipse branch of the shader). halfExtent is written into
// params2.xy so the shader can do pixel-space SDF math regardless of the
// baked-in MVP scaling. Passing strokeWidth = 0 reproduces the pre-5.7
// unstroked look; passing radius = 0 reproduces sharp corners.
void CompositionRenderer::DrawRect(const Mat3& world, const Vec2& size,
                                   unsigned int fillColor, unsigned int strokeColor,
                                   float strokeWidth, float cornerRadius,
                                   UINT logicalW, UINT logicalH) {
    if (!context_ || !ps_shape_sdf_) return;
    ShapeCB cb{};
    // Task 5.8: MVP division uses LOGICAL dims (composition pixels), NOT the
    // RT's actual pixel count. That keeps shape coordinates comp-correct even
    // when the RT is downsampled by the preview-scale knob.
    BuildShapeMVP(world, size, logicalW, logicalH, cb.mvp);
    UnpackABGRToRGBAf(fillColor,   cb.color);
    UnpackABGRToRGBAf(strokeColor, cb.stroke);
    cb.params[0]  = 0.0f;                      // shape type = rect
    cb.params[1]  = std::max(0.0f, strokeWidth);
    cb.params[2]  = std::max(0.0f, cornerRadius);
    cb.params[3]  = 0.0f;
    cb.params2[0] = size.x * 0.5f;             // half-extent X (pixels)
    cb.params2[1] = size.y * 0.5f;             // half-extent Y (pixels)
    cb.params2[2] = 0.0f;
    cb.params2[3] = 0.0f;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(context_->Map(cb_shape_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof(cb));
        context_->Unmap(cb_shape_, 0);
    }
    context_->PSSetShader(ps_shape_sdf_, nullptr, 0);
    context_->DrawIndexed(6, 0, 0);
}

void CompositionRenderer::DrawEllipse(const Mat3& world, const Vec2& size,
                                      unsigned int fillColor, unsigned int strokeColor,
                                      float strokeWidth,
                                      UINT logicalW, UINT logicalH) {
    if (!context_ || !ps_shape_sdf_) return;
    ShapeCB cb{};
    // Task 5.8: see DrawRect — logical dims for MVP.
    BuildShapeMVP(world, size, logicalW, logicalH, cb.mvp);
    UnpackABGRToRGBAf(fillColor,   cb.color);
    UnpackABGRToRGBAf(strokeColor, cb.stroke);
    cb.params[0]  = 1.0f;                      // shape type = ellipse
    cb.params[1]  = std::max(0.0f, strokeWidth);
    cb.params[2]  = 0.0f;                      // radius ignored for ellipse
    cb.params[3]  = 0.0f;
    cb.params2[0] = size.x * 0.5f;
    cb.params2[1] = size.y * 0.5f;
    cb.params2[2] = 0.0f;
    cb.params2[3] = 0.0f;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(context_->Map(cb_shape_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof(cb));
        context_->Unmap(cb_shape_, 0);
    }
    context_->PSSetShader(ps_shape_sdf_, nullptr, 0);
    context_->DrawIndexed(6, 0, 0);
}

void CompositionRenderer::DrawNullMarker(const Mat3& world, const Vec2& size,
                                         UINT logicalW, UINT logicalH) {
    if (!context_ || !ps_null_) return;
    ShapeCB cb{};
    // Task 5.8: see DrawRect — logical dims for MVP.
    BuildShapeMVP(world, size, logicalW, logicalH, cb.mvp);
    cb.color[0] = cb.color[1] = cb.color[2] = 0.75f;
    cb.color[3] = 1.0f;
    cb.params[0] = 2.0f;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(context_->Map(cb_shape_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof(cb));
        context_->Unmap(cb_shape_, 0);
    }
    context_->PSSetShader(ps_null_, nullptr, 0);
    context_->DrawIndexed(6, 0, 0);
}

void CompositionRenderer::RenderLayers(ID3D11RenderTargetView* targetRTV,
                                       UINT targetW, UINT targetH,
                                       LayerManager& layerManager,
                                       const float bgColor[4],
                                       UINT logicalW, UINT logicalH) {
    if (!initialized_ || !context_ || !targetRTV) return;
    if (targetW == 0 || targetH == 0) return;

    // Task 5.8: back-compat — if the caller didn't specify logical dims,
    // fall back to the target dims. That reproduces the pre-5.8 behavior
    // where shape MVP + viewport were locked to the same size.
    if (logicalW == 0) logicalW = targetW;
    if (logicalH == 0) logicalH = targetH;

    // Bind target + viewport + clear. Viewport uses TARGET dims (RT pixel
    // count) — that's how many pixels the rasterizer fills.
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)targetW, (float)targetH, 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    context_->OMSetRenderTargets(1, &targetRTV, nullptr);
    context_->ClearRenderTargetView(targetRTV, bgColor);

    // Pipeline state (set once for all shapes)
    const float blendFactor[4] = { 0, 0, 0, 0 };
    context_->OMSetBlendState(blend_alpha_, blendFactor, 0xFFFFFFFF);
    context_->IASetInputLayout(layout_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(QVert);
    const UINT offset = 0;
    context_->IASetVertexBuffers(0, 1, &vb_quad_, &stride, &offset);
    context_->IASetIndexBuffer(ib_quad_, DXGI_FORMAT_R16_UINT, 0);
    context_->VSSetShader(vs_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &cb_shape_);
    context_->PSSetConstantBuffers(0, 1, &cb_shape_);

    // Draw every visible 2D layer in timeline order (later = on top).
    // 3D layers, Camera, and Null are handled elsewhere (Null has its own
    // draw here since it lives in 2D composition space).
    // Task 5.1: sample AnimatedProperty at the LayerManager's frame comp time
    // so shape size matches whatever GetWorldMatrix() used for the transform.
    const float compT = layerManager.CurrentCompTime();
    for (auto& layer : layerManager.Layers()) {
        if (!layer.isVisible) continue;
        if (layer.is3D)       continue;
        if (layer.type == ShapeType::Camera) continue;

        const Mat3 wm = layerManager.GetWorldMatrix(layer.id);
        const Vec2 sz = layer.transform.sizePixels.Evaluate(compT);

        // Bake per-layer opacity into the color's alpha channel.
        // Task 5.7: apply the same opacity multiply to the stroke color so
        // stroke and fill fade together when the layer opacity animates.
        const float layerOp = layerManager.GetWorldOpacity(layer.id);
        auto applyOp = [&](unsigned int c) -> unsigned int {
            unsigned int a = (c >> 24) & 0xFFu;
            a = (unsigned int)std::clamp((int)((float)a * layerOp), 0, 255);
            return (c & 0x00FFFFFFu) | (a << 24);
        };
        const unsigned int fillC   = applyOp(layer.fillColor);
        const unsigned int strokeC = applyOp(layer.strokeColor);
        const float sw = layer.strokeWidth;
        const float cr = layer.cornerRadius;

        switch (layer.type) {
            case ShapeType::Rectangle:
                DrawRect(wm, sz, fillC, strokeC, sw, cr, logicalW, logicalH); break;
            case ShapeType::Ellipse:
                DrawEllipse(wm, sz, fillC, strokeC, sw, logicalW, logicalH); break;
            case ShapeType::CustomPath:
                DrawEllipse(wm, sz, fillC, strokeC, sw, logicalW, logicalH); break; // stub
            case ShapeType::Null:
                DrawNullMarker(wm, sz, logicalW, logicalH); break;
            case ShapeType::Camera:
                break;
        }
    }
}
