// ---------------------------------------------------------------------------
// Every tile, the wallpaper, the dim layer and the labels are the same textured
// unit quad drawn with different matrices.  This is that quad, its shaders, and
// the state cache that keeps a frame's worth of draws from re-sending the same
// context calls forty times over.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#include <d3d11.h>
#include <DirectXMath.h>
#include <winrt/base.h>

/// Per-draw parameters for a textured quad.
struct QuadDrawCall {
    DirectX::XMFLOAT4X4 mvp;
    float alpha = 1.0f;
    float blurAmount = 0.0f;  // Draw(): horizontal motion blur in UV space;
                              // DrawWallpaper(): background blur intensity 0..1
    float uvMinX = 0.0f, uvMinY = 0.0f;  // UV crop: top-left (default: full texture)
    float uvMaxX = 1.0f, uvMaxY = 1.0f;  // UV crop: bottom-right
};

/// Draws textured quads using a unit-quad mesh and per-draw constant buffer.
/// Shaders are compiled from embedded HLSL at Init() time.
class QuadRenderer {
public:
    QuadRenderer() = default;
    ~QuadRenderer() = default;

    QuadRenderer(const QuadRenderer&) = delete;
    QuadRenderer& operator=(const QuadRenderer&) = delete;

    /// Compile shaders, create vertex/index/constant buffers and sampler.
    bool Init(ID3D11Device* device);

    /// Forget what this renderer believes is bound on the context.  Call
    /// once per frame, after Renderer::BeginFrame().
    ///
    /// Every quad draws from the same mesh with the same vertex shader and
    /// the same constant buffer, so re-sending that state for each of the
    /// forty-odd draws in a frame was forty-odd redundant context calls —
    /// and on this device they are not cheap: it runs with the D3D11
    /// internal lock on (shared with the WGC capture threads), so every
    /// call takes a lock the capture path also wants.  The draws now send
    /// only what actually changed since the previous one, which halves the
    /// submission cost of a frame and, more usefully, halves how often the
    /// render thread contends with capture for the device.
    ///
    /// The cache spans a frame and no longer, because nothing may be
    /// assumed to survive Present.  Within a frame the only other user of
    /// the context is the capture path, whose CopySubresourceRegion /
    /// GenerateMips are resource operations that leave the pipeline
    /// bindings alone (verified against interleaved draws).
    void ResetStateCache();

    /// Toggle tile antialiasing (config `antialiasing`).  True (default)
    /// keeps the original anisotropic sampler; false switches the textured
    /// draws to point filtering.  Cheap — just selects which prebuilt
    /// sampler state is bound.
    void SetAntialiasing(bool enabled) { m_antialiasing = enabled; }

    /// Draw one textured quad.
    void Draw(ID3D11DeviceContext* ctx,
              ID3D11ShaderResourceView* srv,
              const QuadDrawCall& draw);

    /// Draw a placeholder quad (no texture, glass-like tint) when capture is unavailable.
    void DrawPlaceholder(ID3D11DeviceContext* ctx, const QuadDrawCall& draw);

    /// Draw a wallpaper backdrop quad.  Identical to Draw() except the PS
    /// detects α=0 texels (Win11 < 25H2 Progman/WorkerW capture leaves a
    /// transparent strip where the taskbar lives) and falls back to
    /// sampling the closest opaque pixel above, smearing the last opaque
    /// row down to fill the strip.  Identity behaviour on 25H2 / non-
    /// wallpaper textures (texels already opaque).
    void DrawWallpaper(ID3D11DeviceContext* ctx,
                       ID3D11ShaderResourceView* srv,
                       const QuadDrawCall& draw);

    /// Draw a tile's glass floor reflection (config `reflections`).  The
    /// caller provides the mirrored MVP (quad shifted one tile-height down
    /// in unit-quad space) and a vertically flipped UV crop; the dedicated
    /// PS fades the mirror out quadratically away from the tile's bottom
    /// edge.  `draw.alpha` should carry slotAlpha * reflection strength.
    void DrawReflection(ID3D11DeviceContext* ctx,
                        ID3D11ShaderResourceView* srv,
                        const QuadDrawCall& draw);

#ifdef CKFLIP_DEBUG_TASKBAR
    /// Diagnostic draw: identical to Draw() but binds a PS that treats the
    /// input texture as STRAIGHT alpha and converts it to premultiplied
    /// output.  Taskbar-layer-only hypothesis test for the #282832 leak.
    /// Not for general use without a dump classification proving the source
    /// really is straight-alpha.
    void DrawAssumeStraightAlpha(ID3D11DeviceContext* ctx,
                                 ID3D11ShaderResourceView* srv,
                                 const QuadDrawCall& draw);

    /// Solid-red quad (no texture) for the `red` taskbar geometry test.
    /// Debug builds only.
    void DrawDebugRed(ID3D11DeviceContext* ctx, const QuadDrawCall& draw);
#endif

private:
    struct alignas(16) CBPerDraw {
        DirectX::XMFLOAT4X4 mvp;
        float alpha;
        float blurAmount;
        float uvMinX, uvMinY;
        float uvMaxX, uvMaxY;
        float _pad[2];
    };

    /// The one draw path every public Draw* is a thin wrapper over: fill the
    /// per-draw constants, bring the shared state up if this is the frame's
    /// first quad, send the pixel shader / texture / sampler only when they
    /// differ from the last quad, and draw.
    void Submit(ID3D11DeviceContext* ctx,
                ID3D11PixelShader* ps,
                ID3D11ShaderResourceView* srv,
                const QuadDrawCall& draw,
                float blurAmount,
                bool fullUV);

    /// Bind the state shared by every quad.  No-op after the frame's first
    /// draw — see ResetStateCache().
    void BindShared(ID3D11DeviceContext* ctx);

    winrt::com_ptr<ID3D11VertexShader>  m_vs;
    winrt::com_ptr<ID3D11PixelShader>   m_ps;
    winrt::com_ptr<ID3D11PixelShader>   m_psPlaceholder;
    winrt::com_ptr<ID3D11PixelShader>   m_psWallpaper;
    winrt::com_ptr<ID3D11PixelShader>   m_psReflection;
#ifdef CKFLIP_DEBUG_TASKBAR
    winrt::com_ptr<ID3D11PixelShader>   m_psAssumeStraightAlpha;
    winrt::com_ptr<ID3D11PixelShader>   m_psDebugRed;
#endif
    winrt::com_ptr<ID3D11InputLayout>   m_inputLayout;
    ID3D11SamplerState* ActiveSampler() const
    {
        return (m_antialiasing || !m_samplerPoint) ? m_sampler.get()
                                                   : m_samplerPoint.get();
    }

    winrt::com_ptr<ID3D11Buffer>        m_vb;
    winrt::com_ptr<ID3D11Buffer>        m_ib;
    winrt::com_ptr<ID3D11Buffer>        m_cb;
    winrt::com_ptr<ID3D11SamplerState>  m_sampler;       // anisotropic (AA on)
    winrt::com_ptr<ID3D11SamplerState>  m_samplerPoint;  // point (AA off)
    bool                                m_antialiasing = true;

    // What this renderer last sent to the context, for the frame it sent it
    // in.  Raw pointers deliberately: they are compared, never dereferenced,
    // and the objects they name are owned by the com_ptr members above or by
    // the caller for the duration of the draw.
    bool                      m_sharedBound  = false;
    ID3D11PixelShader*        m_boundPS      = nullptr;
    ID3D11ShaderResourceView* m_boundSRV     = nullptr;
    ID3D11SamplerState*       m_boundSampler = nullptr;
};
