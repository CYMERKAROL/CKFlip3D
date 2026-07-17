#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace KeyboardHook {

// Modifier bits used by HotkeySpec::modMask.
constexpr uint8_t kModCtrl  = 0x1;
constexpr uint8_t kModShift = 0x2;
constexpr uint8_t kModAlt   = 0x4;
constexpr uint8_t kModWin   = 0x8;

/// Parsed activation combination.
///   - modMask != 0: hold-to-keep-open semantics (releasing a combo
///     modifier commits the selection, exactly like classic Win+Tab).
///   - modMask == 0: toggle semantics (main key activates then cycles;
///     Enter commits, Escape cancels).  Used for single-key bindings such
///     as a bare mouse button or the Windows key alone.
struct HotkeySpec {
    uint8_t  modMask     = kModWin;  // combination's modifier keys
    unsigned mainVk      = 0x09;     // VK of the trigger key (VK_TAB default)
    bool     mainIsMouse = false;    // mainVk is VK_L/R/M/X1/X2BUTTON
};

/// Parse a '+'-separated combination string ("Win+Tab", "Ctrl+Alt+F",
/// "MButton", "Win+XButton1", "0x47", ...).  Case-insensitive.  Returns
/// false and leaves `out` at the Win+Tab default when the string has no
/// valid main key.
bool ParseHotkey(const std::wstring& text, HotkeySpec& out);

/// Runtime-configurable trigger behaviour.  Read by the hook thread on the
/// activation keypress only (never per-event), so updating these is cheap.
struct TriggerOptions {
    bool ignoreFullscreen = false;   // Pass the hotkey through over fullscreen apps
    bool mouseWheelCycle  = true;    // Wheel cycles the cascade while active
    bool keyboardNav      = true;    // Arrow keys cycle while active
    std::vector<std::wstring> ignoredApps;  // exe names or full paths (lowercase)
    std::wstring activationHotkey = L"Win+Tab";  // see ParseHotkey
};

/// Update trigger options (thread-safe; callable from the UI/main thread).
void SetOptions(const TriggerOptions& opts);

/// Install the low-level keyboard hook.
/// The hook callback is deliberately minimal (no allocations, no blocking
/// calls) to stay well under the LowLevelHooksTimeout (~300 ms on Win 11).
///
/// Messages posted to hwndNotify:
///   msgActivate  — first Win+Tab
///   msgCycle     — subsequent Tab / arrow-down / scroll-down while active
///   msgCycleBack — Shift+Tab / arrow-up / scroll-up while active
///   msgDismiss   — Win released
///   msgEscape    — Escape pressed while active (cancel without switching)
bool Install(HWND hwndNotify,
             UINT msgActivate,
             UINT msgCycle,
             UINT msgCycleBack,
             UINT msgDismiss,
             UINT msgEscape,
             UINT msgCycleStop);

void Uninstall();

} // namespace KeyboardHook
