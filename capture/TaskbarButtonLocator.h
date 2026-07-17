#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <oleacc.h>

/// Looks up the screen-space rect of a window's taskbar button via MSAA
/// (IAccessible).  Used by the entry animator so minimized windows emerge
/// from their actual taskbar buttons (Win7 behaviour).
///
/// MSAA is used in preference to UI Automation because the Windows SDK
/// 10.0.26100 ships with broken UIAutomationCore.h forward declarations.
/// MSAA gives equivalent per-button accuracy on the running-task list.
///
/// Lifetime: Init() once per Flip3D session, Shutdown() on dismiss/escape.
class TaskbarButtonLocator {
public:
    TaskbarButtonLocator() = default;
    ~TaskbarButtonLocator() { Shutdown(); }

    TaskbarButtonLocator(const TaskbarButtonLocator&) = delete;
    TaskbarButtonLocator& operator=(const TaskbarButtonLocator&) = delete;

    /// Initialise OLE + cache the IAccessible for the taskbar button list.
    /// Returns true if the locator can answer queries.  Safe to call again
    /// after Shutdown().
    bool Init();

    /// Release IAccessible / OLE state.  Safe to call when not initialised.
    void Shutdown();

    /// Returns true if Init() found the running-task-list HWND.  The
    /// IAccessible may still be missing (Win11 XAML taskbar often returns
    /// nothing useful from MSAA) — GetButtonRect will simply return false
    /// for per-button lookups in that case, but GetButtonListRect still
    /// returns the list rect, which the caller can use as the single
    /// emerge point for all minimized windows.
    bool IsReady() const { return m_buttonListHwnd != nullptr; }
    bool HasIAccessible() const { return m_buttonListAcc != nullptr; }

    /// Look up `hwnd`'s taskbar button rect.  Returns true on success and
    /// fills `outRect` with screen coordinates.  Matches by accessible
    /// name == window title (with a substring fallback), since taskbar
    /// buttons don't expose the underlying HWND through MSAA.
    bool GetButtonRect(HWND hwnd, RECT& outRect);

    /// Returns the screen rect of the running-task button list itself
    /// (`MSTaskListWClass`).  Used as the canonical "emerge from taskbar"
    /// origin when per-button matching fails — emerging from the task list's
    /// left edge matches Win7 reference frames better than Shell_TrayWnd's
    /// leftmost pixel (which would emerge from inside the start button).
    bool GetButtonListRect(RECT& outRect) const;

private:
    bool          m_oleInitialized = false;
    IAccessible*  m_buttonListAcc  = nullptr;   // MSTaskListWClass IAccessible
    HWND          m_buttonListHwnd = nullptr;   // for sanity/lifetime checks
};
