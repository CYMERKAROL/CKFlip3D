#include "keyboardhook.h"

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
// only on the hook thread.  g_sessionActive is atomic so Install/Uninstall
// on the app thread can reset it safely.
// ---------------------------------------------------------------------------

HHOOK    g_hook          = nullptr;
HHOOK    g_mouseHook     = nullptr;
HWND     g_hwndNotify    = nullptr;
UINT     g_msgActivate   = 0;
UINT     g_msgCycle      = 0;
UINT     g_msgCycleBack  = 0;
UINT     g_msgDismiss    = 0;
UINT     g_msgEscape     = 0;
UINT     g_msgCycleStop  = 0;

std::atomic<bool>   g_sessionActive{false};   // switcher session in flight

// When the session is committed early (Enter) while combo modifiers are
// still physically held, the next release of a Win/Alt combo modifier must
// still be swallowed (dummy-key trick) so the Start menu / window menu bar
// doesn't pop.  Hook-thread only.
bool g_suppressNextModRelease = false;

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
int32_t   g_wheelAccum      = 0;
ULONGLONG g_lastCyclePostMs = 0;
int       g_lastCycleDir    = 0;   // +1 = cycled forward, -1 = cycled back
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
std::atomic<bool>  g_optHasIgnoredApps{false};

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
bool ShouldIgnoreActivation()
{
    if (g_optIgnoreFullscreen.load(std::memory_order_relaxed)
        && ForegroundIsFullscreen())
        return true;
    return ForegroundIsIgnoredApp();
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
    const bool active = g_sessionActive.load(std::memory_order_relaxed);

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
                    if (ShouldIgnoreActivation())
                        return CallNextHookEx(g_hook, nCode, wParam, lParam);
                    g_sessionActive.store(true, std::memory_order_relaxed);
                    ResetWheelState();
                    PostMessage(g_hwndNotify, g_msgActivate, 0, 0);
                } else {
                    PostMessage(g_hwndNotify, g_msgCycle, 0, 0);
                }
                g_bareMainConsumed = true;
                return 1;
            }
            if (isUp && (g_bareMainConsumed
                         || g_sessionActive.load(std::memory_order_relaxed))) {
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
            if (active && g_optToggleMode.load(std::memory_order_relaxed)) {
                if (needSwallow) {
                    SwallowModifierRelease(kb->vkCode);
                    return 1;
                }
            } else if (active) {
                g_sessionActive.store(false, std::memory_order_relaxed);
                g_suppressNextModRelease = false;
                ResetWheelState();
                PostMessage(g_hwndNotify, g_msgDismiss, 0, 0);
                if (needSwallow) {
                    SwallowModifierRelease(kb->vkCode);
                    return 1;
                }
            } else if (g_suppressNextModRelease) {
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
        const bool cycleWithoutMods = active
            && g_optToggleMode.load(std::memory_order_relaxed);
        if (isDown && (ModsSatisfied(modMask) || cycleWithoutMods)) {
            bool shiftHeld = !(modMask & kModShift)
                          && ((GetAsyncKeyState(VK_LSHIFT) & 0x8000)
                           || (GetAsyncKeyState(VK_RSHIFT) & 0x8000));
            if (!active) {
                // Session not started yet — honour the trigger filters
                // (fullscreen apps / ignore list).  Pass the key through so
                // the OS handles the combo normally instead of blocking it.
                if (ShouldIgnoreActivation())
                    return CallNextHookEx(g_hook, nCode, wParam, lParam);
                g_sessionActive.store(true, std::memory_order_relaxed);
                g_suppressNextModRelease = false;
                ResetWheelState();
                PostMessage(g_hwndNotify, g_msgActivate, 0, 0);
            } else {
                PostMessage(g_hwndNotify,
                            shiftHeld ? g_msgCycleBack : g_msgCycle, 0, 0);
            }
            return 1;
        }
        if (isUp && active) {
            // Main key released while session active — tell controller to
            // stop queuing cycles so continuous scroll doesn't over-shoot.
            PostMessage(g_hwndNotify, g_msgCycleStop, 0, 0);
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
                PostMessage(g_hwndNotify, g_msgCycle, 0, 0);
            else if (isUp)
                PostMessage(g_hwndNotify, g_msgCycleStop, 0, 0);
            return 1;
        }
        if (arrowsOn && (kb->vkCode == VK_UP || kb->vkCode == VK_LEFT)) {
            if (isDown)
                PostMessage(g_hwndNotify, g_msgCycleBack, 0, 0);
            else if (isUp)
                PostMessage(g_hwndNotify, g_msgCycleStop, 0, 0);
            return 1;
        }

        // Enter = commit the current selection.  Required for bindings with
        // no hold modifier (bare key / mouse button); a convenience for the
        // classic combos.  If combo modifiers are still held, their release
        // is suppressed once so the Start menu doesn't pop afterwards.
        if (isDown && kb->vkCode == VK_RETURN) {
            g_sessionActive.store(false, std::memory_order_relaxed);
            // Suppress the upcoming combo-modifier release only while one
            // is actually still held — in toggle mode the modifiers are
            // usually up long before commit, and a stale flag would eat
            // the next unrelated Win/Alt release.
            g_suppressNextModRelease = (modMask != 0) && AnyComboModDown(modMask);
            ResetWheelState();
            PostMessage(g_hwndNotify, g_msgDismiss, 0, 0);
            return 1;
        }

        // Escape = cancel
        if (isDown && kb->vkCode == VK_ESCAPE) {
            g_sessionActive.store(false, std::memory_order_relaxed);
            g_suppressNextModRelease = (modMask != 0) && AnyComboModDown(modMask);
            ResetWheelState();
            PostMessage(g_hwndNotify, g_msgEscape, 0, 0);
            return 1;
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
    const bool active = g_sessionActive.load(std::memory_order_relaxed);

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
                const bool cycleWithoutMods = active
                    && g_optToggleMode.load(std::memory_order_relaxed);
                if (ModsSatisfied(modMask) || cycleWithoutMods) {
                    if (!active) {
                        if (ShouldIgnoreActivation())
                            return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
                        g_sessionActive.store(true, std::memory_order_relaxed);
                        g_suppressNextModRelease = false;
                        ResetWheelState();
                        PostMessage(g_hwndNotify, g_msgActivate, 0, 0);
                    } else {
                        bool shiftHeld = !(modMask & kModShift)
                                      && ((GetAsyncKeyState(VK_LSHIFT) & 0x8000)
                                       || (GetAsyncKeyState(VK_RSHIFT) & 0x8000));
                        PostMessage(g_hwndNotify,
                                    shiftHeld ? g_msgCycleBack : g_msgCycle, 0, 0);
                    }
                    return 1;
                }
                if (active)
                    return 1;   // swallow strays while the cascade is open
            } else {
                if (active) {
                    PostMessage(g_hwndNotify, g_msgCycleStop, 0, 0);
                    return 1;
                }
            }
        }
    }

    if (!active)
        return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);

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
            PostMessage(g_hwndNotify, g_msgCycleBack, 0, 0);
            g_wheelAccum     -= WHEEL_DELTA;
            g_lastCyclePostMs = now;
            g_lastCycleDir    = +1;
        }
        while (g_wheelAccum <= -WHEEL_DELTA) {
            PostMessage(g_hwndNotify, g_msgCycle, 0, 0);
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

    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hMod, 0);

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
bool Install(HWND hwndNotify,
             UINT msgActivate,
             UINT msgCycle,
             UINT msgCycleBack,
             UINT msgDismiss,
             UINT msgEscape,
             UINT msgCycleStop)
{
    if (g_hookThread)
        return false;

    g_hwndNotify   = hwndNotify;
    g_msgActivate  = msgActivate;
    g_msgCycle     = msgCycle;
    g_msgCycleBack = msgCycleBack;
    g_msgDismiss   = msgDismiss;
    g_msgEscape    = msgEscape;
    g_msgCycleStop = msgCycleStop;
    g_sessionActive.store(false, std::memory_order_relaxed);
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
    g_sessionActive.store(false, std::memory_order_relaxed);
    g_suppressNextModRelease = false;
    g_wheelAccum      = 0;
    g_lastCyclePostMs = 0;
    g_lastCycleDir    = 0;
}

void SetOptions(const TriggerOptions& opts)
{
    g_optIgnoreFullscreen.store(opts.ignoreFullscreen, std::memory_order_relaxed);
    g_optWheelCycle.store(opts.mouseWheelCycle, std::memory_order_relaxed);
    g_optKeyboardNav.store(opts.keyboardNav, std::memory_order_relaxed);
    g_optToggleMode.store(opts.hotkeyToggleMode, std::memory_order_relaxed);

    HotkeySpec spec;
    ParseHotkey(opts.activationHotkey, spec);   // falls back to Win+Tab
    g_hkSpec.store(PackHotkeySpec(spec.modMask, spec.mainVk, spec.mainIsMouse),
                   std::memory_order_relaxed);

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
