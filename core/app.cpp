#include "app.h"
#include "resource.h"
#include "DebugLog.h"
#include <shellapi.h>
#include <CommCtrl.h>
#include <cstdio>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

static constexpr const wchar_t* kWindowClass = L"CKFlip3D_MessageWindow";

// ---------------------------------------------------------------------------
static App* GetApp(HWND hwnd)
{
    return reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

static bool HasCommandLineFlag(const wchar_t* flag)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return false;

    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpiW(argv[i], flag) == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg,
                               WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    App* app = GetApp(hwnd);

    if (app && msg == WM_TRAYICON) {
        // lParam contains the mouse message on the tray icon.
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            app->ShowTrayContextMenu();
        }
        return 0;
    }

    if (app && msg == WM_COMMAND) {
        if (LOWORD(wParam) == IDM_EXIT) {
            app->RemoveTrayIcon();
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == IDM_SETTINGS) {
            app->ShowSettingsDialog();
            return 0;
        }
    }

    // Settings app broadcast: re-read config.json without a restart.
    if (app && app->m_msgConfigReload != 0 && msg == app->m_msgConfigReload) {
        app->ReloadConfig();
        return 0;
    }

    // Settings app broadcast: full restart (sent after Apply).  This exe is
    // elevated, so the unelevated Settings app cannot kill/relaunch it —
    // instead we relaunch ourselves after the message loop unwinds.
    if (app && app->m_msgRestart != 0 && msg == app->m_msgRestart) {
        app->m_restartPending = true;
        app->RemoveTrayIcon();
        DestroyWindow(hwnd);
        return 0;
    }

    // Explorer (re)started — any previously added icon is gone.  Re-add.
    if (app && app->m_msgTaskbarCreated != 0 && msg == app->m_msgTaskbarCreated) {
        app->m_trayActive = false;
        app->InitTrayIcon();
        return 0;
    }

    // Tray-icon logon-race retry (see InitTrayIcon).
    if (app && msg == WM_TIMER && wParam == kTrayRetryTimer) {
        if (!app->m_trayActive)
            app->m_trayActive = Shell_NotifyIconW(NIM_ADD, &app->m_nid) != FALSE;
        if (app->m_trayActive || ++app->m_trayRetries >= kTrayRetryMax)
            KillTimer(hwnd, kTrayRetryTimer);
        return 0;
    }

    if (app && app->m_hotkeyMgr.HandleMessage(msg, wParam, lParam))
        return 0;

    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
bool App::CreateMessageWindow(HINSTANCE hInstance)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = kWindowClass;

    if (!RegisterClassExW(&wc))
        return false;

    // Hidden window (not message-only) so it can receive tray icon messages.
    m_hwnd = CreateWindowExW(0, kWindowClass, L"CKFlip3D", 0,
                             0, 0, 0, 0,
                             nullptr, nullptr, hInstance,
                             this);
    if (m_hwnd)
        ShowWindow(m_hwnd, SW_HIDE);
    return m_hwnd != nullptr;
}

// ---------------------------------------------------------------------------
void App::InitTrayIcon()
{
    m_nid = {};
    m_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd             = m_hwnd;
    m_nid.uID              = 1;
    m_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;

    // Our cascade icon from the exe resources (app.rc); the stock
    // application icon stays as a fallback for resource-stripped builds.
    m_nid.hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    if (!m_nid.hIcon)
        m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(m_nid.szTip, m_safeMode ? L"CKFlip3D — Safe Mode" : L"CKFlip3D");

    m_trayActive = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    if (!m_trayActive) {
        // Logon race: the scheduled-task autostart can start this exe
        // before the shell's notification area exists, and NIM_ADD then
        // fails silently — the icon never appears.  Retry on a timer
        // until the shell is ready (TaskbarCreated also re-adds below).
        m_trayRetries = 0;
        SetTimer(m_hwnd, kTrayRetryTimer, 2000, nullptr);
    }
}

void App::RemoveTrayIcon()
{
    if (m_trayActive) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_trayActive = false;
    }
}

// Renders an icon resource into a 32-bpp ARGB bitmap for MENUITEMINFO's
// hbmpItem (menus don't take HICONs directly). Returns nullptr on failure —
// the item then simply shows without an icon.
static HBITMAP LoadMenuItemBitmap(int iconId, int size)
{
    HICON icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(iconId), IMAGE_ICON,
        size, size, LR_DEFAULTCOLOR));
    if (!icon) return nullptr;

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = size;
    bmi.bmiHeader.biHeight      = -size;   // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HBITMAP bitmap = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    bitmap = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap) {
        HGDIOBJ old = SelectObject(mem, bitmap);
        // Zero-initialized DIB + DrawIconEx preserves the icon's alpha,
        // which themed menus composite correctly.
        DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(mem, old);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DestroyIcon(icon);
    return bitmap;
}

