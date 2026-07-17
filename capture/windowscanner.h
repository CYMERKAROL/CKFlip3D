#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <vector>
#include <string>

struct WindowInfo {
    HWND         hwnd;
    std::wstring title;
    RECT         rect;      // Window rect in screen coordinates
    bool         minimized; // Was minimized at scan time
};

namespace WindowScanner {

/// Enumerate all alt-tab-eligible application windows (including minimized).
/// Excludes our own windows (identified by ownerPid).
std::vector<WindowInfo> Enumerate(DWORD ownerPid);

/// Find the desktop background window (Progman or WorkerW).
HWND FindDesktopWindow();

} // namespace WindowScanner
