#include "QuadRenderer.hpp"
#include "../core/DebugLog.h"
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------------------
// Embedded HLSL source — avoids a separate shader-compilation build step.
// ---------------------------------------------------------------------------
static const char kQuadHLSL[] = R"(
cbuffer PerDraw : register(b0) {
    row_major float4x4 mvp;
    float    alpha;
    float    blurAmount;
    float2   uvMin;
    float2   uvMax;
    float2   _pad;
};

struct VSInput {
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct PSInput {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

PSInput VSMain(VSInput input)
{
    PSInput output;
    output.pos = mul(float4(input.pos, 1.0f), mvp);
    output.uv  = input.uv;
    return output;
}

Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

// 1-pixel smooth edge fadeout to eliminate hard "spiky" edges.
float EdgeFade(float2 uv)
{
    float2 fw = fwidth(uv);
    float4 d = float4(uv.x, 1.0 - uv.x, uv.y, 1.0 - uv.y);
    float4 w = float4(fw.x, fw.x, fw.y, fw.y);
    float4 f = saturate(d / max(w, 0.0001));
    return f.x * f.y * f.z * f.w;
}

float4 PSMain(PSInput input) : SV_Target
{
    float2 texUV = input.uv * (uvMax - uvMin) + uvMin;
    float4 col;
    if (blurAmount > 0.0005) {
        float2 dir = float2(blurAmount, 0.0);
        col  = tex.Sample(samp, texUV - dir * 0.50) * 0.10;
        col += tex.Sample(samp, texUV - dir * 0.25) * 0.20;
        col += tex.Sample(samp, texUV)               * 0.40;
        col += tex.Sample(samp, texUV + dir * 0.25) * 0.20;
        col += tex.Sample(samp, texUV + dir * 0.50) * 0.10;
    } else {
        col = tex.Sample(samp, texUV);
    }
    float  fade = EdgeFade(input.uv);
    col.rgb *= alpha * fade;
    col.a   *= alpha * fade;
    return col;
}

// Wallpaper backdrop PS.  Detects α=0 texels (Win11 < 25H2 Progman /
// WorkerW WGC capture leaves a transparent strip where the taskbar
// lives) and walks upward in UV.y in small steps to find the closest
// opaque texel, smearing it down to fill the strip.  No-op on
// Win11 25H2 / fully-opaque captures — the loop short-circuits on
// the first sample.
//
// `blurAmount` here carries the Appearance → Background blur intensity
// (0..1, config backgroundBlur %).  At 0 (default) the single-sample
// path runs and the feature costs nothing.  When enabled, a 16-tap
// fixed poisson-disk gather with MIP-PREFILTERED taps (SampleLevel at
// an LOD matched to the inter-tap spacing — the capture regenerates
// its full mipchain on every delivered frame) produces the frosted
// look: prefiltering removes ring banding without per-pixel noise
// rotation, which shimmered on moving live-wallpaper content.  Taps
// landing in the α=0 strip are weighted out so the smear-filled band
// never bleeds dark texels into the blur.
float4 SampleWallpaperFilled(float2 texUV)
{
    float4 col = tex.Sample(samp, texUV);
    if (col.a < 0.05) {
        const float kStep = 0.005;
        const int   kMax  = 32;
        for (int k = 1; k <= kMax; ++k) {
            float2 uv2 = float2(texUV.x, texUV.y - kStep * k);
            if (uv2.y < 0.0)
                break;
            float4 col2 = tex.Sample(samp, uv2);
            if (col2.a >= 0.05) {
                col = col2;
                break;
            }
        }
    }
    return col;
}

float4 PSWallpaper(PSInput input) : SV_Target
{
    float2 texUV = input.uv * (uvMax - uvMin) + uvMin;
    float4 col = SampleWallpaperFilled(texUV);

    if (blurAmount > 0.0005) {
        // Circular pixel-space radius: identical blur in x and y no
        // matter the wallpaper texture's aspect ratio.
        float texW, texH;
        tex.GetDimensions(texW, texH);
        float radiusPx = blurAmount * 0.028 * texH;
        float2 radiusUV = radiusPx / float2(max(texW, 1.0), max(texH, 1.0));

        // Prefilter LOD: each tap reads the mip whose texel footprint
        // matches the inter-tap spacing (~radius/4 for a 16-tap disk),
        // so the fixed taps tile the disk seamlessly — no banding on
        // static content and, unlike a per-pixel noise rotation, fully
        // deterministic and temporally stable on MOVING content (the
        // noise scintillated around moving edges of animated
        // wallpapers).  SampleLevel clamps to the chain automatically.
        float lod = max(0.0, log2(max(radiusPx * 0.25, 1.0)));

        // Fixed poisson disk (16 taps, unit radius).
        const float2 kDisk[16] = {
            float2( 0.2770, -0.1204), float2(-0.4405,  0.2251),
            float2( 0.1029,  0.5924), float2(-0.1520, -0.6104),
            float2( 0.6360,  0.2464), float2(-0.6957, -0.1817),
            float2( 0.4220, -0.6404), float2(-0.3007,  0.7482),
            float2( 0.8409,  0.4321), float2(-0.8564,  0.3877),
            float2( 0.1215,  0.9670), float2(-0.2506, -0.9520),
            float2( 0.7454, -0.7002), float2(-0.9998, -0.0672),
            float2( 0.5387,  0.8615), float2(-0.6656, -0.8032),
        };

        // Centre tap at the same LOD as the ring taps, so no sharp
        // mip-0 ghost bleeds through the blur.  If it lands in the α=0
        // strip, the strip-filled mip-0 sample stands in for it.
        float4 centre = tex.SampleLevel(samp, texUV, lod);
        float4 acc  = (centre.a >= 0.05) ? centre : col;
        float  wsum = 1.0;
        [unroll]
        for (int i = 0; i < 16; ++i) {
            float2 uv2 = clamp(texUV + kDisk[i] * radiusUV, uvMin, uvMax);
            float4 smp = tex.SampleLevel(samp, uv2, lod);
            // Skip transparent-strip taps — the centre sample (strip-
            // filled) already represents that region.
            float w = smp.a >= 0.05 ? 1.0 : 0.0;
            acc  += smp * w;
            wsum += w;
        }
        col = acc / wsum;
    }

    float fade = EdgeFade(input.uv);
    col.rgb *= alpha * fade;
    col.a   *= alpha * fade;
    return col;
}

float4 PSDim(PSInput input) : SV_Target
{
    return float4(0.0f, 0.0f, 0.0f, alpha);
}

// Placeholder for tiles without capture content — frosted glass tint.
float4 PSPlaceholder(PSInput input) : SV_Target
{
    float fade = EdgeFade(input.uv);
    float a = alpha * fade;
    return float4(0.12f * a, 0.14f * a, 0.18f * a, 0.65f * a);
}

// Diagnostic PS (Bug 11') — assumes the input texture is STRAIGHT alpha
// (not premultiplied) and converts to premultiplied output.  Used only for
// the taskbar layer as a hypothesis test for the #282832 leak.  Do NOT use
// globally without dump classification proving WGC textures are
// straight-alpha for that specific source.
float4 PSMainAssumeStraightAlpha(PSInput input) : SV_Target
{
    float2 texUV = input.uv * (uvMax - uvMin) + uvMin;
    float4 smp = tex.Sample(samp, texUV);

    float fade = EdgeFade(input.uv);
    float coverage = alpha * fade;

    float  outA   = smp.a * coverage;
    float3 outRGB = smp.rgb * smp.a * coverage;
    return float4(outRGB, outA);
}

// Debug-only solid-red quad (Bug 11' v8.4 Patch D `red` geometry test).
// Premultiplied red — verifies the taskbar quad's transform / m_taskbarRect
// without depending on any capture texture.
float4 PSDebugRed(PSInput input) : SV_Target
{
    float fade = EdgeFade(input.uv);
    float a = alpha * fade;
    return float4(a, 0.0f, 0.0f, a);
}
)";

// ---------------------------------------------------------------------------
// Vertex layout: position (3 floats) + texcoord (2 floats)
// ---------------------------------------------------------------------------
struct Vertex {
    float x, y, z;
    float u, v;
};

// Unit quad from (-0.5, -0.5) to (+0.5, +0.5), centered at origin.
static constexpr Vertex kQuadVertices[] = {
    { -0.5f, -0.5f, 0.0f,  0.0f, 1.0f },   // bottom-left
    { -0.5f, +0.5f, 0.0f,  0.0f, 0.0f },   // top-left
    { +0.5f, +0.5f, 0.0f,  1.0f, 0.0f },   // top-right
    { +0.5f, -0.5f, 0.0f,  1.0f, 1.0f },   // bottom-right
};

static constexpr UINT16 kQuadIndices[] = {
    0, 1, 2,
    0, 2, 3
};

// ---------------------------------------------------------------------------
static winrt::com_ptr<ID3DBlob> CompileShader(const char* src, UINT srcLen,
                                               const char* entry,
                                               const char* target)
{
    winrt::com_ptr<ID3DBlob> blob;
    winrt::com_ptr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(src, srcLen, nullptr, nullptr, nullptr,
                             entry, target, 0, 0, blob.put(), errors.put());
    if (FAILED(hr)) {
        if (errors)
            CKLog::Log(static_cast<const char*>(errors->GetBufferPointer()));
        return nullptr;
    }
    return blob;
}

// ---------------------------------------------------------------------------
bool QuadRenderer::Init(ID3D11Device* device)
{
    // --- Compile shaders ---------------------------------------------------
    auto vsBlob = CompileShader(kQuadHLSL, sizeof(kQuadHLSL) - 1,
                                "VSMain", "vs_5_0");
    if (!vsBlob) return false;

    auto psBlob = CompileShader(kQuadHLSL, sizeof(kQuadHLSL) - 1,
                                "PSMain", "ps_5_0");
    if (!psBlob) return false;

    HRESULT hr = device->CreateVertexShader(
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        nullptr, m_vs.put());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(
        psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
        nullptr, m_ps.put());
    if (FAILED(hr)) return false;

    auto psDimBlob = CompileShader(kQuadHLSL, sizeof(kQuadHLSL) - 1,
                                    "PSDim", "ps_5_0");
    if (!psDimBlob) return false;

    hr = device->CreatePixelShader(
        psDimBlob->GetBufferPointer(), psDimBlob->GetBufferSize(),
        nullptr, m_psDim.put());
    if (FAILED(hr)) return false;

    auto psPlaceholderBlob = CompileShader(kQuadHLSL, sizeof(kQuadHLSL) - 1,
                                            "PSPlaceholder", "ps_5_0");
    if (!psPlaceholderBlob) return false;

    hr = device->CreatePixelShader(
        psPlaceholderBlob->GetBufferPointer(), psPlaceholderBlob->GetBufferSize(),
        nullptr, m_psPlaceholder.put());
    if (FAILED(hr)) return false;

    auto psWallpaperBlob = CompileShader(kQuadHLSL, sizeof(kQuadHLSL) - 1,
                                          "PSWallpaper", "ps_5_0");
    if (!psWallpaperBlob) return false;
    hr = device->CreatePixelShader(
        psWallpaperBlob->GetBufferPointer(), psWallpaperBlob->GetBufferSize(),
        nullptr, m_psWallpaper.put());
    if (FAILED(hr)) return false;

#ifdef CKFLIP_DEBUG_TASKBAR
    // Bug 11' diagnostic shader (debug builds only).
    auto psAsabBlob = CompileShader(kQuadHLSL, sizeof(kQuadHLSL) - 1,
                                    "PSMainAssumeStraightAlpha", "ps_5_0");
    if (!psAsabBlob) return false;
    hr = device->CreatePixelShader(
        psAsabBlob->GetBufferPointer(), psAsabBlob->GetBufferSize(),
        nullptr, m_psAssumeStraightAlpha.put());
    if (FAILED(hr)) return false;

    // Patch D `red` geometry-test shader (debug builds only).
    auto psRedBlob = CompileShader(kQuadHLSL, sizeof(kQuadHLSL) - 1,
                                   "PSDebugRed", "ps_5_0");
    if (!psRedBlob) return false;
    hr = device->CreatePixelShader(
        psRedBlob->GetBufferPointer(), psRedBlob->GetBufferSize(),
        nullptr, m_psDebugRed.put());
    if (FAILED(hr)) return false;
#endif

    // --- Input layout ------------------------------------------------------
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device->CreateInputLayout(
        layout, _countof(layout),
        vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
        m_inputLayout.put());
    if (FAILED(hr)) return false;

    // --- Vertex buffer -----------------------------------------------------
    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(kQuadVertices);
    vbDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = kQuadVertices;
    hr = device->CreateBuffer(&vbDesc, &vbData, m_vb.put());
    if (FAILED(hr)) return false;

    // --- Index buffer ------------------------------------------------------
    D3D11_BUFFER_DESC ibDesc = {};
    ibDesc.ByteWidth = sizeof(kQuadIndices);
    ibDesc.Usage     = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;

    D3D11_SUBRESOURCE_DATA ibData = {};
    ibData.pSysMem = kQuadIndices;
    hr = device->CreateBuffer(&ibDesc, &ibData, m_ib.put());
    if (FAILED(hr)) return false;

    // --- Constant buffer (dynamic) -----------------------------------------
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth      = sizeof(CBPerDraw);
    cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = device->CreateBuffer(&cbDesc, nullptr, m_cb.put());
    if (FAILED(hr)) return false;

    // --- Anisotropic-clamp sampler (crisp text on tilted quads) -------------
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter   = D3D11_FILTER_ANISOTROPIC;
    sampDesc.MaxAnisotropy = 16;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.MaxLOD   = D3D11_FLOAT32_MAX;

    hr = device->CreateSamplerState(&sampDesc, m_sampler.put());
    if (FAILED(hr)) return false;

    // --- Point-clamp sampler (config antialiasing = false) ------------------
    D3D11_SAMPLER_DESC pointDesc = sampDesc;
    pointDesc.Filter        = D3D11_FILTER_MIN_MAG_MIP_POINT;
    pointDesc.MaxAnisotropy = 1;

    hr = device->CreateSamplerState(&pointDesc, m_samplerPoint.put());
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
void QuadRenderer::DrawWallpaper(ID3D11DeviceContext* ctx,
                                  ID3D11ShaderResourceView* srv,
                                  const QuadDrawCall& draw)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;
    auto* cb    = static_cast<CBPerDraw*>(mapped.pData);
    cb->mvp     = draw.mvp;
    cb->alpha   = draw.alpha;
    cb->blurAmount = draw.blurAmount;
    cb->uvMinX  = draw.uvMinX;
    cb->uvMinY  = draw.uvMinY;
    cb->uvMaxX  = draw.uvMaxX;
    cb->uvMaxY  = draw.uvMaxY;
    cb->_pad[0] = 0.0f;
    cb->_pad[1] = 0.0f;
    ctx->Unmap(m_cb.get(), 0);

    ctx->IASetInputLayout(m_inputLayout.get());
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = m_vb.get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { m_cb.get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);

    ctx->PSSetShader(m_psWallpaper.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* samplers[] = { ActiveSampler() };
    ctx->PSSetSamplers(0, 1, samplers);

    ctx->DrawIndexed(6, 0, 0);
}

// ---------------------------------------------------------------------------
void QuadRenderer::Draw(ID3D11DeviceContext* ctx,
                         ID3D11ShaderResourceView* srv,
                         const QuadDrawCall& draw)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb    = static_cast<CBPerDraw*>(mapped.pData);
    cb->mvp     = draw.mvp;
    cb->alpha   = draw.alpha;
    cb->blurAmount = draw.blurAmount;
    cb->uvMinX  = draw.uvMinX;
    cb->uvMinY  = draw.uvMinY;
    cb->uvMaxX  = draw.uvMaxX;
    cb->uvMaxY  = draw.uvMaxY;
    cb->_pad[0] = 0.0f;
    cb->_pad[1] = 0.0f;
    ctx->Unmap(m_cb.get(), 0);

    // Bind pipeline state.
    ctx->IASetInputLayout(m_inputLayout.get());
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = m_vb.get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { m_cb.get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);

    ctx->PSSetShader(m_ps.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* samplers[] = { ActiveSampler() };
    ctx->PSSetSamplers(0, 1, samplers);

    ctx->DrawIndexed(6, 0, 0);
}

#ifdef CKFLIP_DEBUG_TASKBAR
// ---------------------------------------------------------------------------
// Bug 11' diagnostic — clone of Draw() that binds the straight-alpha PS.
void QuadRenderer::DrawAssumeStraightAlpha(ID3D11DeviceContext* ctx,
                                            ID3D11ShaderResourceView* srv,
                                            const QuadDrawCall& draw)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb    = static_cast<CBPerDraw*>(mapped.pData);
    cb->mvp     = draw.mvp;
    cb->alpha   = draw.alpha;
    cb->blurAmount = draw.blurAmount;
    cb->uvMinX  = draw.uvMinX;
    cb->uvMinY  = draw.uvMinY;
    cb->uvMaxX  = draw.uvMaxX;
    cb->uvMaxY  = draw.uvMaxY;
    cb->_pad[0] = 0.0f;
    cb->_pad[1] = 0.0f;
    ctx->Unmap(m_cb.get(), 0);

    ctx->IASetInputLayout(m_inputLayout.get());
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = m_vb.get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { m_cb.get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);

    ctx->PSSetShader(m_psAssumeStraightAlpha.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbs);
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* samplers[] = { ActiveSampler() };
    ctx->PSSetSamplers(0, 1, samplers);

    ctx->DrawIndexed(6, 0, 0);
}

// ---------------------------------------------------------------------------
// Patch D `red` geometry test — solid-red quad, no texture.
void QuadRenderer::DrawDebugRed(ID3D11DeviceContext* ctx,
                                 const QuadDrawCall& draw)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb        = static_cast<CBPerDraw*>(mapped.pData);
    cb->mvp         = draw.mvp;
    cb->alpha       = draw.alpha;
    cb->blurAmount  = 0.0f;
    cb->uvMinX      = 0.0f;
    cb->uvMinY      = 0.0f;
    cb->uvMaxX      = 1.0f;
    cb->uvMaxY      = 1.0f;
    cb->_pad[0]     = 0.0f;
    cb->_pad[1]     = 0.0f;
    ctx->Unmap(m_cb.get(), 0);

