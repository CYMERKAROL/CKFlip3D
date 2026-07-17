#include "windowcloaker.h"
#include "../core/DebugLog.h"

namespace WindowCloaker {
namespace {

// DWMWA_CLOAK is not in the public SDK headers (undocumented attribute 13).
// It has been stable across Win 8 → Win 11 24H2.
constexpr DWORD DWMWA_CLOAK_VALUE = 13;

// We only track which HWNDs we cloaked via DWM.
std::vector<HWND> g_cloakedWindows;

// ---------------------------------------------------------------------------
// Class-name blacklist — shell infrastructure and system surfaces that must
// NEVER be cloaked (desktop, taskbar, compositor overlays, etc.).
// ---------------------------------------------------------------------------
bool IsClassBlacklisted(const wchar_t* cls)
{
    // Core DWM / Shell infrastructure
    if (wcscmp(cls, L"Progman") == 0)                     return true;
    if (wcscmp(cls, L"WorkerW") == 0)                     return true;
    if (wcscmp(cls, L"Shell_TrayWnd") == 0)               return true;
    if (wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0)      return true;
    if (wcscmp(cls, L"DV2ControlHost") == 0)              return true;
    if (wcscmp(cls, L"DesktopWindowXamlSource") == 0)     return true;
    if (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0)  return true;

    // System overlays / UI
    if (wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0)                      return true;
    if (wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) return true;
    if (wcscmp(cls, L"ApplicationManager_ImmersiveShellWindow") == 0)           return true;
    if (wcscmp(cls, L"ImmersiveLauncher") == 0)           return true;
    if (wcscmp(cls, L"SearchUI") == 0)                    return true;
    if (wcscmp(cls, L"SearchHost") == 0)                  return true;

    // Task switcher / snap layouts
    if (wcscmp(cls, L"MSTaskSwWClass") == 0)              return true;
    if (wcscmp(cls, L"MultitaskingViewFrame") == 0)       return true;

    // System tray overflow
    if (wcscmp(cls, L"NotifyIconOverflowWindow") == 0)    return true;

    // Touch keyboard / input
    if (wcscmp(cls, L"Windows.UI.Input.InputSite.WindowClass") == 0)  return true;

    // DWM / compositor / ghost
    if (wcscmp(cls, L"Ghost") == 0)                       return true;
    if (wcscmp(cls, L"#32768") == 0)                      return true;
    if (wcscmp(cls, L"SysShadow") == 0)                   return true;
    if (wcscmp(cls, L"tooltips_class32") == 0)            return true;

    // Our own DWM thumbnail helper
    if (wcscmp(cls, L"CKFlipDwmThumbnailHelper") == 0)    return true;

    return false;
}

// ---------------------------------------------------------------------------
// Check if a window should be cloaked based on strict Alt-Tab-like criteria.
//
// Criteria:
//   - WS_VISIBLE
//   - No WS_EX_TOOLWINDOW
//   - No owner UNLESS WS_EX_APPWINDOW is set (some dialog-based apps use
//     owned windows with WS_EX_APPWINDOW to appear in Alt-Tab)
//   - Not transparent overlay (WS_EX_TRANSPARENT + WS_EX_LAYERED)
//   - Not system-cloaked (DWMWA_CLOAKED != 0)
//   - Not a blacklisted shell/system class
//   - Root ancestor is not WorkerW (Wallpaper Engine, shell children)
//   - Has nonzero screen rect (invisible but "visible" windows)
//   - Not in the exclude list
// ---------------------------------------------------------------------------
bool ShouldCloak(HWND hwnd, DWORD myPid, const std::vector<HWND>& exclude)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    // Must be visible.
    if (!IsWindowVisible(hwnd))
        return false;

    // Skip our own process.
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == myPid)
        return false;

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    // Must NOT have WS_EX_TOOLWINDOW.
    if (exStyle & WS_EX_TOOLWINDOW)
        return false;

    // Skip transparent overlay windows (snipping tool, screen dimmer, etc.)
    // but NOT if WS_EX_APPWINDOW is set — those are real app windows.
    if ((exStyle & WS_EX_TRANSPARENT) && (exStyle & WS_EX_LAYERED)
        && !(exStyle & WS_EX_APPWINDOW))
        return false;

