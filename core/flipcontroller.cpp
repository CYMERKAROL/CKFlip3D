#include "flipcontroller.h"
#include "DebugLog.h"
#include "../capture/windowcloaker.h"
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <DirectXMath.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <shldisp.h>
#include <objbase.h>

// ---------------------------------------------------------------------------
// v8.7 Bug TB / HC13 — shared content-band UV crop.
//
// One implementation used by every taskbar draw/dump site so they all compute
// the SAME uvMinY/uvMaxY.  On Win10 22H2 / Win11 24H2 the WGC capture of
// Shell_TrayWnd is far taller than the visible bar, with the real taskbar in a
// thin band; DetectContentCenterV (called once in Activate) located that
// band's centre.  We crop a `tbH`-tall window centred on it, slid to stay in
// [0,1].  When `contentResolved` is false (Win11 25H2 — capture already
// bar-sized) the full texture is returned unchanged, so 25H2 is a no-op.
//
// Previously Activate() used the content-band crop but RenderFrame() Layer 2
// reverted to a bottom crop `(texH - tbH)/texH`, which sampled the dark
// #282832 fill on the failing OSes — the taskbar "worked for one frame then
// vanished".  Routing all sites through this helper closes that gap.
static void ComputeTaskbarContentBandUV(int texH, float tbH,
                                        bool contentResolved,
                                        float contentCenterY,
                                        float& outUvMinY, float& outUvMaxY)
{
    outUvMinY = 0.0f;
    outUvMaxY = 1.0f;
    if (!contentResolved)
        return;
    float fTexH = static_cast<float>(texH);
    if (fTexH <= 0.0f || tbH <= 0.0f)
        return;
    float halfBand = (tbH / fTexH) * 0.5f;
    if (halfBand > 0.5f) halfBand = 0.5f;
    float lo = contentCenterY - halfBand;
    float hi = contentCenterY + halfBand;
    if (lo < 0.0f) { hi -= lo; lo = 0.0f; }
    if (hi > 1.0f) { lo -= (hi - 1.0f); hi = 1.0f; }
    if (lo < 0.0f) lo = 0.0f;
    if (hi > 1.0f) hi = 1.0f;
    outUvMinY = lo;
    outUvMaxY = hi;
}

static bool ValidRect(const RECT& r)
{
    return r.right > r.left && r.bottom > r.top;
}

// AddRef-wrap a raw SRV (may be null) — frozen refs must own the view so a
// WGC size-change recreate can't dangle them mid-animation.
static winrt::com_ptr<ID3D11ShaderResourceView> SrvRef(ID3D11ShaderResourceView* p)
{
    winrt::com_ptr<ID3D11ShaderResourceView> ref;
    ref.copy_from(p);
    return ref;
}

FlipController::MonitorLayout FlipController::BuildMonitorLayout() const
{
    MonitorLayout layout{};
    layout.virtualScreen = {
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)
    };
    layout.primary = {
        0,
        0,
        GetSystemMetrics(SM_CXSCREEN),
        GetSystemMetrics(SM_CYSCREEN)
    };

    struct EnumState {
        MonitorLayout* layout = nullptr;
        int monitorCount = 0;
        bool foundPrimary = false;
    } state{ &layout, 0, false };

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM user) -> BOOL {
            auto* state = reinterpret_cast<EnumState*>(user);
            ++state->monitorCount;

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(monitor, &mi)
                && (mi.dwFlags & MONITORINFOF_PRIMARY))
            {
                state->layout->primary = mi.rcMonitor;
                state->foundPrimary = true;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));

    if (!state.foundPrimary) {
        POINT origin{};
        HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (primary && GetMonitorInfoW(primary, &mi))
            layout.primary = mi.rcMonitor;
    }

    auto sameRect = [](const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top
            && a.right == b.right && a.bottom == b.bottom;
    };
    layout.multiMonitor = state.monitorCount > 1
                        && ValidRect(layout.virtualScreen)
                        && ValidRect(layout.primary)
                        && !sameRect(layout.primary, layout.virtualScreen);

    WCHAR buf[256];
    swprintf_s(buf,
        L"CKFlip MONITORS: virtual=(%ld,%ld)-(%ld,%ld) primary=(%ld,%ld)-(%ld,%ld) multi=%d\n",
        layout.virtualScreen.left, layout.virtualScreen.top,
        layout.virtualScreen.right, layout.virtualScreen.bottom,
        layout.primary.left, layout.primary.top,
        layout.primary.right, layout.primary.bottom,
        layout.multiMonitor ? 1 : 0);
    CKLog::Log(buf);

    return layout;
}

static DirectX::XMMATRIX ComputeMonitorRemapNDC(const RECT& primScreen,
                                                float vpW, float vpH,
                                                float originX, float originY)
{
    using namespace DirectX;
    if (vpW <= 0.0f || vpH <= 0.0f || !ValidRect(primScreen))
        return XMMatrixIdentity();

    const float L = static_cast<float>(primScreen.left) - originX;
    const float T = static_cast<float>(primScreen.top) - originY;
    const float W = static_cast<float>(primScreen.right - primScreen.left);
    const float H = static_cast<float>(primScreen.bottom - primScreen.top);
    if (W <= 0.0f || H <= 0.0f)
        return XMMatrixIdentity();

    const float sx = W / vpW;
    const float sy = H / vpH;
    const float cx = ((L + W * 0.5f) / vpW) * 2.0f - 1.0f;
    const float cy = 1.0f - ((T + H * 0.5f) / vpH) * 2.0f;
    return XMMatrixScaling(sx, sy, 1.0f) *
           XMMatrixTranslation(cx, cy, 0.0f);
}

static DirectX::XMMATRIX ComputeScreenRectMVPWithOrigin(const RECT& rect,
                                                        float vpW, float vpH,
                                                        float originX,
                                                        float originY)
{
    using namespace DirectX;
    if (vpW <= 0.0f || vpH <= 0.0f || !ValidRect(rect))
        return XMMatrixIdentity();

    const float left = static_cast<float>(rect.left) - originX;
    const float top = static_cast<float>(rect.top) - originY;
    const float w = static_cast<float>(rect.right - rect.left);
    const float h = static_cast<float>(rect.bottom - rect.top);
    if (w <= 0.0f || h <= 0.0f)
        return XMMatrixIdentity();

    const float scaleX = (w / vpW) * 2.0f;
    const float scaleY = (h / vpH) * 2.0f;
    const float cx = ((left + w * 0.5f) / vpW) * 2.0f - 1.0f;
    const float cy = 1.0f - ((top + h * 0.5f) / vpH) * 2.0f;
    return XMMatrixScaling(scaleX, scaleY, 1.0f) *
           XMMatrixTranslation(cx, cy, 0.0f);
}

static void ApplyTextureUV(QuadDrawCall& draw, const DirectX::XMFLOAT4& uv)
{
    draw.uvMinX = uv.x;
    draw.uvMinY = uv.y;
    draw.uvMaxX = uv.z;
    draw.uvMaxY = uv.w;
}

static bool ResolveTaskbarVisibleRect(HWND taskbar, bool taskbarAutoHide, RECT& out)
{
    if (!taskbar)
        return false;

    RECT tbRect{};
    bool gotRect = false;
    {
        HMONITOR hmon = MonitorFromWindow(taskbar, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hmon, &mi)) {
            if (mi.rcWork.bottom < mi.rcMonitor.bottom) {
                tbRect = { mi.rcMonitor.left, mi.rcWork.bottom,
                           mi.rcMonitor.right, mi.rcMonitor.bottom };
                gotRect = true;
            } else if (mi.rcWork.top > mi.rcMonitor.top) {
                tbRect = { mi.rcMonitor.left, mi.rcMonitor.top,
                           mi.rcMonitor.right, mi.rcWork.top };
                gotRect = true;
            } else if (mi.rcWork.left > mi.rcMonitor.left) {
                tbRect = { mi.rcMonitor.left, mi.rcMonitor.top,
                           mi.rcWork.left, mi.rcMonitor.bottom };
                gotRect = true;
            } else if (mi.rcWork.right < mi.rcMonitor.right) {
                tbRect = { mi.rcWork.right, mi.rcMonitor.top,
                           mi.rcMonitor.right, mi.rcMonitor.bottom };
                gotRect = true;
            }
        }
    }

    if (taskbarAutoHide) {
        RECT windowRect{};
        if (GetWindowRect(taskbar, &windowRect) && ValidRect(windowRect)) {
            tbRect = windowRect;
            gotRect = true;
        }
    }

    if (!gotRect && !GetWindowRect(taskbar, &tbRect))
        return false;
    if (!ValidRect(tbRect))
        return false;

    out = tbRect;
    return true;
}

static RECT ScreenToOverlayRect(const RECT& screenRect, float originX, float originY)
{
    return RECT{
        static_cast<LONG>(static_cast<float>(screenRect.left) - originX),
        static_cast<LONG>(static_cast<float>(screenRect.top) - originY),
        static_cast<LONG>(static_cast<float>(screenRect.right) - originX),
        static_cast<LONG>(static_cast<float>(screenRect.bottom) - originY)
    };
}

static float RectOverlapRatio(const RECT& a, const RECT& b)
{
    if (!ValidRect(a) || !ValidRect(b))
        return 0.0f;

    const LONG left = (std::max)(a.left, b.left);
    const LONG top = (std::max)(a.top, b.top);
    const LONG right = (std::min)(a.right, b.right);
    const LONG bottom = (std::min)(a.bottom, b.bottom);
    if (right <= left || bottom <= top)
        return 0.0f;

    const float overlap = static_cast<float>(right - left)
                        * static_cast<float>(bottom - top);
    const float areaA = static_cast<float>(a.right - a.left)
                      * static_cast<float>(a.bottom - a.top);
    const float areaB = static_cast<float>(b.right - b.left)
                      * static_cast<float>(b.bottom - b.top);
    const float denom = (std::min)(areaA, areaB);
    return denom > 0.0f ? overlap / denom : 0.0f;
}

void FlipController::UpdateCascadeSpace(float vpW, float vpH)
{
    m_overlayOriginX = static_cast<float>(m_monLayout.virtualScreen.left);
    m_overlayOriginY = static_cast<float>(m_monLayout.virtualScreen.top);

    const LONG primaryW = m_monLayout.primary.right - m_monLayout.primary.left;
    const LONG primaryH = m_monLayout.primary.bottom - m_monLayout.primary.top;
    m_cascadeW = primaryW > 0 ? static_cast<float>(primaryW) : vpW;
    m_cascadeH = primaryH > 0 ? static_cast<float>(primaryH) : vpH;
    if (m_cascadeW <= 0.0f) m_cascadeW = 1920.0f;
    if (m_cascadeH <= 0.0f) m_cascadeH = 1080.0f;
    m_cascadeAspect = (m_cascadeH > 0.0f) ? (m_cascadeW / m_cascadeH)
                                          : (16.0f / 9.0f);

    DirectX::XMStoreFloat4x4(
        &m_monRemapNDC,
        ComputeMonitorRemapNDC(m_monLayout.primary, vpW, vpH,
                               m_overlayOriginX, m_overlayOriginY));
}

static DirectX::XMMATRIX ComputeScreenSpaceMVP(const RECT& rect, float vpW, float vpH)
{
    using namespace DirectX;
    if (vpW <= 0.0f || vpH <= 0.0f)
        return XMMatrixIdentity();

    const float originX = static_cast<float>(GetSystemMetrics(SM_XVIRTUALSCREEN));
    const float originY = static_cast<float>(GetSystemMetrics(SM_YVIRTUALSCREEN));
    const float left    = static_cast<float>(rect.left)   - originX;
    const float right   = static_cast<float>(rect.right)  - originX;
    const float top     = static_cast<float>(rect.top)    - originY;
    const float bottom  = static_cast<float>(rect.bottom) - originY;

    const float nL = 2.0f * left   / vpW - 1.0f;
    const float nR = 2.0f * right  / vpW - 1.0f;
    const float nT = 1.0f - 2.0f * top    / vpH;
    const float nB = 1.0f - 2.0f * bottom / vpH;
    const float scX = nR - nL;
    const float scY = nT - nB;
    const float tx = (nL + nR) * 0.5f;
    const float ty = (nT + nB) * 0.5f;

    return XMMatrixScaling(scX, scY, 1.0f) *
           XMMatrixTranslation(tx, ty, 0.0f);
}

static DirectX::XMMATRIX LerpMatrix(DirectX::XMMATRIX a,
                                    DirectX::XMMATRIX b,
                                    float t)
{
    using namespace DirectX;
    t = std::clamp(t, 0.0f, 1.0f);
    return XMMATRIX{
        XMVectorLerp(a.r[0], b.r[0], t),
        XMVectorLerp(a.r[1], b.r[1], t),
        XMVectorLerp(a.r[2], b.r[2], t),
        XMVectorLerp(a.r[3], b.r[3], t)
    };
}

static const RECT& ResolveMorphScreenRect(const EntryExitAnimator& animator,
                                          const std::vector<WindowInfo>& windows,
                                          size_t idx)
{
    const std::vector<RECT>& flatRects = animator.GetFlatSourceRects();
    if (idx < flatRects.size() && ValidRect(flatRects[idx]))
        return flatRects[idx];
    return windows[idx].rect;
}

#ifdef CKFLIP_DEBUG_TASKBAR
// ---------------------------------------------------------------------------
// Bug 11' v8.4 Patch D/E — runtime taskbar debug modes + pre/post-hide dumps.
// Selected via the CKFLIP_TASKBAR_MODE environment variable.  Debug builds
// only; release builds compile none of this.
// ---------------------------------------------------------------------------
enum class TaskbarDebugMode {
    Normal,
    DisableLayer,
    AssumeStraightAlpha,
    SolidRed,
    FreezePreHide,
    NoHideRealTaskbar,
};

static TaskbarDebugMode ReadTaskbarDebugMode()
{
    wchar_t buf[64] = {};
    DWORD n = GetEnvironmentVariableW(L"CKFLIP_TASKBAR_MODE", buf, 64);
    if (n == 0 || n >= 64) return TaskbarDebugMode::Normal;
    if (_wcsicmp(buf, L"disable")  == 0) return TaskbarDebugMode::DisableLayer;
    if (_wcsicmp(buf, L"straight") == 0) return TaskbarDebugMode::AssumeStraightAlpha;
    if (_wcsicmp(buf, L"red")      == 0) return TaskbarDebugMode::SolidRed;
    if (_wcsicmp(buf, L"freeze")   == 0) return TaskbarDebugMode::FreezePreHide;
    if (_wcsicmp(buf, L"nohide")   == 0) return TaskbarDebugMode::NoHideRealTaskbar;
    return TaskbarDebugMode::Normal;
}

// Cached per Activate so we don't hit the env API per frame.
static TaskbarDebugMode g_taskbarDebugMode = TaskbarDebugMode::Normal;

// Pre-hide taskbar SRV for `freeze` mode — strong COM reference (v8.4.1 §4.3)
// so it survives WGCCapture swapping its cached SRV.
static winrt::com_ptr<ID3D11ShaderResourceView> g_taskbarFreezeSRV;

// One-shot UV-aware taskbar dump written next to the executable.
// v8.7.1 §1 — takes the content-band state so the dump reports the EXACT crop
// the renderer uses, not the legacy bottom crop (HC13).
static void DumpTaskbarDebug(WGCCapture* cap, const RECT& tbRect,
                             bool contentResolved, float contentCenterY,
                             const wchar_t* suffix)
{
    if (!cap || !cap->HasCachedFrame()) return;
    int texW = 0, texH = 0;
    cap->GetCapturedSize(texW, texH);
    float tbH = static_cast<float>(tbRect.bottom - tbRect.top);
    float uvMinY = 0.0f, uvMaxY = 1.0f;
    ComputeTaskbarContentBandUV(texH, tbH, contentResolved, contentCenterY,
                                uvMinY, uvMaxY);
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return;
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) slash[1] = L'\0';
    wchar_t base[MAX_PATH] = {};
    wcscpy_s(base, exePath);
    wcscat_s(base, suffix);
    cap->DebugDumpCachedTexture(base, 0.0f, uvMinY, 1.0f, uvMaxY);
}
#endif // CKFLIP_DEBUG_TASKBAR

static void DrawTaskbarLayer(ID3D11DeviceContext* ctx,
                             QuadRenderer& quad,
                             WGCCapture* cap,
                             ID3D11ShaderResourceView* taskbarSRV,
                             const RECT& taskbarRect,
                             bool contentResolved,
                             float contentCenterY,
                             float vpW,
                             float vpH,
                             bool allowDebugFreeze = true)
{
#ifdef CKFLIP_DEBUG_TASKBAR
    if (g_taskbarDebugMode == TaskbarDebugMode::DisableLayer)
        taskbarSRV = nullptr;
    else if (g_taskbarDebugMode == TaskbarDebugMode::FreezePreHide
             && g_taskbarFreezeSRV
             && allowDebugFreeze)
        taskbarSRV = g_taskbarFreezeSRV.get();

    if (g_taskbarDebugMode == TaskbarDebugMode::SolidRed) {
        float tbW = static_cast<float>(taskbarRect.right - taskbarRect.left);
        float tbH = static_cast<float>(taskbarRect.bottom - taskbarRect.top);
        if (tbW > 0.0f && tbH > 0.0f && vpW > 0.0f && vpH > 0.0f) {
            float scaleX = (tbW / vpW) * 2.0f;
            float scaleY = (tbH / vpH) * 2.0f;
            float cx = ((taskbarRect.left + tbW * 0.5f) / vpW) * 2.0f - 1.0f;
            float cy = 1.0f - ((taskbarRect.top + tbH * 0.5f) / vpH) * 2.0f;
            QuadDrawCall tbDraw;
            DirectX::XMStoreFloat4x4(&tbDraw.mvp,
                DirectX::XMMatrixScaling(scaleX, scaleY, 1.0f)
                * DirectX::XMMatrixTranslation(cx, cy, 0.0f));
            tbDraw.alpha      = 1.0f;
            tbDraw.blurAmount = 0.0f;
            quad.DrawDebugRed(ctx, tbDraw);
        }
        taskbarSRV = nullptr;
    }
#else
    (void)allowDebugFreeze;
#endif

    if (!taskbarSRV || !cap || vpW <= 0.0f || vpH <= 0.0f)
        return;

    int texW = 0, texH = 0;
    cap->GetCapturedSize(texW, texH);
    if (texW <= 0 || texH <= 0)
        return;

    float tbW = static_cast<float>(taskbarRect.right - taskbarRect.left);
    float tbH = static_cast<float>(taskbarRect.bottom - taskbarRect.top);
    if (tbW <= 0.0f || tbH <= 0.0f) {
        tbW = static_cast<float>(texW);
        tbH = static_cast<float>(texH);
    }

    float scaleX = (tbW / vpW) * 2.0f;
    float scaleY = (tbH / vpH) * 2.0f;
    float cx = ((taskbarRect.left + tbW * 0.5f) / vpW) * 2.0f - 1.0f;
    float cy = 1.0f - ((taskbarRect.top + tbH * 0.5f) / vpH) * 2.0f;

    QuadDrawCall tbDraw;
    DirectX::XMStoreFloat4x4(&tbDraw.mvp,
        DirectX::XMMatrixScaling(scaleX, scaleY, 1.0f)
        * DirectX::XMMatrixTranslation(cx, cy, 0.0f));
    tbDraw.alpha      = 1.0f;
    tbDraw.blurAmount = 0.0f;
    ComputeTaskbarContentBandUV(texH, tbH, contentResolved, contentCenterY,
        tbDraw.uvMinY, tbDraw.uvMaxY);

#ifdef CKFLIP_DEBUG_TASKBAR
    if (g_taskbarDebugMode == TaskbarDebugMode::AssumeStraightAlpha)
        quad.DrawAssumeStraightAlpha(ctx, taskbarSRV, tbDraw);
    else
        quad.Draw(ctx, taskbarSRV, tbDraw);
#else
    quad.Draw(ctx, taskbarSRV, tbDraw);
#endif
}

