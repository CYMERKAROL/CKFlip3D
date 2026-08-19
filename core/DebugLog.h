// ---------------------------------------------------------------------------
// Runtime-gated OutputDebugString wrapper.  Logging is tied to the
// `showDebugInfo` config key (forced on in --safe-mode) so a release build
// does not spray the debug stream.  It starts ENABLED, because anything that
// logs before the config is read would otherwise be lost: App::Run and
// ReloadConfig sync the flag right after each config read.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <atomic>

namespace CKLog {

inline std::atomic<bool> g_enabled{ true };

inline void Log(const wchar_t* msg)
{
    if (g_enabled.load(std::memory_order_relaxed))
        OutputDebugStringW(msg);
}

inline void Log(const char* msg)
{
    if (g_enabled.load(std::memory_order_relaxed))
        OutputDebugStringA(msg);
}

} // namespace CKLog