    // Owner check: skip owned windows UNLESS they have WS_EX_APPWINDOW.
    // Some dialog-based apps (e.g. certain media players, chat windows)
    // set WS_EX_APPWINDOW on an owned window to appear in Alt-Tab.
    HWND owner = GetWindow(hwnd, GW_OWNER);
    if (owner != nullptr && !(exStyle & WS_EX_APPWINDOW))
        return false;

    // Must NOT be system-cloaked (virtual desktops, suspended UWP).
    DWORD cloakedVal = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
                                        &cloakedVal, sizeof(cloakedVal)))
        && cloakedVal != 0)
        return false;

    // Class-name blacklist.
    wchar_t cls[256] = {};
    GetClassNameW(hwnd, cls, 256);
    if (IsClassBlacklisted(cls))
        return false;

    // Root ancestor check: if the root is WorkerW, this is a shell child
    // (Wallpaper Engine, desktop sidebar, etc.) — never cloak.
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root && root != hwnd) {
        wchar_t rootCls[256] = {};
        GetClassNameW(root, rootCls, 256);
        if (wcscmp(rootCls, L"WorkerW") == 0)
            return false;
        if (wcscmp(rootCls, L"Progman") == 0)
            return false;
    }

    // Skip near-zero-size windows — technically "visible" but nothing on screen.
    // Use threshold of 1 pixel, and skip the check for minimized windows
    // (which report their minimized position, not actual size).
    if (!IsIconic(hwnd)) {
        RECT rc;
        if (GetWindowRect(hwnd, &rc)) {
            if ((rc.right - rc.left) <= 1 || (rc.bottom - rc.top) <= 1)
                return false;
        }
    }

    // Exclude list (our overlay, desktop pseudo-window, etc.).
    for (HWND ex : exclude) {
        if (ex == hwnd)
            return false;
    }

    return true;
}

bool DoCloakWindow(HWND hwnd)
{
    // Check if already tracked.
    for (HWND h : g_cloakedWindows) {
        if (h == hwnd)
            return true;
    }

    BOOL cloakVal = TRUE;
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK_VALUE,
                                       &cloakVal, sizeof(cloakVal));
    if (SUCCEEDED(hr)) {
        g_cloakedWindows.push_back(hwnd);
        return true;
    }

    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, 128);
    wchar_t buf[256];
    swprintf_s(buf, L"CKFlip CLOAK: DWM failed for %p [%s] (hr=0x%08X)\n",
               (void*)hwnd, cls, (unsigned)hr);
    CKLog::Log(buf);
    return false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Full enumeration: find and cloak ALL visible app windows.
// Does NOT call UncloakAll — caller is responsible for that if needed.
// Safe to call repeatedly: DoCloakWindow skips already-tracked HWNDs.
// ---------------------------------------------------------------------------
int CloakVisibleAppWindows(DWORD myPid, const std::vector<HWND>& exclude)
{
    struct EnumCtx {
        DWORD myPid;
        const std::vector<HWND>* exclude;
        int count;
    };
    EnumCtx ctx{ myPid, &exclude, 0 };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lParam);
        if (ShouldCloak(hwnd, c->myPid, *c->exclude)) {
            if (DoCloakWindow(hwnd))
                ++c->count;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.count > 0) {
        wchar_t buf[128];
        swprintf_s(buf, L"CKFlip CLOAK: %d windows hidden (sweep)\n", ctx.count);
        CKLog::Log(buf);
    }
    return ctx.count;
}

void UncloakAll()
{
    for (HWND hwnd : g_cloakedWindows) {
        if (!IsWindow(hwnd))
            continue;
        BOOL uncloakVal = FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK_VALUE,
                              &uncloakVal, sizeof(uncloakVal));
    }

    if (!g_cloakedWindows.empty()) {
        wchar_t buf[128];
        swprintf_s(buf, L"CKFlip UNCLOAK: Released %zu windows\n",
                   g_cloakedWindows.size());
        CKLog::Log(buf);
    }
    g_cloakedWindows.clear();
}

void ForceUncloakEverything()
{
    UncloakAll();

    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (!IsWindow(hwnd))
            return TRUE;
        BOOL uncloakVal = FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_CLOAK_VALUE,
                              &uncloakVal, sizeof(uncloakVal));
        return TRUE;
    }, 0);

    CKLog::Log(L"CKFlip: ForceUncloakEverything completed\n");
}

} // namespace WindowCloaker
