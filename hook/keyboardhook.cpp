#include "keyboardhook.h"
#include "../core/Diagnostics.h"

#include <shellapi.h>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cwctype>
#include <iterator>

#pragma comment(lib, "shell32.lib")

namespace KeyboardHook {
namespace {

// ---------------------------------------------------------------------------
// Threading model
// ---------------------------------------------------------------------------
// Low-level hooks are dispatched on the thread that installed them, and that
// thread MUST be pumping messages or the OS unhooks us with a 300 ms timeout
// stutter on every input.  The render thread spends most of its time inside
// D3D Present / DwmFlush, which is exactly when we don't want hooks queued.
//
// We therefore install both hooks on a dedicated TIME_CRITICAL thread that
// does nothing but GetMessage.  The hook callbacks still post to the render
// window via PostMessage (non-blocking), so cycle/dismiss/etc. flow into the
// app's normal message queue with no semantic change.
//
// All mutable hook state (the wheel accumulator, swallow flags) is touched
// only on the hook thread.  The session word is atomic so Install/Uninstall
// on the app thread can reset it safely.
// ---------------------------------------------------------------------------

HHOOK    g_hook          = nullptr;
HHOOK    g_mouseHook     = nullptr;
HWND     g_hwndNotify    = nullptr;
Messages g_msg;

// Free stack movement (Window snap off): drag state, hook thread only.
// kMouseScrubPixels is how far the pointer travels for one window — tuned so
// a comfortable wrist drag walks a full 10-window stack.
constexpr float kMouseScrubPixels = 260.0f;
bool  g_mouseDragging = false;
LONG  g_mouseDragLastX = 0;
float g_mouseDragResidue = 0.0f;   // sub-unit remainder of the fixed-point post

// ---------------------------------------------------------------------------
// Session state AND session IDENTITY, in ONE atomic.
//   bit 0     — a switcher session is running
//   bits 1..  — WHICH session it is (monotonic, never reused)
//
// Packed for the same reason g_hkSpec is: two separate atomics cannot be
// tested and updated together, and "is this still the session I started?" is
// a question about both halves at once.
//
// Why identity is needed at all: THREE threads raise and drop this flag — the
// hook thread (hotkey), the touchpad worker (gesture) and the app thread (the
// controller's teardown).  The controller's teardown runs long (DwmFlush,
// StopCaptures, UncloakAll) with no message pumping, so a hotkey pressed
// during it opens the NEXT session while the previous one is still tearing
// down.  A bare bool teardown then cleared the flag of a session that had
// already begun — leaving the cascade on screen with the hook disarmed, every
// keystroke passing through to whatever is behind the overlay.  With an
// identity, EndSessionIfEpoch can refuse to end a session that is not the one
// it was handed.
std::atomic<uint64_t> g_session{0};

inline bool     SessionBitOf(uint64_t v)   noexcept { return (v & 1u) != 0; }
inline uint64_t SessionEpochOf(uint64_t v) noexcept { return v >> 1; }

inline bool SessionRunning() noexcept
{
    return SessionBitOf(g_session.load(std::memory_order_relaxed));
}

/// A NEW session begins, so it gets a new identity.  Returns that identity.
uint64_t RaiseSession() noexcept
{
    uint64_t cur = g_session.load(std::memory_order_relaxed);
    uint64_t next;
    do {
        next = ((SessionEpochOf(cur) + 1) << 1) | 1u;
    } while (!g_session.compare_exchange_weak(cur, next,
                                              std::memory_order_relaxed));
    return SessionEpochOf(next);
}

/// The running session ends.  Its identity stays spent, so nothing can
/// mistake a later session for it.
inline void DropSession() noexcept
{
    g_session.fetch_and(~uint64_t{1}, std::memory_order_relaxed);
}

// When the session is committed early (Enter) while combo modifiers are
// still physically held, the next release of a Win/Alt combo modifier must
// still be swallowed (dummy-key trick) so the Start menu / window menu bar
// doesn't pop.
//
// Atomic because EndSessionForeign arms it from whichever thread the
// non-keyboard commit came in on (the render thread for a mouse click, the
// touchpad worker for a tap) — see that function.  Every other use is still
// hook-thread only, and the semantics are unchanged.
std::atomic<bool> g_suppressNextModRelease{false};

// Bare-modifier binding: true while the last main-key DOWN was consumed by
// us, so the matching UP must be swallowed too.  When the press was passed
// through (fullscreen ignore / ignored app), the release passes through as
// well and the OS shortcut (Start menu etc.) behaves normally.
bool g_bareMainConsumed = false;

HANDLE   g_hookThread        = nullptr;
DWORD    g_hookThreadId      = 0;
HANDLE   g_hookReadyEvent    = nullptr;
std::atomic<bool> g_hookInstallOk{false};

// Mouse-wheel accumulator — high-res mice stream sub-WHEEL_DELTA events.
// Cycle only on full ±WHEEL_DELTA (120) multiples, and reject opposite-
// direction events within 80 ms of the last posted cycle (debounce the
// tiny reverse spikes modern wheels emit mid-scroll).
//
// Read/modified on the hook thread only, but atomic so SetSessionActive()
// can clear them from whichever thread opens a session (the touchpad
// gesture worker does).  Single-threaded semantics are unchanged.
std::atomic<int32_t>   g_wheelAccum{0};
std::atomic<ULONGLONG> g_lastCyclePostMs{0};
std::atomic<int>       g_lastCycleDir{0};   // +1 = cycled forward, -1 = cycled back
constexpr ULONGLONG kWheelFlipDebounceMs = 80;

constexpr WORD kVkDummy = 0xFF;

// ---------------------------------------------------------------------------
// Trigger options — written from the app thread (SetOptions), read on the
// hook thread.  The vector is only touched under the lock; everything the
// per-event paths need is mirrored as atomics so they never take the lock.
// ---------------------------------------------------------------------------
SRWLOCK            g_optLock = SRWLOCK_INIT;
std::vector<std::wstring> g_optIgnoredApps;          // lowercase, under lock
std::atomic<bool>  g_optIgnoreFullscreen{false};
std::atomic<bool>  g_optWheelCycle{true};
std::atomic<bool>  g_optKeyboardNav{true};
std::atomic<bool>  g_optToggleMode{false};
std::atomic<bool>  g_optTouchpadNav{true};
std::atomic<bool>  g_optWindowSnap{true};
std::atomic<bool>  g_optHasIgnoredApps{false};
std::atomic<bool>  g_optPointerInCascade{true};
std::atomic<bool>  g_optMouseSelect{true};
std::atomic<int>   g_optSelectButton{kMouseLeft};
std::atomic<bool>  g_optDragEnabled{true};
std::atomic<int>   g_optDragButton{kMouseRight};
std::atomic<bool>  g_optCloseFromCascade{true};
std::atomic<int>   g_optCloseButton{kMouseMiddle};
std::atomic<bool>  g_optCloseKeyEnabled{true};
std::atomic<bool>  g_optSearchEnabled{false};
// Commit / cancel / close main keys (see TriggerOptions).  Plain VKs — the
// parse happens once in SetOptions, never on the hook thread.
std::atomic<unsigned> g_optCommitVk{VK_RETURN};
std::atomic<unsigned> g_optCancelVk{VK_ESCAPE};
std::atomic<unsigned> g_optCloseVk{VK_DELETE};

// Set the moment a character reaches the search query, and cleared when a
// session starts or ends.
//
// You cannot type while holding Win+Tab: letting go of the modifier to reach
// the keyboard IS the commit, so the cascade closed on the first character —
// and whatever was typed after that went to Windows as Win+key shortcuts.
// So the first typed character promotes THIS session to toggle semantics: the
// modifier release stops committing and the cascade waits for the commit or
// cancel key, exactly as the Toggle activation option does permanently.
// Nothing about a session where nobody types changes.
std::atomic<bool>  g_searchLatched{false};

// Activation hold deadline (see SuspendActivation).  A tick count, so it
// expires by itself if whoever armed it never comes back.
std::atomic<ULONGLONG> g_suspendUntilMs{0};

// Parsed activation combination (see HotkeySpec).  Defaults to Win+Tab.
// Packed into ONE atomic (bits 0-7 = modMask, bits 8-39 = mainVk,
// bit 40 = mainIsMouse) so SetOptions can never publish a mixed old/new
// combination to the hook thread mid-update.
constexpr uint64_t PackHotkeySpec(uint8_t mod, unsigned vk, bool isMouse) noexcept
{
    return uint64_t(mod)
         | (uint64_t(vk) << 8)
         | (uint64_t(isMouse ? 1 : 0) << 40);
}
inline void UnpackHotkeySpec(uint64_t v, uint8_t& mod, unsigned& vk, bool& isMouse) noexcept
{
    mod     = static_cast<uint8_t>(v & 0xFF);
    vk      = static_cast<unsigned>((v >> 8) & 0xFFFFFFFFull);
    isMouse = ((v >> 40) & 1) != 0;
}
std::atomic<uint64_t> g_hkSpec{ PackHotkeySpec(kModWin, VK_TAB, false) };

std::wstring ToLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

const wchar_t* FileNameOf(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path.c_str() : path.c_str() + slash + 1;
}

// True if the current foreground app is fullscreen (D3D exclusive, F11
// browser fullscreen, presentation mode).  Called only on the activation
// keypress, never per-event.
bool ForegroundIsFullscreen()
{
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state)))
        return false;
    return state == QUNS_BUSY                      // fullscreen window (F11 etc.)
        || state == QUNS_RUNNING_D3D_FULL_SCREEN   // exclusive D3D app
        || state == QUNS_PRESENTATION_MODE;        // presentation mode
}

