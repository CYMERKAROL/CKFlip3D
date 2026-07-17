#include "WGCCapture.hpp"
#include "../core/DebugLog.h"

#include <d3d11_4.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <inspectable.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <vector>
#include <cstdint>

#ifdef CKFLIP_DEBUG_TASKBAR
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <cmath>
#endif

#pragma comment(lib, "windowsapp.lib")

// Forward-declare the interop function at global scope.
extern "C" HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(
    IDXGIDevice* dxgiDevice, IInspectable** graphicsDevice);

// ---------------------------------------------------------------------------
// Helper: get the IDirect3DDevice WinRT wrapper from a raw ID3D11Device.
// ---------------------------------------------------------------------------
namespace {

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
CreateWinRTDevice(ID3D11Device* d3dDevice)
{
    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{ nullptr };
    winrt::com_ptr<::IInspectable> inspectable;

    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.get(), inspectable.put());
    if (SUCCEEDED(hr))
        winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    return winrtDevice;
}

template <typename T>
winrt::com_ptr<T> GetDXGIInterfaceFromObject(
    winrt::Windows::Foundation::IInspectable const& object)
{
    auto access = object.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<T> result;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(result.put())));
    return result;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DWM Thumbnail helper window — used to capture minimized windows' cached
// DWM surfaces.  Created once, reused across all captures.
// ---------------------------------------------------------------------------
namespace {

HWND g_dwmHelper = nullptr;

HWND GetOrCreateDwmHelper()
{
    if (g_dwmHelper && IsWindow(g_dwmHelper))
        return g_dwmHelper;

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc   = DefWindowProcW;
        wc.hInstance      = GetModuleHandleW(nullptr);
        wc.lpszClassName  = L"CKFlipDwmThumbnailHelper";
        wc.hbrBackground  = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        RegisterClassW(&wc);
        s_registered = true;
    }

    // WS_POPUP: no chrome.  WS_EX_TOOLWINDOW: no taskbar entry.
    // WS_EX_NOACTIVATE: won't steal focus.
    g_dwmHelper = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"CKFlipDwmThumbnailHelper", L"",
        WS_POPUP,
        -1, -1, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    return g_dwmHelper;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
WGCCapture::~WGCCapture()
{
    Stop();
}

WGCCapture::WGCCapture(WGCCapture&& other) noexcept
{
    *this = std::move(other);
}

WGCCapture& WGCCapture::operator=(WGCCapture&& other) noexcept
{
    if (this != &other) {
        Stop();
        m_hwnd          = other.m_hwnd;
        other.m_hwnd    = nullptr;
        m_item          = std::move(other.m_item);
        m_framePool     = std::move(other.m_framePool);
        m_session       = std::move(other.m_session);
        m_frameArrivedToken = other.m_frameArrivedToken;
        other.m_frameArrivedToken = {};
        m_winrtDevice   = std::move(other.m_winrtDevice);
        m_d3dDevice     = std::move(other.m_d3dDevice);
        m_d3dContext    = std::move(other.m_d3dContext);
        m_cachedTexture = std::move(other.m_cachedTexture);
        m_cachedSRV     = std::move(other.m_cachedSRV);
        m_hasEverCaptured = other.m_hasEverCaptured;
        other.m_hasEverCaptured = false;
        m_lastSize      = other.m_lastSize;
        // Move the shared flag object itself: the moved-in frame pool's
        // registered handler captured *other's* flag, so the link between
        // pool and flag must be preserved across the move.
        m_hasNewFrame = std::move(other.m_hasNewFrame);
        other.m_hasNewFrame = std::make_shared<std::atomic<bool>>(false);
        m_started       = other.m_started;
        other.m_started = false;
    }
    return *this;
}

// ---------------------------------------------------------------------------
bool WGCCapture::StartForWindow(HWND hwnd, ID3D11Device* device)
{
    m_hwnd = hwnd;  // Store for potential PrintWindow fallback later
    m_d3dDevice.copy_from(device);
    m_d3dDevice->GetImmediateContext(m_d3dContext.put());

    // Try WGC for ALL windows, including minimized ones.
    // On Win11, DWM keeps the last composition buffer for minimized windows,
    // so WGC can often capture a valid frame.  If WGC fails, the existing
    // wait loop + EnsureFrame()/PrintWindow fallback handles it.
    try {
        auto interop = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(interop->CreateForWindow(
            hwnd,
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item)));

        return StartInternal(item, device);
    }
    catch (...) {
        // WGC failed — try GDI PrintWindow fallback.
        CKLog::Log(L"CKFlip: WGC failed, trying PrintWindow fallback\n");
        return CaptureWithPrintWindow(hwnd, device);
    }
}

