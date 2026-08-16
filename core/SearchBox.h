#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <string>
#include <vector>
#include "ThemePlate.h"

/// The search field drawn below the cascade (Settings → Search).
///
/// A GDI-rendered premultiplied BGRA texture on the shared ThemePlate glass —
/// the same plate the selected-window label sits on, so the two read as one
/// piece of chrome in whichever CKSettings theme is active.  Rebuilt only when
/// the text, the theme or the UI scale actually change; an idle frame with the
/// placeholder showing costs one textured quad and nothing else.
///
/// This owns the picture, not the placement: the controller anchors it, which
/// keeps every screen-space decision (primary monitor, position, clamping) in
/// the one place that already knows about them.
///
/// RESIZING IS NOT SCALING.  The size setting multiplies the metrics BEFORE
/// anything is drawn — the font is created at the final point size, the plate
/// radius, the magnifier stroke and the caret are all derived from it, and the
/// texture is uploaded at exactly the pixel size it is drawn at (one texel to
/// one overlay pixel).  Nothing is ever resampled, so a large field is genuine
/// large type rather than a magnified small one.
class SearchBox {
public:
    /// Rebuild the texture if anything it depends on changed.  `haveMatches`
    /// false adds a "no matches" note beside the query — the stack really is
    /// empty then, and the field is the only thing left to say so.
    /// `scalePercent` (50-200) sizes the whole field.
    void Update(ID3D11Device* device, const std::wstring& query,
                bool haveMatches, int appTheme, bool showBox,
                int scalePercent, float cascadeH);

    /// Drop the texture (session teardown / feature switched off).
    void Reset();

    bool Ready() const { return m_srv != nullptr; }
    int  Width()  const { return m_texW; }
    int  Height() const { return m_texH; }
    ID3D11ShaderResourceView* SRV() const { return m_srv.get(); }

private:
    bool Build(ID3D11Device* device, const std::wstring& query,
               bool haveMatches, int appTheme, bool showBox, float uiScale);

    /// Render `text` into a coverage mask and composite it at `opacity`.
    /// Two passes share this: the query itself, and the "no matches" note.
    /// (Not named DrawText — Windows.h turns that into DrawTextW, which would
    /// make this member shadow the API the implementation calls.)
    static void PaintText(HDC memDC, std::vector<float>& canvas,
                          int width, int height,
                          const std::wstring& text, int left, int right,
                          bool rightAlign, float opacity,
                          const ThemePlate::Style& style, float uiScale,
                          bool haveBox, int* outTextWidth);

    winrt::com_ptr<ID3D11Texture2D>          m_texture;
    winrt::com_ptr<ID3D11ShaderResourceView> m_srv;

    // Everything the built texture depends on — compared verbatim so the
    // rebuild happens on a real change and never on a repaint.
    std::wstring m_builtQuery;
    bool         m_builtMatches = true;
    int          m_builtTheme   = -1;
    bool         m_builtBox     = true;
    float        m_builtScale   = 0.0f;
    bool         m_built        = false;

    int m_texW = 0;
    int m_texH = 0;
};
