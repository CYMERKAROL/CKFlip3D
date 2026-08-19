// ---------------------------------------------------------------------------
// The whole session in one place: scanning windows, starting captures, running
// the entry morph, answering input, drawing every frame, and putting the
// desktop back exactly as it was on the way out.
//
// It is the largest file in the project on purpose.  The pieces here are the
// ones that have to stay in step with each other frame by frame, and splitting
// them apart would only move the coupling somewhere harder to see.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "flipcontroller.h"
#include "DebugLog.h"
#include "Diagnostics.h"
#include "ThemePlate.h"
#include "../capture/windowcloaker.h"
#include "../scene/CoverFlowLayout.hpp"
#include "../input/TileHitTest.hpp"
#include "../hook/keyboardhook.h"
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <DirectXMath.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <shldisp.h>
#include <objbase.h>

// Session hooks for the off-thread window-icon resolver.  Declared here
// because Activate primes it and the teardown paths stop it, while the
// resolver itself lives beside GetLabelIcon — its only reader — further down.
namespace {
HICON ClassIcon(HWND hwnd);
void StartIconResolver(const std::vector<HWND>& prime);
void StopIconResolver();
} // namespace

// ---------------------------------------------------------------------------
// Shared content-band UV crop.
//
// One implementation used by every taskbar draw/dump site so they all compute
// the SAME uvMinY/uvMaxY.  On Win10 22H2 / Win11 24H2 the WGC capture of
// Shell_TrayWnd is far taller than the visible bar, with the real taskbar in a
// thin band; DetectContentCenterV (called once in Activate) located that
// band's centre.  We crop a `tbH`-tall window centred on it, slid to stay in
// [0,1].  When `contentResolved` is false (Win11 25H2 — capture already
// bar-sized) the full texture is returned unchanged, so 25H2 is a no-op.
//
// It has to be one helper, because a site that computes its own crop drifts:
// a bottom crop of `(texH - tbH)/texH` looks reasonable and samples the dark
// #282832 fill on the OSes where the capture is tall, which shows up as a
// taskbar that works for one frame and then vanishes.
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

// The desktop pseudo-tile carries no title of its own, so the search matches
// it on the name the selected-window label already shows for it.
static constexpr const wchar_t* kDesktopSearchName = L"Desktop";

static bool ValidRect(const RECT& r)
{
    return r.right > r.left && r.bottom > r.top;
}

// ---------------------------------------------------------------------------
// A shell flyout in front of the cascade (Start, search, quick settings)
//
// Tapping the Windows key and then reaching for Win+Tab opens the cascade with
// the Start menu still up, and two things go wrong that look unrelated but
// share one cause:
//
//   * Windows refuses SetForegroundWindow to every other process while the
//     Start screen owns the foreground — one of the documented conditions, and
//     not one this process can satisfy from behind an overlay.  The commit at
//     the end of the session therefore cannot raise the window the user picked,
//     which is CK0505 with the cascade having done everything right.
//   * The Shell_TrayWnd capture is taken with the flyout composited into it.
//     The taskbar preview measures the bar's content band in that capture, gets
//     a band far taller than the bar, rejects it as implausible (correctly) and
//     falls back to drawing the WHOLE texture inside the thin bar rect — the
//     glowing, doubled-up taskbar strip.
//
// So the flyout is closed before anything else happens, which is also what the
// system's own Task View does when it opens.  Both symptoms come from the same
// two lines, and a session started with nothing open never reaches them.
// ---------------------------------------------------------------------------
static constexpr double kFlyoutDismissMs = 200.0;

static bool ProcessImageIsOneOf(DWORD pid, const wchar_t* const* names,
                                size_t count)
{
    if (pid == 0)
        return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return false;
    wchar_t path[MAX_PATH] = {};
    DWORD cch = static_cast<DWORD>(_countof(path));
    const bool ok = QueryFullProcessImageNameW(proc, 0, path, &cch) != FALSE;
    CloseHandle(proc);
    if (!ok)
        return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    const wchar_t* leaf  = slash ? slash + 1 : path;
    for (size_t i = 0; i < count; ++i) {
        if (lstrcmpiW(leaf, names[i]) == 0)
            return true;
    }
    return false;
}

/// The foreground window if it is one of the shell's own flyouts, else null.
///
/// Class alone is not enough: every XAML island lives in a
/// Windows.UI.Core.CoreWindow, so matching on it would also match an ordinary
/// UWP app the user is working in — and closing THAT is not this function's
/// business.  The owning executable is what makes it the shell's.
static HWND ForegroundShellFlyout()
{
    HWND fg = GetForegroundWindow();
    if (!fg)
        return nullptr;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, static_cast<int>(_countof(cls)));
    if (lstrcmpiW(cls, L"Windows.UI.Core.CoreWindow") != 0)
        return nullptr;

    static const wchar_t* const kShellHosts[] = {
        L"StartMenuExperienceHost.exe",   // Start menu (Win10 19H1+, Win11)
        L"SearchHost.exe",                // search flyout (Win11)
        L"SearchApp.exe",                 // search flyout (Win10)
        L"ShellExperienceHost.exe",       // quick settings / notification centre
    };
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return ProcessImageIsOneOf(pid, kShellHosts, _countof(kShellHosts))
        ? fg : nullptr;
}

static bool ShellFlyoutStillComposited(HWND fly)
{
    if (!fly || !IsWindow(fly) || !IsWindowVisible(fly))
        return false;
    // A closed flyout is usually kept alive and CLOAKED rather than destroyed,
    // and a cloaked window is still "visible" by the WS_VISIBLE test — so both
    // answers are needed, or the wait below would always run to its full
    // length.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(fly, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked)))
        && cloaked != 0)
        return false;
    return true;
}

static void DismissShellFlyout(HWND fly)
{
    // Escape, injected.  The flyout holds the focus, so it is what receives it;
    // and our own hook drops LLKHF_INJECTED input, so the cancel key cannot
    // cancel the session that is about to open.
    INPUT keys[2] = {};
    keys[0].type       = INPUT_KEYBOARD;
    keys[0].ki.wVk     = VK_ESCAPE;
    keys[1]            = keys[0];
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, keys, sizeof(INPUT));

    // Wait for it to leave the COMPOSITION, not merely to lose focus: DWM keeps
    // drawing it through its close animation, and a taskbar capture taken in
    // the middle of that still contains it.  DwmFlush paces the poll at one
    // compositor tick, the same wait the capture warm-up uses.  Bounded, and
    // only ever entered when a flyout really was in the way.
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    while (ShellFlyoutStillComposited(fly)) {
        DwmFlush();
        if (freq.QuadPart <= 0)
            break;
        QueryPerformanceCounter(&t1);
        const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart)
                        * 1000.0 / static_cast<double>(freq.QuadPart);
        if (ms >= kFlyoutDismissMs)
            break;   // it is not going; carry on rather than stall the hotkey
    }
}

/// Raise `target`, taking the second path Windows leaves open when the first is
/// refused.
///
/// SetForegroundWindow returns FALSE both when it was refused and when the
/// window was already in front, so the answer is always checked against
/// GetForegroundWindow rather than believed.  SwitchToThisWindow is what the
/// system's own switcher uses and gets through refusals the plain call does
/// not; a compositor tick separates the attempt from the verdict, because the
/// activation is not instantaneous.
static bool RaiseWindowForCommit(HWND target)
{
    if (SetForegroundWindow(target) || GetForegroundWindow() == target)
        return true;
    SwitchToThisWindow(target, TRUE);
    DwmFlush();
    return GetForegroundWindow() == target;
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

// `slotIdx` indexes the animator's per-slot flat rects; `windowIdx` is the
// window shown in that slot (differs from slotIdx only in Cover Flow with
// window overflow — see FlipController::SlotWindowIndex).
static const RECT& ResolveMorphScreenRect(const EntryExitAnimator& animator,
                                          const std::vector<WindowInfo>& windows,
                                          size_t slotIdx, size_t windowIdx)
{
    const std::vector<RECT>& flatRects = animator.GetFlatSourceRects();
    if (slotIdx < flatRects.size() && ValidRect(flatRects[slotIdx]))
        return flatRects[slotIdx];
    return windows[windowIdx].rect;
}

#ifdef CKFLIP_DEBUG_TASKBAR
// ---------------------------------------------------------------------------
// Runtime taskbar debug modes plus the pre/post-hide dumps.  Selected via the
// CKFLIP_TASKBAR_MODE environment variable.  Debug builds only; release builds
// compile none of this.
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

// Pre-hide taskbar SRV for `freeze` mode.  A strong COM reference, so it
// survives WGCCapture swapping its cached SRV.
static winrt::com_ptr<ID3D11ShaderResourceView> g_taskbarFreezeSRV;

// One-shot UV-aware taskbar dump written next to the executable.
// Takes the content-band state so the dump reports the EXACT crop the renderer
// uses, rather than a bottom crop of its own.
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
    m_wallpaperCapture.reset();
    StopIconResolver();   // no-op unless the session ended some other way
    ResetSelectedLabel();
    m_renderer.Shutdown();
}