// ---------------------------------------------------------------------------
bool WGCCapture::StartInternal(
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item,
    ID3D11Device* device)
{
    try {
        m_item = item;
        m_d3dDevice.copy_from(device);
        m_d3dDevice->GetImmediateContext(m_d3dContext.put());
        m_winrtDevice = CreateWinRTDevice(device);
        if (!m_winrtDevice)
            return false;

        m_lastSize = m_item.Size();

        m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            m_winrtDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            4,
            m_lastSize);

        // Capture the flag shared_ptr BY VALUE — the handler must never touch
        // `this`: revoking the token in Stop() does not wait for an in-flight
        // callback on the free-threaded pool thread, so `this` may already be
        // destroyed while the callback runs.  The shared_ptr keeps the flag
        // alive for the duration of any late invocation.
        m_frameArrivedToken = m_framePool.FrameArrived(
            [flag = m_hasNewFrame](auto&&, auto&&) {
                flag->store(true, std::memory_order_release);
            });

        m_session = m_framePool.CreateCaptureSession(m_item);

        // Disable cursor capture.
        m_session.IsCursorCaptureEnabled(false);

        // Disable yellow border on Win11+ (runtime check).
        try {
            auto session3 = m_session.as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
            if (session3)
                session3.IsBorderRequired(false);
        }
        catch (...) {
            // Not available on this OS version — that's fine.
        }

        m_session.StartCapture();
        m_started = true;
        return true;
    }
    catch (...) {
        return false;
    }
}

void WGCCapture::Stop()
{
    if (m_framePool)
        m_framePool.FrameArrived(m_frameArrivedToken);
    m_frameArrivedToken = {};

    if (m_session) {
        m_session.Close();
        m_session = nullptr;
    }
    if (m_framePool) {
        m_framePool.Close();
        m_framePool = nullptr;
    }
    m_item       = nullptr;
    m_winrtDevice = nullptr;
    // Owned cache intentionally preserved — frozen frame for minimized windows.
    // m_cachedTexture, m_cachedSRV, m_hasEverCaptured stay valid.
    m_d3dContext  = nullptr;
    m_d3dDevice   = nullptr;
    m_hasNewFrame->store(false, std::memory_order_relaxed);
    m_started     = false;
}

// ---------------------------------------------------------------------------
void WGCCapture::GetCapturedSize(int& outWidth, int& outHeight) const
{
    if (m_cachedTexture) {
        D3D11_TEXTURE2D_DESC desc{};
        m_cachedTexture->GetDesc(&desc);
        outWidth  = static_cast<int>(desc.Width);
        outHeight = static_cast<int>(desc.Height);
    } else {
        outWidth  = 0;
        outHeight = 0;
    }
}

// ---------------------------------------------------------------------------
ID3D11ShaderResourceView* WGCCapture::GetCurrentFrame()
{
    // No new frame and capture not running — return cached owned SRV.
    if (!m_hasNewFrame->load(std::memory_order_acquire) || !m_framePool)
        return m_cachedSRV.get();

    try {
        // Drain all pending frames, keeping only the latest.
        // This ensures we always show the freshest content and frees
        // pool buffers for WGC to deliver new frames without backpressure.
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latestFrame{ nullptr };
        while (auto frame = m_framePool.TryGetNextFrame()) {
            latestFrame = frame;
        }

        // Clear flag only after draining all frames.
        m_hasNewFrame->store(false, std::memory_order_release);

        if (!latestFrame)
            return m_cachedSRV.get();

        auto frameSize = latestFrame.ContentSize();

        // Minimized windows may deliver zero-size frames — ignore them
        // so the previously cached SRV is preserved.
        if (frameSize.Width <= 0 || frameSize.Height <= 0)
            return m_cachedSRV.get();

        // Recreate pool on size change.
        if (frameSize.Width != m_lastSize.Width ||
            frameSize.Height != m_lastSize.Height) {
            m_lastSize = frameSize;
            m_framePool.Recreate(
                m_winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                4,
                m_lastSize);
            // Invalidate owned texture — size changed, must recreate.
            m_cachedTexture = nullptr;
            m_cachedSRV     = nullptr;
            return m_cachedSRV.get();
        }

        auto wgcTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(
            latestFrame.Surface());

        // Ensure owned texture exists and matches size.
        D3D11_TEXTURE2D_DESC desc{};
        wgcTexture->GetDesc(&desc);

        bool needNewTexture = !m_cachedTexture;
        if (m_cachedTexture) {
            D3D11_TEXTURE2D_DESC existingDesc{};
            m_cachedTexture->GetDesc(&existingDesc);
            if (existingDesc.Width != desc.Width || existingDesc.Height != desc.Height)
                needNewTexture = true;
        }

        if (needNewTexture) {
            D3D11_TEXTURE2D_DESC ownedDesc = desc;
            ownedDesc.Usage          = D3D11_USAGE_DEFAULT;
            ownedDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            ownedDesc.CPUAccessFlags = 0;
            ownedDesc.MiscFlags      = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ownedDesc.MipLevels      = 0;   // Auto-generate full mipchain

            m_cachedTexture = nullptr;
            m_cachedSRV     = nullptr;
            if (FAILED(m_d3dDevice->CreateTexture2D(&ownedDesc, nullptr, m_cachedTexture.put())))
                return m_cachedSRV.get();

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);   // All mip levels
            if (FAILED(m_d3dDevice->CreateShaderResourceView(
                    m_cachedTexture.get(), &srvDesc, m_cachedSRV.put()))) {
                m_cachedTexture = nullptr;
                return m_cachedSRV.get();
            }
        }

        // Copy WGC texture (single mip) to mip 0 of our mipchain texture,
        // then generate the remaining mip levels for crisp anisotropic sampling.
        m_d3dContext->CopySubresourceRegion(
            m_cachedTexture.get(), 0, 0, 0, 0,
            wgcTexture.get(), 0, nullptr);
        m_d3dContext->GenerateMips(m_cachedSRV.get());
        m_hasEverCaptured = true;
    }
    catch (...) {
        // Frame acquisition failed; return stale owned SRV.
    }

    return m_cachedSRV.get();
}