// ---------------------------------------------------------------------------
bool FlipController::Init(HINSTANCE hInstance)
{
    m_hInstance = hInstance;

    QueryPerformanceFrequency(&m_perfFreq);

    // Refresh budget for the auto perf tune — primary display refresh rate.
    {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)
            && dm.dmDisplayFrequency >= 30) {
            m_refreshBudgetMs = 1000.0 / static_cast<double>(dm.dmDisplayFrequency);
        }
    }

    if (!m_renderer.Init(hInstance))
        return false;

    if (!m_quad.Init(m_renderer.GetDevice()))
        return false;

    return true;
}

void FlipController::Shutdown()
{
    // Immediate teardown — Escape() would only BEGIN the exit morph and
    // rely on further RenderFrame calls that never come during shutdown,
    // leaving the real taskbar hidden/disabled and windows cloaked.
    if (m_active)
        FinishEscape();

    // Safety net: force-uncloak everything in case normal uncloak failed.
    WindowCloaker::ForceUncloakEverything();

    m_captureCache.clear();
    m_renderer.Shutdown();
}

// ---------------------------------------------------------------------------
void FlipController::Activate()
{
    if (m_active)
        return;

    m_monLayout = BuildMonitorLayout();

    // Re-measure the primary display's refresh rate each session — the
    // user may have changed the primary monitor (or its mode) since Init,
    // and both the auto perf tune budget and the start-delay derivation
    // key off it.
    {
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)
            && dm.dmDisplayFrequency >= 30) {
            m_refreshBudgetMs = 1000.0 / static_cast<double>(dm.dmDisplayFrequency);
        }
    }

    // Launch-timing keypress anchor.  Captured as soon as we enter
    // Activate() so it covers the WHOLE loading phase that runs before
    // the first content render is presented: window scan, capture init,
    // EnsureFrame fallbacks, taskbar capture, cloak setup, the first
    // BeginFrame/EndFrame, and finally Show().  The animator's
    // DimFactor() samples the dim curve at (now - activateStartQPC) /
    // duration on entry, so the first visible frame's dim is already
    // wherever the dim animation would have been if it had run since
    // keypress on a fast machine.  Cascade tile motion is unaffected.
    LARGE_INTEGER activateStartQPC{};
    QueryPerformanceCounter(&activateStartQPC);

    // Defensive: clear any cycle-anim state lingering from a previous session.
    // See FinishDismiss() for the failure mode this guards against — without
    // it, the next session's first RenderFrame sees a stale m_active=true on
    // the cycle anim and writes pre-rotated slot positions over the flat-
    // state ApplyState that BeginEntry just installed, producing a one-frame
    // "leak" of a window that wasn't even cycled in this session.
    m_cycleAnim.Cancel();
    m_closeAnim.Cancel();       // same cross-session stale-state guard
    ClearClosingCaptures();
    m_cycleQueue.clear();
    m_reverseDelayPending = false;
    m_exitSelectedStableSRV = nullptr;
    m_exitSelectedStableTexture = nullptr;
    m_exitSelectedStableHwnd = nullptr;
    m_frozenTaskbarSRV = nullptr;
    m_taskbarDrawOnTop = false;

    // Detect "activated on desktop" before scanning windows so the entry
    // morph can fade the desktop tile in from α=0 (Win7 behaviour).  The
    // foreground may shift after Activate() begins, so capture it now.
    {
        HWND fg = GetForegroundWindow();
        WCHAR cls[64] = {};
        if (fg) GetClassNameW(fg, cls, static_cast<int>(_countof(cls)));
        m_activatedOnDesktop =
            (fg == nullptr)
            || (lstrcmpiW(cls, L"Progman") == 0)
            || (lstrcmpiW(cls, L"WorkerW") == 0);
    }

    m_active = true;

    // 1. Size the overlay to cover all monitors but DON'T show yet.
    //    We'll show only after rendering the first content frame.
    m_renderer.CoverAllMonitors();

    // 2. Scan windows — purely read-only, no state changes.
    DWORD myPid = GetCurrentProcessId();
    m_windows = WindowScanner::Enumerate(myPid);

    // 3. Exclude our overlay and inject desktop pseudo-window.
    DeduplicateWindows();
    InjectDesktopWindow();
    UpdateDesktopCaptureGeometry();

    // 3b. Sort windows by program (grouped by PID, smaller first within group).
    SortWindowsByProgram();

    // 4. If no windows remain, abort.
    if (m_windows.empty()) {
        m_active = false;
        return;
    }

    // 5. Get viewport dimensions for resolution-independent layout.
    RECT rc;
    GetClientRect(m_renderer.GetHwnd(), &rc);
    float vpW = static_cast<float>(rc.right - rc.left);
    float vpH = static_cast<float>(rc.bottom - rc.top);
    if (vpW <= 0) vpW = 1920.0f;
    if (vpH <= 0) vpH = 1080.0f;
    UpdateCascadeSpace(vpW, vpH);

    // 6. Build the 3D scene with viewport-adaptive layout.
    //    Display at most maxWindows slots; the rest stay off-screen but
    //    rotate into view when cycling.
    uint32_t totalWin    = static_cast<uint32_t>(m_windows.size());
    uint32_t displayCount = totalWin;
    if (m_config && m_config->maxWindows < displayCount)
        displayCount = m_config->maxWindows;
    m_scene.BuildSlots(displayCount, m_cascadeW, m_cascadeH);
    RebuildSceneAspects();

    // 6a. Lazy-init the MSAA taskbar-button locator only when at least one
    //     window in this session is minimized.  Skipping it for the no-
    //     minimized case avoids a per-activation cross-process call to
    //     Explorer (the locator's bootstrap calls SendMessageTimeoutW on
    //     Shell_TrayWnd).  Failure here is non-fatal — FlatStackBuilder
    //     falls back to rcNormalPosition.
    {
        bool anyMinimized = false;
        for (size_t i = 0; i < m_windows.size(); ++i) {
            HWND h = m_windows[i].hwnd;
            if (h && h != m_desktopHwnd && IsIconic(h)) { anyMinimized = true; break; }
        }
        if (anyMinimized) m_taskbarLocator.Init();
    }

    // 6b. Begin entry animation — snapshots cascade, builds flat slots from
    //     window rects + camera, overwrites scene with flat state so the
    //     first rendered frame shows tiles at their real desktop positions.
    {
        // Entry/exit fallback dimensions are intentionally primary-monitor
        // sized: the cascade lives on the primary display in the staged
        // multi-monitor model.
        const LONG primaryW = m_monLayout.primary.right - m_monLayout.primary.left;
        const LONG primaryH = m_monLayout.primary.bottom - m_monLayout.primary.top;
        float dW = primaryW > 0 ? static_cast<float>(primaryW) : vpW;
        float dH = primaryH > 0 ? static_cast<float>(primaryH) : vpH;

        // Per-window taskbar-button rect overrides for minimized windows.
        // First-pass: MSAA per-button lookup.  Fallback: synthetic icon-
        // sized rect spaced along Shell_TrayWnd so the tile still emerges
        // from the taskbar visually.  Empty rect = no override → flat
        // rect resolves via rcNormalPosition (legacy behaviour).
        std::vector<RECT> tbOverrides(m_windows.size(), RECT{0,0,0,0});
        {
            // Collect indices of minimized windows in window-list order.
            std::vector<size_t> minIdx;
            minIdx.reserve(m_windows.size());
            for (size_t i = 0; i < m_windows.size(); ++i) {
                HWND h = m_windows[i].hwnd;
                if (h && h != m_desktopHwnd && IsIconic(h))
                    minIdx.push_back(i);
            }

            // First-pass MSAA per-button match.
            if (m_taskbarLocator.IsReady()) {
                for (size_t k : minIdx) {
                    RECT btn;
                    if (m_taskbarLocator.GetButtonRect(m_windows[k].hwnd, btn))
                        tbOverrides[k] = btn;
                }
            }

            // Fallback: place every unmatched minimized window at the SAME
            // single emerge point — an icon-sized rect at the running-task
            // list's left edge (or Shell_TrayWnd's left edge if the list
            // can't be located).  Win7 reference frames show all minimized
            // tiles emerging from one spot; the visible diagonal stack in
            // mid-morph comes from each tile's distinct cascade-end z, not
            // from spread flat starts.  Spreading the flat rects (as a
            // prior iteration did) makes the tiles fan along the taskbar
            // instead of unfurling out of one button.
            RECT emerge{};
            bool haveEmerge = false;
            if (m_taskbarLocator.IsReady()
                && m_taskbarLocator.GetButtonListRect(emerge))
            {
                haveEmerge = true;
            } else if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) {
                if (GetWindowRect(tray, &emerge)
                    && emerge.right > emerge.left && emerge.bottom > emerge.top)
                    haveEmerge = true;
            }

            if (haveEmerge) {
                const LONG tbH = emerge.bottom - emerge.top;
                const LONG iconSide = (tbH > 0 ? tbH : 40);
                const LONG x = emerge.left;
                const LONG y = emerge.top;
                const RECT singlePoint{ x, y, x + iconSide, y + iconSide };
                for (size_t k : minIdx) {
                    const RECT& cur = tbOverrides[k];
                    if (cur.right > cur.left && cur.bottom > cur.top)
                        continue;   // MSAA already filled this one
                    tbOverrides[k] = singlePoint;
                }
            }
        }

        DesktopEntryMode desktopMode = DesktopEntryMode::HiddenUntilCascade;
        if (m_activatedOnDesktop)
            desktopMode = DesktopEntryMode::FadeFromFlat;

        m_entryExitAnimator.BeginEntry(m_scene, m_windows,
                                        vpW, vpH, dW, dH, m_desktopHwnd,
                                        m_cascadeAspect,
                                        m_overlayOriginX, m_overlayOriginY,
                                        DirectX::XMLoadFloat4x4(&m_monRemapNDC),
                                        desktopMode,
                                        tbOverrides,
                                        activateStartQPC.QuadPart);

        // Entry/exit animation off (master toggle or per-animation
        // selection): skip the entry morph — snap straight to the full
        // cascade (mirrors the cycle animator's instant-snap path).
        if (!AnimEntryExitEnabled())
            m_entryExitAnimator.Finalize(m_scene);
    }

    // 7. Start WGC captures — grabs DWM cached surfaces.
    //    Desktop (Progman/WorkerW) capture provides the wallpaper texture.
    StartCaptures();

    // 8. Capture the taskbar via WGC.
    StartTaskbarCapture();

    // 9. WGC warm-up: pump DwmFlush cycles until every capture has its first
    //    frame, bounded by the start-delay budget (config startDelayMs;
    //    auto perf tune substitutes a device-derived value).  The default
    //    16 ms ≈ the original single DwmFlush at 60 Hz, and the all-ready
    //    early exit means larger budgets only ever wait as long as the
    //    slowest capture actually needs.
    {
        const uint32_t delayMs = EffectiveStartDelayMs();
        LARGE_INTEGER wf{}, w0{}, w1{};
        QueryPerformanceFrequency(&wf);
        QueryPerformanceCounter(&w0);
        for (;;) {
            DwmFlush();
            bool allReady = true;
            for (auto& cap : m_captures) {
                if (!cap) continue;
                if (!cap->HasCachedFrame())
                    cap->GetCurrentFrame();   // process any pending WGC frame
                if (!cap->HasCachedFrame())
                    allReady = false;
            }
            if (allReady)
                break;
            QueryPerformanceCounter(&w1);
            double elapsedMs = static_cast<double>(w1.QuadPart - w0.QuadPart)
                             * 1000.0 / static_cast<double>(wf.QuadPart);
            if (elapsedMs >= static_cast<double>(delayMs))
                break;
        }
    }

    // 10. EnsureFrame for all captures — so the first render has content.
    for (auto& cap : m_captures) {
        if (cap && !cap->HasCachedFrame()) {
            cap->GetCurrentFrame();   // process any pending WGC frame
            if (!cap->HasCachedFrame())
                cap->EnsureFrame();   // DwmThumbnail → PrintWindow fallback
        }
    }
    // Ensure taskbar has a frame too.
    if (m_taskbarCapture && !m_taskbarCapture->HasCachedFrame()) {
        m_taskbarCapture->GetCurrentFrame();
        if (!m_taskbarCapture->HasCachedFrame())
            m_taskbarCapture->EnsureFrame();
    }
    for (auto& tray : m_secondaryTrays) {
        if (tray.capture && !tray.capture->HasCachedFrame()) {
            tray.capture->GetCurrentFrame();
            if (!tray.capture->HasCachedFrame())
                tray.capture->EnsureFrame();
        }
    }

    // v8.5 — resolve the taskbar content band so the draw can UV-crop to the
    // real taskbar.  On Win10 / Win11 24H2 the WGC Shell_TrayWnd capture is
    // far taller than the bar with the content in only a thin band; this
    // measures where that band is instead of guessing.  Falls back to the
    // full texture when no content band is found (e.g. Win11 25H2 where the
    // capture is already bar-sized).
    m_taskbarContentResolved = false;
    if (m_taskbarCapture && m_taskbarCapture->HasCachedFrame())
        m_taskbarContentResolved =
            m_taskbarCapture->DetectContentCenterV(m_taskbarContentCenterY);
    for (auto& tray : m_secondaryTrays) {
        tray.contentResolved = false;
        tray.contentCenterY = 0.5f;
        if (tray.capture && tray.capture->HasCachedFrame())
            tray.contentResolved =
                tray.capture->DetectContentCenterV(tray.contentCenterY);
    }

    // Taskbar live preview eligibility — only when the capture is already
    // bar-sized (no content band detected, the Win11 25H2 behaviour).  On
    // builds that deliver the tall 24H2-style capture, live taskbar frames
    // have historically misrendered, so those sessions keep the frozen
    // pre-hide snapshot instead.  Decided BEFORE HideRealTaskbar so the
    // hide step can hold the bar visible for the live stream.
    m_taskbarLiveActive = m_config && m_config->taskbarLivePreview
        && m_taskbarCapture && !m_taskbarContentResolved;
    for (auto& tray : m_secondaryTrays)
        tray.liveActive = m_config && m_config->taskbarLivePreview
            && tray.capture && !tray.contentResolved;

    // Shell_TrayWnd is hidden for the whole CKFlip session, so its live WGC
    // stream can legitimately advance to transparent/empty frames. Keep the
    // pre-hide SRV as the taskbar layer until teardown.
    if (m_taskbarCapture && m_taskbarCapture->HasCachedFrame())
        m_frozenTaskbarSRV = SrvRef(m_taskbarCapture->GetCurrentFrame());
    for (auto& tray : m_secondaryTrays) {
        tray.frozenSRV = (tray.capture && tray.capture->HasCachedFrame())
            ? SrvRef(tray.capture->GetCurrentFrame())
            : nullptr;
    }

#ifdef CKFLIP_DEBUG_TASKBAR
    // Bug 11' v8.4 Patch D/E — read the taskbar debug mode and snapshot the
    // pre-hide state BEFORE HideRealTaskbar() runs, so `nohide`/`freeze`
    // work and the pre-hide dump captures the live source.  Controller
    // thread; once per activation; never from the WGC FrameArrived callback.
    g_taskbarDebugMode = ReadTaskbarDebugMode();
    g_taskbarFreezeSRV = nullptr;
    if (m_taskbarCapture && m_taskbarCapture->HasCachedFrame()) {
        ID3D11ShaderResourceView* rawTb = m_taskbarCapture->GetCurrentFrame();
        if (rawTb)
            g_taskbarFreezeSRV.copy_from(rawTb);   // AddRef — strong ref
    }
    DumpTaskbarDebug(m_taskbarCapture.get(), m_taskbarRect,
                     m_taskbarContentResolved, m_taskbarContentCenterY,
                     L"ckflip_taskbar_prehide");
    for (size_t i = 0; i < m_secondaryTrays.size(); ++i) {
        wchar_t suffix[64] = {};
        swprintf_s(suffix, L"ckflip_taskbar_prehide_sec%zu", i);
        DumpTaskbarDebug(m_secondaryTrays[i].capture.get(),
                         m_secondaryTrays[i].rectOverlay,
                         m_secondaryTrays[i].contentResolved,
                         m_secondaryTrays[i].contentCenterY,
                         suffix);
    }