void App::ShowTrayContextMenu()
{
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    // Glyph bitmaps next to the items (menu_settings/menu_exit in app.rc).
    int iconSize = GetSystemMetrics(SM_CXSMICON);
    HBITMAP bmpSettings = LoadMenuItemBitmap(IDI_MENU_SETTINGS, iconSize);
    HBITMAP bmpExit     = LoadMenuItemBitmap(IDI_MENU_EXIT, iconSize);

    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask  = MIIM_BITMAP;
    if (bmpSettings) {
        mii.hbmpItem = bmpSettings;
        SetMenuItemInfoW(hMenu, IDM_SETTINGS, FALSE, &mii);
    }
    if (bmpExit) {
        mii.hbmpItem = bmpExit;
        SetMenuItemInfoW(hMenu, IDM_EXIT, FALSE, &mii);
    }

    // TrackPopupMenu requires the window to be foreground.
    SetForegroundWindow(m_hwnd);

    POINT pt;
    GetCursorPos(&pt);
    // Without TPM_RETURNCMD this blocks until the menu closes, so the
    // bitmaps stay valid for the menu's whole lifetime.
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(hMenu);
    if (bmpSettings) DeleteObject(bmpSettings);
    if (bmpExit) DeleteObject(bmpExit);

    // Required after TrackPopupMenu to dismiss the menu if user clicks away.
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
}

// ---------------------------------------------------------------------------
// Settings — external CKFlip3D.Settings.exe living next to CKFlip3D.exe.
// The old in-process Win32 settings dialog was removed in favour of the
// dedicated WPF settings app (core/Settings).  Config changes are picked up
// at runtime via the CKFLIP3D_CONFIG_RELOAD broadcast (see ReloadConfig()).
// ---------------------------------------------------------------------------
void App::ShowSettingsDialog()
{
    // Single instance: if the settings window already exists, bring it to
    // the front instead of spawning another copy.  (The settings app also
    // enforces this itself with a named mutex — this is just the fast path.)
    HWND existing = FindWindowW(nullptr, L"CKFlip3D Settings");
    if (existing) {
        if (IsIconic(existing))
            ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return;
    }

    // Settings exe sits next to the main executable.
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring settingsPath(exePath);
    size_t slash = settingsPath.find_last_of(L'\\');
    settingsPath = (slash == std::wstring::npos ? L"" : settingsPath.substr(0, slash + 1));
    settingsPath += L"CKFlip3D.Settings.exe";

    if (GetFileAttributesW(settingsPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(m_hwnd,
                    L"The settings executable is missing.\n\n"
                    L"Expected location:\n"
                    L"CKFlip3D.Settings.exe next to CKFlip3D.exe.\n\n"
                    L"Reinstall or rebuild the Settings app\n"
                    L"(core\\Settings\\build_settings.bat).",
                    L"CKFlip3D — Settings not found",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    HINSTANCE result = ShellExecuteW(nullptr, L"open", settingsPath.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(m_hwnd,
                    L"Failed to launch CKFlip3D.Settings.exe.",
                    L"CKFlip3D — Error", MB_OK | MB_ICONERROR);
    }
}

// ---------------------------------------------------------------------------
void App::ApplyTriggerOptions()
{
    KeyboardHook::TriggerOptions opts;
    opts.ignoreFullscreen = m_config.ignoreFullscreen;
    opts.mouseWheelCycle  = m_config.mouseWheelCycle;
    opts.keyboardNav      = m_config.keyboardNav;
    opts.hotkeyToggleMode = m_config.hotkeyToggleMode;
    opts.activationHotkey = m_config.activationHotkey;

    // ignoredApps is a ';'-separated list of exe names / full paths.
    const std::wstring& list = m_config.ignoredApps;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos) end = list.size();
        if (end > start)
            opts.ignoredApps.emplace_back(list.substr(start, end - start));
        if (end == list.size()) break;
        start = end + 1;
    }

    m_hotkeyMgr.SetTriggerOptions(opts);
}

// ---------------------------------------------------------------------------
// Safe mode (--safe-mode, used by the Settings app's Recovery page).
// Conservative overrides on top of config.json:
//   - every animation / effect off (animations, motion blur, antialiasing)
//   - live previews off (window, taskbar) + VSYNC pacing off — static
//     snapshots only, minimum capture surface
//   - manual Low profile, auto perf tune off (no runtime switching)
//   - stack clamped to 5 windows, generous WGC warm-up budget
//   - custom activation hotkey ignored — the known-good Win+Tab path
//   - debug output forced on + a plain-text session log for diagnosis
// ---------------------------------------------------------------------------
void App::ApplySafeModeOverrides()
{
    m_config.animations         = false;
    m_config.motionBlur         = false;
    m_config.antialiasing       = false;
    m_config.livePreview        = false;
    m_config.vsyncLivePreview   = false;
    m_config.taskbarLivePreview = false;
    m_config.autoPerfTune       = false;
    m_config.perfProfile        = 0;     // Low
    if (m_config.maxWindows > 5)
        m_config.maxWindows = 5;
    if (m_config.startDelayMs < 100)
        m_config.startDelayMs = 100;
    m_config.activationHotkey   = L"Win+Tab";
    m_config.showDebugInfo      = true;
}

void App::WriteSafeModeLog()
{
    std::wstring path = Config::GetConfigPath();
    size_t slash = path.find_last_of(L'\\');
    path = (slash == std::wstring::npos ? L"" : path.substr(0, slash + 1));
    path += L"safemode.log";

    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8");
    if (!f)
        return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(f, L"CKFlip3D safe-mode session %04u-%02u-%02u %02u:%02u:%02u\n",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // OS build straight from the registry (no manifest-dependent
    // GetVersionEx lies) — the taskbar capture quirks are build-specific.
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                0, KEY_READ, &key) == ERROR_SUCCESS) {
            wchar_t build[64] = {}, display[64] = {};
            DWORD cb = sizeof(build);
            RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(build), &cb);
            cb = sizeof(display);
            RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(display), &cb);
            fwprintf(f, L"windows: build %s (%s)\n", build, display);
            RegCloseKey(key);
        }
    }

    fwprintf(f, L"monitors: virtual %dx%d at (%d,%d), primary %dx%d\n",
             GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
             GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
             GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    fwprintf(f,
        L"effective config: animations=%d motionBlur=%d antialiasing=%d\n"
        L"  livePreview=%d vsyncLivePreview=%d taskbarLivePreview=%d\n"
        L"  perfProfile=%d autoPerfTune=%d maxWindows=%u startDelayMs=%u\n"
        L"  backgroundOpacity=%u hotkey=%s\n",
        m_config.animations, m_config.motionBlur, m_config.antialiasing,
        m_config.livePreview, m_config.vsyncLivePreview, m_config.taskbarLivePreview,
        m_config.perfProfile, m_config.autoPerfTune,
        m_config.maxWindows, m_config.startDelayMs,
        m_config.backgroundOpacity, m_config.activationHotkey.c_str());

    fwprintf(f, L"restrictions: live capture disabled, quality profile Low, "
                L"stack <= 5 windows, default Win+Tab trigger, debug output on.\n"
                L"Use DebugView (OutputDebugString) for per-frame diagnostics.\n");
    fclose(f);
}