// ---------------------------------------------------------------------------
void WGCCapture::EnsureFrame()
{
    if (m_hasEverCaptured)
        return;   // already have a valid frame
    if (!m_hwnd || !IsWindow(m_hwnd))
        return;
    if (!m_d3dDevice)
        return;

    // Try DWM Thumbnail first — captures minimized windows' cached DWM surface.
    if (CaptureWithDwmThumbnail(m_hwnd, m_d3dDevice.get())) {
        CKLog::Log(L"CKFlip: EnsureFrame — DWM thumbnail succeeded\n");
        return;
    }

    // Fall back to PrintWindow (gives at least window chrome).
    CKLog::Log(L"CKFlip: EnsureFrame — PrintWindow fallback\n");
    CaptureWithPrintWindow(m_hwnd, m_d3dDevice.get());
}

// ---------------------------------------------------------------------------
// DWM Thumbnail capture pipeline.
//
// DWM keeps the last composed frame for every window, even minimized ones.
// DwmRegisterThumbnail renders that cached surface onto a helper window.
// We capture the helper via a one-shot WGC session to get a D3D11 texture.
// This is the same mechanism that Alt+Tab uses for minimized previews.
// ---------------------------------------------------------------------------
bool WGCCapture::CaptureWithDwmThumbnail(HWND target, ID3D11Device* device)
{
    if (!target || !IsWindow(target))
        return false;

    HWND helper = GetOrCreateDwmHelper();
    if (!helper)
        return false;

    // --- Determine target dimensions ---
    int width = 0, height = 0;

    if (IsIconic(target)) {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(target, &wp)) {
            // If was maximized, use monitor work area.
            if (wp.showCmd == SW_SHOWMAXIMIZED ||
                (wp.flags & WPF_RESTORETOMAXIMIZED)) {
                HMONITOR hMon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMon, &mi)) {
                    width  = mi.rcWork.right  - mi.rcWork.left;
                    height = mi.rcWork.bottom - mi.rcWork.top;
                }
            }
            if (width <= 0 || height <= 0) {
                width  = wp.rcNormalPosition.right  - wp.rcNormalPosition.left;
                height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
            }
        }
    } else {
        // Use DWMWA_EXTENDED_FRAME_BOUNDS — same as the window scanner —
        // to exclude invisible Win10/11 borders (~7px each side).
        // Mismatched dimensions cause texture/quad aspect ratio mismatch.
        RECT rc = {};
        HRESULT hrRect = DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS,
                                                &rc, sizeof(rc));
        if (FAILED(hrRect) || (rc.right - rc.left) <= 0 || (rc.bottom - rc.top) <= 0)
            GetWindowRect(target, &rc);
        width  = rc.right  - rc.left;
        height = rc.bottom - rc.top;
    }

    if (width < 100)  width  = 800;
    if (height < 100)  height = 600;

    // --- Size and show helper window (off-screen, behind everything) ---
    SetWindowPos(helper, HWND_BOTTOM, -width - 100, 0, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // --- Register DWM thumbnail: source = target, destination = helper ---
    HTHUMBNAIL thumb = nullptr;
    HRESULT hr = DwmRegisterThumbnail(helper, target, &thumb);
    if (FAILED(hr)) {
        wchar_t buf[128];
        swprintf_s(buf, L"CKFlip DwmThumb: Register failed hr=0x%08X\n", (unsigned)hr);
        CKLog::Log(buf);
        ShowWindow(helper, SW_HIDE);
        return false;
    }

    // Configure thumbnail to fill the entire helper client area.
    DWM_THUMBNAIL_PROPERTIES props = {};
    props.dwFlags       = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION;
    props.fVisible       = TRUE;
    props.rcDestination  = { 0, 0, width, height };
    DwmUpdateThumbnailProperties(thumb, &props);

    // Let DWM compose the thumbnail onto the helper.
    DwmFlush();
    DwmFlush();

    // --- Capture helper via one-shot WGC session ---
    bool captured = false;
    try {
        auto interop = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>();

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(interop->CreateForWindow(
            helper,
            winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
            winrt::put_abi(item)));

        auto winrtDev = CreateWinRTDevice(device);
        if (!winrtDev) {
            DwmUnregisterThumbnail(thumb);
            ShowWindow(helper, SW_HIDE);
            return false;
        }

        auto size = item.Size();
        auto pool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDev,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            1, size);

        auto session = pool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);

        // Disable yellow border on Win11+.
        try {
            auto s3 = session.as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
            if (s3) s3.IsBorderRequired(false);
        } catch (...) {}

        session.StartCapture();

        // Wait for a frame (up to 10 DwmFlush cycles ≈ 60–170 ms).
        for (int attempt = 0; attempt < 10; ++attempt) {
            DwmFlush();
            auto frame = pool.TryGetNextFrame();
            if (!frame) continue;

            auto frameSize = frame.ContentSize();
            if (frameSize.Width <= 0 || frameSize.Height <= 0)
                continue;

            auto wgcTexture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(
                frame.Surface());

            // Create our owned mipchain texture.
            D3D11_TEXTURE2D_DESC desc{};
            wgcTexture->GetDesc(&desc);

            D3D11_TEXTURE2D_DESC ownedDesc = desc;
            ownedDesc.Usage          = D3D11_USAGE_DEFAULT;
            ownedDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            ownedDesc.CPUAccessFlags = 0;
            ownedDesc.MiscFlags      = D3D11_RESOURCE_MISC_GENERATE_MIPS;
            ownedDesc.MipLevels      = 0;

            m_cachedTexture = nullptr;
            m_cachedSRV     = nullptr;

            if (FAILED(device->CreateTexture2D(&ownedDesc, nullptr, m_cachedTexture.put())))
                break;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
            srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);
            if (FAILED(device->CreateShaderResourceView(
                    m_cachedTexture.get(), &srvDesc, m_cachedSRV.put()))) {
                m_cachedTexture = nullptr;
                break;
            }

            // Copy the thumbnail frame to mip 0, then generate mips.
            winrt::com_ptr<ID3D11DeviceContext> ctx;
            device->GetImmediateContext(ctx.put());
            ctx->CopySubresourceRegion(
                m_cachedTexture.get(), 0, 0, 0, 0,
                wgcTexture.get(), 0, nullptr);
            ctx->GenerateMips(m_cachedSRV.get());

            m_hasEverCaptured = true;
            captured = true;
            break;
        }

        session.Close();
        pool.Close();
    }
    catch (...) {
        CKLog::Log(L"CKFlip DwmThumb: WGC mini-session failed\n");
    }

    DwmUnregisterThumbnail(thumb);
    ShowWindow(helper, SW_HIDE);

    if (captured) {
        wchar_t buf[128];
        swprintf_s(buf, L"CKFlip DwmThumb: Captured %dx%d for %p\n",
                   width, height, (void*)target);
        CKLog::Log(buf);
    }

    return captured;
}

