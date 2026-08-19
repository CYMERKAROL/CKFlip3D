// ---------------------------------------------------------------------------
// CKFlip3D.Launch.exe — the target of the launch shortcut (General → Startup →
// "Create launch shortcut").
//
// It exists so that OPENING THE CASCADE and STARTING THE PROGRAM stay two
// different things.  CKFlip3D.exe is the switcher itself: it requires
// administrator (the low-level hooks do), it lives in the tray, and running it
// has never opened anything — a shortcut to it would raise a UAC prompt on
// every single use and, once elevated, would just be a second copy shutting
// itself down again.  So the shortcut points here instead, at an ordinary
// unelevated process that does one of exactly two things and then exits:
//
//   - the switcher is running → ask it to show the cascade and leave.  The
//     request travels as a registered window message the core allows through
//     its UIPI filter; nothing else in the program sends it, so nothing else
//     can open the cascade by accident.
//   - the switcher is NOT running → start it, which is where the UAC prompt
//     belongs.  The cascade deliberately does NOT follow: the click asked for
//     something that was not there yet, and a switcher flinging itself over the
//     screen as its first act after an elevation prompt is not what anybody
//     meant by "start it".  The next click opens the cascade.
//
// No dependency on the rest of the tree: it is one translation unit so that the
// thing standing between a click and the cascade stays a few milliseconds.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>
#include <strsafe.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

// Kept in step by hand with core/app.cpp (kWindowClass and the registered
// message it allows through ChangeWindowMessageFilterEx).
static constexpr const wchar_t* kCoreWindowClass = L"CKFlip3D_MessageWindow";
static constexpr const wchar_t* kShowCascadeMsg  = L"CKFLIP3D_SHOW_CASCADE";
static constexpr const wchar_t* kCoreExeName     = L"CKFlip3D.exe";

/// Full path of a file sitting next to this executable.  Derived from the
/// module path rather than the working directory, which a shortcut is free to
/// point anywhere.
static bool SiblingPath(const wchar_t* fileName, wchar_t* out, size_t cch)
{
    DWORD len = GetModuleFileNameW(nullptr, out, static_cast<DWORD>(cch));
    if (len == 0 || len >= cch)
        return false;

    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash)
        return false;
    slash[1] = L'\0';

    return SUCCEEDED(StringCchCatW(out, cch, fileName));
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // ---- The switcher is already running --------------------------------
    // Same question the Settings app asks, and for the same reason: finding a
    // window belonging to a higher-integrity process is allowed (UIPI filters
    // MESSAGES to it, which is what the core's filter opens for this one).
    HWND core = FindWindowW(kCoreWindowClass, nullptr);
    if (core) {
        UINT msg = RegisterWindowMessageW(kShowCascadeMsg);
        if (msg != 0)
            PostMessageW(core, msg, 0, 0);
        return 0;
    }

    // ---- It is not: start it, and stop there ----------------------------
    wchar_t exePath[MAX_PATH] = {};
    if (!SiblingPath(kCoreExeName, exePath, MAX_PATH)
        || GetFileAttributesW(exePath) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr,
                    L"CKFlip3D.exe was not found next to this shortcut's target.\n\n"
                    L"Reinstall CKFlip3D, or recreate the shortcut from\n"
                    L"Settings → General → Startup.",
                    L"CKFlip3D — not found", MB_OK | MB_ICONWARNING);
        return 1;
    }

    // The core's manifest requires administrator, so this is what raises the
    // prompt.  A declined prompt is simply the user saying no — nothing to
    // report and nothing to repair, so the return value is not inspected.
    ShellExecuteW(nullptr, L"open", exePath, nullptr, nullptr, SW_SHOWNORMAL);
    return 0;
}
