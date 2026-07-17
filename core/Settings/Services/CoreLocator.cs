using System.Diagnostics;
using System.IO;

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
            foreach (var p in Process.GetProcessesByName(ProcessName))
            {
                try
                {
                    string? path = p.MainModule?.FileName;
                    if (!string.IsNullOrEmpty(path) && File.Exists(path)) return path;
                }
                catch { /* access denied — skip */ }
            }
        }
        catch { }

        return null;
    }

    public static bool IsCoreRunning()
    {
        try { return Process.GetProcessesByName(ProcessName).Length > 0; }
        catch { return false; }
    }
}
