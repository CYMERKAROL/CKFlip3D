// ---------------------------------------------------------------------------
// Reading and writing config.json, and telling a running core to pick the file
// back up.  The layout is written flat and one key per line, because the C++
// side scans for keys rather than parsing a document.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using CKFlip3D.Settings.Models;

namespace CKFlip3D.Settings.Services;

/// <summary>
/// Reads/writes %APPDATA%\CKFlip3D\config.json.
///
/// The C++ core (core/Config.cpp) uses a naive flat key scanner, so this writer
/// always emits a flat, one-key-per-line JSON object and keeps the key names
/// byte-for-byte identical to the C++ writer. Keys the core does not know yet
/// (cascadeMonitor, secondaryTaskbarMode) are ignored by it and kept for
/// forward compatibility.
/// </summary>
public static class ConfigService
{
    public static string ConfigPath
    {
        get
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "CKFlip3D");
            Directory.CreateDirectory(dir);
            return Path.Combine(dir, "config.json");
        }
    }

    public static SettingsModel Load()
    {
        var m = new SettingsModel();
        try
        {
            string path = ConfigPath;
            if (File.Exists(path))
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(path),
                    new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
                var root = doc.RootElement;

                // Keys consumed by the core (must stay in sync with core/Config.h)
                m.Antialiasing      = GetBool(root, "antialiasing", m.Antialiasing);
                m.Animations        = GetBool(root, "animations", m.Animations);
                m.AnimEntryExit     = GetBool(root, "animEntryExit", m.AnimEntryExit);
                m.AnimCycle         = GetBool(root, "animCycle", m.AnimCycle);
                m.AnimClose         = GetBool(root, "animClose", m.AnimClose);
                m.AnimLabel         = GetBool(root, "animLabel", m.AnimLabel);
                m.AnimHover         = GetBool(root, "animHover", m.AnimHover);
                m.MotionBlur        = GetBool(root, "motionBlur", m.MotionBlur);
                m.LivePreview       = GetBool(root, "livePreview", m.LivePreview);
                m.LiveBackground    = GetBool(root, "liveBackground", m.LiveBackground);
                m.VsyncLivePreview  = GetBool(root, "vsyncLivePreview", m.VsyncLivePreview);
                m.TaskbarLivePreview = GetBool(root, "taskbarLivePreview", m.TaskbarLivePreview);
                m.TaskbarPreview    = GetBool(root, "taskbarPreview", m.TaskbarPreview);
                m.MaxWindows        = (uint)GetInt(root, "maxWindows", (int)m.MaxWindows);
                m.VisualPreset      = GetInt(root, "visualPreset", m.VisualPreset);
                m.Reflections       = GetBool(root, "reflections", m.Reflections);
                m.BackgroundOpacity = GetInt(root, "backgroundOpacity", m.BackgroundOpacity);
                m.BackgroundBlur    = GetInt(root, "backgroundBlur", m.BackgroundBlur);
                m.ShowDesktopTile    = GetBool(root, "showDesktopTile", m.ShowDesktopTile);
                m.SelectedLabel      = GetBool(root, "selectedLabel", m.SelectedLabel);
                m.SelectedLabelTitle = GetBool(root, "selectedLabelTitle", m.SelectedLabelTitle);
                m.SelectedLabelIcon  = GetBool(root, "selectedLabelIcon", m.SelectedLabelIcon);
                m.SelectedLabelBox   = GetBool(root, "selectedLabelBox", m.SelectedLabelBox);
                m.AutoPerfTune      = GetBool(root, "autoPerfTune", m.AutoPerfTune);
                m.PerfProfile       = GetInt(root, "perfProfile", m.PerfProfile);
                m.StartDelayMs      = GetInt(root, "startDelayMs", m.StartDelayMs);
                m.IgnoreFullscreen  = GetBool(root, "ignoreFullscreen", m.IgnoreFullscreen);
                m.MouseWheelCycle   = GetBool(root, "mouseWheelCycle", m.MouseWheelCycle);
                m.IgnoredApps       = GetString(root, "ignoredApps", m.IgnoredApps);
                m.ExcludedApps      = GetString(root, "excludedApps", m.ExcludedApps);
                m.ActivationHotkey  = GetString(root, "activationHotkey", m.ActivationHotkey);
                m.HotkeyToggleMode  = GetBool(root, "hotkeyToggleMode", m.HotkeyToggleMode);

                // ---- Commit / cancel / close: one key each, until Build 3 ---
                // Mirrors core/Config.cpp: read the list, and when the file has
                // none, seed it from the single key that file DOES carry.
                bool hadCommit = root.TryGetProperty("commitKeys", out _);
                m.CommitKeys = GetString(root, "commitKeys", m.CommitKeys);
                if (!hadCommit)
                    m.CommitKeys = GetString(root, "commitHotkey", m.CommitKeys);

                bool hadCancel = root.TryGetProperty("cancelKeys", out _);
                m.CancelKeys = GetString(root, "cancelKeys", m.CancelKeys);
                if (!hadCancel)
                    m.CancelKeys = GetString(root, "cancelHotkey", m.CancelKeys);

                // The close key's old master switch said exactly what an empty
                // (or wholly parked) list says, so it is gone the way
                // keyboardNav went. A file that still carries it OFF meant "no
                // close key" and has to keep meaning that — parked, not
                // deleted, so the page shows it ready to come back.
                bool hadClose = root.TryGetProperty("closeKeys", out _);
                m.CloseKeys = GetString(root, "closeKeys", m.CloseKeys);
                if (!hadClose)
                {
                    m.CloseKeys = GetString(root, "closeHotkey", m.CloseKeys);
                    if (!GetBool(root, "closeKeyEnabled", true))
                        m.CloseKeys = ParkAll(m.CloseKeys);
                }

                // ---- Navigation keys, and the 1.5 file that has none -------
                // Read after activationHotkey, and mirroring core/Config.cpp
                // exactly — the core reads this same file, often before this
                // app is ever opened, and the two must not disagree about what
                // an older config means.
                //
                // A pre-1.6 file has no lists at all, and back then the
                // ACTIVATION key stepped the stack as a side effect of opening
                // it. Seed from that file's own hotkey so an update changes
                // nothing the user can feel, and only when the key is ABSENT:
                // once 1.6 has written the lists they are the truth, hotkey
                // changes included.
                bool hadNavKeys = root.TryGetProperty("navForwardKeys", out _);
                m.NavForwardKeys = GetString(root, "navForwardKeys", m.NavForwardKeys);
                m.NavBackKeys    = GetString(root, "navBackKeys", m.NavBackKeys);
                if (!hadNavKeys)
                {
                    string main = NavTokenOfBinding(m.ActivationHotkey);
                    m.NavForwardKeys = main.Length == 0 ? "Down;Right" : $"{main};Down;Right";
                    m.NavBackKeys    = main.Length == 0 ? "Up;Left" : $"Shift+{main};Up;Left";
                }

                // Legacy `keyboardNav` (1.5 and earlier): one switch over the
                // four hard-wired arrows, replaced in 1.6 by the two lists. A
                // file that still carries it OFF meant "no navigation keys", so
                // park every entry rather than handing back arrows somebody
                // switched off — parked and not deleted, so the Navigation keys
                // page shows them ready to come back.
                if (!GetBool(root, "keyboardNav", true))
                {
                    m.NavForwardKeys = ParkAll(m.NavForwardKeys);
                    m.NavBackKeys = ParkAll(m.NavBackKeys);
                }
                m.PointerInCascade   = GetBool(root, "pointerInCascade", m.PointerInCascade);
                m.MouseSelect        = GetBool(root, "mouseSelect", m.MouseSelect);
                m.MouseSelectButton  = GetInt(root, "mouseSelectButton", m.MouseSelectButton);
                m.MouseDragEnabled   = GetBool(root, "mouseDragEnabled", m.MouseDragEnabled);
                m.MouseDragButton    = GetInt(root, "mouseDragButton", m.MouseDragButton);
                m.CloseFromCascade   = GetBool(root, "closeFromCascade", m.CloseFromCascade);
                m.MouseCloseButton   = GetInt(root, "mouseCloseButton", m.MouseCloseButton);
                m.SearchEnabled      = GetBool(root, "searchEnabled", m.SearchEnabled);
                m.SearchBox          = GetBool(root, "searchBox", m.SearchBox);
                m.SearchMatchProcess = GetBool(root, "searchMatchProcess", m.SearchMatchProcess);
                m.SearchPosX         = GetInt(root, "searchPosX", m.SearchPosX);
                m.SearchPosY         = GetInt(root, "searchPosY", m.SearchPosY);
                m.SearchScale        = GetInt(root, "searchScale", m.SearchScale);
                m.TouchpadNav             = GetBool(root, "touchpadNav", m.TouchpadNav);
                m.TouchpadReverse         = GetBool(root, "touchpadReverse", m.TouchpadReverse);
                m.TouchpadSensitivity     = GetInt(root, "touchpadSensitivity", m.TouchpadSensitivity);
                m.TouchpadSmoothing       = GetInt(root, "touchpadSmoothing", m.TouchpadSmoothing);
                m.TouchpadCancelSwipe     = GetBool(root, "touchpadCancelSwipe", m.TouchpadCancelSwipe);
                m.TouchpadContinuous      = GetBool(root, "touchpadContinuous", m.TouchpadContinuous);
                m.WindowSnap              = GetBool(root, "windowSnap", m.WindowSnap);

                // ---- Touchpad gestures: one apiece, until Build 3 -----------
                // Each list replaces a single integer whose 0 meant "off" —
                // which is what an empty list says now. Seeded from the integer
                // only when the list is absent (core/Config.cpp does the same).
                if (root.TryGetProperty("touchpadActivateGestures", out _))
                    m.TouchpadActivateGestures = GetString(root, "touchpadActivateGestures", m.TouchpadActivateGestures);
                else
                    m.TouchpadActivateGestures = GetInt(root, "touchpadActivateGesture", 1) switch
                    {
                        0 => "",
                        2 => "TwoDownLeft",
                        3 => "FourDownRight",
                        4 => "FourDownLeft",
                        _ => "TwoDownRight",
                    };

                if (root.TryGetProperty("touchpadCycleGestures", out _))
                    m.TouchpadCycleGestures = GetString(root, "touchpadCycleGestures", m.TouchpadCycleGestures);
                else
                    m.TouchpadCycleGestures =
                        GetInt(root, "touchpadCycleFingers", 2) >= 4 ? "FourSwipe" : "TwoSwipe";

                if (root.TryGetProperty("touchpadCommitGestures", out _))
                    m.TouchpadCommitGestures = GetString(root, "touchpadCommitGestures", m.TouchpadCommitGestures);
                else
                    m.TouchpadCommitGestures = GetInt(root, "touchpadCommitGesture", 1) switch
                    {
                        0 => "",
                        2 => "TwoTap",
                        3 => "TwoDown",
                        _ => "OneTap",
                    };
                m.ShowDebugInfo     = GetBool(root, "showDebugInfo", m.ShowDebugInfo);

                // Forward-compatible keys (not consumed by the core yet)
                m.AppTheme             = GetInt(root, "appTheme", m.AppTheme);
                m.CascadeMonitor       = GetInt(root, "cascadeMonitor", m.CascadeMonitor);
                m.SecondaryTaskbarMode = GetInt(root, "secondaryTaskbarMode", m.SecondaryTaskbarMode);
            }
        }
        catch
        {
            // Corrupt config: fall back to defaults rather than crash the settings UI.
        }

        // Startup state lives in the Task Scheduler, not in config.json — read
        // from where it actually is, so a change made outside this app shows up
        // here rather than being overwritten.
        m.StartWithWindows = StartupService.IsEnabled();

        m.TakeSnapshot();
        return m;
    }

    public static void Save(SettingsModel m)
    {
        var sb = new StringBuilder(1024);
        sb.Append("{\n");

        // Core keys, same order as the C++ writer (core/Config.cpp).
        AppendBool(sb, "antialiasing", m.Antialiasing);
        AppendBool(sb, "animations", m.Animations);
        AppendBool(sb, "animEntryExit", m.AnimEntryExit);
        AppendBool(sb, "animCycle", m.AnimCycle);
        AppendBool(sb, "animClose", m.AnimClose);
        AppendBool(sb, "animLabel", m.AnimLabel);
        AppendBool(sb, "animHover", m.AnimHover);
        AppendBool(sb, "motionBlur", m.MotionBlur);
        AppendBool(sb, "livePreview", m.LivePreview);
        AppendBool(sb, "liveBackground", m.LiveBackground);
        AppendBool(sb, "vsyncLivePreview", m.VsyncLivePreview);
        AppendBool(sb, "taskbarLivePreview", m.TaskbarLivePreview);
        AppendBool(sb, "taskbarPreview", m.TaskbarPreview);
        AppendInt(sb, "maxWindows", (int)m.MaxWindows);
        AppendInt(sb, "visualPreset", m.VisualPreset);
        AppendBool(sb, "reflections", m.Reflections);
        AppendInt(sb, "backgroundOpacity", m.BackgroundOpacity);
        AppendInt(sb, "backgroundBlur", m.BackgroundBlur);
        AppendBool(sb, "showDesktopTile", m.ShowDesktopTile);
        AppendBool(sb, "selectedLabel", m.SelectedLabel);
        AppendBool(sb, "selectedLabelTitle", m.SelectedLabelTitle);
        AppendBool(sb, "selectedLabelIcon", m.SelectedLabelIcon);
        AppendBool(sb, "selectedLabelBox", m.SelectedLabelBox);
        AppendBool(sb, "autoPerfTune", m.AutoPerfTune);
        AppendInt(sb, "perfProfile", m.PerfProfile);
        AppendInt(sb, "startDelayMs", m.StartDelayMs);
        AppendBool(sb, "ignoreFullscreen", m.IgnoreFullscreen);
        AppendBool(sb, "mouseWheelCycle", m.MouseWheelCycle);
        AppendString(sb, "navForwardKeys", m.NavForwardKeys);
        AppendString(sb, "navBackKeys", m.NavBackKeys);
        AppendString(sb, "ignoredApps", m.IgnoredApps);
        AppendString(sb, "excludedApps", m.ExcludedApps);
        AppendString(sb, "activationHotkey", m.ActivationHotkey);
        AppendString(sb, "commitKeys", m.CommitKeys);
        AppendString(sb, "cancelKeys", m.CancelKeys);
        AppendString(sb, "closeKeys", m.CloseKeys);
        AppendBool(sb, "hotkeyToggleMode", m.HotkeyToggleMode);
        AppendBool(sb, "pointerInCascade", m.PointerInCascade);
        AppendBool(sb, "mouseSelect", m.MouseSelect);
        AppendInt(sb, "mouseSelectButton", m.MouseSelectButton);
        AppendBool(sb, "mouseDragEnabled", m.MouseDragEnabled);
        AppendInt(sb, "mouseDragButton", m.MouseDragButton);
        AppendBool(sb, "closeFromCascade", m.CloseFromCascade);
        AppendInt(sb, "mouseCloseButton", m.MouseCloseButton);
        AppendBool(sb, "searchEnabled", m.SearchEnabled);
        AppendBool(sb, "searchBox", m.SearchBox);
        AppendBool(sb, "searchMatchProcess", m.SearchMatchProcess);
        AppendInt(sb, "searchPosX", m.SearchPosX);
        AppendInt(sb, "searchPosY", m.SearchPosY);
        AppendInt(sb, "searchScale", m.SearchScale);
        AppendBool(sb, "touchpadNav", m.TouchpadNav);
        AppendString(sb, "touchpadActivateGestures", m.TouchpadActivateGestures);
        AppendString(sb, "touchpadCycleGestures", m.TouchpadCycleGestures);
        AppendString(sb, "touchpadCommitGestures", m.TouchpadCommitGestures);
        AppendBool(sb, "touchpadReverse", m.TouchpadReverse);
        AppendInt(sb, "touchpadSensitivity", m.TouchpadSensitivity);
        AppendInt(sb, "touchpadSmoothing", m.TouchpadSmoothing);
        AppendBool(sb, "touchpadCancelSwipe", m.TouchpadCancelSwipe);
        AppendBool(sb, "touchpadContinuous", m.TouchpadContinuous);
        AppendBool(sb, "windowSnap", m.WindowSnap);
        AppendBool(sb, "showDebugInfo", m.ShowDebugInfo);

        // Forward-compatible keys (ignored by the core until wired up).
        AppendInt(sb, "appTheme", m.AppTheme);
        AppendInt(sb, "cascadeMonitor", m.CascadeMonitor);
        AppendInt(sb, "secondaryTaskbarMode", m.SecondaryTaskbarMode);

        // Trim trailing ",\n" -> "\n"
        if (sb.Length >= 2 && sb[^2] == ',') sb.Remove(sb.Length - 2, 1);
        sb.Append("}\n");

        // Temp-write + atomic rename so the elevated core never observes a
        // half-written config.json on the reload broadcast.
        string path = ConfigPath;
        string tmp  = path + ".tmp";
        try
        {
            File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
            File.Move(tmp, path, overwrite: true);
        }
        catch (Exception ex)
        {
            // A failed write must be said out loud, or Apply lights up, the bar
            // slides away, and nothing has been written. Deliberately no
            // snapshot either, so the app still knows the settings are unsaved.
            DiagnosticsLog.Append("CK0603", DiagSeverity.Critical,
                "Settings could not be saved",
                $"config.json could not be written — {ex.Message}. The settings in "
                + "this window are still unsaved.");
            return;
        }
        m.TakeSnapshot();
        NotifyCore();
    }

    // -------------------------------------------------------------------
    // Apply/reload signal. The core registers the same message in
    // App::Run and re-reads config.json when it arrives, so animations,
    // motion blur, max windows, background opacity and the trigger
    // filters apply without restarting CKFlip3D.
    // -------------------------------------------------------------------

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern uint RegisterWindowMessageW(string name);

    [DllImport("user32.dll")]
    private static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

    private static readonly IntPtr HWND_BROADCAST = new(0xFFFF);

    public static void NotifyCore()
    {
        uint msg = RegisterWindowMessageW("CKFLIP3D_CONFIG_RELOAD");
        if (msg != 0)
            PostMessageW(HWND_BROADCAST, msg, IntPtr.Zero, IntPtr.Zero);
    }

    /// <summary>
    /// Asks a running core to hold cascade activation off (or to let go).
    ///
    /// The touchpad-activity panel reads the same pad the core is listening
    /// to, so practising the opening diagonal would fling the real cascade
    /// over this window — and the activation hotkey is just as reachable
    /// while the panel has focus. The core's hold EXPIRES on its own, so this
    /// is re-sent periodically while the panel is live: a settings window that
    /// dies mid-preview re-arms the switcher within a couple of seconds
    /// instead of disabling it for good.
    /// </summary>
    public static void SuspendCoreActivation(bool on)
    {
        uint msg = RegisterWindowMessageW("CKFLIP3D_INPUT_SUSPEND");
        if (msg != 0)
            PostMessageW(HWND_BROADCAST, msg, new IntPtr(on ? 1 : 0), IntPtr.Zero);
    }

    /// <summary>
    /// Asks a running core to restart itself (sent after Apply so every
    /// change — including ones only read at startup — fully lands). The
    /// core is elevated and this process is not, so killing/relaunching it
    /// from here is impossible; the core allows this registered message
    /// through the UIPI filter and relaunches itself.
    /// </summary>
    public static void RestartCore()
    {
        if (!CoreLocator.IsCoreRunning()) return;
        uint msg = RegisterWindowMessageW("CKFLIP3D_RESTART");
        if (msg != 0)
            PostMessageW(HWND_BROADCAST, msg, IntPtr.Zero, IntPtr.Zero);
    }

    // ---- helpers ---------------------------------------------------------

    private static bool GetBool(JsonElement r, string key, bool def) =>
        r.TryGetProperty(key, out var v) && (v.ValueKind == JsonValueKind.True || v.ValueKind == JsonValueKind.False)
            ? v.GetBoolean() : def;

    private static int GetInt(JsonElement r, string key, int def) =>
        r.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out int i)
            ? i : def;

    /// <summary>
    /// The main key of a binding, or "" when that key cannot be a navigation
    /// entry (a bare modifier or a mouse button — those keep their own
    /// re-press cycling in the hook). Mirrors NavTokenOfBinding in
    /// core/Config.cpp.
    /// </summary>
    private static string NavTokenOfBinding(string combo)
    {
        string token = (combo ?? "").Split('+', StringSplitOptions.TrimEntries)[^1];
        return HotkeyService.TokenToVk(token) == 0 ? "" : token;
    }

    /// <summary>Switch every entry of a binding list off, keeping the entries.</summary>
    private static string ParkAll(string list) =>
        SettingsModel.FormatBindings(
            SettingsModel.ParseBindings(list).Select(k => k with { Enabled = false }));

    /// <summary>
    /// The default stands in for a MISSING key only. An empty string in the
    /// file is a value in its own right and is returned as one — which is what
    /// lets "no navigation keys at all" survive a reopen.
    /// </summary>
    private static string GetString(JsonElement r, string key, string def) =>
        r.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() ?? def : def;

    private static void AppendBool(StringBuilder sb, string key, bool v) =>
        sb.Append("  \"").Append(key).Append("\": ").Append(v ? "true" : "false").Append(",\n");

    private static void AppendInt(StringBuilder sb, string key, int v) =>
        sb.Append("  \"").Append(key).Append("\": ").Append(v.ToString(CultureInfo.InvariantCulture)).Append(",\n");

    private static void AppendString(StringBuilder sb, string key, string v) =>
        sb.Append("  \"").Append(key).Append("\": \"").Append(v.Replace("\\", "\\\\").Replace("\"", "\\\"")).Append("\",\n");
}
