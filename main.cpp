#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "core/app.h"

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int /*nCmdShow*/)
{
    // Single-instance guard.  Nothing is recorded when it fires: the second
    // copy closing itself IS the guard working, and a log entry for it would
    // be the program reporting its own correct behaviour.
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"CKFlip3D_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    App app;
    int result = app.Run(hInstance);

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }
    return result;
}