#endif

    // 11. Render the first CONTENT frame (wallpaper + taskbar + tiles)
    //     into the composition swap chain BEFORE showing the overlay.
    //     This eliminates the black flash — the first visible frame has content.
    {
        m_renderer.BeginFrame();
        m_quad.SetAntialiasing(EffectiveAntialiasing());
        auto* ctx = m_renderer.GetContext();
        float cascadeAspect = m_cascadeAspect;
        DirectX::XMMATRIX monRemap =
            DirectX::XMLoadFloat4x4(&m_monRemapNDC);
        uint32_t count = m_scene.SlotCount();

        m_quad.DrawDim(ctx, 1.0f);

        // Wallpaper background.  Source = Progman/WorkerW WGC capture.
        // Drawn via DrawWallpaper() which uses a PS that fills any
        // transparent strip in the texture (Win11 < 25H2 leaves an α=0
        // band where the taskbar lives) by sampling the closest opaque
        // pixel above.  No-op on Win11 25H2 where the capture is fully
        // opaque.  Wallpaper-Engine and other dynamic-wallpaper apps
        // route their content through Progman, so this preserves them.
        for (size_t i = 0; i < m_windows.size(); ++i) {
            if (m_windows[i].hwnd == m_desktopHwnd && i < m_captures.size() && m_captures[i]) {
                ID3D11ShaderResourceView* srv = m_captures[i]->GetCurrentFrame();
                if (srv) {
                    QuadDrawCall bgDraw;
                    DirectX::XMStoreFloat4x4(&bgDraw.mvp,
                        ComputeScreenRectMVPWithOrigin(m_desktopBackdropRect,
                                                       vpW, vpH,
                                                       m_overlayOriginX,
                                                       m_overlayOriginY));
                    // DimFactor 0 = wallpaper fully visible, 1 = full target dim.
                    // Dim target comes from config (backgroundOpacity %, default
                    // 28 == the original kBgAlpha look); only the endpoint
                    // changes — the animation curve is untouched.
                    const float bgAlpha = m_config
                        ? static_cast<float>(m_config->backgroundOpacity) / 100.0f
                        : kBgAlpha;
                    bgDraw.alpha      = 1.0f - m_entryExitAnimator.DimFactor() * (1.0f - bgAlpha);
                    bgDraw.blurAmount = 0.0f;
                    m_quad.DrawWallpaper(ctx, srv, bgDraw);
                }
                break;
            }
        }

        // Taskbar layer — quad sized to the visible bar rect, UV-cropped to
        // sample only the visible portion of the WGC texture.  On Win11 ≤24H2
        // the Shell_TrayWnd XAML host extends above the visible bar, so the
        // WGC texture is taller than the bar.  Cropping UVs to the bottom
        // (texH - tbH)/texH..1.0 avoids the dark-band artefact without
        // stretching the content.
        {
            ID3D11ShaderResourceView* tbSRV = m_taskbarCapture
                ? m_taskbarCapture->GetCurrentFrame() : nullptr;
#ifdef CKFLIP_DEBUG_TASKBAR
            if (g_taskbarDebugMode == TaskbarDebugMode::DisableLayer)
                tbSRV = nullptr;
            else if (g_taskbarDebugMode == TaskbarDebugMode::FreezePreHide
                     && g_taskbarFreezeSRV)
                tbSRV = g_taskbarFreezeSRV.get();

            if (g_taskbarDebugMode == TaskbarDebugMode::SolidRed) {
                // `red` geometry test — draws even without a taskbar SRV.
                float tbW = static_cast<float>(m_taskbarRect.right  - m_taskbarRect.left);
                float tbH = static_cast<float>(m_taskbarRect.bottom - m_taskbarRect.top);
                if (tbW > 0.0f && tbH > 0.0f) {
                    float scaleX = (tbW / vpW) * 2.0f;
                    float scaleY = (tbH / vpH) * 2.0f;
                    float cx = ((m_taskbarRect.left + tbW * 0.5f) / vpW) * 2.0f - 1.0f;
                    float cy = 1.0f - ((m_taskbarRect.top + tbH * 0.5f) / vpH) * 2.0f;
                    QuadDrawCall tbDraw;
                    DirectX::XMStoreFloat4x4(&tbDraw.mvp,
                        DirectX::XMMatrixScaling(scaleX, scaleY, 1.0f)
                        * DirectX::XMMatrixTranslation(cx, cy, 0.0f));
                    tbDraw.alpha      = 1.0f;
                    tbDraw.blurAmount = 0.0f;
                    m_quad.DrawDebugRed(ctx, tbDraw);
                }
                tbSRV = nullptr;   // skip the normal textured draw
            }
#endif
            if (tbSRV) {
                int texW = 0, texH = 0;
                m_taskbarCapture->GetCapturedSize(texW, texH);
                if (texW > 0 && texH > 0) {
                    float tbW = static_cast<float>(m_taskbarRect.right  - m_taskbarRect.left);
                    float tbH = static_cast<float>(m_taskbarRect.bottom - m_taskbarRect.top);
                    if (tbW <= 0 || tbH <= 0) { tbW = static_cast<float>(texW); tbH = static_cast<float>(texH); }
                    float scaleX = (tbW / vpW) * 2.0f;
                    float scaleY = (tbH / vpH) * 2.0f;
                    float cx = ((m_taskbarRect.left + tbW * 0.5f) / vpW) * 2.0f - 1.0f;
                    float cy = 1.0f - ((m_taskbarRect.top + tbH * 0.5f) / vpH) * 2.0f;

                    QuadDrawCall tbDraw;
                    DirectX::XMStoreFloat4x4(&tbDraw.mvp,
                        DirectX::XMMatrixScaling(scaleX, scaleY, 1.0f)
                        * DirectX::XMMatrixTranslation(cx, cy, 0.0f));
                    tbDraw.alpha      = 1.0f;
                    tbDraw.blurAmount = 0.0f;
                    // v8.7 Bug TB — taskbar UV crop to the MEASURED content
                    // band via the shared helper (same crop as RenderFrame
                    // Layer 2 and the debug dump — HC13).
                    ComputeTaskbarContentBandUV(texH, tbH,
                        m_taskbarContentResolved, m_taskbarContentCenterY,
                        tbDraw.uvMinY, tbDraw.uvMaxY);
#ifdef CKFLIP_DEBUG_TASKBAR
                    if (g_taskbarDebugMode == TaskbarDebugMode::AssumeStraightAlpha)
                        m_quad.DrawAssumeStraightAlpha(ctx, tbSRV, tbDraw);
                    else
                        m_quad.Draw(ctx, tbSRV, tbDraw);
#else
                    m_quad.Draw(ctx, tbSRV, tbDraw);
#endif
                }
            }
        }

        // Tiles at initial positions — MUST be drawn back-to-front (largest
        // Z first) under the painter's algorithm.  Without this sort the
        // first visible frame inverted the on-screen Z-order of every
        // window (slot 0 — the foreground, smallest flat-Z — was drawn
        // first, then back tiles painted over it), and the next RenderFrame
        // suddenly snapped to the correct order.  That single-frame
        // discrepancy is the entry "flash" / "windows overlap randomly
        // then sort normally" symptom.
        //
        // Mirror the main RenderFrame draw list: visible cascade slots
        // (idx >= 0) plus entry-only overflow tiles (idx < 0 → -(k+1)),
        // sorted descending by Z.
        for (auto& tray : m_secondaryTrays) {
            ID3D11ShaderResourceView* secSRV = tray.frozenSRV.get();
            if (!secSRV && tray.capture)
                secSRV = tray.capture->GetCurrentFrame();
            DrawTaskbarLayer(ctx, m_quad, tray.capture.get(), secSRV,
                             tray.rectOverlay, tray.contentResolved,
                             tray.contentCenterY, vpW, vpH, false);
        }

        struct InitialDrawEntry { int idx; float z; };
        const std::vector<TileSlot>& initOverflow = m_entryExitAnimator.GetOverflowSlots();
        const std::vector<HWND>&     initOverflowH = m_entryExitAnimator.GetOverflowHwnds();
        std::vector<InitialDrawEntry> initOrder;
        initOrder.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
            initOrder.push_back({ static_cast<int>(i), m_scene.GetSlot(i).z });
        std::sort(initOrder.begin(), initOrder.end(),
                  [](const InitialDrawEntry& a, const InitialDrawEntry& b) {
                      return a.z > b.z;
                  });

        for (const auto& e : initOrder) {
            if (e.idx < 0) {
                size_t k = static_cast<size_t>(-e.idx - 1);
                if (k >= initOverflow.size()) continue;
                const TileSlot& slot = initOverflow[k];
                if (slot.alpha < 0.001f) continue;

                using namespace DirectX;
                XMMATRIX world =
                    XMMatrixScaling(slot.scaleX, slot.scaleY, 1.0f) *
                    XMMatrixRotationX(XMConvertToRadians(m_scene.GetSceneTiltX())) *
                    XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY())) *
                    XMMatrixTranslation(slot.x, slot.y, slot.z);
                XMVECTOR eye    = XMVectorSet(m_scene.GetCamEyeX(),    m_scene.GetCamEyeY(),    m_scene.GetCamEyeZ(),    1.0f);
                XMVECTOR target = XMVectorSet(m_scene.GetCamTargetX(), m_scene.GetCamTargetY(), m_scene.GetCamTargetZ(), 1.0f);
                XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                XMMATRIX view   = XMMatrixLookAtLH(eye, target, up);
                XMMATRIX proj   = XMMatrixPerspectiveFovLH(
                    XMConvertToRadians(m_scene.GetFovDeg()), cascadeAspect, 0.1f, 200.0f);

                QuadDrawCall draw;
                XMStoreFloat4x4(&draw.mvp, world * view * proj * monRemap);
                draw.alpha = slot.alpha;
                draw.blurAmount = 0.0f;
                if (k < initOverflowH.size()
                    && initOverflowH[k] == m_desktopHwnd)
                    ApplyTextureUV(draw, m_desktopTileUV);

                ID3D11ShaderResourceView* srv = nullptr;
                if (k < initOverflowH.size()) {
                    HWND ohwnd = initOverflowH[k];
                    for (size_t wi = 0; wi < m_windows.size(); ++wi) {
                        if (m_windows[wi].hwnd == ohwnd) {
                            if (wi < m_captures.size() && m_captures[wi])
                                srv = m_captures[wi]->GetCurrentFrame();
                            break;
                        }
                    }
                }
                if (srv) m_quad.Draw(ctx, srv, draw);
                else     m_quad.DrawPlaceholder(ctx, draw);
                continue;
            }

            uint32_t i = static_cast<uint32_t>(e.idx);
            if (i >= static_cast<uint32_t>(m_captures.size())) continue;

            QuadDrawCall draw;
            float alpha = m_scene.GetSlot(i).alpha;
            if (alpha < 0.001f) continue;

            ID3D11ShaderResourceView* srv = m_captures[i] ? m_captures[i]->GetCurrentFrame() : nullptr;
            m_scene.GetDrawCall(i, cascadeAspect, draw.mvp, alpha);
            DirectX::XMMATRIX perspMVP =
                DirectX::XMLoadFloat4x4(&draw.mvp) * monRemap;
            DirectX::XMStoreFloat4x4(&draw.mvp, perspMVP);
            if (i < m_windows.size()) {
                const RECT& morphRect =
                    ResolveMorphScreenRect(m_entryExitAnimator, m_windows, i);
                DirectX::XMMATRIX screenMVP =
                    ComputeScreenSpaceMVP(morphRect, vpW, vpH);
                DirectX::XMStoreFloat4x4(&draw.mvp,
                    LerpMatrix(screenMVP, perspMVP,
                               m_entryExitAnimator.GetMorphBlend()));
            }
            draw.alpha = alpha;
            draw.blurAmount = 0.0f;
            if (i < m_windows.size() && m_windows[i].hwnd == m_desktopHwnd)
                ApplyTextureUV(draw, m_desktopTileUV);
            if (srv)
                m_quad.Draw(ctx, srv, draw);
            else
                m_quad.DrawPlaceholder(ctx, draw);
        }

        m_renderer.EndFrame();  // Present(0) into composition swap chain
    }

    // 12. NOW show the overlay — first visible frame already has content.
    m_renderer.Show();

    // 13. Hide desktop icons and real taskbar.
    DwmFlush();
    HideDesktopIcons();
    HideRealTaskbar();

#ifdef CKFLIP_DEBUG_TASKBAR
    // Patch E — post-hide dump.  Give DWM a frame to refresh the capture
    // so the dump reflects the source state after HideRealTaskbar().
    if (m_taskbarCapture) {
        DwmFlush();
        m_taskbarCapture->GetCurrentFrame();
        DumpTaskbarDebug(m_taskbarCapture.get(), m_taskbarRect,
                         m_taskbarContentResolved, m_taskbarContentCenterY,
                         L"ckflip_taskbar_posthide");
    }
    for (size_t i = 0; i < m_secondaryTrays.size(); ++i) {
        if (m_secondaryTrays[i].capture) {
            m_secondaryTrays[i].capture->GetCurrentFrame();
            wchar_t suffix[64] = {};
            swprintf_s(suffix, L"ckflip_taskbar_posthide_sec%zu", i);
            DumpTaskbarDebug(m_secondaryTrays[i].capture.get(),
                             m_secondaryTrays[i].rectOverlay,
                             m_secondaryTrays[i].contentResolved,
                             m_secondaryTrays[i].contentCenterY,
                             suffix);
        }
    }
#endif

    // 14. Cloak ALL visible app windows.
    {
        WindowCloaker::UncloakAll();   // clear stale leftovers
        std::vector<HWND> exclude;
        exclude.push_back(m_renderer.GetHwnd());   // our overlay
        if (m_desktopHwnd)
            exclude.push_back(m_desktopHwnd);       // desktop wallpaper
        WindowCloaker::CloakVisibleAppWindows(GetCurrentProcessId(), exclude);
    }
}

void FlipController::Cycle()
{
    if (!m_active || m_windows.size() < 2)
        return;

    // No cycling while the entry/exit morph is in flight — the morph owns
    // the scene's tilt + slot state for that interval.
    if (m_entryExitAnimator.IsActive())
        return;

    // Close-transition cooldown: the stack is already reflowing to its
    // smaller layout — a cycle on top would fight for the same slot state
    // (and would rotate arrays the close animator's mapping depends on).
    // Dropped, not queued: the transition lasts one cycle length (220 ms).
    if (m_closeAnim.IsActive())
        return;

    if (m_cycleAnim.IsActive()) {
        // Queue instead of blocking — creates smooth continuous motion.
        if (m_cycleQueue.size() < kMaxQueueSize)
            m_cycleQueue.push_back(true);
        return;
    }
    ExecuteCycleForward();
}

void FlipController::CycleBack()
{
    if (!m_active || m_windows.size() < 2)
        return;

    if (m_entryExitAnimator.IsActive())
        return;

    // Close-transition cooldown — see Cycle().
    if (m_closeAnim.IsActive())
        return;

    if (m_cycleAnim.IsActive()) {
        if (m_cycleQueue.size() < kMaxQueueSize)
            m_cycleQueue.push_back(false);
        return;
    }
    ExecuteCycleBackward();
}

void FlipController::ExecuteCycleForward()
{
    ExecuteCycleForward(false);
}

void FlipController::ExecuteCycleForward(bool chained)
{
    // Cycle animation disabled (master toggle or per-animation selection):
    // just rotate and rebuild scene — no animation.
    if (!AnimCycleEnabled()) {
        std::rotate(m_windows.begin(),  m_windows.begin()  + 1, m_windows.end());
        std::rotate(m_captures.begin(), m_captures.begin() + 1, m_captures.end());
        // CRITICAL: Rotate scene slots to keep internal state in sync with arrays.
        // Without this, the next animated cycle will have stale slot positions.
        m_scene.RotateAspects(true);
        RebuildSceneAspects();
        return;
    }

    m_cycleAnim.Begin(m_scene, true, chained);

    // Freeze SRVs BEFORE rotation — these are the "start" textures.
    // The wrapping tile (slot n-1) in phase 1 needs the departing window's
    // texture, which is at index 0 BEFORE rotation.
    m_frozenStartSRVs.resize(m_captures.size());
    for (size_t i = 0; i < m_captures.size(); ++i)
        m_frozenStartSRVs[i] = m_captures[i] ? SrvRef(m_captures[i]->GetCurrentFrame()) : nullptr;

    std::rotate(m_windows.begin(),  m_windows.begin()  + 1, m_windows.end());
    std::rotate(m_captures.begin(), m_captures.begin() + 1, m_captures.end());

    // Freeze SRVs AFTER rotation — these are the "target" textures.
    // Non-wrapping tiles and phase 2 of the wrapping tile use these.
    m_frozenTargetSRVs.resize(m_captures.size());
    for (size_t i = 0; i < m_captures.size(); ++i)
        m_frozenTargetSRVs[i] = m_captures[i] ? SrvRef(m_captures[i]->GetCurrentFrame()) : nullptr;

    // Freeze background layers.
    m_frozenDesktopSRV = nullptr;
    for (size_t i = 0; i < m_windows.size(); ++i) {
        if (m_windows[i].hwnd == m_desktopHwnd && i < m_captures.size() && m_captures[i]) {
            m_frozenDesktopSRV = SrvRef(m_captures[i]->GetCurrentFrame());
            break;
        }
    }
    if (!m_frozenTaskbarSRV && m_taskbarCapture)
        m_frozenTaskbarSRV = SrvRef(m_taskbarCapture->GetCurrentFrame());
    m_sessionFrozen = true;

    // Rotate cached per-window aspects/scales to match the window rotation.
    // Then immediately rebuild them from actual window rects to ensure they're
    // tied to the correct windows, not slot positions. This prevents aspect
    // ratio leakage when windows wrap around visible slots.
    m_scene.RotateAspects(true);
    RebuildSceneAspects();
    m_cycleAnim.SetTarget(m_scene);
}

void FlipController::ExecuteCycleBackward()
{
    ExecuteCycleBackward(false);
}

