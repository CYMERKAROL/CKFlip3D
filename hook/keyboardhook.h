// ---------------------------------------------------------------------------
// What counts as a trigger and what the user is allowed to bind to it.  The
// combination is parsed from a config string, so a binding is data rather than
// something baked into the hook procedure.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
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

/// Mouse-button identifier shared by the in-cascade bindings (Controls →
/// Mouse & keyboard).  Matches the config integers 1:1.
enum MouseButtonId : int {
    kMouseNone   = 0,
    kMouseLeft   = 1,
    kMouseRight  = 2,
    kMouseMiddle = 3,
    kMouseX1     = 4,
    kMouseX2     = 5,
};

/// Runtime-configurable trigger behaviour.  Read by the hook thread on the
/// activation keypress only (never per-event), so updating these is cheap.
struct TriggerOptions {
    bool ignoreFullscreen = false;   // Pass the hotkey through over fullscreen apps
    bool mouseWheelCycle  = true;    // Wheel cycles the cascade while active
    // Toggle semantics for combo bindings: releasing the combo modifier
    // no longer commits — the session stays open until Enter/Escape,
    // exactly like a single-key binding.  Ignored when the binding has no
    // modifier (those are inherently toggle).
    bool hotkeyToggleMode = false;
    // Touchpad gestures are live (config touchpadNav).  The hook itself
    // never reads the touchpad — it only stops passing the OS's horizontal
    // wheel through while a session is open, so a two-finger cycle swipe
    // cannot double as a stray horizontal scroll somewhere behind the
    // overlay.  Off = the horizontal wheel behaves exactly as before.
    bool touchpadNav      = true;
    // Window snap (config windowSnap).  Off = holding the left mouse button
    // and moving scrubs the stack continuously instead of stepping window by
    // window.  Deliberately does NOT touch the wheel or the keyboard: those
    // stay discrete either way.
    bool windowSnap       = true;
    // --- Mouse in the cascade (Controls → Mouse & keyboard) ---------------
    // Master switch for everything the pointer does to the stack.  Off = the
    // cascade takes keyboard, wheel and touchpad only, no pointer message is
    // posted, and no hit test runs — the pre-pointer cascade exactly.
    bool pointerInCascade = false;
    // Hover + click to pick a window.
    bool mouseSelect      = true;
    int  selectButton     = kMouseLeft;
    // Drag the stack while Window snap is off.  Right by default now that
    // left picks a window.
    bool dragEnabled      = true;
    int  dragButton       = kMouseRight;
    // Close the hovered window with the mouse.  A pointer feature, so
    // pointerInCascade gates it along with everything else the mouse does.
    bool closeFromCascade = true;
    int  closeButton      = kMouseMiddle;

    // Type-to-filter (Settings → Search).  Off = printable keys are
    // swallowed as strays exactly as before.
    bool searchEnabled    = false;

    std::vector<std::wstring> ignoredApps;  // exe names or full paths (lowercase)
    std::wstring activationHotkey = L"Win+Tab";  // see ParseHotkey
    // ---- The five in-cascade key LISTS ------------------------------------
    // ';'-separated token lists in the same vocabulary as the hotkey above —
    // a bare key, or Shift+key, and nothing else, because any other modifier
    // would have to be pressed alongside a combination that may still be held.
    // A '!' prefix means the binding is kept but switched off.  Up to
    // kMaxBindingKeys per list survive; see SetOptions.  Empty is legitimate
    // and means the action has no key at all.
    //
    // navForwardKeys / navBackKeys hold EVERY key that steps through the stack,
    // the activation hotkey's own key included — that is why the defaults name
    // Tab.
    std::wstring commitKeys     = L"Enter";
    std::wstring cancelKeys     = L"Escape";
    std::wstring closeKeys      = L"Delete";
    std::wstring navForwardKeys = L"Tab;Down;Right";
    std::wstring navBackKeys    = L"Shift+Tab;Up;Left";
};

/// How many keys one binding list can hold.
///
/// Seven because that is what fits in the SINGLE word the hook reads per
/// keystroke: seven VK bytes (bits 0-55) plus one "needs Shift" bit each (bits
/// 56-62).  A list the hook could catch half-updated — half the keys the user
/// just removed, half the ones they added — is worse than a ceiling nobody
/// reaches; three per direction is the shipped configuration.
constexpr int kMaxBindingKeys = 7;

