using System.Diagnostics;
using System.IO;
using Microsoft.Win32;

namespace CKFlip3D.Installer.Engine;

/// <summary>
/// Reverses an installation: stops the app, removes the scheduled startup
/// task, deletes the files listed in install.manifest (never a blind wipe of
/// a folder the user owned), removes shortcuts and the Apps &amp; Features
/// entry, optionally clears %APPDATA%\CKFlip3D and finally removes the
/// install folder itself — but only when the manifest records that setup
/// created that folder (see <see cref="InstallContext.DirCreatedMarker"/>).
///
/// This engine normally runs from a %TEMP% copy of the uninstaller (see
/// App.RelaunchFromTemp), so nothing in the install dir is locked and the
/// folder can be removed directly; the temp copy then deletes itself via a
/// detached cmd. If the bootstrap failed and we still run from the install
/// dir, the same cmd fallback removes the locked exe and the folder.
/// </summary>
public sealed class UninstallEngine
{
    /// <summary>Folder this uninstaller was installed to.</summary>
    public static string ResolveInstallDir()
    {
        // Prefer the registry (survives a moved exe), fall back to our folder.
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(InstallContext.UninstallKeyPath);
            if (key?.GetValue("InstallLocation") is string loc && Directory.Exists(loc))
                return loc;
        }
        catch { }
        return Path.GetDirectoryName(Environment.ProcessPath) ?? AppContext.BaseDirectory;
    }

    public async Task RunAsync(string installDir, bool removeUserData,
                               IProgress<InstallProgress> progress, CancellationToken ct)
    {
        await Task.Run(() =>
        {
            installDir = Path.GetFullPath(installDir);
            var (files, dirCreatedBySetup) = ReadManifest(installDir);

            // ---- 1. Stop running instances (0–10%) -----------------------
            progress.Report(new InstallProgress(0, "Stopping CKFlip3D…"));
            foreach (string name in InstallContext.ProcessNames)
            {
                foreach (var proc in Process.GetProcessesByName(name))
                {
                    try
                    {
                        progress.Report(new InstallProgress(4, "Stopping CKFlip3D…", name));
                        proc.CloseMainWindow();
                        if (!proc.WaitForExit(3000)) proc.Kill();
                        proc.WaitForExit(5000);
                    }
                    catch { }
                    finally { proc.Dispose(); }
                }
            }
            ct.ThrowIfCancellationRequested();

            // ---- 2. Startup task (10–18%) --------------------------------
            progress.Report(new InstallProgress(10, "Removing startup entry…",
                "Scheduled task CKFlip3D"));
            RemoveScheduledTask();
            RemoveRunKeyEntries();

            // ---- 3. Files from the manifest (18–66%) ---------------------
            for (int i = 0; i < files.Count; i++)
            {
                ct.ThrowIfCancellationRequested();
                string relative = files[i];
                progress.Report(new InstallProgress(
                    18 + (i + 1) * 48.0 / Math.Max(1, files.Count), "Removing files…", relative));
                TryDeleteFile(Path.Combine(installDir, relative));
            }
            TryDeleteFile(Path.Combine(installDir, InstallContext.ManifestFileName));

            // ---- 4. Shortcuts (66–76%) -----------------------------------
            progress.Report(new InstallProgress(66, "Removing shortcuts…", "Start Menu / Desktop"));
            TryDeleteFile(InstallContext.CommonDesktopShortcut);
            TryDeleteDirectory(InstallContext.StartMenuDir, recursive: true);

            // ---- 5. Registry (76–84%) ------------------------------------
            progress.Report(new InstallProgress(76, "Removing registry entries…",
                "Apps & Features entry"));
            try
            {
                Registry.LocalMachine.DeleteSubKeyTree(
                    InstallContext.UninstallKeyPath, throwOnMissingSubKey: false);
            }
            catch { }

            // ---- 6. User data (84–90%) -----------------------------------
            if (removeUserData)
            {
                progress.Report(new InstallProgress(84, "Removing settings…",
                    InstallContext.AppDataDir));
                TryDeleteDirectory(InstallContext.AppDataDir, recursive: true);
            }

            // ---- 7. Install folder + self-delete (90–100%) ---------------
            progress.Report(new InstallProgress(90, "Removing application folder…", installDir));
            bool runningInsideInstallDir = IsRunningInside(installDir);

            if (dirCreatedBySetup && IsSafeToRemove(installDir) && !runningInsideInstallDir)
            {
                // Setup created this folder — remove it entirely. Running
                // from %TEMP%, so nothing inside is our own locked image.
                TryDeleteDirectory(installDir, recursive: true);
            }
            else
            {
                // The user's own folder (or a degraded in-place run): only
                // our files were deleted above; sweep now-empty subdirs.
                CleanupEmptySubdirectories(installDir);
            }

            progress.Report(new InstallProgress(96, "Cleaning up…",
                "Removing uninstaller"));
            ScheduleSelfDelete(runningInsideInstallDir && dirCreatedBySetup ? installDir : null);

            progress.Report(new InstallProgress(100, "Uninstall complete", ""));
        }, ct);
    }

    // =====================================================================

    private static (List<string> Files, bool DirCreated) ReadManifest(string installDir)
    {
        string path = Path.Combine(installDir, InstallContext.ManifestFileName);
        var list = new List<string>();
        bool dirCreated = false;
        try
        {
            if (File.Exists(path))
            {
                foreach (string raw in File.ReadAllLines(path))
                {
                    string line = raw.Trim();
                    if (line.Length == 0) continue;
                    if (line.Equals(InstallContext.DirCreatedMarker, StringComparison.OrdinalIgnoreCase))
                    {
                        dirCreated = true;
                        continue;
                    }
                    if (line.StartsWith('#')) continue;
                    // Never follow entries that point outside the install dir.
                    string full = Path.GetFullPath(Path.Combine(installDir, line));
                    if (full.StartsWith(installDir.TrimEnd('\\') + '\\', StringComparison.OrdinalIgnoreCase))
                        list.Add(line);
                }
            }
        }
        catch { }

        if (list.Count == 0)
        {
            // Manifest lost: fall back to the well-known application files only.
            list.AddRange(new[]
            {
                InstallContext.CoreExeName,
                InstallContext.SettingsExeName,
                "CKFlip3D.Settings.dll",
                "CKFlip3D.Settings.deps.json",
                "CKFlip3D.Settings.runtimeconfig.json",
                "CKFlip3D.Settings.pdb",
                InstallContext.IconFileName,
                InstallContext.UninstallerFileName,
            });
        }
        return (list, dirCreated);
    }

    /// <summary>Refuse to recursively delete anything that isn't a plain app folder.</summary>
    private static bool IsSafeToRemove(string dir)
    {
        try
        {
            string full = Path.GetFullPath(dir).TrimEnd('\\');
            string root = (Path.GetPathRoot(full) ?? "").TrimEnd('\\');
            if (full.Length == 0 || string.Equals(full, root, StringComparison.OrdinalIgnoreCase))
                return false;

            foreach (var special in new[]
            {
                Environment.SpecialFolder.ProgramFiles,
                Environment.SpecialFolder.ProgramFilesX86,
                Environment.SpecialFolder.Windows,
                Environment.SpecialFolder.UserProfile,
                Environment.SpecialFolder.MyDocuments,
                Environment.SpecialFolder.DesktopDirectory,
            })
            {
                if (string.Equals(full, Environment.GetFolderPath(special),
                        StringComparison.OrdinalIgnoreCase))
                    return false;
            }
            return true;
        }
        catch
        {
            return false;
        }
    }

    private static bool IsRunningInside(string dir)
    {
        string? self = Environment.ProcessPath;
        if (self == null) return false;
        return Path.GetFullPath(self)
            .StartsWith(Path.GetFullPath(dir).TrimEnd('\\') + '\\', StringComparison.OrdinalIgnoreCase);
    }

    private static void RemoveScheduledTask()
    {
        try
        {
            var psi = new ProcessStartInfo("schtasks.exe", "/Delete /F /TN \"CKFlip3D\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var p = Process.Start(psi);
            p?.StandardOutput.ReadToEnd();
            p?.StandardError.ReadToEnd();
            p?.WaitForExit(10000);
        }
        catch { }
    }

    private static void RemoveRunKeyEntries()
    {
        const string runKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Run";
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(runKey, writable: true);
            key?.DeleteValue("CKFlip3D", throwOnMissingValue: false);
        }
        catch { }
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(runKey, writable: true);
            key?.DeleteValue("CKFlip3D", throwOnMissingValue: false);
        }
        catch { }
    }

    private static void TryDeleteFile(string path)
    {
        try
        {
            // The running exe image can't be deleted (only relevant in the
            // degraded in-place run) — the cmd fallback removes it later.
            if (string.Equals(path, Environment.ProcessPath, StringComparison.OrdinalIgnoreCase))
                return;
            if (File.Exists(path)) File.Delete(path);
        }
        catch { }
    }

    private static void TryDeleteDirectory(string path, bool recursive)
    {
        try
        {
            if (Directory.Exists(path)) Directory.Delete(path, recursive);
        }
        catch { }
    }

    private static void CleanupEmptySubdirectories(string installDir)
    {
        try
        {
            foreach (string dir in Directory.EnumerateDirectories(installDir, "*",
                         SearchOption.AllDirectories).OrderByDescending(d => d.Length))
            {
                try
                {
                    if (!Directory.EnumerateFileSystemEntries(dir).Any())
                        Directory.Delete(dir);
                }
                catch { }
            }
            if (!Directory.EnumerateFileSystemEntries(installDir).Any())
                Directory.Delete(installDir);
        }
        catch { }
    }

    /// <summary>
    /// After this process exits, a detached cmd deletes this exe (normally
    /// the %TEMP% copy). When <paramref name="alsoRemoveDir"/> is set (the
    /// degraded in-place run where the exe blocked its own folder), the
    /// install dir is removed afterwards as well.
    /// </summary>
    private static void ScheduleSelfDelete(string? alsoRemoveDir)
    {
        string self = Environment.ProcessPath ?? string.Empty;
        if (self.Length == 0) return;

        try
        {
            string cmd = $"/c ping 127.0.0.1 -n 4 >nul & del /f /q \"{self}\"";
            if (alsoRemoveDir != null)
                cmd += $" & rd /s /q \"{alsoRemoveDir}\"";

            Process.Start(new ProcessStartInfo("cmd.exe", cmd)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = Path.GetTempPath(),
            });
        }
        catch { /* leftover exe is harmless */ }
    }
}
