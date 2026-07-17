#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "keyboardhook.h"

// Window messages posted from the LL hook → application message loop.
constexpr UINT WM_FLIP_ACTIVATE   = WM_APP + 1;  // First Win+Tab press
constexpr UINT WM_FLIP_CYCLE      = WM_APP + 2;  // Tab / Arrow-Down / Scroll-Down
constexpr UINT WM_FLIP_CYCLE_BACK = WM_APP + 3;  // Shift+Tab / Arrow-Up / Scroll-Up
constexpr UINT WM_FLIP_DISMISS    = WM_APP + 4;  // Win released (commit selection)
constexpr UINT WM_FLIP_ESCAPE     = WM_APP + 5;  // Escape (cancel)
constexpr UINT WM_FLIP_CYCLE_STOP = WM_APP + 6;  // Tab released (stop queuing)

enum class HotkeyEvent {
    Activate,
    Cycle,
    CycleBack,
    Dismiss,
    Escape,
    CycleStop
};

using HotkeyCallback = void(*)(HotkeyEvent event, void* userData);

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

    /// Forward messages from WndProc here.  Returns true if handled.
    bool HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND            m_hwndOwner = nullptr;
    HotkeyCallback  m_callback  = nullptr;
    void*           m_userData  = nullptr;
    bool            m_installed = false;
};
