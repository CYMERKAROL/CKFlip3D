#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include "../hook/hotkeymanager.h"
#include "flipcontroller.h"
#include "Config.h"

class App {
public:
    int Run(HINSTANCE hInstance);

private:
    // WM_APP+1..+5 are used by hotkeymanager — start tray messages at +100.
    static constexpr UINT WM_TRAYICON  = WM_APP + 100;
    static constexpr UINT IDM_SETTINGS = 4000;
    static constexpr UINT IDM_EXIT     = 4001;
    static constexpr UINT_PTR kTrayRetryTimer   = 10;
    static constexpr UINT     kTrayRetryMax     = 45;   // × 2 s ≈ 90 s window

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                    WPARAM wParam, LPARAM lParam);
    static void OnHotkeyEvent(HotkeyEvent event, void* userData);

    bool CreateMessageWindow(HINSTANCE hInstance);
    void InitTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayContextMenu();
    void ShowSettingsDialog();      // launches the external CKFlip3D.Settings.exe
    void ApplyTriggerOptions();     // push config trigger settings into the hook
    void ReloadConfig();            // re-read config.json (CKFLIP3D_CONFIG_RELOAD)
    void ApplySafeModeOverrides();  // conservative --safe-mode config clamps
    void WriteSafeModeLog();        // %APPDATA%\CKFlip3D\safemode.log diagnostics

    void OnFlipActivate();
    void OnFlipCycle();
    void OnFlipCycleBack();
    void OnFlipDismiss();
    void OnFlipEscape();
    void OnFlipCycleStop();

    HWND              m_hwnd       = nullptr;
    HotkeyManager     m_hotkeyMgr;
    FlipController    m_flip;
    NOTIFYICONDATAW   m_nid        = {};
    bool              m_trayActive = false;
    AppConfig         m_config;
    bool              m_safeMode   = false;  // --safe-mode: conservative overrides
    UINT              m_msgConfigReload = 0; // RegisterWindowMessage(CKFLIP3D_CONFIG_RELOAD)
    UINT              m_msgRestart      = 0; // RegisterWindowMessage(CKFLIP3D_RESTART)
    UINT              m_msgTaskbarCreated = 0; // RegisterWindowMessage(TaskbarCreated)
    UINT              m_trayRetries     = 0;  // logon-race retry counter
    bool              m_restartPending  = false; // relaunch self after the loop exits
};