// True if the foreground process executable matches an ignore-list entry
// (full path match or bare exe-name match, case-insensitive).
bool ForegroundIsIgnoredApp()
{
    if (!g_optHasIgnoredApps.load(std::memory_order_relaxed))
        return false;

    HWND fg = GetForegroundWindow();
    if (!fg) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return false;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;

    wchar_t buf[MAX_PATH * 2] = {};
    DWORD len = static_cast<DWORD>(std::size(buf));
    bool ok = QueryFullProcessImageNameW(proc, 0, buf, &len) != 0;
    CloseHandle(proc);
    if (!ok) return false;

    std::wstring fullPath = ToLower(buf);
    std::wstring fileName = FileNameOf(fullPath);

    AcquireSRWLockShared(&g_optLock);
    bool match = false;
    for (const auto& entry : g_optIgnoredApps) {
        if (entry.empty()) continue;
        if (entry == fullPath || entry == fileName
            || std::wstring(FileNameOf(entry)) == fileName) {
            match = true;
            break;
        }
    }
    ReleaseSRWLockShared(&g_optLock);
    return match;
}

// Combined activation filter — when true, the hotkey is passed through to
// the OS untouched (no capture, no blocking).
bool ShouldIgnoreActivationImpl()
{
    // Held off by the Settings app's touchpad-activity preview (see
    // SuspendActivation).  First, and cheapest: one atomic read.
    const ULONGLONG until = g_suspendUntilMs.load(std::memory_order_relaxed);
    if (until != 0 && GetTickCount64() < until)
        return true;

    if (g_optIgnoreFullscreen.load(std::memory_order_relaxed)
        && ForegroundIsFullscreen())
        return true;
    return ForegroundIsIgnoredApp();
}

// VK for a configured mouse-button binding (0 = unbound).
unsigned VkForButtonId(int id)
{
    switch (id) {
    case kMouseLeft:   return VK_LBUTTON;
    case kMouseRight:  return VK_RBUTTON;
    case kMouseMiddle: return VK_MBUTTON;
    case kMouseX1:     return VK_XBUTTON1;
    case kMouseX2:     return VK_XBUTTON2;
    default:           return 0;
    }
}

// Screen coordinates travel as two signed LONGs (wParam / lParam) rather than
// packed into one — a virtual desktop can easily run past what a SHORT holds,
// and a wrapped coordinate would put the hit test on the wrong monitor.
inline void PostPointer(UINT msg, LONG x, LONG y)
{
    // g_hwndNotify too, not just the message id: Uninstall clears it, and
    // PostMessage(nullptr, ...) would quietly post a THREAD message to the
    // hook thread's own queue instead of going nowhere.
    if (msg != 0 && g_hwndNotify != nullptr)
        PostMessage(g_hwndNotify, msg,
                    static_cast<WPARAM>(static_cast<LONG_PTR>(x)),
                    static_cast<LPARAM>(static_cast<LONG_PTR>(y)));
}

// ---------------------------------------------------------------------------
// Modifier helpers
// ---------------------------------------------------------------------------

// Returns the kMod* bit for a modifier VK, or 0 for non-modifier keys.
uint8_t ModifierBitOf(DWORD vk)
{
    switch (vk) {
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return kModCtrl;
    case VK_LSHIFT:   case VK_RSHIFT:   case VK_SHIFT:   return kModShift;
    case VK_LMENU:    case VK_RMENU:    case VK_MENU:    return kModAlt;
    case VK_LWIN:     case VK_RWIN:                      return kModWin;
    default: return 0;
    }
}

bool ModBitDown(uint8_t bit)
{
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    switch (bit) {
    case kModCtrl:  return down(VK_CONTROL);
    case kModShift: return down(VK_SHIFT);
    case kModAlt:   return down(VK_MENU);
    case kModWin:   return down(VK_LWIN) || down(VK_RWIN);
    default:        return false;
    }
}

bool ModsSatisfied(uint8_t mask)
{
    if (mask & kModCtrl  && !ModBitDown(kModCtrl))  return false;
    if (mask & kModShift && !ModBitDown(kModShift)) return false;
    if (mask & kModAlt   && !ModBitDown(kModAlt))   return false;
    if (mask & kModWin   && !ModBitDown(kModWin))   return false;
    return true;
}

// Any combo modifier still physically held?  Used on Enter/Escape to decide
// whether the NEXT modifier release still belongs to the activation combo
// (and must be swallowed) — in toggle mode the modifiers are typically long
// released by commit time, and suppressing a future unrelated Win release
// would eat one legitimate Start-menu open.
bool AnyComboModDown(uint8_t mask)
{
    if (mask & kModCtrl  && ModBitDown(kModCtrl))  return true;
    if (mask & kModShift && ModBitDown(kModShift)) return true;
    if (mask & kModAlt   && ModBitDown(kModAlt))   return true;
    if (mask & kModWin   && ModBitDown(kModWin))   return true;
    return false;
}

