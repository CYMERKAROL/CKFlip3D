#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "keyboardhook.h"
#include "touchpadhook.h"

// Window messages posted from the LL hook → application message loop.
constexpr UINT WM_FLIP_ACTIVATE   = WM_APP + 1;  // First Win+Tab press
constexpr UINT WM_FLIP_CYCLE      = WM_APP + 2;  // Tab / Arrow-Down / Scroll-Down
constexpr UINT WM_FLIP_CYCLE_BACK = WM_APP + 3;  // Shift+Tab / Arrow-Up / Scroll-Up
constexpr UINT WM_FLIP_DISMISS    = WM_APP + 4;  // Win released (commit selection)
constexpr UINT WM_FLIP_ESCAPE     = WM_APP + 5;  // Escape (cancel)
constexpr UINT WM_FLIP_CYCLE_STOP = WM_APP + 6;  // Tab released (stop queuing)
// Free stack movement (config windowSnap = false).  SCRUB carries a signed
// fixed-point delta in lParam — TEN-thousandths of a window (1/10000),
// positive = forward; SCRUB_END asks the stack to settle onto the nearest
// window.
constexpr UINT WM_FLIP_SCRUB      = WM_APP + 7;
constexpr UINT WM_FLIP_SCRUB_END  = WM_APP + 8;
// Mouse in the cascade (Controls → Mouse & keyboard).  The pointer messages
// carry SCREEN coordinates as two signed LONGs (wParam = X, lParam = Y).
constexpr UINT WM_FLIP_POINTER_MOVE   = WM_APP + 9;
constexpr UINT WM_FLIP_POINTER_SELECT = WM_APP + 10;
constexpr UINT WM_FLIP_POINTER_CLOSE  = WM_APP + 11;
constexpr UINT WM_FLIP_CLOSE_SELECTED = WM_APP + 12;  // Delete
// Type-to-filter (Settings → Search).  SEARCH_CHAR carries the character in
// wParam.
constexpr UINT WM_FLIP_SEARCH_CHAR    = WM_APP + 13;
constexpr UINT WM_FLIP_SEARCH_BACK    = WM_APP + 14;

enum class HotkeyEvent {
    Activate,
    Cycle,
    CycleBack,
    Dismiss,
    Escape,
    CycleStop,
    Scrub,       // amount = fractional windows, positive = forward
    ScrubEnd,
    PointerMove,    // x, y = screen coordinates
    PointerSelect,  // x, y = screen coordinates
    PointerClose,   // x, y = screen coordinates
    CloseSelected,
    SearchChar,     // x = the typed character
    SearchBack
};

/// `amount` is only meaningful for HotkeyEvent::Scrub; `x` / `y` carry the
/// pointer position for the Pointer* events and the character for SearchChar.
using HotkeyCallback = void(*)(HotkeyEvent event, float amount,
                               int x, int y, void* userData);

class HotkeyManager {
public:
    HotkeyManager() = default;
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    bool Init(HWND hwndOwner);
    void Shutdown();

    void SetCallback(HotkeyCallback callback, void* userData = nullptr);

    /// Push runtime trigger options (fullscreen ignore, ignore list,
    /// wheel/arrow-key cycling) down to the low-level hook.
    void SetTriggerOptions(const KeyboardHook::TriggerOptions& opts);

    /// Push touchpad-gesture options down to the raw-input worker.  The
    /// worker posts the same WM_FLIP_* messages as the keyboard hook, so
    /// nothing downstream can tell the two input sources apart.
    void SetTouchpadOptions(const TouchpadHook::Options& opts);

    /// Forward messages from WndProc here.  Returns true if handled.
    bool HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    /// Every message the keyboard hook is wired to post, in one place so the
    /// hook and this dispatcher can never drift apart.
    static KeyboardHook::Messages HookMessages();

private:
    HWND            m_hwndOwner = nullptr;
    HotkeyCallback  m_callback  = nullptr;
    void*           m_userData  = nullptr;
    bool            m_installed = false;
};
