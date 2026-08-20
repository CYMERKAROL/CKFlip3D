// ---------------------------------------------------------------------------
// Recording an activation hotkey by actually pressing it.  While capture is
// running the keyboard is swallowed, Windows key included, so a Win combo can
// be recorded without the Start menu answering first.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Runtime.InteropServices;
using System.Windows;

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
    // Enter and Escape are the session's own commit/cancel keys, so the
    // ACTIVATION capture refuses them — binding activation to the key that
    // closes the cascade would make it un-openable.  Capturing the commit and
    // cancel bindings themselves is the one case where they are exactly what
    // the user is trying to press, so that capture opts in.  The modal's
    // Cancel button is then the way out (a bare left click always passes
    // through to the UI).
    private static bool _allowReservedKeys;

    // Mouse-button-only capture (the pointer bindings).  Here a BARE left
    // click is exactly what the user may be trying to bind, so it can no
    // longer be allowed through to the UI — which would leave the modal's
    // Cancel button unclickable.  The caller therefore hands over that
    // button's screen rectangle as a dead zone: clicks inside it are ignored
    // by the capture and reach WPF normally, clicks anywhere else are the
    // binding.
    //
    // The dead zone is not tied to mouse-only capture: a capture that takes
    // keys AND buttons (the cascade key lists) needs exactly the same way out,
    // and once there is one, Esc is free to be a binding like any other key.
    private static bool _mouseOnly;
    private static RECT _deadZone;

    // Releasing the last held modifier with nothing combined normally FINISHES
    // the capture as a bare-modifier binding ("Win").  The cascade lists have
    // no use for one — the hook answers modifier keys before it consults any
    // binding — so there it only resets the prompt.
    private static bool _bareModifiers = true;

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")]
    private static extern bool GetCursorPos(out POINT pt);

    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X, Y; }

    public static bool IsCapturing => _capturing;

    /// <summary>
    /// Capture a single mouse button (no modifiers, no keyboard keys).
    /// <paramref name="deadZoneScreen"/> is a screen rectangle — normally the
    /// modal's button strip — where clicks are left alone so there is always a
    /// way out that is not the keyboard.
    /// </summary>
    public static void StartMouseButtonCapture(Action<string> onPreview,
                                               Action<string> onCaptured,
                                               Action onCancelled,
                                               Rect deadZoneScreen)
    {
        StartCapture(onPreview, onCaptured, onCancelled,
                     deadZoneScreen: deadZoneScreen);
        _mouseOnly = true;
    }

    private static bool HasDeadZone =>
        _deadZone.Right > _deadZone.Left && _deadZone.Bottom > _deadZone.Top;

    private static bool InDeadZone()
    {
        if (!HasDeadZone)
            return false;
        if (!GetCursorPos(out POINT p))
            return false;
        return p.X >= _deadZone.Left && p.X < _deadZone.Right
            && p.Y >= _deadZone.Top && p.Y < _deadZone.Bottom;
    }

    /// <summary>
    /// Begin capturing. Callbacks arrive on the UI thread.
    ///
    /// <paramref name="deadZoneScreen"/> is a screen rectangle — normally the
    /// modal's button strip — where clicks are left alone. Passing one is what
    /// makes a BARE left click bindable, because the Cancel button underneath
    /// keeps working; without it a bare left click is only a candidate
    /// alongside modifiers, so it can keep driving the UI.
    /// </summary>
    public static void StartCapture(Action<string> onPreview,
                                    Action<string> onCaptured,
                                    Action onCancelled,
                                    bool allowReservedKeys = false,
                                    Rect deadZoneScreen = default,
                                    bool allowBareModifiers = true)
    {
        if (_capturing) StopCapture();

        _onPreview = onPreview;
        _onCaptured = onCaptured;
        _onCancelled = onCancelled;
        _allowReservedKeys = allowReservedKeys;
        _bareModifiers = allowBareModifiers;
        _deadZone = deadZoneScreen.IsEmpty ? default : new RECT
        {
            Left = (int)Math.Round(deadZoneScreen.Left),
            Top = (int)Math.Round(deadZoneScreen.Top),
            Right = (int)Math.Round(deadZoneScreen.Right),
            Bottom = (int)Math.Round(deadZoneScreen.Bottom),
        };
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
        _allowReservedKeys = false;
        _bareModifiers = true;
        _mouseOnly = false;
        _deadZone = default;
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
        // bindable trigger) — unless the caller is capturing that very
        // binding, in which case it falls through to KeyNameOf below.
        if (isDown && vk == 0x1B /*VK_ESCAPE*/ && !_allowReservedKeys)
        {
            var cancelled = _onCancelled;
            StopCapture();
            cancelled?.Invoke();
            return (IntPtr)1;
        }

        // Mouse-only capture: the keyboard has nothing to contribute, so every
        // key is swallowed (no stray input into the page behind the modal) and
        // only Esc — handled above — means anything.
        if (_mouseOnly)
            return (IntPtr)1;

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
                if (_multiMods || !_bareModifiers)
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
            // Enter is the session-commit key — not bindable as a trigger.
            if (vk == 0x0D && !_allowReservedKeys)
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

        // Leave the dead zone (the modal's own buttons) alone so Cancel keeps
        // working, and take everything else as-is — including a bare left
        // click, which is a perfectly ordinary binding for picking a window.
        if (msg == WM_LBUTTONDOWN && InDeadZone())
            return CallNextHookEx(_mouseHook, nCode, wParam, lParam);

        string? btn = null;
        switch (msg)
        {
            case WM_LBUTTONDOWN:
                // Without a dead zone a bare left click keeps driving the UI
                // (Cancel button); it is only a candidate when combined with
                // modifiers.
                if (_mouseOnly || HasDeadZone || _ctrl || _shift || _alt || _win)
                    btn = "LButton";
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
            Finish(_mouseOnly ? btn : ModsPrefix() + btn);
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
            // Reached only by a capture that opted into the reserved keys;
            // without names here they would be recorded as "0x0D" / "0x1B".
            0x0D => "Enter",
            0x1B => "Escape",
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

    // ---- Token → VK (the inverse of KeyNameOf) ------------------------------

    /// <summary>
    /// The virtual-key code a MAIN-key token stands for, or 0 for anything that
    /// is not one (a mouse button, a modifier name, nonsense).
    ///
    /// Needed because the same key has several spellings that all parse: the
    /// defaults ship as "Enter" / "Escape" / "Delete" while a capture writes
    /// what KeyNameOf produces, which for a few keys is the raw "0x0D" form.
    /// Comparing the STRINGS would therefore miss a genuine collision — someone
    /// binding a navigation key to their own cancel key and finding the cascade
    /// no longer closes. Comparing the codes cannot.
    ///
    /// Mirrors KeyboardHook::ParseHotkey's main-key table (hook/keyboardhook.cpp);
    /// the two are one contract and have to move together.
    /// </summary>
    public static uint TokenToVk(string? token)
    {
        if (string.IsNullOrWhiteSpace(token)) return 0;
        string t = token.Trim().ToLowerInvariant();

        if (t.Length == 1)
        {
            char c = t[0];
            if (c is >= 'a' and <= 'z') return (uint)(c - 'a' + 'A');
            if (c is >= '0' and <= '9') return c;
            return 0;
        }
        if (t.Length >= 2 && t[0] == 'f' && char.IsDigit(t[1])
            && int.TryParse(t.AsSpan(1), out int fn) && fn is >= 1 and <= 24)
            return (uint)(0x70 + fn - 1);
        if (t.StartsWith("0x", StringComparison.Ordinal)
            && uint.TryParse(t.AsSpan(2), System.Globalization.NumberStyles.HexNumber,
                             System.Globalization.CultureInfo.InvariantCulture, out uint hv)
            && hv is > 0 and < 0xFF)
            return hv;

        return t switch
        {
            "tab" => 0x09,
            "space" => 0x20,
            "enter" or "return" => 0x0D,
            "esc" or "escape" => 0x1B,
            "backspace" => 0x08,
            "delete" or "del" => 0x2E,
            "insert" or "ins" => 0x2D,
            "home" => 0x24,
            "end" => 0x23,
            "pageup" or "pgup" => 0x21,
            "pagedown" or "pgdn" => 0x22,
            "left" => 0x25,
            "up" => 0x26,
            "right" => 0x27,
            "down" => 0x28,
            "capslock" => 0x14,
            "numlock" => 0x90,
            "scrolllock" => 0x91,
            "printscreen" or "prtsc" => 0x2C,
            "pause" => 0x13,
            "apps" => 0x5D,
            "plus" => 0xBB,
            "minus" => 0xBD,
            "comma" => 0xBC,
            "period" => 0xBE,
            "semicolon" => 0xBA,
            "slash" => 0xBF,
            "grave" or "tilde" => 0xC0,
            "lbracket" => 0xDB,
            "backslash" => 0xDC,
            "rbracket" => 0xDD,
            "quote" => 0xDE,
            "numpad0" => 0x60, "numpad1" => 0x61, "numpad2" => 0x62,
            "numpad3" => 0x63, "numpad4" => 0x64, "numpad5" => 0x65,
            "numpad6" => 0x66, "numpad7" => 0x67, "numpad8" => 0x68,
            "numpad9" => 0x69,
            "multiply" => 0x6A,
            "add" => 0x6B,
            "subtract" => 0x6D,
            "decimal" => 0x6E,
            "divide" => 0x6F,
            _ => 0,
        };
    }

    /// <summary>The VK the combination ENDS in — its main key — or 0.</summary>
    public static uint MainKeyVk(string? combo) =>
        string.IsNullOrWhiteSpace(combo)
            ? 0
            : TokenToVk(combo.Split('+', StringSplitOptions.TrimEntries)[^1]);

    // ---- Mouse buttons and modifiers as part of a binding ------------------

    /// <summary>The VK a MOUSE-button token stands for, or 0.</summary>
    public static uint MouseTokenToVk(string? token) =>
        (token ?? "").Trim().ToLowerInvariant() switch
        {
            "lbutton" => 0x01,
            "rbutton" => 0x02,
            "mbutton" or "middlebutton" => 0x04,
            "xbutton1" or "mouse4" => 0x05,
            "xbutton2" or "mouse5" => 0x06,
            _ => 0,
        };

    /// <summary>Anything the cascade can bind — key or mouse button — or 0.</summary>
    public static uint BindingTokenToVk(string? token)
    {
        uint vk = TokenToVk(token);
        return vk != 0 ? vk : MouseTokenToVk(token);
    }

    /// <summary>The VK the combination ends in, mouse buttons included, or 0.</summary>
    public static uint MainBindingVk(string? combo) =>
        string.IsNullOrWhiteSpace(combo)
            ? 0
            : BindingTokenToVk(combo.Split('+', StringSplitOptions.TrimEntries)[^1]);

    public const uint ModCtrl = 1, ModShift = 2, ModAlt = 4, ModWin = 8;

    /// <summary>The modifier bit this token names, or 0 when it names a key.</summary>
    public static uint ModifierBitOf(string? token) =>
        (token ?? "").Trim().ToLowerInvariant() switch
        {
            "ctrl" or "control" => ModCtrl,
            "shift" => ModShift,
            "alt" => ModAlt,
            "win" or "windows" or "super" or "meta" => ModWin,
            _ => 0,
        };

    /// <summary>
    /// The modifiers a combination asks to be held, its main key aside.
    /// "Ctrl+Shift+F" → Ctrl|Shift; "F" and a bare "Ctrl" → none.
    /// </summary>
    public static uint ModsOf(string? combo)
    {
        if (string.IsNullOrWhiteSpace(combo)) return 0;
        uint mods = 0;
        foreach (string part in combo.Split('+', StringSplitOptions.TrimEntries).SkipLast(1))
            mods |= ModifierBitOf(part);
        return mods;
    }

    /// <summary>A combination that is nothing but a modifier ("Ctrl", "Win").</summary>
    public static bool IsBareModifier(string? combo)
    {
        if (string.IsNullOrWhiteSpace(combo)) return false;
        string[] parts = combo.Split('+', StringSplitOptions.TrimEntries);
        return parts.Length == 1 && ModifierBitOf(parts[0]) != 0;
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