void FlipController::Activate()
{
    if (m_active) {
        // Re-triggered while the LAST session is still morphing out.  The
        // hook has already flipped its session flag back on (to it, this is a
        // fresh activation) — so silently returning here left the two sides
        // disagreeing the moment the exit finished: the controller went idle
        // while the hook carried on swallowing every keystroke, including the
        // Windows key, for the rest of the process's life.  That is the limbo.
        //
        // Remember the request instead and honour it the moment the teardown
        // completes.  A pending exit is the only state this can be reached
        // in; a genuinely live session never posts an activation at all.
        if (m_exitPending || m_reverseDelayPending
            || m_entryExitAnimator.IsActive())
            m_reactivatePending = true;
        return;
    }

    m_reactivatePending = false;
    // Whose session this is.  The hook (or the gesture worker) raised the flag
    // before posting the activation, so this reads the identity it just minted
    // — and the teardown will hand back exactly that one, never a successor's.
    m_sessionEpoch = KeyboardHook::CurrentSessionEpoch();
    m_pendingExit  = PendingExit::None;
    m_scrubActive  = false;
    m_scrubPending = false;
    m_scrubT       = 0.0f;
    m_flinging     = false;
    m_scrubVelocity = 0.0f;
    m_scrubPendingDist = 0.0f;
    m_scrubSampleQPC.QuadPart = 0;

    // Close the Start menu (or a search / quick-settings flyout) before
    // anything else reads the screen — see DismissShellFlyout for what it
    // breaks: the taskbar preview's capture and the commit's foreground call.
    // This has to run BEFORE the launch-timing anchor below, or the dim curve
    // would open sampled at wherever the wait ended.
    if (HWND fly = ForegroundShellFlyout())
        DismissShellFlyout(fly);

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
    m_staticBackdropTexture = nullptr;   // live-background-off snapshot is per-session
    m_staticBackdropSRV     = nullptr;
    m_taskbarDrawOnTop = false;
    m_labelAnim.Reset();   // label anchor re-derives from this session's cascade
    // Pointer / search state is strictly per-session — a hover from the last
    // cascade would highlight a slot that now holds a different window, and a
    // stale query would open the stack already filtered.
    m_hover.Reset();
    m_hoverSlot    = -1;
    m_hoverStillQPC.QuadPart = 0;   // the next session's stillness is its own
    CancelSelectJump();
    ClearSearchState();
    // Seed the pointer from where it actually is, so a cascade opened under a
    // stationary hand already highlights the tile beneath it — waiting for the
    // first WM_MOUSEMOVE would mean nothing happens until the mouse twitches.
    m_pointerValid = GetCursorPos(&m_pointerScreen) != FALSE;

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
    FilterExcludedWindows();

    // 3. Exclude our overlay and inject desktop pseudo-window.
    //    Desktop tile disabled (Appearance → Desktop in cascade off): no
    //    tile is injected — the freed slot goes to the next real window —
    //    but m_desktopHwnd is still resolved because the wallpaper
    //    backdrop, the cloak exclude list and the backdrop geometry all
    //    key off it.
    m_desktopTileDisabled = m_config && !m_config->showDesktopTile;
    DeduplicateWindows();
    if (m_desktopTileDisabled)
        m_desktopHwnd = WindowScanner::FindDesktopWindow();
    else
        InjectDesktopWindow();
    UpdateDesktopCaptureGeometry();

    // 3b. Sort windows by program (grouped by PID, smaller first within group).
    SortWindowsByProgram();

    // 3c. Per-window facts the search filter needs (executable name, session
    //     order).  Resolved here, once, off the final window order — and only
    //     when searching is switched on, so it costs nothing otherwise.
    BuildWindowMetadata();

    // 4. If no windows remain, abort.
    if (m_windows.empty()) {
        // The hotkey worked and nothing happened, which is the least
        // explicable failure the switcher has.  Name the two things that
        // actually cause it.
        Diag::Report(Diag::Code::NoEligibleWindows, Diag::Sev::Warning,
                     L"The hotkey worked, but there was nothing to show",
                     m_desktopTileDisabled
                        ? L"no window passed the scan — check the exclusion list "
                          L"(General → Cascade), and note that “Desktop in "
                          L"cascade” is off, so an empty desktop has no tile "
                          L"of its own to fall back on"
                        : L"no window passed the scan — check the exclusion list "
                          L"under General → Cascade");
        m_active = false;
        return;
    }

    // 4a. Start asking, off-thread, for the icons that cannot be read from
    //     window class data — primed with the whole stack so the answers are
    //     in long before the label first appears.  See the resolver's note:
    //     this is what keeps a busy window's WM_GETICON off the render
    //     thread.  Costs the session one thread that spends its life asleep.
    {
        std::vector<HWND> primeIcons;
        primeIcons.reserve(m_windows.size());
        for (const auto& w : m_windows)
            if (w.hwnd && w.hwnd != m_desktopHwnd)
                primeIcons.push_back(w.hwnd);
        StartIconResolver(primeIcons);
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
    // Latch the visual preset for this session (Appearance → Visual
    // preset).  A config reload mid-session applies on the next
    // activation — the layout never switches under a live stack (the
    // RemoveClosedWindows rebuild keeps the latched preset).
    m_scene.SetVisualPreset(
        (m_config && m_config->visualPreset == 1) ? VisualPreset::CoverFlow
                                                  : VisualPreset::Cascade);
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

        // The entry/exit animator pairs its window list index i with
        // cascade slot i.  Hand it the slot-ordered permutation so the
        // pairing holds in Cover Flow with more windows than slots (an
        // exact copy of m_windows otherwise — the cascade is unaffected).
        const std::vector<WindowInfo> orderedWins = SlotOrderedWindows();

        // Per-window taskbar-button rect overrides for minimized windows.
        // First-pass: MSAA per-button lookup.  Fallback: synthetic icon-
        // sized rect spaced along Shell_TrayWnd so the tile still emerges
        // from the taskbar visually.  Empty rect = no override → flat
        // rect resolves via rcNormalPosition (legacy behaviour).
        // Indexed in the SAME slot order as orderedWins.
        std::vector<RECT> tbOverrides(orderedWins.size(), RECT{0,0,0,0});
        {
            // Collect indices of minimized windows in slot order.
            std::vector<size_t> minIdx;
            minIdx.reserve(orderedWins.size());
            for (size_t i = 0; i < orderedWins.size(); ++i) {
                HWND h = orderedWins[i].hwnd;
                if (h && h != m_desktopHwnd && IsIconic(h))
                    minIdx.push_back(i);
            }

            // First-pass MSAA per-button match.
            if (m_taskbarLocator.IsReady()) {
                for (size_t k : minIdx) {
                    RECT btn;
                    if (m_taskbarLocator.GetButtonRect(orderedWins[k].hwnd, btn))
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

        m_entryExitAnimator.BeginEntry(m_scene, orderedWins,
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
            if (m_wallpaperCapture && m_desktopTileDisabled) {
                if (!m_wallpaperCapture->HasCachedFrame())
                    m_wallpaperCapture->GetCurrentFrame();
                if (!m_wallpaperCapture->HasCachedFrame())
                    allReady = false;
            }
            if (allReady)
                break;
            QueryPerformanceCounter(&w1);
            double elapsedMs = static_cast<double>(w1.QuadPart - w0.QuadPart)
                             * 1000.0 / static_cast<double>(wf.QuadPart);
            // Out of budget.  NOT a diagnostic: this loop is a best-effort
            // wait, and the fallback below is the designed answer to it — the
            // default budget is one compositor tick, so a capture that has not
            // delivered yet on the first activation of a session is the
            // ordinary case, not a fault.  Reporting here fired on every first
            // activation on every machine, which is exactly the noise that
            // teaches people to stop reading the log.
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
    // Wallpaper backdrop capture (desktop tile disabled) needs a frame too.
    if (m_wallpaperCapture && m_desktopTileDisabled
        && !m_wallpaperCapture->HasCachedFrame()) {
        m_wallpaperCapture->GetCurrentFrame();
        if (!m_wallpaperCapture->HasCachedFrame())
            m_wallpaperCapture->EnsureFrame();
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

    // Only NOW is an empty capture worth reporting.  The warm-up budget
    // expiring means nothing on its own — the DwmThumbnail and PrintWindow
    // fallbacks above exist precisely for the captures it leaves behind.  A
    // capture that has no frame after all three have been tried is a tile that
    // will open as a blank placeholder, which is a thing the user can see.
    {
        size_t blank = 0;
        for (auto& cap : m_captures)
            if (cap && !cap->HasCachedFrame()) ++blank;
        if (blank > 0) {
            wchar_t detail[288];
            _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                L"%zu of %zu tiles had no picture after live capture, the "
                L"thumbnail fallback and PrintWindow had all been tried; they "
                L"open as blank placeholders. Windows refuses previews of "
                L"protected content, and some minimised windows keep nothing "
                L"to copy",
                blank, m_captures.size());
            Diag::Report(Diag::Code::CaptureWarmupTimeout, Diag::Sev::Warning,
                         L"Some windows could not be pictured in the cascade",
                         detail);
        }
    }

    // Resolve the taskbar content band so the draw can UV-crop to the
    // real taskbar.  On Win10 / Win11 24H2 the WGC Shell_TrayWnd capture is
    // far taller than the bar with the content in only a thin band; this
    // measures where that band is instead of guessing.  Falls back to the
    // full texture when no content band is found (e.g. Win11 25H2 where the
    // capture is already bar-sized).
    // The bar's own height is handed over as the sanity check: a detected
    // band much taller than that is something else that got composited into
    // the capture, and cropping around its centre draws the wrong slice —
    // the "taskbar looks doubled" artefact.  See DetectContentCenterV.
    m_taskbarContentResolved = false;
    if (m_taskbarCapture && m_taskbarCapture->HasCachedFrame()) {
        const int barPx =
            static_cast<int>(m_taskbarRect.bottom - m_taskbarRect.top);
        m_taskbarContentResolved =
            m_taskbarCapture->DetectContentCenterV(m_taskbarContentCenterY, barPx);
    }
    for (auto& tray : m_secondaryTrays) {
        tray.contentResolved = false;
        tray.contentCenterY = 0.5f;
        if (tray.capture && tray.capture->HasCachedFrame()) {
            const int barPx =
                static_cast<int>(tray.rectOverlay.bottom - tray.rectOverlay.top);
            tray.contentResolved =
                tray.capture->DetectContentCenterV(tray.contentCenterY, barPx);
        }
    }

    // Taskbar live preview eligibility — pure user opt-in.  Decided BEFORE
    // HideRealTaskbar so the hide step can hold the bar visible for the
    // live stream (a hidden Shell_TrayWnd stops delivering WGC frames,
    // which is exactly the frozen-clock symptom).
    //
    // There is deliberately no "the capture looks tall, so fall back to the
    // frozen snapshot" rule here.  Every draw site UV-crops live frames
    // through the same ComputeTaskbarContentBandUV path as the frozen
    // snapshot, so a tall capture renders identically either way, and any
    // such rule would only make the option permanently dead on the very
    // Windows builds that produce tall captures.
    m_taskbarLiveActive = m_config && m_config->taskbarLivePreview
        && m_taskbarCapture != nullptr;
    // Asked for a live taskbar and got no capture to make one from: the
    // overlay shows the frozen pre-hide frame instead, which is a clock that
    // never moves — the exact symptom, said out loud.
    if (m_config && m_config->taskbarLivePreview && m_config->taskbarPreview
        && !m_taskbarCapture)
        Diag::ReportOnce(Diag::Code::TaskbarCaptureOdd, Diag::Sev::Warning,
                         L"The taskbar preview cannot be live on this system",
                         L"Windows would not hand over a capture of the taskbar, "
                         L"so the overlay is drawing the frozen frame taken just "
                         L"before it hid the real one");
    for (auto& tray : m_secondaryTrays)
        tray.liveActive = m_config && m_config->taskbarLivePreview
            && tray.capture != nullptr;
    if (m_config && m_config->showDebugInfo && m_taskbarCapture) {
        int texW = 0, texH = 0;
        m_taskbarCapture->GetCapturedSize(texW, texH);
        wchar_t buf[160];
        swprintf_s(buf,
            L"CKFlip TB-LIVE: live=%d tex=%dx%d barH=%ld contentResolved=%d centerY=%.3f\n",
            m_taskbarLiveActive ? 1 : 0, texW, texH,
            m_taskbarRect.bottom - m_taskbarRect.top,
            m_taskbarContentResolved ? 1 : 0, m_taskbarContentCenterY);
        CKLog::Log(buf);
    }

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
    // Read the taskbar debug mode and snapshot the pre-hide state BEFORE
    // HideRealTaskbar() runs, so `nohide`/`freeze`
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
        m_quad.ResetStateCache();
        m_quad.SetAntialiasing(EffectiveAntialiasing());
        auto* ctx = m_renderer.GetContext();
        float cascadeAspect = m_cascadeAspect;
        DirectX::XMMATRIX monRemap =
            DirectX::XMLoadFloat4x4(&m_monRemapNDC);
        uint32_t count = m_scene.SlotCount();

        // The opaque backdrop is BeginFrame's clear — see Renderer::BeginFrame.

        // Wallpaper background.  Source = Progman/WorkerW WGC capture —
        // the desktop tile's capture, or the dedicated wallpaper capture
        // when the desktop tile is disabled (WallpaperCaptureSource).
        // Drawn via DrawWallpaper() which uses a PS that fills any
        // transparent strip in the texture (Win11 < 25H2 leaves an α=0
        // band where the taskbar lives) by sampling the closest opaque
        // pixel above.  No-op on Win11 25H2 where the capture is fully
        // opaque.  Wallpaper-Engine and other dynamic-wallpaper apps
        // route their content through Progman, so this preserves them.
        {
            // Same live/static resolution as RenderFrame Layer 1 — in
            // static mode this is also where the session's owned snapshot
            // gets created, from the warm-up frame.
            ID3D11ShaderResourceView* srv = BackdropSRV();
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
                // Background blur intensity (config backgroundBlur %, 0 =
                // off → single-sample shader path, no extra cost).
                bgDraw.blurAmount = m_config
                    ? static_cast<float>(m_config->backgroundBlur) / 100.0f
                    : 0.0f;
                m_quad.DrawWallpaper(ctx, srv, bgDraw);
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
                    // Taskbar UV crop to the MEASURED content band via the
                    // shared helper (same crop as RenderFrame Layer 2 and
                    // the debug dump).
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
        // Stable for the same reason as the render loop's sort: Cover Flow's
        // mirrored slots tie exactly, and an unspecified order among them is
        // an unspecified first frame.
        std::stable_sort(initOrder.begin(), initOrder.end(),
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
                    XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY() + slot.rotY)) *
                    XMMatrixTranslation(slot.x, slot.y, slot.z);
                XMMATRIX view, proj;
                m_scene.CameraMatrices(cascadeAspect, view, proj);

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
            const uint32_t wi = SlotWindowIndex(i);
            if (wi >= static_cast<uint32_t>(m_captures.size())) continue;

            QuadDrawCall draw;
            float alpha = m_scene.GetSlot(i).alpha;
            if (alpha < 0.001f) continue;

            ID3D11ShaderResourceView* srv = m_captures[wi] ? m_captures[wi]->GetCurrentFrame() : nullptr;
            m_scene.GetDrawCall(i, cascadeAspect, draw.mvp, alpha);
            DirectX::XMMATRIX perspMVP =
                DirectX::XMLoadFloat4x4(&draw.mvp) * monRemap;
            DirectX::XMStoreFloat4x4(&draw.mvp, perspMVP);
            if (wi < m_windows.size()) {
                const RECT& morphRect =
                    ResolveMorphScreenRect(m_entryExitAnimator, m_windows, i, wi);
                DirectX::XMMATRIX screenMVP =
                    ComputeScreenSpaceMVP(morphRect, vpW, vpH);
                DirectX::XMStoreFloat4x4(&draw.mvp,
                    LerpMatrix(screenMVP, perspMVP,
                               m_entryExitAnimator.GetMorphBlend()));
            }
            draw.alpha = alpha;
            draw.blurAmount = 0.0f;
            if (wi < m_windows.size() && m_windows[wi].hwnd == m_desktopHwnd)
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

    // 13. Hide the real taskbar.  The desktop ICONS are deliberately left
    // alone — see the note further down for what hiding them cost and never
    // bought.
    DwmFlush();
    HideRealTaskbar();

#ifdef CKFLIP_DEBUG_TASKBAR
    // Post-hide dump.  Give DWM a frame to refresh the capture
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

    // A death from here on takes the overlay, the cloaked windows and the
    // hidden taskbar with it, and looks nothing like one that happens while the
    // process sits in the tray — so the marker says which it was (Diag::
    // NoteState).  Once per activation, never per frame.
    Diag::NoteState(L"cascade open");
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

    // A click-to-select spin owns the stack until it lands on its window;
    // stepping it further would leave the spin chasing a moving target.
    if (m_jumpTargetHwnd)
        return;

    if (m_cycleAnim.IsActive()) {
        // Queue instead of blocking — creates smooth continuous motion.
        if (m_cycleQueue.size() < kMaxQueueSize)
            m_cycleQueue.push_back(true);
        return;
    }

    // A raised tile comes down as the stack sets off, not before it (see
    // DropHoverLift) — the cycle starts on this frame either way.
    DropHoverLift();

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
    if (m_jumpTargetHwnd)
        return;                             // spin in flight — see Cycle()

    if (m_cycleAnim.IsActive()) {
        if (m_cycleQueue.size() < kMaxQueueSize)
            m_cycleQueue.push_back(false);
        return;
    }

    DropHoverLift();            // see Cycle()

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
        m_hover.Rotate(true);   // the lift belongs to a window, not to a slot
        RebuildSceneAspects();
        return;
    }

    // A click-to-select spin fixes its own per-step duration (see
    // AdvanceSelectJump); every other cycle keeps the built-in timing.
    const float stepMs = m_jumpTargetHwnd ? m_jumpStepMs : 0.0f;
    if (m_scrubPending) m_cycleAnim.BeginScrub(m_scene, true);
    else                m_cycleAnim.Begin(m_scene, true, chained, stepMs);

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
    if (WGCCapture* wallCap = WallpaperCaptureSource())
        m_frozenDesktopSRV = SrvRef(wallCap->GetCurrentFrame());
    if (!m_frozenTaskbarSRV && m_taskbarCapture)
        m_frozenTaskbarSRV = SrvRef(m_taskbarCapture->GetCurrentFrame());
    m_sessionFrozen = true;

    // Rotate cached per-window aspects/scales to match the window rotation.
    // Then immediately rebuild them from actual window rects to ensure they're
    // tied to the correct windows, not slot positions. This prevents aspect
    // ratio leakage when windows wrap around visible slots.
    m_scene.RotateAspects(true);
    // Same rotation for the pointer-hover lift: it is a per-SLOT draw offset,
    // and slot i now shows the window that will END there, so a fall in flight
    // has to travel with its own tile.  Left un-rotated, the tile that was
    // never raised is the one seen sinking (see HoverAnimator::Rotate).
    m_hover.Rotate(true);
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
        m_hover.Rotate(false);  // the lift belongs to a window, not to a slot
        RebuildSceneAspects();
        return;
    }

    const float stepMs = m_jumpTargetHwnd ? m_jumpStepMs : 0.0f;
    if (m_scrubPending) m_cycleAnim.BeginScrub(m_scene, false);
    else                m_cycleAnim.Begin(m_scene, false, chained, stepMs);

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
    if (WGCCapture* wallCap = WallpaperCaptureSource())
        m_frozenDesktopSRV = SrvRef(wallCap->GetCurrentFrame());
    if (!m_frozenTaskbarSRV && m_taskbarCapture)
        m_frozenTaskbarSRV = SrvRef(m_taskbarCapture->GetCurrentFrame());
    m_sessionFrozen = true;

    // Rotate cached per-window aspects/scales to match the window rotation.
    // Then immediately rebuild them from actual window rects to ensure they're
    // tied to the correct windows, not slot positions. This prevents aspect
    // ratio leakage when windows wrap around visible slots.
    m_scene.RotateAspects(false);
    m_hover.Rotate(false);      // see ExecuteCycleForward
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
// Free stack movement — Window snap off (config windowSnap).
//
// A scrub is the ordinary cycle transition with the pointer holding its
// parameter.  Walking past either end of the in-flight step commits it and
// opens the next one in the same direction, so one long drag flows through
// the whole stack; falling back below a step's own start pose rolls that
// step's array rotation away again and turns around.  Everything else — the
// wrap phases, the frozen textures, the Cover Flow side swap — is the
// unchanged cycle path, which is exactly the point.
// ---------------------------------------------------------------------------
void FlipController::Scrub(float windows)
{
    if (!m_active || m_windows.size() < 2 || windows == 0.0f)
        return;
    if (!m_config || m_config->windowSnap)
        return;                             // snapping: discrete steps only
    // The morphs own the scene's slot state while they run.
    if (m_entryExitAnimator.IsActive() || m_closeAnim.IsActive())
        return;
    if (m_jumpTargetHwnd)
        return;                             // spin in flight — see Cycle()

    if (!AnimCycleEnabled()) {
        // No cycle animation to scrub through — fall back to whole steps so
        // the drag still moves the stack.
        while (windows >= 1.0f) { ExecuteCycleForward();  windows -= 1.0f; }
        while (windows <= -1.0f) { ExecuteCycleBackward(); windows += 1.0f; }
        return;
    }

    // A timed cycle from the wheel or the keyboard is landing — let it.
    if (m_cycleAnim.IsActive() && !m_cycleAnim.IsScrubbing())
        return;

    // Grabbing the stack mid-throw stops the throw dead, as catching a
    // spinning wheel should.
    m_flinging = false;

    // A raised tile goes down here too, alongside the first fraction of the
    // drag — same reason, same mechanism as a cycle (see DropHoverLift).
    DropHoverLift();

    // ---- Velocity, for the throw on release -------------------------------
    // Pointer deltas arrive in bursts (several posted messages drained in one
    // pass), so measure over a short window instead of per message — a
    // per-message dt would read as either infinity or zero.
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        if (m_scrubSampleQPC.QuadPart == 0) {
            m_scrubSampleQPC = now;
            m_scrubPendingDist = 0.0f;
            m_scrubVelocity = 0.0f;
        }
        m_scrubPendingDist += windows;
        double dt = static_cast<double>(now.QuadPart - m_scrubSampleQPC.QuadPart)
                  / static_cast<double>(freq.QuadPart);
        if (dt >= 0.008) {
            if (dt > 0.20) {
                // Long pause: the hand stopped, so the stack should too.
                m_scrubVelocity = 0.0f;
            } else {
                const float inst = static_cast<float>(m_scrubPendingDist / dt);
                m_scrubVelocity += 0.45f * (inst - m_scrubVelocity);
            }
            m_scrubPendingDist = 0.0f;
            m_scrubSampleQPC = now;
        }
    }

    ScrubAdvance(windows);
}

void FlipController::ScrubAdvance(float windows)
{
    float remaining = windows;
    for (int guard = 0; guard < 64 && remaining != 0.0f; ++guard) {
        if (!m_scrubActive) {
            m_scrubForward = remaining > 0.0f;
            m_scrubPending = true;
            if (m_scrubForward) ExecuteCycleForward(false);
            else                ExecuteCycleBackward(false);
            m_scrubPending = false;
            if (!m_cycleAnim.IsScrubbing())
                return;                     // animation disabled underneath us
            m_scrubActive = true;
            m_scrubT      = 0.0f;
        }

        // Inside the step, t always grows along the step's own direction.
        const float t = m_scrubT + (m_scrubForward ? remaining : -remaining);

        if (t >= 1.0f) {
            m_cycleAnim.SetScrubT(m_scene, 1.0f);
            m_cycleAnim.Cancel();           // pose already equals the rest pose
            remaining     = m_scrubForward ? (t - 1.0f) : -(t - 1.0f);
            m_scrubActive = false;
            continue;
        }
        if (t < 0.0f) {
            m_cycleAnim.SetScrubT(m_scene, 0.0f);
            m_cycleAnim.Cancel();
            ScrubUndoStep(m_scrubForward);
            remaining     = m_scrubForward ? t : -t;
            m_scrubActive = false;
            continue;
        }

        m_scrubT = t;
        m_cycleAnim.SetScrubT(m_scene, m_scrubT);
        remaining = 0.0f;
    }
}

void FlipController::ScrubEnd()
{
    m_scrubSampleQPC.QuadPart = 0;
    m_scrubPendingDist = 0.0f;
    if (!m_scrubActive)
        return;

    // Let go of a stack that was moving and it keeps going, shedding speed
    // under friction — the throw.  Only a hand that had genuinely stopped
    // hands over to the short settle instead.
    const float v = std::clamp(m_scrubVelocity, -kFlingMaxVel, kFlingMaxVel);
    if (std::fabs(v) >= kFlingMinVel && m_cycleAnim.IsScrubbing()) {
        m_scrubVelocity = v;
        m_flinging = true;
        QueryPerformanceCounter(&m_flingLastQPC);
        return;                             // ScrubTickFling takes it from here
    }

    m_scrubActive = false;
    if (m_cycleAnim.IsScrubbing())
        m_cycleAnim.BeginSettle(ScrubSettleTarget(), ScrubStepVelocity());
    m_scrubVelocity = 0.0f;
}

// ---------------------------------------------------------------------------
// The scrub's speed expressed along the CURRENT step's own direction, which
// is the frame BeginSettle reasons in: m_scrubVelocity is signed in windows
// (forward positive), while a step's t always grows the way that step runs.
float FlipController::ScrubStepVelocity() const
{
    return m_scrubForward ? m_scrubVelocity : -m_scrubVelocity;
}

// ---------------------------------------------------------------------------
// Where a released or spent scrub comes to rest.
//
// Nearest-window is the rule, with one exception that matters: a step already
// visibly under way and still moving FORWARD finishes rather than rewinding.
// That reversal is the jolt the free-drag stop had — the stack coasts past a
// window, the throw runs out of speed a fraction into the next step, and the
// settle drags it backwards to where it just came from.  Carrying it through
// is both smoother and what the hand asked for; either way it lands squarely
// on a whole window, so the snap the feature exists for is untouched.
float FlipController::ScrubSettleTarget() const
{
    if (m_scrubT >= 0.5f)
        return 1.0f;
    const float v = ScrubStepVelocity();
    if (m_scrubT >= kSettleCarryT && v >= kSettleCarryVel)
        return 1.0f;
    return 0.0f;
}

// ---------------------------------------------------------------------------
// One frame of a thrown stack: shed speed, move by whatever that speed
// covered, and hand over to the settle once it is down to a crawl.
// ---------------------------------------------------------------------------
void FlipController::ScrubTickFling()
{
    if (!m_flinging)
        return;
    if (!m_active || !m_cycleAnim.IsScrubbing()) {
        m_flinging = false;
        return;
    }

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dt = static_cast<float>(
        static_cast<double>(now.QuadPart - m_flingLastQPC.QuadPart)
        / static_cast<double>(freq.QuadPart));
    m_flingLastQPC = now;
    if (dt <= 0.0f) return;
    if (dt > 0.10f) dt = 0.10f;             // a stall must not teleport the stack

    // Exponential friction: fast throws coast further, everything comes to
    // rest in about the same feel.
    const float decay = std::exp(-dt / kFlingTauSec);
    const float vAvg  = m_scrubVelocity * (1.0f + decay) * 0.5f;
    m_scrubVelocity  *= decay;

    ScrubAdvance(vAvg * dt);

    if (std::fabs(m_scrubVelocity) < kFlingStopVel) {
        m_flinging      = false;
        m_scrubActive   = false;
        // The throw is spent but the stack is still moving at kFlingStopVel —
        // hand that speed to the settle so the last window is eased into
        // rather than jumped to.  See CycleAnimator::BeginSettle.
        if (m_cycleAnim.IsScrubbing())
            m_cycleAnim.BeginSettle(ScrubSettleTarget(), ScrubStepVelocity());
        m_scrubVelocity = 0.0f;
    }
}

void FlipController::ScrubUndoStep(bool wasForward)
{
    // Called at t = 0, where the stack already renders the pre-rotation
    // pose — so rolling the arrays back and rebuilding the rest layout is
    // invisible, it only re-aligns the bookkeeping with the picture.
    if (wasForward) {
        std::rotate(m_windows.rbegin(),  m_windows.rbegin()  + 1, m_windows.rend());
        std::rotate(m_captures.rbegin(), m_captures.rbegin() + 1, m_captures.rend());
        m_scene.RotateAspects(false);
        m_hover.Rotate(false);  // undo travels with everything else
    } else {
        std::rotate(m_windows.begin(),  m_windows.begin()  + 1, m_windows.end());
        std::rotate(m_captures.begin(), m_captures.begin() + 1, m_captures.end());
        m_scene.RotateAspects(true);
        m_hover.Rotate(true);
    }
    m_cycleAnim.Cancel();

    uint32_t displayCount = static_cast<uint32_t>(m_windows.size());
    if (m_config && m_config->maxWindows < displayCount)
        displayCount = m_config->maxWindows;
    m_scene.BuildSlots(displayCount, m_cascadeW, m_cascadeH);
    RebuildSceneAspects();
}

void FlipController::ResolveScrub()
{
    m_flinging = false;
    m_scrubVelocity = 0.0f;
    if (!m_cycleAnim.IsScrubbing()) {
        m_scrubActive = false;
        return;
    }
    const bool commitStep = m_scrubT >= 0.5f;
    m_cycleAnim.SetScrubT(m_scene, commitStep ? 1.0f : 0.0f);
    m_cycleAnim.Cancel();
    if (!commitStep)
        ScrubUndoStep(m_scrubForward);
    m_scrubActive = false;
    m_scrubT      = 0.0f;
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

    // A click-to-select spin is in flight: the user has already told us WHICH
    // window they want, so releasing the trigger cannot mean "take whatever
    // is in front right now".  Mark the commit and let the spin land — the
    // window transition always plays before the exit.
    if (m_jumpTargetHwnd) {
        m_jumpCommit = true;
        // The cascade is still up, still moving, and still the thing the user
        // is looking at — so the hook has to go on treating it as a session.
        // Releasing the modifier is what got us here, and that release already
        // dropped the hook's flag; without this the spin ran for up to
        // kJumpBudgetMs with Escape and Tab doing nothing and typed keys
        // reaching the app behind the overlay.  ResumeSession is exactly the
        // "lend it back" this needs — it deliberately keeps the per-session
        // state (the search latch above all) intact.
        KeyboardHook::ResumeSession();
        return;
    }

    // An exit is already committed and only waiting for the search filter's
    // windows to finish returning.  A stray dismiss arriving in that window
    // (the modifier being released, say) must not overtake it and start a
    // second one — the pending exit already knows what was chosen, and
    // whether it was a commit or a cancel.
    if (m_pendingExit != PendingExit::None)
        return;

    // A query that matched nothing leaves NOTHING to switch to, so the commit
    // simply does not apply and the cascade stays open.  Exiting from here
    // would mean an entry/exit morph over zero tiles — a degenerate case the
    // morph should never be handed — and it would drop the user back on the
    // desktop for a keystroke they meant as "pick this".  Backspace or the
    // cancel key is the way out, and both bring the windows straight back.
    if (m_windows.empty()) {
        // The hook let go of the session when the commit key arrived; the
        // cascade is in fact still up, so hand it back.
        KeyboardHook::ResumeSession();
        return;
    }

    // Committing mid-drag: land the stack on a whole window first, so the
    // window that gets raised is the one actually in front.
    ResolveScrub();

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

    // The exit morph is about the WHOLE session, not about what the search
    // narrowed it to: every window it animates has to be there, in an order
    // that matches the desktop it is flying back to.  So the filter is lifted
    // first — the windows that were hidden rise back in around the chosen
    // one, in the same order scrolling to it by hand would have left them —
    // and the morph waits for them.  See RestoreSearchWindowsForExit.
    if (RestoreSearchWindowsForExit()) {
        m_pendingExit = PendingExit::Dismiss;
        // The cascade is genuinely still up for the length of that arrival,
        // so the hook — which let go of the session when the commit key
        // arrived — gets it back until the exit really starts.
        KeyboardHook::ResumeSession();
        return;
    }
    m_pendingExit = PendingExit::None;

    // The session ends HERE, and the hook has to hear it.
    //
    // The keyboard paths tell it themselves before they post (the commit key,
    // the modifier release), but a POINTER commit cannot: the hook sees a
    // click, not a decision — it has no idea whether that click hit a tile.
    // So a mouse-picked window tore the cascade down while the hook still
    // believed a session was running, and it went on swallowing every
    // keystroke for the rest of the process's life.  Same limbo as the
    // mid-exit re-activation, reached from the other side.
    //
    EndSessionForHook();

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

    // Endpoint Z ranks for the flat endpoint.
    //   - Sized to visibleN (matches m_flatSlots.size() in BeginExit, NOT
    //     m_windows.size()).
    //   - Selected window (index 0) goes to rank 0 (top of flat endpoint).
    //   - Remaining visible slots sorted by raw original Z-order, then
    //     dense-ranked 0..visibleN-1.
    uint32_t visibleN = std::min<uint32_t>(
        m_scene.SlotCount(),
        static_cast<uint32_t>(m_windows.size()));

    // The exit morph pairs its window list index i with cascade slot i —
    // hand it the slot-ordered permutation (an exact copy of m_windows
    // for the cascade preset).  zRanks and fadeOutFlags below index the
    // same slot order.
    const std::vector<WindowInfo> orderedWins = SlotOrderedWindows();

    std::vector<uint32_t> zRanks(visibleN, 0);

    if (visibleN > 0) {
        zRanks[0] = 0;   // selected after Dismiss is top

        auto rawRankOf = [&](size_t i) -> uint32_t {
            HWND h = orderedWins[i].hwnd;
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
            HWND h = orderedWins[i].hwnd;
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
    // window" artefact, and covers the desktop-selected case with the same
    // rule rather than a special one.
    std::vector<bool> fadeOutFlags(orderedWins.size(), false);
    for (size_t i = 0; i < orderedWins.size(); ++i) {
        if (i == 0) continue;                          // selected stays visible
        HWND h = orderedWins[i].hwnd;
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

    m_entryExitAnimator.BeginExit(m_scene, orderedWins, zRanks,
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

    // Everything below asks Windows to change what is in front — the shell's
    // show-desktop, or SetForegroundWindow — and Windows only grants that to
    // a process that has just received input.  CKFlip normally has not: the
    // overlay is WS_EX_NOACTIVATE and the hook SWALLOWS every key the cascade
    // uses, so the only reason a keyboard commit ever qualified is that the
    // hook happens to inject a keystroke on its way out of a held-modifier
    // release, and injected input counts.
    //
    // A mouse pick injects nothing.  The request was therefore refused and
    // the desktop never appeared, while an ordinary window still ended up in
    // front because FinishDismiss retries SetForegroundWindow — a retry the
    // desktop deliberately cannot have, since its toggle would undo itself.
    // Hence "clicking the desktop tile does nothing, the keyboard is fine".
    //
    // Claiming it here, explicitly and for every commit source, also closes
    // the race the keyboard path always had: the hook POSTS the dismiss and
    // only then injects, so a quick render thread could reach this block
    // first.
    KeyboardHook::AssertInputOwnership();

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
            if (m_config && m_config->showDebugInfo) {
                wchar_t buf[96];
                swprintf_s(buf, L"CKFlip: show-desktop via IShellDispatch=%d\n",
                           toggled ? 1 : 0);
                CKLog::Log(buf);
            }
            if (!toggled) {
                // Timed, not blocking.  This runs on the render thread in the
                // middle of the dismiss, and a plain SendMessage into
                // explorer stalls the whole switcher for as long as explorer
                // takes to answer — indefinitely if it is hung.  ABORTIFHUNG
                // returns at once from a hung shell instead, and the switch
                // simply does not happen rather than taking the overlay down
                // with it.  Same short-timeout discipline the label's
                // WM_GETICON fallback already uses.
                HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
                if (tray) {
                    DWORD_PTR unused = 0;
                    SendMessageTimeoutW(tray, WM_COMMAND, 419, 0,
                                        SMTO_ABORTIFHUNG | SMTO_NORMAL,
                                        400, &unused);
                }
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

    // Search first: one Escape clears the query and puts the filtered-out
    // windows back, a second one closes the cascade.  Nobody expects a
    // half-typed search to swallow their way out, and nobody expects the way
    // out to leave the search applied either.
    //
    // The hook has already dropped the session flag (it owns the modifier-
    // release bookkeeping that keeps the Start menu shut, and that has to
    // happen on the hook thread) — so re-arm it here, because the cascade is
    // in fact staying open.
    if (m_config && m_config->searchEnabled && !m_search.Empty()) {
        SearchClear();
        KeyboardHook::ResumeSession();
        return;
    }

    // A spin toward a clicked window is abandoned outright: Escape means
    // "none of them", including the one just clicked.
    if (m_jumpTargetHwnd)
        CancelSelectJump();

    ResolveScrub();

    // An empty stack has nothing to morph back to its desktop position — a
    // query that matched no window, now being dismissed.  Tear the session
    // down directly rather than running the exit over zero tiles.
    if (m_windows.empty()) {
        m_exitPending         = false;
        m_reverseDelayPending = false;
        FinishEscape();
        return;
    }

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

    // Same reason as Dismiss: the reverse morph has to carry every window of
    // the session back to its desktop position, not just the ones the query
    // left on screen — and it waits for them to get there.
    if (RestoreSearchWindowsForExit()) {
        m_pendingExit = PendingExit::Escape;
        KeyboardHook::ResumeSession();
        return;
    }
    m_pendingExit = PendingExit::None;

    // See Dismiss — the session is over and the hook must know it, whichever
    // input asked for the cancel.
    EndSessionForHook();

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

    // Escape passes an empty zRanks vector so the endpoint-Z override is
    // skipped: a cancel puts every window back where it was, untouched.
    std::vector<uint32_t> escZRanks;  // empty → override skipped

    // Escape is a pure cancel — every tile reverses cleanly to its entry
    // flat position; no per-tile fade-out (the empty vector signals that).
    std::vector<bool> escFadeOut;  // empty
    m_entryExitAnimator.BeginExit(m_scene, SlotOrderedWindows(), escZRanks,
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
// FinishDismiss — the actual switching, run by the render loop once the exit
// morph completes.  Brings the selected window forward, or triggers the
// Show-Desktop toggle.
// ---------------------------------------------------------------------------
void FlipController::FinishDismiss()
{
    m_active = false;
    m_cycleQueue.clear();
    m_reverseDelayPending = false;
    StopIconResolver();

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
            // The one thing the whole program exists to do.  Windows grants
            // foreground only under conditions this process does not fully
            // control, and when it refuses, the cascade closes onto the window
            // the user did NOT pick — with nothing anywhere saying why.
            //
            // Both attempts are made before anything is recorded (see
            // RaiseWindowForCommit): the entry is for a window that is really
            // still behind, not for one call that answered FALSE.
            if (!RaiseWindowForCommit(target))
                Diag::ReportLastError(Diag::Code::ForegroundRestoreFail,
                                      Diag::Sev::Warning,
                                      L"Windows refused to bring the chosen window forward",
                                      L"the cascade closed on the right window but "
                                      L"the system kept focus where it was; this is "
                                      L"the foreground lock, usually held by a "
                                      L"window that is busy or elevated, or by a "
                                      L"shell surface such as the Start menu");
        }
    }

    DwmFlush();

    StopCaptures();
    // Stop (don't destroy) the dedicated wallpaper capture — its cached
    // frame is the warm start for the next tile-disabled session.
    if (m_wallpaperCapture) m_wallpaperCapture->Stop();
    ResetSelectedLabel();
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
    m_staticBackdropTexture = nullptr;
    m_staticBackdropSRV     = nullptr;
    m_exitSelectedStableSRV = nullptr;
    m_exitSelectedStableTexture = nullptr;
    m_exitSelectedStableHwnd = nullptr;
    m_sessionFrozen = false;

    m_renderer.Hide();
    // The taskbar comes back on THIS side of the hide: it is topmost, so
    // showing it while the overlay is still up would pop it over the cascade.
    ShowRealTaskbar();
    m_secondaryTrays.clear();

    m_windows.clear();
    m_desktopHwnd = nullptr;
    m_activatedOnDesktop = false;
    m_taskbarDrawOnTop = false;
    m_taskbarLocator.Shutdown();
    m_entryExitAnimator.ClearEntryFlatCache();
    m_originalZOrder.clear();   // session-end cleanup
    // Pointer / search state, released with everything else the session owned
    // (ClearSearchState stops the hidden windows' captures too).
    m_hover.Reset();
    m_hoverSlot    = -1;
    m_hoverStillQPC.QuadPart = 0;   // the next session's stillness is its own
    m_pointerValid = false;
    CancelSelectJump();
    ClearSearchState();
    m_pendingExit = PendingExit::None;
#ifdef CKFLIP_DEBUG_TASKBAR
    g_taskbarFreezeSRV = nullptr;
    g_taskbarDebugMode = TaskbarDebugMode::Normal;
#endif
    // INVARIANT, enforced last: an idle controller and a hook that still
    // believes a session is running is the limbo state — it swallows every
    // keystroke, Windows key included, until the process dies.  Two separate
    // paths have already produced it (a re-activation during the exit, a
    // pointer commit), so rather than keep patching call sites, the teardown
    // itself now guarantees the two agree.
    //
    // The one exception is a re-activation waiting to be honoured: there the
    // hook's flag belongs to the NEXT session and clearing it would strand
    // that one instead.  ResolveReactivation owns it from here.
    //
    // ...and m_reactivatePending only catches re-activations DELIVERED before
    // this teardown began.  Everything above (DwmFlush, StopCaptures,
    // UncloakAll, ShowRealTaskbar) runs without pumping messages, so a hotkey
    // pressed during it raises the hook's flag for a session whose activation
    // is still sitting in the queue.  Clearing unconditionally would switch
    // that session off before it ever ran: cascade on screen, hook disarmed,
    // every key falling through to whatever is behind the overlay.  Handing
    // back the identity taken at Activate makes the clear a no-op in exactly
    // that case and unchanged in every other.
    if (!m_reactivatePending)
        KeyboardHook::EndSessionIfEpoch(m_sessionEpoch);
    // Back to waiting for a hotkey — see the note at the end of Activate.
    // Before ResolveReactivation, which may open the next cascade and set the
    // state straight back.
    Diag::NoteState(L"idle in the tray");
    ResolveReactivation();
}

// ---------------------------------------------------------------------------
// FinishEscape — teardown after the exit morph completes.  No foreground call
// and no show-desktop: Escape is a pure cancel.
// ---------------------------------------------------------------------------
void FlipController::FinishEscape()
{
    m_active = false;
    m_cycleQueue.clear();
    m_reverseDelayPending = false;
    StopIconResolver();
    // Cancel any in-flight cycle (see FinishDismiss for rationale).
    m_cycleAnim.Cancel();
    m_closeAnim.Cancel();
    ClearClosingCaptures();

    WindowCloaker::UncloakAll();
    DwmFlush();

    StopCaptures();
    // See FinishDismiss — warm-cached wallpaper capture, label teardown.
    if (m_wallpaperCapture) m_wallpaperCapture->Stop();
    ResetSelectedLabel();
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
    m_staticBackdropTexture = nullptr;
    m_staticBackdropSRV     = nullptr;
    m_exitSelectedStableSRV = nullptr;
    m_exitSelectedStableTexture = nullptr;
    m_exitSelectedStableHwnd = nullptr;
    m_sessionFrozen = false;
    m_renderer.Hide();

    // The taskbar is topmost and has to wait for the overlay to be gone.
    ShowRealTaskbar();
    m_secondaryTrays.clear();

    m_windows.clear();
    m_desktopHwnd = nullptr;
    m_activatedOnDesktop = false;
    m_taskbarDrawOnTop = false;
    m_taskbarLocator.Shutdown();
    m_entryExitAnimator.ClearEntryFlatCache();
    m_originalZOrder.clear();   // session-end cleanup
    // Pointer / search state, released with everything else the session owned
    // (ClearSearchState stops the hidden windows' captures too).
    m_hover.Reset();
    m_hoverSlot    = -1;
    m_hoverStillQPC.QuadPart = 0;   // the next session's stillness is its own
    m_pointerValid = false;
    CancelSelectJump();
    ClearSearchState();
    m_pendingExit = PendingExit::None;
#ifdef CKFLIP_DEBUG_TASKBAR
    g_taskbarFreezeSRV = nullptr;
    g_taskbarDebugMode = TaskbarDebugMode::Normal;
#endif
    // See FinishDismiss — the same invariant, enforced last for the same
    // reasons: an idle controller must never leave the hook believing a
    // session is still running, and the epoch is what makes the clear a no-op
    // for a re-activation already on its way.
    if (!m_reactivatePending)
        KeyboardHook::EndSessionIfEpoch(m_sessionEpoch);
    // Back to waiting for a hotkey — see the note at the end of Activate.
    // Before ResolveReactivation, which may open the next cascade and set the
    // state straight back.
    Diag::NoteState(L"idle in the tray");
    ResolveReactivation();
}

// ---------------------------------------------------------------------------
// Honour an activation that arrived while the previous session was morphing
// out (see Activate).  Called at the very end of both teardown paths, once
// everything the old session owned has been released.
//
// The hook's session flag decides whether it still stands: a cancel key
// pressed during the exit clears that flag, and re-opening the cascade after
// the user has said no is exactly the surprise this avoids.  If the fresh
// activation then finds nothing to show, the flag has to be dropped too —
// otherwise the hook goes on swallowing input for a cascade that never
// opened, which is the same limbo from the other direction.
// ---------------------------------------------------------------------------
// Tell the hook the session is over — and, when the request did not come from
// the keyboard, arm the Win/Alt release defusal it could not arm itself.
//
// WHICH IT IS, IS DERIVED, NOT REMEMBERED.  The hook drops its own session
// flag before posting a keyboard commit or cancel; nothing drops it for a
// click on a tile or a touchpad tap, because the hook cannot see a decision
// in a click.  So "the hook still thinks a session is running" IS the test
// for a non-keyboard commit, and it is one the code cannot get out of step
// with.  Carrying a boolean from the click down to here instead is what this
// shape rules out: such a flag gets lost on the way, and did, cleared by
// CancelSelectJump one line before the commit that needed it.
//
// Getting it wrong the other way matters too: re-deciding "is a modifier
// still held" milliseconds after the hook already decided it races the
// release the hook has just replayed through SendInput, and a false positive
// there eats one later, unrelated Start-menu press.  Deriving avoids that as
// well, because on the keyboard paths the flag is already down.
// ---------------------------------------------------------------------------
void FlipController::EndSessionForHook()
{
    if (KeyboardHook::IsSessionActive())
        KeyboardHook::EndSessionForeign();
    else
        KeyboardHook::SetSessionActive(false);
}

void FlipController::ResolveReactivation()
{
    if (!m_reactivatePending)
        return;
    m_reactivatePending = false;

    if (!KeyboardHook::IsSessionActive())
        return;   // cancelled while the old session was still leaving

    Activate();
    if (!m_active)
        KeyboardHook::AbortSessionIfIdle(m_sessionEpoch);
}

void FlipController::StartCaptures()
{
    m_captures.clear();
    m_captures.resize(m_windows.size());
    auto* device = m_renderer.GetDevice();

    // Desktop tile re-enabled since the last session: hand the dedicated
    // wallpaper capture (with its warm cached frame) back to the cache so
    // the loop below reuses it for the desktop tile instead of creating a
    // second Progman capture.
    if (!m_desktopTileDisabled && m_wallpaperCapture) {
        HWND h = m_wallpaperCapture->GetHwnd();
        if (h)
            m_captureCache[h] = std::move(m_wallpaperCapture);
        else
            m_wallpaperCapture.reset();
    }

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

    // Desktop tile disabled: the wallpaper backdrop still needs a live
    // Progman/WorkerW capture — start (or restart) the dedicated one.
    // A warm object from a previous tile-enabled session may sit in the
    // cache under the desktop HWND; adopt it to keep its cached frame.
    if (m_desktopTileDisabled && m_desktopHwnd) {
        if (!m_wallpaperCapture) {
            auto it = m_captureCache.find(m_desktopHwnd);
            if (it != m_captureCache.end()) {
                m_wallpaperCapture = std::move(it->second);
                m_captureCache.erase(it);
            } else {
                m_wallpaperCapture = std::make_unique<WGCCapture>();
            }
        }
        m_wallpaperCapture->StartForWindow(m_desktopHwnd, device);
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

WGCCapture* FlipController::WallpaperCaptureSource()
{
    if (m_desktopTileDisabled) {
        // The dedicated capture must be the one started for THIS session's
        // desktop window — a stale object from a previous session (desktop
        // window unresolved this time, or Progman/WorkerW recreated) could
        // otherwise draw an old-resolution frame stretched into the
        // current backdrop rect.
        if (m_desktopHwnd && m_wallpaperCapture
            && m_wallpaperCapture->GetHwnd() == m_desktopHwnd)
            return m_wallpaperCapture.get();
        return nullptr;
    }
    if (m_desktopHwnd) {
        for (size_t i = 0; i < m_windows.size(); ++i) {
            if (m_windows[i].hwnd == m_desktopHwnd && i < m_captures.size())
                return m_captures[i].get();
        }
        // The desktop TILE can leave the stack while the desktop CAPTURE is
        // still the wallpaper's only source — the search filter hides it like
        // any other window whose title does not match.  Without this the
        // backdrop lost its texture the moment a query excluded "Desktop"
        // (which is nearly every query) and the cascade fell back to the
        // fully-opaque black underlay.  The capture keeps running while it is
        // hidden, so the wallpaper — animated ones included — carries on.
        for (const auto& hidden : m_searchHidden) {
            if (hidden.win.hwnd == m_desktopHwnd && hidden.capture)
                return hidden.capture.get();
        }
    }
    return nullptr;
}

ID3D11ShaderResourceView* FlipController::BackdropSRV()
{
    WGCCapture* wallCap = WallpaperCaptureSource();

    if (!m_config || m_config->liveBackground) {
        // Live (default): sample the running capture every frame so
        // animated wallpapers keep playing — the cycle animation included.
        // Safe during the cycle freeze: WallpaperCaptureSource() resolves
        // by desktop HWND (stable across the atomic array rotation, same
        // thread) and the wrap-slot correctness the freeze exists for
        // lives entirely in the frozen TILE SRVs.  The taskbar live
        // preview already samples its capture per frame while cycling —
        // same cost class.  The cycle-start snapshot remains a fallback
        // for a frame-less capture (device recreate mid-animation).
        ID3D11ShaderResourceView* srv =
            wallCap ? wallCap->GetCurrentFrame() : nullptr;
        if (!srv && m_sessionFrozen)
            srv = m_frozenDesktopSRV.get();
        return srv;
    }

    // Live background OFF: one owned snapshot serves the whole session.
    // Copy lazily on the first frame the capture delivers — an SRV ref
    // alone would NOT be static, because a live desktop tile keeps
    // copying fresh frames into the same shared cached texture.
    if (m_staticBackdropSRV)
        return m_staticBackdropSRV.get();

    ID3D11ShaderResourceView* liveSRV =
        wallCap ? wallCap->GetCurrentFrame() : nullptr;
    if (!liveSRV)
        return nullptr;

    ID3D11Device*        dev = m_renderer.GetDevice();
    ID3D11DeviceContext* ctx = m_renderer.GetContext();
    if (!dev || !ctx)
        return liveSRV;   // degraded: draw live this frame, retry next

    winrt::com_ptr<ID3D11Resource> res;
    liveSRV->GetResource(res.put());
    winrt::com_ptr<ID3D11Texture2D> src = res.try_as<ID3D11Texture2D>();
    if (!src)
        return liveSRV;

    D3D11_TEXTURE2D_DESC desc = {};
    src->GetDesc(&desc);
    winrt::com_ptr<ID3D11Texture2D> copyTex;
    if (FAILED(dev->CreateTexture2D(&desc, nullptr, copyTex.put())))
        return liveSRV;
    ctx->CopyResource(copyTex.get(), src.get());

    winrt::com_ptr<ID3D11ShaderResourceView> copySRV;
    if (FAILED(dev->CreateShaderResourceView(copyTex.get(), nullptr,
                                             copySRV.put())))
        return liveSRV;

    m_staticBackdropTexture = std::move(copyTex);
    m_staticBackdropSRV     = std::move(copySRV);
    return m_staticBackdropSRV.get();
}

// ---------------------------------------------------------------------------
// Slot ↔ window mapping (see flipcontroller.h).  The cascade preset — and
// Cover Flow whenever every window fits a slot — pairs slot i with
// m_windows[i].  Cover Flow with overflow is a circular carousel: the
// left-of-centre slots must show the TAIL of the window array (the most
// recently cycled-away windows) so the invisible pool sits "behind" the
// carousel, between the outer-right and outer-left slots.  With the
// identity pairing the left side showed windows from the MIDDLE of the
// cycle order and a forward cycle dropped the centre window into the
// invisible pool instead of stepping it to the inner-left slot — the
// "front window suddenly becomes a different app" symptom.
// ---------------------------------------------------------------------------
uint32_t FlipController::SlotWindowIndexFor(uint32_t slot, uint32_t slotCount,
                                            size_t windowCount, bool coverFlow)
{
    if (slot >= slotCount || windowCount <= slotCount || !coverFlow)
        return slot;
    if (CoverFlowLayout::SlotOffset(slot, slotCount) < 0)
        return static_cast<uint32_t>(windowCount - (slotCount - slot));
    return slot;
}

int FlipController::WindowSlotIndexFor(size_t windowIdx, uint32_t slotCount,
                                       size_t windowCount, bool coverFlow)
{
    if (windowCount <= slotCount || !coverFlow)
        return windowIdx < slotCount ? static_cast<int>(windowIdx) : -1;
    const uint32_t rightCount = slotCount / 2;   // slots 0..rightCount keep identity
    if (windowIdx <= rightCount)
        return static_cast<int>(windowIdx);
    const uint32_t leftCount = slotCount - 1 - rightCount;
    if (windowIdx >= windowCount - leftCount)
        return static_cast<int>(slotCount - (windowCount - windowIdx));
    return -1;   // invisible pool between outer-right and outer-left
}

uint32_t FlipController::SlotWindowIndex(uint32_t slot) const
{
    return SlotWindowIndexFor(
        slot, m_scene.SlotCount(), m_windows.size(),
        m_scene.GetVisualPreset() == VisualPreset::CoverFlow);
}

int FlipController::WindowSlotIndex(size_t windowIdx) const
{
    return WindowSlotIndexFor(
        windowIdx, m_scene.SlotCount(), m_windows.size(),
        m_scene.GetVisualPreset() == VisualPreset::CoverFlow);
}

std::vector<size_t> FlipController::SlotOrderIndices() const
{
    const size_t   m = m_windows.size();
    const uint32_t n = m_scene.SlotCount();
    std::vector<size_t> order;
    order.reserve(m);
    for (uint32_t i = 0; i < n && i < m; ++i)
        order.push_back(SlotWindowIndex(i));
    // Invisible pool follows, preserving array order.
    std::vector<bool> used(m, false);
    for (size_t idx : order)
        if (idx < m) used[idx] = true;
    for (size_t w = 0; w < m; ++w)
        if (!used[w]) order.push_back(w);
    return order;
}

std::vector<WindowInfo> FlipController::SlotOrderedWindows() const
{
    std::vector<size_t> order = SlotOrderIndices();
    std::vector<WindowInfo> out;
    out.reserve(order.size());
    for (size_t idx : order)
        out.push_back(m_windows[idx]);
    return out;
}

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
        const uint32_t wi = SlotWindowIndex(i);
        if (wi >= m_windows.size()) continue;
        const auto& rc = m_windows[wi].rect;
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

    // Cover Flow: the row was spaced from nominal tile widths at BuildSlots
    // time; now that every tile carries its window's real proportions,
    // re-derive the x coordinates so the shingle overlap is uniform.  No-op
    // for the cascade preset.  Every caller of this function (Activate, both
    // cycle paths, the close rebuild) therefore gets a consistent row, and
    // the animators snapshot their endpoints after it.
    m_scene.RelayoutCoverFlowX();
}

void FlipController::InjectDesktopWindow()
{
    // Find the desktop background window (Progman or WorkerW with SHELLDLL_DefView)
    m_desktopHwnd = WindowScanner::FindDesktopWindow();
    if (!m_desktopHwnd) {
        // No Progman/WorkerW means no wallpaper source: the cascade's backdrop
        // and the desktop tile both come from that one window.
        Diag::ReportOnce(Diag::Code::DesktopCaptureFailed, Diag::Sev::Warning,
                         L"The desktop wallpaper could not be captured",
                         L"the desktop background window was not found, so the "
                         L"backdrop behind the cascade falls back to plain black "
                         L"and there is no desktop tile");
        return;
    }

    // Build a WindowInfo for the desktop — sized to the full primary monitor
    WindowInfo desktop;
    desktop.hwnd      = m_desktopHwnd;
    desktop.title     = L"Desktop";
    desktop.rect      = m_monLayout.primary;
    desktop.minimized = false;

    // Append at the end — in Flip3D, desktop is always the LAST in the stack.
    m_windows.push_back(desktop);
}

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
// General-page exclusion list: remove windows whose owning executable is on
// config `excludedApps` (';'-separated, full path or bare exe name, case-
// insensitive — same matching rules as the trigger ignore list).  Runs once
// per activation right after the scan, so excluded windows never reach slot
// building, captures or animations; the cloaker still hides them behind the
// overlay together with every other non-cascade window and restores them on
// dismiss.  Process paths are resolved once per unique PID.
// ---------------------------------------------------------------------------
void FlipController::FilterExcludedWindows()
{
    if (!m_config || m_config->excludedApps.empty() || m_windows.empty())
        return;

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };
    auto fileNameOf = [](const std::wstring& path) -> std::wstring {
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    };

    // Parse the ';'-separated list (lowercased once).
    std::vector<std::wstring> entries;
    {
        const std::wstring& list = m_config->excludedApps;
        size_t start = 0;
        while (start <= list.size()) {
            size_t end = list.find(L';', start);
            if (end == std::wstring::npos) end = list.size();
            if (end > start)
                entries.push_back(toLower(list.substr(start, end - start)));
            if (end == list.size()) break;
            start = end + 1;
        }
    }
    if (entries.empty())
        return;

    // Per-PID verdict cache — multi-window processes resolve their image
    // path only once.
    std::unordered_map<DWORD, bool> pidExcluded;
    auto isExcludedPid = [&](DWORD pid) -> bool {
        auto it = pidExcluded.find(pid);
        if (it != pidExcluded.end())
            return it->second;

        bool match = false;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD len = static_cast<DWORD>(_countof(buf));
            if (QueryFullProcessImageNameW(proc, 0, buf, &len)) {
                std::wstring fullPath = toLower(buf);
                std::wstring fileName = fileNameOf(fullPath);
                for (const auto& entry : entries) {
                    if (entry == fullPath || entry == fileName
                        || fileNameOf(entry) == fileName) {
                        match = true;
                        break;
                    }
                }
            }
            CloseHandle(proc);
        }
        pidExcluded.emplace(pid, match);
        return match;
    };

    m_windows.erase(
        std::remove_if(m_windows.begin(), m_windows.end(),
                       [&](const WindowInfo& wi) {
                           DWORD pid = 0;
                           GetWindowThreadProcessId(wi.hwnd, &pid);
                           return pid != 0 && isExcludedPid(pid);
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
// Why PURE Z-order, and not a nicer-looking sort by program or by size: any
// order that disagrees with the OS Z-order leaks tiles on dismiss.  The
// freshly-foregrounded pick shows back tiles through it until DWM catches up.
// Honouring the OS order makes the cascade match what the user will see the
// moment the overlay hides.
// ---------------------------------------------------------------------------
void FlipController::SortWindowsByProgram()
{
    // m_originalZOrder is a session-start snapshot.
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

    // Promote the raw OS Z-rank map to a session-start member
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

    // A pending search edit reflows the stack as soon as no other animator
    // owns it.  Deferred rather than dropped, so a burst of keystrokes always
    // converges on the query that was actually typed.
    if (m_searchDirty)
        ApplySearchFilter();

    // Cloak any new windows that appeared since activation.
    CloakNewWindows();

    // Lazy EnsureFrame: only when NOT animating to avoid cursor lag.
    // PrintWindow is synchronous and heavy — never run it during animation.
    //
    // m_sessionFrozen only tracks the CYCLE animation; it is NOT set
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
        // Windows the search filter is holding out of the stack are still
        // streaming (they may come back on the next keystroke) — freeze them
        // on the same terms, or "live preview off" would quietly keep paying
        // for captures nobody can see.
        for (auto& hidden : m_searchHidden) {
            if (hidden.capture && hidden.capture->IsCapturing()
                && hidden.capture->HasNewFrame()) {
                hidden.capture->GetCurrentFrame();
                hidden.capture->Stop();
            }
        }
    }

    if (m_windows.empty() && !SearchHoldsEmptyStack()) {
        Escape();
        return;
    }

    m_renderer.BeginFrame();
    m_quad.ResetStateCache();
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

    // The fully opaque black backdrop that blocks everything behind the
    // overlay is BeginFrame's clear — see Renderer::BeginFrame.

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
        // Live vs static wallpaper backdrop (config liveBackground) —
        // see BackdropSRV().  Live keeps animated wallpapers playing
        // through the cycle animation; off serves one owned snapshot.
        ID3D11ShaderResourceView* desktopSRV = BackdropSRV();
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
            // Background blur intensity (config backgroundBlur %, 0 =
            // off → single-sample shader path, no extra cost).
            bgDraw.blurAmount = m_config
                ? static_cast<float>(m_config->backgroundBlur) / 100.0f
                : 0.0f;
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
                // The content-band UV crop belongs here as much as in
                // Activate's first-content frame.  Leave it out and every
                // frame after the first reverts to a bottom crop, which on
                // Win10 22H2 / Win11 24H2 samples the dark #282832 fill below
                // the real taskbar band: a taskbar that works for one frame
                // and then vanishes.  The shared helper keeps Activate,
                // RenderFrame and the debug dump identical.  Where no content
                // band was resolved m_taskbarContentResolved stays false and
                // this is a no-op.
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

    // Carry a thrown stack on before the animator reads the scene, so the
    // frame about to be drawn already reflects this tick's coasting.
    ScrubTickFling();

    // Advance cycle animation (if active) and apply slot overrides.
    m_cycleAnim.Tick(m_scene);

    // A released scrub that eased back to its own start pose: the rotation
    // Begin() performed still has to be rolled away (see ScrubUndoStep).
    if (m_cycleAnim.JustSettledToStart())
        ScrubUndoStep(m_scrubForward);

    // If animation just finished and there are queued cycles, start the next
    // one immediately — creates seamless continuous motion when key is held.
    ProcessCycleQueue();

    // Click-to-select spin: one step per frame, started the instant the last
    // one lands so the whole run is continuous, and the commit fires only
    // once the stack is settled on the chosen window.  Same frame as the
    // finish above, deliberately — a gap here would let RemoveClosedWindows
    // see an idle cascade mid-spin and start a close transition on top of it.
    AdvanceSelectJump();

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

    // The windows the search filter had hidden have finished rising back into
    // the stack — now the exit that was waiting for them can start, with the
    // full session in place.  Runs BEFORE the entry/exit tick so the morph
    // begins on this very frame, with no idle frame in between.
    if (m_pendingExit != PendingExit::None && !m_closeAnim.IsActive()) {
        const PendingExit what = m_pendingExit;
        m_pendingExit = PendingExit::None;
        // The hook's flag is left as ResumeSession lent it to us: Dismiss /
        // Escape read it to work out whether this commit came from the
        // keyboard, and clearing it here would tell them "keyboard" for a
        // click.  They hand it back themselves (EndSessionForHook).
        if (what == PendingExit::Dismiss) Dismiss();
        else                              Escape();
    }

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

    // Same-frame finalized flat present.  When the exit morph
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

    // Frozen-SRV cleanup guard.  This block runs
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

    // Pointer hover.  Re-tested every frame rather than only on pointer
    // movement: the stack moves under a stationary hand all the time (a
    // cycle, a throw, a close reflow), and the highlight has to stay on the
    // tile the pointer is genuinely over, not on the one that was there when
    // the mouse last moved.
    {
        // Only ever over a STILL stack — see PointerInteractionReady.  While
        // the stack moves the highlight simply lets go; it comes back on the
        // tile genuinely under the pointer once everything has settled.
        const bool stackStill = m_pointerValid && PointerEnabled()
            && PointerInteractionReady()
            && (m_config->mouseSelect || m_config->closeFromCascade);

        // ...and still for a MOMENT, not merely still on this frame.
        //
        // Nothing waits on the lift (see DropHoverLift), so this is purely
        // about not starting a rise that is only going to be sent back down.
        // Without the hold-off, the frames between two commands — a second
        // wheel notch, a held key's repeat, one spin step and the next — are
        // enough for the tile under a resting cursor to start climbing, and the
        // eye reads that bob as the stack being unsure of itself.  A stack that
        // is still being worked therefore lifts nothing; a stack the hand has
        // genuinely stopped on lifts a tenth of a second later, which is not a
        // difference anyone can see.
        if (!stackStill)
            m_hoverStillQPC.QuadPart = 0;
        else if (m_hoverStillQPC.QuadPart == 0)
            QueryPerformanceCounter(&m_hoverStillQPC);

        // A fall in progress bars the rise on its own account: the whole point
        // of it is that the tile reaches the floor, and re-targeting the slot
        // the pointer happens to rest on would pull it straight back up
        // mid-fall (see DropHoverLift).
        bool liftAllowed = stackStill && !m_hover.IsDropping();
        if (liftAllowed && m_perfFreq.QuadPart > 0) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double stillMs =
                static_cast<double>(now.QuadPart - m_hoverStillQPC.QuadPart)
                * 1000.0 / static_cast<double>(m_perfFreq.QuadPart);
            if (stillMs < kHoverRiseHoldMs)
                liftAllowed = false;
        }

        // The hold-off delays the LIFT, never the answer to "which tile is the
        // pointer over".  Those are different questions with different stakes:
        // the lift is decoration, while the hovered slot decides what a click
        // commits and what the close key closes — and a close that lands on the
        // front window because the highlight had not caught up yet would be the
        // worst kind of surprise this feature could produce.
        m_hoverSlot = stackStill
            ? HitTestScreen(m_pointerScreen.x, m_pointerScreen.y)
            : -1;
        m_hover.SetTarget(liftAllowed ? m_hoverSlot : -1);
        m_hover.Tick(m_scene.SlotCount(), AnimHoverEnabled());
    }

    float motionIntensity = m_cycleAnim.GetMotionIntensity();
    if (m_closeAnim.GetMotionIntensity() > motionIntensity)
        motionIntensity = m_closeAnim.GetMotionIntensity();
    float motionBlur = motionIntensity * 0.004f;
    if (!EffectiveMotionBlur()) motionBlur = 0.0f;

    // Glass floor reflections (Appearance → Reflections).  Each tile draws
    // a faint mirrored copy below its bottom edge, interleaved into the
    // back-to-front pass just before its tile so nearer tiles correctly
    // occlude farther reflections.  During the entry/exit morph the
    // reflection alpha rides the morph blend, fading in as the 3D pose
    // forms (the flat endpoint has nothing to reflect from) and out again
    // on exit.  Off (default): zero extra draws.
    const bool reflectionsOn = EffectiveReflections();
    const float reflectionGate =
        (m_entryExitAnimator.IsActive() || finishAfterPresent)
            ? m_entryExitAnimator.GetMorphBlend() : 1.0f;
    static constexpr float kReflectionStrength = 0.34f;

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
    // A Cover Flow exit supplies its own paint depths (a sort key only — the
    // geometry still comes from the slot).  Empty every other time, which is
    // why this needs no condition of its own.
    const std::vector<float>& paintZ = m_entryExitAnimator.GetExitPaintDepths();
    std::vector<DrawEntry> drawOrder;
    drawOrder.reserve(count + overflow.size() + dying.size());
    for (uint32_t i = 0; i < count; ++i) {
        const float z = (i < paintZ.size()) ? paintZ[i] : m_scene.GetSlot(i).z;
        drawOrder.push_back({ 0, static_cast<int>(i), z });
    }
    for (size_t k = 0; k < overflow.size(); ++k) {
        drawOrder.push_back({ 1, static_cast<int>(k), overflow[k].z });
    }
    for (size_t k = 0; k < dying.size(); ++k) {
        drawOrder.push_back({ 2, static_cast<int>(k), dying[k].z });
    }
    // Stable: Cover Flow mirrors a left and a right slot at EXACTLY equal Z,
    // and std::sort leaves the relative order of equal keys unspecified — so
    // which of the pair painted in front could differ from one frame to the
    // next as the other keys moved around them.  Identical output wherever
    // the keys differ, which is every other case.
    std::stable_sort(drawOrder.begin(), drawOrder.end(),
                     [](const DrawEntry& a, const DrawEntry& b) {
                         return a.z > b.z;
                     });

    // Texture for a visible cascade slot — shared by the reflection pass
    // and the tile pass so a mirror can never show a different frame than
    // its tile.  Selection logic:
    //   - exit stable SRV for the selected minimized window during exit;
    //   - frozen SRVs while a cycle animation is in flight — the wrap
    //     slot uses the PRE-rotate SRV during phase 1 (the departing
    //     window owns the journey until the α=0 boundary) and the post-
    //     rotate SRV afterwards; every other slot uses post-rotate.  At
    //     N > slot count the pre/post distinction is what keeps the
    //     departing and arriving windows' textures from swapping visibly
    //     (see the carousel-overflow wrap in CycleAnimator);
    //   - live capture otherwise.
    auto ResolveSlotSRV = [&](uint32_t idx) -> ID3D11ShaderResourceView* {
        // All texture arrays (captures, frozen SRV snapshots) are indexed
        // by WINDOW index — map the slot through the carousel pairing.
        const uint32_t wi = SlotWindowIndex(idx);
        ID3D11ShaderResourceView* srv = nullptr;
        if (m_exitSelectedStableSRV
            && (m_entryExitAnimator.IsReverse() || finishAfterPresent)
            && idx == 0
            && wi < m_windows.size()
            && m_windows[wi].hwnd == m_exitSelectedStableHwnd) {
            srv = m_exitSelectedStableSRV.get();
        } else if (m_sessionFrozen) {
            uint32_t n = m_cycleAnim.SlotCount();
            bool isWrapSlot = (m_cycleAnim.IsForward() && idx == n - 1) ||
                              (!m_cycleAnim.IsForward() && idx == 0);

            if (m_cycleAnim.IsSideSwapSlot(idx)
                && m_cycleAnim.IsInSideSwapPhase1()) {
                // Cover Flow side swap, phase 1: the DEPARTING window is
                // fading out at the row's far end.  Its texture is the
                // pre-rotate frame of whichever window occupied the
                // SOURCE slot — the slot↔window mapping is a function of
                // slot/count/preset only, so it reads the same before and
                // after the array rotation.
                const uint32_t srcSlot = m_cycleAnim.IsForward()
                    ? (idx + 1) % n
                    : (idx == 0 ? n - 1 : idx - 1);
                const uint32_t srcWin = SlotWindowIndex(srcSlot);
                if (srcWin < m_frozenStartSRVs.size())
                    srv = m_frozenStartSRVs[srcWin].get();
            } else if (isWrapSlot && m_cycleAnim.IsInWrapPhase1()) {
                if (m_cycleAnim.IsForward()) {
                    uint32_t srcIdx = (idx + 1) % n;   // = 0, old front
                    if (srcIdx < m_frozenStartSRVs.size())
                        srv = m_frozenStartSRVs[srcIdx].get();
                } else {
                    uint32_t backIdx = n - 1;          // old back slot
                    if (backIdx < m_frozenStartSRVs.size())
                        srv = m_frozenStartSRVs[backIdx].get();
                }
            } else {
                if (wi < m_frozenTargetSRVs.size())
                    srv = m_frozenTargetSRVs[wi].get();
            }
        } else if (wi < m_captures.size() && m_captures[wi]) {
            srv = m_captures[wi]->GetCurrentFrame();
        }
        return srv;
    };

    // World-space lift for a hovered tile — a DRAW offset only (see
    // HoverAnimator).
    //
    // It rides the morph blend rather than being switched off for the morph.
    // Off was nearly right and wrong in one visible way: the reason not to
    // apply a world-space nudge mid-morph is that the tile's transform is then
    // a lerp toward its flat screen rect, where a 3D offset reads as a wobble
    // — but that argument scales with how 3D the pose currently is, and a hard
    // cut does not.  Committing a lifted tile with a click therefore dropped
    // it 20 % of its own height in a single frame, right as the exit began: the
    // very twitch the lift is supposed to be too gentle to cause.  Multiplying
    // by the blend removes the nudge exactly as fast as the pose flattens, and
    // leaves the settled cascade (blend 1) bit-identical.
    // (m_exitPending with the animator idle is an exit that was SNAPPED —
    // entry/exit animation off.  There is no blend to ride, and the flat
    // finalized poses must be drawn exactly as written, so the lift goes.)
    const float hoverMorphGate =
        (m_entryExitAnimator.IsActive() || finishAfterPresent)
            ? m_entryExitAnimator.GetMorphBlend()
            : (m_exitPending ? 0.0f : 1.0f);
    const bool hoverLiftOn = m_hover.AnyLift() && hoverMorphGate > 0.001f;
    auto HoverLiftFor = [&](uint32_t idx) -> float {
        if (!hoverLiftOn) return 0.0f;
        const float l = m_hover.Lift(idx);
        if (l <= 0.001f) return 0.0f;
        return l * m_scene.GetSlot(idx).scaleY * HoverAnimator::kRiseFactor
             * hoverMorphGate;
    };

    // Texture for a dying tile.  The frozen snapshot comes first: a window the
    // search filter hid still owns a LIVE capture (it may be one keystroke
    // from returning), so sampling that capture would animate the fade-out
    // with frames the window is still producing.  A genuinely closed window
    // has no live capture and falls through to the stopped one, whose cached
    // last frame is exactly what the fade needs.
    auto ResolveDyingSRV = [&](size_t k) -> ID3D11ShaderResourceView* {
        if (k < m_closingSRVs.size() && m_closingSRVs[k])
            return m_closingSRVs[k].get();
        if (k < m_closingCaptures.size() && m_closingCaptures[k])
            return m_closingCaptures[k]->GetCurrentFrame();
        return nullptr;
    };

    // --- Reflection pass (Appearance → Reflections) ------------------------
    // ALL mirrors are drawn BEFORE any tile, in the same back-to-front
    // order.  Interleaving them with the tiles let a nearer tile's mirror
    // paint across the FACE of a deeper neighbour — the common floor plane
    // projects deeper tiles' bottom edges higher on screen, so the mirror
    // quad of a close tile overlaps them — which showed fragments of other
    // windows along the carousel's sides.  With a dedicated pass every
    // tile face is painted after, and therefore over, every mirror; the
    // mirrors still layer correctly among themselves.
    //
    // Layering note: this pass runs AFTER the taskbar layer (Layer 2) and
    // before the tiles, so a mirror hanging into the taskbar strip tints the
    // taskbar preview — whereas an AUTOHIDE bar, re-drawn on top after the
    // tiles (m_taskbarDrawOnTop below), hides it.  The two taskbar modes can
    // therefore differ slightly at the very bottom of the screen with
    // reflections enabled.  Not reproduced at 1920×1080, where the cascade's
    // tiles bottom out above the bar; if it ever shows up, move this pass
    // ahead of Layer 2 (it only has to stay behind the tiles).
    if (reflectionsOn && reflectionGate > 0.001f) {
        for (const auto& entry : drawOrder) {
            if (entry.kind == 1)
                continue;   // entry-morph overflow tiles cast no mirror
            if (entry.kind == 2) {
                // Dying close-anim tile — mirror under the fading window.
                size_t k = static_cast<size_t>(entry.idx);
                if (k >= dying.size()) continue;
                const TileSlot& slot = dying[k];
                if (slot.alpha < 0.001f) continue;
                ID3D11ShaderResourceView* srv = ResolveDyingSRV(k);
                if (!srv) continue;

                using namespace DirectX;
                XMMATRIX world =
                    XMMatrixScaling(slot.scaleX, slot.scaleY, 1.0f) *
                    XMMatrixRotationX(XMConvertToRadians(m_scene.GetSceneTiltX())) *
                    XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY() + slot.rotY)) *
                    XMMatrixTranslation(slot.x, slot.y, slot.z);
                XMMATRIX view, proj;
                m_scene.CameraMatrices(cascadeAspect, view, proj);

                QuadDrawCall refl;
                XMStoreFloat4x4(&refl.mvp,
                    XMMatrixTranslation(0.0f, -1.0f, 0.0f) * world
                    * view * proj * monRemap);
                // Same morph gate as the cascade slots below.  A close
                // transition is snapped before any exit morph begins, so
                // the gate reads 1.0 here today — carrying it keeps the two
                // reflection paths from drifting apart if that ever changes.
                refl.alpha  = slot.alpha * kReflectionStrength * reflectionGate;
                refl.uvMinY = 1.0f;   // V-flip: mirror image
                refl.uvMaxY = 0.0f;
                m_quad.DrawReflection(ctx, srv, refl);
                continue;
            }

            uint32_t idx = static_cast<uint32_t>(entry.idx);
            QuadDrawCall refl;
            float slotAlpha = 0.0f;
            // The mirror SINKS by as much as the tile rises: a floor mirror
            // does not follow the object up, the gap between them opens.
            m_scene.GetReflectionDrawCall(idx, cascadeAspect, refl.mvp,
                                          slotAlpha, -HoverLiftFor(idx));
            if (slotAlpha < 0.001f) continue;
            DirectX::XMMATRIX reflMVP =
                DirectX::XMLoadFloat4x4(&refl.mvp) * monRemap;
            DirectX::XMStoreFloat4x4(&refl.mvp, reflMVP);
            refl.alpha = slotAlpha * kReflectionStrength * reflectionGate;
            // Same crop as the tile (desktop tile's UV band included),
            // then swap the V endpoints for the mirror image.
            const uint32_t rwi = SlotWindowIndex(idx);
            if (rwi < m_windows.size()
                && m_windows[rwi].hwnd == m_desktopHwnd)
                ApplyTextureUV(refl, m_desktopTileUV);
            std::swap(refl.uvMinY, refl.uvMaxY);
            ID3D11ShaderResourceView* srv = ResolveSlotSRV(idx);
            if (srv) m_quad.DrawReflection(ctx, srv, refl);
        }
    }

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
                XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY() + slot.rotY)) *
                XMMatrixTranslation(slot.x, slot.y, slot.z);
            XMMATRIX view, proj;
            m_scene.CameraMatrices(cascadeAspect, view, proj);

            QuadDrawCall draw;
            XMStoreFloat4x4(&draw.mvp, world * view * proj * monRemap);
            draw.alpha = slot.alpha;
            draw.blurAmount = motionBlur;

            ID3D11ShaderResourceView* srv = ResolveDyingSRV(k);
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
                XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY() + slot.rotY)) *
                XMMatrixTranslation(slot.x, slot.y, slot.z);
            XMMATRIX view, proj;
            m_scene.CameraMatrices(cascadeAspect, view, proj);

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
        const uint32_t widx = SlotWindowIndex(idx);

        QuadDrawCall draw;
        float alpha;
        m_scene.GetDrawCall(idx, cascadeAspect, draw.mvp, alpha,
                            HoverLiftFor(idx));
        DirectX::XMMATRIX perspMVP =
            DirectX::XMLoadFloat4x4(&draw.mvp) * monRemap;
        DirectX::XMStoreFloat4x4(&draw.mvp, perspMVP);
        if ((m_entryExitAnimator.IsActive() || finishAfterPresent)
            && widx < m_windows.size()) {
            const RECT& morphRect =
                ResolveMorphScreenRect(m_entryExitAnimator, m_windows, idx, widx);
            DirectX::XMMATRIX screenMVP =
                ComputeScreenSpaceMVP(morphRect, vpW, vpH);
            DirectX::XMStoreFloat4x4(&draw.mvp,
                LerpMatrix(screenMVP, perspMVP,
                           m_entryExitAnimator.GetMorphBlend()));
        }
        draw.alpha = alpha;
        draw.blurAmount = motionBlur;
        if (widx < m_windows.size() && m_windows[widx].hwnd == m_desktopHwnd)
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
        //     slot 0) wrapping around.  Always take the POST-rotation
        //     new-front SRV, so it is the same window throughout the wrap.
        //     Reaching for m_frozenStartSRVs[n-1] instead is only correct
        //     at N == cascade slot count, where the two happen to be the
        //     same window.  With overflow they are not: pre-rotation slot
        //     n-1 is a window about to be pushed OUT, while post-rotation
        //     slot 0 is the previously-overflow window wrapping IN, so the
        //     first 40% of the cycle draws the wrong texture and then snaps
        //     to the right one at phase 2.
        ID3D11ShaderResourceView* srv = ResolveSlotSRV(idx);

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

    // Selected-window label (Appearance → Selected window label): title +
    // program icon of the front-slot window, drawn topmost as a screen-
    // space pill.  Hidden during the entry/exit morph — tiles are still
    // travelling and the pill would float over mid-flight geometry.
    if (!m_entryExitAnimator.IsActive() && !m_exitPending
        && !finishAfterPresent) {
        UpdateSelectedLabel();
        DrawSelectedLabel(ctx, vpW, vpH, monRemap);
        // Search field below the cascade — same gate as the label, for the
        // same reason: mid-morph there is no cascade for it to sit under.
        UpdateSearchBox();
        DrawSearchBox(ctx, vpW, vpH);
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

    // The finalized flat scene has now been presented; run the
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
                // Degradation compares against a 60 Hz-FLOORED budget.  On
                // 120/144/165 Hz displays the raw per-refresh budget is so
                // small (6-8 ms) that mid-range GPUs fail it every window
                // and ride the ladder down to tier 3, live preview off,
                // within seconds of the first session.  The cascade does
                // not need native-refresh frame rates to look right; only
                // dropping below ~44 fps is treated as "this device cannot
                // cope".
                //
                // Recovery takes kPerfRecoveryWindows comfortable windows
                // in a row to step one tier back up.  The wide 0.85/1.35
                // hysteresis gap plus the multi-second dwell is what stops
                // it oscillating.  Tiles already frozen by tier 3 stay
                // frozen for the rest of the session, since their capture
                // sessions were stopped; recovery re-enables live preview
                // from the NEXT activation, so nothing flickers mid-session.
                if (m_config && m_config->autoPerfTune
                    && m_config->perfProfile == -1) {
                    const double tuneBudgetMs =
                        (std::max)(m_refreshBudgetMs, kMinTuneBudgetMs);
                    if (avgMs > tuneBudgetMs * 1.35 && m_perfTier < 3) {
                        m_perfTier++;
                        m_perfGoodWindows = 0;
                        const wchar_t* lost =
                            m_perfTier == 1 ? L"motion blur"
                            : m_perfTier == 2 ? L"antialiasing" : L"live preview";
                        wchar_t buf[160];
                        swprintf_s(buf,
                            L"CKFlip3D: auto perf tune → tier %d (avg %.2f ms, budget %.2f ms) — %s disabled\n",
                            m_perfTier, avgMs, tuneBudgetMs, lost);
                        CKLog::Log(buf);
                        // A WARNING, not a notice: something the user switched
                        // on is not happening.  Auto perf tune working is not
                        // the point — the point is that a setting is being
                        // overruled and nothing else on screen says so, which
                        // is how a setting comes to look broken.
                        wchar_t detail[288];
                        _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                            L"frames were averaging %.1f ms against a %.1f ms "
                            L"budget, so %s was switched off for this session "
                            L"(Auto performance tune, General → Performance)",
                            avgMs, tuneBudgetMs, lost);
                        Diag::Report(Diag::Code::QualityLowered, Diag::Sev::Warning,
                                     L"A quality setting was turned off to keep up",
                                     detail);
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

bool FlipController::EffectiveReflections() const
{
    if (!m_config) return false;              // default-off feature
    // Reflections DOUBLE the tile draw calls, and of the optional effects
    // they are the cheapest to lose visually — so they go first, sharing
    // tier 1 with motion blur instead of claiming a rung of their own.
    // Renumbering the ladder would have changed when motion blur, AA and
    // live preview drop out; this way those thresholds are untouched and a
    // user who never enables reflections sees no behavioural difference.
    if (m_config->perfProfile == 0 || m_config->perfProfile == 1) return false;
    if (m_config->perfProfile == -1 && m_config->autoPerfTune && m_perfTier >= 1)
        return false;
    return m_config->reflections;
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

bool FlipController::AnimLabelEnabled() const
{
    return !m_config || (m_config->animations && m_config->animLabel);
}

bool FlipController::AnimHoverEnabled() const
{
    return !m_config || (m_config->animations && m_config->animHover);
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

void FlipController::RemoveClosedWindows()
{
    // Windows the search filter took out of the stack have no tile on screen,
    // so one closing is not a transition — it is simply gone.  Swept first and
    // unconditionally: none of the gates below concern them, and leaving a
    // dead HWND in the hidden set would let it re-enter the cascade when the
    // query is cleared.
    if (!m_searchHidden.empty()) {
        for (auto it = m_searchHidden.begin(); it != m_searchHidden.end(); ) {
            if (it->win.hwnd != m_desktopHwnd && !IsWindow(it->win.hwnd)) {
                if (it->capture) it->capture->Stop();
                it = m_searchHidden.erase(it);
            } else {
                ++it;
            }
        }
    }

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

    // A click-to-select spin runs a cycle per frame with no idle frame in
    // between — but it does briefly hand the arrays back between two steps,
    // and a close transition started in that gap would rebuild the scene the
    // next step is about to animate.  The sweep resumes the moment it lands.
    if (m_jumpTargetHwnd)
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

    const uint32_t oldSlotCount   = m_scene.SlotCount();
    const size_t   oldWindowCount = m_windows.size();
    const bool     coverFlow =
        (m_scene.GetVisualPreset() == VisualPreset::CoverFlow);

    // Old slot → HWND, so the post-rebuild layout can be traced back to
    // the poses each window is animating FROM.  Identity-ordered for the
    // cascade preset; the carousel's row order is a permutation.
    std::vector<HWND> oldSlotHwnd(oldSlotCount, nullptr);
    for (uint32_t s = 0; s < oldSlotCount; ++s) {
        const uint32_t w = SlotWindowIndexFor(s, oldSlotCount,
                                              oldWindowCount, coverFlow);
        if (w < oldWindowCount)
            oldSlotHwnd[s] = m_windows[w].hwnd;
    }

    // Animate only when the close animation is enabled (master toggle AND
    // its per-animation selection) and at least one closed window occupies
    // a visible cascade slot — pure-overflow closes have no tile on
    // screen, so the silent rebuild is already seamless.
    bool anyVisible = false;
    for (size_t i : closed) {
        if (WindowSlotIndexFor(i, oldSlotCount, oldWindowCount, coverFlow) >= 0) {
            anyVisible = true;
            break;
        }
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
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> dyingSRVs;
    for (auto it = closed.rbegin(); it != closed.rend(); ++it) {
        size_t i = *it;
        const int dyingSlot = WindowSlotIndexFor(i, oldSlotCount,
                                                oldWindowCount, coverFlow);
        const bool dyingVisible = animate && dyingSlot >= 0;
        if (dyingVisible) {
            std::unique_ptr<WGCCapture> cap;
            winrt::com_ptr<ID3D11ShaderResourceView> srv;
            if (i < m_captures.size() && m_captures[i]) {
                // Snapshot the SRV before the Stop so the fade-out draws from
                // a ref this list owns — the capture object still backs the
                // texture, but the reference is no longer the capture's to
                // recreate.
                srv = SrvRef(m_captures[i]->GetCurrentFrame());
                m_captures[i]->Stop();
                cap = std::move(m_captures[i]);
            }
            dyingCaps.push_back(std::move(cap));
            dyingSRVs.push_back(std::move(srv));
            dyingSlotIdx.push_back(static_cast<uint32_t>(dyingSlot));
        }
        if (i < m_captures.size()) {
            if (m_captures[i]) m_captures[i]->Stop();
            m_captures.erase(m_captures.begin() + static_cast<ptrdiff_t>(i));
        }
        m_windows.erase(m_windows.begin() + static_cast<ptrdiff_t>(i));
    }
    // Begin() and the dying-tile draw expect ascending SLOT order, and the
    // captures must stay parallel.  The reverse walk emitted descending
    // window indices, which is descending slot order only in the cascade;
    // sort the pairs so the carousel's permuted row is handled too.
    {
        std::vector<size_t> order(dyingSlotIdx.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) {
                      return dyingSlotIdx[a] < dyingSlotIdx[b];
                  });
        std::vector<uint32_t> sortedIdx;
        std::vector<std::unique_ptr<WGCCapture>> sortedCaps;
        std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> sortedSRVs;
        sortedIdx.reserve(order.size());
        sortedCaps.reserve(order.size());
        sortedSRVs.reserve(order.size());
        for (size_t k : order) {
            sortedIdx.push_back(dyingSlotIdx[k]);
            sortedCaps.push_back(std::move(dyingCaps[k]));
            sortedSRVs.push_back(std::move(dyingSRVs[k]));
        }
        dyingSlotIdx = std::move(sortedIdx);
        dyingCaps    = std::move(sortedCaps);
        dyingSRVs    = std::move(sortedSRVs);
    }

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
    // the entry-time flat slots cached per HWND are now
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
        for (auto& srv : dyingSRVs)
            m_closingSRVs.push_back(std::move(srv));

        // Cover Flow: hand the animator an explicit per-new-slot source
        // map.  Its default derivation pairs survivors with new slots in
        // ascending order, which only holds while slot order == window
        // order — the carousel permutes the row, and a window from
        // outside the visible row can surface into it.  The cascade keeps
        // the default (nullptr) and is therefore completely untouched.
        std::vector<int> newSlotSource;
        if (coverFlow)
            newSlotSource = BuildSlotSourceMap(oldSlotHwnd);

        // The reflow re-assigns slots exactly as a cycle does, so a lift still
        // on its way down would land on whichever window inherited the slot.
        // It cannot WAIT here the way a cycle can — a window that has already
        // gone must not stay in the arrays for another tenth of a second — so
        // the fall simply runs alongside the reflow, bounded and brief instead
        // of trailing the whole transition (see DropHoverLift).
        DropHoverLift();

        m_closeAnim.Begin(m_scene, startSlots, dyingSlotIdx, oldCam,
                          coverFlow ? &newSlotSource : nullptr);
        CKLog::Log(L"CKFlip: Window closed — close transition started\n");
    } else {
        CKLog::Log(L"CKFlip: Window closed — rebuilt scene\n");
    }
}

void FlipController::ClearClosingCaptures()
{
    for (auto& cap : m_closingCaptures) {
        if (cap)
            cap->Stop();
    }
    m_closingCaptures.clear();
    m_closingSRVs.clear();
}

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

void FlipController::HideRealTaskbar()
{
    m_taskbarWasVisible = false;
    m_taskbarHeld = false;
    m_taskbarHeldPinPosition = false;
    m_heldPinCounter = 0;
    for (auto& tray : m_secondaryTrays) {
        tray.wasVisible = false;
        tray.held = false;
        tray.heldPinPosition = false;
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
        const bool continuity = m_taskbarAutoHide && m_taskbarExtendedAtStart;
        const bool hold = continuity || m_taskbarLiveActive;
        if (hold && ValidRect(m_taskbarHoldRectScreen)) {
            m_taskbarHeld = true;
            m_taskbarHeldPinPosition = continuity;
            m_taskbarLastSeenRect = m_taskbarHoldRectScreen;
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
            const bool continuity = m_taskbarAutoHide && tray.extendedAtStart;
            const bool hold = continuity || tray.liveActive;
            if (hold && ValidRect(tray.holdRectScreen)) {
                tray.held = true;
                tray.heldPinPosition = continuity;
                tray.lastSeenRect = tray.holdRectScreen;
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
    // raising the bar above the overlay).  Continuity holds re-pin the
    // rect when it drifted; live-only holds deliberately DON'T — with
    // autohide enabled the shell re-animates the retraction every frame,
    // and answering each step with a cross-process SetWindowPos is a
    // per-frame tug of war (measurable frame cost) for a position the
    // live preview doesn't even need.  The below-overlay Z is re-asserted
    // periodically for every held bar as cheap insurance.
    ++m_heldPinCounter;
    const bool reassertZ = (m_heldPinCounter % 30) == 0;
    HWND overlay = m_renderer.GetHwnd();

    auto pin = [&](HWND bar, const RECT& hold, bool enforcePos,
                   RECT& lastSeen) {
        if (!bar || !IsWindow(bar) || !ValidRect(hold))
            return;
        RECT cur{};
        GetWindowRect(bar, &cur);
        if (!enforcePos) {
            // Live-only hold: never fight the shell's position animation.
            // But a rect in motion (edge-hover reveal / retraction) is
            // exactly the moment the shell may also raise the bar above
            // the overlay, so re-assert the Z IMMEDIATELY while moving —
            // Z-only (SWP_NOMOVE|SWP_NOSIZE) never feeds the animation
            // back, so the burst self-terminates in a few hundred ms.
            const bool moving = !EqualRect(&cur, &lastSeen);
            lastSeen = cur;
            if (moving || reassertZ)
                SetWindowPos(bar, overlay, 0, 0, 0, 0,
                             SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
            return;
        }
        lastSeen = cur;
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
        pin(m_taskbarHwnd, m_taskbarHoldRectScreen, m_taskbarHeldPinPosition,
            m_taskbarLastSeenRect);
    for (auto& tray : m_secondaryTrays)
        if (tray.held)
            pin(tray.hwnd, tray.holdRectScreen, tray.heldPinPosition,
                tray.lastSeenRect);
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
    m_taskbarHeldPinPosition = false;
    if (m_taskbarWasVisible && m_taskbarHwnd && IsWindow(m_taskbarHwnd)) {
        ShowWindow(m_taskbarHwnd, SW_SHOW);
        // A taskbar this program hid and could not put back is a desktop the
        // user has to fix by restarting explorer.  Nothing else in the session
        // teardown is this visible when it goes wrong.
        if (!IsWindowVisible(m_taskbarHwnd))
            Diag::Report(Diag::Code::TaskbarStateFailed, Diag::Sev::Critical,
                         L"The taskbar did not come back after the cascade closed",
                         L"CKFlip3D hid the real taskbar for the session and the "
                         L"shell has not shown it again; opening and closing the "
                         L"switcher usually restores it, restarting Windows "
                         L"Explorer always does");
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
        tray.heldPinPosition = false;
        if (tray.wasVisible && tray.hwnd && IsWindow(tray.hwnd))
            ShowWindow(tray.hwnd, SW_SHOW);
        tray.wasVisible = false;
        tray.liveActive = false;
    }
}

// ---------------------------------------------------------------------------
// Nothing here hides the desktop icons, and nothing should.  Hiding them was
// tried and removed: it changed nothing visible, because every surface the
// cascade draws is captured at or before the moment the hide would happen, and
// it cost a blink on every dismiss.  SW_SHOW is a request to Explorer, the
// repaint lands on Explorer's message loop long after the call returns, so the
// overlay lifted onto bare wallpaper and the icons arrived a moment later.
// Waiting for Explorer only trades that blink for dead time on every commit.
// ===========================================================================
// Pointer in the cascade (Controls → Mouse & keyboard)
//
// Hover, click-to-select and close-from-the-stack.  Everything below is a new
// SOURCE of the commands the cascade already understands — the pointer never
// gets its own path through Dismiss, the exit morph or the close transition,
// it just decides which window they act on.  That is what keeps the keyboard,
// wheel and touchpad paths bit-identical with the feature switched off, and
// what keeps a click from being a second way for something to go wrong.
// ===========================================================================

bool FlipController::PointerInteractionReady() const
{
    return !m_cycleAnim.IsActive()
        && m_cycleQueue.empty()
        && !m_flinging
        && !m_scrubActive
        && !m_closeAnim.IsActive()
        && !m_jumpTargetHwnd
        && m_pendingExit == PendingExit::None
        && !m_entryExitAnimator.IsActive()
        && !m_exitPending
        && !m_reverseDelayPending;
}

void FlipController::DropHoverLift()
{
    if (!m_hover.AnyLift())
        return;                 // nothing is up: the common case
    m_hoverSlot = -1;
    m_hover.BeginDrop(AnimHoverEnabled());
}

int FlipController::HitTestScreen(int screenX, int screenY) const
{
    if (!m_active || m_scene.SlotCount() == 0)
        return -1;
    // Only a settled cascade is hittable.  During the entry/exit morph the
    // drawn transform is a lerp between the tile's flat screen rect and its
    // 3D pose (see the draw loop), so testing the 3D pose alone would report
    // a tile that is not where it says it is.
    if (m_entryExitAnimator.IsActive() || m_exitPending || m_reverseDelayPending)
        return -1;

    RECT rc{};
    GetClientRect(m_renderer.GetHwnd(), &rc);
    const float vpW = static_cast<float>(rc.right - rc.left);
    const float vpH = static_cast<float>(rc.bottom - rc.top);
    if (vpW <= 0.0f || vpH <= 0.0f)
        return -1;

    // Screen → overlay client space (the overlay spans the virtual screen).
    const float px = static_cast<float>(screenX) - m_overlayOriginX;
    const float py = static_cast<float>(screenY) - m_overlayOriginY;

    // The hover lift is a DRAW offset (HoverAnimator owns no slot state), so
    // the scene alone describes tiles as if none were lifted.  Hand the hit
    // test the same per-slot offset the draw pass applies, or the highlighted
    // tile answers clicks over the area it has just risen OUT of, and its own
    // top edge belongs to whichever tile lies behind it.
    //
    // The draw gates this on the entry/exit morph as well; both of those states
    // already returned -1 above, so AnyLift() is the whole remaining condition.
    const uint32_t slotCount = m_scene.SlotCount();
    float lifts[kMaxHitTestSlots] = {};
    TileHitTest::SlotOffsets offsets;
    if (m_hover.AnyLift() && slotCount <= kMaxHitTestSlots) {
        for (uint32_t i = 0; i < slotCount; ++i) {
            const float l = m_hover.Lift(i);
            lifts[i] = (l > 0.001f)
                ? l * m_scene.GetSlot(i).scaleY * HoverAnimator::kRiseFactor
                : 0.0f;
        }
        offsets.y     = lifts;
        offsets.count = slotCount;
    }

    return TileHitTest::PickSlot(m_scene,
                                 DirectX::XMLoadFloat4x4(&m_monRemapNDC),
                                 m_cascadeAspect, vpW, vpH, px, py, offsets);
}

void FlipController::PointerMove(int screenX, int screenY)
{
    if (!m_active || !PointerEnabled())
        return;
    m_pointerScreen.x = screenX;
    m_pointerScreen.y = screenY;
    m_pointerValid    = true;
    // The hit test itself runs per frame in RenderFrame — the stack moves
    // under a still pointer far more often than the pointer moves.
}

void FlipController::PointerSelect(int screenX, int screenY)
{
    if (!m_active || !PointerEnabled() || !m_config->mouseSelect)
        return;
    // A click only counts on a settled stack: mid-cycle the tile under the
    // cursor is whichever one is sweeping past, not the one being aimed at.
    if (!PointerInteractionReady())
        return;

    const int slot = HitTestScreen(screenX, screenY);
    if (slot < 0)
        return;   // clicked the backdrop — deliberately does nothing
    CommitSlot(static_cast<uint32_t>(slot));
}

void FlipController::PointerClose(int screenX, int screenY)
{
    if (!m_active || !PointerEnabled() || !m_config->closeFromCascade)
        return;
    if (!PointerInteractionReady())
        return;   // same reason as PointerSelect — aim needs a still stack
    CloseWindowAtSlot(HitTestScreen(screenX, screenY));
}

void FlipController::CloseSelectedWindow()
{
    // No switch to consult: the message only arrives when a key on `closeKeys`
    // was pressed, and an empty list is how that binding is switched off.
    // Deliberately not behind PointerEnabled() or the close click's toggle
    // either — it falls back to the selection when no tile is hovered, which is
    // the keyboard-only case.  The close CLICK (PointerClose) keeps the pointer
    // gate, because it is one.
    if (!m_active || !m_config)
        return;
    // Delete follows the pointer when there is one over the stack, and the
    // selection otherwise — both are "the window I am looking at".
    CloseWindowAtSlot(m_hoverSlot >= 0 ? m_hoverSlot : 0);
}

void FlipController::CloseWindowAtSlot(int slot)
{
    if (slot < 0)
        return;
    const uint32_t widx = SlotWindowIndex(static_cast<uint32_t>(slot));
    if (widx >= m_windows.size())
        return;

    HWND target = m_windows[widx].hwnd;
    if (!target || target == m_desktopHwnd || !IsWindow(target))
        return;   // the desktop pseudo-tile is not a window anyone can close

    // WM_CLOSE, never a kill: the application runs its own close path — a
    // save prompt, a confirmation, a veto — exactly as if its title-bar × had
    // been clicked.  The tile therefore leaves the cascade only once the
    // window actually dies, which RemoveClosedWindows already notices and
    // animates; nothing here removes anything, so a window that declines to
    // close simply stays in the stack, which is the honest outcome.
    PostMessageW(target, WM_CLOSE, 0, 0);
    CKLog::Log(L"CKFlip: close requested from the cascade (WM_CLOSE)\n");
}

void FlipController::CommitSlot(uint32_t slot)
{
    const uint32_t widx = SlotWindowIndex(slot);
    if (widx >= m_windows.size())
        return;

    if (widx == 0) {
        Dismiss();          // already the selection — the ordinary commit
        return;
    }

    // A window further back has to reach the front BEFORE the exit morph
    // starts.  The morph pairs window 0 with the tile that flies to the
    // foreground and derives the whole endpoint Z ranking from that pairing
    // (see Dismiss), so committing a back slot directly would animate one
    // window while raising another.  Spin first, commit on landing.
    ResolveScrub();
    m_cycleQueue.clear();

    const size_t n = m_windows.size();
    const size_t steps = (std::min)(static_cast<size_t>(widx), n - widx);
    m_jumpTargetHwnd = m_windows[widx].hwnd;
    m_jumpCommit     = true;
    m_jumpStepMs     = std::clamp(
        kJumpBudgetMs / static_cast<float>((std::max<size_t>)(steps, 1)),
        kJumpStepMinMs, kJumpStepMaxMs);

    // The tile just clicked is the one under the pointer, and so the one that
    // is up: it comes down as the spin carries it forward (see DropHoverLift).
    DropHoverLift();

    // Start on this frame rather than waiting for the next one — the click
    // should move the stack, not pause first.
    AdvanceSelectJump();
}

void FlipController::AdvanceSelectJump()
{
    if (!m_jumpTargetHwnd)
        return;
    if (!m_active) { CancelSelectJump(); return; }

    // Another animator owns the slots — wait it out rather than layering on
    // top of it.  (The entry morph is the realistic case: a click landing in
    // the first few frames of the cascade opening.)
    if (m_cycleAnim.IsActive() || m_closeAnim.IsActive()
        || m_entryExitAnimator.IsActive())
        return;

    // Located by HWND every step: the stack can be re-ordered underneath the
    // spin (a window closes, the search filter changes), and an index would
    // quietly start chasing whichever window inherited it.
    size_t idx = m_windows.size();
    for (size_t i = 0; i < m_windows.size(); ++i) {
        if (m_windows[i].hwnd == m_jumpTargetHwnd) { idx = i; break; }
    }
    if (idx >= m_windows.size()) {
        // The window went away mid-spin.  Abandon the commit as well: raising
        // whatever happens to be in front now is not what was clicked.
        CancelSelectJump();
        return;
    }

    if (idx == 0) {
        const bool commit = m_jumpCommit;
        CancelSelectJump();
        if (commit)
            Dismiss();
        return;
    }

    const size_t n = m_windows.size();
    // Whichever way round is shorter.  In Cover Flow the left-hand slots map
    // onto the TAIL of the window array, so a tile two places to the left is
    // two backward steps, not n-2 forward ones.
    const bool forward = (idx <= n - idx);

    if (!AnimCycleEnabled()) {
        // Nothing to animate — walk the whole way in one go and commit.
        for (size_t guard = 0; guard < n && !m_windows.empty()
                               && m_windows[0].hwnd != m_jumpTargetHwnd; ++guard) {
            if (forward) ExecuteCycleForward();
            else         ExecuteCycleBackward();
        }
        const bool commit = m_jumpCommit;
        CancelSelectJump();
        if (commit)
            Dismiss();
        return;
    }

    m_scrubPending = false;
    if (forward) ExecuteCycleForward(true);
    else         ExecuteCycleBackward(true);
    // Tick once so this frame already shows movement — the same zero-gap
    // chaining ProcessCycleQueue relies on.
    m_cycleAnim.Tick(m_scene);
}

void FlipController::CancelSelectJump()
{
    m_jumpTargetHwnd = nullptr;
    m_jumpCommit     = false;
    m_jumpStepMs     = 0.0f;
}

std::vector<int> FlipController::BuildSlotSourceMap(
    const std::vector<HWND>& oldSlotHwnd) const
{
    const uint32_t newSlotCount   = m_scene.SlotCount();
    const size_t   newWindowCount = m_windows.size();
    const bool     coverFlow =
        (m_scene.GetVisualPreset() == VisualPreset::CoverFlow);

    std::vector<int> map(newSlotCount, -1);
    for (uint32_t i = 0; i < newSlotCount; ++i) {
        const uint32_t w = SlotWindowIndexFor(i, newSlotCount,
                                              newWindowCount, coverFlow);
        if (w >= newWindowCount)
            continue;
        const HWND h = m_windows[w].hwnd;
        for (size_t s = 0; s < oldSlotHwnd.size(); ++s) {
            if (oldSlotHwnd[s] == h) {
                map[i] = static_cast<int>(s);
                break;
            }
        }
    }
    return map;
}

// ===========================================================================
// Type-to-filter (Settings → Search)
//
// Typing narrows the cascade to the matching windows and clearing the query
// brings the rest back — both through the SAME close transition a real window
// close uses, so a filtered-out window falls away exactly like a closed one
// and a returning window arrives exactly like an overflow window rotating in.
// No new choreography, and therefore no new way for the stack to look wrong.
//
// The windows themselves are only ever moved between two lists (m_windows and
// m_searchHidden); nothing is destroyed, and their captures keep streaming,
// because the very next keystroke may bring them back.
// ===========================================================================

void FlipController::SearchAppend(wchar_t c)
{
    // Once the exit is committed and only waiting for the windows to return,
    // the query is history — editing it would filter a stack that is on its
    // way out.
    if (!m_active || !m_config || !m_config->searchEnabled
        || m_pendingExit != PendingExit::None)
        return;
    if (m_search.Append(c))
        m_searchDirty = true;
}

void FlipController::SearchBackspace()
{
    if (!m_active || !m_config || !m_config->searchEnabled
        || m_pendingExit != PendingExit::None)
        return;
    if (m_search.Backspace())
        m_searchDirty = true;
}

bool FlipController::SearchClear()
{
    if (!m_search.Clear())
        return false;
    m_searchDirty = true;
    return true;
}

bool FlipController::SearchHoldsEmptyStack() const
{
    // Only while there is something to come BACK to.  An empty stack with an
    // empty hidden set is not "your query matched nothing" — it is a session
    // whose windows have genuinely all closed, and that has to end the way it
    // always did rather than hang on an empty cascade waiting for a backspace
    // that would bring nothing with it.
    return m_config && m_config->searchEnabled
        && !m_search.Empty() && !m_searchHidden.empty();
}

// ---------------------------------------------------------------------------
// Lift the filter for good, right before an exit morph is built.
//
// The morph is a statement about the whole desktop: every tile flies from its
// cascade slot to the real screen rect of its window, and the endpoint Z
// ranking is derived from the full list (see Dismiss).  Handing it a filtered
// subset would animate some windows and silently leave the rest cloaked until
// teardown — windows appearing to "go missing" mid-exit is exactly the
// conflict this avoids.
//
// The restored order is the session's own, rotated so the chosen window keeps
// the front slot: precisely the arrangement the user would have reached by
// scrolling to that window by hand.  Deliberately NOT animated — the exit
// morph starts from this pose on the very next frame, so the windows come
// back and fly out as one motion instead of two.
// ---------------------------------------------------------------------------
bool FlipController::RestoreSearchWindowsForExit()
{
    // Whatever happens, the query does not survive into the next session.
    m_search.Reset();
    m_searchDirty   = false;
    m_searchNoMatch = false;

    if (m_searchHidden.empty())
        return false;

    const HWND front = m_windows.empty() ? nullptr : m_windows[0].hwnd;

    // The pose the returning windows animate FROM — snapshotted before
    // anything moves, exactly like every other reflow here.
    const uint32_t oldSlotCount   = m_scene.SlotCount();
    const size_t   oldWindowCount = m_windows.size();
    const bool     coverFlow =
        (m_scene.GetVisualPreset() == VisualPreset::CoverFlow);
    std::vector<TileSlot> startSlots(oldSlotCount);
    for (uint32_t i = 0; i < oldSlotCount; ++i)
        startSlots[i] = m_scene.GetSlot(i);
    const CloseAnimator::CameraPose oldCam{
        m_scene.GetCamEyeX(),    m_scene.GetCamEyeY(),    m_scene.GetCamEyeZ(),
        m_scene.GetCamTargetX(), m_scene.GetCamTargetY(), m_scene.GetCamTargetZ()
    };
    std::vector<HWND> oldSlotHwnd(oldSlotCount, nullptr);
    for (uint32_t s = 0; s < oldSlotCount; ++s) {
        const uint32_t w = SlotWindowIndexFor(s, oldSlotCount,
                                              oldWindowCount, coverFlow);
        if (w < oldWindowCount)
            oldSlotHwnd[s] = m_windows[w].hwnd;
    }

    struct Entry {
        WindowInfo                  win;
        std::unique_ptr<WGCCapture> cap;
        size_t                      order = 0;
    };
    std::vector<Entry> all;
    all.reserve(m_windows.size() + m_searchHidden.size());

    for (size_t i = 0; i < m_windows.size(); ++i) {
        Entry e;
        e.win = m_windows[i];
        auto it = m_windowMeta.find(e.win.hwnd);
        e.order = (it != m_windowMeta.end()) ? it->second.order : i;
        if (i < m_captures.size())
            e.cap = std::move(m_captures[i]);
        all.push_back(std::move(e));
    }
    for (auto& hidden : m_searchHidden) {
        Entry e;
        e.win   = hidden.win;
        e.cap   = std::move(hidden.capture);
        e.order = hidden.order;
        all.push_back(std::move(e));
    }
    m_searchHidden.clear();

    std::sort(all.begin(), all.end(),
              [](const Entry& a, const Entry& b) { return a.order < b.order; });
    if (front) {
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].win.hwnd == front) {
                std::rotate(all.begin(), all.begin() + i, all.end());
                break;
            }
        }
    }

    m_windows.clear();
    m_captures.clear();
    m_windows.reserve(all.size());
    m_captures.reserve(all.size());
    for (auto& e : all) {
        m_windows.push_back(e.win);
        m_captures.push_back(std::move(e.cap));
    }

    RECT rc{};
    GetClientRect(m_renderer.GetHwnd(), &rc);
    float vpW = static_cast<float>(rc.right - rc.left);
    float vpH = static_cast<float>(rc.bottom - rc.top);
    if (vpW <= 0) vpW = 1920.0f;
    if (vpH <= 0) vpH = 1080.0f;
    UpdateCascadeSpace(vpW, vpH);

    uint32_t displayCount = static_cast<uint32_t>(m_windows.size());
    if (m_config && m_config->maxWindows < displayCount)
        displayCount = m_config->maxWindows;
    m_scene.BuildSlots(displayCount, m_cascadeW, m_cascadeH);
    RebuildSceneAspects();
    // The camera was re-derived for the restored count, so the entry-time
    // flat rects cached per HWND are in a stale frame — dropping them makes
    // the exit compute fresh ones (see RemoveClosedWindows).
    m_entryExitAnimator.ClearEntryFlatCache();

    // The slot the pointer was over no longer holds the same window.
    m_hoverSlot = -1;
    m_hover.SetTarget(-1);

    // Show them ARRIVING.  The windows appearing in one frame and immediately
    // flying out was the whole cascade blinking: the eye has no chance to see
    // what came back, only that something flashed.  They rise into place
    // first, and the caller holds the exit until they have landed.
    ClearClosingCaptures();
    if (AnimCloseEnabled()) {
        const std::vector<int> newSlotSource = BuildSlotSourceMap(oldSlotHwnd);
        const std::vector<uint32_t> noDying;
        m_closeAnim.Begin(m_scene, startSlots, noDying, oldCam,
                          &newSlotSource, /*riseIn*/ true);
    }

    CKLog::Log(L"CKFlip: search filter lifted — windows returning before exit\n");
    return AnimCloseEnabled();
}

void FlipController::ClearSearchState()
{
    // Hidden windows' captures go back to the warm cache exactly like the
    // visible ones do (StopCaptures) — a window filtered out of THIS session
    // is no less likely to be wanted in the next one, and Stop() keeps its
    // cached frame for the warm start.
    for (auto& hidden : m_searchHidden) {
        if (!hidden.capture)
            continue;
        HWND h = hidden.capture->GetHwnd();
        hidden.capture->Stop();
        if (h)
            m_captureCache[h] = std::move(hidden.capture);
    }
    m_searchHidden.clear();
    m_windowMeta.clear();
    m_search.Reset();
    m_searchBox.Reset();
    m_searchDirty   = false;
    m_searchNoMatch = false;
}

// ---------------------------------------------------------------------------
// Per-window facts the filter needs, resolved once per session.  Keyed by
// HWND on purpose: m_windows is rotated by every cycle, and a parallel array
// would be one more thing to keep in step through paths that have nothing to
// do with searching.
//
// Skipped entirely when the feature is off, so a user who never searches
// never pays for a single OpenProcess.
// ---------------------------------------------------------------------------
void FlipController::BuildWindowMetadata()
{
    m_windowMeta.clear();
    if (!m_config || !m_config->searchEnabled)
        return;

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), ::towlower);
        return s;
    };

    // One image-path lookup per process, however many windows it owns.
    std::unordered_map<DWORD, std::wstring> pidExe;
    auto exeForPid = [&](DWORD pid) -> const std::wstring& {
        auto it = pidExe.find(pid);
        if (it != pidExe.end())
            return it->second;

        std::wstring name;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t buf[MAX_PATH * 2] = {};
            DWORD len = static_cast<DWORD>(_countof(buf));
            if (QueryFullProcessImageNameW(proc, 0, buf, &len)) {
                std::wstring full = toLower(buf);
                size_t slash = full.find_last_of(L"\\/");
                name = (slash == std::wstring::npos) ? full
                                                     : full.substr(slash + 1);
            }
            CloseHandle(proc);
        }
        return pidExe.emplace(pid, std::move(name)).first->second;
    };

    for (size_t i = 0; i < m_windows.size(); ++i) {
        WindowMeta meta;
        meta.order = i;
        DWORD pid = 0;
        GetWindowThreadProcessId(m_windows[i].hwnd, &pid);
        if (pid != 0)
            meta.exeLower = exeForPid(pid);
        m_windowMeta[m_windows[i].hwnd] = std::move(meta);
    }
}

void FlipController::ApplySearchFilter()
{
    if (!m_active || !m_config || !m_config->searchEnabled) {
        m_searchDirty = false;
        return;
    }

    // The same gates RemoveClosedWindows uses, for the same reason: the
    // window/capture arrays and the slot poses must belong to nobody else.
    // Returning WITHOUT clearing m_searchDirty is the point — the edit is
    // simply applied on a later frame, so nothing typed is ever lost.
    if (m_sessionFrozen || m_entryExitAnimator.IsActive())
        return;
    if (m_exitPending || m_reverseDelayPending)
        return;
    if (m_cycleAnim.IsActive() || !m_cycleQueue.empty())
        return;
    if (m_closeAnim.IsActive())
        return;
    if (m_jumpTargetHwnd)
        return;

    m_searchDirty = false;

    // ---- Snapshot the pose the transition starts from ---------------------
    // Taken BEFORE anything moves, exactly like the close sweep: BuildSlots
    // below re-derives the camera for the new count, and CloseAnimator
    // re-expresses these poses under it so frame 1 matches the last frame.
    const uint32_t oldSlotCount   = m_scene.SlotCount();
    const size_t   oldWindowCount = m_windows.size();
    const bool     coverFlow =
        (m_scene.GetVisualPreset() == VisualPreset::CoverFlow);

    std::vector<TileSlot> startSlots(oldSlotCount);
    for (uint32_t i = 0; i < oldSlotCount; ++i)
        startSlots[i] = m_scene.GetSlot(i);
    const CloseAnimator::CameraPose oldCam{
        m_scene.GetCamEyeX(),    m_scene.GetCamEyeY(),    m_scene.GetCamEyeZ(),
        m_scene.GetCamTargetX(), m_scene.GetCamTargetY(), m_scene.GetCamTargetZ()
    };

    std::vector<HWND> oldSlotHwnd(oldSlotCount, nullptr);
    for (uint32_t s = 0; s < oldSlotCount; ++s) {
        const uint32_t w = SlotWindowIndexFor(s, oldSlotCount,
                                              oldWindowCount, coverFlow);
        if (w < oldWindowCount)
            oldSlotHwnd[s] = m_windows[w].hwnd;
    }
    const HWND oldFront = m_windows.empty() ? nullptr : m_windows[0].hwnd;

    // ---- Everything this session knows about, visible or hidden -----------
    struct Entry {
        WindowInfo                  win;
        std::unique_ptr<WGCCapture> cap;
        size_t                      order   = 0;
        int                         oldSlot = -1;   // -1 = was not on screen
        bool                        visible = false;
    };
    std::vector<Entry> all;
    all.reserve(oldWindowCount + m_searchHidden.size());

    auto orderOf = [&](HWND h, size_t fallback) -> size_t {
        auto it = m_windowMeta.find(h);
        return it != m_windowMeta.end() ? it->second.order : fallback;
    };

    for (size_t i = 0; i < m_windows.size(); ++i) {
        Entry e;
        e.win     = m_windows[i];
        e.order   = orderOf(e.win.hwnd, i);
        e.oldSlot = WindowSlotIndexFor(i, oldSlotCount, oldWindowCount,
                                       coverFlow);
        e.visible = true;
        if (i < m_captures.size())
            e.cap = std::move(m_captures[i]);
        all.push_back(std::move(e));
    }
    for (auto& hidden : m_searchHidden) {
        Entry e;
        e.win   = hidden.win;
        e.cap   = std::move(hidden.capture);
        e.order = hidden.order;
        all.push_back(std::move(e));
    }
    m_searchHidden.clear();

    // ---- Verdicts ---------------------------------------------------------
    auto matches = [&](const Entry& e) {
        // The desktop pseudo-tile has no title of its own; give it the name
        // the label already shows so "desk" finds it like anything else.
        const std::wstring& title =
            (e.win.hwnd == m_desktopHwnd) ? kDesktopSearchName : e.win.title;
        std::wstring exe;
        if (m_config->searchMatchProcess) {
            auto it = m_windowMeta.find(e.win.hwnd);
            if (it != m_windowMeta.end())
                exe = it->second.exeLower;
        }
        return m_search.Matches(title, exe);
    };

    std::vector<size_t> keep, drop;
    keep.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i)
        (matches(all[i]) ? keep : drop).push_back(i);

    // Nothing matches is an ANSWER, not an error: the stack empties and the
    // field says so.  Anything else lies — leaving the previous result on
    // screen would tell the user their query matched windows it did not, and
    // that is exactly what "typing a word with no relation to anything open
    // still shows those windows" looked like.  The session survives the empty
    // stack (SearchHoldsEmptyStack) so one backspace brings it all back.
    m_searchNoMatch = keep.empty();

    // Nothing actually moves — a character that matches everything the stack
    // already holds, or (via the branch above) one that matches nothing.  Put
    // the arrays back untouched and stop: a reflow with identical endpoints
    // still looks like nothing, but it would block cycling for its full third
    // of a second while it played out.
    bool unchanged = (keep.size() == oldWindowCount);
    if (unchanged) {
        for (size_t i : keep) {
            if (!all[i].visible) { unchanged = false; break; }
        }
    }
    if (unchanged) {
        m_captures.clear();
        m_captures.reserve(oldWindowCount);
        for (size_t i = 0; i < all.size(); ++i) {
            if (all[i].visible) {
                m_captures.push_back(std::move(all[i].cap));
            } else {
                HiddenWindow hidden;
                hidden.win     = all[i].win;
                hidden.capture = std::move(all[i].cap);
                hidden.order   = all[i].order;
                m_searchHidden.push_back(std::move(hidden));
            }
        }
        return;   // m_windows was never modified
    }

    // ---- Order the survivors ---------------------------------------------
    // Canonical session order, then rotated so the current selection stays in
    // front.  If the selection was filtered out, the first match takes the
    // front — which is exactly what typing a name is asking for.
    std::sort(keep.begin(), keep.end(),
              [&](size_t a, size_t b) { return all[a].order < all[b].order; });
    if (oldFront) {
        for (size_t k = 0; k < keep.size(); ++k) {
            if (all[keep[k]].win.hwnd == oldFront) {
                std::rotate(keep.begin(), keep.begin() + k, keep.end());
                break;
            }
        }
    }

    // ---- Rebuild the visible stack ---------------------------------------
    m_windows.clear();
    m_captures.clear();
    m_windows.reserve(keep.size());
    m_captures.reserve(keep.size());
    for (size_t i : keep) {
        m_windows.push_back(all[i].win);
        m_captures.push_back(std::move(all[i].cap));
    }

    const bool animate = AnimCloseEnabled();
    std::vector<uint32_t> dyingSlotIdx;
    std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> dyingSRVs;

    for (size_t i : drop) {
        // Freeze the tile's current frame for the fade-out.  The capture goes
        // on running in m_searchHidden (one backspace may want it back), so
        // the fade must draw from a snapshot rather than from a stream that
        // keeps changing under it.
        if (animate && all[i].oldSlot >= 0) {
            dyingSlotIdx.push_back(static_cast<uint32_t>(all[i].oldSlot));
            dyingSRVs.push_back(all[i].cap
                ? SrvRef(all[i].cap->GetCurrentFrame())
                : nullptr);
        }
        HiddenWindow hidden;
        hidden.win     = all[i].win;
        hidden.capture = std::move(all[i].cap);
        hidden.order   = all[i].order;
        m_searchHidden.push_back(std::move(hidden));
    }

    // Ascending slot order, captures in step — what CloseAnimator::Begin and
    // the dying-tile draw both expect.
    {
        std::vector<size_t> order(dyingSlotIdx.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::sort(order.begin(), order.end(),
                  [&](size_t a, size_t b) {
                      return dyingSlotIdx[a] < dyingSlotIdx[b];
                  });
        std::vector<uint32_t> sortedIdx;
        std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> sortedSRVs;
        sortedIdx.reserve(order.size());
        sortedSRVs.reserve(order.size());
        for (size_t k : order) {
            sortedIdx.push_back(dyingSlotIdx[k]);
            sortedSRVs.push_back(std::move(dyingSRVs[k]));
        }
        dyingSlotIdx = std::move(sortedIdx);
        dyingSRVs    = std::move(sortedSRVs);
    }

    // ---- Rebuild the scene for the new count ------------------------------
    RECT rc{};
    GetClientRect(m_renderer.GetHwnd(), &rc);
    float vpW = static_cast<float>(rc.right - rc.left);
    float vpH = static_cast<float>(rc.bottom - rc.top);
    if (vpW <= 0) vpW = 1920.0f;
    if (vpH <= 0) vpH = 1080.0f;
    UpdateCascadeSpace(vpW, vpH);

    uint32_t displayCount = static_cast<uint32_t>(m_windows.size());
    if (m_config && m_config->maxWindows < displayCount)
        displayCount = m_config->maxWindows;
    m_scene.BuildSlots(displayCount, m_cascadeW, m_cascadeH);
    RebuildSceneAspects();
    // The camera moved with the count, so the entry-time flat rects cached
    // per HWND are expressed in a stale frame — see RemoveClosedWindows.
    m_entryExitAnimator.ClearEntryFlatCache();

    // ---- Animate the reflow ----------------------------------------------
    // The window order can change arbitrarily here (a restored window lands
    // wherever its session order puts it), so the animator always gets an
    // explicit per-slot source map — the ascending derivation it falls back
    // to only holds when the new row is a prefix of the old one.
    ClearClosingCaptures();
    if (animate) {
        for (auto& srv : dyingSRVs) {
            m_closingCaptures.push_back(nullptr);   // no capture to own here
            m_closingSRVs.push_back(std::move(srv));
        }

        const std::vector<int> newSlotSource = BuildSlotSourceMap(oldSlotHwnd);
        // riseIn: a window the filter let back in is RETURNING to a place it
        // fell out of, so it rises back into it.  The overflow refill's
        // back-spawn arrival would instead fly it in from behind the whole
        // stack, straight through every window standing between.
        m_closeAnim.Begin(m_scene, startSlots, dyingSlotIdx, oldCam,
                          &newSlotSource, /*riseIn*/ true);
    }
    // Close animation off: the scene already holds the rebuilt layout, which
    // IS the end state — nothing to animate toward.

    // The stack changed under the pointer — re-derive the highlight rather
    // than leaving it on a slot that now shows something else.
    m_hoverSlot = -1;
    m_hover.SetTarget(-1);
}

void FlipController::UpdateSearchBox()
{
    if (!m_config || !m_config->searchEnabled) {
        m_searchBox.Reset();
        return;
    }
    m_searchBox.Update(m_renderer.GetDevice(), m_search.Query(),
                       !m_searchNoMatch, m_config->appTheme,
                       m_config->searchBox,
                       static_cast<int>(m_config->searchScale),
                       m_cascadeH);
}

void FlipController::DrawSearchBox(ID3D11DeviceContext* ctx, float vpW,
                                   float vpH)
{
    if (!m_searchBox.Ready() || !ctx || vpW <= 0.0f || vpH <= 0.0f)
        return;

    const float w = static_cast<float>(m_searchBox.Width());
    const float h = static_cast<float>(m_searchBox.Height());

    // Placed on the cascade host (the primary display) from the user's own
    // percentages — X is the field's CENTRE, Y its BOTTOM edge — so one
    // setting reads the same on every resolution, which is exactly what the
    // Settings preview shows.
    const float primL = static_cast<float>(m_monLayout.primary.left)   - m_overlayOriginX;
    const float primT = static_cast<float>(m_monLayout.primary.top)    - m_overlayOriginY;
    const float primR = static_cast<float>(m_monLayout.primary.right)  - m_overlayOriginX;
    const float primB = static_cast<float>(m_monLayout.primary.bottom) - m_overlayOriginY;
    const float primW = primR - primL;
    const float primH = primB - primT;

    const float fx = (m_config ? m_config->searchPosX : 50u) / 100.0f;
    const float fy = (m_config ? m_config->searchPosY : 94u) / 100.0f;

    float left = primL + primW * fx - w * 0.5f;
    float top  = primT + primH * fy - h;

    // Keep it on the host whatever the percentages and the size add up to.
    const float margin = 6.0f;
    left = std::clamp(left, primL + margin,
                      (std::max)(primL + margin, primR - w - margin));
    top  = std::clamp(top, primT + margin,
                      (std::max)(primT + margin, primB - h - margin));

    QuadDrawCall draw;
    DirectX::XMStoreFloat4x4(&draw.mvp,
        DirectX::XMMatrixScaling((w / vpW) * 2.0f, (h / vpH) * 2.0f, 1.0f)
        * DirectX::XMMatrixTranslation(
              ((left + w * 0.5f) / vpW) * 2.0f - 1.0f,
              1.0f - ((top + h * 0.5f) / vpH) * 2.0f, 0.0f));
    draw.alpha      = 1.0f;
    draw.blurAmount = 0.0f;
    m_quad.Draw(ctx, m_searchBox.SRV(), draw);
}

// ---------------------------------------------------------------------------
// Selected-window label: title + program icon of the front-slot
// window, GDI-rendered once per selection change into a premultiplied BGRA
// pill texture and drawn as a screen-space quad above the front tile.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Window icons, resolved off the render thread.
//
// Most windows hand their icon over for nothing: GetClassLongPtrW reads class
// data with no cross-process message at all.  The ones that do not, meaning UWP
// frames, Electron shells and anything that answers WM_GETICON instead of
// registering a class icon, must never be asked from the render thread.
// SMTO_ABORTIFHUNG only returns early for a window the system has marked HUNG,
// so a merely BUSY message loop pays the full timeout twice over, and the
// cascade freezes for up to 120 ms on the frame after any cycle step that lands
// on such a window.  An eight-second profile of ordinary cycling caught a
// single 83 ms frame, all of it inside this call.
//
// So the question goes to a worker thread and the answer is kept for the
// session.  Activate primes the queue with every window in the stack, so the
// answers are in by the time the entry morph ends and the label appears; an
// unprimed label simply draws without an icon for a frame or two, then rebuilds
// once it lands.
//
// The cached HICONs are NOT owned, since a class icon belongs to the class and
// a WM_GETICON result to the window that answered, so the map needs clearing
// rather than a destruction pass.  Session-scoped all the same: HWNDs are
// recycled, and an icon handle outliving the window that vouched for it is
// exactly the stale answer this must not hand back.
// ---------------------------------------------------------------------------
namespace {

/// The icon a window publishes through its window class, or nullptr.
///
/// Free: class data is read from the kernel with no message to the owning
/// process, which is what makes it safe on the render thread and worth
/// asking before anything else.  It is also what decides whether a window
/// needs the worker at all — see StartIconResolver.
HICON ClassIcon(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return nullptr;
    if (HICON big = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON)))
        return big;
    return reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
}

struct IconResolver {
    std::mutex                      mtx;
    std::condition_variable         cv;
    std::unordered_map<HWND, HICON> answered;   // nullptr = asked, has none
    std::vector<HWND>               pending;
    std::atomic<unsigned>           generation{ 0 };
    std::atomic<bool>               stop{ false };
};

// Render thread only — the worker holds its own shared_ptr, so the object
// outlives this pointer being reset.
std::shared_ptr<IconResolver> g_iconResolver;

// The Desktop tile's icon comes from the shell, and the FIRST
// SHGetStockIconInfo in a process is dear: 82 ms, measured on the render
// thread during ordinary cycling — by a wide margin the longest single stall
// in the profile, and it landed the first time the Desktop tile reached the
// front slot.  Every later call still cost about 0.2 ms, paid again on each
// rebuild, because the answer was thrown away each time.
//
// Asked once, on the worker, and kept for the life of the process: a stock
// icon does not change, and one handle held forever is a fair price for
// never asking again.  Held NON-owned by its readers for the same reason —
// the cache owns it, so nothing destroys it after a draw.
std::mutex         g_stockIconMutex;
std::atomic<HICON> g_desktopStockIcon{ nullptr };
std::atomic<bool>  g_desktopStockResolved{ false };

void ResolveDesktopStockIcon()
{
    std::lock_guard<std::mutex> lock(g_stockIconMutex);
    if (g_desktopStockResolved.load(std::memory_order_relaxed))
        return;
    SHSTOCKICONINFO sii{};
    sii.cbSize = sizeof(sii);
    HICON icon = nullptr;
    if (SUCCEEDED(SHGetStockIconInfo(SIID_DESKTOPPC,
                                     SHGSI_ICON | SHGSI_LARGEICON, &sii)))
        icon = sii.hIcon;
    g_desktopStockIcon.store(icon, std::memory_order_release);
    g_desktopStockResolved.store(true, std::memory_order_release);
}

void IconResolverWorker(std::shared_ptr<IconResolver> r)
{
    // First job of the session: the one shell call that used to freeze a
    // frame outright.  Done before the queue so it is ready long before the
    // entry morph ends and the label first draws.
    ResolveDesktopStockIcon();
    r->generation.fetch_add(1, std::memory_order_release);

    for (;;) {
        HWND hwnd = nullptr;
        {
            std::unique_lock<std::mutex> lock(r->mtx);
            r->cv.wait(lock, [&r] {
                return r->stop.load(std::memory_order_acquire)
                    || !r->pending.empty();
            });
            if (r->stop.load(std::memory_order_acquire))
                return;
            hwnd = r->pending.back();
            r->pending.pop_back();
        }

        HICON icon = nullptr;
        if (hwnd && IsWindow(hwnd)) {
            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &result)
                && result)
                icon = reinterpret_cast<HICON>(result);
            else if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL2, 0,
                                         SMTO_ABORTIFHUNG | SMTO_BLOCK, 60,
                                         &result)
                     && result)
                icon = reinterpret_cast<HICON>(result);
        }

        {
            std::lock_guard<std::mutex> lock(r->mtx);
            r->answered[hwnd] = icon;
        }
        r->generation.fetch_add(1, std::memory_order_release);
    }
}

