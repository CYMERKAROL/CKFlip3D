// ---------------------------------------------------------------------------
// Painting the search field with GDI into a premultiplied BGRA texture, on the
// shared theme plate.  The texture is rebuilt only when the text, the theme or
// the UI scale actually change, so an idle frame costs one textured quad.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "SearchBox.h"
#include "ThemePlate.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr const wchar_t* kPlaceholder = L"Type to search...";
constexpr const wchar_t* kNoMatchNote = L"no matches";

// The placeholder and the "no matches" note sit back from the glass: neither
// is something the user typed, so neither should read as strongly as the
// query does.
constexpr float kPlaceholderOpacity = 0.52f;
constexpr float kNoteOpacity        = 0.60f;

// The field's natural size — what "100 %" means.  The metrics below were
// originally dialled in a third smaller and read as cramped next to the
// cascade, so the whole field was re-based rather than leaving everyone to
// discover they had to set 130 % by hand.  Folded into the scale so every
// derived metric (font, padding, corner radius, magnifier stroke, caret)
// moves with it and nothing has to be re-tuned individually.
constexpr float kBaseScale = 1.30f;

} // namespace

void SearchBox::Reset()
{
    m_srv     = nullptr;
    m_texture = nullptr;
    m_built   = false;
    m_builtQuery.clear();
    m_builtTheme = -1;
    m_builtScale = 0.0f;
    m_texW = 0;
    m_texH = 0;
}

void SearchBox::Update(ID3D11Device* device, const std::wstring& query,
                       bool haveMatches, int appTheme, bool showBox,
                       int scalePercent, float cascadeH)
{
    // UI scale keys off the cascade host height, exactly like the selected-
    // window label, so both have the same physical presence at 1080p and 4K —
    // then the user's own size setting multiplies it.
    const float uiScale = std::clamp(cascadeH / 1080.0f, 1.0f, 2.5f)
                        * kBaseScale
                        * (std::clamp(scalePercent, 50, 200) / 100.0f);
    const int   theme   = std::clamp(appTheme, 0, 4);

    if (m_built
        && query == m_builtQuery
        && haveMatches == m_builtMatches
        && theme == m_builtTheme
        && showBox == m_builtBox
        && std::fabs(uiScale - m_builtScale) < 0.001f)
        return;   // up to date

    if (!Build(device, query, haveMatches, theme, showBox, uiScale)) {
        Reset();
        return;
    }

    m_builtQuery   = query;
    m_builtMatches = haveMatches;
    m_builtTheme   = theme;
    m_builtBox     = showBox;
    m_builtScale   = uiScale;
    m_built        = true;
}

// ---------------------------------------------------------------------------
// One text pass: render `text` into a white-on-black coverage mask and
// composite it through the shared theme painter.  Long text is shifted LEFT so
// its tail stays visible (DT_END_ELLIPSIS would hide the newest characters,
// which is the opposite of what a caret wants), and the whole pass is clipped
// to [left, right) so a shifted head cannot spill into the magnifier.
// ---------------------------------------------------------------------------
void SearchBox::PaintText(HDC memDC, std::vector<float>& canvas,
                          int width, int height,
                          const std::wstring& text, int left, int right,
                          bool rightAlign, float opacity,
                          const ThemePlate::Style& style, float uiScale,
                          bool haveBox, int* outTextWidth)
{
    if (text.empty() || right <= left)
        return;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        return;
    }

    HGDIOBJ prev = SelectObject(memDC, dib);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));

    const int fieldW = right - left;
    SIZE ext{};
    GetTextExtentPoint32W(memDC, text.c_str(),
                          static_cast<int>(text.size()), &ext);
    if (outTextWidth)
        *outTextWidth = static_cast<int>(ext.cx);

    int x = left;
    if (rightAlign)
        x = right - static_cast<int>(ext.cx);
    else
        x = left - (std::max)(0, static_cast<int>(ext.cx) - fieldW);

    const int padY = (height - static_cast<int>(ext.cy)) / 2;
    RECT tr{ x, padY, x + (std::max)(static_cast<int>(ext.cx), fieldW),
             height - padY };
    HRGN clip = CreateRectRgn(left, 0, right, height);
    SelectClipRgn(memDC, clip);
    DrawTextW(memDC, text.c_str(), -1, &tr,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_NOCLIP);
    SelectClipRgn(memDC, nullptr);
    DeleteObject(clip);
    GdiFlush();
    SelectObject(memDC, prev);

    auto* mask = static_cast<uint8_t*>(bits);
    // Dimming scales the coverage MASK itself, so the drop shadow recedes with
    // the glyph instead of the glyph losing only its fill.
    if (opacity < 1.0f) {
        const size_t bytes = static_cast<size_t>(width) * height * 4u;
        for (size_t i = 0; i < bytes; ++i)
            mask[i] = static_cast<uint8_t>(mask[i] * opacity);
    }
    ThemePlate::CompositeTextMask(canvas, width, height, mask, style,
                                  uiScale, haveBox);

    DeleteObject(dib);
}

