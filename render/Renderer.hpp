#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <winrt/base.h>

/// Owns the D3D11 device, swap chain, and render state for the overlay window.
/// The swap chain is bound to an existing HWND (the overlay).
class Renderer {
public:
    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    /// Create a new fullscreen popup HWND and initialise D3D11 on it.
    bool Init(HINSTANCE hInstance);
    void Shutdown();

    HWND GetHwnd() const { return m_hwnd; }

    /// Resize swap chain to match the current window size.
    bool Resize(UINT width, UINT height);

    /// Clear the render target and bind it.
    void BeginFrame();

    /// Present the frame (with tearing if supported).
    void EndFrame();

    /// Present with vsync (blocks until compositor picks up the frame).
    void EndFrameVSync();

    ID3D11Device*        GetDevice()  const { return m_device.get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.get(); }

    /// Show / hide the D3D overlay window.
    void Show();
    void Hide();

    /// Resize the overlay window to span the whole virtual screen.
    void CoverAllMonitors();

private:
    bool CreateD3DWindow(HINSTANCE hInstance);
    bool CreateDeviceAndSwapChain();
    bool CreateRenderTarget();
    void CreateBlendState();
    void CreateDepthStencilState();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND m_hwnd = nullptr;

    winrt::com_ptr<ID3D11Device>           m_device;
    winrt::com_ptr<ID3D11DeviceContext>     m_context;
    winrt::com_ptr<IDXGISwapChain1>        m_swapChain;
    winrt::com_ptr<ID3D11RenderTargetView> m_rtv;
    winrt::com_ptr<ID3D11BlendState>       m_blendState;
    winrt::com_ptr<ID3D11DepthStencilState> m_depthStencilState;

    UINT m_width  = 0;
    UINT m_height = 0;
    bool m_tearingSupported = false;
};
