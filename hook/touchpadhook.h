#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

// ---------------------------------------------------------------------------
// Windows Precision Touchpad gesture source.
//
// Sits next to the low-level keyboard/mouse hook (hook/keyboardhook) as a
// SECOND, fully optional input source that posts the same WM_FLIP_* messages
// to the app window — so every gesture ends up in the unchanged
// FlipController path (Activate / Cycle / CycleBack / Dismiss / Escape /
// Scrub) and nothing about the keyboard trigger changes.
//
// Contacts come from WM_INPUT (HID usage page 0x0D "Digitizer", usage 0x05
// "Touch Pad") on a dedicated worker thread with a message-only window.  The
// report layout is read from the device's own descriptor through HidP_*, so
// the code is vendor-agnostic: pads that pack every contact into one report
// and pads that split a frame across several both work, as do descriptors
// that declare contacts as usage ranges.
//
// WHY DIAGONALS
// There is no low-level hook for touchpad gestures: SetWindowsHookEx offers
// WH_KEYBOARD_LL and WH_MOUSE_LL and nothing for digitizers, raw input is a
// passive copy with no way to consume an event, and the multi-finger
// recogniser lives inside the OS input stack — above the driver, below
// anything user mode can reach.  Fighting the shell for a gesture it already
// owns therefore cannot be won cleanly.  So we do not: the cascade opens on
// a DIAGONAL stroke, which Windows' slide recogniser (four cardinal
// directions) does not claim.  Nothing of the user's Windows configuration
// is touched, and Task View / show-desktop keep working exactly as before.
//
// Session state is SHARED with the keyboard hook
// (KeyboardHook::IsSessionActive), so a gesture-opened cascade still takes
// Enter/Escape/Tab and a Win+Tab-opened one still takes gestures.
// ---------------------------------------------------------------------------
namespace TouchpadHook {

/// Gesture that opens the cascade (config `touchpadActivateGesture`).
/// Diagonals only — see WHY DIAGONALS above.  "\" runs from the pad's
/// top-left toward its bottom-right; "/" mirrors it from top-right to
/// bottom-left.  Reversing the same stroke cancels.
///
/// TWO or FOUR fingers, never three.  Three-finger slides are the ones
/// Windows binds to Alt+Tab / Task View by default, and its recogniser is
/// generous enough about the angle that a three-finger DIAGONAL still trips
/// it — the switcher would open on top of ours.  Two fingers only ever means
/// "scroll" to Windows, which claims no cardinal direction of its own here,
/// and four-finger slides are free unless the user has rebound them.
enum ActivateGesture : int {
    kActivateOff       = 0,
    kActivateTwoBack   = 1,   // two fingers,  "\"  (default)
    kActivateTwoFwd    = 2,   // two fingers,  "/"
    kActivateFourBack  = 3,   // four fingers, "\"
    kActivateFourFwd   = 4,   // four fingers, "/"
};

/// Gesture that commits the selection (config `touchpadCommitGesture`).
enum CommitGesture : int {
    kCommitOff     = 0,
    kCommitOneTap  = 1,
    kCommitTwoTap  = 2,
    kCommitTwoDown = 3,
};

/// Runtime options, pushed from the config on every load/reload.
struct Options {
    bool enabled          = true;  // master switch (config touchpadNav)
    int  cycleFingers     = 2;     // fingers for the left/right cycle swipe (2 or 4)
    bool reverse          = false; // invert the cycle direction
    int  sensitivity      = 50;    // 1-100, swipe distance per step
    int  smoothing        = 35;    // 0-100, jitter filter strength
    int  activateGesture  = kActivateTwoBack;
    int  commitGesture    = kCommitOneTap;
    bool cancelSwipe      = true;  // reversed activation stroke cancels
    // Window snap off = the cycle swipe scrubs the stack continuously
    // instead of stepping window by window (config windowSnap).
    bool windowSnap       = true;
};

/// True when at least one precision touchpad is currently attached.
bool IsTouchpadPresent();

/// Human-readable list of attached precision touchpads (vendor/product IDs)
/// for the Diagnostics page.  Safe to call at any time; never touches the
/// worker thread's state.
std::wstring DescribeDevices();

/// Start the gesture worker.  Never fails the app: a machine without a
/// touchpad (or a system that refuses the raw-input registration) simply
/// never posts anything.
///
/// msgScrub carries a signed fixed-point delta in lParam (1/10000 of a
/// window, positive = forward) and is only used while Window snap is off;
/// msgScrubEnd asks the controller to settle onto the nearest window.
///
/// (No msgCycleStop counterpart: the controller's CycleStop clears the
/// pending-cycle queue, which is right for a released key but would throw
/// away the tail of a fast flick.  Gesture steps are discrete, so there is
/// nothing to stop when the fingers lift.)
bool Install(HWND hwndNotify,
             UINT msgActivate,
             UINT msgCycle,
             UINT msgCycleBack,
             UINT msgDismiss,
             UINT msgEscape,
             UINT msgScrub,
             UINT msgScrubEnd);

/// Stop the worker.
void Uninstall();

/// Push new options (thread-safe, callable from the app thread).
void SetOptions(const Options& opts);

} // namespace TouchpadHook