void StopIconResolver()
{
    if (!g_iconResolver)
        return;
    {
        std::lock_guard<std::mutex> lock(g_iconResolver->mtx);
        g_iconResolver->stop.store(true, std::memory_order_release);
    }
    g_iconResolver->cv.notify_all();
    // The worker may be a few tens of milliseconds into a SendMessageTimeout;
    // it owns a reference, finishes, sees the flag and leaves.  Nothing here
    // waits for it — a join would put the timeout it is serving back on the
    // teardown path, which is the stall this exists to remove.
    g_iconResolver.reset();
}

void StartIconResolver(const std::vector<HWND>& prime)
{
    StopIconResolver();
    auto resolver = std::make_shared<IconResolver>();

    // Only the windows that will actually be ASKED about.  GetLabelIcon
    // consults this resolver solely when a window publishes no class icon, so
    // priming the rest queued a cross-process WM_GETICON — up to two 60 ms
    // timeouts — for an answer nothing would ever read.  Wasted messages on
    // their own, but worse at the other end of the session: a worker still
    // grinding through them is a worker that cannot notice the stop flag and
    // leave.  Filtering is free, being the same class lookup the render
    // thread does first anyway.
    resolver->pending.reserve(prime.size());   // not shared yet — no lock needed
    for (HWND hwnd : prime)
        if (hwnd && !ClassIcon(hwnd))
            resolver->pending.push_back(hwnd);

    // A thread this cannot start must not throw its way into Activate.
    // Without a resolver every window still shows its class icon, which is
    // nearly all of them, and the few that answer WM_GETICON instead show
    // none — a cosmetic loss against losing the whole session.  Nothing is
    // recorded: a machine that cannot spawn one sleeping thread is already
    // reporting worse news through every other subsystem, and a diagnostic
    // entry about a missing label icon would only bury it.
    try {
        std::thread(IconResolverWorker, resolver).detach();
    } catch (...) {
        return;
    }

    g_iconResolver = std::move(resolver);
    g_iconResolver->cv.notify_all();
}