void FlipController::ExecuteCycleBackward(bool chained)
{
    // Cycle animation disabled (master toggle or per-animation selection):
    // just rotate and rebuild scene — no animation.
    if (!AnimCycleEnabled()) {
        std::rotate(m_windows.rbegin(),  m_windows.rbegin()  + 1, m_windows.rend());
        std::rotate(m_captures.rbegin(), m_captures.rbegin() + 1, m_captures.rend());
        // CRITICAL: Rotate scene slots to keep internal state in sync with arrays.
        // Without this, the next animated cycle will have stale slot positions.
        m_scene.RotateAspects(false);
        RebuildSceneAspects();
        return;
    }

    m_cycleAnim.Begin(m_scene, false, chained);

    // Freeze SRVs BEFORE rotation — the wrapping tile (slot 0) in phase 1
    // needs the departing window's texture at index n-1 BEFORE rotation.
    m_frozenStartSRVs.resize(m_captures.size());
    for (size_t i = 0; i < m_captures.size(); ++i)
        m_frozenStartSRVs[i] = m_captures[i] ? SrvRef(m_captures[i]->GetCurrentFrame()) : nullptr;

    std::rotate(m_windows.rbegin(),  m_windows.rbegin()  + 1, m_windows.rend());
    std::rotate(m_captures.rbegin(), m_captures.rbegin() + 1, m_captures.rend());

    // Freeze SRVs AFTER rotation.
    m_frozenTargetSRVs.resize(m_captures.size());
    for (size_t i = 0; i < m_captures.size(); ++i)
        m_frozenTargetSRVs[i] = m_captures[i] ? SrvRef(m_captures[i]->GetCurrentFrame()) : nullptr;

    // Freeze background layers — same as forward path.  Without these, the
    // wrap-slot texture logic below falls back to live captures (m_captures[
    // idx]->GetCurrentFrame()) and the wrapping tile's texture can drift
    // mid-animation as WGC delivers new frames or the array indexing is
    // re-resolved between phases.  This is what produces the user-visible
    // "desktop's texture replaced by Explorer's mid-transition" symptom on
    // backward cycling but not forward.
    m_frozenDesktopSRV = nullptr;
    for (size_t i = 0; i < m_windows.size(); ++i) {
        if (m_windows[i].hwnd == m_desktopHwnd && i < m_captures.size() && m_captures[i]) {
            m_frozenDesktopSRV = SrvRef(m_captures[i]->GetCurrentFrame());
            break;
        }
    }
    if (!m_frozenTaskbarSRV && m_taskbarCapture)
        m_frozenTaskbarSRV = SrvRef(m_taskbarCapture->GetCurrentFrame());
    m_sessionFrozen = true;

    // Rotate cached per-window aspects/scales to match the window rotation.
    // Then immediately rebuild them from actual window rects to ensure they're
    // tied to the correct windows, not slot positions. This prevents aspect
    // ratio leakage when windows wrap around visible slots.
    m_scene.RotateAspects(false);
    RebuildSceneAspects();
    m_cycleAnim.SetTarget(m_scene);
}

void FlipController::ProcessCycleQueue()
{
    if (m_cycleQueue.empty())
        return;

    bool ready = false;

    // Normal completion
    if (m_cycleAnim.JustFinished()) {
        ready = true;
    }
    // Early blend: near end of a NON-chained animation with queued input.
    // Non-chained (220ms OutCubic) can be cut at 0.75 for the responsive
    // first-press → chain transition.
    // Chained animations (170ms Linear) MUST finish fully: Linear has
    // constant velocity, so cutting early would create a position jump.
    // JustFinished + Tick in the same RenderFrame ensures zero-gap chaining.
    else if (m_cycleAnim.IsActive() && !m_cycleAnim.IsChained()
             && m_cycleAnim.GetRawT() > 0.75f) {
        ready = true;
    }

    if (!ready)
        return;

    bool forward = m_cycleQueue.front();
    m_cycleQueue.pop_front();

    // Chained (Linear momentum) only when there are MORE queued cycles
    // remaining — indicating a held key with autorepeat buffered.
    // Single taps that queue one item get snappy OutCubic easing.
    bool useChain = !m_cycleQueue.empty();

    if (forward)
        ExecuteCycleForward(useChain);
    else
        ExecuteCycleBackward(useChain);

    // Tick the new animation once so the first frame shows movement.
    m_cycleAnim.Tick(m_scene);
}

void FlipController::CycleStop()
{
    // Tab released — clear any pending cycles so the current animation
    // finishes on the next window without drifting further.
    m_cycleQueue.clear();

    // Switch current animation from Linear (held-key momentum) to OutCubic
    // (deceleration) so it smoothly stops on the next window.
    m_cycleAnim.SwitchToDecel();
}

// ---------------------------------------------------------------------------
// Dismiss — public entry: begin the exit morph, defer teardown.
// FinishDismiss() runs the actual window-switching logic after the morph
// completes (signalled by m_entryExitAnimator.JustFinishedExit()).
// ---------------------------------------------------------------------------
void FlipController::Dismiss()
{
    if (!m_active)
        return;

    // If the entry morph is still running, defer reverse-in-place by
    // kReverseDelayMs.  Releasing the trigger key very early in the entry
    // (e.g. ≤2 frames in) would otherwise immediately mirror the morph and
    // produce a visible flicker — the user's eye still parses the entry
    // when the geometry suddenly starts reversing.  The delay lets the
    // entry play out a few more frames of forward motion so the reversal
    // feels like a deceleration, not a snap.  Already-reversing morph is
    // left alone; an in-flight delayed reverse is not retriggered.
    if (m_entryExitAnimator.IsActive()) {
        if (m_entryExitAnimator.IsReverse())
            return;
        if (m_reverseDelayPending)
            return;
        m_reverseDelayPending    = true;
        m_reverseDelayFromEscape = false;
        QueryPerformanceCounter(&m_reverseDelayStartQPC);
        m_cycleQueue.clear();
        return;
    }

    // Stop any in-flight cycle so the cascade snapshot for the morph is stable.
    m_cycleQueue.clear();
    // Cycle stays active if it was running; the exit morph's Tick runs
    // after cycle's Tick in RenderFrame() and overwrites slot state, so
    // the exit visibly takes over within one frame.

    // A close transition in flight: snap it to its end state so BeginExit
    // below snapshots a clean, fully-settled cascade — two animators must
    // never own the slots across the same exit.  Unlike the cycle above,
    // the close anim also draws freestanding dying tiles that the exit
    // morph would NOT overwrite, so it cannot simply be left running.
    // The dying windows are already gone from the OS — dropping their
    // tiles instantly is the correct end state.
    if (m_closeAnim.IsActive()) {
        m_closeAnim.FinishImmediate(m_scene);
        ClearClosingCaptures();
    }

    // Compute the same params Activate() used.
    RECT rcVp;
    GetClientRect(m_renderer.GetHwnd(), &rcVp);
    float vpW = static_cast<float>(rcVp.right - rcVp.left);
    float vpH = static_cast<float>(rcVp.bottom - rcVp.top);
    if (vpW <= 0) vpW = 1920.0f;
    if (vpH <= 0) vpH = 1080.0f;

    const LONG primaryW = m_monLayout.primary.right - m_monLayout.primary.left;
    const LONG primaryH = m_monLayout.primary.bottom - m_monLayout.primary.top;
    float dW = primaryW > 0 ? static_cast<float>(primaryW) : vpW;
    float dH = primaryH > 0 ? static_cast<float>(primaryH) : vpH;

    // Endpoint Z ranks for v8.2 Bug 8''.
    //   - Sized to visibleN (matches m_flatSlots.size() in BeginExit, NOT
    //     m_windows.size()).
    //   - Selected window (index 0) goes to rank 0 (top of flat endpoint).
    //   - Remaining visible slots sorted by raw original Z-order, then
    //     dense-ranked 0..visibleN-1.
    uint32_t visibleN = std::min<uint32_t>(
        m_scene.SlotCount(),
        static_cast<uint32_t>(m_windows.size()));

    std::vector<uint32_t> zRanks(visibleN, 0);

    if (visibleN > 0) {
        zRanks[0] = 0;   // selected after Dismiss is top

        auto rawRankOf = [&](size_t i) -> uint32_t {
            HWND h = m_windows[i].hwnd;
            auto it = m_originalZOrder.find(h);
            if (it != m_originalZOrder.end()) return it->second;
            // Desktop pseudo-window and any late-injected HWNDs fall here.
            // Push them to the very back of the endpoint while preserving
            // m_windows-order as the tiebreaker.
            // Parenthesised to defeat the Windows.h max() macro.
            return (std::numeric_limits<uint32_t>::max)() / 2u
                 + static_cast<uint32_t>(i);
        };

        auto endpointTier = [&](size_t i) -> int {
            HWND h = m_windows[i].hwnd;
            if (h == m_desktopHwnd) return 2;
            if (h && IsIconic(h))   return 1;
            return 0;
        };

        std::vector<size_t> rest;
        rest.reserve(visibleN > 0 ? visibleN - 1 : 0);
        for (size_t i = 1; i < visibleN; ++i)
            rest.push_back(i);

        std::stable_sort(rest.begin(), rest.end(),
            [&](size_t a, size_t b) {
                int ta = endpointTier(a);
                int tb = endpointTier(b);
                if (ta != tb) return ta < tb;
                return rawRankOf(a) < rawRankOf(b);
            });

        uint32_t dense = 1;
        for (size_t i : rest)
            zRanks[i] = dense++;
    }

    bool desktopSelected = (!m_windows.empty()
                         && m_windows[0].hwnd == m_desktopHwnd);

    // Per-tile fade-out: tiles whose corresponding window won't be visible
    // after the overlay hides decay to α=0 across the reverse morph.  This
    // prevents the "non-selected app's last frame leaks behind the picked
    // window" artefact and replaces the old desktop-selected special case.
    std::vector<bool> fadeOutFlags(m_windows.size(), false);
    for (size_t i = 0; i < m_windows.size(); ++i) {
        if (i == 0) continue;                          // selected stays visible
        HWND h = m_windows[i].hwnd;
        if (h == m_desktopHwnd) {
            // Desktop tile is only visible post-exit when it's the pick.
            fadeOutFlags[i] = !desktopSelected;
            continue;
        }
        if (desktopSelected) {
            // User picked desktop → all non-desktop tiles disappear.
            fadeOutFlags[i] = true;
            continue;
        }
        if (h && IsIconic(h)) {
            // Non-selected minimized window stays minimized → fade out.
            fadeOutFlags[i] = true;
            continue;
        }
        // Visible app, not selected → still visible behind foreground; keep.
    }

    HWND target = m_windows.empty() ? nullptr : m_windows[0].hwnd;
    const bool selectedMinimized =
        !desktopSelected && target && IsWindow(target) && IsIconic(target);
    m_exitSelectedStableSRV = nullptr;
    m_exitSelectedStableTexture = nullptr;
    m_exitSelectedStableHwnd = nullptr;
    if (selectedMinimized && !m_captures.empty() && m_captures[0]) {
        ID3D11ShaderResourceView* srcSRV = m_captures[0]->GetCurrentFrame();
        ID3D11Device* device = m_renderer.GetDevice();
        ID3D11DeviceContext* ctx = m_renderer.GetContext();
        if (srcSRV && device && ctx) {
            ID3D11Resource* srcRes = nullptr;
            srcSRV->GetResource(&srcRes);
            ID3D11Texture2D* srcTex = nullptr;
            if (srcRes) {
                srcRes->QueryInterface(__uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(&srcTex));
                srcRes->Release();
            }
            if (srcTex) {
                D3D11_TEXTURE2D_DESC desc{};
                srcTex->GetDesc(&desc);
                D3D11_TEXTURE2D_DESC copyDesc = desc;
                copyDesc.Usage = D3D11_USAGE_DEFAULT;
                copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                copyDesc.CPUAccessFlags = 0;
                copyDesc.MiscFlags = 0;

                winrt::com_ptr<ID3D11Texture2D> copyTex;
                if (SUCCEEDED(device->CreateTexture2D(&copyDesc, nullptr,
                                                      copyTex.put()))) {
                    ctx->CopyResource(copyTex.get(), srcTex);

                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srcSRV->GetDesc(&srvDesc);
                    winrt::com_ptr<ID3D11ShaderResourceView> copySRV;
                    if (SUCCEEDED(device->CreateShaderResourceView(
                            copyTex.get(), &srvDesc, copySRV.put()))) {
                        m_exitSelectedStableTexture = std::move(copyTex);
                        m_exitSelectedStableSRV = std::move(copySRV);
                        m_exitSelectedStableHwnd = target;
                    }
                }
                srcTex->Release();
            }
        }
    }

    m_entryExitAnimator.BeginExit(m_scene, m_windows, zRanks,
                                  vpW, vpH, dW, dH, m_desktopHwnd,
                                  m_cascadeAspect,
                                  m_overlayOriginX, m_overlayOriginY,
                                  DirectX::XMLoadFloat4x4(&m_monRemapNDC),
                                  fadeOutFlags,
                                  /*animateOverflow=*/!desktopSelected);
    m_exitPending    = true;
    m_exitFromEscape = false;

    // Entry/exit animation off: snap the exit too — Finalize raises the
    // JustFinishedExit edge, so the render loop runs FinishDismiss on the
    // very next frame.
    if (!AnimEntryExitEnabled())
        m_entryExitAnimator.Finalize(m_scene);

    // Fire the OS-side actions immediately so DWM's restore/minimize
    // transition happens behind the exit morph.  If the selected window was
    // minimized, RenderFrame uses an owned copy of that tile's current SRV
    // through the exit morph while the real window is restoring behind the
    // overlay; geometry keeps animating normally, but live WGC transient
    // restore surfaces are not sampled into the tile or cached capture.
    WindowCloaker::UncloakAll();
    DwmFlush();

    if (desktopSelected) {
        // Toggle show-desktop only when we weren't already on the desktop —
        // otherwise it minimises nothing useful and momentarily hides the
        // wallpaper-drag icons.
        if (!m_activatedOnDesktop) {
            // Prefer IShellDispatch::ToggleDesktop — the SAME shell entry
            // point Win+D uses, so it minimises every window-style the user
            // would expect: standard frames, MDI-children, AND bare WS_POPUP
            // top-levels (e.g. fullscreen color displays / bespoke debug
            // tools).  The legacy `SendMessageW(Shell_TrayWnd, WM_COMMAND,
            // 419)` was unreliable for WS_POPUP-only windows because the
            // taskbar's Show-Desktop handler enumerates a narrower set of
            // top-levels than the Win+D hotkey's runtime path.
            //
            // Falls back to the legacy command if COM activation fails
            // (e.g. shell isn't fully loaded yet).
            bool toggled = false;
            HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            {
                IShellDispatch4* pShell = nullptr;
                HRESULT hr = CoCreateInstance(CLSID_Shell, nullptr,
                                               CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(&pShell));
                if (SUCCEEDED(hr) && pShell) {
                    if (SUCCEEDED(pShell->ToggleDesktop()))
                        toggled = true;
                    pShell->Release();
                }
            }
            if (coHr == S_OK)
                CoUninitialize();   // only undo our own successful init
            if (!toggled) {
                HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
                if (tray) SendMessageW(tray, WM_COMMAND, 419, 0);
            }
        }
    } else if (target && IsWindow(target)) {
        if (selectedMinimized)
            ShowWindow(target, SW_RESTORE);
        SetForegroundWindow(target);
    }
}

// ---------------------------------------------------------------------------
// Escape — public entry: same morph as Dismiss; only post-morph teardown
// differs (no foreground/show-desktop call in FinishEscape).
// ---------------------------------------------------------------------------
void FlipController::Escape()
{
    if (!m_active)
        return;

    // Mid-entry ESC also defers reverse-in-place (see Dismiss for the
    // delay rationale).
    if (m_entryExitAnimator.IsActive()) {
        if (m_entryExitAnimator.IsReverse())
            return;
        if (m_reverseDelayPending) {
            // Upgrade an in-flight Dismiss-deferred reverse to Escape — the
            // delay timer already started, only the post-morph teardown
            // path changes.
            m_reverseDelayFromEscape = true;
            return;
        }
        m_reverseDelayPending    = true;
        m_reverseDelayFromEscape = true;
        QueryPerformanceCounter(&m_reverseDelayStartQPC);
        m_cycleQueue.clear();
        return;
    }

    m_cycleQueue.clear();
    // Cycle stays active if it was running; the exit morph's Tick runs
    // after cycle's Tick in RenderFrame() and overwrites slot state, so
    // the exit visibly takes over within one frame.

    // Snap any in-flight close transition — see Dismiss() for rationale.
    if (m_closeAnim.IsActive()) {
        m_closeAnim.FinishImmediate(m_scene);
        ClearClosingCaptures();
    }

    RECT rcVp;
    GetClientRect(m_renderer.GetHwnd(), &rcVp);
    float vpW = static_cast<float>(rcVp.right - rcVp.left);
    float vpH = static_cast<float>(rcVp.bottom - rcVp.top);
    if (vpW <= 0) vpW = 1920.0f;
    if (vpH <= 0) vpH = 1080.0f;

    const LONG primaryW = m_monLayout.primary.right - m_monLayout.primary.left;
    const LONG primaryH = m_monLayout.primary.bottom - m_monLayout.primary.top;
    float dW = primaryW > 0 ? static_cast<float>(primaryW) : vpW;
    float dH = primaryH > 0 ? static_cast<float>(primaryH) : vpH;

    // Bug 8'' §8.7 Option A — Escape passes an empty zRanks vector so the
    // endpoint-Z override is skipped, preserving legacy escape behaviour.
    std::vector<uint32_t> escZRanks;  // empty → override skipped

    // Escape is a pure cancel — every tile reverses cleanly to its entry
    // flat position; no per-tile fade-out (the empty vector signals that).
    std::vector<bool> escFadeOut;  // empty
    m_entryExitAnimator.BeginExit(m_scene, m_windows, escZRanks,
                                  vpW, vpH, dW, dH, m_desktopHwnd,
                                  m_cascadeAspect,
                                  m_overlayOriginX, m_overlayOriginY,
                                  DirectX::XMLoadFloat4x4(&m_monRemapNDC),
                                  escFadeOut,
                                  /*animateOverflow=*/true);
    m_exitPending    = true;
    m_exitFromEscape = true;

    // Entry/exit animation off: snap the escape exit as well.
    if (!AnimEntryExitEnabled())
        m_entryExitAnimator.Finalize(m_scene);
}

