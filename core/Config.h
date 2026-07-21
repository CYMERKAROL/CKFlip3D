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
    // Selected-window label motion: the smooth glide between differently
    // sized front tiles and the fade out/in on held-key rapid cycling.
    // Off = the label snaps instantly and shows/hides without fades.
    bool     animLabel        = true;
    bool     motionBlur       = true;     // Motion blur during animation
    bool     livePreview      = true;     // Live WGC window thumbnails (false = static snapshots)
    // Live wallpaper backdrop: stream the desktop capture behind the
    // cascade every frame so animated wallpapers (Wallpaper Engine, Lively)
    // keep playing — including during the cycle animation.  Off = one
    // static snapshot taken when the cascade opens (dedicated GPU copy, so
    // a live desktop tile can't mutate it through the shared texture).
    bool     liveBackground   = true;
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
    // Wallpaper backdrop blur while the cascade is shown (0-100 %).
    // 0 (default) = no blur — the shader takes the single-sample path, so
    // the feature costs nothing unless enabled.
    uint32_t backgroundBlur = 0;
    // Include the desktop pseudo-window as the last tile of the cascade
    // (the classic Win7 Flip3D behaviour).  Off removes the tile — the
    // freed slot goes to the next real window — while the wallpaper
    // backdrop keeps working through a dedicated capture.
    bool     showDesktopTile  = true;
    // Selected-window label on the front slot (title + program icon) for
    // readability.  `selectedLabel` is the master switch (default OFF —
    // the classic clean cascade look); the flags below select the parts
    // independently (Appearance → Customize) once it's enabled.
    bool     selectedLabel      = false;
    bool     selectedLabelTitle = true;
    bool     selectedLabelIcon  = true;
    // Aero-glass plate behind the label.  Off draws the text/icon directly
    // with a stronger drop shadow instead.
    bool     selectedLabelBox   = true;

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
    // Exclusion list (General): windows of these executables never appear
    // in the cascade — the hotkey still works, the windows are simply left
    // out of the stack (they stay cloaked behind the overlay like any
    // other non-cascade surface and are restored on dismiss).
    std::wstring excludedApps;            // ';'-separated exe names/paths
    // Toggle activation for combo bindings: releasing the combo modifier
    // does NOT commit — the cascade stays open until Enter (commit) or
    // Escape (cancel), exactly like single-key bindings.  Single-key
    // bindings are inherently toggle, so this flag only matters when the
    // combination has at least one modifier.
    bool     hotkeyToggleMode = false;
    // Activation combination, '+'-separated tokens: modifiers Ctrl/Shift/
    // Alt/Win plus one main key ("Win+Tab", "Ctrl+Alt+F", "MButton",
    // "Win+XButton1", ...).  Parsed by KeyboardHook::ParseHotkey; invalid
    // strings fall back to Win+Tab.
    std::wstring activationHotkey = L"Win+Tab";

    // --- Misc ---
    bool     showDebugInfo    = false;    // Output debug strings
    // Settings-app theme index (0 Skeuo Dark, 1 Skeuo White, 2 Minimal
    // Dark, 3 Minimal White, 4 Glassmorphism).  Owned by the Settings app;
    // the core reads it so the selected-window label's plate matches the
    // chosen CKSettings look.
    int      appTheme         = 0;
};

namespace Config {

/// Get the config file path (%APPDATA%\CKFlip3D\config.json).
std::wstring GetConfigPath();

/// Load config from disk. Returns defaults if file doesn't exist.
AppConfig Load();

/// Save config to disk. Creates directory if needed.
void Save(const AppConfig& cfg);

} // namespace Config
