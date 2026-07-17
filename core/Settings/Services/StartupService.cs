using System.ComponentModel;
using System.Diagnostics;
using System.Security.Principal;
using Microsoft.Win32;

namespace CKFlip3D.Settings.Services;

/// <summary>
/// "Start on System boot" via a Task Scheduler logon task with highest run
/// level. CKFlip3D.exe requires administrator (manifest), and Windows
/// silently skips Run-key entries that would need elevation — the entry
/// shows up in Task Manager's startup list but never launches. A scheduled
/// task with /RL HIGHEST is the supported way to autostart an elevated app
/// without a UAC prompt at sign-in. Creating/deleting the task itself needs
/// admin rights, so the toggle carries the stock UAC shield; when this
/// process is not elevated we relaunch ourselves elevated with
/// --set-startup, which performs just the task write and exits.
/// </summary>
public static class StartupService
{
    private const string TaskName = "CKFlip3D";
    private const string RunKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "CKFlip3D";

    public static bool IsEnabled()
    {
        // schtasks /Query works from a non-elevated process (exit 0 when the
        // task exists). The TaskCache\Tree registry key does NOT — reading
        // it throws access-denied for a filtered admin token, which made
        // this always return false: every Apply then re-ran the elevation
        // helper and ended in "the elevated helper did not update the
        // startup entry" even though the task was created fine.
        try
        {
            var psi = new ProcessStartInfo("schtasks.exe", $"/Query /TN \"{TaskName}\"")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };
            using var p = Process.Start(psi);
            if (p == null) return false;
            p.StandardOutput.ReadToEnd();
            p.StandardError.ReadToEnd();
            p.WaitForExit(5000);
            return p.ExitCode == 0;
        }
        catch
        {
            return false;
        }
    }

    private static bool IsElevated
    {
        get
        {
            try
            {
                using var id = WindowsIdentity.GetCurrent();
                return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
            }
            catch
            {
                return false;
            }
        }
    }

    /// <summary>
    /// Enables/disables the logon task. Returns null on success, otherwise a
    /// human-readable error. Elevates itself when required.
    /// </summary>
    public static string? Apply(bool enable)
    {
        if (enable == IsEnabled())
        {
            CleanupLegacyRunEntries();
            return null;
        }

        string? exe = null;
        if (enable)
        {
            exe = CoreLocator.FindCoreExe();
            if (exe == null)
                return "CKFlip3D.exe could not be located — startup entry was not created.";
        }

        // Fast path: already elevated.
        if (IsElevated)
        {
            try
            {
                WriteEntry(enable, exe);
                return null;
            }
            catch (Exception ex)
            {
                return $"Scheduled-task update failed: {ex.Message}";
            }
        }

        // Need elevation — relaunch ourselves with --set-startup.
        try
        {
            string self = Environment.ProcessPath ?? AppContext.BaseDirectory + "CKFlip3D.Settings.exe";
            var psi = new ProcessStartInfo(self,
                enable ? $"--set-startup on \"{exe}\"" : "--set-startup off")
            {
                UseShellExecute = true,
                Verb = "runas",
            };
            using var p = Process.Start(psi);
            p?.WaitForExit(15000);
        }
        catch (Win32Exception w32) when (w32.NativeErrorCode == 1223)
        {
            return "Administrator approval was declined — the startup entry was not changed.";
        }
        catch (Exception ex)
        {
            return $"Elevation failed: {ex.Message}";
        }

        return IsEnabled() == enable
            ? null
            : "The elevated helper did not update the startup entry.";
    }

    /// <summary>
    /// Creates or deletes the logon task. Requires an elevated process —
    /// caller handles errors (the --set-startup helper runs this elevated).
    /// </summary>
    public static void WriteEntry(bool enable, string? exePath)
    {
        if (enable)
        {
            if (string.IsNullOrEmpty(exePath))
                throw new ArgumentException("Core exe path is required to enable startup.");
            RunSchtasks($"/Create /F /TN \"{TaskName}\" /TR \"\\\"{exePath}\\\"\" /SC ONLOGON /RL HIGHEST");
        }
        else
        {
            try
            {
                RunSchtasks($"/Delete /F /TN \"{TaskName}\"");
            }
            catch when (!IsEnabled())
            {
                // Task was already gone — deletion goal reached.
            }
        }
        CleanupLegacyRunEntries();
    }

    private static void RunSchtasks(string args)
    {
        var psi = new ProcessStartInfo("schtasks.exe", args)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        using var p = Process.Start(psi)
            ?? throw new InvalidOperationException("schtasks.exe could not be started.");
        string err = p.StandardError.ReadToEnd();
        p.WaitForExit(15000);
        if (p.ExitCode != 0)
            throw new InvalidOperationException(
                string.IsNullOrWhiteSpace(err) ? $"schtasks exited with code {p.ExitCode}." : err.Trim());
    }

    /// <summary>
    /// Removes the Run-key entries from earlier builds (HKCU always; HKLM
    /// only succeeds when elevated — silently kept otherwise and retried on
    /// the next elevated write). Those entries never worked for an
    /// elevation-requiring exe and only cluttered Task Manager's startup tab.
    /// </summary>
    private static void CleanupLegacyRunEntries()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
            key?.DeleteValue(ValueName, throwOnMissingValue: false);
        }
        catch { }
        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(RunKey, writable: true);
            key?.DeleteValue(ValueName, throwOnMissingValue: false);
        }
        catch { }
    }
}
