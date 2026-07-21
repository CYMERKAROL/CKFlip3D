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

    /// Toggle tile antialiasing (config `antialiasing`).  True (default)
    /// keeps the original anisotropic sampler; false switches the textured
    /// draws to point filtering.  Cheap — just selects which prebuilt
    /// sampler state is bound.
    void SetAntialiasing(bool enabled) { m_antialiasing = enabled; }

    /// Draw one textured quad.
    void Draw(ID3D11DeviceContext* ctx,
              ID3D11ShaderResourceView* srv,
              const QuadDrawCall& draw);

    /// Draw a fullscreen dim quad (no texture, solid black with alpha).
    void DrawDim(ID3D11DeviceContext* ctx, float alpha);

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

#ifdef CKFLIP_DEBUG_TASKBAR
    /// Bug 11' diagnostic — identical to Draw() but binds a PS that treats
    /// the input texture as STRAIGHT alpha and converts it to premultiplied
    /// output.  Taskbar-layer-only hypothesis test for the #282832 leak;
    /// see repair prompt v8.2 §11.  Not for global use without dump
    /// classification proving the source is straight-alpha.
    void DrawAssumeStraightAlpha(ID3D11DeviceContext* ctx,
                                 ID3D11ShaderResourceView* srv,
                                 const QuadDrawCall& draw);

    /// Bug 11' v8.4 Patch D — solid-red quad (no texture) for the `red`
    /// taskbar geometry test.  Debug builds only.
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

    winrt::com_ptr<ID3D11VertexShader>  m_vs;
    winrt::com_ptr<ID3D11PixelShader>   m_ps;
    winrt::com_ptr<ID3D11PixelShader>   m_psDim;
    winrt::com_ptr<ID3D11PixelShader>   m_psPlaceholder;
    winrt::com_ptr<ID3D11PixelShader>   m_psWallpaper;
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
};