// ---------------------------------------------------------------------------
// FinishDismiss — original Dismiss() body, now called by the render loop
// after the exit morph completes.  Brings the selected window forward (or
// triggers the Show-Desktop toggle).
// ---------------------------------------------------------------------------
void FlipController::FinishDismiss()
{
    m_active = false;
    m_cycleQueue.clear();
    m_reverseDelayPending = false;

    // Cancel any cycle-anim state that may have been left in-flight when
    // Dismiss() fired (Dismiss intentionally does NOT stop the cycle so the
    // exit morph can overwrite the cascade smoothly — see Dismiss()).  But
    // CycleAnimator never auto-clears m_active on session end, so a half-
    // finished cycle can leak into the NEXT activation: the next session's
    // RenderFrame calls m_cycleAnim.Tick(scene) before m_entryExitAnimator.
    // Tick(scene), and on the first tick the entry animator returns early
    // (re-anchor) — the scene therefore gets the stale cycle anim's slot
    // writes for that frame instead of the flat-state from BeginEntry,
    // producing the every-other-launch entry "flash" the user reports.
    m_cycleAnim.Cancel();
    m_closeAnim.Cancel();       // same stale-state rationale
    ClearClosingCaptures();

    // Dismiss() fires normal OS-side actions early.  A mid-entry Dismiss()
    // can still reach this path through ReverseInPlace without running
    // Dismiss()'s action block, so keep this idempotent safety pass.
    WindowCloaker::UncloakAll();
    if (!m_windows.empty() && m_windows[0].hwnd != m_desktopHwnd) {
        HWND target = m_windows[0].hwnd;
        if (target && IsWindow(target)) {
            if (IsIconic(target))
                ShowWindow(target, SW_RESTORE);
            SetForegroundWindow(target);
        }
    }
    DwmFlush();

    StopCaptures();
    if (m_taskbarCapture) { m_taskbarCapture->Stop(); m_taskbarCapture.reset(); }
    for (auto& tray : m_secondaryTrays) {
        if (tray.capture)
            tray.capture->Stop();
        tray.frozenSRV = nullptr;
    }
    m_frozenStartSRVs.clear();
    m_frozenTargetSRVs.clear();
    m_frozenDesktopSRV = nullptr;
    m_frozenTaskbarSRV = nullptr;
    m_exitSelectedStableSRV = nullptr;
    m_exitSelectedStableTexture = nullptr;
    m_exitSelectedStableHwnd = nullptr;
    m_sessionFrozen = false;

    m_renderer.Hide();
    RestoreDesktopIcons();
    ShowRealTaskbar();
    m_secondaryTrays.clear();

    m_windows.clear();
    m_desktopHwnd = nullptr;
    m_activatedOnDesktop = false;
    m_taskbarDrawOnTop = false;
    m_taskbarLocator.Shutdown();
    m_entryExitAnimator.ClearEntryFlatCache();
    m_originalZOrder.clear();   // Bug 8'' — session-end cleanup
#ifdef CKFLIP_DEBUG_TASKBAR
    g_taskbarFreezeSRV = nullptr;
    g_taskbarDebugMode = TaskbarDebugMode::Normal;
#endif
}

// ---------------------------------------------------------------------------
// FinishEscape — original Escape() body, called after the exit morph
// completes.  No foreground/show-desktop — Escape is a pure cancel.
// ---------------------------------------------------------------------------
void FlipController::FinishEscape()
{
    m_active = false;
    m_cycleQueue.clear();
    m_reverseDelayPending = false;
    // Cancel any in-flight cycle (see FinishDismiss for rationale).
    m_cycleAnim.Cancel();
    m_closeAnim.Cancel();
    ClearClosingCaptures();

    WindowCloaker::UncloakAll();
    DwmFlush();

    StopCaptures();
    if (m_taskbarCapture) { m_taskbarCapture->Stop(); m_taskbarCapture.reset(); }
    for (auto& tray : m_secondaryTrays) {
        if (tray.capture)
            tray.capture->Stop();
        tray.frozenSRV = nullptr;
    }
    m_frozenStartSRVs.clear();
    m_frozenTargetSRVs.clear();
    m_frozenDesktopSRV = nullptr;
    m_frozenTaskbarSRV = nullptr;
    m_exitSelectedStableSRV = nullptr;
    m_exitSelectedStableTexture = nullptr;
    m_exitSelectedStableHwnd = nullptr;
    m_sessionFrozen = false;
    m_renderer.Hide();

    RestoreDesktopIcons();
    ShowRealTaskbar();
    m_secondaryTrays.clear();

    m_windows.clear();
    m_desktopHwnd = nullptr;
    m_activatedOnDesktop = false;
    m_taskbarDrawOnTop = false;
    m_taskbarLocator.Shutdown();
    m_entryExitAnimator.ClearEntryFlatCache();
    m_originalZOrder.clear();   // Bug 8'' — session-end cleanup
#ifdef CKFLIP_DEBUG_TASKBAR
    g_taskbarFreezeSRV = nullptr;
    g_taskbarDebugMode = TaskbarDebugMode::Normal;
#endif
}

// ---------------------------------------------------------------------------
void FlipController::StartCaptures()
{
    m_captures.clear();
    m_captures.resize(m_windows.size());
    auto* device = m_renderer.GetDevice();

    // NOTE: live-preview-off does NOT change anything here.  Sessions always
    // start normally (identical activation latency to live mode); RenderFrame
    // freezes each capture at its first delivered frame instead.  An earlier
    // one-shot DwmThumbnail variant cost ~60-170 ms per window, which pushed
    // the keypress-anchored dim past its 100 ms ramp (instant full dim) and
    // flashed the busy cursor once per window.

    for (size_t i = 0; i < m_windows.size(); ++i) {
        HWND hwnd = m_windows[i].hwnd;

        // Reuse cached capture if available — cached frame shows immediately
        // while the WGC session restarts in the background.
        auto it = m_captureCache.find(hwnd);
        if (it != m_captureCache.end()) {
            m_captures[i] = std::move(it->second);
            m_captureCache.erase(it);
            // Restart WGC session (Stop preserved the cached frame).
            m_captures[i]->StartForWindow(hwnd, device);
        } else {
            m_captures[i] = std::make_unique<WGCCapture>();
            m_captures[i]->StartForWindow(hwnd, device);
        }
    }

    // Discard any leftover cache entries for windows that no longer exist.
    m_captureCache.clear();
}

void FlipController::StopCaptures()
{
    // Move captures to cache — Stop() preserves the cached frame so
    // re-activation can show content immediately while WGC restarts.
    for (size_t i = 0; i < m_captures.size(); ++i) {
        if (m_captures[i]) {
            HWND h = m_captures[i]->GetHwnd();
            m_captures[i]->Stop();
            if (h)
                m_captureCache[h] = std::move(m_captures[i]);
        }
    }
    m_captures.clear();
}

// ---------------------------------------------------------------------------
void FlipController::RebuildSceneAspects()
{
    // Desktop-relative sizing is intentionally based on the primary monitor,
    // matching the cascade host and the desktop pseudo-window rect.
    const LONG primaryW = m_monLayout.primary.right - m_monLayout.primary.left;
    const LONG primaryH = m_monLayout.primary.bottom - m_monLayout.primary.top;
    float dW = primaryW > 0 ? static_cast<float>(primaryW) : m_cascadeW;
    float dH = primaryH > 0 ? static_cast<float>(primaryH) : m_cascadeH;

    // Only set aspects for visible slots (scene may have fewer than m_windows)
    uint32_t slotCount = m_scene.SlotCount();
    for (uint32_t i = 0; i < slotCount && i < static_cast<uint32_t>(m_windows.size()); ++i) {
        const auto& rc = m_windows[i].rect;
        float w = static_cast<float>(rc.right - rc.left);
        float h = static_cast<float>(rc.bottom - rc.top);
        if (w > 1.0f && h > 1.0f) {
            m_scene.SetSlotAspect(i, w / h);
            m_scene.SetSlotScale(i, w, h, dW, dH);
        } else {
            // Fallback: 16:9 for windows with unknown/zero dimensions
            // (e.g. minimized windows that weren't restored yet).
            m_scene.SetSlotAspect(i, 1.77f);
        }
    }
}

// ---------------------------------------------------------------------------
void FlipController::InjectDesktopWindow()
{
    // Find the desktop background window (Progman or WorkerW with SHELLDLL_DefView)
    m_desktopHwnd = WindowScanner::FindDesktopWindow();
    if (!m_desktopHwnd)
        return;

    // Build a WindowInfo for the desktop — sized to the full primary monitor
    WindowInfo desktop;
    desktop.hwnd      = m_desktopHwnd;
    desktop.title     = L"Desktop";
    desktop.rect      = m_monLayout.primary;
    desktop.minimized = false;

    // Append at the end — in Flip3D, desktop is always the LAST in the stack.
    m_windows.push_back(desktop);
}

// ---------------------------------------------------------------------------
void FlipController::UpdateDesktopCaptureGeometry()
{
    m_desktopBackdropRect = m_monLayout.virtualScreen;
    m_desktopTileUV = DirectX::XMFLOAT4{0.0f, 0.0f, 1.0f, 1.0f};

    RECT prog{};
    bool haveProg = m_desktopHwnd
                 && GetWindowRect(m_desktopHwnd, &prog)
                 && ValidRect(prog);
    bool uvFallback = true;
    if (haveProg) {
        m_desktopBackdropRect = prog;

        const RECT& primary = m_monLayout.primary;
        const bool coversPrimary =
            prog.left <= primary.left && prog.top <= primary.top
            && prog.right >= primary.right && prog.bottom >= primary.bottom;
        const float progW = static_cast<float>(prog.right - prog.left);
        const float progH = static_cast<float>(prog.bottom - prog.top);
        if (coversPrimary && progW > 0.0f && progH > 0.0f) {
            float u0 = static_cast<float>(primary.left - prog.left) / progW;
            float v0 = static_cast<float>(primary.top - prog.top) / progH;
            float u1 = static_cast<float>(primary.right - prog.left) / progW;
            float v1 = static_cast<float>(primary.bottom - prog.top) / progH;
            u0 = std::clamp(u0, 0.0f, 1.0f);
            v0 = std::clamp(v0, 0.0f, 1.0f);
            u1 = std::clamp(u1, 0.0f, 1.0f);
            v1 = std::clamp(v1, 0.0f, 1.0f);
            if (u1 > u0 && v1 > v0) {
                m_desktopTileUV = DirectX::XMFLOAT4{u0, v0, u1, v1};
                uvFallback = false;
            }
        }
    }

    WCHAR buf[320];
    swprintf_s(buf,
        L"CKFlip DESKTOP: prog=(%ld,%ld)-(%ld,%ld) primary=(%ld,%ld)-(%ld,%ld) uv=(%.4f,%.4f)-(%.4f,%.4f) fallback=%d\n",
        haveProg ? prog.left : 0,
        haveProg ? prog.top : 0,
        haveProg ? prog.right : 0,
        haveProg ? prog.bottom : 0,
        m_monLayout.primary.left, m_monLayout.primary.top,
        m_monLayout.primary.right, m_monLayout.primary.bottom,
        m_desktopTileUV.x, m_desktopTileUV.y,
        m_desktopTileUV.z, m_desktopTileUV.w,
        uvFallback ? 1 : 0);
    CKLog::Log(buf);
}

// ---------------------------------------------------------------------------
void FlipController::DeduplicateWindows()
{
    // Remove our overlay HWND and any desktop background windows that the
    // scanner might have picked up (we inject a proper desktop entry later).
    HWND overlay   = m_renderer.GetHwnd();
    HWND desktopBg = WindowScanner::FindDesktopWindow();
    m_windows.erase(
        std::remove_if(m_windows.begin(), m_windows.end(),
                        [overlay, desktopBg](const WindowInfo& wi) {
                            return wi.hwnd == overlay || wi.hwnd == desktopBg;
                        }),
        m_windows.end());
}

// ---------------------------------------------------------------------------
// Sort windows by their actual top-level Z-order (front → back), with the
// desktop pseudo-window pinned to the very end.
//
// WindowScanner builds m_windows by walking EnumWindows, which already
// returns top-to-bottom Z-order on Windows.  But we re-validate here using
// GetWindow(GW_HWNDPREV) walking from the topmost window so the order is
// stable even if the scanner did extra filtering / reordering elsewhere.
//
// Why pure Z-order: the previous PID-grouping + pixel-area sort caused
// "tile leak" artefacts on dismiss — when the visual back-to-front order
// of cascade tiles disagreed with the OS Z-order, the freshly-foregrounded
// pick momentarily showed back tiles "leaking through" until DWM caught
// up.  Honouring the OS Z-order makes our cascade match what the user
// will see immediately after the overlay hides.
// ---------------------------------------------------------------------------
void FlipController::SortWindowsByProgram()
{
    // Bug 8'' (v8.2.1 §3) — m_originalZOrder is a session-start snapshot.
    // Clear it first so an early return leaves it empty, never stale from
    // a previous session.
    m_originalZOrder.clear();

    if (m_windows.size() < 2) return;

    // Walk top-level Z-order top → bottom.  Map each HWND to its rank.
    std::unordered_map<HWND, int> zRank;
    int rank = 0;
    HWND h = GetTopWindow(nullptr);
    while (h) {
        zRank.emplace(h, rank++);
        h = GetWindow(h, GW_HWNDNEXT);
    }

    // Two-tier sort:
    //   tier 0: visible non-desktop windows  (Z-order: most-recent fg first)
    //   tier 1: minimized non-desktop windows (Z-order)
    //   tier 2: desktop pseudo-window         (always last)
    //
    // The two-tier split fixes the "Firefox flashes in front of just-picked
    // window" leak: a minimized window that happens to be high in the OS
    // Z-order would otherwise land in front of visible apps in the cascade,
    // even though it's not on screen.  Pinning minimized windows behind all
    // visible ones (but before desktop) matches Win7's flip3d ordering and
    // makes the cascade match what the user sees post-exit.
    HWND desktopHwnd = m_desktopHwnd;
    auto tierOf = [desktopHwnd](const WindowInfo& w) -> int {
        if (w.hwnd == desktopHwnd) return 2;
        if (w.hwnd && IsIconic(w.hwnd)) return 1;
        return 0;
    };
    std::stable_sort(m_windows.begin(), m_windows.end(),
        [&zRank, &tierOf](const WindowInfo& a, const WindowInfo& b) {
            int ta = tierOf(a), tb = tierOf(b);
            if (ta != tb) return ta < tb;
            auto ia = zRank.find(a.hwnd);
            auto ib = zRank.find(b.hwnd);
            int ra = (ia != zRank.end()) ? ia->second : INT_MAX;
            int rb = (ib != zRank.end()) ? ib->second : INT_MAX;
            return ra < rb;
        });

    // Occlusion-aware cascade sort. Within tier-0, windows fully covered by
    // a DWM-closer tier-0 window are kept directly behind that coverer. Covered
    // fullscreen/maximized windows are placed before covered small windows, so
    // a hidden fullscreen background stays paired with the fullscreen window
    // hiding it instead of wrapping ahead when the user later cycles to a
    // smaller covered window.
    {
        auto isLargeWindow = [](const WindowInfo& w) -> bool {
            if (w.minimized || !w.hwnd)
                return false;

            const RECT& r = w.rect;
            if (r.right <= r.left || r.bottom <= r.top)
                return false;

            HMONITOR hm = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hm, &mi))
                return false;

            const int monitorW = mi.rcMonitor.right - mi.rcMonitor.left;
            const int monitorH = mi.rcMonitor.bottom - mi.rcMonitor.top;
            if (monitorW <= 0 || monitorH <= 0)
                return false;

            const int windowW = r.right - r.left;
            const int windowH = r.bottom - r.top;
            if (IsZoomed(w.hwnd) && windowW * 100 >= monitorW * 80)
                return true;

            return windowW * 100 >= monitorW * 90 &&
                   windowH * 100 >= monitorH * 90;
        };

        size_t tier0End = 0;
        for (size_t i = 0; i < m_windows.size(); ++i) {
            if (tierOf(m_windows[i]) == 0)
                tier0End = i + 1;
            else
                break;
        }

        if (tier0End >= 2) {
            std::vector<bool> occluded(tier0End, false);
            std::vector<size_t> coverer(tier0End, tier0End);
            for (size_t i = 0; i < tier0End; ++i) {
                const WindowInfo& wi = m_windows[i];
                if (wi.minimized || !wi.hwnd)
                    continue;

                auto rankIt = zRank.find(wi.hwnd);
                if (rankIt == zRank.end())
                    continue;
                const int rankI = rankIt->second;

                for (size_t j = 0; j < tier0End; ++j) {
                    if (i == j)
                        continue;

                    const WindowInfo& wj = m_windows[j];
                    if (wj.minimized || !wj.hwnd)
                        continue;

                    auto coverRankIt = zRank.find(wj.hwnd);
                    if (coverRankIt == zRank.end() || coverRankIt->second >= rankI)
                        continue;

                    const RECT& ri = wi.rect;
                    const RECT& rj = wj.rect;
                    if (rj.left  <= ri.left  && rj.top    <= ri.top &&
                        rj.right >= ri.right && rj.bottom >= ri.bottom) {
                        occluded[i] = true;
                        coverer[i] = j;
                        break;
                    }
                }
            }

            bool hasOcclusion = false;
            for (bool value : occluded) {
                if (value) {
                    hasOcclusion = true;
                    break;
                }
            }

            if (hasOcclusion) {
                std::vector<WindowInfo> reordered;
                reordered.reserve(tier0End);
                for (size_t i = 0; i < tier0End; ++i) {
                    if (occluded[i])
                        continue;

                    reordered.push_back(m_windows[i]);

                    std::vector<size_t> coveredChildren;
                    coveredChildren.reserve(tier0End);
                    for (size_t j = 0; j < tier0End; ++j)
                        if (occluded[j] && coverer[j] == i)
                            coveredChildren.push_back(j);

                    std::stable_sort(coveredChildren.begin(), coveredChildren.end(),
                        [&isLargeWindow, this](size_t a, size_t b) {
                            const bool largeA = isLargeWindow(m_windows[a]);
                            const bool largeB = isLargeWindow(m_windows[b]);
                            if (largeA != largeB)
                                return largeA;
                            return false;
                        });

                    for (size_t j : coveredChildren)
                        reordered.push_back(m_windows[j]);
                }

                if (reordered.size() == tier0End)
                    std::copy(reordered.begin(), reordered.end(), m_windows.begin());
            }
        }
    }

    // Bug 8'' — promote the raw OS Z-rank map to a session-start member
    // snapshot for the Dismiss endpoint-Z computation.  Populated only
    // here; never refreshed after Activate.
    m_originalZOrder.reserve(zRank.size());
    for (const auto& kv : zRank)
        m_originalZOrder.insert({ kv.first, static_cast<uint32_t>(kv.second) });
}

