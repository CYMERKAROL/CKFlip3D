// ---------------------------------------------------------------------------
// Installing the hooks and routing what they post.  HandleMessage runs on the
// message loop, well away from the hook callbacks, so the work a trigger sets
// off never happens inside the low-level hook's time budget.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "hotkeymanager.h"
#include "keyboardhook.h"
#include "touchpadhook.h"

HotkeyManager::~HotkeyManager()
{
    Shutdown();
}

KeyboardHook::Messages HotkeyManager::HookMessages()
{
    KeyboardHook::Messages m;
    m.activate      = WM_FLIP_ACTIVATE;
    m.cycle         = WM_FLIP_CYCLE;
    m.cycleBack     = WM_FLIP_CYCLE_BACK;
    m.dismiss       = WM_FLIP_DISMISS;
    m.escape        = WM_FLIP_ESCAPE;
    m.cycleStop     = WM_FLIP_CYCLE_STOP;
    m.scrub         = WM_FLIP_SCRUB;
    m.scrubEnd      = WM_FLIP_SCRUB_END;
    m.pointerMove   = WM_FLIP_POINTER_MOVE;
    m.pointerSelect = WM_FLIP_POINTER_SELECT;
    m.pointerClose  = WM_FLIP_POINTER_CLOSE;
    m.closeSelected = WM_FLIP_CLOSE_SELECTED;
    m.searchChar    = WM_FLIP_SEARCH_CHAR;
    m.searchBack    = WM_FLIP_SEARCH_BACK;
    return m;
}

bool HotkeyManager::Init(HWND hwndOwner)
{
    if (m_installed)
        return false;

    m_hwndOwner = hwndOwner;

    if (!KeyboardHook::Install(hwndOwner, HookMessages()))
        return false;

    // Optional second input source.  A failure here is never fatal — the
    // keyboard trigger is already live and stays the app's contract.
    TouchpadHook::Install(hwndOwner,
                          WM_FLIP_ACTIVATE,
                          WM_FLIP_CYCLE,
                          WM_FLIP_CYCLE_BACK,
                          WM_FLIP_DISMISS,
                          WM_FLIP_ESCAPE,
                          WM_FLIP_SCRUB,
                          WM_FLIP_SCRUB_END);

    m_installed = true;
    return true;
}

void HotkeyManager::Shutdown()
{
    if (m_installed) {
        TouchpadHook::Uninstall();
        KeyboardHook::Uninstall();
        m_installed = false;
    }
    m_hwndOwner = nullptr;
}

void HotkeyManager::SetCallback(HotkeyCallback callback, void* userData)
{
    m_callback = callback;
    m_userData = userData;
}

void HotkeyManager::SetTriggerOptions(const KeyboardHook::TriggerOptions& opts)
{
    KeyboardHook::SetOptions(opts);
}

void HotkeyManager::SetTouchpadOptions(const TouchpadHook::Options& opts)
{
    TouchpadHook::SetOptions(opts);
}

bool HotkeyManager::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    HotkeyEvent event;
    float amount = 0.0f;
    int   x = 0, y = 0;
    switch (msg) {
    case WM_FLIP_ACTIVATE:   event = HotkeyEvent::Activate;  break;
    case WM_FLIP_CYCLE:      event = HotkeyEvent::Cycle;     break;
    case WM_FLIP_CYCLE_BACK: event = HotkeyEvent::CycleBack; break;
    case WM_FLIP_DISMISS:    event = HotkeyEvent::Dismiss;   break;
    case WM_FLIP_ESCAPE:     event = HotkeyEvent::Escape;    break;
    case WM_FLIP_CYCLE_STOP: event = HotkeyEvent::CycleStop; break;
    case WM_FLIP_SCRUB:
        event  = HotkeyEvent::Scrub;
        // Fixed point: ten-thousandths of a window (see WM_FLIP_SCRUB).
        amount = static_cast<float>(static_cast<LONG>(lParam)) / 10000.0f;
        break;
    case WM_FLIP_SCRUB_END:  event = HotkeyEvent::ScrubEnd;  break;
    case WM_FLIP_POINTER_MOVE:
    case WM_FLIP_POINTER_SELECT:
    case WM_FLIP_POINTER_CLOSE:
        // Screen coordinates as two signed LONGs — a virtual desktop wider
        // than a SHORT would wrap a packed pair onto the wrong monitor.
        event = (msg == WM_FLIP_POINTER_MOVE)   ? HotkeyEvent::PointerMove
              : (msg == WM_FLIP_POINTER_SELECT) ? HotkeyEvent::PointerSelect
                                                : HotkeyEvent::PointerClose;
        x = static_cast<int>(static_cast<LONG>(static_cast<LONG_PTR>(wParam)));
        y = static_cast<int>(static_cast<LONG>(static_cast<LONG_PTR>(lParam)));
        break;
    case WM_FLIP_CLOSE_SELECTED: event = HotkeyEvent::CloseSelected; break;
    case WM_FLIP_SEARCH_CHAR:
        event = HotkeyEvent::SearchChar;
        x = static_cast<int>(wParam);
        break;
    case WM_FLIP_SEARCH_BACK: event = HotkeyEvent::SearchBack; break;
    default: return false;
    }

    if (m_callback)
        m_callback(event, amount, x, y, m_userData);

    return true;
}
