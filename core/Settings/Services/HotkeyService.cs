using System.Runtime.InteropServices;

namespace CKFlip3D.Settings.Services;

/// <summary>
/// Activation-hotkey capture + classification.
///
/// Capture installs temporary low-level keyboard/mouse hooks on the UI
/// thread (WPF pumps messages, so the callbacks arrive on the dispatcher).
/// Keyboard input is swallowed while capturing — including the Windows key,
/// so Win-combos can be recorded without the Start menu popping.  Mouse
/// AXES (movement, wheel) are ignored by design; buttons are candidates,
/// except a bare left click, which keeps driving the UI (Cancel button).
///
/// The combo string format ("Win+Tab", "Ctrl+Alt+F", "MButton") is a shared
/// contract with the C++ parser — keep token names in sync with
/// KeyboardHook::ParseHotkey (hook/keyboardhook.cpp).
/// </summary>
public static class HotkeyService
{
    // ---- LL hook plumbing --------------------------------------------------

    private const int WH_KEYBOARD_LL = 13;
    private const int WH_MOUSE_LL = 14;

    private const int WM_KEYDOWN = 0x0100;
    private const int WM_KEYUP = 0x0101;
    private const int WM_SYSKEYDOWN = 0x0104;
    private const int WM_SYSKEYUP = 0x0105;
    private const int WM_LBUTTONDOWN = 0x0201;
    private const int WM_RBUTTONDOWN = 0x0204;
    private const int WM_MBUTTONDOWN = 0x0207;
    private const int WM_XBUTTONDOWN = 0x020B;

    [StructLayout(LayoutKind.Sequential)]
    private struct KBDLLHOOKSTRUCT
    {
        public uint vkCode;
        public uint scanCode;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MSLLHOOKSTRUCT
    {
        public int ptX, ptY;
        public uint mouseData;
        public uint flags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    private delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SetWindowsHookExW(int idHook, HookProc proc, IntPtr hMod, uint threadId);

    [DllImport("user32.dll")]
    private static extern bool UnhookWindowsHookEx(IntPtr hhk);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandleW(string? name);

    // Delegates kept alive for the duration of the capture (a GC'd hook
    // delegate crashes the process on the next callback).
    private static HookProc? _keyboardProc;
    private static HookProc? _mouseProc;
    private static IntPtr _keyboardHook;
    private static IntPtr _mouseHook;

    private static Action<string>? _onPreview;   // live "currently held" text
    private static Action<string>? _onCaptured;  // final combo
    private static Action? _onCancelled;         // Esc pressed

    // Modifiers currently held during the capture (tracked from swallowed
    // events — the OS key state never sees them).
    private static bool _ctrl, _shift, _alt, _win;
    // ≥2 modifier families were held at once — releasing them without a
    // main key is NOT a bare-modifier binding, just an abandoned attempt.
    private static bool _multiMods;
    private static bool _capturing;

    public static bool IsCapturing => _capturing;

    /// <summary>Begin capturing. Callbacks arrive on the UI thread.</summary>
    public static void StartCapture(Action<string> onPreview,
                                    Action<string> onCaptured,
                                    Action onCancelled)
    {
        if (_capturing) StopCapture();

        _onPreview = onPreview;
        _onCaptured = onCaptured;
        _onCancelled = onCancelled;
        _ctrl = _shift = _alt = _win = false;
        _multiMods = false;
        _capturing = true;

        _keyboardProc = KeyboardProc;
        _mouseProc = MouseProc;
        IntPtr module = GetModuleHandleW(null);
        _keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, _keyboardProc, module, 0);
        _mouseHook = SetWindowsHookExW(WH_MOUSE_LL, _mouseProc, module, 0);
    }

    public static void StopCapture()
    {
        if (_keyboardHook != IntPtr.Zero) { UnhookWindowsHookEx(_keyboardHook); _keyboardHook = IntPtr.Zero; }
        if (_mouseHook != IntPtr.Zero) { UnhookWindowsHookEx(_mouseHook); _mouseHook = IntPtr.Zero; }
        _keyboardProc = null;
        _mouseProc = null;
        _onPreview = null;
        _onCaptured = null;
        _onCancelled = null;
        _capturing = false;
    }

