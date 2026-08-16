using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace CKFlip3D.Settings.Services;

/// <summary>Finds the main CKFlip3D.exe for startup registration and Safe Mode launch.</summary>
public static class CoreLocator
{
    public const string ProcessName = "CKFlip3D";

    /// <summary>
    /// Resolution order: next to this Settings exe (deployed layout),
    /// the repo root during development (Settings builds to
    /// core\Settings\bin\&lt;cfg&gt;\net8.0-windows), then the path of a
    /// running core process. Returns null when not found.
    /// </summary>
    public static string? FindCoreExe()
    {
        string baseDir = AppContext.BaseDirectory;

        string sideBySide = Path.Combine(baseDir, "CKFlip3D.exe");
        if (File.Exists(sideBySide)) return sideBySide;

        // Dev tree: walk up a few levels looking for CKFlip3D.exe.
        var dir = new DirectoryInfo(baseDir);
        for (int i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, "CKFlip3D.exe");
            if (File.Exists(candidate)) return candidate;
        }

        try
        {
            // Disposed for the same reason as in IsCoreRunning: each Process
            // holds a native handle, and MainModule on an elevated core throws
            // access-denied from here, so the loop usually falls through having
            // allocated one per running core.
            var procs = Process.GetProcessesByName(ProcessName);
            try
            {
                foreach (var p in procs)
                {
                    try
                    {
                        string? path = p.MainModule?.FileName;
                        if (!string.IsNullOrEmpty(path) && File.Exists(path)) return path;
                    }
                    catch { /* access denied — skip */ }
                }
            }
            finally { foreach (var p in procs) p.Dispose(); }
        }
        catch { }

        return null;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? className, string? windowName);

    /// <summary>The core's hidden top-level window (core/app.cpp kWindowClass).</summary>
    private const string CoreWindowClass = "CKFlip3D_MessageWindow";

    /// <summary>
    /// Is the switcher alive?
    ///
    /// This used to be asked once, as the settings window opened. It is now
    /// also asked on a timer for as long as that window lives, so what it
    /// costs stopped being irrelevant.
    ///
    /// The fast path is a single FindWindow for the core's window class: no
    /// allocation, no handles, and a bounded lookup rather than a snapshot of
    /// every process on the machine. UIPI does not interfere — it filters
    /// MESSAGES to a higher-integrity window, not the ability to find one — and
    /// it is the same window Apply has to reach, so a "yes" here is a yes to
    /// the question the callers actually care about.
    ///
    /// Process enumeration stays as the fallback, for the gap between the
    /// process starting and its window existing. Its results are now DISPOSED:
    /// every Process object holds a native handle until finalisation, so the
    /// old version leaked one per running core per call — harmless at once per
    /// window, not harmless several times a minute.
    ///
    /// Inherently a snapshot: the core can exit immediately after this returns
    /// true. That is fine for every caller here — the only consequence is a log
    /// entry, and the watch that drives it is edge-triggered, so the next tick
    /// corrects it.
    /// </summary>
    public static bool IsCoreRunning()
    {
        if (FindWindowW(CoreWindowClass, null) != IntPtr.Zero) return true;

        try
        {
            var procs = Process.GetProcessesByName(ProcessName);
            try { return procs.Length > 0; }
            finally { foreach (var p in procs) p.Dispose(); }
        }
        catch { return false; }
    }
}