/// Answer for `hwnd`, or nullptr while the worker has yet to reach it.  A
/// window nobody has asked about is queued on the way past.
HICON ResolvedIcon(HWND hwnd)
{
    if (!g_iconResolver)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_iconResolver->mtx);
    auto it = g_iconResolver->answered.find(hwnd);
    if (it != g_iconResolver->answered.end())
        return it->second;
    if (std::find(g_iconResolver->pending.begin(),
                  g_iconResolver->pending.end(), hwnd)
        == g_iconResolver->pending.end()) {
        g_iconResolver->pending.push_back(hwnd);
        g_iconResolver->cv.notify_one();
    }
    return nullptr;
}

/// True while `hwnd` has no answer yet — the label built without its icon
/// has to be rebuilt when one arrives, and this is what tells it apart from
/// a window that genuinely has no icon to show.
bool IconStillPending(HWND hwnd, HWND desktopHwnd)
{
    if (hwnd && hwnd == desktopHwnd)
        return !g_desktopStockResolved.load(std::memory_order_acquire);
    if (!g_iconResolver)
        return false;
    std::lock_guard<std::mutex> lock(g_iconResolver->mtx);
    return g_iconResolver->answered.find(hwnd)
        == g_iconResolver->answered.end();
}

unsigned IconGeneration()
{
    return g_iconResolver
        ? g_iconResolver->generation.load(std::memory_order_acquire) : 0u;
}

} // namespace