// On a modifier keyup the async state of the RELEASED key may still read
// down (the LL hook runs before the input updates the key state), so "is
// the modifier pair fully released" only consults the OTHER side.
bool PairStillDown(DWORD releasedVk)
{
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    switch (releasedVk) {
    case VK_LCONTROL: return down(VK_RCONTROL);
    case VK_RCONTROL: return down(VK_LCONTROL);
    case VK_LSHIFT:   return down(VK_RSHIFT);
    case VK_RSHIFT:   return down(VK_LSHIFT);
    case VK_LMENU:    return down(VK_RMENU);
    case VK_RMENU:    return down(VK_LMENU);
    case VK_LWIN:     return down(VK_RWIN);
    case VK_RWIN:     return down(VK_LWIN);
    default:          return false;
    }
}

// Main-key match.  Left/right variants of a modifier-as-main binding (bare
// Win key etc.) match either physical key.
bool MatchesMainVk(DWORD vk, unsigned mainVk)
{
    if (vk == mainVk) return true;
    switch (mainVk) {
    case VK_LWIN:     case VK_RWIN:     return vk == VK_LWIN     || vk == VK_RWIN;
    case VK_LCONTROL: case VK_RCONTROL: return vk == VK_LCONTROL || vk == VK_RCONTROL;
    case VK_LSHIFT:   case VK_RSHIFT:   return vk == VK_LSHIFT   || vk == VK_RSHIFT;
    case VK_LMENU:    case VK_RMENU:    return vk == VK_LMENU    || vk == VK_RMENU;
    default: return false;
    }
}

void ResetWheelState()
{
    g_wheelAccum      = 0;
    g_lastCyclePostMs = 0;
    g_lastCycleDir    = 0;
}

// Toggle semantics for THIS session: the user's permanent setting, or the
// search latch a typed character raised (see g_searchLatched).
bool ToggleSemantics()
{
    return g_optToggleMode.load(std::memory_order_relaxed)
        || g_searchLatched.load(std::memory_order_relaxed);
}

// A session is beginning: nothing from the last one carries over.
void BeginSessionState()
{
    g_searchLatched.store(false, std::memory_order_relaxed);
    ResetWheelState();
}