// ---------------------------------------------------------------------------
// PrintWindow fallback for minimized windows.
// Creates a D3D11 texture from the window's GDI bitmap.
// For minimized windows, uses GetWindowPlacement to get restored size.
// ---------------------------------------------------------------------------
bool WGCCapture::CaptureWithPrintWindow(HWND hwnd, ID3D11Device* device)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    int width = 0, height = 0;

    // For minimized windows, GetWindowRect returns taskbar button rect.
    // Use GetWindowPlacement to get the restored window size instead.
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(hwnd, &wp)) {
            width  = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
            height = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
        CKLog::Log(L"CKFlip: Using GetWindowPlacement for minimized window\n");
    } else {
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            width  = rc.right - rc.left;
            height = rc.bottom - rc.top;
        }
    }

    // Validate dimensions
    if (width < 100) width = 800;
    if (height < 100) height = 600;

    wchar_t buf[128];
    swprintf_s(buf, L"CKFlip PrintWindow: size=%dx%d\n", width, height);
    CKLog::Log(buf);

    // Create a compatible DC and bitmap.
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(screenDC, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);

    // Fill with a dark background in case PrintWindow fails partially.
    RECT fillRect = { 0, 0, width, height };
    HBRUSH darkBrush = CreateSolidBrush(RGB(40, 40, 50));
    FillRect(memDC, &fillRect, darkBrush);
    DeleteObject(darkBrush);

    // PrintWindow with PW_RENDERFULLCONTENT (0x2) for best results.
    // Note: PrintWindow often fails for minimized windows, returning empty content.
    BOOL printed = PrintWindow(hwnd, memDC, 0x2);  // PW_RENDERFULLCONTENT
    if (!printed) {
        printed = PrintWindow(hwnd, memDC, 0x1);  // PW_CLIENTONLY
    }
    if (!printed) {
        printed = PrintWindow(hwnd, memDC, 0x0);  // No flags
    }

    // Log result
    if (printed) {
        CKLog::Log(L"CKFlip: PrintWindow succeeded\n");
    } else {
        CKLog::Log(L"CKFlip: PrintWindow failed, using dark placeholder\n");
    }

    // Extract bitmap bits.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;  // Top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(width * height * 4);
    if (!GetDIBits(memDC, hBitmap, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS)) {
        SelectObject(memDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return false;
    }

    // Convert BGR to BGRA (set alpha to 255).
    for (int i = 0; i < width * height; ++i) {
        pixels[i * 4 + 3] = 255;  // Alpha
    }

    // Create D3D11 texture with mipchain for crisp anisotropic sampling.
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width              = width;
    texDesc.Height             = height;
    texDesc.MipLevels          = 0;    // Auto-generate full mipchain
    texDesc.ArraySize          = 1;
    texDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count   = 1;
    texDesc.Usage              = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags          = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    texDesc.MiscFlags          = D3D11_RESOURCE_MISC_GENERATE_MIPS;

    // MipLevels=0 doesn't support initial data, so create empty then upload mip 0.
    m_cachedTexture = nullptr;
    m_cachedSRV = nullptr;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, m_cachedTexture.put());
    if (FAILED(hr)) {
        SelectObject(memDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return false;
    }

    // Upload pixel data to mip level 0.
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(ctx.put());
    ctx->UpdateSubresource(m_cachedTexture.get(), 0, nullptr,
                           pixels.data(), width * 4, 0);

    // Create SRV spanning all mip levels.
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = static_cast<UINT>(-1);

    hr = device->CreateShaderResourceView(m_cachedTexture.get(), &srvDesc, m_cachedSRV.put());

    // Generate remaining mip levels.
    if (SUCCEEDED(hr))
        ctx->GenerateMips(m_cachedSRV.get());

    // Cleanup GDI objects.
    SelectObject(memDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (FAILED(hr)) {
        m_cachedTexture = nullptr;
        return false;
    }

    m_hasEverCaptured = true;
    CKLog::Log(L"CKFlip: PrintWindow capture successful\n");
    return true;
}

// ---------------------------------------------------------------------------
// v8.5 — find the vertical centre of the captured texture's content band.
bool WGCCapture::DetectContentCenterV(float& outCenterUvY)
{
    if (!m_cachedTexture || !m_d3dDevice || !m_d3dContext)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    m_cachedTexture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0)
        return false;

    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;

    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(m_d3dDevice->CreateTexture2D(&sd, nullptr, staging.put())))
        return false;
    m_d3dContext->CopyResource(staging.get(), m_cachedTexture.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    const uint32_t W = desc.Width;
    const uint32_t H = desc.Height;
    const uint8_t* base = static_cast<const uint8_t*>(mapped.pData);

    // A row is "content" if it carries enough pixels brighter than the dark
    // taskbar fill (#282832-ish — every channel low) and not transparent.
    const uint32_t pixelThreshold = W / 48u + 4u;
    int firstRow = -1, lastRow = -1;
    for (uint32_t y = 0; y < H; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * mapped.RowPitch;
        uint32_t bright = 0;
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4u;  // B,G,R,A
            if (px[3] < 16) continue;                  // transparent
            int mx = px[0];
            if (px[1] > mx) mx = px[1];
            if (px[2] > mx) mx = px[2];
            if (mx > 75) ++bright;                     // brighter than fill
        }
        if (bright >= pixelThreshold) {
            if (firstRow < 0) firstRow = static_cast<int>(y);
            lastRow = static_cast<int>(y);
        }
    }
    m_d3dContext->Unmap(staging.get(), 0);

    if (firstRow < 0 || lastRow < firstRow)
        return false;

    outCenterUvY = (static_cast<float>(firstRow) + static_cast<float>(lastRow) + 1.0f)
                 * 0.5f / static_cast<float>(H);
    return true;
}

