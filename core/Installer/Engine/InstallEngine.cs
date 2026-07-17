using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using CKFlip3D.Installer.Interop;
using Microsoft.Win32;

namespace CKFlip3D.Installer.Engine;

/// <summary>
/// Runs the whole installation as an ordered sequence of steps. Every change
/// registers an undo action with the <see cref="RollbackManager"/>; a failure
/// or user cancel at any point unwinds everything already done (files,
/// shortcuts, registry), restoring overwritten files from backup.
/// </summary>
public sealed class InstallEngine
{
    private const string PayloadResource = "CKFlip3D.Installer.payload.zip";

    private readonly RollbackManager _rollback = new();

    /// <summary>
    /// Executes the install. Throws OperationCanceledException when the user
    /// cancelled (after rollback) and InvalidOperationException with a
    /// human-readable message on failure (after rollback).
    /// </summary>
    public async Task RunAsync(InstallContext ctx, IProgress<InstallProgress> progress,
                               CancellationToken ct)
    {
        long extractedBytes = 0;
        try
        {
            // ---- 1. Preparation & safety checks (0–4%) -------------------
            progress.Report(new InstallProgress(0, "Preparing installation…"));
            ValidateTarget(ctx);
            CloseRunningInstances(progress);
            ct.ThrowIfCancellationRequested();

            // ---- 2. .NET Desktop Runtime (4–30%) -------------------------
            progress.Report(new InstallProgress(4, "Checking .NET Desktop Runtime…"));
            ctx.RuntimeWarning = await RuntimeBootstrapper.EnsureAsync(progress, 4, 30, ct);

            // ---- 3. Directories (30–32%) ---------------------------------
            progress.Report(new InstallProgress(30, "Creating directories…", ctx.InstallDir));
            // Updates keep the original install's "setup created this folder"
            // marker — otherwise an update would strand the folder at uninstall.
            ctx.InstallDirCreated = !Directory.Exists(ctx.InstallDir)
                || HasDirCreatedMarker(ctx.InstallDir);
            CreateDirectoryTracked(ctx.InstallDir);
            ct.ThrowIfCancellationRequested();

            // ---- 4. Extract payload (32–78%) -----------------------------
            var installedFiles = ExtractPayload(ctx, progress, ct, out extractedBytes);

            // ---- 5. Uninstaller (78–82%) ---------------------------------
            progress.Report(new InstallProgress(78, "Writing uninstaller…",
                InstallContext.UninstallerFileName));
            WriteUninstaller(ctx, installedFiles);
            ct.ThrowIfCancellationRequested();

            // ---- 6. Shortcuts (82–88%) -----------------------------------
            CreateShortcuts(ctx, progress);
            ct.ThrowIfCancellationRequested();

            // ---- 7. Apps & Features registration (88–94%) ----------------
            progress.Report(new InstallProgress(88, "Registering application…",
                "Windows Apps & Features entry"));
            RegisterUninstallEntry(ctx, extractedBytes);
            ct.ThrowIfCancellationRequested();

            // ---- 8. Manifest + commit (94–100%) --------------------------
            progress.Report(new InstallProgress(94, "Finalizing…",
                InstallContext.ManifestFileName));
            WriteManifest(ctx, installedFiles);

            _rollback.Commit();
            progress.Report(new InstallProgress(100, "Installation complete", ""));
        }
        catch (OperationCanceledException)
        {
            _rollback.RollbackAll(progress);
            throw;
        }
        catch (Exception ex)
        {
            _rollback.RollbackAll(progress);
            throw new InvalidOperationException(ex.Message, ex);
        }
    }

    // =====================================================================
    // Steps
    // =====================================================================

