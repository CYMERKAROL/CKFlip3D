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
/// (visualPreset, cascadeMonitor, secondaryTaskbarMode) are ignored by it and
/// kept for forward compatibility.
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
                m.MotionBlur        = GetBool(root, "motionBlur", m.MotionBlur);
                m.LivePreview       = GetBool(root, "livePreview", m.LivePreview);
                m.LiveBackground    = GetBool(root, "liveBackground", m.LiveBackground);
                m.VsyncLivePreview  = GetBool(root, "vsyncLivePreview", m.VsyncLivePreview);
                m.TaskbarLivePreview = GetBool(root, "taskbarLivePreview", m.TaskbarLivePreview);
                m.TaskbarPreview    = GetBool(root, "taskbarPreview", m.TaskbarPreview);
                m.MaxWindows        = (uint)GetInt(root, "maxWindows", (int)m.MaxWindows);
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
                m.KeyboardNav       = GetBool(root, "keyboardNav", m.KeyboardNav);
                m.IgnoredApps       = GetString(root, "ignoredApps", m.IgnoredApps);
                m.ExcludedApps      = GetString(root, "excludedApps", m.ExcludedApps);
                m.ActivationHotkey  = GetString(root, "activationHotkey", m.ActivationHotkey);
                m.HotkeyToggleMode  = GetBool(root, "hotkeyToggleMode", m.HotkeyToggleMode);
                m.ShowDebugInfo     = GetBool(root, "showDebugInfo", m.ShowDebugInfo);

                // Forward-compatible keys (not consumed by the core yet)
                m.AppTheme             = GetInt(root, "appTheme", m.AppTheme);
                m.VisualPreset         = GetInt(root, "visualPreset", m.VisualPreset);
                m.CascadeMonitor       = GetInt(root, "cascadeMonitor", m.CascadeMonitor);
                m.SecondaryTaskbarMode = GetInt(root, "secondaryTaskbarMode", m.SecondaryTaskbarMode);
            }
        }
        catch
        {
            // Corrupt config: fall back to defaults rather than crash the settings UI.
        }

        // Startup state lives in the registry, not in config.json.
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
        AppendBool(sb, "motionBlur", m.MotionBlur);
        AppendBool(sb, "livePreview", m.LivePreview);
        AppendBool(sb, "liveBackground", m.LiveBackground);
        AppendBool(sb, "vsyncLivePreview", m.VsyncLivePreview);
        AppendBool(sb, "taskbarLivePreview", m.TaskbarLivePreview);
        AppendBool(sb, "taskbarPreview", m.TaskbarPreview);
        AppendInt(sb, "maxWindows", (int)m.MaxWindows);
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
        AppendBool(sb, "keyboardNav", m.KeyboardNav);
        AppendString(sb, "ignoredApps", m.IgnoredApps);
        AppendString(sb, "excludedApps", m.ExcludedApps);
        AppendString(sb, "activationHotkey", m.ActivationHotkey);
        AppendBool(sb, "hotkeyToggleMode", m.HotkeyToggleMode);
        AppendBool(sb, "showDebugInfo", m.ShowDebugInfo);

        // Forward-compatible keys (ignored by the core until wired up).
        AppendInt(sb, "appTheme", m.AppTheme);
        AppendInt(sb, "visualPreset", m.VisualPreset);
        AppendInt(sb, "cascadeMonitor", m.CascadeMonitor);
        AppendInt(sb, "secondaryTaskbarMode", m.SecondaryTaskbarMode);

        // Trim trailing ",\n" -> "\n"
        if (sb.Length >= 2 && sb[^2] == ',') sb.Remove(sb.Length - 2, 1);
        sb.Append("}\n");

        // Temp-write + atomic rename so the elevated core never observes a
        // half-written config.json on the reload broadcast.
        string path = ConfigPath;
        string tmp  = path + ".tmp";
        File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
        File.Move(tmp, path, overwrite: true);
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