/// Best-available program icon for a window, or nullptr.  Never blocks and
/// never transfers ownership: a class icon belongs to its class, a WM_GETICON
/// answer to the window that gave it, and the desktop's stock icon to the
/// process-lifetime cache above.  The caller destroys nothing.
static HICON GetLabelIcon(HWND hwnd, HWND desktopHwnd)
{
    if (hwnd && hwnd == desktopHwnd) {
        // Owned by the process-lifetime cache, never by this caller — see
        // the note there.  nullptr only in the first frames of the very
        // first session, while the worker is still fetching it.
        return g_desktopStockIcon.load(std::memory_order_acquire);
    }
    if (!hwnd || !IsWindow(hwnd))
        return nullptr;

    // Class icon first — free, and the answer for nearly every window.
    if (HICON cls = ClassIcon(hwnd))
        return cls;

    // No class icon: the answer is whatever the worker has learned by asking
    // the window itself.  Never asked from here — see the note above the
    // resolver.
    return ResolvedIcon(hwnd);
}

void FlipController::ResetSelectedLabel()
{
    m_labelSRV     = nullptr;
    m_labelTexture = nullptr;
    m_labelHwnd    = nullptr;
    m_labelTitle.clear();
    m_labelTexW = 0;
    m_labelTexH = 0;
    m_labelIconPending = false;
    m_labelIconGen     = 0;
    m_labelAnim.Reset();
}

