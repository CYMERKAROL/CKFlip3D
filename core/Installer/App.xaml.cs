using System.Diagnostics;
using System.IO;
using System.Windows;
using CKFlip3D.Installer.Engine;

namespace CKFlip3D.Installer;

/// <summary>
/// One executable, two modes: it is the installer when run as CKFlip3D.Setup,
/// and the uninstaller when the install step copies it into the install dir
/// as "Uninstall CKFlip3D.exe" (or when launched with --uninstall).
///
/// Uninstall bootstrap: the copy living in the install dir cannot delete
/// itself or its own folder, so on launch it re-executes itself from %TEMP%
/// with "--from &lt;installDir&gt;" and exits; the temp copy does the actual
/// work (including removing the whole install folder) and self-deletes last.
/// </summary>
public partial class App : Application
{
    public static bool IsUninstallMode { get; private set; }
    public static bool IsQuiet { get; private set; }

    /// <summary>Install dir handed over by the bootstrap copy (--from).</summary>
    public static string? UninstallFrom { get; private set; }

    private const string MutexName = @"Global\CKFlip3D.Setup.SingleInstance";
    private Mutex? _instanceMutex;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        bool argUninstall = e.Args.Any(a =>
            string.Equals(a, "--uninstall", StringComparison.OrdinalIgnoreCase));
        bool nameUninstall = (Path.GetFileNameWithoutExtension(Environment.ProcessPath) ?? "")
            .Contains("uninstall", StringComparison.OrdinalIgnoreCase);
        IsUninstallMode = argUninstall || nameUninstall;
        IsQuiet = e.Args.Any(a => string.Equals(a, "--quiet", StringComparison.OrdinalIgnoreCase));
        UninstallFrom = ReadArgValue(e.Args, "--from");

        // Single instance across installer AND uninstaller. WaitOne (instead
        // of a plain createdNew check) also bridges the short hand-off window
        // while the install-dir copy relaunches itself from %TEMP%.
        _instanceMutex = new Mutex(initiallyOwned: false, MutexName);
        bool acquired;
        try
        {
            acquired = _instanceMutex.WaitOne(TimeSpan.FromSeconds(8));
        }
        catch (AbandonedMutexException)
        {
            acquired = true;   // previous holder died — the lock is ours
        }
        if (!acquired)
        {
            if (!IsQuiet)
                MessageBox.Show("CKFlip3D Setup is already running.",
                    "CKFlip3D Setup", MessageBoxButton.OK, MessageBoxImage.Information);
            Shutdown(1);
            return;
        }

        if (IsUninstallMode && UninstallFrom == null && RelaunchFromTemp())
            return;   // the %TEMP% copy takes over; this process exits

        if (IsUninstallMode && IsQuiet)
        {
            // Headless uninstall for the Apps & Features QuietUninstallString.
            RunQuietUninstall();
            return;
        }

        MainWindow = new MainWindow();
        MainWindow.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        try { _instanceMutex?.ReleaseMutex(); } catch { }
        _instanceMutex?.Dispose();
        base.OnExit(e);
    }

    /// <summary>
    /// Copy this exe to %TEMP% and run the copy with the resolved install
    /// dir. Returns false when the bootstrap fails, in which case the
    /// in-place uninstall still runs (folder removal degraded, not broken).
    /// </summary>
    private bool RelaunchFromTemp()
    {
        try
        {
            string self = Environment.ProcessPath
                ?? throw new InvalidOperationException("Process path unavailable.");
            string installDir = UninstallEngine.ResolveInstallDir();
            string tempExe = Path.Combine(Path.GetTempPath(),
                $"CKFlip3D.Uninstall.{Environment.ProcessId}.exe");

            File.Copy(self, tempExe, overwrite: true);

            string args = $"--uninstall --from \"{installDir}\"";
            if (IsQuiet) args += " --quiet";
            Process.Start(new ProcessStartInfo(tempExe, args) { UseShellExecute = false });

            // Release the mutex before exiting so the temp copy's WaitOne
            // succeeds as soon as possible.
            try { _instanceMutex?.ReleaseMutex(); } catch { }
            Shutdown(0);
            return true;
        }
        catch
        {
            return false;
        }
    }

    private async void RunQuietUninstall()
    {
        try
        {
            var engine = new UninstallEngine();
            await engine.RunAsync(UninstallFrom ?? UninstallEngine.ResolveInstallDir(),
                removeUserData: false,
                new Progress<InstallProgress>(),
                CancellationToken.None);
        }
        catch { /* quiet mode never surfaces UI */ }
        Shutdown(0);
    }

    private static string? ReadArgValue(string[] args, string name)
    {
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (string.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
                return args[i + 1];
        }
        return null;
    }
}
