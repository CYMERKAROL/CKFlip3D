#include "windowscanner.h"
#include <dwmapi.h>

namespace WindowScanner {
namespace {

struct EnumContext {
    DWORD                   ownerPid;
    std::vector<WindowInfo> results;
};

bool IsClassBlacklisted(HWND hwnd)
{
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);

    // Shell UI surfaces that should never appear in the switcher.
    if (wcscmp(cls, L"Shell_TrayWnd") == 0)          return true;
    if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0)  return true;
    if (wcscmp(cls, L"Progman") == 0)                 return true;
    if (wcscmp(cls, L"WorkerW") == 0)                 return true;
    if (wcscmp(cls, L"Shell_InputSwitchTopLevelWindow") == 0) return true;

    // Windows.UI.Core.CoreWindow is the raw UWP container — the visible
    // UWP windows use ApplicationFrameWindow and are handled normally.
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0) return true;

    return false;
}

/// For ApplicationFrameWindow (UWP host), verify the frame actually has
/// a content window.  Empty frames (no CoreWindow child) should be skipped.
bool HasUWPContent(HWND hwnd)
{
    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    if (wcscmp(cls, L"ApplicationFrameWindow") != 0)
        return true;   // not a UWP frame — don't filter

    // Look for a Windows.UI.Core.CoreWindow child.  If found, the frame
    // hosts real UWP content.  If not, it's an empty shell.
    HWND child = FindWindowExW(hwnd, nullptr,
                               L"Windows.UI.Core.CoreWindow", nullptr);
    return child != nullptr;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam)
{
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);

    // Must be visible (minimized windows are still "visible" in the WS sense).
    if (!IsWindowVisible(hwnd))
        return TRUE;

    // Skip our own process.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->ownerPid)
        return TRUE;

    // ---- Alt-Tab eligibility heuristics -----------------------------------
    HWND owner = GetWindow(hwnd, GW_OWNER);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_TOOLWINDOW)
        return TRUE;

    // Skip transparent overlay windows (snipping tool, overlays, etc.)
    if ((exStyle & WS_EX_TRANSPARENT) && (exStyle & WS_EX_LAYERED))
        return TRUE;

    if (owner != nullptr && !(exStyle & WS_EX_APPWINDOW))
        return TRUE;

    // Skip windows cloaked by the system (virtual desktops, suspended UWP).
    // NOTE: we do NOT skip windows cloaked by us (DWMWA_CLOAK attr 13),
    // because DwmGetWindowAttribute DWMWA_CLOAKED returns nonzero only for
    // system-level cloaking (DWM_CLOAKED_APP | DWM_CLOAKED_SHELL |
    // DWM_CLOAKED_INHERITED).  Our manual cloak sets a different path.
    DWORD cloakedVal = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloakedVal, sizeof(cloakedVal)))
        && cloakedVal != 0)
        return TRUE;

    // Class-level blacklist.
    if (IsClassBlacklisted(hwnd))
        return TRUE;

    // UWP frames without content (no CoreWindow child) are empty shells.
    if (!HasUWPContent(hwnd))
        return TRUE;

    // Must have a non-empty title.
    int titleLen = GetWindowTextLengthW(hwnd);
    if (titleLen <= 0)
        return TRUE;

    WindowInfo info;
    info.hwnd      = hwnd;
    info.minimized = (IsIconic(hwnd) != FALSE);
    info.title.resize(static_cast<size_t>(titleLen));
    GetWindowTextW(hwnd, info.title.data(), titleLen + 1);

    if (info.minimized) {
        // For minimized windows, determine the rect that would be used
        // if the window were restored.
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(hwnd, &wp)) {
            if (wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED)) {
                // Window was maximized before being minimized.
                // rcNormalPosition gives the RESTORED (non-maximized) rect,
                // which is wrong — use the monitor's work area instead.
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMon, &mi)) {
                    info.rect = mi.rcWork;
                } else {
                    // Fallback to primary desktop.
                    GetClientRect(GetDesktopWindow(), &info.rect);
                }
            } else {
                // Use DWM extended frame bounds on the normal position for
                // accurate sizing (strips invisible Win10/11 borders).
                info.rect = wp.rcNormalPosition;
            }
        } else {
            info.rect = { 0, 0, 800, 450 };
        }
    } else {
        // Use DWM extended frame bounds to get the actual visible rect.
        // GetWindowRect includes invisible Win10/11 borders (~7px each side)
        // which inflates width and distorts the aspect ratio.
        RECT dwmRect = {};
        HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                           &dwmRect, sizeof(dwmRect));
        if (SUCCEEDED(hr) && (dwmRect.right - dwmRect.left) > 0
                          && (dwmRect.bottom - dwmRect.top) > 0) {
            info.rect = dwmRect;
        } else {
            GetWindowRect(hwnd, &info.rect);
        }
    }

    ctx->results.push_back(std::move(info));
    return TRUE;
}

// Callback used to find the window that hosts SHELLDLL_DefView (the desktop).
// This is the definitive desktop surface — it works for both static wallpapers
// and slideshows.  On slideshow desktops Windows moves SHELLDLL_DefView from
// Progman into a WorkerW, so we must enumerate to find the actual host.
HWND g_desktopHost = nullptr;

BOOL CALLBACK FindDesktopHost(HWND hwnd, LPARAM /*lParam*/)
{
    HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView) {
        // This hwnd (could be Progman or a WorkerW) is hosting the live
        // desktop view — this is the one we want to thumbnail.
        g_desktopHost = hwnd;
        return FALSE;   // found it, stop enumeration
    }
    return TRUE;
}

} // anonymous namespace

std::vector<WindowInfo> Enumerate(DWORD ownerPid)
{
    EnumContext ctx;
    ctx.ownerPid = ownerPid;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return std::move(ctx.results);
}

HWND FindDesktopWindow()
{
    // Find the top-level window that hosts SHELLDLL_DefView.  This is the
    // definitive desktop surface — works for static wallpapers AND slideshows.
    g_desktopHost = nullptr;
    EnumWindows(FindDesktopHost, 0);

    if (g_desktopHost)
        return g_desktopHost;

    // Fallback: Progman (always exists, may not host the view on slideshows).
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman)
        return progman;

    return GetShellWindow();
}

} // namespace WindowScanner
