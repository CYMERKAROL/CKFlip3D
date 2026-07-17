#include "hotkeymanager.h"
#include "keyboardhook.h"

HotkeyManager::~HotkeyManager()
{
    Shutdown();
}

bool HotkeyManager::Init(HWND hwndOwner)
{
    if (m_installed)
        return false;

    m_hwndOwner = hwndOwner;

    if (!KeyboardHook::Install(hwndOwner,
                               WM_FLIP_ACTIVATE,
                               WM_FLIP_CYCLE,
                               WM_FLIP_CYCLE_BACK,
                               WM_FLIP_DISMISS,
                               WM_FLIP_ESCAPE,
                               WM_FLIP_CYCLE_STOP))
        return false;

    m_installed = true;
    return true;
}

void HotkeyManager::Shutdown()
{
    if (m_installed) {
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

bool HotkeyManager::HandleMessage(UINT msg, WPARAM /*wParam*/, LPARAM /*lParam*/)
{
    HotkeyEvent event;
    switch (msg) {
    case WM_FLIP_ACTIVATE:   event = HotkeyEvent::Activate;  break;
    case WM_FLIP_CYCLE:      event = HotkeyEvent::Cycle;     break;
    case WM_FLIP_CYCLE_BACK: event = HotkeyEvent::CycleBack; break;
    case WM_FLIP_DISMISS:    event = HotkeyEvent::Dismiss;   break;
    case WM_FLIP_ESCAPE:     event = HotkeyEvent::Escape;    break;
    case WM_FLIP_CYCLE_STOP: event = HotkeyEvent::CycleStop; break;
    default: return false;
    }

    if (m_callback)
        m_callback(event, m_userData);

    return true;
}
