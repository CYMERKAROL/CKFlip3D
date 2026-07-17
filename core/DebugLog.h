#pragma once
// ---------------------------------------------------------------------------
// Runtime-gated OutputDebugString wrapper.
//
// Release builds used to emit an unconditional debug-stream firehose; these
// logs are now gated on the existing `showDebugInfo` config key (forced on
// in --safe-mode).  Defaults to ENABLED so any logging that happens before
// the config is loaded behaves exactly as before — App::Run / ReloadConfig
// sync the flag to cfg.showDebugInfo right after each config read.
// ---------------------------------------------------------------------------
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