void FlipController::UpdateSelectedLabel()
{
    const bool master    = !m_config || m_config->selectedLabel;
    const bool showTitle = !m_config || m_config->selectedLabelTitle;
    const bool showIcon  = !m_config || m_config->selectedLabelIcon;
    const bool showBox   = !m_config || m_config->selectedLabelBox;
    if (!master || (!showTitle && !showIcon) || m_windows.empty()) {
        if (m_labelSRV)
            ResetSelectedLabel();
        return;
    }

    HWND selected = m_windows[0].hwnd;
    // The scan-time title is used as the session-stable key — polling
    // GetWindowTextW per frame would SendMessage into foreign (possibly
    // hung) processes from the render loop.
    const std::wstring& title =
        (selected == m_desktopHwnd) ? L"Desktop" : m_windows[0].title;

    const int theme = m_config ? std::clamp(m_config->appTheme, 0, 4) : 0;
    // An icon the resolver had not answered for yet leaves the texture
    // provisional: the generation counter moving means SOME answer landed,
    // which is the cheapest possible cue to look again.  A window that
    // genuinely has no icon clears the pending flag, so this settles.
    const bool iconAnswerArrived =
        m_labelIconPending && IconGeneration() != m_labelIconGen;
    if (m_labelSRV && selected == m_labelHwnd && title == m_labelTitle
        && showTitle == m_labelShowTitle && showIcon == m_labelShowIcon
        && showBox == m_labelShowBox && theme == m_labelTheme
        && !iconAnswerArrived)
        return;   // up to date

    if (!BuildSelectedLabelTexture(selected, title, showTitle, showIcon,
                                   showBox)) {
        // Keep the smoothed anchor — only the texture failed; releasing
        // the animator here would make the next successful build snap.
        m_labelSRV     = nullptr;
        m_labelTexture = nullptr;
        m_labelHwnd    = nullptr;
        m_labelTitle.clear();
        m_labelTexW = 0;
        m_labelTexH = 0;
    }
}