void FlipController::RenderFrame()
{
    // Keep held taskbars (autohide continuity / live preview) pinned in
    // place below the overlay — the shell keeps trying to retract or raise
    // them.
    PinHeldTaskbars();

    // Live monitoring: remove windows that were closed while active.
    RemoveClosedWindows();

    // Cloak any new windows that appeared since activation.
    CloakNewWindows();

    // Lazy EnsureFrame: only when NOT animating to avoid cursor lag.
    // PrintWindow is synchronous and heavy — never run it during animation.
    //
    // v8.5 — m_sessionFrozen only tracks the CYCLE animation; it is NOT set
    // during the entry/exit morph.  Without the IsActive() check the heavy
    // synchronous PrintWindow ran on entry/exit frames, stalling them by
    // tens of ms each — and because the morph is wall-clock driven, that
    // stall made rawT teleport deep into the morph between two rendered
    // frames (the perceived "snap into 3D on frame 2").  Skip EnsureFrame
    // for the whole entry/exit morph too; any tile lacking a frame simply
    // shows a placeholder for the ~266 ms morph and is filled afterwards.
    // The close transition is wall-clock driven the same way — a heavy
    // synchronous stall would make it teleport too, so it gates as well.
    if (!m_sessionFrozen && !m_entryExitAnimator.IsActive()
        && !m_closeAnim.IsActive()) {
        // Ensure taskbar has a frame.
        if (m_taskbarCapture && !m_taskbarCapture->HasCachedFrame()) {
            m_taskbarCapture->GetCurrentFrame();
            if (!m_taskbarCapture->HasCachedFrame())
                m_taskbarCapture->EnsureFrame();
        }
        for (auto& tray : m_secondaryTrays) {
            if (tray.capture && !tray.capture->HasCachedFrame()) {
                tray.capture->GetCurrentFrame();
                if (!tray.capture->HasCachedFrame())
                    tray.capture->EnsureFrame();
            }
        }
        // Then try one tile capture per frame.
        for (auto& cap : m_captures) {
            if (cap && !cap->HasCachedFrame()) {
                cap->EnsureFrame();
                break;   // One per frame to avoid stalls
            }
        }
    }

    // Live preview off (user toggle / Low profile / auto-perf tier 3):
    // freeze every tile at its first frame delivered by THIS session —
    // latch it into the cached SRV, then close the WGC session.  Until a
    // fresh frame arrives the capture keeps streaming, so the tile never
    // shows a minutes-old snapshot from a previous flip session.
    if (!EffectiveLivePreview()) {
        for (auto& cap : m_captures) {
            if (cap && cap->IsCapturing() && cap->HasNewFrame()) {
                cap->GetCurrentFrame();
                cap->Stop();
            }
        }
    }

    if (m_windows.empty()) {
        Escape();
        return;
    }

    m_renderer.BeginFrame();
    m_quad.SetAntialiasing(EffectiveAntialiasing());

    RECT rc;
    GetClientRect(m_renderer.GetHwnd(), &rc);
    float vpW = static_cast<float>(rc.right - rc.left);
    float vpH = static_cast<float>(rc.bottom - rc.top);
    if (vpW <= 0 || vpH <= 0) {
        m_renderer.EndFrame();
        return;
    }
    float cascadeAspect = m_cascadeAspect;
    DirectX::XMMATRIX monRemap =
        DirectX::XMLoadFloat4x4(&m_monRemapNDC);

    auto* ctx = m_renderer.GetContext();
    uint32_t count = m_scene.SlotCount();

    // Draw fully opaque black backdrop — blocks everything behind the overlay.
    m_quad.DrawDim(ctx, 1.0f);

    // --- Composed background: wallpaper + taskbar as separate layers ---
    // During animation, use frozen SRVs to prevent any live capture mutation.

    // Layer 1: Desktop wallpaper (full screen, dimmed).  Always sourced
    // from the Progman/WorkerW WGC capture so dynamic wallpapers
    // (Wallpaper Engine, Lively, etc.) keep working.  Drawn via the
    // wallpaper PS which fills any α=0 strip in the texture (Win11 <
    // 25H2 quirk where DWM leaves a blank taskbar-shaped band) by
    // sampling the closest opaque pixel above — no visible change on
    // 25H2 where the capture is fully opaque.
    {
        ID3D11ShaderResourceView* desktopSRV = nullptr;
        if (m_sessionFrozen) {
            desktopSRV = m_frozenDesktopSRV.get();
        } else if (m_desktopHwnd) {
            for (size_t i = 0; i < m_windows.size(); ++i) {
                if (m_windows[i].hwnd == m_desktopHwnd
                    && i < m_captures.size() && m_captures[i]) {
                    desktopSRV = m_captures[i]->GetCurrentFrame();
                    break;
                }
            }
        }
        if (desktopSRV) {
            QuadDrawCall bgDraw;
            DirectX::XMStoreFloat4x4(&bgDraw.mvp,
                ComputeScreenRectMVPWithOrigin(m_desktopBackdropRect,
                                               vpW, vpH,
                                               m_overlayOriginX,
                                               m_overlayOriginY));
            // DimFactor 0 = wallpaper fully visible, 1 = full target dim.
            // Dim target from config (backgroundOpacity %, default 28 ==
            // the original kBgAlpha look); the animation curve is untouched.
            const float bgAlpha = m_config
                ? static_cast<float>(m_config->backgroundOpacity) / 100.0f
                : kBgAlpha;
            bgDraw.alpha      = 1.0f - m_entryExitAnimator.DimFactor() * (1.0f - bgAlpha);
            bgDraw.blurAmount = 0.0f;
            m_quad.DrawWallpaper(ctx, desktopSRV, bgDraw);
        }
    }

    // Layer 2: Taskbar — quad sized to m_taskbarRect, UV-cropped to the
    // visible portion of the WGC texture (see Activate taskbar layer).
    {
        // Live taskbar preview samples the running WGC stream (the real bar
        // is held visible behind the overlay); otherwise the frozen pre-hide
        // frame is the whole session's taskbar.
        ID3D11ShaderResourceView* tbSRV = nullptr;
        if (m_taskbarLiveActive && m_taskbarCapture)
            tbSRV = m_taskbarCapture->GetCurrentFrame();
        if (!tbSRV && m_frozenTaskbarSRV)
            tbSRV = m_frozenTaskbarSRV.get();
        if (!tbSRV && m_taskbarCapture)
            tbSRV = m_taskbarCapture->GetCurrentFrame();

#ifdef CKFLIP_DEBUG_TASKBAR
        if (g_taskbarDebugMode == TaskbarDebugMode::DisableLayer)
            tbSRV = nullptr;
        else if (g_taskbarDebugMode == TaskbarDebugMode::FreezePreHide
                 && g_taskbarFreezeSRV)
            tbSRV = g_taskbarFreezeSRV.get();

        if (g_taskbarDebugMode == TaskbarDebugMode::SolidRed) {
            // `red` geometry test — draws even without a taskbar SRV.
            float tbW = static_cast<float>(m_taskbarRect.right  - m_taskbarRect.left);
            float tbH = static_cast<float>(m_taskbarRect.bottom - m_taskbarRect.top);
            if (tbW > 0.0f && tbH > 0.0f) {
                float scaleX = (tbW / vpW) * 2.0f;
                float scaleY = (tbH / vpH) * 2.0f;
                float cx = ((m_taskbarRect.left + tbW * 0.5f) / vpW) * 2.0f - 1.0f;
                float cy = 1.0f - ((m_taskbarRect.top + tbH * 0.5f) / vpH) * 2.0f;
                QuadDrawCall tbDraw;
                DirectX::XMStoreFloat4x4(&tbDraw.mvp,
                    DirectX::XMMatrixScaling(scaleX, scaleY, 1.0f)
                    * DirectX::XMMatrixTranslation(cx, cy, 0.0f));
                tbDraw.alpha      = 1.0f;
                tbDraw.blurAmount = 0.0f;
                m_quad.DrawDebugRed(ctx, tbDraw);
            }
            tbSRV = nullptr;   // skip the normal textured draw
        }
#endif

        if (tbSRV && m_taskbarCapture) {
            int texW = 0, texH = 0;
            m_taskbarCapture->GetCapturedSize(texW, texH);
            if (texW > 0 && texH > 0) {
                float tbW = static_cast<float>(m_taskbarRect.right  - m_taskbarRect.left);
                float tbH = static_cast<float>(m_taskbarRect.bottom - m_taskbarRect.top);
                if (tbW <= 0 || tbH <= 0) { tbW = static_cast<float>(texW); tbH = static_cast<float>(texH); }
                float scaleX = (tbW / vpW) * 2.0f;
                float scaleY = (tbH / vpH) * 2.0f;
                float cx = ((m_taskbarRect.left + tbW * 0.5f) / vpW) * 2.0f - 1.0f;
                float cy = 1.0f - ((m_taskbarRect.top + tbH * 0.5f) / vpH) * 2.0f;

                QuadDrawCall tbDraw;
                DirectX::XMStoreFloat4x4(&tbDraw.mvp,
                    DirectX::XMMatrixScaling(scaleX, scaleY, 1.0f)
                    * DirectX::XMMatrixTranslation(cx, cy, 0.0f));
                tbDraw.alpha      = 1.0f;
                tbDraw.blurAmount = 0.0f;
                // v8.7 Bug TB — content-band UV crop must be applied in
                // RenderFrame too, not only Activate's first-content frame.
                // Before this fix the second rendered frame onwards reverted
                // to the bottom crop, which on Win10 22H2 / Win11 24H2 sampled
                // the dark #282832 fill below the real taskbar band — the
                // taskbar "worked for one frame then vanished".  Shared helper
                // keeps Activate, RenderFrame and the debug dump identical.
                // On Win11 25H2 m_taskbarContentResolved stays false, so this
                // is a no-op and 25H2 behaviour is preserved.
                ComputeTaskbarContentBandUV(texH, tbH,
                    m_taskbarContentResolved, m_taskbarContentCenterY,
                    tbDraw.uvMinY, tbDraw.uvMaxY);
#ifdef CKFLIP_DEBUG_TASKBAR
                if (g_taskbarDebugMode == TaskbarDebugMode::AssumeStraightAlpha)
                    m_quad.DrawAssumeStraightAlpha(ctx, tbSRV, tbDraw);
                else
                    m_quad.Draw(ctx, tbSRV, tbDraw);
#else
                m_quad.Draw(ctx, tbSRV, tbDraw);
#endif
            }
        }
    }

    for (auto& tray : m_secondaryTrays) {
        ID3D11ShaderResourceView* secSRV = nullptr;
        if (tray.liveActive && tray.capture)
            secSRV = tray.capture->GetCurrentFrame();
        if (!secSRV)
            secSRV = tray.frozenSRV.get();
        if (!secSRV && tray.capture)
            secSRV = tray.capture->GetCurrentFrame();
        DrawTaskbarLayer(ctx, m_quad, tray.capture.get(), secSRV,
                         tray.rectOverlay, tray.contentResolved,
                         tray.contentCenterY, vpW, vpH, false);
    }

    // Advance cycle animation (if active) and apply slot overrides.
    m_cycleAnim.Tick(m_scene);

    // If animation just finished and there are queued cycles, start the next
    // one immediately — creates seamless continuous motion when key is held.
    ProcessCycleQueue();

    // Advance the close transition (if active) — slides survivors to the
    // rebuilt smaller layout and fades the dying tiles.  Never concurrent
    // with the cycle anim (RemoveClosedWindows waits for the cycle+queue
    // to drain; Cycle/CycleBack are blocked while this runs).
    m_closeAnim.Tick(m_scene);

    // Release the dying tiles' frozen captures the moment the transition
    // is over — however it ended (natural finish, cancel, or the snap in
    // Dismiss/Escape).  The windows are gone; the frames have no reuse
    // value, and an empty list keeps the draw loop free of stale tiles.
    if (!m_closeAnim.IsActive() && !m_closingCaptures.empty())
        ClearClosingCaptures();

    // Advance entry/exit morph (if active).  Mutates scene slots + tilt.
    m_entryExitAnimator.Tick(m_scene);

    // Handle deferred mid-entry reverse (see Dismiss/Escape).  Two
    // possible terminations:
    //   1. Entry still active when the delay elapses → ReverseInPlace,
    //      mapping current rawT to (1 - rawT) on the reverse track.
    //   2. Entry finishes naturally during the delay → run the standard
    //      BeginExit path so the exit morph plays from full cascade.
    if (m_reverseDelayPending) {
        if (!m_entryExitAnimator.IsActive()) {
            // Entry already wrapped up while we were waiting — fall through
            // to a normal Dismiss/Escape that snapshots the cascade and
            // begins exit cleanly.
            bool fromEscape = m_reverseDelayFromEscape;
            m_reverseDelayPending = false;
            if (fromEscape) Escape();
            else            Dismiss();
        } else {
            LARGE_INTEGER nowQpc;
            QueryPerformanceCounter(&nowQpc);
            double elapsedMs = m_perfFreq.QuadPart > 0
                ? static_cast<double>(nowQpc.QuadPart - m_reverseDelayStartQPC.QuadPart)
                  * 1000.0 / static_cast<double>(m_perfFreq.QuadPart)
                : kReverseDelayMs;
            if (elapsedMs >= kReverseDelayMs) {
                if (m_entryExitAnimator.ReverseInPlace()) {
                    m_exitPending    = true;
                    m_exitFromEscape = m_reverseDelayFromEscape;
                }
                m_reverseDelayPending = false;
            }
        }
    }

    // Bug 7' — same-frame finalized flat present.  When the exit morph
    // finishes, do NOT tear down immediately; defer FinishDismiss/
    // FinishEscape until after this frame's tile draw list + Present so
    // the finalized flat poses (already written to FlipScene by
    // Finalize()) are drawn once before teardown — eliminates the
    // one-frame close-out flash.
    bool finishAfterPresent = false;
    bool finishFromEscape   = false;
    if (m_entryExitAnimator.JustFinishedExit() && m_exitPending) {
        m_exitPending = false;
        finishAfterPresent = true;
        finishFromEscape   = m_exitFromEscape;
        // Do NOT call FinishDismiss/FinishEscape yet — fall through to
        // draw the finalized flat scene first.
    }

    // Bug 7' (v8.2 — A) — wider frozen-SRV cleanup guard.  This block runs
    // before the tile draw list, so it must NOT clear frozen SRVs during
    // any active entry/exit morph, during exit frames awaiting
    // finalization (m_exitPending), or on the final-flat frame
    // (finishAfterPresent) — otherwise the draw list would switch from
    // frozen SRVs to live captures mid-morph.
    const bool entryExitBusy =
        m_entryExitAnimator.IsActive() ||
        m_exitPending ||
        finishAfterPresent;

    if (!entryExitBusy && !m_cycleAnim.IsActive() && m_sessionFrozen) {
        m_frozenStartSRVs.clear();
        m_frozenTargetSRVs.clear();
        m_frozenDesktopSRV = nullptr;
        m_frozenTaskbarSRV = nullptr;
        m_sessionFrozen = false;
    }

    float motionIntensity = m_cycleAnim.GetMotionIntensity();
    if (m_closeAnim.GetMotionIntensity() > motionIntensity)
        motionIntensity = m_closeAnim.GetMotionIntensity();
    float motionBlur = motionIntensity * 0.004f;
    if (!EffectiveMotionBlur()) motionBlur = 0.0f;

    // Build draw list and sort by Z-depth (furthest first).  Overflow tiles
    // — entry-only, fading toward the back-most cascade slot — share the
    // same depth-sort so they composite correctly with the visible cascade
    // during the morph.  Dying close-anim tiles (windows closed while the
    // cascade is up, fading out) join the same sort for the same reason.
    struct DrawEntry {
        int   kind;    // 0 = cascade slot, 1 = overflow tile, 2 = dying tile
        int   idx;
        float z;
    };
    const std::vector<TileSlot>& overflow = m_entryExitAnimator.GetOverflowSlots();
    const std::vector<TileSlot>& dying    = m_closeAnim.GetDyingSlots();
    std::vector<DrawEntry> drawOrder;
    drawOrder.reserve(count + overflow.size() + dying.size());
    for (uint32_t i = 0; i < count; ++i) {
        drawOrder.push_back({ 0, static_cast<int>(i), m_scene.GetSlot(i).z });
    }
    for (size_t k = 0; k < overflow.size(); ++k) {
        drawOrder.push_back({ 1, static_cast<int>(k), overflow[k].z });
    }
    for (size_t k = 0; k < dying.size(); ++k) {
        drawOrder.push_back({ 2, static_cast<int>(k), dying[k].z });
    }
    std::sort(drawOrder.begin(), drawOrder.end(),
              [](const DrawEntry& a, const DrawEntry& b) {
                  return a.z > b.z;
              });

    // Draw back-to-front using sorted order.
    const std::vector<HWND>& overflowHwnds = m_entryExitAnimator.GetOverflowHwnds();
    for (const auto& entry : drawOrder) {
        if (entry.kind == 2) {
            // Dying close-anim tile — freestanding like overflow, MVP
            // built inline from the same camera + scene tilt; textured
            // from the closed window's frozen last frame.  FlipScene
            // stays read-only.
            size_t k = static_cast<size_t>(entry.idx);
            if (k >= dying.size()) continue;
            const TileSlot& slot = dying[k];
            if (slot.alpha < 0.001f) continue;

            using namespace DirectX;
            XMMATRIX world =
                XMMatrixScaling(slot.scaleX, slot.scaleY, 1.0f) *
                XMMatrixRotationX(XMConvertToRadians(m_scene.GetSceneTiltX())) *
                XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY())) *
                XMMatrixTranslation(slot.x, slot.y, slot.z);
            XMVECTOR eye    = XMVectorSet(m_scene.GetCamEyeX(),    m_scene.GetCamEyeY(),    m_scene.GetCamEyeZ(),    1.0f);
            XMVECTOR target = XMVectorSet(m_scene.GetCamTargetX(), m_scene.GetCamTargetY(), m_scene.GetCamTargetZ(), 1.0f);
            XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            XMMATRIX view   = XMMatrixLookAtLH(eye, target, up);
            XMMATRIX proj   = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(m_scene.GetFovDeg()), cascadeAspect, 0.1f, 200.0f);

            QuadDrawCall draw;
            XMStoreFloat4x4(&draw.mvp, world * view * proj * monRemap);
            draw.alpha = slot.alpha;
            draw.blurAmount = motionBlur;

            ID3D11ShaderResourceView* srv = nullptr;
            if (k < m_closingCaptures.size() && m_closingCaptures[k])
                srv = m_closingCaptures[k]->GetCurrentFrame();
            if (srv) m_quad.Draw(ctx, srv, draw);
            else     m_quad.DrawPlaceholder(ctx, draw);
            continue;
        }

        if (entry.kind == 1) {
            // Overflow tile — build MVP inline using the same camera +
            // scene-tilt the cascade tiles use, but with the freestanding
            // overflow TileSlot's transform.  FlipScene stays read-only.
            size_t k = static_cast<size_t>(entry.idx);
            if (k >= overflow.size()) continue;
            const TileSlot& slot = overflow[k];
            if (slot.alpha < 0.001f) continue;

            using namespace DirectX;
            XMMATRIX world =
                XMMatrixScaling(slot.scaleX, slot.scaleY, 1.0f) *
                XMMatrixRotationX(XMConvertToRadians(m_scene.GetSceneTiltX())) *
                XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY())) *
                XMMatrixTranslation(slot.x, slot.y, slot.z);
            XMVECTOR eye    = XMVectorSet(m_scene.GetCamEyeX(),    m_scene.GetCamEyeY(),    m_scene.GetCamEyeZ(),    1.0f);
            XMVECTOR target = XMVectorSet(m_scene.GetCamTargetX(), m_scene.GetCamTargetY(), m_scene.GetCamTargetZ(), 1.0f);
            XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            XMMATRIX view   = XMMatrixLookAtLH(eye, target, up);
            XMMATRIX proj   = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(m_scene.GetFovDeg()), cascadeAspect, 0.1f, 200.0f);

            QuadDrawCall draw;
            XMStoreFloat4x4(&draw.mvp, world * view * proj * monRemap);
            draw.alpha = slot.alpha;
            draw.blurAmount = motionBlur;
            if (k < overflowHwnds.size()
                && overflowHwnds[k] == m_desktopHwnd)
                ApplyTextureUV(draw, m_desktopTileUV);
            if (draw.alpha < 0.001f)
                continue;

            ID3D11ShaderResourceView* srv = nullptr;
            if (k < overflowHwnds.size()) {
                HWND ohwnd = overflowHwnds[k];
                for (size_t wi = 0; wi < m_windows.size(); ++wi) {
                    if (m_windows[wi].hwnd == ohwnd) {
                        if (wi < m_captures.size() && m_captures[wi])
                            srv = m_captures[wi]->GetCurrentFrame();
                        break;
                    }
                }
            }
            if (srv) m_quad.Draw(ctx, srv, draw);
            else     m_quad.DrawPlaceholder(ctx, draw);
            continue;
        }

        uint32_t idx = static_cast<uint32_t>(entry.idx);

        QuadDrawCall draw;
        float alpha;
        m_scene.GetDrawCall(idx, cascadeAspect, draw.mvp, alpha);
        DirectX::XMMATRIX perspMVP =
            DirectX::XMLoadFloat4x4(&draw.mvp) * monRemap;
        DirectX::XMStoreFloat4x4(&draw.mvp, perspMVP);
        if ((m_entryExitAnimator.IsActive() || finishAfterPresent)
            && idx < m_windows.size()) {
            const RECT& morphRect =
                ResolveMorphScreenRect(m_entryExitAnimator, m_windows, idx);
            DirectX::XMMATRIX screenMVP =
                ComputeScreenSpaceMVP(morphRect, vpW, vpH);
            DirectX::XMStoreFloat4x4(&draw.mvp,
                LerpMatrix(screenMVP, perspMVP,
                           m_entryExitAnimator.GetMorphBlend()));
        }
        draw.alpha = alpha;
        draw.blurAmount = motionBlur;
        if (idx < m_windows.size() && m_windows[idx].hwnd == m_desktopHwnd)
            ApplyTextureUV(draw, m_desktopTileUV);

        if (alpha < 0.001f)
            continue;

        // Get the captured frame texture (SRV).
        // Wrapping tile texture choice:
        //   Forward wrap (idx == n-1): the tile journey is "front → N0 →
        //     backSpawn → back".  It's the OLD front (pre-rotation slot 0)
        //     visually sliding past the camera and reappearing at the back.
        //     Phase 1 needs the pre-rotate SRV (W1's frame), phase 2 the
        //     post-rotate SRV (still W1, since post-rotation slot n-1 = W1).
        //   Backward wrap (idx == 0): the tile journey is "back →
        //     backSpawn → N0 → front".  It's the NEW front (post-rotation
        //     slot 0) wrapping around.  At N == cascade slot count the
        //     pre-rotation slot n-1 happens to be the same window
        //     (m_frozenStartSRVs[n-1] == m_frozenTargetSRVs[0]) and the
        //     old code worked.  At N > cascade count there's overflow:
        //     pre-rotation slot n-1 is some window ABOUT to be pushed
        //     OUT of the cascade, while post-rotation slot 0 is a
        //     DIFFERENT window (the previously-overflow window) that's
        //     wrapping IN.  Using m_frozenStartSRVs[n-1] for phase 1
        //     therefore showed the wrong texture during the first 40%
        //     of the cycle and snapped to the right one at phase 2.
        //     Always use the post-rotation new-front SRV — same window
        //     throughout the wrap.
        ID3D11ShaderResourceView* srv = nullptr;
        if (m_exitSelectedStableSRV
            && (m_entryExitAnimator.IsReverse() || finishAfterPresent)
            && idx == 0
            && idx < m_windows.size()
            && m_windows[idx].hwnd == m_exitSelectedStableHwnd) {
            srv = m_exitSelectedStableSRV.get();
        } else if (m_sessionFrozen) {
            uint32_t n = m_cycleAnim.SlotCount();
            bool isWrapSlot = (m_cycleAnim.IsForward() && idx == n - 1) ||
                              (!m_cycleAnim.IsForward() && idx == 0);

            if (isWrapSlot && m_cycleAnim.IsInWrapPhase1()) {
                if (m_cycleAnim.IsForward()) {
                    // Forward wrap phase 1: pre-rotate SRV of the
                    // departing front window (= post-rotation slot
                    // n-1, which equals OLD slot 0).
                    uint32_t srcIdx = (idx + 1) % n;   // = 0
                    if (srcIdx < m_frozenStartSRVs.size())
                        srv = m_frozenStartSRVs[srcIdx].get();
                } else {
                    // Backward wrap phase 1: pre-rotation SRV of the
                    // departing back-slot window.  At N > slot count
                    // the window at pre-rotation slot n-1 differs
                    // from post-rotation slot 0; using the post-
                    // rotate SRV would flash the arriving window's
                    // texture at the old back position before it
                    // wraps to the front.  Alpha is 0 at the phase
                    // boundary so the texture switch to the post-
                    // rotate SRV in phase 2 is imperceptible.
                    uint32_t backIdx = n - 1;
                    if (backIdx < m_frozenStartSRVs.size())
                        srv = m_frozenStartSRVs[backIdx].get();
                }
            } else {
                // Phase 2 or non-wrapping: use post-rotate SRV.
                if (idx < m_frozenTargetSRVs.size())
                    srv = m_frozenTargetSRVs[idx].get();
            }
        } else if (idx < m_captures.size() && m_captures[idx]) {
            srv = m_captures[idx]->GetCurrentFrame();
        }

        draw.alpha = alpha;
        if (draw.alpha >= 0.001f) {
            if (srv) {
                m_quad.Draw(ctx, srv, draw);
            } else {
                m_quad.DrawPlaceholder(ctx, draw);
            }
        }
    }

    if (m_taskbarDrawOnTop) {
        ID3D11ShaderResourceView* tbSRV = nullptr;
        if (m_taskbarLiveActive && m_taskbarCapture)
            tbSRV = m_taskbarCapture->GetCurrentFrame();
        if (!tbSRV)
            tbSRV = m_frozenTaskbarSRV.get();
        if (!tbSRV && m_taskbarCapture)
            tbSRV = m_taskbarCapture->GetCurrentFrame();
        DrawTaskbarLayer(ctx, m_quad, m_taskbarCapture.get(), tbSRV,
                         m_taskbarRect, m_taskbarContentResolved,
                         m_taskbarContentCenterY, vpW, vpH);
    }

    // Present.  Default path: non-blocking Present(0) + DwmFlush for frame
    // pacing.  Present(0) queues the frame to the GPU immediately without
    // blocking; DwmFlush then waits for the compositor's next refresh.
    // This is more responsive than Present(1), which blocks inside the
    // driver and can cause cursor/input lag on VMs and some GPU configs.
    //
    // VSYNC live preview (config vsyncLivePreview): Present(1) instead —
    // the present is queued against the monitor's vblank, so every refresh
    // shows a fresh preview frame.  The trailing DwmFlush must be skipped
    // in that mode or the loop would wait out a second compositor tick and
    // halve the frame rate.
    const bool vsyncPace = m_config && m_config->vsyncLivePreview;
    if (vsyncPace)
        m_renderer.EndFrameVSync();
    else
        m_renderer.EndFrame();

    // Bug 7' — finalized flat scene has now been presented; run the
    // deferred teardown after Present + DwmFlush.
    if (finishAfterPresent) {
        DwmFlush();
        if (finishFromEscape) FinishEscape();
        else                  FinishDismiss();
        return;
    }

    if (!vsyncPace)
        DwmFlush();

    // --- Performance monitoring ---
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (m_lastFrameTime.QuadPart > 0 && m_perfFreq.QuadPart > 0) {
            double frameMs = static_cast<double>(now.QuadPart - m_lastFrameTime.QuadPart)
                           / static_cast<double>(m_perfFreq.QuadPart) * 1000.0;
            m_frameTimes[m_frameTimeIdx] = frameMs;
            m_frameTimeIdx = (m_frameTimeIdx + 1) % kPerfSampleCount;
            if (m_frameTimeCount < kPerfSampleCount) m_frameTimeCount++;

            // Every full sample window: compute the average once for both
            // the debug print and the auto-perf-tune ladder.
            if (m_frameTimeCount == kPerfSampleCount && m_frameTimeIdx == 0) {
                double sum = 0;
                for (size_t i = 0; i < kPerfSampleCount; i++) sum += m_frameTimes[i];
                double avgMs = sum / static_cast<double>(kPerfSampleCount);

                if (m_config && m_config->showDebugInfo) {
                    wchar_t buf[128];
                    swprintf_s(buf, L"CKFlip3D: Avg frame %.2f ms (%.1f fps)\n",
                              avgMs, 1000.0 / avgMs);
                    OutputDebugStringW(buf);
                }

                // Auto performance tune (profile Auto only) — two-way
                // ladder with hysteresis.
                //
                // Degradation compares against a 60 Hz-floored budget: on
                // 120/144/165 Hz displays the raw per-refresh budget is so
                // small (6-8 ms) that mid-range GPUs failed it every
                // window and silently rode the ladder to tier 3 (live
                // preview off) within ~4 s of the first session — the
                // "live previews freeze on weaker hardware" bug.  The
                // cascade does not need native-refresh frame rates to look
                // right; only dropping below ~44 fps (60 Hz budget ×1.35)
                // is treated as "this device can't cope".
                //
                // Recovery: kPerfRecoveryWindows consecutive comfortable
                // windows (avg < 0.85× budget) step one tier back up.  The
                // wide 0.85/1.35 hysteresis gap plus the multi-second
                // dwell prevents oscillation.  Tiles already frozen by
                // tier 3 stay frozen for the rest of the session (their
                // WGC sessions were stopped) — recovery re-enables live
                // preview from the NEXT activation, so there is no
                // mid-session flicker.
                if (m_config && m_config->autoPerfTune
                    && m_config->perfProfile == -1) {
                    const double tuneBudgetMs =
                        (std::max)(m_refreshBudgetMs, kMinTuneBudgetMs);
                    if (avgMs > tuneBudgetMs * 1.35 && m_perfTier < 3) {
                        m_perfTier++;
                        m_perfGoodWindows = 0;
                        wchar_t buf[160];
                        swprintf_s(buf,
                            L"CKFlip3D: auto perf tune → tier %d (avg %.2f ms, budget %.2f ms) — %s disabled\n",
                            m_perfTier, avgMs, tuneBudgetMs,
                            m_perfTier == 1 ? L"motion blur"
                            : m_perfTier == 2 ? L"antialiasing" : L"live preview");
                        CKLog::Log(buf);
                    } else if (avgMs < tuneBudgetMs * 0.85 && m_perfTier > 0) {
                        if (++m_perfGoodWindows >= kPerfRecoveryWindows) {
                            m_perfTier--;
                            m_perfGoodWindows = 0;
                            wchar_t buf[160];
                            swprintf_s(buf,
                                L"CKFlip3D: auto perf tune → tier %d (avg %.2f ms, budget %.2f ms) — %s restored\n",
                                m_perfTier, avgMs, tuneBudgetMs,
                                m_perfTier == 0 ? L"motion blur"
                                : m_perfTier == 1 ? L"antialiasing" : L"live preview");
                            CKLog::Log(buf);
                        }
                    } else {
                        m_perfGoodWindows = 0;
                    }
                }
            }
        }
        m_lastFrameTime = now;
    }
}

