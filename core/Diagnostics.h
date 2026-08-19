// ---------------------------------------------------------------------------
// The record of everything that went wrong, kept where the user can read it.
//
// CKFlip3D spends its life behind an overlay, inside a keyboard hook, on a
// device it does not own.  When something fails there, the failure has
// historically been invisible: an OutputDebugString nobody was watching, a
// silent `return false`, or a modal box at logon that the user dismissed and
// could never see again.  Finding out what happened meant attaching a debugger
// to a program whose whole job is to take over the screen.
//
// Every such failure now becomes an ENTRY: a stable code, a severity, a line
// the user can read and a line the author can act on.  Entries are appended to
// %APPDATA%\CKFlip3D\diagnostics.jsonl, one JSON object per line, and the
// Settings app shows them.  The file is the only contract between the two —
// the core never waits on the Settings app, and the Settings app never needs
// the core to be running.
//
// Appending is deliberately cheap and deliberately unreliable in the right
// direction: a failure to write a diagnostic never becomes a failure of its
// own.  Nothing here throws, nothing here blocks, and every call is safe from
// any thread, including the low-level hook thread and the crash filter.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>

namespace Diag {

enum class Sev : int {
    Info     = 0,   // worth knowing, nothing is wrong
    Warning  = 1,   // degraded: a feature is off, slower, or approximated
    Critical = 2,   // the thing the user asked for did not happen
};

/// Arm the session: adopt the crash filter, notice whether the PREVIOUS
/// session ended without saying goodbye, and answer the environment questions
/// that only need asking once.  Call once, as early as possible.
void BeginSession();

/// Release the session cleanly.  Its absence is what BeginSession detects next
/// time, so every exit path has to reach it — see the guard below.
void EndSession();

/// Record what the process is currently DOING, in the session marker.
///
/// CK0001 is written by the NEXT process from whatever the marker still says,
/// so the marker is the only channel a death that reached no handler can speak
/// through.  "Ended without shutting down" on its own leaves nothing to act on;
/// "ended without shutting down, and it was in the middle of a cascade opened
/// eleven seconds earlier" is a place to look.  Called on state changes only
/// (cascade opened, cascade closed) — never per frame.
///
/// `state` is a short present-tense phrase: "cascade open", "idle in the tray".
void NoteState(const wchar_t* state);

/// Put the session marker back after EndSession has been called early.
///
/// Windows shutting down is not a crash, but it looks exactly like one from
/// here: the process is terminated where it stands and no destructor ever
/// runs.  The shutdown messages are therefore the LAST chance to close the
/// session, and they arrive before it is certain the shutdown will go ahead —
/// another program can still veto it.  So the marker is released when the
/// shutdown is announced and re-armed if it turns out to be cancelled.
void ArmSession();

/// RAII form of the pair, so an early `return` cannot leave the marker behind
/// and report a crash that never happened.
struct SessionGuard {
    SessionGuard()  { BeginSession(); }
    ~SessionGuard() { EndSession(); }
    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;
};

/// Record one entry.  `detail` is for the author: the HRESULT, the window
/// class, the number that was out of range.  `message` is for the user.
///
/// `sticky` marks an entry that must not come back once the user has cleared
/// it — a statement about the machine rather than about something that just
/// happened, which would otherwise reappear on every single launch.
void Report(const wchar_t* code, Sev sev, const wchar_t* message,
            const wchar_t* detail = nullptr, bool sticky = false);

/// Report at most once per process for this code.  For failures that sit
/// inside a per-window or per-frame path, where the second thousand copies say
/// nothing the first did not.
void ReportOnce(const wchar_t* code, Sev sev, const wchar_t* message,
                const wchar_t* detail = nullptr, bool sticky = false);

/// Report with an HRESULT appended to the detail, decoded where Windows has a
/// message for it.
void ReportHr(const wchar_t* code, Sev sev, const wchar_t* message,
              HRESULT hr, const wchar_t* detail = nullptr);

/// Report with GetLastError() appended, decoded the same way.  Call it before
/// anything else can overwrite the thread's error.
void ReportLastError(const wchar_t* code, Sev sev, const wchar_t* message,
                     const wchar_t* detail = nullptr);

/// %APPDATA%\CKFlip3D\diagnostics.jsonl.
std::wstring LogPath();

// ---------------------------------------------------------------------------
// Codes.  Stable identifiers — a code that has been shipped keeps its meaning
// for good, and a retired one is not reused for something else.
// ---------------------------------------------------------------------------
namespace Code {

// --- Session and environment ---
inline constexpr const wchar_t* SessionUnexpectedEnd   = L"CK0001";
inline constexpr const wchar_t* SessionCrash           = L"CK0002";
inline constexpr const wchar_t* OsUnsupported          = L"CK0003";
inline constexpr const wchar_t* SafeModeActive         = L"CK0004";
inline constexpr const wchar_t* AlreadyRunning         = L"CK0005";
inline constexpr const wchar_t* NotElevated            = L"CK0006";
inline constexpr const wchar_t* SessionRestart         = L"CK0007";
inline constexpr const wchar_t* ProfileNotWritable     = L"CK0008";
inline constexpr const wchar_t* MessageWindowFailed    = L"CK0009";
inline constexpr const wchar_t* TrayIconFailed         = L"CK0010";
inline constexpr const wchar_t* SettingsExeMissing     = L"CK0011";
inline constexpr const wchar_t* SettingsLaunchFailed   = L"CK0012";

// --- Configuration ---
inline constexpr const wchar_t* ConfigUnreadable       = L"CK0101";
inline constexpr const wchar_t* ConfigValueClamped     = L"CK0102";
inline constexpr const wchar_t* HotkeyUnparsable       = L"CK0103";
inline constexpr const wchar_t* BindingUnparsable      = L"CK0104";
inline constexpr const wchar_t* ConfigSaveFailed       = L"CK0106";
inline constexpr const wchar_t* ConfigConflict         = L"CK0107";

// --- Graphics ---
inline constexpr const wchar_t* GpuDeviceFailed        = L"CK0201";
inline constexpr const wchar_t* GpuSoftwareFallback    = L"CK0202";
inline constexpr const wchar_t* GpuLowVideoMemory      = L"CK0203";
inline constexpr const wchar_t* SwapChainFailed        = L"CK0204";
inline constexpr const wchar_t* CompositionFailed      = L"CK0205";
inline constexpr const wchar_t* ShaderCompileFailed    = L"CK0206";
inline constexpr const wchar_t* OverlayWindowFailed    = L"CK0207";
inline constexpr const wchar_t* DeviceRemoved          = L"CK0208";
inline constexpr const wchar_t* BackbufferResizeFailed = L"CK0209";
inline constexpr const wchar_t* PresentFailed          = L"CK0210";
inline constexpr const wchar_t* DxgiFactoryFailed      = L"CK0211";
inline constexpr const wchar_t* MultithreadUnavailable = L"CK0212";

// --- Capture ---
inline constexpr const wchar_t* CaptureUnsupported     = L"CK0301";
inline constexpr const wchar_t* CaptureStartFailed     = L"CK0302";
inline constexpr const wchar_t* CaptureItemFailed      = L"CK0303";
inline constexpr const wchar_t* TaskbarCaptureOdd      = L"CK0304";
inline constexpr const wchar_t* DesktopCaptureFailed   = L"CK0305";
inline constexpr const wchar_t* CaptureTextureFailed   = L"CK0306";
inline constexpr const wchar_t* CaptureWarmupTimeout   = L"CK0307";
inline constexpr const wchar_t* ThumbnailFallbackFail  = L"CK0308";
inline constexpr const wchar_t* CaptureFrameStalled    = L"CK0309";

// --- Input ---
inline constexpr const wchar_t* KeyboardHookFailed     = L"CK0401";
inline constexpr const wchar_t* MouseHookFailed        = L"CK0402";
inline constexpr const wchar_t* RawInputFailed         = L"CK0403";
inline constexpr const wchar_t* HotkeyReserved         = L"CK0404";
inline constexpr const wchar_t* TouchpadMissing        = L"CK0405";
inline constexpr const wchar_t* HookEvicted            = L"CK0406";
inline constexpr const wchar_t* ActivationSuppressed   = L"CK0407";
inline constexpr const wchar_t* BindingCollision       = L"CK0408";

// --- Window session ---
inline constexpr const wchar_t* NoEligibleWindows      = L"CK0501";
inline constexpr const wchar_t* WindowScanIncomplete   = L"CK0502";
inline constexpr const wchar_t* CloakFailed            = L"CK0503";
inline constexpr const wchar_t* UncloakFailed          = L"CK0504";
inline constexpr const wchar_t* ForegroundRestoreFail  = L"CK0505";
inline constexpr const wchar_t* TaskbarStateFailed     = L"CK0506";
inline constexpr const wchar_t* CloseRequestIgnored    = L"CK0508";
inline constexpr const wchar_t* MonitorLayoutOdd       = L"CK0509";
inline constexpr const wchar_t* SessionEndedForeign    = L"CK0510";

// --- Performance ---
inline constexpr const wchar_t* FrameBudgetMissed      = L"CK0701";
inline constexpr const wchar_t* QualityLowered         = L"CK0702";
inline constexpr const wchar_t* ActivationSlow         = L"CK0703";

} // namespace Code

} // namespace Diag
