// ---------------------------------------------------------------------------
// Finding CKFlip3D.exe.  The Settings app has to work from an install folder
// and from a development build tree alike, so the answer is looked up rather
// than assumed, with a running core as the last resort.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
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

    /// <summary>
    /// CKFlip3D.Launch.exe — the launch shortcut's target, which ships beside
    /// the core and the settings app. Same search as <see cref="FindCoreExe"/>
    /// minus the running-process fallback: this one is never running by the
    /// time anybody looks for it (it exits in milliseconds). Returns null when
    /// it is not there — a build from before 1.6, or a partial install.
    /// </summary>
    public static string? FindLauncherExe()
    {
        const string name = "CKFlip3D.Launch.exe";

        string sideBySide = Path.Combine(AppContext.BaseDirectory, name);
        if (File.Exists(sideBySide)) return sideBySide;

        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 6 && dir != null; i++, dir = dir.Parent)
        {
            string candidate = Path.Combine(dir.FullName, name);
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? className, string? windowName);

    /// <summary>The core's hidden top-level window (core/app.cpp kWindowClass).</summary>
    private const string CoreWindowClass = "CKFlip3D_MessageWindow";

    /// <summary>
    /// Is the switcher alive?  Asked on a timer for as long as the settings
    /// window lives, so the cost of asking matters.
    ///
    /// The fast path is a single FindWindow for the core's window class: no
    /// allocation, no handles, and a bounded lookup rather than a snapshot of
    /// every process on the machine. UIPI does not interfere, since it filters
    /// MESSAGES to a higher-integrity window rather than the ability to find
    /// one, and it is the same window Apply has to reach, so a "yes" here
    /// answers the question the callers actually have.
    ///
    /// Process enumeration is the fallback, for the gap between the process
    /// starting and its window existing. Its results MUST be disposed: every
    /// Process object holds a native handle until finalisation, and this runs
    /// several times a minute.
    ///
    /// Inherently a snapshot: the core can exit immediately after this returns
    /// true. Harmless for every caller here, and the watch that drives it is
    /// edge-triggered, so the next tick corrects it.
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
