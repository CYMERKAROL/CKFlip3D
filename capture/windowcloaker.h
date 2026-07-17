#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dwmapi.h>
#include <vector>

/// Hides source windows visually (DWM cloaking) while the switcher is active,
/// then restores them on dismiss.
///
/// Strategy:
///   - Primary: DWMWA_CLOAK (value 13, undocumented but stable since Win 8).
///     This fully removes the window from DWM composition while keeping its
///     thumbnail redirection surface alive.
///   - Fallback: if cloaking fails (e.g. non-elevated on some builds),
///     we skip that window — no partial state.
///
/// All cloaked HWNDs are tracked and uncloaked on CloakAll/UncloakAll or
/// destructor, so teardown is always safe.
namespace WindowCloaker {

/// Enumerate ALL top-level app windows and cloak them.
/// Uses strict criteria: WS_VISIBLE, no WS_EX_TOOLWINDOW, no owner,
/// not system-cloaked, not a shell/system class.
/// Skips HWNDs in the `exclude` set (our overlay, desktop surface, etc.)
/// and our own process (identified by `myPid`).
/// Returns how many were cloaked.
int CloakVisibleAppWindows(DWORD myPid, const std::vector<HWND>& exclude);

/// Uncloak all previously cloaked windows.  Retries to ensure none are left.
void UncloakAll();

/// Force-uncloak every visible window on the system.  Nuclear option for
/// crash recovery — ensures nothing stays permanently hidden.
void ForceUncloakEverything();

} // namespace WindowCloaker