    private static void ValidateTarget(InstallContext ctx)
    {
        if (string.IsNullOrWhiteSpace(ctx.InstallDir) || !Path.IsPathFullyQualified(ctx.InstallDir))
            throw new InvalidOperationException("The install path must be a full path (e.g. C:\\Program Files\\CKFlip3D).");
        if (ctx.InstallDir.IndexOfAny(Path.GetInvalidPathChars()) >= 0)
            throw new InvalidOperationException("The install path contains invalid characters.");

        // Refuse plainly dangerous targets (drive roots, bare Program Files):
        // the uninstaller deletes files inside InstallDir.
        string full = Path.GetFullPath(ctx.InstallDir).TrimEnd('\\');
        string root = Path.GetPathRoot(full) ?? "";
        if (string.Equals(full, root.TrimEnd('\\'), StringComparison.OrdinalIgnoreCase)
            || string.Equals(full, Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), StringComparison.OrdinalIgnoreCase)
            || string.Equals(full, Environment.GetFolderPath(Environment.SpecialFolder.Windows), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Please choose a dedicated folder (e.g. C:\\Program Files\\CKFlip3D), not a system root.");
        ctx.InstallDir = full;

        long payloadSize;
        using (var payload = GetPayloadStream())
            payloadSize = payload.Length;
        var drive = new DriveInfo(root);
        if (drive.AvailableFreeSpace < payloadSize * 3 + 300L * 1024 * 1024)
            throw new InvalidOperationException(
                $"Not enough free space on {drive.Name} — at least "
                + $"{(payloadSize * 3 + 300L * 1024 * 1024) / 1048576} MB are required.");
    }

    private static void CloseRunningInstances(IProgress<InstallProgress> progress)
    {
        foreach (string name in InstallContext.ProcessNames)
        {
            foreach (var proc in Process.GetProcessesByName(name))
            {
                try
                {
                    progress.Report(new InstallProgress(2, "Closing running instances…", name));
                    proc.CloseMainWindow();
                    if (!proc.WaitForExit(3000))
                        proc.Kill();
                    proc.WaitForExit(5000);
                }
                catch { /* already gone or inaccessible */ }
                finally { proc.Dispose(); }
            }
        }
    }

    private void CreateDirectoryTracked(string dir)
    {
        // Register removal only for directories we actually create, walking
        // up from the deepest missing level.
        var toCreate = new Stack<string>();
        var current = new DirectoryInfo(dir);
        while (current != null && !current.Exists)
        {
            toCreate.Push(current.FullName);
            current = current.Parent;
        }
        while (toCreate.Count > 0)
        {
            string path = toCreate.Pop();
            Directory.CreateDirectory(path);
            _rollback.Push($"Removing directory {path}", () =>
            {
                if (Directory.Exists(path) && !Directory.EnumerateFileSystemEntries(path).Any())
                    Directory.Delete(path);
            });
        }
    }

    private static bool HasDirCreatedMarker(string installDir)
    {
        try
        {
            string manifest = Path.Combine(installDir, InstallContext.ManifestFileName);
            return File.Exists(manifest) && File.ReadLines(manifest).Any(l =>
                l.Trim().Equals(InstallContext.DirCreatedMarker, StringComparison.OrdinalIgnoreCase));
        }
        catch
        {
            return false;
        }
    }

    private static Stream GetPayloadStream()
    {
        return Assembly.GetExecutingAssembly().GetManifestResourceStream(PayloadResource)
            ?? throw new InvalidOperationException(
                "This setup executable contains no payload. Build it with "
                + "core\\Installer\\build_installer.bat, which packs the application "
                + "files before publishing.");
    }

    private List<string> ExtractPayload(InstallContext ctx, IProgress<InstallProgress> progress,
                                        CancellationToken ct, out long extractedBytes)
    {
        var installed = new List<string>();
        extractedBytes = 0;

        using var zip = new ZipArchive(GetPayloadStream(), ZipArchiveMode.Read);
        var entries = zip.Entries.Where(e => !string.IsNullOrEmpty(e.Name)).ToList();
        long totalSize = Math.Max(1, entries.Sum(e => e.Length));
        long doneSize = 0;

        foreach (var entry in entries)
        {
            ct.ThrowIfCancellationRequested();

            string relative = entry.FullName.Replace('/', '\\');
            string target = Path.GetFullPath(Path.Combine(ctx.InstallDir, relative));
            if (!target.StartsWith(ctx.InstallDir.TrimEnd('\\') + '\\', StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException($"Payload entry escapes the install dir: {entry.FullName}");

            progress.Report(new InstallProgress(
                32 + doneSize * 46.0 / totalSize, "Extracting files…", relative));

            string targetDir = Path.GetDirectoryName(target)!;
            if (!Directory.Exists(targetDir))
                CreateDirectoryTracked(targetDir);

            // Upgrade safety: keep the old file so rollback can restore it.
            _rollback.BackupFile(target);
            entry.ExtractToFile(target, overwrite: true);
            _rollback.Push($"Deleting {relative}", () =>
            {
                if (File.Exists(target)) File.Delete(target);
            });

            installed.Add(relative);
            doneSize += entry.Length;
            extractedBytes += entry.Length;
        }

        if (!installed.Contains(InstallContext.CoreExeName, StringComparer.OrdinalIgnoreCase))
            throw new InvalidOperationException(
                $"The payload does not contain {InstallContext.CoreExeName} — rebuild the payload.");

        // Legacy payload cleanup: older payloads shipped CKFlip3D.ico next
        // to the exes, but the icon is embedded in CKFlip3D.exe (app.rc)
        // and every consumer (shortcuts, Apps & Features DisplayIcon) falls
        // back to the exe path — a loose .ico in the install dir is
        // unnecessary.  Upgrades over an old install delete the stale copy;
        // rollback restores it if the install fails midway.
        if (!installed.Contains(InstallContext.IconFileName, StringComparer.OrdinalIgnoreCase))
        {
            string legacyIcon = Path.Combine(ctx.InstallDir, InstallContext.IconFileName);
            if (File.Exists(legacyIcon))
            {
                _rollback.BackupFile(legacyIcon);
                File.Delete(legacyIcon);
            }
        }

        return installed;
    }

    private void WriteUninstaller(InstallContext ctx, List<string> installedFiles)
    {
        // The setup exe doubles as the uninstaller (see App.DetectMode):
        // a self-contained copy named "Uninstall CKFlip3D.exe" works on any
        // machine, .NET installed or not.
        string self = Environment.ProcessPath
            ?? throw new InvalidOperationException("The setup executable path could not be determined.");

        _rollback.BackupFile(ctx.UninstallerPath);
        File.Copy(self, ctx.UninstallerPath, overwrite: true);
        _rollback.Push("Deleting uninstaller", () =>
        {
            if (File.Exists(ctx.UninstallerPath)) File.Delete(ctx.UninstallerPath);
        });
        installedFiles.Add(InstallContext.UninstallerFileName);
    }

    private void CreateShortcuts(InstallContext ctx, IProgress<InstallProgress> progress)
    {
        // Icons come straight from each target exe — both binaries embed
        // their own (app.rc for the core, ApplicationIcon for Settings), so
        // no loose .ico is needed in the install dir.  The uninstaller is
        // the one exception: it is a copy of the Setup exe whose embedded
        // icon is the setup one, so its distinct icon ships as a file.
        if (ctx.CreateStartMenuFolder)
        {
            progress.Report(new InstallProgress(82, "Creating shortcuts…", "Start Menu folder"));
            CreateDirectoryTracked(InstallContext.StartMenuDir);

            CreateShortcutTracked(Path.Combine(InstallContext.StartMenuDir, "CKFlip3D.lnk"),
                ctx.CoreExePath, "Launch the CKFlip3D window switcher", ctx.CoreExePath);
            CreateShortcutTracked(Path.Combine(InstallContext.StartMenuDir, "CKFlip3D Settings.lnk"),
                ctx.SettingsExePath, "Configure CKFlip3D", ctx.SettingsExePath);
            string uninstallIcon = Path.Combine(ctx.InstallDir, InstallContext.UninstallIconFileName);
            CreateShortcutTracked(Path.Combine(InstallContext.StartMenuDir, "Uninstall CKFlip3D.lnk"),
                ctx.UninstallerPath, "Remove CKFlip3D from this computer",
                File.Exists(uninstallIcon) ? uninstallIcon : null);
        }

        if (ctx.CreateDesktopShortcut)
        {
            progress.Report(new InstallProgress(86, "Creating shortcuts…", "Desktop shortcut"));
            CreateShortcutTracked(InstallContext.CommonDesktopShortcut,
                ctx.CoreExePath, "Launch the CKFlip3D window switcher", ctx.CoreExePath);
        }
    }

    private void CreateShortcutTracked(string lnkPath, string target, string description, string? icon)
    {
        _rollback.BackupFile(lnkPath);
        ShellLink.Create(lnkPath, target, description, iconPath: icon);
        _rollback.Push($"Deleting shortcut {Path.GetFileName(lnkPath)}", () =>
        {
            if (File.Exists(lnkPath)) File.Delete(lnkPath);
        });
    }

    private void RegisterUninstallEntry(InstallContext ctx, long extractedBytes)
    {
        bool existedBefore;
        using (var probe = Registry.LocalMachine.OpenSubKey(InstallContext.UninstallKeyPath))
            existedBefore = probe != null;

        using var key = Registry.LocalMachine.CreateSubKey(InstallContext.UninstallKeyPath)
            ?? throw new InvalidOperationException("The uninstall registry key could not be created.");

        if (!existedBefore)
        {
            _rollback.Push("Removing Apps & Features entry", () =>
                Registry.LocalMachine.DeleteSubKeyTree(InstallContext.UninstallKeyPath, throwOnMissingSubKey: false));
        }

        // Apps & Features icon straight from the core exe's embedded icon —
        // no loose .ico ships in the install dir (see CreateShortcuts).
        string icon = ctx.CoreExePath;

        key.SetValue("DisplayName", InstallContext.AppName);
        key.SetValue("DisplayVersion", InstallContext.AppVersion);
        key.SetValue("Publisher", InstallContext.Publisher);
        key.SetValue("InstallLocation", ctx.InstallDir);
        key.SetValue("DisplayIcon", icon);
        key.SetValue("UninstallString", $"\"{ctx.UninstallerPath}\" --uninstall");
        key.SetValue("QuietUninstallString", $"\"{ctx.UninstallerPath}\" --uninstall --quiet");
        key.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd", CultureInfo.InvariantCulture));
        key.SetValue("EstimatedSize", (int)Math.Min(int.MaxValue, extractedBytes / 1024), RegistryValueKind.DWord);
        key.SetValue("NoModify", 1, RegistryValueKind.DWord);
        key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
    }

    private void WriteManifest(InstallContext ctx, List<string> installedFiles)
    {
        // The uninstaller deletes exactly what this manifest lists — it never
        // blindly wipes the whole folder, in case the user picked a shared dir.
        string path = Path.Combine(ctx.InstallDir, InstallContext.ManifestFileName);
        var sb = new StringBuilder();
        sb.AppendLine("# CKFlip3D install manifest v1");
        if (ctx.InstallDirCreated)
            sb.AppendLine(InstallContext.DirCreatedMarker);
        foreach (string file in installedFiles.Distinct(StringComparer.OrdinalIgnoreCase))
            sb.AppendLine(file);

        _rollback.BackupFile(path);
        File.WriteAllText(path, sb.ToString(), new UTF8Encoding(false));
        _rollback.Push("Deleting manifest", () =>
        {
            if (File.Exists(path)) File.Delete(path);
        });
    }
}