    private static string ModsPrefix()
    {
        string s = "";
        if (_ctrl) s += "Ctrl+";
        if (_shift) s += "Shift+";
        if (_alt) s += "Alt+";
        if (_win) s += "Win+";
        return s;
    }

    private static void Finish(string combo)
    {
        var captured = _onCaptured;
        StopCapture();
        captured?.Invoke(combo);
    }

    private static IntPtr KeyboardProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0 || !_capturing)
            return CallNextHookEx(_keyboardHook, nCode, wParam, lParam);

        var kb = Marshal.PtrToStructure<KBDLLHOOKSTRUCT>(lParam);
        int msg = (int)wParam;
        bool isDown = msg is WM_KEYDOWN or WM_SYSKEYDOWN;
        bool isUp = msg is WM_KEYUP or WM_SYSKEYUP;
        uint vk = kb.vkCode;

        // Esc cancels the capture (it is the session-cancel key, never a
        // bindable trigger).
        if (isDown && vk == 0x1B /*VK_ESCAPE*/)
        {
            var cancelled = _onCancelled;
            StopCapture();
            cancelled?.Invoke();
            return (IntPtr)1;
        }

        string? modFamily = vk switch
        {
            0xA2 or 0xA3 or 0x11 => "Ctrl",
            0xA0 or 0xA1 or 0x10 => "Shift",
            0xA4 or 0xA5 or 0x12 => "Alt",
            0x5B or 0x5C => "Win",
            _ => null,
        };

        if (modFamily != null)
        {
            switch (modFamily)
            {
                case "Ctrl": _ctrl = isDown; break;
                case "Shift": _shift = isDown; break;
                case "Alt": _alt = isDown; break;
                case "Win": _win = isDown; break;
            }
            int heldCount = (_ctrl ? 1 : 0) + (_shift ? 1 : 0)
                          + (_alt ? 1 : 0) + (_win ? 1 : 0);
            if (heldCount >= 2) _multiMods = true;

            // Bare-modifier binding: the last held modifier released with
            // nothing else combined → e.g. "Win" alone.  An abandoned
            // multi-modifier hold just resets the prompt.
            if (isUp && heldCount == 0)
            {
                if (_multiMods)
                {
                    _multiMods = false;
                    _onPreview?.Invoke("…");
                    return (IntPtr)1;
                }
                Finish(modFamily);
                return (IntPtr)1;
            }

            string held = ModsPrefix().TrimEnd('+');
            _onPreview?.Invoke(held.Length > 0 ? held + " + …" : "…");
            return (IntPtr)1;   // swallow all modifier traffic while capturing
        }