#ifdef CKFLIP_DEBUG_TASKBAR
// ---------------------------------------------------------------------------
// Bug 11' v8.2 §12 Test B — debug-only taskbar texture dump.  Compiled only
// under CKFLIP_DEBUG_TASKBAR; release builds have zero footprint.
// ---------------------------------------------------------------------------
namespace {

// Write a 32-bit BGRA BMP (bottom-up) from a tight top-down BGRA buffer.
bool WriteBmp32(const wchar_t* file, uint32_t W, uint32_t H,
                const uint8_t* pixels)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, file, L"wb") != 0 || !f)
        return false;

    const uint32_t rowBytes   = W * 4u;
    const uint32_t imageBytes = rowBytes * H;
    const uint32_t fileBytes  = 14u + 40u + imageBytes;

    uint8_t fh[14] = {};
    fh[0] = 'B'; fh[1] = 'M';
    fh[2]  = static_cast<uint8_t>(fileBytes);
    fh[3]  = static_cast<uint8_t>(fileBytes >> 8);
    fh[4]  = static_cast<uint8_t>(fileBytes >> 16);
    fh[5]  = static_cast<uint8_t>(fileBytes >> 24);
    fh[10] = 54;   // pixel-data offset

    uint8_t ih[40] = {};
    ih[0]  = 40;
    ih[4]  = static_cast<uint8_t>(W);
    ih[5]  = static_cast<uint8_t>(W >> 8);
    ih[6]  = static_cast<uint8_t>(W >> 16);
    ih[7]  = static_cast<uint8_t>(W >> 24);
    ih[8]  = static_cast<uint8_t>(H);
    ih[9]  = static_cast<uint8_t>(H >> 8);
    ih[10] = static_cast<uint8_t>(H >> 16);
    ih[11] = static_cast<uint8_t>(H >> 24);
    ih[12] = 1;    // planes
    ih[14] = 32;   // bpp

    fwrite(fh, 1, 14, f);
    fwrite(ih, 1, 40, f);
    for (int32_t y = static_cast<int32_t>(H) - 1; y >= 0; --y)   // bottom-up
        fwrite(pixels + static_cast<size_t>(y) * rowBytes, 1, rowBytes, f);

    fclose(f);
    return true;
}

} // namespace