// ---------------------------------------------------------------------------
// Effective quality: user toggles + manual profile override + auto-tune tier.
//   perfProfile  2 (High):   user toggles as-is, never degraded
//   perfProfile  1 (Medium): motion blur forced off
//   perfProfile  0 (Low):    motion blur + antialiasing forced off
//   perfProfile -1 (Auto):   user toggles minus the auto-tune tier
// ---------------------------------------------------------------------------
bool FlipController::EffectiveMotionBlur() const
{
    if (!m_config) return true;
    if (m_config->perfProfile == 0 || m_config->perfProfile == 1) return false;
    if (m_config->perfProfile == -1 && m_config->autoPerfTune && m_perfTier >= 1)
        return false;
    return m_config->motionBlur;
}

bool FlipController::EffectiveAntialiasing() const
{
    if (!m_config) return true;
    if (m_config->perfProfile == 0) return false;
    if (m_config->perfProfile == -1 && m_config->autoPerfTune && m_perfTier >= 2)
        return false;
    return m_config->antialiasing;
}

bool FlipController::EffectiveLivePreview() const
{
    if (!m_config) return true;
    if (m_config->perfProfile == 0) return false;
    if (m_config->perfProfile == -1 && m_config->autoPerfTune && m_perfTier >= 3)
        return false;
    return m_config->livePreview;
}

bool FlipController::AnimEntryExitEnabled() const
{
    return !m_config || (m_config->animations && m_config->animEntryExit);
}

bool FlipController::AnimCycleEnabled() const
{
    return !m_config || (m_config->animations && m_config->animCycle);
}

bool FlipController::AnimCloseEnabled() const
{
    return !m_config || (m_config->animations && m_config->animClose);
}

uint32_t FlipController::EffectiveStartDelayMs() const
{
    uint32_t v = m_config ? m_config->startDelayMs : 16;
    // Auto perf tune owns the value: two vsync cycles of the measured
    // refresh rate (WGC delivers on compositor ticks), plus headroom per
    // degraded tier on machines that already proved slow.  The all-ready
    // early exit in Activate keeps generous budgets from adding latency.
    if (m_config && m_config->autoPerfTune && m_config->perfProfile == -1) {
        double d = m_refreshBudgetMs * 2.0
                 + static_cast<double>(m_perfTier) * 30.0;
        if (d < 16.0)  d = 16.0;
        if (d > 250.0) d = 250.0;
        v = static_cast<uint32_t>(d);
    }
    if (v < 1)    v = 1;
    if (v > 1000) v = 1000;
    return v;
}