    ctx->IASetInputLayout(m_inputLayout.get());
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = m_vb.get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { m_cb.get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);

    ctx->PSSetShader(m_psDebugRed.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    ctx->DrawIndexed(6, 0, 0);
}
#endif

// ---------------------------------------------------------------------------
void QuadRenderer::DrawDim(ID3D11DeviceContext* ctx, float dimAlpha)
{
    // Scale unit quad [-0.5,+0.5] to NDC [-1,+1].
    QuadDrawCall draw;
    DirectX::XMStoreFloat4x4(&draw.mvp,
        DirectX::XMMatrixScaling(2.0f, 2.0f, 1.0f));
    draw.alpha = dimAlpha;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    auto* cb        = static_cast<CBPerDraw*>(mapped.pData);
    cb->mvp         = draw.mvp;
    cb->alpha       = draw.alpha;
    cb->blurAmount  = 0.0f;
    cb->uvMinX      = 0.0f;
    cb->uvMinY      = 0.0f;
    cb->uvMaxX      = 1.0f;
    cb->uvMaxY      = 1.0f;
    cb->_pad[0]     = 0.0f;
    cb->_pad[1]     = 0.0f;
    ctx->Unmap(m_cb.get(), 0);

    ctx->IASetInputLayout(m_inputLayout.get());
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = m_vb.get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { m_cb.get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);

    ctx->PSSetShader(m_psDim.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    // Clear any bound SRV from previous draw.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    ctx->DrawIndexed(6, 0, 0);
}

// ---------------------------------------------------------------------------
void QuadRenderer::DrawPlaceholder(ID3D11DeviceContext* ctx,
                                    const QuadDrawCall& draw)
{
    // Update constant buffer.
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    HRESULT hr = ctx->Map(m_cb.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    auto* cb        = static_cast<CBPerDraw*>(mapped.pData);
    cb->mvp         = draw.mvp;
    cb->alpha       = draw.alpha;
    cb->blurAmount  = 0.0f;
    cb->uvMinX      = 0.0f;
    cb->uvMinY      = 0.0f;
    cb->uvMaxX      = 1.0f;
    cb->uvMaxY      = 1.0f;
    cb->_pad[0]     = 0.0f;
    cb->_pad[1]     = 0.0f;
    ctx->Unmap(m_cb.get(), 0);

    ctx->IASetInputLayout(m_inputLayout.get());
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vb = m_vb.get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetIndexBuffer(m_ib.get(), DXGI_FORMAT_R16_UINT, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ctx->VSSetShader(m_vs.get(), nullptr, 0);
    ID3D11Buffer* cbs[] = { m_cb.get() };
    ctx->VSSetConstantBuffers(0, 1, cbs);

    ctx->PSSetShader(m_psPlaceholder.get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, cbs);

    // Clear any bound SRV.
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);

    ctx->DrawIndexed(6, 0, 0);
}