bool SearchBox::Build(ID3D11Device* device, const std::wstring& query,
                      bool haveMatches, int theme, bool showBox, float uiScale)
{
    if (!device)
        return false;

    const ThemePlate::Style& st = ThemePlate::Get(theme);

    const int padX      = static_cast<int>(16.0f * uiScale);
    const int padY      = static_cast<int>(9.0f  * uiScale);
    const int fontH     = static_cast<int>(16.0f * uiScale);
    const int noteFontH = static_cast<int>(12.0f * uiScale);
    const int glassSide = static_cast<int>(15.0f * uiScale);   // magnifier box
    const int gap       = static_cast<int>(9.0f  * uiScale);
    // A fixed field width keeps the box from breathing on every keystroke —
    // a search field that resizes under the caret is unusable.
    const int fieldW    = static_cast<int>(340.0f * uiScale);

    const bool placeholder = query.empty();
    const bool showNote    = !placeholder && !haveMatches;

    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);
    // Grayscale antialiasing on purpose: the glyphs are composited from a
    // white-on-black coverage MASK, and ClearType's per-channel fringes would
    // bleed colour into it.
    auto makeFont = [](int h) {
        return CreateFontW(-h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    HFONT font     = makeFont(fontH);
    HFONT noteFont = makeFont(noteFontH);
    HGDIOBJ oldFont = SelectObject(memDC, font);

    int textH = 0;
    {
        RECT calc{ 0, 0, fieldW, 0 };
        DrawTextW(memDC, L"Wg", -1, &calc, DT_SINGLELINE | DT_CALCRECT);
        textH = calc.bottom;
    }

    // The note shares the field, taking its right-hand end.
    int noteW = 0;
    if (showNote) {
        SelectObject(memDC, noteFont);
        SIZE ns{};
        GetTextExtentPoint32W(memDC, kNoMatchNote,
                              static_cast<int>(wcslen(kNoMatchNote)), &ns);
        noteW = static_cast<int>(ns.cx) + gap;
        SelectObject(memDC, font);
    }

    const int contentH = (std::max)(glassSide, textH);
    const int width    = padX + glassSide + gap + fieldW + padX;
    const int height   = padY + contentH + padY;

    const size_t pxCount = static_cast<size_t>(width) * height;
    std::vector<float> canvas(pxCount * 4u, 0.0f);

    // 1. Theme plate (Settings → Search → Search box).  With the plate off the
    //    glyphs still carry a drop shadow, so the field stays readable over a
    //    bright wallpaper.
    if (showBox)
        ThemePlate::PaintPlate(canvas, width, height, st, uiScale);

    // 2. Magnifier — drawn straight into the float canvas as a stroked circle
    //    plus a handle, so it is resolution-independent and needs no asset.
    {
        const float stroke = (std::max)(1.2f, 1.5f * uiScale);
        const float half   = stroke * 0.5f;
        const float cx     = static_cast<float>(padX) + glassSide * 0.42f;
        const float cy     = height * 0.5f;
        const float radius = glassSide * 0.34f;
        // Handle: from the circle's lower-right, running out at 45°.
        const float hx0 = cx + radius * 0.72f, hy0 = cy + radius * 0.72f;
        const float hx1 = cx + glassSide * 0.48f, hy1 = cy + glassSide * 0.48f;

        const float gr = st.textR / 255.0f;
        const float gg = st.textG / 255.0f;
        const float gb = st.textB / 255.0f;
        const float tint = placeholder ? 0.55f : 0.80f;

        const int x0 = (std::max)(0, static_cast<int>(cx - glassSide));
        const int x1 = (std::min)(width,  static_cast<int>(cx + glassSide) + 1);
        const int y0 = (std::max)(0, static_cast<int>(cy - glassSide));
        const int y1 = (std::min)(height, static_cast<int>(cy + glassSide) + 1);
        for (int yy = y0; yy < y1; ++yy) {
            for (int xx = x0; xx < x1; ++xx) {
                const float px = static_cast<float>(xx) + 0.5f;
                const float py = static_cast<float>(yy) + 0.5f;

                // Ring coverage.
                const float d = std::sqrt((px - cx) * (px - cx)
                                        + (py - cy) * (py - cy));
                float cov = std::clamp(half - std::fabs(d - radius) + 0.5f,
                                       0.0f, 1.0f);

                // Handle coverage — distance to the segment.
                const float sx = hx1 - hx0, sy = hy1 - hy0;
                const float len2 = sx * sx + sy * sy;
                if (len2 > 0.0f) {
                    float t = ((px - hx0) * sx + (py - hy0) * sy) / len2;
                    t = std::clamp(t, 0.0f, 1.0f);
                    const float qx = hx0 + sx * t, qy = hy0 + sy * t;
                    const float dh = std::sqrt((px - qx) * (px - qx)
                                             + (py - qy) * (py - qy));
                    cov = (std::max)(cov,
                        std::clamp(half - dh + 0.5f, 0.0f, 1.0f));
                }

                if (cov <= 0.0f)
                    continue;
                const float a = cov * tint;
                ThemePlate::Over(canvas,
                                 static_cast<size_t>(yy) * width + xx,
                                 gb * a, gg * a, gr * a, a);
            }
        }
    }

    // 3. The query, or the placeholder when nothing is typed.  The note (when
    //    the query matched nothing) takes the right-hand end of the field, and
    //    the query's own space shrinks to match so the two never overlap.
    const int fieldL = padX + glassSide + gap;
    const int fieldR = fieldL + fieldW;
    int queryW = 0;
    PaintText(memDC, canvas, width, height,
              placeholder ? std::wstring(kPlaceholder) : query,
              fieldL, fieldR - noteW, /*rightAlign*/ false,
              placeholder ? kPlaceholderOpacity : 1.0f,
              st, uiScale, showBox, &queryW);

    if (showNote) {
        SelectObject(memDC, noteFont);
        PaintText(memDC, canvas, width, height, kNoMatchNote,
                  fieldR - noteW + gap, fieldR, /*rightAlign*/ true,
                  kNoteOpacity, st, uiScale, showBox, nullptr);
        SelectObject(memDC, font);
    }

    // 4. Caret — a static bar (a blinking one would mean rebuilding this
    //    texture twice a second for no information).  Sits immediately after
    //    the last glyph, or at the field's left edge when the text has been
    //    scrolled to keep its tail visible.
    if (!placeholder) {
        const int queryR = fieldR - noteW;
        const int caretX = (std::min)(queryR - 2,
            fieldL + (std::min)(queryW, queryR - fieldL) + 1);
        const int caretW = (std::max)(1, static_cast<int>(1.5f * uiScale));
        const float cr = st.textR / 255.0f;
        const float cg = st.textG / 255.0f;
        const float cb = st.textB / 255.0f;
        for (int yy = padY + 1; yy < height - padY - 1; ++yy) {
            for (int xx = caretX; xx < caretX + caretW && xx < width; ++xx) {
                if (xx < 0) continue;
                ThemePlate::Over(canvas,
                                 static_cast<size_t>(yy) * width + xx,
                                 cb, cg, cr, 1.0f);
            }
        }
    }

    std::vector<uint8_t> packed;
    ThemePlate::Pack(canvas, packed);

    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    const bool ok = ThemePlate::CreateTexture(device, packed, width, height,
                                              tex, srv);

    SelectObject(memDC, oldFont);
    DeleteObject(font);
    DeleteObject(noteFont);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (!ok)
        return false;

    m_texture = std::move(tex);
    m_srv     = std::move(srv);
    m_texW    = width;
    m_texH    = height;
    return true;
}
