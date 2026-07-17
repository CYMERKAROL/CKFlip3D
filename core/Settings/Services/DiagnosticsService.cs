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

    public string ToClipboardText()
    {
        var sb = new StringBuilder();
        sb.AppendLine("=== CKFlip3D Diagnostics ===");
        sb.AppendLine($"Generated:        {DateTime.Now:yyyy-MM-dd HH:mm:ss}");
        sb.AppendLine($"Settings version: {SettingsVersion}");
        sb.AppendLine($"Core executable:  {CoreExe}");
        sb.AppendLine($"Core process:     {CoreProcess}");
        sb.AppendLine($"Core window:      {CoreWindow}");
        sb.AppendLine($"D3D/WGC/hook:     {RuntimeStatusNote}");
        sb.AppendLine($"Monitor count:    {MonitorCount}");
        sb.AppendLine($"Virtual screen:   {VirtualScreenRect}");
        sb.AppendLine($"Primary monitor:  {PrimaryMonitorRect}");
        foreach (var m in Monitors)
            sb.AppendLine($"  Monitor {m.Index + 1}: {m.DeviceName}  ({m.Left},{m.Top})-({m.Right},{m.Bottom})  " +
                          $"{m.Width}x{m.Height} {m.AspectLabel}{(m.IsPrimary ? "  [PRIMARY]" : "")}");
        sb.AppendLine($"Taskbars:         {TaskbarInfo}");
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

        return new DiagnosticsSnapshot
        {
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
}