bool FlipController::BuildSelectedLabelTexture(HWND hwnd,
                                               const std::wstring& title,
                                               bool showTitle, bool showIcon,
                                               bool showBox)
{
    ID3D11Device* device = m_renderer.GetDevice();
    if (!device)
        return false;

    const int theme = m_config ? std::clamp(m_config->appTheme, 0, 4) : 0;
    // Shared with the search field (core/ThemePlate) so both pieces of
    // on-screen chrome are the same object in the same theme.
    const ThemePlate::Style& st = ThemePlate::Get(theme);

    // UI scale keys off the cascade host height so the label has the same
    // physical presence at 1080p and 4K.
    const float uiScale = std::clamp(m_cascadeH / 1080.0f, 1.0f, 2.5f);
    const int padX     = static_cast<int>(16.0f * uiScale);
    const int padY     = static_cast<int>(10.0f * uiScale);
    const int iconSide = static_cast<int>(26.0f * uiScale);
    const int gap      = static_cast<int>(10.0f * uiScale);
    const int fontH    = static_cast<int>(17.0f * uiScale);
    const int maxTextW = static_cast<int>(m_cascadeW * 0.45f);

    HICON icon = showIcon ? GetLabelIcon(hwnd, m_desktopHwnd) : nullptr;
    // Record whether this texture is standing in for an icon still being
    // resolved — UpdateSelectedLabel rebuilds once the answer arrives.
    m_labelIconGen     = IconGeneration();
    m_labelIconPending = showIcon && !icon
                      && IconStillPending(hwnd, m_desktopHwnd);
    const bool haveIcon = icon != nullptr;
    const bool haveText = showTitle && !title.empty();
    if (!haveIcon && !haveText)
        return false;

    HDC screenDC = GetDC(nullptr);
    HDC memDC    = CreateCompatibleDC(screenDC);
    // Grayscale antialiasing on purpose: the glyphs are rendered as a
    // white-on-black coverage MASK and composited manually — ClearType's
    // per-channel fringes would bleed color into the mask.
    HFONT font   = CreateFontW(-fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(memDC, font);

    int textW = 0, textH = 0;
    if (haveText) {
        RECT calc{ 0, 0, maxTextW, 0 };
        DrawTextW(memDC, title.c_str(), -1, &calc,
                  DT_SINGLELINE | DT_CALCRECT);
        textW = std::min<int>(calc.right, maxTextW);
        textH = calc.bottom;
    }

    const int contentH = (std::max)(haveIcon ? iconSide : 0, textH);
    const int width  = padX + (haveIcon ? iconSide : 0)
                     + ((haveIcon && haveText) ? gap : 0)
                     + textW + padX;
    const int height = padY + contentH + padY;

    auto makeDib = [&](int w, int h, void** outBits) -> HBITMAP {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h;   // top-down
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        return CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, outBits,
                                nullptr, 0);
    };

    // ---- Compose in a premultiplied float buffer (B,G,R,A per pixel) ----
    const size_t pxCount = static_cast<size_t>(width) * height;
    std::vector<float> out(pxCount * 4u, 0.0f);
    auto over = [&](size_t idx, float b, float g, float r, float a) {
        ThemePlate::Over(out, idx, b, g, r, a);
    };

    // 1. Theme plate — vertical gradient with an optional top sheen, a
    //    1-px border and rounded corners, all parameterised by the active
    //    CKSettings theme.  Translucent so the cascade shows through,
    //    tinted for text contrast.
    if (showBox)
        ThemePlate::PaintPlate(out, width, height, st, uiScale);

    // 2. Icon — drawn into its own zeroed DIB so DrawIconEx preserves the
    //    icon's per-pixel alpha (same trick as the tray-menu bitmaps in
    //    app.cpp), then composited premultiplied over the glass.
    if (haveIcon) {
        void* iconBits = nullptr;
        HBITMAP iconDib = makeDib(iconSide, iconSide, &iconBits);
        if (iconDib && iconBits) {
            HGDIOBJ prev = SelectObject(memDC, iconDib);
            DrawIconEx(memDC, 0, 0, icon, iconSide, iconSide, 0, nullptr,
                       DI_NORMAL);
            GdiFlush();
            SelectObject(memDC, prev);

            auto* ip = static_cast<uint8_t*>(iconBits);
            // Legacy mask icons leave alpha at 0 while writing colors —
            // detect that and fall back to "any color ⇒ opaque".
            bool anyAlpha = false;
            for (size_t i = 0; i < static_cast<size_t>(iconSide) * iconSide; ++i)
                if (ip[i * 4u + 3]) { anyAlpha = true; break; }

            const int ix0 = padX;
            const int iy0 = (height - iconSide) / 2;
            for (int yy = 0; yy < iconSide; ++yy) {
                for (int xx = 0; xx < iconSide; ++xx) {
                    const uint8_t* p =
                        ip + (static_cast<size_t>(yy) * iconSide + xx) * 4u;
                    float a = anyAlpha
                        ? p[3] / 255.0f
                        : ((p[0] | p[1] | p[2]) ? 1.0f : 0.0f);
                    if (a <= 0.0f)
                        continue;
                    float b = p[0] / 255.0f;
                    float g = p[1] / 255.0f;
                    float r = p[2] / 255.0f;
                    if (!anyAlpha) { /* straight color, a = 1 → already premult */ }
                    over(static_cast<size_t>(iy0 + yy) * width + (ix0 + xx),
                         b, g, r, a);
                }
            }
        }
        if (iconDib) DeleteObject(iconDib);
    }

    // 3. Title — rendered as a white-on-black coverage mask, composited as
    //    a soft dark drop shadow (readability with the box off) plus the
    //    white glyph pass.
    if (haveText) {
        void* textBits = nullptr;
        HBITMAP textDib = makeDib(width, height, &textBits);
        if (textDib && textBits) {
            HGDIOBJ prev = SelectObject(memDC, textDib);
            // DIB sections start zeroed = black background for the mask.
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(255, 255, 255));
            const int tx = padX + (haveIcon ? iconSide + gap : 0);
            RECT tr{ tx, padY, tx + textW, height - padY };
            DrawTextW(memDC, title.c_str(), -1, &tr,
                      DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS
                      | DT_NOPREFIX);
            GdiFlush();
            SelectObject(memDC, prev);

            ThemePlate::CompositeTextMask(out, width, height,
                                          static_cast<uint8_t*>(textBits),
                                          st, uiScale, showBox);
        }
        if (textDib) DeleteObject(textDib);
    }

    // ---- Pack to premultiplied BGRA bytes and upload -----------------------
    std::vector<uint8_t> packed;
    ThemePlate::Pack(out, packed);

    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    bool ok = ThemePlate::CreateTexture(device, packed, width, height,
                                        tex, srv);

    SelectObject(memDC, oldFont);
    DeleteObject(font);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    // No DestroyIcon: GetLabelIcon never hands over ownership (see its note).

    if (!ok)
        return false;

    m_labelTexture  = std::move(tex);
    m_labelSRV      = std::move(srv);
    m_labelHwnd     = hwnd;
    m_labelTitle    = title;
    m_labelShowTitle = showTitle;
    m_labelShowIcon  = showIcon;
    m_labelShowBox   = showBox;
    m_labelTheme     = theme;
    m_labelTexW = width;
    m_labelTexH = height;
    return true;
}