void App::ReloadConfig()
{
    m_config = Config::Load();
    if (m_safeMode)
        ApplySafeModeOverrides();
    // m_flip holds a pointer to m_config — values are visible immediately.
    // A settings change resets the auto-perf-tune ladder so the new
    // configuration gets a fresh measurement.
    m_flip.ResetPerfTune();
    ApplyTriggerOptions();
    CKLog::g_enabled.store(m_config.showDebugInfo, std::memory_order_relaxed);
    CKLog::Log(L"CKFlip: config reloaded (CKFLIP3D_CONFIG_RELOAD)\n");
}

// ---------------------------------------------------------------------------
void App::OnHotkeyEvent(HotkeyEvent event, void* userData)
{
    auto* app = static_cast<App*>(userData);
    switch (event) {
    case HotkeyEvent::Activate:  app->OnFlipActivate();  break;
    case HotkeyEvent::Cycle:     app->OnFlipCycle();     break;
    case HotkeyEvent::CycleBack: app->OnFlipCycleBack(); break;
    case HotkeyEvent::Dismiss:   app->OnFlipDismiss();   break;
    case HotkeyEvent::Escape:    app->OnFlipEscape();    break;
    case HotkeyEvent::CycleStop: app->OnFlipCycleStop(); break;
    }
}

void App::OnFlipActivate()  { m_flip.Activate(); }
void App::OnFlipCycle()     { m_flip.Cycle(); }
void App::OnFlipCycleBack() { m_flip.CycleBack(); }
// Route through the fade-then-dispatch pre-step so the ~180 ms exit fade
// runs before the unchanged Dismiss()/Escape() teardown fires.
void App::OnFlipDismiss()   { m_flip.Dismiss(); }
void App::OnFlipEscape()    { m_flip.Escape(); }
void App::OnFlipCycleStop() { m_flip.CycleStop(); }

