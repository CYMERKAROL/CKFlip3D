#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

// ---------------------------------------------------------------------------
// Windows Precision Touchpad gesture source.
//
// A second, fully optional input source alongside the keyboard hook.  It posts
// the same WM_FLIP_* messages, so every gesture reaches the unchanged
// FlipController path and nothing about the keyboard trigger changes.
//
// Contacts come from WM_INPUT (HID usage page 0x0D "Digitizer", usage 0x05
// "Touch Pad") on a dedicated worker thread with a message-only window.  The
// report layout is read from the device's own descriptor through HidP_*, so
// this is vendor-agnostic: pads that pack every contact into one report and
// pads that split a frame across several both work, as do descriptors that
// declare contacts as usage ranges.
//
// WHY DIAGONALS
// There is no low-level hook for touchpad gestures.  SetWindowsHookEx offers
// WH_KEYBOARD_LL and WH_MOUSE_LL and nothing for digitizers, raw input is a
// passive copy with no way to consume an event, and the multi-finger
// recogniser lives inside the OS input stack, above the driver and below
// anything user mode can reach.  Fighting the shell for a gesture it already
// owns cannot be won cleanly, so we do not: the cascade opens on a DIAGONAL
// stroke, which Windows' slide recogniser does not claim.  Nothing of the
// user's configuration is touched and Task View keeps working.
//
// Session state is SHARED with the keyboard hook, so a gesture-opened cascade
// still takes Enter/Escape/Tab and a Win+Tab-opened one still takes gestures.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
namespace TouchpadHook {

/// Gesture that opens the cascade (config `touchpadActivateGestures`).
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

/// Gesture that commits the selection (config `touchpadCommitGestures`).
enum CommitGesture : int {
    kCommitOff     = 0,
    kCommitOneTap  = 1,
    kCommitTwoTap  = 2,
    kCommitTwoDown = 3,
};

/// Swipe that cycles the stack (config `touchpadCycleGestures`).
enum CycleGesture : int {
    kCycleTwo  = 2,   // two fingers left / right
    kCycleFour = 4,   // four fingers left / right
};

/// Bit for one gesture inside the masks below.  Every list the config carries
/// is a SET, not a choice: two hands (and two postures) do not always want the
/// same stroke, and nothing in the recogniser ever needed the answer to be
/// singular.
constexpr unsigned GestureBit(int gesture) { return 1u << gesture; }

/// Runtime options, pushed from the config on every load/reload.
struct Options {
    bool enabled          = true;  // master switch (config touchpadNav)
    // Gesture SETS — see GestureBit.  Zero means the action has no gesture,
    // which is what the old `0 = off` value of each single-gesture setting
    // said.  Built from the config's token lists by the Parse*List functions
    // below, so the parsing lives with the recogniser that consumes it.
    unsigned activateMask = GestureBit(kActivateTwoBack);
    unsigned cycleMask    = GestureBit(kCycleTwo);
    unsigned commitMask   = GestureBit(kCommitOneTap);
    bool reverse          = false; // invert the cycle direction
    int  sensitivity      = 50;    // 1-100, swipe distance per step
    int  smoothing        = 35;    // 0-100, jitter filter strength
    bool cancelSwipe      = true;  // reversed activation stroke cancels
    // Several gestures out of one touch (config touchpadContinuous).  Off, a
    // touch that fires a gesture is finished; on, the stroke is retired and a
    // fresh one starts under the same fingers.
    bool continuous       = false;
    // Window snap off = the cycle swipe scrubs the stack continuously
    // instead of stepping window by window (config windowSnap).
    bool windowSnap       = true;
};

/// Config token list → gesture mask.  Case-insensitive, ';'-separated, a '!'
/// prefix parks an entry (kept in the file, ignored here), and an unknown
/// token is skipped rather than fatal — `dropped`, when given, counts them so
/// the caller can report once.
///
///   activate: TwoDownRight, TwoDownLeft, FourDownRight, FourDownLeft
///   cycle:    TwoSwipe, FourSwipe
///   commit:   OneTap, TwoTap, TwoDown
unsigned ParseActivateList(const std::wstring& list, unsigned* dropped = nullptr);
unsigned ParseCycleList(const std::wstring& list, unsigned* dropped = nullptr);
unsigned ParseCommitList(const std::wstring& list, unsigned* dropped = nullptr);

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
