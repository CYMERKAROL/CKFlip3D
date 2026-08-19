// ---------------------------------------------------------------------------
// Bringing up the overlay: a layered, click-through popup with no redirection
// bitmap, a composition swap chain bound to it through DirectComposition, and
// the present paths the render loop picks between.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "Renderer.hpp"
#include "../core/DebugLog.h"
#include "../core/Diagnostics.h"
#include <d3d11_4.h>
#include <dwmapi.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

#include <dcomp.h>
#pragma comment(lib, "dcomp.lib")

// DirectComposition objects for binding the composition swap chain to the
// WS_EX_NOREDIRECTIONBITMAP window.  File-scope so the DirectComposition
// headers stay out of Renderer.hpp and its includers.
static winrt::com_ptr<IDCompositionDevice> g_dcompDevice;
static winrt::com_ptr<IDCompositionTarget> g_dcompTarget;
static winrt::com_ptr<IDCompositionVisual> g_dcompVisual;

static constexpr const wchar_t* kD3DOverlayClass = L"CKFlip3D_D3DOverlay";
static bool g_d3dClassRegistered = false;

LRESULT CALLBACK Renderer::WndProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    switch (msg) {
    case WM_MOUSEMOVE:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return 0;
    case WM_SETCURSOR: {
        static HCURSOR hArrow = LoadCursorW(nullptr, IDC_ARROW);
        SetCursor(hArrow);
        return TRUE;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Renderer::CreateD3DWindow(HINSTANCE hInstance)
{
    if (!g_d3dClassRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize       = sizeof(wc);
        wc.lpfnWndProc  = WndProc;
        wc.hInstance     = hInstance;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kD3DOverlayClass;

        if (!RegisterClassExW(&wc)) {
            Diag::ReportLastError(Diag::Code::OverlayWindowFailed, Diag::Sev::Critical,
                                  L"CKFlip3D could not register its overlay window class");
            return false;
        }
        g_d3dClassRegistered = true;
    }

    // WS_EX_NOREDIRECTIONBITMAP — no GDI surface, DX renders directly onto
    // the compositor surface.  Combined with TOPMOST + NOACTIVATE for overlay.
    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kD3DOverlayClass, L"",
        WS_POPUP,
        0, 0, 1, 1,
        nullptr, nullptr, hInstance, nullptr);

    if (!m_hwnd) {
        Diag::ReportLastError(Diag::Code::OverlayWindowFailed, Diag::Sev::Critical,
                              L"CKFlip3D could not create its overlay window");
        return false;
    }

    BOOL excludePeek = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_EXCLUDED_FROM_PEEK,
                          &excludePeek, sizeof(excludePeek));
    return true;
}