        if (isDown)
        {
            // Enter is the session-commit key — not bindable; ignore.
            if (vk == 0x0D)
                return (IntPtr)1;

            string? name = KeyNameOf(vk);
            if (name != null)
            {
                Finish(ModsPrefix() + name);
                return (IntPtr)1;
            }
        }
        return (IntPtr)1;   // swallow everything else too
    }

    private static IntPtr MouseProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode < 0 || !_capturing)
            return CallNextHookEx(_mouseHook, nCode, wParam, lParam);

        int msg = (int)wParam;
        string? btn = null;
        switch (msg)
        {
            case WM_LBUTTONDOWN:
                // A bare left click keeps driving the UI (Cancel button);
                // it is only a candidate when combined with modifiers.
                if (_ctrl || _shift || _alt || _win) btn = "LButton";
                break;
            case WM_RBUTTONDOWN: btn = "RButton"; break;
            case WM_MBUTTONDOWN: btn = "MButton"; break;
            case WM_XBUTTONDOWN:
                var ms = Marshal.PtrToStructure<MSLLHOOKSTRUCT>(lParam);
                btn = (ms.mouseData >> 16) == 1 ? "XButton1" : "XButton2";
                break;
        }

        if (btn != null)
        {
            Finish(ModsPrefix() + btn);
            return (IntPtr)1;   // swallow the captured click
        }
        // Movement, wheel (axes) and plain left clicks pass through.
        return CallNextHookEx(_mouseHook, nCode, wParam, lParam);
    }

    // ---- VK → shared token name (mirror of KeyboardHook::ParseHotkey) -----

    private static string? KeyNameOf(uint vk)
    {
        if (vk >= 'A' && vk <= 'Z') return ((char)vk).ToString();
        if (vk >= '0' && vk <= '9') return ((char)vk).ToString();
        if (vk >= 0x70 && vk <= 0x87) return "F" + (vk - 0x6F);
        return vk switch
        {
            0x09 => "Tab",
            0x20 => "Space",
            0x08 => "Backspace",
            0x2E => "Delete",
            0x2D => "Insert",
            0x24 => "Home",
            0x23 => "End",
            0x21 => "PageUp",
            0x22 => "PageDown",
            0x25 => "Left",
            0x26 => "Up",
            0x27 => "Right",
            0x28 => "Down",
            0x14 => "CapsLock",
            0x90 => "NumLock",
            0x91 => "ScrollLock",
            0x2C => "PrintScreen",
            0x13 => "Pause",
            0x5D => "Apps",
            0xBB => "Plus",
            0xBD => "Minus",
            0xBC => "Comma",
            0xBE => "Period",
            0xBA => "Semicolon",
            0xBF => "Slash",
            0xC0 => "Grave",
            0xDB => "LBracket",
            0xDC => "Backslash",
            0xDD => "RBracket",
            0xDE => "Quote",
            >= 0x60 and <= 0x69 => "Numpad" + (vk - 0x60),
            0x6A => "Multiply",
            0x6B => "Add",
            0x6D => "Subtract",
            0x6E => "Decimal",
            0x6F => "Divide",
            _ => $"0x{vk:X2}",
        };
    }

    // ---- Problematic-combination classification ----------------------------

    private static readonly HashSet<string> CommonShortcuts = new(StringComparer.OrdinalIgnoreCase)
    {
        "Ctrl+C", "Ctrl+V", "Ctrl+X", "Ctrl+Z", "Ctrl+Y", "Ctrl+A",
        "Ctrl+S", "Ctrl+F", "Ctrl+W", "Ctrl+T", "Ctrl+N", "Ctrl+P",
        "Alt+Tab", "Alt+F4", "Win+D", "Win+E", "Win+R", "Win+L",
        "Win+S", "Win+X", "Win+V", "Ctrl+Shift+Esc",
    };

    /// <summary>
    /// Returns a confirmation question for risky combinations (bare Windows
    /// key, single letters/digits, well-known shortcuts like Ctrl+C), or
    /// null when the combo is safe to assign silently.
    /// </summary>
    public static string? GetWarning(string combo)
    {
        if (string.IsNullOrWhiteSpace(combo)) return null;

        string[] parts = combo.Split('+', StringSplitOptions.TrimEntries);
        bool hasMods = parts.Length > 1;
        string main = parts[^1];

        if (!hasMods)
        {
            if (main.Equals("Win", StringComparison.OrdinalIgnoreCase))
                return "The Windows key alone will no longer open the Start menu — every press opens CKFlip3D instead. Are you sure you want to use it?";
            if (main is { Length: 1 })
                return $"\"{main}\" alone means typing this character anywhere will open CKFlip3D instead. Are you sure you want to use it?";
            if (main.Equals("LButton", StringComparison.OrdinalIgnoreCase)
                || main.Equals("RButton", StringComparison.OrdinalIgnoreCase))
                return "Binding a bare mouse button will interfere with normal clicking. Are you sure you want to use it?";
            if (main.Equals("Ctrl", StringComparison.OrdinalIgnoreCase)
                || main.Equals("Shift", StringComparison.OrdinalIgnoreCase)
                || main.Equals("Alt", StringComparison.OrdinalIgnoreCase))
                return $"The {main} key alone is used constantly while typing — every press opens CKFlip3D. Are you sure you want to use it?";
            return null;   // MButton / XButton1 / XButton2 / F-keys etc.
        }

        if (CommonShortcuts.Contains(combo))
            return $"{combo} is a common system shortcut — binding it means the original action stops working while CKFlip3D runs. Are you sure you want to use it?";

        return null;
    }
}