// Swallow a Win/Alt keyup so the OS doesn't open the Start menu (Win) or
// activate the window menu bar (Alt).  The dummy-key sandwich makes the OS
// see "some key" between the modifier press and release, which cancels the
// single-modifier shortcut, then replays the release as injected input.
void SwallowModifierRelease(DWORD vk)
{
    INPUT inputs[3] = {};
    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = kVkDummy;
    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = kVkDummy;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[2].type       = INPUT_KEYBOARD;
    inputs[2].ki.wVk     = static_cast<WORD>(vk);
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(3, inputs, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// The printable character a keystroke would produce, or 0.
//
// A low-level hook runs BEFORE the key reaches the input state, so
// GetKeyboardState here describes the previous keystroke — hence the state
// array is built from GetAsyncKeyState instead.  The layout is the FOREGROUND
// window's, not ours: whoever is behind the overlay is the app the user was
// last typing into, and their layout is the one their fingers expect.
//
// ToUnicodeEx is not a pure function — a dead key (n < 0) arms a compose state
// that the next call would fold into.  Running the translation a second time
// consumes it, which is the standard way to leave the state as we found it.
wchar_t TranslateToChar(const KBDLLHOOKSTRUCT* kb)
{
    // Ctrl / Alt combinations are commands, not text.  (AltGr shows up as
    // Ctrl+Alt, so this also declines AltGr characters rather than risk
    // eating a shortcut — a niche loss against a real correctness win.)
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000)
        || (GetAsyncKeyState(VK_MENU) & 0x8000))
        return 0;

    BYTE state[256] = {};
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1)         state[VK_CAPITAL] = 0x01;

    const HKL layout = GetKeyboardLayout(
        GetWindowThreadProcessId(GetForegroundWindow(), nullptr));

    WCHAR buf[8] = {};
    int n = ToUnicodeEx(kb->vkCode, kb->scanCode, state, buf,
                        static_cast<int>(std::size(buf)), 0, layout);
    if (n < 0) {
        // Dead key — flush the compose state and take nothing.
        ToUnicodeEx(kb->vkCode, kb->scanCode, state, buf,
                    static_cast<int>(std::size(buf)), 0, layout);
        return 0;
    }
    // n == 2 is a dead key that did NOT combine, handed back as the dead
    // character followed by the one just typed.  The call above has already
    // consumed the compose state, so returning nothing here would swallow the
    // keystroke outright — take the character the user actually pressed, which
    // is the last one.  (n > 2 is a layout handing back a ligature we have no
    // single character for; still declined.)
    if (n != 1 && n != 2)
        return 0;

    const wchar_t c = buf[n - 1];
    return (c >= 0x20 && c != 0x7F) ? c : 0;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

    if (kb->flags & LLKHF_INJECTED)
        return CallNextHookEx(g_hook, nCode, wParam, lParam);

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

    uint8_t  modMask     = 0;
    unsigned mainVk      = 0;
    bool     mainIsMouse = false;
    UnpackHotkeySpec(g_hkSpec.load(std::memory_order_relaxed),
                     modMask, mainVk, mainIsMouse);
    const bool active = SessionRunning();

    const uint8_t modBit    = ModifierBitOf(kb->vkCode);
    const bool    isMainKey = !mainIsMouse && MatchesMainVk(kb->vkCode, mainVk);

    // ---- Modifier keys ----------------------------------------------------
    if (modBit) {
        // Bare-modifier binding (e.g. the Windows key alone) — toggle
        // semantics: press activates, further presses cycle, Enter commits,
        // Escape cancels.  The press AND release are swallowed so the OS
        // never sees the single-modifier shortcut.
        if (isMainKey && modMask == 0) {
            if (isDown) {
                if (!active) {
                    if (ShouldIgnoreActivationImpl())
                        return CallNextHookEx(g_hook, nCode, wParam, lParam);
                    RaiseSession();
                    BeginSessionState();
                    PostMessage(g_hwndNotify, g_msg.activate, 0, 0);
                } else {
                    PostMessage(g_hwndNotify, g_msg.cycle, 0, 0);
                }
                g_bareMainConsumed = true;
                return 1;
            }
            if (isUp && (g_bareMainConsumed || SessionRunning())) {
                // Swallow the release that belongs to a swallowed press —
                // for Win/Alt also defuse the Start-menu / menu-bar side
                // effect.
                g_bareMainConsumed = false;
                if (kb->vkCode == VK_LWIN || kb->vkCode == VK_RWIN
                    || kb->vkCode == VK_LMENU || kb->vkCode == VK_RMENU)
                    SwallowModifierRelease(kb->vkCode);
                return 1;
            }
            return CallNextHookEx(g_hook, nCode, wParam, lParam);
        }

        // Combo-modifier release = commit (classic Win-release behaviour),
        // once BOTH sides of the pair are up.  Toggle mode (config
        // hotkeyToggleMode): the release does NOT commit — the session
        // stays open until Enter/Escape — but a Win/Alt release is still
        // defused so the Start menu / menu bar can't pop over the cascade.
        if (isUp && (modMask & modBit) && !PairStillDown(kb->vkCode)) {
            const bool needSwallow =
                (modBit == kModWin || modBit == kModAlt);
            if (active && ToggleSemantics()) {
                if (needSwallow) {
                    SwallowModifierRelease(kb->vkCode);
                    return 1;
                }
            } else if (active) {
                DropSession();
                g_suppressNextModRelease = false;
                ResetWheelState();
                PostMessage(g_hwndNotify, g_msg.dismiss, 0, 0);
                if (needSwallow) {
                    SwallowModifierRelease(kb->vkCode);
                    return 1;
                }
            } else if (g_suppressNextModRelease.load(std::memory_order_relaxed)) {
                // Session was committed early (Enter) — this release still
                // belongs to the combo; keep the Start menu shut.
                g_suppressNextModRelease = false;
                if (needSwallow) {
                    SwallowModifierRelease(kb->vkCode);
                    return 1;
                }
            }
        }
        return CallNextHookEx(g_hook, nCode, wParam, lParam);
    }

    // ---- Main key (non-modifier) -------------------------------------------
    if (isMainKey) {
        // Toggle mode: once the session is open the combo modifiers are
        // typically released — the bare main key keeps cycling (matching
        // single-key-binding behaviour).  Activation still requires the
        // full combination.
        const bool cycleWithoutMods = active && ToggleSemantics();
        if (isDown && (ModsSatisfied(modMask) || cycleWithoutMods)) {
            bool shiftHeld = !(modMask & kModShift)
                          && ((GetAsyncKeyState(VK_LSHIFT) & 0x8000)
                           || (GetAsyncKeyState(VK_RSHIFT) & 0x8000));
            if (!active) {
                // Session not started yet — honour the trigger filters
                // (fullscreen apps / ignore list).  Pass the key through so
                // the OS handles the combo normally instead of blocking it.
                if (ShouldIgnoreActivationImpl())
                    return CallNextHookEx(g_hook, nCode, wParam, lParam);
                RaiseSession();
                g_suppressNextModRelease = false;
                BeginSessionState();
                PostMessage(g_hwndNotify, g_msg.activate, 0, 0);
            } else {
                PostMessage(g_hwndNotify,
                            shiftHeld ? g_msg.cycleBack : g_msg.cycle, 0, 0);
            }
            return 1;
        }
        if (isUp && active) {
            // Main key released while session active — tell controller to
            // stop queuing cycles so continuous scroll doesn't over-shoot.
            PostMessage(g_hwndNotify, g_msg.cycleStop, 0, 0);
            return 1;
        }
        // Fall through (mods not satisfied) — the session-active catch-all
        // below still swallows strays.
    }

    // ---- Extra navigation while session is active ------------------------
    if (active) {
        // Arrow keys — cycle on keydown, stop on keyup.  With keyboard
        // navigation disabled they fall through to the catch-all below,
        // which still swallows them (no stray input to background apps).
        const bool arrowsOn = g_optKeyboardNav.load(std::memory_order_relaxed);
        if (arrowsOn && (kb->vkCode == VK_DOWN || kb->vkCode == VK_RIGHT)) {
            if (isDown)
                PostMessage(g_hwndNotify, g_msg.cycle, 0, 0);
            else if (isUp)
                PostMessage(g_hwndNotify, g_msg.cycleStop, 0, 0);
            return 1;
        }
        if (arrowsOn && (kb->vkCode == VK_UP || kb->vkCode == VK_LEFT)) {
            if (isDown)
                PostMessage(g_hwndNotify, g_msg.cycleBack, 0, 0);
            else if (isUp)
                PostMessage(g_hwndNotify, g_msg.cycleStop, 0, 0);
            return 1;
        }

        // Commit the current selection (Enter by default, see commitHotkey).
        // Required for bindings with no hold modifier (bare key / mouse
        // button); a convenience for the classic combos.  If combo modifiers
        // are still held, their release is suppressed once so the Start menu
        // doesn't pop afterwards.
        if (isDown
            && kb->vkCode == g_optCommitVk.load(std::memory_order_relaxed)) {
            DropSession();
            // Suppress the upcoming combo-modifier release only while one
            // is actually still held — in toggle mode the modifiers are
            // usually up long before commit, and a stale flag would eat
            // the next unrelated Win/Alt release.
            g_suppressNextModRelease = (modMask != 0) && AnyComboModDown(modMask);
            ResetWheelState();
            PostMessage(g_hwndNotify, g_msg.dismiss, 0, 0);
            return 1;
        }

        // Cancel (Escape by default, see cancelHotkey).  The hook ends the
        // session here exactly as it always has; when a search query is being
        // typed the controller re-arms it (KeyboardHook::SetSessionActive)
        // because there the first Esc only clears the query — that decision
        // needs the query, which lives on the other side.  Keeping the
        // modifier-release bookkeeping on this side is what matters: it is
        // the part that stops the Start menu popping over the cascade.
        if (isDown
            && kb->vkCode == g_optCancelVk.load(std::memory_order_relaxed)) {
            DropSession();
            g_suppressNextModRelease = (modMask != 0) && AnyComboModDown(modMask);
            ResetWheelState();
            PostMessage(g_hwndNotify, g_msg.escape, 0, 0);
            return 1;
        }

        // Close the hovered (or selected) window — the switcher doubling as
        // a window manager.  Gated on the same toggle as the close click.
        {
            const unsigned closeVk =
                g_optCloseVk.load(std::memory_order_relaxed);
            // The key has its OWN switch — not the mouse master, and not the
            // close CLICK's switch either.  It acts on the selection when no
            // tile is hovered, so it works with every pointer feature off, and
            // wanting one of the two bindings without the other is reasonable
            // in both directions.
            if (isDown && closeVk != 0 && kb->vkCode == closeVk
                && g_optCloseKeyEnabled.load(std::memory_order_relaxed)) {
                if (g_msg.closeSelected != 0)
                    PostMessage(g_hwndNotify, g_msg.closeSelected, 0, 0);
                return 1;
            }
        }

        // ---- Type-to-filter ----------------------------------------------
        // LAST, deliberately: every binding above claims its key first, so
        // "any character no binding wants" is exactly what reaches the query.
        if (isDown && g_optSearchEnabled.load(std::memory_order_relaxed)) {
            if (kb->vkCode == VK_BACK) {
                if (g_msg.searchBack != 0)
                    PostMessage(g_hwndNotify, g_msg.searchBack, 0, 0);
                return 1;
            }
            const wchar_t ch = TranslateToChar(kb);
            if (ch != 0 && g_msg.searchChar != 0) {
                // Typing means the hand has left the hotkey — from here the
                // session holds itself open (see g_searchLatched), so
                // releasing Win/Alt no longer commits and the rest of the
                // word cannot escape into Windows as Win+key shortcuts.
                g_searchLatched.store(true, std::memory_order_relaxed);
                PostMessage(g_hwndNotify, g_msg.searchChar,
                            static_cast<WPARAM>(ch), 0);
                return 1;
            }
        }

        // Eat all other keys while session is active (prevent stray input).
        // Modifier keys always pass (handled above) so held combos stay
        // consistent for the OS.
        return 1;
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Mouse-button VK for a LL mouse message (0 when not a button-down/up event).
// `isDown` receives whether it was the press.  Wheel/move return 0.
unsigned MouseButtonVk(WPARAM wParam, const MSLLHOOKSTRUCT* ms, bool& isDown)
{
    switch (wParam) {
    case WM_LBUTTONDOWN: isDown = true;  return VK_LBUTTON;
    case WM_LBUTTONUP:   isDown = false; return VK_LBUTTON;
    case WM_RBUTTONDOWN: isDown = true;  return VK_RBUTTON;
    case WM_RBUTTONUP:   isDown = false; return VK_RBUTTON;
    case WM_MBUTTONDOWN: isDown = true;  return VK_MBUTTON;
    case WM_MBUTTONUP:   isDown = false; return VK_MBUTTON;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        isDown = (wParam == WM_XBUTTONDOWN);
        return HIWORD(ms->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    default:
        isDown = false;
        return 0;
    }
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode != HC_ACTION)
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);

    const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);

    // Input injected by a LOWER-INTEGRITY process is not the user.  This exe
    // runs elevated, so UIPI already stops a medium-IL process from posting to
    // our windows — but SendInput goes into the GLOBAL input stream, which UIPI
    // does not gate, and the cascade's bindings include a DESTRUCTIVE one
    // (close the hovered window).  Refusing those events closes that hole.
    //
    // Deliberately NOT the blanket LLMHF_INJECTED the keyboard path uses:
    // touchpad tap-to-click, Remote Desktop and accessibility tools all reach
    // us as injected-but-same-or-higher-IL input, and rejecting those would
    // take the mouse away from users who depend on them.  Lower IL is the part
    // that is actually untrusted.
    if (ms->flags & LLMHF_LOWER_IL_INJECTED)
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);

    const bool active = SessionRunning();

    uint8_t  modMask     = 0;
    unsigned mainVk      = 0;
    bool     mainIsMouse = false;
    UnpackHotkeySpec(g_hkSpec.load(std::memory_order_relaxed),
                     modMask, mainVk, mainIsMouse);

    // ---- Mouse-button main key (axes are ignored by design) --------------
    if (mainIsMouse) {
        bool btnDown = false;
        unsigned btnVk = MouseButtonVk(wParam, ms, btnDown);
        if (btnVk != 0
            && btnVk == mainVk) {
            if (btnDown) {
                // Toggle mode: with the session open the bare main button
                // keeps cycling even after the combo modifiers were
                // released (see the keyboard-path counterpart).
                const bool cycleWithoutMods = active && ToggleSemantics();
                if (ModsSatisfied(modMask) || cycleWithoutMods) {
                    if (!active) {
                        if (ShouldIgnoreActivationImpl())
                            return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
                        RaiseSession();
                        g_suppressNextModRelease = false;
                        BeginSessionState();
                        PostMessage(g_hwndNotify, g_msg.activate, 0, 0);
                    } else {
                        bool shiftHeld = !(modMask & kModShift)
                                      && ((GetAsyncKeyState(VK_LSHIFT) & 0x8000)
                                       || (GetAsyncKeyState(VK_RSHIFT) & 0x8000));
                        PostMessage(g_hwndNotify,
                                    shiftHeld ? g_msg.cycleBack : g_msg.cycle, 0, 0);
                    }
                    return 1;
                }
                if (active)
                    return 1;   // swallow strays while the cascade is open
            } else {
                if (active) {
                    PostMessage(g_hwndNotify, g_msg.cycleStop, 0, 0);
                    return 1;
                }
            }
        }
    }

    if (!active) {
        if (g_mouseDragging) {
            // Session ended under a held button — drop the drag silently.
            g_mouseDragging = false;
            g_mouseDragResidue = 0.0f;
        }
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
    }

    // ---- Pointer over the cascade ----------------------------------------
    // A third input source alongside the keyboard and the touchpad, and just
    // as additive: the hover message is only posted when something is
    // listening for it, and every branch here is behind its own toggle, so a
    // user who wants none of this gets the pre-pointer cascade exactly.
    const bool pointerOn =
        g_optPointerInCascade.load(std::memory_order_relaxed);
    const bool selectOn = pointerOn
        && g_optMouseSelect.load(std::memory_order_relaxed);
    const bool closeOn  = pointerOn
        && g_optCloseFromCascade.load(std::memory_order_relaxed);
    const bool dragOn   = pointerOn
        && g_optDragEnabled.load(std::memory_order_relaxed);
    const unsigned selectVk = selectOn
        ? VkForButtonId(g_optSelectButton.load(std::memory_order_relaxed)) : 0;
    const unsigned closeVk  = closeOn
        ? VkForButtonId(g_optCloseButton.load(std::memory_order_relaxed)) : 0;
    const unsigned dragVk   = dragOn
        ? VkForButtonId(g_optDragButton.load(std::memory_order_relaxed)) : 0;

    // The hover highlight and the close click both need to know which tile is
    // under the pointer, so the move is reported whenever either is live.
    if (wParam == WM_MOUSEMOVE && (selectOn || closeOn))
        PostPointer(g_msg.pointerMove, ms->pt.x, ms->pt.y);

    {
        bool btnDown = false;
        const unsigned btnVk = MouseButtonVk(wParam, ms, btnDown);
        if (btnVk != 0) {
            // Select wins a collision with the other two bindings — it is the
            // one the user is most likely to have meant, and the Settings page
            // warns before letting a duplicate be saved.
            if (selectVk != 0 && btnVk == selectVk) {
                if (btnDown)
                    PostPointer(g_msg.pointerSelect, ms->pt.x, ms->pt.y);
                return 1;   // both edges — the release belongs to the press
            }
            if (closeVk != 0 && btnVk == closeVk) {
                if (btnDown)
                    PostPointer(g_msg.pointerClose, ms->pt.x, ms->pt.y);
                return 1;
            }
        }
    }

    // ---- Free stack movement (Window snap off) ---------------------------
    // Hold the drag button (right by default) and move: the stack rides the
    // pointer instead of stepping window by window.  Wheel and keyboard are
    // untouched — they stay discrete by design.  With Window snap on (the
    // default) none of this runs and the mouse behaves exactly as it always
    // has.
    if (!g_optWindowSnap.load(std::memory_order_relaxed) && dragVk != 0) {
        bool dragBtnDown = false;
        const unsigned dragBtnVk = MouseButtonVk(wParam, ms, dragBtnDown);
        if (dragBtnVk == dragVk && dragBtnDown) {
            g_mouseDragging    = true;
            g_mouseDragLastX   = ms->pt.x;
            g_mouseDragResidue = 0.0f;
            return 1;
        }
        if (dragBtnVk == dragVk && !dragBtnDown && g_mouseDragging) {
            g_mouseDragging = false;
            PostMessage(g_hwndNotify, g_msg.scrubEnd, 0, 0);
            return 1;
        }
        if (wParam == WM_MOUSEMOVE && g_mouseDragging) {
            const float dxPx = static_cast<float>(ms->pt.x - g_mouseDragLastX);
            g_mouseDragLastX = ms->pt.x;
            // Dragging right pulls the row right, so the PREVIOUS window
            // comes forward — the same sense as the touchpad swipe.
            float windows = -dxPx / kMouseScrubPixels + g_mouseDragResidue;
            LONG fixed = static_cast<LONG>(windows * 10000.0f);
            g_mouseDragResidue = windows - static_cast<float>(fixed) / 10000.0f;
            if (fixed != 0)
                PostMessage(g_hwndNotify, g_msg.scrub, 0,
                            static_cast<LPARAM>(fixed));
            return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
        }
    }

    // Horizontal wheel: with touchpad gestures live, a two-finger cycle
    // swipe also reaches the OS as a horizontal scroll.  The gesture is
    // handled from raw input (hook/touchpadhook), so eat the scroll here —
    // it would otherwise land on whatever sits under the cursor.  With
    // touchpad navigation off, nothing changes.
    if (wParam == WM_MOUSEHWHEEL
        && g_optTouchpadNav.load(std::memory_order_relaxed))
        return 1;

    if (wParam == WM_MOUSEWHEEL) {
        // Wheel cycling disabled: still swallow the scroll while the
        // session is active (matching the keyboard catch-all so background
        // apps don't scroll invisibly), but post no cycle commands.
        if (!g_optWheelCycle.load(std::memory_order_relaxed))
            return 1;

        short delta = static_cast<short>(HIWORD(ms->mouseData));
        if (delta == 0)
            return 1;

        // Event direction: wheel-up (delta > 0) cycles back, wheel-down
        // (delta < 0) cycles forward.  Posted-cycle direction uses the same
        // sign convention: +1 = a back cycle was just posted, -1 = forward.
        int eventDir = (delta > 0) ? +1 : -1;
        ULONGLONG now = GetTickCount64();

        // Directional debounce: a flip opposite to the last posted cycle
        // inside the 80 ms window is almost always a high-res mouse's
        // spurious reverse spike — swallow it.
        if (g_lastCycleDir != 0 && eventDir != g_lastCycleDir
            && (now - g_lastCyclePostMs) < kWheelFlipDebounceMs) {
            return 1;
        }

        // Reset accumulator on direction change so residue from the
        // previous direction can't instantly trigger a reverse cycle.
        if (eventDir != g_lastCycleDir) {
            g_wheelAccum = 0;
        }

        g_wheelAccum += delta;

        // Drain the accumulator in WHEEL_DELTA steps.  Each full notch
        // posts exactly one cycle.
        while (g_wheelAccum >= WHEEL_DELTA) {
            PostMessage(g_hwndNotify, g_msg.cycleBack, 0, 0);
            g_wheelAccum     -= WHEEL_DELTA;
            g_lastCyclePostMs = now;
            g_lastCycleDir    = +1;
        }
        while (g_wheelAccum <= -WHEEL_DELTA) {
            PostMessage(g_hwndNotify, g_msg.cycle, 0, 0);
            g_wheelAccum     += WHEEL_DELTA;
            g_lastCyclePostMs = now;
            g_lastCycleDir    = -1;
        }
        return 1;   // eat the scroll while session is active
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Hook thread: installs both LL hooks on its own thread, runs a minimal
// GetMessage loop until WM_QUIT, then unhooks and exits.
// ---------------------------------------------------------------------------
DWORD WINAPI HookThreadProc(LPVOID /*param*/)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // Force a message queue to exist before SetWindowsHookExW, so the OS
    // can dispatch hook callbacks on this thread immediately.
    MSG dummy;
    PeekMessageW(&dummy, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    HMODULE hMod = GetModuleHandleW(nullptr);
    g_hook      = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hMod, 0);

    if (!g_hook)
        Diag::ReportLastError(Diag::Code::KeyboardHookFailed, Diag::Sev::Critical,
                              L"CKFlip3D could not watch the keyboard",
                              L"SetWindowsHookEx(WH_KEYBOARD_LL)");

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hMod, 0);
    if (!g_mouseHook)
        Diag::ReportLastError(Diag::Code::MouseHookFailed, Diag::Sev::Warning,
                              L"CKFlip3D could not watch the mouse",
                              L"SetWindowsHookEx(WH_MOUSE_LL) — the wheel will not "
                              L"cycle the stack and no mouse binding in the cascade "
                              L"will fire; the keyboard is unaffected");

    // Mouse hook is optional; only the keyboard hook is required for the
    // install to be considered successful.
    bool ok = (g_hook != nullptr);
    g_hookInstallOk.store(ok, std::memory_order_release);
    if (g_hookReadyEvent)
        SetEvent(g_hookReadyEvent);

    if (!ok) {
        if (g_hook)      { UnhookWindowsHookEx(g_hook);      g_hook      = nullptr; }
        if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    if (g_hook)      { UnhookWindowsHookEx(g_hook);      g_hook      = nullptr; }
    return 0;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Hotkey string parser ("Win+Tab", "Ctrl+Alt+F", "MButton", "0x47", ...).
// Shared contract with the Settings app's capture popup — keep the token
// names in sync with core/Settings/Services/HotkeyService.cs.
// ---------------------------------------------------------------------------
bool ParseHotkey(const std::wstring& text, HotkeySpec& out)
{
    out = HotkeySpec{};   // Win+Tab default

    struct NameVk { const wchar_t* name; unsigned vk; };
    static const NameVk kMainKeys[] = {
        { L"tab", VK_TAB }, { L"space", VK_SPACE },
        { L"enter", VK_RETURN }, { L"return", VK_RETURN },
        { L"esc", VK_ESCAPE }, { L"escape", VK_ESCAPE },
        { L"backspace", VK_BACK },
        { L"delete", VK_DELETE }, { L"del", VK_DELETE },
        { L"insert", VK_INSERT }, { L"ins", VK_INSERT },
        { L"home", VK_HOME }, { L"end", VK_END },
        { L"pageup", VK_PRIOR }, { L"pgup", VK_PRIOR },
        { L"pagedown", VK_NEXT }, { L"pgdn", VK_NEXT },
        { L"left", VK_LEFT }, { L"right", VK_RIGHT },
        { L"up", VK_UP }, { L"down", VK_DOWN },
        { L"capslock", VK_CAPITAL }, { L"numlock", VK_NUMLOCK },
        { L"scrolllock", VK_SCROLL },
        { L"printscreen", VK_SNAPSHOT }, { L"prtsc", VK_SNAPSHOT },
        { L"pause", VK_PAUSE }, { L"apps", VK_APPS },
        { L"plus", VK_OEM_PLUS }, { L"minus", VK_OEM_MINUS },
        { L"comma", VK_OEM_COMMA }, { L"period", VK_OEM_PERIOD },
        { L"semicolon", VK_OEM_1 }, { L"slash", VK_OEM_2 },
        { L"grave", VK_OEM_3 }, { L"tilde", VK_OEM_3 },
        { L"lbracket", VK_OEM_4 }, { L"backslash", VK_OEM_5 },
        { L"rbracket", VK_OEM_6 }, { L"quote", VK_OEM_7 },
        { L"numpad0", VK_NUMPAD0 }, { L"numpad1", VK_NUMPAD1 },
        { L"numpad2", VK_NUMPAD2 }, { L"numpad3", VK_NUMPAD3 },
        { L"numpad4", VK_NUMPAD4 }, { L"numpad5", VK_NUMPAD5 },
        { L"numpad6", VK_NUMPAD6 }, { L"numpad7", VK_NUMPAD7 },
        { L"numpad8", VK_NUMPAD8 }, { L"numpad9", VK_NUMPAD9 },
        { L"multiply", VK_MULTIPLY }, { L"add", VK_ADD },
        { L"subtract", VK_SUBTRACT }, { L"decimal", VK_DECIMAL },
        { L"divide", VK_DIVIDE },
    };
    static const NameVk kMouseKeys[] = {
        { L"lbutton", VK_LBUTTON }, { L"rbutton", VK_RBUTTON },
        { L"mbutton", VK_MBUTTON }, { L"middlebutton", VK_MBUTTON },
        { L"xbutton1", VK_XBUTTON1 }, { L"mouse4", VK_XBUTTON1 },
        { L"xbutton2", VK_XBUTTON2 }, { L"mouse5", VK_XBUTTON2 },
    };

    uint8_t  modMask = 0;
    unsigned mainVk = 0;
    bool     mainIsMouse = false;
    uint8_t  lastModBit = 0;   // for bare-modifier bindings ("Win")

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'+', start);
        if (end == std::wstring::npos) end = text.size();
        std::wstring tokRaw = text.substr(start, end - start);
        // trim
        size_t b = tokRaw.find_first_not_of(L" \t");
        size_t e = tokRaw.find_last_not_of(L" \t");
        std::wstring tok = (b == std::wstring::npos)
            ? std::wstring()
            : ToLower(tokRaw.substr(b, e - b + 1));

        if (!tok.empty()) {
            if (tok == L"ctrl" || tok == L"control") {
                modMask |= kModCtrl; lastModBit = kModCtrl;
            } else if (tok == L"shift") {
                modMask |= kModShift; lastModBit = kModShift;
            } else if (tok == L"alt") {
                modMask |= kModAlt; lastModBit = kModAlt;
            } else if (tok == L"win" || tok == L"windows" || tok == L"super"
                       || tok == L"meta") {
                modMask |= kModWin; lastModBit = kModWin;
            } else {
                unsigned vk = 0;
                bool isMouse = false;
                if (tok.size() == 1) {
                    wchar_t c = tok[0];
                    if (c >= L'a' && c <= L'z') vk = static_cast<unsigned>(c - L'a') + 'A';
                    else if (c >= L'0' && c <= L'9') vk = static_cast<unsigned>(c);
                } else if (tok.size() >= 2 && tok[0] == L'f'
                           && iswdigit(tok[1])) {
                    int fn = _wtoi(tok.c_str() + 1);
                    if (fn >= 1 && fn <= 24) vk = VK_F1 + (fn - 1);
                } else if (tok.size() > 2 && tok[0] == L'0' && tok[1] == L'x') {
                    unsigned long hv = wcstoul(tok.c_str() + 2, nullptr, 16);
                    if (hv > 0 && hv < 0xFF) vk = static_cast<unsigned>(hv);
                }
                if (vk == 0) {
                    for (const auto& nk : kMainKeys)
                        if (tok == nk.name) { vk = nk.vk; break; }
                }
                if (vk == 0) {
                    for (const auto& nk : kMouseKeys)
                        if (tok == nk.name) { vk = nk.vk; isMouse = true; break; }
                }
                if (vk == 0)
                    return false;   // unknown token → keep default
                if (mainVk != 0)
                    return false;   // two main keys → invalid
                mainVk = vk;
                mainIsMouse = isMouse;
            }
        }

        if (end == text.size()) break;
        start = end + 1;
    }

    // Bare-modifier binding ("Win"): exactly one modifier token, no main
    // key → that modifier becomes the toggle-mode main key.
    if (mainVk == 0) {
        if (modMask == lastModBit && lastModBit != 0) {
            switch (lastModBit) {
            case kModCtrl:  mainVk = VK_LCONTROL; break;
            case kModShift: mainVk = VK_LSHIFT;   break;
            case kModAlt:   mainVk = VK_LMENU;    break;
            case kModWin:   mainVk = VK_LWIN;     break;
            }
            modMask = 0;
        } else {
            return false;   // no main key
        }
    }

    out.modMask     = modMask;
    out.mainVk      = mainVk;
    out.mainIsMouse = mainIsMouse;
    return true;
}

