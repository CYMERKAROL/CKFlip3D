// ---------------------------------------------------------------------------
// The per-theme style table and the painter that turns it into pixels: plate
// body, sheen, border, then the text mask composited on top.  All of it in
// premultiplied float, packed to bytes only once at the very end.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "ThemePlate.h"
#include <algorithm>
#include <cmath>

namespace ThemePlate {
namespace {

// Index = AppConfig::appTheme (0 Skeuo Dark, 1 Skeuo White, 2 Minimal
// Dark, 3 Minimal White, 4 Glassmorphism).
constexpr Style kStyles[5] = {
    // 0 — Skeuomorphic Dark: steel gradient + aero sheen (#202A36 family).
    {  62.f,  74.f,  92.f,   26.f,  30.f,  40.f,  0.55f, 50.f, 0.45f,
      215.f, 225.f, 238.f,  0.45f, 0.16f,  9.0f,
      243.f, 246.f, 250.f,    0.f,   0.f,   0.f,  0.38f, 0.60f },
    // 1 — Skeuomorphic White: bright glass (#FCFEFF cards, #B9C8D6 border).
    { 252.f, 253.f, 255.f,  212.f, 220.f, 229.f,  0.70f, 26.f, 0.50f,
      146.f, 162.f, 178.f,  0.50f, 0.12f,  9.0f,
       27.f,  39.f,  51.f,  255.f, 255.f, 255.f,  0.42f, 0.62f },
    // 2 — Minimalism Dark: flat near-black (#101214), hairline border.
    {  22.f,  24.f,  27.f,   16.f,  18.f,  20.f,  0.66f,  0.f, 0.00f,
      120.f, 126.f, 134.f,  0.30f, 0.08f,  4.5f,
      242.f, 245.f, 248.f,    0.f,   0.f,   0.f,  0.30f, 0.60f },
    // 3 — Minimalism White: flat white (#FFFFFF, #DDE2E7 border).
    { 250.f, 251.f, 253.f,  243.f, 245.f, 248.f,  0.74f,  0.f, 0.00f,
      178.f, 186.f, 195.f,  0.35f, 0.10f,  4.5f,
       21.f,  25.f,  29.f,  255.f, 255.f, 255.f,  0.40f, 0.62f },
    // 4 — Glassmorphism: highly translucent dark glass (#1A2430 base),
    //     broad sheen, luminous white border, large radius.
    {  46.f,  60.f,  78.f,   22.f,  32.f,  46.f,  0.46f, 58.f, 0.58f,
      240.f, 246.f, 252.f,  0.55f, 0.20f, 13.0f,
      250.f, 252.f, 255.f,    0.f,   0.f,   0.f,  0.44f, 0.62f },
};

} // namespace

const Style& Get(int appTheme)
{
    return kStyles[std::clamp(appTheme, 0, 4)];
}

void PaintPlate(std::vector<float>& canvas, int width, int height,
                const Style& st, float uiScale)
{
    if (width <= 0 || height <= 0)
        return;

    const float radius   = st.radiusPx * uiScale;
    const float borderPx = 1.4f * uiScale;
    for (int yy = 0; yy < height; ++yy) {
        const float t = height > 1
            ? static_cast<float>(yy) / static_cast<float>(height - 1)
            : 0.0f;
        float fr = st.topR + (st.botR - st.topR) * t;
        float fg = st.topG + (st.botG - st.topG) * t;
        float fb = st.topB + (st.botB - st.topB) * t;
        // Sheen band over the upper part (skeuo/glass themes only).
        if (st.sheenExtent > 0.0f && t < st.sheenExtent) {
            const float s = (st.sheenExtent - t) / st.sheenExtent;
            fr += (st.sheenAdd + 4.0f) * s;
            fg += (st.sheenAdd + 4.0f) * s;
            fb += (st.sheenAdd + 8.0f) * s;
        }
        for (int xx = 0; xx < width; ++xx) {
            const float fx = static_cast<float>(xx) + 0.5f;
            const float fy = static_cast<float>(yy) + 0.5f;
            const float cx = std::clamp(fx, radius,
                                        static_cast<float>(width) - radius);
            const float cy = std::clamp(fy, radius,
                                        static_cast<float>(height) - radius);
            const float dx = fx - cx, dy = fy - cy;
            // Signed inside-distance to the rounded-rect boundary.
            float inDist;
            if (dx == 0.0f && dy == 0.0f) {
                inDist = (std::min)(
                    (std::min)(fx, static_cast<float>(width)  - fx),
                    (std::min)(fy, static_cast<float>(height) - fy));
            } else {
                inDist = radius - std::sqrt(dx * dx + dy * dy);
            }
            const float cov = std::clamp(inDist + 0.5f, 0.0f, 1.0f);
            if (cov <= 0.0f)
                continue;

            float r = fr, g = fg, b = fb;
            float a = st.fillAlpha;
            // Border hugging the outer edge.
            const float bi = std::clamp(
                (borderPx - inDist) / borderPx + 0.35f, 0.0f, 1.0f);
            if (bi > 0.0f) {
                const float bw = bi * st.borderMix;
                r += (st.borderR - r) * bw;
                g += (st.borderG - g) * bw;
                b += (st.borderB - b) * bw;
                a += st.borderAlphaBoost * bi;
            }
            const float aa = a * cov;
            Over(canvas, static_cast<size_t>(yy) * width + xx,
                 (b / 255.0f) * aa, (g / 255.0f) * aa,
                 (r / 255.0f) * aa, aa);
        }
    }
}

void CompositeTextMask(std::vector<float>& canvas, int width, int height,
                       const uint8_t* mask, const Style& st,
                       float uiScale, bool haveBox)
{
    if (!mask || width <= 0 || height <= 0)
        return;

    auto maskAt = [&](int xx, int yy) -> float {
        if (xx < 0 || yy < 0 || xx >= width || yy >= height)
            return 0.0f;
        const uint8_t* p = mask + (static_cast<size_t>(yy) * width + xx) * 4u;
        int mx = p[0];
        if (p[1] > mx) mx = p[1];
        if (p[2] > mx) mx = p[2];
        return static_cast<float>(mx) / 255.0f;
    };

    const int   shadowOff      = (std::max)(1, static_cast<int>(1.5f * uiScale));
    const float shadowStrength = haveBox ? st.shadowBox : st.shadowNoBox;
    // Theme text + shadow (light themes cast a white halo so the dark glyphs
    // stay readable without a plate).
    const float tb  = st.textB / 255.0f;
    const float tg  = st.textG / 255.0f;
    const float tr2 = st.textR / 255.0f;
    const float sb  = st.shadowB / 255.0f;
    const float sg  = st.shadowG / 255.0f;
    const float sr  = st.shadowR / 255.0f;
    for (int yy = 0; yy < height; ++yy) {
        for (int xx = 0; xx < width; ++xx) {
            const size_t idx = static_cast<size_t>(yy) * width + xx;
            const float sh = maskAt(xx - shadowOff, yy - shadowOff)
                           * shadowStrength;
            if (sh > 0.0f)
                Over(canvas, idx, sb * sh, sg * sh, sr * sh, sh);
            const float m = maskAt(xx, yy);
            if (m > 0.0f)
                Over(canvas, idx, tb * m, tg * m, tr2 * m, m);
        }
    }
}

void Pack(const std::vector<float>& canvas, std::vector<uint8_t>& out)
{
    out.resize(canvas.size());
    for (size_t i = 0; i < canvas.size(); ++i)
        out[i] = static_cast<uint8_t>(
            std::clamp(canvas[i], 0.0f, 1.0f) * 255.0f + 0.5f);
}

bool CreateTexture(ID3D11Device* device, const std::vector<uint8_t>& packed,
                   int width, int height,
                   winrt::com_ptr<ID3D11Texture2D>& outTex,
                   winrt::com_ptr<ID3D11ShaderResourceView>& outSrv)
{
    if (!device || width <= 0 || height <= 0
        || packed.size() < static_cast<size_t>(width) * height * 4u)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width            = static_cast<UINT>(width);
    desc.Height           = static_cast<UINT>(height);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem     = packed.data();
    init.SysMemPitch = static_cast<UINT>(width) * 4u;

    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateTexture2D(&desc, &init, tex.put())))
        return false;
    if (FAILED(device->CreateShaderResourceView(tex.get(), nullptr, srv.put())))
        return false;

    outTex = std::move(tex);
    outSrv = std::move(srv);
    return true;
}

} // namespace ThemePlate