/// Update trigger options (thread-safe; callable from the UI/main thread).
void SetOptions(const TriggerOptions& opts);

/// Shared switcher-session state.  The hook owns it, but a second input
/// source (hook/touchpadhook) has to open and close sessions through the
/// SAME flag — otherwise a gesture-opened cascade would not take Enter /
/// Escape / Tab, and a Win+Tab-opened one would not take gestures.
/// Setting it does NOT post any message; the caller posts the matching
/// WM_FLIP_* itself, exactly like the hook's own branches do.
bool IsSessionActive();
void SetSessionActive(bool active);

/// Make the RUNNING session hold itself open, whatever "Toggle activation" is
/// set to — releasing a modifier stops committing, and the cascade waits for
/// the commit or cancel key.
///
/// For sessions nobody is holding anything for.  The launch shortcut opens the
/// cascade from a click on the desktop or the taskbar: there is no hotkey down,
/// so the classic "release commits" rule has nothing to release, and the first
/// unrelated Win or Alt the user let go of would commit a window they had not
/// chosen.  (Search raises the same latch from the other side — see the typed
/// character in the hook.)
///
/// Call AFTER raising the session: SetSessionActive resets per-session state,
/// which includes this latch.
void LatchToggleSession();

/// Identity of the session currently running (0 = none has ever run).
///
/// THREE threads raise and drop the session flag: the hook thread (hotkey),
/// the touchpad worker (gesture) and the app thread (the controller's
/// teardown).  The teardown is long and pumps no messages, so a hotkey pressed
/// during it starts the NEXT session while the previous one is still being
/// dismantled — and a teardown that just cleared a bare flag would clear the
/// new session's, leaving the cascade up with the hook disarmed.  Taking the
/// identity at Activate and handing it back at teardown is what makes the two
/// tell each other apart.
uint64_t CurrentSessionEpoch();

/// End the session with this identity, and ONLY that one.
///
/// Returns false — changing nothing — when a newer session has since taken
/// over, which is precisely the case a plain "session = false" got wrong.
/// Otherwise clears the flag (if still set), resets the per-session state the
/// unconditional store always reset, and returns true.
bool EndSessionIfEpoch(uint64_t epoch);

/// True when the in-cascade pointer bindings claim a plain LEFT click.
///
/// A one-finger tap on a precision touchpad is ALSO a left click: Windows
/// synthesises one, the mouse hook sees it, and the pointer path commits the
/// tile under the cursor.  If the gesture source commits as well, one physical
/// tap produces two commits from two threads, and whichever message the app
/// drains first decides which window is raised.  The gesture source asks this
/// and stands down — the pointer answer is the better one anyway, because it
/// acts on the tile being pointed at instead of on the front slot.
bool PointerOwnsLeftClick();

/// Make this process the one that "received the last input event".
///
/// Windows refuses a foreground change — including the shell's show-desktop —
/// from a process that has not just received input.  The overlay is
/// WS_EX_NOACTIVATE and every keystroke the cascade uses is SWALLOWED by the
/// hook, so CKFlip normally qualifies only by accident: SwallowModifierRelease
/// injects a keystroke on the way out of a held-modifier commit, and injected
/// input counts.  That accident is why committing with the keyboard has
/// always worked and picking the desktop with the MOUSE did not — a click
/// injects nothing, the request was refused, and the desktop simply never
/// appeared.
///
/// Injects the same unmapped dummy key the hook already uses for its own
/// purposes: it produces no character, no application can act on it, and our
/// own hook passes injected events straight through.
void AssertInputOwnership();

/// End the session on behalf of an input source that is NOT the keyboard —
/// a click on a tile, a touchpad tap.
///
/// The keyboard paths do their own bookkeeping before they post: committing
/// with Enter arms the defusal that swallows the next Win/Alt release, so the
/// Start menu cannot pop after a session that was opened by holding Win.  A
/// pointer commit cannot do that from inside the hook, because the hook only
/// sees a click — it has no idea whether that click hit a tile or the
/// backdrop.  Without this, picking a window with the mouse while still
/// holding Win left the OS seeing a bare Win press-and-release, and the Start
/// menu opened over whatever had just been switched to.
///
/// Only ever call this for a genuinely non-keyboard commit.  Calling it on a
/// keyboard path would re-evaluate "is a modifier still down" a few
/// milliseconds after the hook already answered it, and a race there arms the
/// defusal for a release that has already happened — eating one later,
/// unrelated Start-menu press.
void EndSessionForeign();