void FlipController::DrawSelectedLabel(ID3D11DeviceContext* ctx, float vpW,
                                       float vpH, DirectX::XMMATRIX monRemap)
{
    if (!m_labelSRV || !ctx || vpW <= 0.0f || vpH <= 0.0f)
        return;
    if (m_scene.SlotCount() == 0)
        return;

    // Hold detection: held-key rapid cycling (queued commands or a chained
    // animation) fades the label out instead of letting it dart between
    // poses; it fades back in at the FINAL position once the hold ends —
    // CycleStop clears the queue and SwitchToDecel drops the chained flag,
    // so the fade-in already runs during the deceleration animation.
    //
    // A free-drag scrub (Window snap off) runs on the chained flag too — it
    // wants the Linear inner easing — but a hand walking the stack is the
    // deliberate case, not the runaway one, so it must NOT hide the label.
    // Only genuine speed does, which is the same thing a held key expresses.
    //
    // Every term is a statement about a cycle that is RUNNING, so each one is
    // gated on the animator actually being active.  The chained flag was not,
    // and it outlived its animation: a click on a back tile spins the stack
    // with chained steps, the last of which is cancelled by the teardown, and
    // the flag stayed set into the NEXT session — which opened with its label
    // faded out and no way back but a keyboard cycle, because only Begin()
    // ever cleared the flag.  It is cleared by Cancel() now as well; asking
    // IsActive() here makes the same mistake impossible to reintroduce.
    const bool cycling    = m_cycleAnim.IsActive();
    const bool rapidScrub = cycling && m_cycleAnim.IsScrubbing()
                         && std::fabs(m_scrubVelocity) > kLabelHoldVel;
    const bool suppressed = !m_cycleQueue.empty()
                         || (cycling && m_cycleAnim.IsChained()
                             && !m_cycleAnim.IsScrubbing())
                         || rapidScrub;
    // Label animation toggle (Appearance → Animations dropdown): off =
    // instant position snap, instant show/hide instead of the fades.
    const bool animate = AnimLabelEnabled();

    // Target anchor: projected bounds of the front slot's REST pose.
    // While a cycle is in flight the target is the cycle's destination
    // slot 0 — projecting the live animated slot would ride the wrap
    // tile's journey, and on backward cycles that path STARTS at the back
    // of the cascade (the label dove toward the far end, then swept
    // forward).  During the close reflow the previous target is kept.
    {
        // The pointer-hover lift is a DRAW offset (see HoverAnimator), so the
        // scene's slot 0 describes the front tile as if it were still flat.
        // The label anchors to the tile's TOP edge — read the rest pose while
        // the tile is up and the pill ends up printed across the window it is
        // labelling.  It takes the same offset the draw pass applies, so the
        // gap between tile and pill is the one constant of the pair.
        //
        // Only the settled branch needs it: while a cycle runs the anchor
        // comes from the destination REST pose and nothing is lifted (the
        // lift has already fallen — see HoverDropGate).
        float frontLift = 0.0f;
        if (m_hover.AnyLift() && !m_entryExitAnimator.IsActive()) {
            const float l = m_hover.Lift(0);
            if (l > 0.001f)
                frontLift = l * m_scene.GetSlot(0).scaleY
                          * HoverAnimator::kRiseFactor;
        }
        using namespace DirectX;
        XMMATRIX mvp{};
        bool haveMvp = false;

        if (m_cycleAnim.IsActive()) {
            if (const TileSlot* t = m_cycleAnim.GetTargetSlot(0)) {
                // Same inline MVP construction the overflow/dying tiles
                // use — faithful to FlipScene::GetDrawCall geometry.
                XMMATRIX world =
                    XMMatrixScaling(t->scaleX, t->scaleY, 1.0f) *
                    XMMatrixRotationX(XMConvertToRadians(m_scene.GetSceneTiltX())) *
                    XMMatrixRotationY(XMConvertToRadians(m_scene.GetSceneTiltY() + t->rotY)) *
                    XMMatrixTranslation(t->x, t->y, t->z);
                XMMATRIX view, proj;
                m_scene.CameraMatrices(m_cascadeAspect, view, proj);
                mvp = world * view * proj * monRemap;
                haveMvp = true;
            }
        } else if (!m_closeAnim.IsActive()) {
            QuadDrawCall probe;
            float a = 0.0f;
            m_scene.GetDrawCall(0, m_cascadeAspect, probe.mvp, a, frontLift);
            mvp = XMLoadFloat4x4(&probe.mvp) * monRemap;
            haveMvp = true;
        }

        if (haveMvp) {
            float minX = 0, minY = 0, maxX = 0, maxY = 0;
            bool first = true, valid = true;
            const float cx[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
            const float cy[4] = { -0.5f,  0.5f, 0.5f, -0.5f };
            for (int i = 0; i < 4; ++i) {
                XMVECTOR v = XMVector3TransformCoord(
                    XMVectorSet(cx[i], cy[i], 0.0f, 1.0f), mvp);
                const float ndcX = XMVectorGetX(v);
                const float ndcY = XMVectorGetY(v);
                if (!std::isfinite(ndcX) || !std::isfinite(ndcY)) {
                    valid = false;
                    break;
                }
                const float sx = (ndcX * 0.5f + 0.5f) * vpW;
                const float sy = (0.5f - ndcY * 0.5f) * vpH;
                if (first) { minX = maxX = sx; minY = maxY = sy; first = false; }
                else {
                    minX = (std::min)(minX, sx); maxX = (std::max)(maxX, sx);
                    minY = (std::min)(minY, sy); maxY = (std::max)(maxY, sy);
                }
            }
            if (valid && maxX > minX && maxY > minY)
                m_labelAnim.Update((minX + maxX) * 0.5f, minY, suppressed,
                                   animate);
            else
                haveMvp = false;
        }
        if (!haveMvp && m_labelAnim.HasPos()) {
            // No fresh target this frame (close reflow / degenerate
            // projection) — hold position, keep the fade advancing.
            m_labelAnim.Update(m_labelAnim.X(), m_labelAnim.Y(), suppressed,
                               animate);
        }
    }
    if (!m_labelAnim.HasPos() || m_labelAnim.Alpha() < 0.01f)
        return;

    const float uiScale = std::clamp(m_cascadeH / 1080.0f, 1.0f, 2.5f);
    const float w = static_cast<float>(m_labelTexW);
    const float h = static_cast<float>(m_labelTexH);

    // Primary-monitor bounds in overlay space keep the pill on the cascade
    // host regardless of virtual-screen origin.  Clamping happens on the
    // SMOOTHED anchor so the clamp itself never causes a jump — the
    // animator glides, the clamp merely bounds the result.
    const float primL = static_cast<float>(m_monLayout.primary.left)  - m_overlayOriginX;
    const float primT = static_cast<float>(m_monLayout.primary.top)   - m_overlayOriginY;
    const float primR = static_cast<float>(m_monLayout.primary.right) - m_overlayOriginX;
    const float margin = 8.0f * uiScale;

    float left = m_labelAnim.X() - w * 0.5f;
    left = std::clamp(left, primL + margin, (std::max)(primL + margin, primR - w - margin));
    float top = m_labelAnim.Y() - h - 14.0f * uiScale;
    if (top < primT + margin)
        top = primT + margin;

    QuadDrawCall draw;
    DirectX::XMStoreFloat4x4(&draw.mvp,
        DirectX::XMMatrixScaling((w / vpW) * 2.0f, (h / vpH) * 2.0f, 1.0f)
        * DirectX::XMMatrixTranslation(
              ((left + w * 0.5f) / vpW) * 2.0f - 1.0f,
              1.0f - ((top + h * 0.5f) / vpH) * 2.0f, 0.0f));
    draw.alpha      = m_labelAnim.Alpha();
    draw.blurAmount = 0.0f;
    m_quad.Draw(ctx, m_labelSRV.get(), draw);
}
