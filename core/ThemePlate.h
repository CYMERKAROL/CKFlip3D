#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------------------
// The aero-glass plate the overlay draws its on-screen text on.
//
// Two surfaces need it — the selected-window label above the front tile and
// the search field below the cascade — and they must look like the same
// object, because they ARE the same object as far as the user is concerned:
// one piece of CKFlip3D chrome, styled to whichever CKSettings theme is
// selected.  Keeping the style table and the painter in one place is what
// guarantees that; the alternative is two tables that drift apart the first
// time a theme is touched.
//
// Everything composes into a PREMULTIPLIED float BGRA canvas and is packed to
// bytes at the end, so overlapping passes (plate, icon, glyph shadow, glyph)
// blend correctly at any coverage instead of clipping at 8 bits per step.
// ---------------------------------------------------------------------------
namespace ThemePlate {

/// Per-theme plate/text styling, matching the CKSettings application themes
/// (config `appTheme`).  Colours are sampled from the corresponding
/// Theme/Themes/*.xaml brushes.
struct Style {
    float topR, topG, topB;        // fill gradient top (flat: == bottom)
    float botR, botG, botB;        // fill gradient bottom
    float fillAlpha;               // plate opacity
    float sheenAdd;                // white added at the very top of the sheen
    float sheenExtent;             // fraction of height the sheen covers (0 = flat)
    float borderR, borderG, borderB;
    float borderMix;               // colour pull toward border tone at the edge
    float borderAlphaBoost;
    float radiusPx;                // corner radius at uiScale 1
    float textR, textG, textB;
    float shadowR, shadowG, shadowB;
    float shadowBox, shadowNoBox;  // shadow strength with/without the plate
};

/// Style for an `appTheme` index (0 Skeuo Dark, 1 Skeuo White, 2 Minimal
/// Dark, 3 Minimal White, 4 Glassmorphism).  Out-of-range clamps.
const Style& Get(int appTheme);

/// Premultiplied source-over of one pixel: dst = src + dst * (1 - srcA).
/// `canvas` holds 4 floats (B, G, R, A) per pixel; `idx` is the pixel index.
inline void Over(std::vector<float>& canvas, size_t idx,
                 float b, float g, float r, float a)
{
    float* d = &canvas[idx * 4u];
    const float inv = 1.0f - a;
    d[0] = b + d[0] * inv;
    d[1] = g + d[1] * inv;
    d[2] = r + d[2] * inv;
    d[3] = a + d[3] * inv;
}

/// Paint the themed plate over the whole canvas: a vertical gradient with an
/// optional top sheen, a 1-px border and rounded corners.  Translucent, so the
/// cascade shows through, tinted for text contrast.
void PaintPlate(std::vector<float>& canvas, int width, int height,
                const Style& st, float uiScale);

/// Composite a white-on-black GDI coverage mask as a soft drop shadow plus the
/// themed glyph pass.  `mask` is a width×height BGRA DIB (only the maximum
/// channel is read), which is exactly what DrawTextW into a zeroed DIB yields.
void CompositeTextMask(std::vector<float>& canvas, int width, int height,
                       const uint8_t* mask, const Style& st,
                       float uiScale, bool haveBox);

/// Pack the float canvas to premultiplied BGRA bytes.
void Pack(const std::vector<float>& canvas, std::vector<uint8_t>& out);

/// Upload packed BGRA bytes as an immutable texture + SRV.
bool CreateTexture(ID3D11Device* device, const std::vector<uint8_t>& packed,
                   int width, int height,
                   winrt::com_ptr<ID3D11Texture2D>& outTex,
                   winrt::com_ptr<ID3D11ShaderResourceView>& outSrv);

} // namespace ThemePlate