// ---------------------------------------------------------------------------
bool Install(HWND hwndNotify, const Messages& msgs)
{
    if (g_hookThread)
        return false;

    g_hwndNotify   = hwndNotify;
    g_msg          = msgs;
    g_mouseDragging = false;
    g_mouseDragResidue = 0.0f;
    DropSession();
    g_suppressNextModRelease = false;
    g_wheelAccum      = 0;
    g_lastCyclePostMs = 0;
    g_lastCycleDir    = 0;
    g_hookInstallOk.store(false, std::memory_order_relaxed);

    g_hookReadyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hookReadyEvent)
        return false;

    g_hookThread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, &g_hookThreadId);
    if (!g_hookThread) {
        CloseHandle(g_hookReadyEvent);
        g_hookReadyEvent = nullptr;
        return false;
    }

    // Wait for the hook thread to either install the hooks or fail.
    WaitForSingleObject(g_hookReadyEvent, INFINITE);
    CloseHandle(g_hookReadyEvent);
    g_hookReadyEvent = nullptr;

    if (!g_hookInstallOk.load(std::memory_order_acquire)) {
        WaitForSingleObject(g_hookThread, 5000);
        CloseHandle(g_hookThread);
        g_hookThread   = nullptr;
        g_hookThreadId = 0;
        return false;
    }

    return true;
}