bool Renderer::CreateDeviceAndSwapChain()
{
    // Create the DXGI factory first so we can query tearing support.
    winrt::com_ptr<IDXGIFactory2> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::DxgiFactoryFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not reach the graphics subsystem", hr,
                       L"CreateDXGIFactory1");
        return false;
    }

    // Step 1: query tearing support before creating the swap chain.
    m_tearingSupported = false;
    {
        winrt::com_ptr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(factory5.put())))) {
            BOOL supported = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(
                    DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                    &supported, sizeof(supported))))
                m_tearingSupported = (supported != FALSE);
        }
    }

    // Create device (hardware, feature level 11.0).
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createFlags, &featureLevel, 1,
        D3D11_SDK_VERSION,
        device.put(), nullptr, context.put());

    if (FAILED(hr)) {
        // Try WARP (software) fallback — if this works, 3D acceleration is missing.
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createFlags, &featureLevel, 1,
            D3D11_SDK_VERSION,
            device.put(), nullptr, context.put());

        if (SUCCEEDED(hr)) {
            // Used to be a modal box at startup, which the user dismissed once
            // and could never see again \u2014 the exact failure mode the log
            // exists to end.
            Diag::Report(Diag::Code::GpuSoftwareFallback, Diag::Sev::Warning,
                         L"No hardware 3D acceleration \u2014 CKFlip3D is rendering in software",
                         L"the D3D11 hardware device could not be created and "
                         L"WARP took over; expect low frame rates. In a virtual "
                         L"machine, enable 3D acceleration in the VM settings");
        } else {
            Diag::ReportHr(Diag::Code::GpuDeviceFailed, Diag::Sev::Critical,
                           L"CKFlip3D could not create a Direct3D 11 device", hr,
                           L"neither hardware nor WARP");
            return false;
        }
    }

    // Query adapter info for debug output.
    {
        winrt::com_ptr<IDXGIDevice> dxgiDev;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgiDev.put())))) {
            winrt::com_ptr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDev->GetAdapter(adapter.put()))) {
                DXGI_ADAPTER_DESC adapterDesc;
                if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
                    wchar_t buf[512];
                    swprintf_s(buf, L"CKFlip3D: GPU = %s, VRAM = %llu MB\n",
                              adapterDesc.Description,
                              (unsigned long long)(adapterDesc.DedicatedVideoMemory / (1024 * 1024)));
                    CKLog::Log(buf);

                    // Warn if dedicated VRAM is very low (< 128 MB).
                    if (adapterDesc.DedicatedVideoMemory > 0 &&
                        adapterDesc.DedicatedVideoMemory < 128ULL * 1024 * 1024) {
                        // _TRUNCATE: the adapter description is a driver
                        // string of up to 128 characters and is not this
                        // program's to bound.
                        wchar_t detail[320];
                        _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                            L"%s reports %llu MB of dedicated video memory; live "
                            L"previews of many windows may not fit",
                            adapterDesc.Description,
                            static_cast<unsigned long long>(
                                adapterDesc.DedicatedVideoMemory / (1024 * 1024)));
                        Diag::Report(Diag::Code::GpuLowVideoMemory, Diag::Sev::Warning,
                                     L"Very little video memory on this display adapter",
                                     detail, /*sticky=*/true);
                    }
                }
            }
        }
    }

    m_device  = device;
    m_context = context;

    // The WGC free-threaded frame pools (capture/WGCCapture) deliver and
    // copy frames on threadpool threads while this thread renders on the
    // SAME device.  Concurrent immediate-context use is only defined with
    // the D3D11 internal lock enabled — without it the two paths race,
    // which surfaced as wedged capture sessions on slower GPUs and VMs.
    // (Required by the Windows.Graphics.Capture docs for shared devices.)
    {
        winrt::com_ptr<ID3D11Multithread> multithread;
        if (SUCCEEDED(m_context->QueryInterface(
                IID_PPV_ARGS(multithread.put()))))
            multithread->SetMultithreadProtected(TRUE);
        else
            Diag::Report(Diag::Code::MultithreadUnavailable, Diag::Sev::Warning,
                         L"CKFlip3D could not lock its graphics device for shared use",
                         L"ID3D11Multithread is unavailable; live window previews "
                         L"share this device with capture threads and may freeze "
                         L"or tear on this driver");
    }

    // Step 2: create composition swap chain with premultiplied alpha.
    // Composition swap chains don't support tearing.
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width       = 1;
    desc.Height      = 1;
    desc.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc  = { 1, 0 };
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;

    hr = factory->CreateSwapChainForComposition(
        m_device.get(), &desc, nullptr, m_swapChain.put());
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::SwapChainFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not create its drawing surface", hr,
                       L"CreateSwapChainForComposition");
        return false;
    }

    // Tearing is not supported with composition swap chains.
    m_tearingSupported = false;

    // Bind the composition swap chain to the HWND via DirectComposition.
    winrt::com_ptr<IDXGIDevice> dxgiDev;
    m_device->QueryInterface(IID_PPV_ARGS(dxgiDev.put()));

    hr = DCompositionCreateDevice(dxgiDev.get(), IID_PPV_ARGS(g_dcompDevice.put()));
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::CompositionFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not reach the desktop compositor", hr,
                       L"DCompositionCreateDevice — the overlay cannot be shown "
                       L"without it; check that Desktop Window Manager is running");
        return false;
    }

    hr = g_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, g_dcompTarget.put());
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::CompositionFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not attach its overlay to the screen", hr,
                       L"CreateTargetForHwnd");
        return false;
    }

    hr = g_dcompDevice->CreateVisual(g_dcompVisual.put());
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::CompositionFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not build its compositor visual", hr,
                       L"CreateVisual");
        return false;
    }

    g_dcompVisual->SetContent(m_swapChain.get());
    g_dcompTarget->SetRoot(g_dcompVisual.get());
    hr = g_dcompDevice->Commit();
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::CompositionFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not commit its overlay to the compositor",
                       hr, L"IDCompositionDevice::Commit");
        return false;
    }

    return true;
}

bool Renderer::CreateRenderTarget()
{
    m_rtv = nullptr;

    winrt::com_ptr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()));
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::BackbufferResizeFailed, Diag::Sev::Critical,
                       L"CKFlip3D lost its drawing surface", hr,
                       L"IDXGISwapChain::GetBuffer");
        return false;
    }

    hr = m_device->CreateRenderTargetView(backBuffer.get(), nullptr, m_rtv.put());
    if (FAILED(hr)) {
        Diag::ReportHr(Diag::Code::BackbufferResizeFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not prepare its drawing surface", hr,
                       L"CreateRenderTargetView");
        return false;
    }
    return true;
}

void Renderer::CreateBlendState()
{
    D3D11_BLEND_DESC desc = {};
    desc.RenderTarget[0].BlendEnable           = TRUE;
    desc.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    desc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    desc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    desc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    m_device->CreateBlendState(&desc, m_blendState.put());
}

