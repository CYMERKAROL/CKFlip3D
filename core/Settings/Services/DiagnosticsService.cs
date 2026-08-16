using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using CKFlip3D.Settings.Interop;

namespace CKFlip3D.Settings.Services;

public sealed class DiagnosticsSnapshot
{
    public required string SettingsVersion { get; init; }
    public required string CoreExe { get; init; }
    public required string CoreProcess { get; init; }
    public required string CoreWindow { get; init; }
    public required string RuntimeStatusNote { get; init; }
    public required int MonitorCount { get; init; }
    public required string VirtualScreenRect { get; init; }
    public required string PrimaryMonitorRect { get; init; }
    public required string TaskbarInfo { get; init; }
    public required string ConfigPath { get; init; }
    public required List<MonitorEntry> Monitors { get; init; }

    // ---- Environment -------------------------------------------------------
    public required string WindowsVersion { get; init; }
    public required string ProcessInfo { get; init; }
    public required string DpiInfo { get; init; }

    // ---- Input -------------------------------------------------------------
    public required List<InputDeviceEntry> InputDevices { get; init; }
    public required string PointingSummary { get; init; }
    public required string TouchpadGestureState { get; init; }
    public required string TriggerSummary { get; init; }

    public string ToClipboardText()
    {
        var sb = new StringBuilder();
        sb.AppendLine("=== CKFlip3D Diagnostics ===");
        sb.AppendLine($"Generated:        {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        sb.AppendLine($"Settings version: {SettingsVersion}");
        sb.AppendLine($"Windows:          {WindowsVersion}");
        sb.AppendLine($"Settings process: {ProcessInfo}");
        sb.AppendLine($"Core executable:  {CoreExe}");
        sb.AppendLine($"Core process:     {CoreProcess}");
        sb.AppendLine($"Core window:      {CoreWindow}");
        sb.AppendLine($"D3D/WGC/hook:     {RuntimeStatusNote}");
        sb.AppendLine();
        sb.AppendLine($"Monitor count:    {MonitorCount}");
        sb.AppendLine($"Virtual screen:   {VirtualScreenRect}");
        sb.AppendLine($"Primary monitor:  {PrimaryMonitorRect}");
        sb.AppendLine($"DPI / scaling:    {DpiInfo}");
        foreach (var m in Monitors)
            sb.AppendLine($"  Monitor {m.Index + 1}: {m.DeviceName}  ({m.Left},{m.Top})-({m.Right},{m.Bottom})  " +
                          $"{m.Width}x{m.Height} {m.AspectLabel}{(m.IsPrimary ? "  [PRIMARY]" : "")}");
        sb.AppendLine($"Taskbars:         {TaskbarInfo}");
        sb.AppendLine();
        sb.AppendLine($"Pointing devices: {PointingSummary}");
        sb.AppendLine(InputDeviceService.ToText(InputDevices));
        sb.AppendLine($"Touchpad gesture: {TouchpadGestureState}");
        sb.AppendLine($"Triggers:         {TriggerSummary}");
        sb.AppendLine();
        sb.AppendLine($"Config path:      {ConfigPath}");
        return sb.ToString();
    }
}

/// <summary>
/// Gathers everything the Diagnostics page shows. Monitor/taskbar/config data
/// is live. The core's internal D3D/WGC/hook state is not exposed over any
/// IPC channel yet, so we report what is observable from outside: process
/// presence, exe location and whether its message window answers.
/// </summary>
public static class DiagnosticsService
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? className, string? windowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeoutW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam,
                                                     uint flags, uint timeoutMs, out IntPtr result);

    private const uint WM_NULL = 0;
    private const uint SMTO_ABORTIFHUNG = 0x0002;

    public static DiagnosticsSnapshot Collect()
    {
        var monitors = MonitorInterop.EnumerateMonitors();
        var primary = monitors.FirstOrDefault(m => m.IsPrimary);
        var (vx, vy, vw, vh) = MonitorInterop.GetVirtualScreenRect();
        var (taskbarPrimary, taskbarSecondary) = MonitorInterop.DetectTaskbars();

        bool coreRunning = CoreLocator.IsCoreRunning();
        string? coreExe = CoreLocator.FindCoreExe();

        // The core's hidden message window (core/app.cpp, class
        // CKFlip3D_MessageWindow) — a responsive ping means its message loop
        // (hotkeys, tray, config reload) is alive.
        string coreWindow = "not found";
        IntPtr msgWnd = FindWindowW("CKFlip3D_MessageWindow", null);
        if (msgWnd != IntPtr.Zero)
        {
            IntPtr ok = SendMessageTimeoutW(msgWnd, WM_NULL, IntPtr.Zero, IntPtr.Zero,
                                            SMTO_ABORTIFHUNG, 500, out _);
            coreWindow = ok != IntPtr.Zero ? "responding (message loop alive)" : "found but NOT responding";
        }

        string version = Assembly.GetExecutingAssembly().GetName().Version?.ToString(3) ?? "unknown";

        var inputDevices = InputDeviceService.Enumerate();
        int mice = inputDevices.Count(d => d.Kind == "Mouse");
        int keyboards = inputDevices.Count(d => d.Kind == "Keyboard");
        int pads = inputDevices.Count(d => d.Kind == "Precision touchpad");

        return new DiagnosticsSnapshot
        {
            WindowsVersion = DescribeWindows(),
            ProcessInfo = $"{(Environment.Is64BitProcess ? "64-bit" : "32-bit")}, "
                        + $".NET {Environment.Version}, session {Process.GetCurrentProcess().SessionId}",
            DpiInfo = DescribeDpi(),
            InputDevices = inputDevices,
            PointingSummary = $"{pads} precision touchpad(s), {mice} mouse/mice, "
                            + $"{keyboards} keyboard(s)",
            TouchpadGestureState = DescribeActivationGesture(),
            TriggerSummary = DescribeTriggers(),
            SettingsVersion = version,
            CoreExe = coreExe ?? "not found",
            CoreProcess = coreRunning ? "CKFlip3D.exe — running" : "CKFlip3D.exe — not running",
            CoreWindow = coreWindow,
            RuntimeStatusNote = coreRunning
                ? "Internal D3D / WGC / hook state is not exposed by the core over IPC yet — " +
                  "only process and message-window health can be shown."
                : "Core not running — no runtime state to report.",
            MonitorCount = monitors.Count,
            VirtualScreenRect = $"({vx}, {vy})  {vw} x {vh}",
            PrimaryMonitorRect = primary != null
                ? $"({primary.Left}, {primary.Top})  {primary.Width} x {primary.Height}"
                : "not found",
            TaskbarInfo = taskbarPrimary
                ? $"Primary detected{(taskbarSecondary > 0 ? $", {taskbarSecondary} secondary" : ", no secondary")}"
                : "Primary NOT detected",
            ConfigPath = ConfigService.ConfigPath,
            Monitors = monitors,
        };
    }

    // ---- Environment helpers -----------------------------------------------

    private static string DescribeWindows()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                @"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
            string product = key?.GetValue("ProductName") as string ?? "Windows";
            string display = key?.GetValue("DisplayVersion") as string ?? "";
            string build = key?.GetValue("CurrentBuildNumber") as string ?? "";
            object? ubr = key?.GetValue("UBR");
            // Win11 still reports "Windows 10 Pro" in ProductName; the build
            // number is what actually matters for the taskbar-capture quirks.
            string label = build.Length > 0 && int.TryParse(build, out int b) && b >= 22000
                ? product.Replace("Windows 10", "Windows 11") : product;
            return $"{label} {display} (build {build}{(ubr != null ? $".{ubr}" : "")})".Trim();
        }
        catch
        {
            return Environment.OSVersion.VersionString;
        }
    }

    private static string DescribeDpi()
    {
        try
        {
            var src = System.Windows.PresentationSource.FromVisual(
                System.Windows.Application.Current?.MainWindow!);
            double scale = src?.CompositionTarget?.TransformToDevice.M11 ?? 1.0;
            return $"{scale * 100:0}% ({scale * 96:0} DPI)";
        }
        catch
        {
            return "unknown";
        }
    }

    /// <summary>
    /// The opening stroke, spelled out. Diagonals on purpose: Windows only
    /// claims the four cardinal slide directions, so nothing of the user's
    /// touchpad configuration has to be touched for this to work.
    /// </summary>
    private static string DescribeActivationGesture() =>
        App.Settings.TouchpadActivateGesture switch
        {
            1 => "Two fingers ↘ (top-left → bottom-right) — Windows' own gestures untouched",
            2 => "Two fingers ↙ (top-right → bottom-left) — Windows' own gestures untouched",
            3 => "Four fingers ↘ (top-left → bottom-right) — Windows' own gestures untouched",
            4 => "Four fingers ↙ (top-right → bottom-left) — Windows' own gestures untouched",
            _ => "No touchpad opening gesture",
        };

    private static string DescribeTriggers()
    {
        var s = App.Settings;
        string touchpad = !s.TouchpadNav ? "touchpad off"
            : $"touchpad: {s.TouchpadCycleFingers}-finger cycle, "
              + $"activation {s.TouchpadActivateGesture}, commit {s.TouchpadCommitGesture}, "
              + $"sensitivity {s.TouchpadSensitivity}%, smoothing {s.TouchpadSmoothing}%";
        return $"hotkey {s.ActivationHotkey}"
             + $"{(s.HotkeyToggleMode ? " (toggle)" : "")}, "
             + $"wheel {(s.MouseWheelCycle ? "on" : "off")}, "
             + $"arrows {(s.KeyboardNav ? "on" : "off")}, "
             + $"window snap {(s.WindowSnap ? "on" : "off")}, {touchpad}";
    }
}