void Uninstall()
{
    bool joined = true;
    if (g_hookThread) {
        // Tell the hook thread to leave its message loop and unhook.
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        joined = (WaitForSingleObject(g_hookThread, 5000) == WAIT_OBJECT_0);
        CloseHandle(g_hookThread);
        g_hookThread   = nullptr;
        g_hookThreadId = 0;
    }

    // If the join timed out the hook thread may still be running its
    // callbacks — leave the (non-atomic) hook-thread state untouched.
    if (!joined)
        return;

    g_hwndNotify  = nullptr;
    DropSession();
    g_suppressNextModRelease = false;
    g_wheelAccum      = 0;
    g_lastCyclePostMs = 0;
    g_lastCycleDir    = 0;
}

// ---------------------------------------------------------------------------
// Shared session state (see the header).  Only the atomic flag and the
// wheel accumulator are touched — the hook-thread-private swallow flags
// belong to the keyboard path and stay untouched, which is exactly right:
// a gesture never presses a modifier, so there is no release to defuse.
// ---------------------------------------------------------------------------
bool IsSessionActive()
{
    return SessionRunning();
}

uint64_t CurrentSessionEpoch()
{
    return SessionEpochOf(g_session.load(std::memory_order_relaxed));
}

bool EndSessionIfEpoch(uint64_t epoch)
{
    uint64_t cur = g_session.load(std::memory_order_relaxed);
    for (;;) {
        if (SessionEpochOf(cur) != epoch)
            return false;          // a NEWER session owns the flag — hands off
        if (!SessionBitOf(cur)) {
            // Ours, and already ended through one of the keyboard paths.  The
            // per-session reset still belongs here, because that is what the
            // unconditional store this replaced always did.
            BeginSessionState();
            return true;
        }
        if (g_session.compare_exchange_weak(cur, epoch << 1,
                                            std::memory_order_relaxed)) {
            BeginSessionState();
            return true;
        }
    }
}