/// Re-arm a session the hook has already dropped, WITHOUT resetting the
/// per-session state SetSessionActive clears.  Exists for one case: the
/// cancel key with a search query typed, where the hook must still do its
/// modifier-release bookkeeping (that is what keeps the Start menu shut) but
/// the controller — the only side that can see the query — decides the
/// cascade stays open.
void ResumeSession();

/// Drop a session that never actually opened (FlipController::Activate
/// aborts when nothing is eligible for the cascade), so the hook does not
/// keep swallowing input for a cascade that is not there.  A trigger that
/// is still physically held is left alone: its release still belongs to the
/// combination and must keep being defused (Start menu / menu bar).
///
/// Takes the identity of the session being given up, and drops nothing else —
/// the failed activation's window scan is slow enough for a second trigger to
/// have started a real session in the meantime.  See EndSessionIfEpoch.
void AbortSessionIfIdle(uint64_t epoch);

/// True when the Controls-page activation filters (Ignore in fullscreen
/// applications, the ignored-apps list) say this activation should not
/// happen.  Exposed so the touchpad gesture source honours exactly the same
/// rules as the hotkey — one set of filters, both input paths.
bool ShouldIgnoreActivation();

/// Hold activation off for `ms` milliseconds (0 releases it immediately).
///
/// The Settings app's touchpad-activity panel takes the pointer and reads raw
/// contacts to preview the gestures — but the core is still listening to the
/// same pad and the same keyboard, so practising the opening diagonal (or
/// brushing the hotkey) would fling the real cascade over the settings window.
/// This gates every activation path at once, because they all pass through
/// ShouldIgnoreActivation.
///
/// The deadline EXPIRES on its own rather than waiting to be cleared: the
/// suspending process could be killed mid-preview, and a switcher that stayed
/// permanently disarmed because a settings window died is far worse than one
/// that re-arms a couple of seconds late.  The caller simply re-sends while it
/// still wants the hold.
void SuspendActivation(unsigned ms);

/// Window messages the hook posts to the app window.
///
///   activate      — first Win+Tab
///   cycle         — subsequent Tab / arrow-down / scroll-down while active
///   cycleBack     — Shift+Tab / arrow-up / scroll-up while active
///   dismiss       — Win released (commit)
///   escape        — cancel key pressed while active
///   cycleStop     — main key released; stop queuing cycles
///   scrub         — drag with Window snap off; lParam holds a signed
///                   fixed-point delta (1/10000 window, + = forward)
///   scrubEnd      — that drag ended; settle onto the nearest window
///   pointerMove   — pointer moved over the overlay; wParam = screen X,
///                   lParam = screen Y (both signed LONG)
///   pointerSelect — select button pressed; same coordinate packing
///   pointerClose  — close button pressed; same coordinate packing
///   closeSelected — Delete pressed; close the hovered or front window
///   searchChar    — a printable key no binding claimed; wParam = character
///   searchBack    — Backspace while the search query is being typed
struct Messages {
    UINT activate      = 0;
    UINT cycle         = 0;
    UINT cycleBack     = 0;
    UINT dismiss       = 0;
    UINT escape        = 0;
    UINT cycleStop     = 0;
    UINT scrub         = 0;
    UINT scrubEnd      = 0;
    UINT pointerMove   = 0;
    UINT pointerSelect = 0;
    UINT pointerClose  = 0;
    UINT closeSelected = 0;
    UINT searchChar    = 0;
    UINT searchBack    = 0;
};

/// Install the low-level keyboard hook.
/// The hook callback is deliberately minimal (no allocations, no blocking
/// calls) to stay well under the LowLevelHooksTimeout (~300 ms on Win 11).
bool Install(HWND hwndNotify, const Messages& msgs);

void Uninstall();

} // namespace KeyboardHook
