using System.IO;

namespace CKFlip3D.Installer.Engine;

/// <summary>Shared constants and the user's choices from the Options page.</summary>
public sealed class InstallContext
{
    public const string AppName = "CKFlip3D";
    public const string AppVersion = "1.2.0";
    public const string Publisher = "CYMERKAROL";

    /// <summary>Apps & Features registry key (HKLM).</summary>
    public const string UninstallKeyPath =
        @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\CKFlip3D";

    public const string UninstallerFileName = "Uninstall CKFlip3D.exe";
    public const string ManifestFileName = "install.manifest";
    /// <summary>Manifest marker: the install dir itself was created by setup
    /// (and may therefore be removed entirely on uninstall).</summary>
    public const string DirCreatedMarker = "# dir-created";
    public const string CoreExeName = "CKFlip3D.exe";
    public const string SettingsExeName = "CKFlip3D.Settings.exe";
    public const string IconFileName = "CKFlip3D.ico";
    public const string UninstallIconFileName = "CKFlip3D.Uninstall.ico";

    /// <summary>Names of processes that must not be running while files are replaced.</summary>
    public static readonly string[] ProcessNames = { "CKFlip3D", "CKFlip3D.Settings" };

    public static string DefaultInstallDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), AppName);

    public static string StartMenuDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonPrograms), AppName);

    public static string CommonDesktopShortcut => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory), AppName + ".lnk");

    public static string AppDataDir => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), AppName);

    // ---- user choices (Options page) ----
    public string InstallDir { get; set; } = DefaultInstallDir;
    public bool CreateStartMenuFolder { get; set; } = true;
    public bool CreateDesktopShortcut { get; set; } = true;

    /// <summary>The chosen folder already holds a CKFlip3D install (update flow).</summary>
    public bool IsUpdate { get; set; }

    /// <summary>Setup created the install dir (it did not exist before).</summary>
    public bool InstallDirCreated { get; set; }

    /// <summary>True when <paramref name="dir"/> looks like an existing CKFlip3D installation.</summary>
    public static bool IsExistingInstall(string dir)
    {
        try
        {
            return File.Exists(Path.Combine(dir, CoreExeName))
                || File.Exists(Path.Combine(dir, ManifestFileName));
        }
        catch
        {
            return false;
        }
    }

    // ---- results ----
    public string CoreExePath => Path.Combine(InstallDir, CoreExeName);
    public string SettingsExePath => Path.Combine(InstallDir, SettingsExeName);
    public string UninstallerPath => Path.Combine(InstallDir, UninstallerFileName);

    /// <summary>Set when the .NET runtime bootstrap was skipped (offline etc.).</summary>
    public string? RuntimeWarning { get; set; }
}

/// <summary>One progress tick for the UI status bar.</summary>
public readonly record struct InstallProgress(double Percent, string Status, string Detail = "");