void SetSessionActive(bool active)
{
    if (active) RaiseSession();
    else        DropSession();
    BeginSessionState();
}

void AssertInputOwnership()
{
    // The keyup half alone would do, but a matched pair keeps the injected
    // sequence well-formed for anything else watching the input stream.
    INPUT inputs[2] = {};
    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = kVkDummy;
    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = kVkDummy;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void EndSessionForeign()
{
    uint8_t  modMask     = 0;
    unsigned mainVk      = 0;
    bool     mainIsMouse = false;
    UnpackHotkeySpec(g_hkSpec.load(std::memory_order_relaxed),
                     modMask, mainVk, mainIsMouse);

    // Arm the defusal BEFORE dropping the session flag: the release could
    // arrive on the hook thread between the two, and it must find the flag
    // already set or it will reach the OS.
    g_suppressNextModRelease.store(
        (modMask != 0) && AnyComboModDown(modMask), std::memory_order_relaxed);
    DropSession();
    BeginSessionState();
}

void ResumeSession()
{
    // Deliberately NOT BeginSessionState(): this re-arms a session the hook
    // dropped a moment ago and that the controller decided is still running
    // (the cancel key clearing a search query rather than closing).  Wiping
    // the search latch here would hand the still-held modifier back its
    // commit-on-release, and the very next key-up would close the cascade the
    // user just chose to keep.
    //
    // fetch_or, NOT RaiseSession(): the epoch must not advance.  This is the
    // same session resuming, and minting a fresh identity here would make the
    // teardown that eventually runs for it fail to recognise its own session.
    g_session.fetch_or(1u, std::memory_order_relaxed);
}

bool ShouldIgnoreActivation()
{
    return ShouldIgnoreActivationImpl();
}

bool PointerOwnsLeftClick()
{
    return g_optPointerInCascade.load(std::memory_order_relaxed)
        && g_optMouseSelect.load(std::memory_order_relaxed)
        && g_optSelectButton.load(std::memory_order_relaxed) == kMouseLeft;
}

void SuspendActivation(unsigned ms)
{
    g_suspendUntilMs.store(ms == 0 ? 0 : GetTickCount64() + ms,
                           std::memory_order_relaxed);
}

void AbortSessionIfIdle(uint64_t epoch)
{
    uint8_t  modMask     = 0;
    unsigned mainVk      = 0;
    bool     mainIsMouse = false;
    UnpackHotkeySpec(g_hkSpec.load(std::memory_order_relaxed),
                     modMask, mainVk, mainIsMouse);

    // Held trigger → the classic Win+Tab path owns the teardown; leave it be
    // so the release still commits and stays swallowed.
    if (modMask != 0 && AnyComboModDown(modMask))
        return;
    if (!mainIsMouse && mainVk != 0
        && (GetAsyncKeyState(static_cast<int>(mainVk)) & 0x8000))
        return;

    // By identity, for the same reason the teardown is (see EndSessionIfEpoch).
    // The window scan this follows is slow — enumeration plus an OpenProcess
    // per program — so a second trigger can easily raise a NEW session while
    // the failed one is still being given up, and dropping the flag blindly
    // would disarm the hook for a cascade that is about to open.
    //
    // The two guards above happen to cover a held key, but they cover NOTHING
    // for a mouse-button binding: modMask is 0 and mainIsMouse skips the second
    // test, so that configuration reached the bare drop every time.
    EndSessionIfEpoch(epoch);
}

// ---------------------------------------------------------------------------
void SetOptions(const TriggerOptions& opts)
{
    g_optIgnoreFullscreen.store(opts.ignoreFullscreen, std::memory_order_relaxed);
    g_optWheelCycle.store(opts.mouseWheelCycle, std::memory_order_relaxed);
    g_optKeyboardNav.store(opts.keyboardNav, std::memory_order_relaxed);
    g_optToggleMode.store(opts.hotkeyToggleMode, std::memory_order_relaxed);
    g_optTouchpadNav.store(opts.touchpadNav, std::memory_order_relaxed);
    g_optWindowSnap.store(opts.windowSnap, std::memory_order_relaxed);
    g_optPointerInCascade.store(opts.pointerInCascade, std::memory_order_relaxed);
    g_optMouseSelect.store(opts.mouseSelect, std::memory_order_relaxed);
    g_optSelectButton.store(std::clamp(opts.selectButton, 0, 5),
                            std::memory_order_relaxed);
    g_optDragEnabled.store(opts.dragEnabled, std::memory_order_relaxed);
    g_optDragButton.store(std::clamp(opts.dragButton, 0, 5),
                          std::memory_order_relaxed);
    g_optCloseFromCascade.store(opts.closeFromCascade, std::memory_order_relaxed);
    g_optCloseButton.store(std::clamp(opts.closeButton, 0, 5),
                           std::memory_order_relaxed);
    g_optCloseKeyEnabled.store(opts.closeKeyEnabled, std::memory_order_relaxed);
    g_optSearchEnabled.store(opts.searchEnabled, std::memory_order_relaxed);

    HotkeySpec spec;
    if (!ParseHotkey(opts.activationHotkey, spec))   // falls back to Win+Tab
        Diag::Report(Diag::Code::HotkeyUnparsable, Diag::Sev::Warning,
                     L"The activation hotkey could not be understood",
                     (L"\"" + opts.activationHotkey
                      + L"\" is not a combination CKFlip3D can bind; "
                        L"Win+Tab is in use instead").c_str());
    g_hkSpec.store(PackHotkeySpec(spec.modMask, spec.mainVk, spec.mainIsMouse),
                   std::memory_order_relaxed);

    // Commit / cancel: the MAIN key only.  A mouse-button commit would fight
    // the in-cascade pointer bindings for the same click, and a modifier-
    // qualified one would need the activation combination released first —
    // both fail closed here, back onto the defaults that always work.
    {
        auto fellBack = [](const wchar_t* role, const std::wstring& asked,
                           const wchar_t* used) {
            // _TRUNCATE, not swprintf_s: `asked` comes straight out of
            // config.json and can be any length at all, and the secure form
            // would treat that as a programming error and kill the process.
            wchar_t detail[256];
            _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                       L"\"%s\" cannot be a %s key — it must be a single "
                       L"keyboard key with no modifiers; %s is in use instead",
                       asked.c_str(), role, used);
            Diag::Report(Diag::Code::BindingUnparsable, Diag::Sev::Warning,
                         L"A key binding could not be understood", detail);
        };

        HotkeySpec commit;
        if (!ParseHotkey(opts.commitHotkey, commit) || commit.mainIsMouse
            || commit.mainVk == 0) {
            commit.mainVk = VK_RETURN;
            fellBack(L"commit", opts.commitHotkey, L"Enter");
        }
        g_optCommitVk.store(commit.mainVk, std::memory_order_relaxed);

        HotkeySpec cancel;
        if (!ParseHotkey(opts.cancelHotkey, cancel) || cancel.mainIsMouse
            || cancel.mainVk == 0) {
            cancel.mainVk = VK_ESCAPE;
            fellBack(L"cancel", opts.cancelHotkey, L"Escape");
        }
        g_optCancelVk.store(cancel.mainVk, std::memory_order_relaxed);

        HotkeySpec close;
        if (!ParseHotkey(opts.closeHotkey, close) || close.mainIsMouse
            || close.mainVk == 0) {
            close.mainVk = VK_DELETE;
            fellBack(L"close-window", opts.closeHotkey, L"Delete");
        }
        g_optCloseVk.store(close.mainVk, std::memory_order_relaxed);
    }

    // Two pointer bindings on one button is resolved in a fixed order, so the
    // second one silently never happens.  The Settings page warns before
    // saving it; this catches a config that was written by hand or by an
    // older build.
    if (opts.pointerInCascade) {
        const int select = opts.mouseSelect      ? opts.selectButton : 0;
        const int close  = opts.closeFromCascade ? opts.closeButton  : 0;
        const int drag   = opts.dragEnabled && !opts.windowSnap ? opts.dragButton : 0;
        const bool clash = (select && select == close)
                        || (select && select == drag)
                        || (close  && close  == drag);
        if (clash)
            Diag::Report(Diag::Code::BindingCollision, Diag::Sev::Warning,
                         L"Two mouse actions in the cascade are on the same button",
                         L"picking wins over closing, and closing over dragging, "
                         L"so the others will never happen (Controls → Mouse & "
                         L"keyboard)");
    }

    std::vector<std::wstring> lowered;
    lowered.reserve(opts.ignoredApps.size());
    for (const auto& entry : opts.ignoredApps)
        if (!entry.empty())
            lowered.push_back(ToLower(entry));

    AcquireSRWLockExclusive(&g_optLock);
    g_optIgnoredApps = std::move(lowered);
    g_optHasIgnoredApps.store(!g_optIgnoredApps.empty(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_optLock);
}

} // namespace KeyboardHook
