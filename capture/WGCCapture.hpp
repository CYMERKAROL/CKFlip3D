#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <winrt/base.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <atomic>
#include <memory>

/// Captures a single HWND (or HMONITOR) via Windows.Graphics.Capture.
/// Falls back to GDI PrintWindow for minimized windows.
/// Each instance owns its own GraphicsCaptureItem, FramePool, and Session.
/// Call GetCurrentFrame() each render tick to get the latest SRV.
class WGCCapture {
public:
    WGCCapture() = default;
    ~WGCCapture();

    WGCCapture(const WGCCapture&) = delete;
    WGCCapture& operator=(const WGCCapture&) = delete;
    WGCCapture(WGCCapture&&) noexcept;
    WGCCapture& operator=(WGCCapture&&) noexcept;

    /// Start capturing a window. Uses PrintWindow fallback if minimized.
    bool StartForWindow(HWND hwnd, ID3D11Device* device);

    /// Stop capturing and release all resources.
    /// The last captured SRV is preserved so the tile can show a frozen frame.
    void Stop();

    /// Returns true if a new frame has arrived since the last GetCurrentFrame() call.
    bool HasNewFrame() const { return m_hasNewFrame->load(std::memory_order_acquire); }

    /// Returns true if capture session is actively running.
    bool IsCapturing() const { return m_started; }

    /// Returns true if we have ever captured a valid frame (cached texture exists).
    bool HasCachedFrame() const { return m_hasEverCaptured; }

    /// Returns the HWND this capture was started for (nullptr for monitor captures).
    HWND GetHwnd() const { return m_hwnd; }

    /// Returns the actual captured texture dimensions (may differ from GetWindowRect
    /// due to DPI scaling, DWM decorations, or resize). Returns {0,0} if no frame yet.
    void GetCapturedSize(int& outWidth, int& outHeight) const;

    /// Returns the current SRV. Never returns nullptr after the first frame.
    /// Reuses the previous SRV if no new frame is available.
    ID3D11ShaderResourceView* GetCurrentFrame();

    /// If no frame has been captured yet, try GDI PrintWindow as a fallback
    /// so every tile shows content instead of a placeholder.
    void EnsureFrame();

    /// v8.5 — locate the vertical centre (as a UV.y in [0,1]) of the
    /// captured texture's real content band.  On Win10 / Win11 24H2 the
    /// Shell_TrayWnd WGC capture is far taller than the visible bar, with
    /// the real taskbar in only a thin band and opaque dark fill elsewhere;
    /// this scans for rows containing bright (icon/clock/text) pixels so the
    /// draw can UV-crop to the real content instead of guessing top/bottom.
    /// Returns false if no frame is cached or no content band is found.
    bool DetectContentCenterV(float& outCenterUvY);

#ifdef CKFLIP_DEBUG_TASKBAR
    /// Debug-only (Bug 11' v8.2 §12 Test B + v8.4 §6).  Copies
    /// m_cachedTexture to a staging texture and writes, using `basePath`
    /// as a prefix (no extension):
    ///   <basePath>.bmp        — full 32-bit capture
    ///   <basePath>_alpha.bmp  — alpha heatmap
    ///   <basePath>.txt        — advanced analysis report (geometry,
    ///                           averages, alpha edge cases, premultiplied
    ///                           check, #282832 classification, per-channel
    ///                           histograms, [uv crop] region stats, verdict)
    /// ONE-SHOT — call once per session from the render/controller thread
    /// only, never from the WGC FrameArrived callback.  Compiled out
    /// entirely in release builds.
    ///
    /// Variant 1 — full-texture dump (legacy; crop region = whole texture).
    bool DebugDumpCachedTexture(const wchar_t* basePath);

    /// Variant 2 — full-texture dump + a [uv crop] section analysing only
    /// the region CKFlip actually samples.  uvMin*/uvMax* match
    /// QuadDrawCall.uvMin*/uvMax*; they are clamped to [0,1] internally.
    bool DebugDumpCachedTexture(const wchar_t* basePath,
                                float uvMinX, float uvMinY,
                                float uvMaxX, float uvMaxY);
#endif

private:
    bool StartInternal(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item,
                       ID3D11Device* device);

    /// GDI PrintWindow fallback for minimized windows.
    /// Creates a D3D11 texture from the window's GDI bitmap.
    bool CaptureWithPrintWindow(HWND hwnd, ID3D11Device* device);

    /// DWM Thumbnail pipeline: registers a DWM thumbnail on a helper window,
    /// captures the helper via a one-shot WGC session to get the minimized
    /// window's last-known DWM surface.  Zero state mutation on the target.
    bool CaptureWithDwmThumbnail(HWND target, ID3D11Device* device);

    HWND m_hwnd = nullptr;  // Stored for PrintWindow fallback

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem       m_item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession    m_session{ nullptr };
    winrt::event_token m_frameArrivedToken{};

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
    winrt::com_ptr<ID3D11Device>              m_d3dDevice;
    winrt::com_ptr<ID3D11DeviceContext>        m_d3dContext;
    winrt::com_ptr<ID3D11Texture2D>           m_cachedTexture;  // Owned texture — survives WGC frame recycling
    winrt::com_ptr<ID3D11ShaderResourceView>  m_cachedSRV;      // SRV to our owned texture

    winrt::Windows::Graphics::SizeInt32 m_lastSize{};
    // Shared with the FrameArrived lambda (captured by value) so a handler
    // still in flight on a WGC pool thread can never write into a destroyed
    // WGCCapture — the flag object outlives the capture.
    std::shared_ptr<std::atomic<bool>> m_hasNewFrame
        = std::make_shared<std::atomic<bool>>(false);
    bool              m_started = false;
    bool              m_hasEverCaptured = false;  // True once we have a valid cached frame
};
