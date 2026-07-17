#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>

/// Runtime configuration — loaded from %APPDATA%\CKFlip3D\config.json.
/// Default values are the "high quality" defaults.
struct AppConfig {
    // --- Quality settings ---
    bool     antialiasing     = true;     // Anisotropic tile filtering (false = point)
    bool     animations       = true;     // Master switch: every animation (false = instant snap)
    // Per-animation selection under the master switch — an animation plays
    // only when `animations` AND its own flag are both true.  Lets the user
    // keep e.g. cycling animated while entry/exit and close snap instantly.
    bool     animEntryExit    = true;     // enter/exit morph (flat ↔ cascade)
    bool     animCycle        = true;     // Tab/Shift-Tab cycle rotation
    bool     animClose        = true;     // window-closed-mid-cascade reflow
    bool     motionBlur       = true;     // Motion blur during animation
    bool     livePreview      = true;     // Live WGC window thumbnails (false = static snapshots)
    // Pace overlay rendering with Present(1) so live previews update once
    // per monitor refresh.  Costs GPU time on high-refresh displays.
    bool     vsyncLivePreview = false;
    // Capture the real taskbar live during the session instead of freezing
    // the pre-hide frame.  Requires a bar-sized WGC capture (Win11 25H2+);
    // on builds that deliver the tall 24H2-style capture the core falls
    // back to the frozen snapshot automatically.
    bool     taskbarLivePreview = false;
    // Draw a taskbar preview inside the overlay at all.  Off = the overlay
    // still hides/holds the real taskbar for the session but renders nothing
    // in its place (and taskbarLivePreview is implicitly disabled).
    bool     taskbarPreview   = true;
    uint32_t maxWindows       = 10;       // Max windows displayed in stack (2-10)

    // --- Appearance ---
    // Background opacity target while the cascade is shown (0-100 %).
    // 0 = fully black backdrop, 100 = wallpaper fully visible.
    // 28 matches the original kBgAlpha = 0.28f cascade look.
    uint32_t backgroundOpacity = 28;

    // --- Performance ---
    bool     autoPerfTune     = true;     // Auto-detect and lower quality if needed
    int      perfProfile      = -1;       // -1=auto, 0=low, 1=medium, 2=high
    // Activation warm-up budget (ms): how long Activate keeps pumping
    // DwmFlush cycles for WGC first-frame delivery before showing the
    // overlay (leaves early once every capture has a frame).  16 ms ≈ the
    // original single DwmFlush at 60 Hz.  Auto perf tune overrides this
    // with a device-derived value (see EffectiveStartDelayMs).
    uint32_t startDelayMs     = 16;       // 1-1000

    // --- Input / triggers ---
    bool     ignoreFullscreen = false;    // Don't capture Win+Tab over fullscreen apps
    bool     mouseWheelCycle  = true;     // Mouse wheel cycles the cascade
    bool     keyboardNav      = true;     // Arrow keys cycle while active
    std::wstring ignoredApps;             // ';'-separated exe names/paths to ignore
    // Activation combination, '+'-separated tokens: modifiers Ctrl/Shift/
    // Alt/Win plus one main key ("Win+Tab", "Ctrl+Alt+F", "MButton",
    // "Win+XButton1", ...).  Parsed by KeyboardHook::ParseHotkey; invalid
    // strings fall back to Win+Tab.
    std::wstring activationHotkey = L"Win+Tab";

    // --- Misc ---
    bool     showDebugInfo    = false;    // Output debug strings
};

namespace Config {

/// Get the config file path (%APPDATA%\CKFlip3D\config.json).
std::wstring GetConfigPath();

/// Load config from disk. Returns defaults if file doesn't exist.
AppConfig Load();

/// Save config to disk. Creates directory if needed.
void Save(const AppConfig& cfg);

} // namespace Config
