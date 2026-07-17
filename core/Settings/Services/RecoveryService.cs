using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace CKFlip3D.Settings.Services;

/// <summary>
/// Recovery actions implemented directly against Win32 so they work even when
/// the core process is dead or hung — no IPC channel required. They mirror the
/// core's own teardown logic (capture/windowcloaker.cpp ForceUncloakEverything,
/// flipcontroller ShowRealTaskbar / RestoreDesktopIcons).
/// </summary>
public static class RecoveryService
{
    private const int DWMWA_CLOAK = 13;
    private const int SW_SHOW = 5;

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(IntPtr hwnd, int attr, out int value, int size);

    private const int DWMWA_CLOAKED = 14;

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hwnd, int cmd);

    [DllImport("user32.dll")]
    private static extern bool IsWindowEnabled(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool EnableWindow(IntPtr hwnd, bool enable);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? className, string? windowName);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowExW(IntPtr parent, IntPtr childAfter, string? className, string? windowName);

    [DllImport("user32.dll")]
    private static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);

    private const uint WM_CLOSE = 0x0010;

    /// <summary>
    /// Stops every CKFlip3D.exe process. The core runs elevated while this
    /// app does not, so Process.Kill is denied — the graceful path posts
    /// WM_CLOSE to the core's message window (allowed through its UIPI
    /// filter); Kill remains as a fallback for non-elevated cores.
    /// </summary>
    private static int StopCoreProcesses()
    {
        var procs = Process.GetProcessesByName(CoreLocator.ProcessName);
        if (procs.Length == 0) return 0;

        IntPtr msgWnd = FindWindowW("CKFlip3D_MessageWindow", null);
        if (msgWnd != IntPtr.Zero)
            PostMessageW(msgWnd, WM_CLOSE, IntPtr.Zero, IntPtr.Zero);

        int stopped = 0;
        foreach (var p in procs)
        {
            try
            {
                if (p.WaitForExit(3000)) { stopped++; continue; }
                p.Kill();
                p.WaitForExit(2000);
                stopped++;
            }
            catch { /* access denied / already gone */ }
            finally { p.Dispose(); }
        }
        return stopped;
    }

    /// <summary>
    /// Clears the DWM cloak attribute on every top-level window. Returns the
    /// number of windows that were actually cloaked-by-app before the call.
    /// </summary>
    public static int ForceUncloakAllWindows()
    {
        int wasCloaked = 0;
        EnumWindows((hwnd, _) =>
        {
            if (!IsWindow(hwnd)) return true;

            // Count only app-cloaked windows (DWM_CLOAKED_APP = 1) so the
            // status message reflects real repairs, not shell internals.
            if (DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, out int cloaked, sizeof(int)) == 0
                && (cloaked & 1) != 0)
                wasCloaked++;

            int off = 0;
            DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, ref off, sizeof(int));
            return true;
        }, IntPtr.Zero);
        return wasCloaked;
    }

    /// <summary>
    /// Re-shows and re-enables the primary and all secondary taskbars.
    /// A crash mid-flip can leave a bar hidden (classic hide path) or
    /// disabled (hold-behind-overlay path used by autohide continuity and
    /// taskbar live preview). Returns how many bars needed repair.
    /// </summary>
    public static int RestoreTaskbars()
    {
        int restored = 0;

        static bool Repair(IntPtr bar)
        {
            bool repaired = false;
            if (!IsWindowVisible(bar)) { ShowWindow(bar, SW_SHOW); repaired = true; }
            if (!IsWindowEnabled(bar)) { EnableWindow(bar, true); repaired = true; }
            return repaired;
        }

        IntPtr primary = FindWindowW("Shell_TrayWnd", null);
        if (primary != IntPtr.Zero && Repair(primary))
            restored++;

        IntPtr h = IntPtr.Zero;
        while ((h = FindWindowExW(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero)
        {
            if (Repair(h))
                restored++;
        }
        return restored;
    }

    /// <summary>
    /// True when the user keeps desktop icons hidden on purpose (desktop
    /// context menu → View → "Show desktop icons" unchecked). Restoring
    /// icons must never override that preference — a flip only ever hides
    /// the list view temporarily, it does not touch this setting.
    /// </summary>
    private static bool UserPrefersIconsHidden()
    {
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(
                @"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced");
            return key?.GetValue("HideIcons") is int v && v != 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Re-shows the desktop icon list view if a flip left it hidden —
    /// unless the user hides icons by preference (then hidden IS the
    /// correct state and showing the list view would "switch icons on").
    /// </summary>
    public static bool RestoreDesktopIcons()
    {
        if (UserPrefersIconsHidden()) return false;

        // SHELLDLL_DefView can be a child of Progman or a WorkerW
        // (same lookup the core uses in HideDesktopIcons).
        IntPtr defView = IntPtr.Zero;
        IntPtr progman = FindWindowW("Progman", null);
        if (progman != IntPtr.Zero)
            defView = FindWindowExW(progman, IntPtr.Zero, "SHELLDLL_DefView", null);

        if (defView == IntPtr.Zero)
        {
            IntPtr worker = IntPtr.Zero;
            while ((worker = FindWindowExW(IntPtr.Zero, worker, "WorkerW", null)) != IntPtr.Zero)
            {
                defView = FindWindowExW(worker, IntPtr.Zero, "SHELLDLL_DefView", null);
                if (defView != IntPtr.Zero) break;
            }
        }
        if (defView == IntPtr.Zero) return false;

        IntPtr listView = FindWindowExW(defView, IntPtr.Zero, "SysListView32", null);
        if (listView == IntPtr.Zero) return false;

        bool wasHidden = !IsWindowVisible(listView);
        if (wasHidden) ShowWindow(listView, SW_SHOW);
        return wasHidden;
    }

    /// <summary>Uncloak + taskbars + desktop icons in one go.</summary>
    public static string PanicRestore()
    {
        int uncloaked = ForceUncloakAllWindows();
        int taskbars = RestoreTaskbars();
        bool icons = RestoreDesktopIcons();

        var sb = new StringBuilder("Panic restore done: ");
        sb.Append(uncloaked > 0 ? $"uncloaked {uncloaked} window(s), " : "no cloaked windows, ");
        sb.Append(taskbars > 0 ? $"restored {taskbars} taskbar(s), " : "taskbars were visible, ");
        sb.Append(icons ? "desktop icons restored." : "desktop icons were visible.");
        return sb.ToString();
    }

    /// <summary>Stops every CKFlip3D.exe process, then repairs the desktop state.</summary>
    public static string ForceQuitCore()
    {
        int killed = 0;
        try { killed = StopCoreProcesses(); }
        catch { }

        // A kill mid-flip leaves cloaked windows / hidden taskbars behind —
        // always follow up with the desktop repair.
        string repair = PanicRestore();
        return killed > 0
            ? $"Terminated {killed} CKFlip3D process(es). {repair}"
            : $"CKFlip3D was not running. {repair}";
    }

    /// <summary>
    /// Restarts the core with --safe-mode (animations and motion blur forced
    /// off in core/app.cpp). Returns a status string; null exe means failure.
    /// </summary>
    public static string LaunchSafeMode()
    {
        string? exe = CoreLocator.FindCoreExe();
        if (exe == null)
            return "CKFlip3D.exe could not be located — Safe Mode launch aborted.";

        int killed = 0;
        try { killed = StopCoreProcesses(); }
        catch { }

        try
        {
            Process.Start(new ProcessStartInfo(exe, "--safe-mode") { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            return $"Safe Mode launch failed: {ex.Message}";
        }

        return killed > 0
            ? "Restarted CKFlip3D in Safe Mode (previous instance terminated)."
            : "CKFlip3D started in Safe Mode.";
    }
}