// ---------------------------------------------------------------------------
int App::Run(HINSTANCE hInstance)
{
    m_config = Config::Load();

    // --safe-mode (used by the Settings app's Recovery page): run with
    // conservative settings regardless of config.json and write a
    // diagnostics log (%APPDATA%\CKFlip3D\safemode.log).
    m_safeMode = HasCommandLineFlag(L"--safe-mode");
    if (m_safeMode) {
        ApplySafeModeOverrides();
        WriteSafeModeLog();
    }
    // Sync AFTER the safe-mode override so --safe-mode keeps logging.
    CKLog::g_enabled.store(m_config.showDebugInfo, std::memory_order_relaxed);

    m_msgConfigReload   = RegisterWindowMessageW(L"CKFLIP3D_CONFIG_RELOAD");
    m_msgRestart        = RegisterWindowMessageW(L"CKFLIP3D_RESTART");
    m_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    if (!CreateMessageWindow(hInstance))
        return 1;

    // This exe runs elevated (requireAdministrator) while the Settings app
    // does not — UIPI silently drops messages from the lower-integrity
    // process unless they are explicitly allowed.  Without this the
    // reload/restart broadcasts (and the Recovery page's graceful WM_CLOSE)
    // never arrive and config changes appear to need a manual restart.
    // TaskbarCreated comes from explorer (medium IL) — required so the
    // tray icon survives explorer restarts and the autostart logon race.
    ChangeWindowMessageFilterEx(m_hwnd, m_msgConfigReload,   MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, m_msgRestart,        MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, m_msgTaskbarCreated, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(m_hwnd, WM_CLOSE,            MSGFLT_ALLOW, nullptr);

    if (!m_flip.Init(hInstance)) {
        MessageBoxW(nullptr,
                    L"Failed to initialise D3D11 overlay.\n"
                    L"Make sure your GPU supports D3D11.\n\n"
                    L"If you are running in a VM, ensure 3D\n"
                    L"acceleration is enabled.",
                    L"CKFlip3D \u2014 Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    m_flip.SetConfig(&m_config);
    m_hotkeyMgr.SetCallback(OnHotkeyEvent, this);

    if (!m_hotkeyMgr.Init(m_hwnd)) {
        MessageBoxW(nullptr,
                    L"Failed to install keyboard hook.\n"
                    L"Make sure CKFlip3D is not already running.",
                    L"CKFlip3D \u2014 Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Push trigger options (fullscreen ignore, ignore list, wheel/arrows)
    // from config into the freshly installed hook.
    ApplyTriggerOptions();

    // Add system tray icon with right-click → Exit.
    InitTrayIcon();

    const bool autoTestEntryExit =
        HasCommandLineFlag(L"--auto-test-entry-exit");
    bool autoDismissSent = false;
    bool autoQuitPosted = false;
    ULONGLONG autoActivatedTick = 0;
    if (autoTestEntryExit) {
        Sleep(800);
        OnHotkeyEvent(HotkeyEvent::Activate, this);
        autoActivatedTick = GetTickCount64();
        if (!m_flip.IsActive()) {
            PostQuitMessage(0);
            autoQuitPosted = true;
        }
    }

    // Message loop: PeekMessage idle-render when Flip3D is active,
    // blocking GetMessage when idle to save CPU.
    MSG msg;
    for (;;) {
        if (m_flip.IsActive()) {
            // Drain all pending messages without blocking.
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT)
                    goto quit;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            // Render one frame — DwmFlush() at the end blocks until the
            // monitor's next vsync, giving us perfect 165Hz/144Hz/60Hz
            // pacing with zero WM_TIMER latency.
            m_flip.RenderFrame();
            if (autoTestEntryExit
                && !autoDismissSent
                && autoActivatedTick != 0
                && GetTickCount64() - autoActivatedTick >= 500) {
                OnHotkeyEvent(HotkeyEvent::Dismiss, this);
                autoDismissSent = true;
            }
            if (autoTestEntryExit
                && autoDismissSent
                && !m_flip.IsActive()
                && !autoQuitPosted) {
                PostQuitMessage(0);
                autoQuitPosted = true;
            }
        } else {
            if (autoTestEntryExit && autoDismissSent && !autoQuitPosted) {
                PostQuitMessage(0);
                autoQuitPosted = true;
            }
            // Not active — block until the next message (zero CPU).
            if (!GetMessageW(&msg, nullptr, 0, 0))
                break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
quit:

    RemoveTrayIcon();
    m_hotkeyMgr.Shutdown();
    m_flip.Shutdown();

    // CKFLIP3D_RESTART: relaunch after the hook and overlay are fully torn
    // down so the new instance can install its own keyboard hook cleanly.
    if (m_restartPending) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        ShellExecuteW(nullptr, L"open", exePath,
                      m_safeMode ? L"--safe-mode" : nullptr,
                      nullptr, SW_SHOWNORMAL);
    }
    return static_cast<int>(msg.wParam);
}