void Renderer::CreateDepthStencilState()
{
    D3D11_DEPTH_STENCIL_DESC desc = {};
    desc.DepthEnable    = FALSE;
    desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    m_device->CreateDepthStencilState(&desc, m_depthStencilState.put());
}

Renderer::~Renderer()
{
    Shutdown();
}

bool Renderer::Init(HINSTANCE hInstance)
{
    if (!CreateD3DWindow(hInstance))
        return false;

    if (!CreateDeviceAndSwapChain())
        return false;

    CreateBlendState();
    CreateDepthStencilState();
    return true;
}

void Renderer::Shutdown()
{
    if (m_context)
        m_context->ClearState();

    m_rtv              = nullptr;
    m_blendState       = nullptr;
    m_depthStencilState = nullptr;
    m_swapChain        = nullptr;
    g_dcompVisual      = nullptr;
    g_dcompTarget      = nullptr;
    g_dcompDevice      = nullptr;
    m_context          = nullptr;
    m_device           = nullptr;

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_width = 0;
    m_height = 0;
}

bool Renderer::Resize(UINT width, UINT height)
{
    if (width == 0 || height == 0)
        return false;
    if (width == m_width && height == m_height)
        return true;

    m_width  = width;
    m_height = height;

    // Release old RTV before resizing.
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv = nullptr;

    UINT flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height,
                                             DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) {
        wchar_t detail[128];
        swprintf_s(detail, L"ResizeBuffers to %ux%u", width, height);
        Diag::ReportHr(Diag::Code::BackbufferResizeFailed, Diag::Sev::Critical,
                       L"CKFlip3D could not resize its overlay to the desktop", hr,
                       detail);
        return false;
    }

    return CreateRenderTarget();
}

// ---------------------------------------------------------------------------
// A Present that fails is either a lost device or a compositor that has gone
// away, and both look identical from the user's chair: the cascade freezes or
// never appears.  Naming which one it was is the whole point of asking.
void Renderer::ReportPresentFailure(HRESULT hr)
{
    if (SUCCEEDED(hr))
        return;

    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : hr;
        wchar_t detail[160];
        swprintf_s(detail, L"device removed reason 0x%08X",
                   static_cast<unsigned>(reason));
        Diag::ReportHr(Diag::Code::DeviceRemoved, Diag::Sev::Critical,
                       L"The graphics device was reset while CKFlip3D was drawing",
                       hr, detail);
        return;
    }
    Diag::ReportHr(Diag::Code::PresentFailed, Diag::Sev::Warning,
                   L"CKFlip3D could not present a frame", hr);
}

void Renderer::BeginFrame()
{
    // Ensure we have a render target matching window size.
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    UINT w = static_cast<UINT>(rc.right  - rc.left);
    UINT h = static_cast<UINT>(rc.bottom - rc.top);
    if (w != m_width || h != m_height)
        Resize(w, h);

    // OPAQUE black, not transparent.  The overlay's backdrop has to block
    // everything behind it, and it used to get there in two steps: clear to
    // transparent, then blend a full-screen black quad with α = 1 over the
    // top.  The blend result is (0,0,0,1) for every pixel — exactly what the
    // clear can write directly, and a clear is a fast path on every GPU
    // while the quad was a full-screen read-modify-write of the back buffer.
    // Dropping it takes a quarter of the frame's GPU time off a 1920x1080
    // cascade (measured 368 -> 274 us on a GTX 1650) and scales with the
    // virtual screen, so it is worth most where the overlay is largest.
    // The two are pixel-identical: verified by rendering a full frame both
    // ways and comparing the read-back surfaces byte for byte.
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (m_rtv) {
        ID3D11RenderTargetView* rtv = m_rtv.get();
        m_context->OMSetRenderTargets(1, &rtv, nullptr);
        m_context->ClearRenderTargetView(m_rtv.get(), clearColor);
    }

    // Set blend and depth stencil state.
    float blendFactor[4] = { 0, 0, 0, 0 };
    m_context->OMSetBlendState(m_blendState.get(), blendFactor, 0xFFFFFFFF);
    m_context->OMSetDepthStencilState(m_depthStencilState.get(), 0);

    // Set viewport.
    D3D11_VIEWPORT vp = {};
    vp.Width    = static_cast<float>(m_width);
    vp.Height   = static_cast<float>(m_height);
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
}

void Renderer::EndFrame()
{
    UINT presentFlags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
    ReportPresentFailure(m_swapChain->Present(0, presentFlags));
}

void Renderer::EndFrameVSync()
{
    ReportPresentFailure(m_swapChain->Present(1, 0));
}

void Renderer::Show()
{
    if (m_hwnd) {
        CoverAllMonitors();
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }
}

void Renderer::Hide()
{
    if (m_hwnd)
        ShowWindow(m_hwnd, SW_HIDE);
}

void Renderer::CoverAllMonitors()
{
    if (!m_hwnd)
        return;
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}