// ---------------------------------------------------------------------------
void FlipController::RemoveClosedWindows()
{
    // Never modify window/capture arrays during animation — frozen SRV
    // pointers and animator start/target slots depend on stable indices.
    //
    // The entry/exit morph counts too: BuildSlots below re-derives the
    // CAMERA for the new window count (framing depends on N above
    // framingFloor), while the animator's endpoint slots are world-space
    // positions computed under the OLD camera.  Removing a window mid-morph
    // made Tick's size-mismatch guard Finalize with those stale slots — the
    // whole stack rendered horizontally displaced ("cascade jumps right,
    // almost out of bounds").  Deferring removal until the morph ends (a
    // few hundred ms) keeps geometry and camera consistent at all times.
    if (m_sessionFrozen || m_entryExitAnimator.IsActive())
        return;

    // Exit already armed or under way: the overlay is about to hide —
    // starting a close transition now would fight the exit morph for the
    // slots and duplicate the teardown work.  FinishDismiss/FinishEscape
    // clear the arrays anyway.
    if (m_exitPending || m_reverseDelayPending)
        return;

    // The close transition waits for cycling to FULLY drain — the active
    // animation AND every queued command.  Starting mid-cycle would mix
    // two writers of the same slot state and desync the frozen-SRV
    // indexing (classic race).  m_sessionFrozen above already covers most
    // frames of an active cycle; these checks make the gate airtight.
    if (m_cycleAnim.IsActive() || !m_cycleQueue.empty())
        return;

    // NOTE: an already-running close transition does NOT gate this sweep.
    // Windows closed mid-transition MERGE into it (CloseAnimator::Begin
    // carries the in-flight dying tiles over and re-routes the survivors
    // from their current mid-lerp pose), so an N-window burst costs one
    // fresh 220 ms pass from the merge point — never N stacked passes.
    // Because a closed window leaves m_windows the moment its transition
    // starts, it can never be re-detected here — the fade-out can neither
    // loop nor duplicate.

    // ---- Detect closed windows -------------------------------------------
    std::vector<size_t> closed;
    for (size_t i = 0; i < m_windows.size(); ++i) {
        // Desktop pseudo-window: always keep (Progman/WorkerW is immortal).
        if (m_windows[i].hwnd == m_desktopHwnd)
            continue;
        if (!IsWindow(m_windows[i].hwnd))
            closed.push_back(i);
    }
    if (closed.empty())
        return;

    const uint32_t oldSlotCount = m_scene.SlotCount();

    // Animate only when the close animation is enabled (master toggle AND
    // its per-animation selection) and at least one closed window occupies
    // a visible cascade slot — pure-overflow closes have no tile on
    // screen, so the silent rebuild is already seamless.
    bool anyVisible = false;
    for (size_t i : closed) {
        if (i < oldSlotCount) { anyVisible = true; break; }
    }
    const bool animate = anyVisible && AnimCloseEnabled();

    // Snapshot the current visual pose BEFORE any removal/rebuild — these
    // are the close transition's start slots.  The camera pose is captured
    // alongside them: BuildSlots below re-frames the camera for the new
    // count, and CloseAnimator::Begin re-expresses every start pose in the
    // new camera's view space so frame 1 of the transition projects
    // exactly like the last pre-close frame (no lateral/vertical snap).
    std::vector<TileSlot> startSlots(oldSlotCount);
    for (uint32_t i = 0; i < oldSlotCount; ++i)
        startSlots[i] = m_scene.GetSlot(i);
    const CloseAnimator::CameraPose oldCam{
        m_scene.GetCamEyeX(),    m_scene.GetCamEyeY(),    m_scene.GetCamEyeZ(),
        m_scene.GetCamTargetX(), m_scene.GetCamTargetY(), m_scene.GetCamTargetZ()
    };

    // ---- Remove the dead entries (reverse order keeps indices valid) ------
    // Visible dying windows hand their capture object over so the frozen
    // last frame survives for the fade-out draw; Stop() preserves the
    // cached SRV.  dyingCaps stays 1:1 with dyingSlotIdx even when a
    // capture is missing (null entry → placeholder tile).
    std::vector<uint32_t> dyingSlotIdx;
    std::vector<std::unique_ptr<WGCCapture>> dyingCaps;
    for (auto it = closed.rbegin(); it != closed.rend(); ++it) {
        size_t i = *it;
        const bool dyingVisible = animate && i < oldSlotCount;
        if (dyingVisible) {
            std::unique_ptr<WGCCapture> cap;
            if (i < m_captures.size() && m_captures[i]) {
                m_captures[i]->Stop();
                cap = std::move(m_captures[i]);
            }
            dyingCaps.push_back(std::move(cap));
            dyingSlotIdx.push_back(static_cast<uint32_t>(i));
        }
        if (i < m_captures.size()) {
            if (m_captures[i]) m_captures[i]->Stop();
            m_captures.erase(m_captures.begin() + static_cast<ptrdiff_t>(i));
        }
        m_windows.erase(m_windows.begin() + static_cast<ptrdiff_t>(i));
    }
    // The reverse walk produced descending lists; Begin() and the dying-
    // tile draw expect ascending slot order.
    std::reverse(dyingSlotIdx.begin(), dyingSlotIdx.end());
    std::reverse(dyingCaps.begin(), dyingCaps.end());

    // ---- Rebuild the 3D scene with the updated window count ---------------
    RECT rc;
    GetClientRect(m_renderer.GetHwnd(), &rc);
    float vpW = static_cast<float>(rc.right - rc.left);
    float vpH = static_cast<float>(rc.bottom - rc.top);
    if (vpW <= 0) vpW = 1920.0f;
    if (vpH <= 0) vpH = 1080.0f;
    UpdateCascadeSpace(vpW, vpH);

    uint32_t totalWin    = static_cast<uint32_t>(m_windows.size());
    uint32_t displayCount = totalWin;
    if (m_config && m_config->maxWindows < displayCount)
        displayCount = m_config->maxWindows;
    m_scene.BuildSlots(displayCount, m_cascadeW, m_cascadeH);
    RebuildSceneAspects();

    // BuildSlots above re-derived the camera for the smaller count, so
    // the entry-time flat slots cached per HWND (Round-6 Fix 19) are now
    // expressed in a stale camera frame.  If BeginExit later substituted
    // them as exit targets, every tile would fly toward a horizontally
    // displaced position and only "snap back" when the overlay hides
    // and the real windows show through.  Dropping the cache makes the
    // exit fall back to fresh flat rects computed under the current
    // camera — correct screen positions, no snap.
    m_entryExitAnimator.ClearEntryFlatCache();

    // ---- Start the close transition ---------------------------------------
    // The camera re-frame from BuildSlots is compensated inside Begin()
    // (start poses re-expressed via oldCam's view space), so the whole
    // camera change is absorbed into the smooth slot lerp — no visible
    // shift on any axis at the start of the transition.
    if (animate && !dyingSlotIdx.empty()) {
        // Merge path keeps the existing dying captures in place (parallel
        // to the animator's carried tiles) and appends the new ones — the
        // same order Begin() appends its new dying slots.  A fresh start
        // has nothing to keep; defensively drop leftovers so the capture
        // list can never desync from the animator's dying-tile list.
        if (!m_closeAnim.IsActive())
            ClearClosingCaptures();
        for (auto& cap : dyingCaps)
            m_closingCaptures.push_back(std::move(cap));
        m_closeAnim.Begin(m_scene, startSlots, dyingSlotIdx, oldCam);
        CKLog::Log(L"CKFlip: Window closed — close transition started\n");
    } else {
        CKLog::Log(L"CKFlip: Window closed — rebuilt scene\n");
    }
}

// ---------------------------------------------------------------------------
void FlipController::ClearClosingCaptures()
{
    for (auto& cap : m_closingCaptures) {
        if (cap)
            cap->Stop();
    }
    m_closingCaptures.clear();
}

// ---------------------------------------------------------------------------
void FlipController::CloakNewWindows()
{
    // Periodically sweep for new top-level windows that appeared after
    // activation (e.g. popups, dialogs, new app launches).
    // Reuses the same ShouldCloak criteria from CloakVisibleAppWindows.
    // DoCloakWindow skips already-tracked HWNDs, so this is cheap.
    static DWORD s_lastCheck = 0;
    DWORD now = GetTickCount();
    if (now - s_lastCheck < 500)
        return;
    s_lastCheck = now;

    std::vector<HWND> exclude;
    exclude.push_back(m_renderer.GetHwnd());
    if (m_desktopHwnd) exclude.push_back(m_desktopHwnd);

    WindowCloaker::CloakVisibleAppWindows(GetCurrentProcessId(), exclude);
}

// ---------------------------------------------------------------------------
// Start a WGC capture session for the taskbar (Shell_TrayWnd).
// WGC gives live, composited content identical to what DWM displays.
// ---------------------------------------------------------------------------
// True when an autohide bar is substantially on-screen (slid out) — at
// least half of its window area intersects its monitor.  A retracted Win11
// autohide bar sits almost entirely past the monitor edge.
static bool IsBarExtended(HWND bar)
{
    RECT r{};
    if (!bar || !GetWindowRect(bar, &r) || !ValidRect(r))
        return false;
    HMONITOR mon = MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi))
        return false;
    RECT inter{};
    if (!IntersectRect(&inter, &r, &mi.rcMonitor))
        return false;
    const LONG barArea = (r.right - r.left) * (r.bottom - r.top);
    const LONG visArea = (inter.right - inter.left) * (inter.bottom - inter.top);
    return barArea > 0 && visArea * 2 >= barArea;
}

void FlipController::StartTaskbarCapture()
{
    m_secondaryTrays.clear();
    m_taskbarAutoHide = false;
    m_taskbarExtendedAtStart = false;
    m_taskbarHoldRectScreen = RECT{};

    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!taskbar)
        return;

    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    const bool taskbarAutoHide =
        (SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) != 0;
    m_taskbarAutoHide = taskbarAutoHide;
    m_taskbarExtendedAtStart = taskbarAutoHide && IsBarExtended(taskbar);
    GetWindowRect(taskbar, &m_taskbarHoldRectScreen);

    // Resolve the taskbar's VISIBLE on-screen rect.  On Win11 ≤24H2 the
    // Shell_TrayWnd window extends above the visible bar (the XAML host
    // has a semi-opaque strip that is not part of the painted taskbar), so
    // GetWindowRect returns a top edge significantly higher than where the
    // bar actually paints.  WGC captures this full window, and sizing the
    // render quad to the texture produces a dark band above the bar.
    //
    // Derive the visible rect from the gap between the monitor's full
    // bounds (rcMonitor) and its work area (rcWork).  The work area is
    // the desktop minus the taskbar reservation — the difference IS the
    // visible bar, regardless of how tall the Shell_TrayWnd window is.
    RECT tbRect{};
    if (!ResolveTaskbarVisibleRect(taskbar, taskbarAutoHide, tbRect))
        return;

    // Convert screen coordinates to overlay-relative coordinates.
    // The overlay covers the entire virtual screen, whose origin may be
    // negative on multi-monitor setups.
    tbRect = ScreenToOverlayRect(tbRect, m_overlayOriginX, m_overlayOriginY);

    m_taskbarHwnd = taskbar;
    m_taskbarRect = tbRect;
    m_taskbarDrawOnTop =
        taskbarAutoHide && IsWindowVisible(taskbar) && ValidRect(tbRect);

    // "Taskbar preview" toggle: when off, the session still hides/holds the
    // real bar (m_taskbarHwnd/rect stay resolved above) but no WGC capture
    // is created — every draw site already handles a null capture/SRV, so
    // nothing is rendered where the bar was.
    const bool wantPreview = !m_config || m_config->taskbarPreview;
    if (wantPreview) {
        m_taskbarCapture = std::make_unique<WGCCapture>();
        m_taskbarCapture->StartForWindow(taskbar, m_renderer.GetDevice());
    }

    HMONITOR primaryMonitor =
        MonitorFromRect(&m_monLayout.primary, MONITOR_DEFAULTTOPRIMARY);
    HWND secondary = nullptr;
    while ((secondary = FindWindowExW(nullptr, secondary,
                                      L"Shell_SecondaryTrayWnd",
                                      nullptr)) != nullptr)
    {
        RECT secScreen{};
        if (!ResolveTaskbarVisibleRect(secondary, taskbarAutoHide, secScreen))
            continue;

        HMONITOR secMonitor = MonitorFromRect(&secScreen, MONITOR_DEFAULTTONEAREST);
        if (primaryMonitor && secMonitor == primaryMonitor)
            continue;

        RECT secOverlay = ScreenToOverlayRect(secScreen,
                                              m_overlayOriginX,
                                              m_overlayOriginY);
        if (!ValidRect(secOverlay))
            continue;
        if (RectOverlapRatio(secOverlay, m_taskbarRect) >= 0.5f)
            continue;

        SecondaryTray tray;
        tray.hwnd = secondary;
        tray.rectOverlay = secOverlay;
        tray.extendedAtStart = taskbarAutoHide && IsBarExtended(secondary);
        GetWindowRect(secondary, &tray.holdRectScreen);
        if (wantPreview) {
            tray.capture = std::make_unique<WGCCapture>();
            tray.capture->StartForWindow(secondary, m_renderer.GetDevice());
        }
        m_secondaryTrays.push_back(std::move(tray));
    }
}

// ---------------------------------------------------------------------------
void FlipController::HideRealTaskbar()
{
    m_taskbarWasVisible = false;
    m_taskbarHeld = false;
    m_heldPinCounter = 0;
    for (auto& tray : m_secondaryTrays) {
        tray.wasVisible = false;
        tray.held = false;
    }
#ifdef CKFLIP_DEBUG_TASKBAR
    if (g_taskbarDebugMode == TaskbarDebugMode::NoHideRealTaskbar) {
        OutputDebugStringW(L"CKFlip TB-DEBUG: nohide — leaving real taskbar visible\n");
        return;   // skip the actual hide
    }
#endif
    // Hold instead of hide when either applies:
    //   - autohide bar that was slid out at activation: keeping it extended
    //     (occluded by the overlay) means the moment the overlay hides, the
    //     real bar stands exactly where the fake one was drawn and the shell
    //     retracts it with its normal slide — no sudden gap.
    //   - taskbar live preview: the WGC stream only delivers real frames
    //     while the source window stays visible.
    // Held bars are disabled (no clicks through the click-through overlay)
    // and pinned below the overlay every frame (PinHeldTaskbars).
    if (m_taskbarHwnd && IsWindowVisible(m_taskbarHwnd)) {
        const bool hold = (m_taskbarAutoHide && m_taskbarExtendedAtStart)
                       || m_taskbarLiveActive;
        if (hold && ValidRect(m_taskbarHoldRectScreen)) {
            m_taskbarHeld = true;
            EnableWindow(m_taskbarHwnd, FALSE);
            SetWindowPos(m_taskbarHwnd, m_renderer.GetHwnd(),
                         m_taskbarHoldRectScreen.left,
                         m_taskbarHoldRectScreen.top,
                         m_taskbarHoldRectScreen.right - m_taskbarHoldRectScreen.left,
                         m_taskbarHoldRectScreen.bottom - m_taskbarHoldRectScreen.top,
                         SWP_NOACTIVATE);
        } else {
            m_taskbarWasVisible = true;
            ShowWindow(m_taskbarHwnd, SW_HIDE);
        }
    }
    for (auto& tray : m_secondaryTrays) {
        if (tray.hwnd && IsWindowVisible(tray.hwnd)) {
            const bool hold = (m_taskbarAutoHide && tray.extendedAtStart)
                           || tray.liveActive;
            if (hold && ValidRect(tray.holdRectScreen)) {
                tray.held = true;
                EnableWindow(tray.hwnd, FALSE);
                SetWindowPos(tray.hwnd, m_renderer.GetHwnd(),
                             tray.holdRectScreen.left,
                             tray.holdRectScreen.top,
                             tray.holdRectScreen.right - tray.holdRectScreen.left,
                             tray.holdRectScreen.bottom - tray.holdRectScreen.top,
                             SWP_NOACTIVATE);
            } else {
                tray.wasVisible = true;
                ShowWindow(tray.hwnd, SW_HIDE);
            }
        }
    }
}

void FlipController::PinHeldTaskbars()
{
    if (!m_taskbarHeld) {
        bool anyHeld = false;
        for (auto& tray : m_secondaryTrays)
            if (tray.held) { anyHeld = true; break; }
        if (!anyHeld)
            return;
    }

    // The shell fights the hold (autohide retraction, edge-hover reveal
    // raising the bar above the overlay).  Re-pin when the rect drifted;
    // re-assert the below-overlay Z periodically as cheap insurance.
    ++m_heldPinCounter;
    const bool reassertZ = (m_heldPinCounter % 30) == 0;
    HWND overlay = m_renderer.GetHwnd();

    auto pin = [&](HWND bar, const RECT& hold) {
        if (!bar || !IsWindow(bar) || !ValidRect(hold))
            return;
        RECT cur{};
        GetWindowRect(bar, &cur);
        const bool moved = cur.left != hold.left || cur.top != hold.top
                        || cur.right != hold.right || cur.bottom != hold.bottom;
        if (!moved && !reassertZ)
            return;
        UINT flags = SWP_NOACTIVATE;
        if (!moved)
            flags |= SWP_NOMOVE | SWP_NOSIZE;
        SetWindowPos(bar, overlay, hold.left, hold.top,
                     hold.right - hold.left, hold.bottom - hold.top, flags);
    };

    if (m_taskbarHeld)
        pin(m_taskbarHwnd, m_taskbarHoldRectScreen);
    for (auto& tray : m_secondaryTrays)
        if (tray.held)
            pin(tray.hwnd, tray.holdRectScreen);
}

void FlipController::ShowRealTaskbar()
{
#ifdef CKFLIP_DEBUG_TASKBAR
    if (g_taskbarDebugMode == TaskbarDebugMode::NoHideRealTaskbar) {
        m_taskbarWasVisible = false;
        m_taskbarHeld = false;
        for (auto& tray : m_secondaryTrays) {
            tray.wasVisible = false;
            tray.held = false;
        }
        return;   // we never hid it — nothing to restore
    }
#endif
    if (m_taskbarHeld && m_taskbarHwnd && IsWindow(m_taskbarHwnd)) {
        // Re-enable input and hand the bar back to the shell at its held
        // (extended) position in the topmost band.  An autohide bar then
        // retracts with the shell's own slide animation.
        EnableWindow(m_taskbarHwnd, TRUE);
        SetWindowPos(m_taskbarHwnd, HWND_TOPMOST,
                     m_taskbarHoldRectScreen.left,
                     m_taskbarHoldRectScreen.top,
                     m_taskbarHoldRectScreen.right - m_taskbarHoldRectScreen.left,
                     m_taskbarHoldRectScreen.bottom - m_taskbarHoldRectScreen.top,
                     SWP_NOACTIVATE);
    }
    m_taskbarHeld = false;
    if (m_taskbarWasVisible && m_taskbarHwnd && IsWindow(m_taskbarHwnd)) {
        ShowWindow(m_taskbarHwnd, SW_SHOW);
    }
    m_taskbarWasVisible = false;
    m_taskbarLiveActive = false;
    for (auto& tray : m_secondaryTrays) {
        if (tray.held && tray.hwnd && IsWindow(tray.hwnd)) {
            EnableWindow(tray.hwnd, TRUE);
            SetWindowPos(tray.hwnd, HWND_TOPMOST,
                         tray.holdRectScreen.left,
                         tray.holdRectScreen.top,
                         tray.holdRectScreen.right - tray.holdRectScreen.left,
                         tray.holdRectScreen.bottom - tray.holdRectScreen.top,
                         SWP_NOACTIVATE);
        }
        tray.held = false;
        if (tray.wasVisible && tray.hwnd && IsWindow(tray.hwnd))
            ShowWindow(tray.hwnd, SW_SHOW);
        tray.wasVisible = false;
        tray.liveActive = false;
    }
}

// ---------------------------------------------------------------------------
// Desktop icon toggle — hides the SysListView32 inside SHELLDLL_DefView
// so the background shows clean wallpaper + taskbar without icon clutter.
// ---------------------------------------------------------------------------
void FlipController::HideDesktopIcons()
{
    m_iconListView   = nullptr;
    m_iconsWereVisible = false;

    // SHELLDLL_DefView can be a child of Progman or a WorkerW.
    HWND defView = nullptr;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman)
        defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);

    if (!defView) {
        HWND workerW = nullptr;
        while ((workerW = FindWindowExW(nullptr, workerW, L"WorkerW", nullptr)) != nullptr) {
            defView = FindWindowExW(workerW, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defView) break;
        }
    }

    if (!defView)
        return;

    HWND listView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
    if (!listView)
        return;

    if (IsWindowVisible(listView)) {
        m_iconListView     = listView;
        m_iconsWereVisible = true;
        ShowWindow(listView, SW_HIDE);
        CKLog::Log(L"CKFlip: Desktop icons hidden\n");
    }
}

void FlipController::RestoreDesktopIcons()
{
    if (m_iconsWereVisible && m_iconListView && IsWindow(m_iconListView)) {
        ShowWindow(m_iconListView, SW_SHOW);
        CKLog::Log(L"CKFlip: Desktop icons restored\n");
    }
    m_iconListView     = nullptr;
    m_iconsWereVisible = false;
}