bool WGCCapture::DebugDumpCachedTexture(const wchar_t* basePath)
{
    // Legacy full-texture variant — crop region == whole texture.
    return DebugDumpCachedTexture(basePath, 0.0f, 0.0f, 1.0f, 1.0f);
}

bool WGCCapture::DebugDumpCachedTexture(const wchar_t* basePath,
                                        float uvMinX, float uvMinY,
                                        float uvMaxX, float uvMaxY)
{
    if (!m_cachedTexture || !m_d3dDevice || !m_d3dContext || !basePath)
        return false;

    D3D11_TEXTURE2D_DESC desc{};
    m_cachedTexture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0)
        return false;

    // Staging copy so the CPU can read the texture.
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags      = 0;

    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(m_d3dDevice->CreateTexture2D(&sd, nullptr, staging.put())))
        return false;

    m_d3dContext->CopyResource(staging.get(), m_cachedTexture.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_d3dContext->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    const uint32_t W = desc.Width;
    const uint32_t H = desc.Height;
    const uint8_t* mapBase = static_cast<const uint8_t*>(mapped.pData);

    // Repack into a tight top-down BGRA buffer (staging RowPitch may
    // exceed W*4).
    std::vector<uint8_t> tight(static_cast<size_t>(W) * H * 4u);
    for (uint32_t y = 0; y < H; ++y)
        memcpy(tight.data() + static_cast<size_t>(y) * W * 4u,
               mapBase + static_cast<size_t>(y) * mapped.RowPitch,
               static_cast<size_t>(W) * 4u);

    m_d3dContext->Unmap(staging.get(), 0);

    // Full-texture analysis (Test B does NOT restrict to bottom rows).
    const uint64_t total = static_cast<uint64_t>(W) * H;
    if (total == 0)
        return false;

    uint64_t alphaBuckets[8] = {};
    uint64_t rBuckets[8] = {}, gBuckets[8] = {}, bBuckets[8] = {};
    uint64_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
    uint64_t hit282832 = 0;
    uint64_t aZero = 0, aFull = 0;
    uint64_t premulViolations = 0;   // max(r,g,b) > a  → not premultiplied
    int minA = 255, maxA = 0;
    std::vector<uint8_t> heat(static_cast<size_t>(W) * H * 4u);

    for (uint64_t i = 0; i < total; ++i) {
        const uint8_t* px = tight.data() + i * 4u;   // B,G,R,A
        uint8_t b = px[0], g = px[1], r = px[2], a = px[3];
        alphaBuckets[a >> 5]++;
        rBuckets[r >> 5]++; gBuckets[g >> 5]++; bBuckets[b >> 5]++;
        sumB += b; sumG += g; sumR += r; sumA += a;
        if (a == 0)   aZero++;
        if (a == 255) aFull++;
        if (a < minA) minA = a;
        if (a > maxA) maxA = a;
        int mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
        if (mx > int(a) + 2) premulViolations++;
        // #282832 = R 0x28, G 0x28, B 0x32 (±10 tolerance).
        if (std::abs(int(r) - 0x28) <= 10 &&
            std::abs(int(g) - 0x28) <= 10 &&
            std::abs(int(b) - 0x32) <= 10)
            hit282832++;
        uint8_t* hp = heat.data() + i * 4u;          // alpha heatmap (grey)
        hp[0] = hp[1] = hp[2] = a; hp[3] = 255;
    }

    // v8.4 §6 — UV crop-region analysis (the exact region CKFlip samples).
    // UVs clamped to [0,1] and ordered defensively; empty crop handled.
    auto clamp01 = [](float v) -> float {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    };
    float u0 = clamp01(uvMinX < uvMaxX ? uvMinX : uvMaxX);
    float u1 = clamp01(uvMinX < uvMaxX ? uvMaxX : uvMinX);
    float v0 = clamp01(uvMinY < uvMaxY ? uvMinY : uvMaxY);
    float v1 = clamp01(uvMinY < uvMaxY ? uvMaxY : uvMinY);

    uint32_t cropX0 = static_cast<uint32_t>(std::floor(u0 * static_cast<float>(W)));
    uint32_t cropY0 = static_cast<uint32_t>(std::floor(v0 * static_cast<float>(H)));
    uint32_t cropX1 = static_cast<uint32_t>(std::ceil (u1 * static_cast<float>(W)));
    uint32_t cropY1 = static_cast<uint32_t>(std::ceil (v1 * static_cast<float>(H)));
    if (cropX0 > W) cropX0 = W;
    if (cropY0 > H) cropY0 = H;
    if (cropX1 > W) cropX1 = W;
    if (cropY1 > H) cropY1 = H;

    uint64_t cropCount = 0, cropPremulViol = 0, cropHit282832 = 0;
    uint64_t cropSumR = 0, cropSumG = 0, cropSumB = 0, cropSumA = 0;
    int cropMinA = 255, cropMaxA = 0;
    if (cropX1 > cropX0 && cropY1 > cropY0) {
        for (uint32_t y = cropY0; y < cropY1; ++y) {
            for (uint32_t x = cropX0; x < cropX1; ++x) {
                const uint8_t* px = tight.data()
                    + (static_cast<size_t>(y) * W + x) * 4u;
                uint8_t b = px[0], g = px[1], r = px[2], a = px[3];
                cropCount++;
                cropSumB += b; cropSumG += g; cropSumR += r; cropSumA += a;
                if (a < cropMinA) cropMinA = a;
                if (a > cropMaxA) cropMaxA = a;
                int mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
                if (mx > int(a) + 2) cropPremulViol++;
                if (std::abs(int(r) - 0x28) <= 10 &&
                    std::abs(int(g) - 0x28) <= 10 &&
                    std::abs(int(b) - 0x32) <= 10) cropHit282832++;
            }
        }
    }

    std::wstring base(basePath);
    WriteBmp32((base + L".bmp").c_str(),       W, H, tight.data());
    WriteBmp32((base + L"_alpha.bmp").c_str(), W, H, heat.data());

    const double dtotal = double(total);
    const double pctViol  = 100.0 * double(premulViolations) / dtotal;
    const double pct282   = 100.0 * double(hit282832)        / dtotal;
    const double pctZero  = 100.0 * double(aZero)            / dtotal;
    const double pctFull  = 100.0 * double(aFull)            / dtotal;

    const wchar_t* verdict;
    if (pctFull > 99.0)
        verdict = L"LooksOpaqueOrPlaceholder — keep normal Draw()";
    else if (pctViol > 1.0 || (pctZero > 1.0 && pct282 > 0.5))
        verdict = L"LooksStraightOrDirtyAlpha — DrawAssumeStraightAlpha candidate";
    else
        verdict = L"LooksPremultiplied — keep normal Draw()";

    FILE* rep = nullptr;
    if (_wfopen_s(&rep, (base + L".txt").c_str(), L"w") == 0 && rep) {
        fwprintf(rep, L"CKFlip3D taskbar capture dump  (Bug 11' v8.2 Test B)\n");
        fwprintf(rep, L"================================================\n\n");
        fwprintf(rep, L"[geometry]\n");
        fwprintf(rep, L"  dimensions    : %u x %u  (%llu px)\n",
                 W, H, (unsigned long long)total);
        fwprintf(rep, L"  DXGI format   : %d\n", static_cast<int>(desc.Format));
        fwprintf(rep, L"  staging pitch : %u bytes/row\n\n", mapped.RowPitch);

        // [environment] — OS / window context (v8.5.1 §6.2).  RtlGetVersion
        // is used because the GetVersionEx helpers are manifest-gated.
        wchar_t osStr[80] = L"unknown";
        {
            OSVERSIONINFOW osv = {};
            osv.dwOSVersionInfoSize = sizeof(osv);
            HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
            if (ntdll) {
                typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW*);
                auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
                    reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
                if (rtlGetVersion && rtlGetVersion(&osv) == 0)
                    swprintf_s(osStr, L"%lu.%lu build %lu",
                               osv.dwMajorVersion, osv.dwMinorVersion,
                               osv.dwBuildNumber);
            }
        }
        wchar_t clsStr[64] = L"(none)";
        RECT winRc = {};
        int winVisible = -1;
        if (m_hwnd) {
            GetClassNameW(m_hwnd, clsStr, 64);
            GetWindowRect(m_hwnd, &winRc);
            winVisible = IsWindowVisible(m_hwnd) ? 1 : 0;
        }
        wchar_t modeStr[64] = L"(unset)";
        GetEnvironmentVariableW(L"CKFLIP_TASKBAR_MODE", modeStr, 64);

        fwprintf(rep, L"[environment]\n");
        fwprintf(rep, L"  OS version          : %ls\n", osStr);
        fwprintf(rep, L"  taskbar HWND        : class=%ls  visible=%d\n",
                 clsStr, winVisible);
        fwprintf(rep, L"  taskbar window rect : (%ld,%ld)-(%ld,%ld)\n",
                 winRc.left, winRc.top, winRc.right, winRc.bottom);
        fwprintf(rep, L"  CKFLIP_TASKBAR_MODE : %ls\n\n", modeStr);

        fwprintf(rep, L"[averages]\n");
        fwprintf(rep, L"  avg R/G/B/A   : %llu / %llu / %llu / %llu\n",
                 (unsigned long long)(sumR / total),
                 (unsigned long long)(sumG / total),
                 (unsigned long long)(sumB / total),
                 (unsigned long long)(sumA / total));
        fwprintf(rep, L"  alpha min/max : %d / %d\n\n", minA, maxA);

        fwprintf(rep, L"[alpha edge cases]\n");
        fwprintf(rep, L"  fully transparent (a=0)   : %llu (%.2f%%)\n",
                 (unsigned long long)aZero, pctZero);
        fwprintf(rep, L"  fully opaque      (a=255) : %llu (%.2f%%)\n\n",
                 (unsigned long long)aFull, pctFull);

        fwprintf(rep, L"[premultiplied-alpha check]\n");
        fwprintf(rep, L"  pixels with max(R,G,B) > A : %llu (%.2f%%)\n",
                 (unsigned long long)premulViolations, pctViol);
        fwprintf(rep, L"  (high %% => texture is straight/dirty alpha,\n");
        fwprintf(rep, L"   ~0%% => texture is already premultiplied)\n\n");

        fwprintf(rep, L"[#282832 classification]\n");
        fwprintf(rep, L"  #282832-like pixels (R28 G28 B32 +-10): %llu (%.2f%%)\n\n",
                 (unsigned long long)hit282832, pct282);

        auto dumpHist = [&](const wchar_t* name, const uint64_t* h) {
            fwprintf(rep, L"  %ls:\n", name);
            for (int k = 0; k < 8; ++k)
                fwprintf(rep, L"    [%3d..%3d] %10llu (%.1f%%)\n",
                         k * 32, k * 32 + 31,
                         (unsigned long long)h[k],
                         100.0 * double(h[k]) / dtotal);
        };
        fwprintf(rep, L"[histograms]\n");
        dumpHist(L"alpha", alphaBuckets);
        dumpHist(L"red",   rBuckets);
        dumpHist(L"green", gBuckets);
        dumpHist(L"blue",  bBuckets);

        fwprintf(rep, L"\n[uv crop]\n");
        fwprintf(rep, L"  uvMin = (%.4f, %.4f)  uvMax = (%.4f, %.4f)\n",
                 uvMinX, uvMinY, uvMaxX, uvMaxY);
        fwprintf(rep, L"  pixel rect = (%u..%u, %u..%u)  -> %llu px\n",
                 cropX0, cropX1, cropY0, cropY1,
                 (unsigned long long)cropCount);
        if (cropCount > 0) {
            fwprintf(rep, L"  crop avg R/G/B/A   : %llu / %llu / %llu / %llu\n",
                     (unsigned long long)(cropSumR / cropCount),
                     (unsigned long long)(cropSumG / cropCount),
                     (unsigned long long)(cropSumB / cropCount),
                     (unsigned long long)(cropSumA / cropCount));
            fwprintf(rep, L"  crop alpha min/max : %d / %d\n",
                     cropMinA, cropMaxA);
            fwprintf(rep, L"  crop max(R,G,B)>A  : %llu (%.2f%%)\n",
                     (unsigned long long)cropPremulViol,
                     100.0 * double(cropPremulViol) / double(cropCount));
            fwprintf(rep, L"  crop #282832-like  : %llu (%.2f%%)\n",
                     (unsigned long long)cropHit282832,
                     100.0 * double(cropHit282832) / double(cropCount));
        } else {
            fwprintf(rep, L"  (empty crop region)\n");
        }

        fwprintf(rep, L"\n[verdict]\n  %ls\n", verdict);
        fclose(rep);
    }

    OutputDebugStringW(L"CKFlip: DebugDumpCachedTexture wrote taskbar dump\n");
    return true;
}
#endif // CKFLIP_DEBUG_TASKBAR
