using System.Diagnostics;
using System.IO;
using System.Net.Http;

namespace CKFlip3D.Installer.Engine;

/// <summary>
/// The Settings app is framework-dependent (net10.0-windows), so the target
/// machine needs the .NET 10 Windows Desktop Runtime. The setup exe itself is
/// self-contained and runs regardless. When the runtime is missing this
/// downloads the official Microsoft installer (aka.ms evergreen link) and runs
/// it silently. A failed/offline bootstrap is reported as a warning, not a
/// fatal error — the C++ core works without .NET.
/// </summary>
public static class RuntimeBootstrapper
{
    private const int RequiredMajor = 10;

    private const string DownloadUrl =
        "https://aka.ms/dotnet/10.0/windowsdesktop-runtime-win-x64.exe";

    /// <summary>True when a Microsoft.WindowsDesktop.App 10.x runtime is present machine-wide.</summary>
    public static bool IsDesktopRuntimeInstalled()
    {
        try
        {
            string shared = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                "dotnet", "shared", "Microsoft.WindowsDesktop.App");
            if (!Directory.Exists(shared)) return false;

            foreach (string dir in Directory.EnumerateDirectories(shared))
            {
                string name = Path.GetFileName(dir);
                if (Version.TryParse(name, out var v) && v.Major == RequiredMajor)
                    return true;
            }
        }
        catch { }
        return false;
    }

    /// <summary>
    /// Download + silent install of the desktop runtime. Reports progress in
    /// the given percent window. Throws OperationCanceledException on cancel;
    /// any other failure is returned as a warning string (null = success).
    /// </summary>
    public static async Task<string?> EnsureAsync(
        IProgress<InstallProgress> progress, double pctFrom, double pctTo,
        CancellationToken ct)
    {
        if (IsDesktopRuntimeInstalled())
        {
            progress.Report(new InstallProgress(pctTo, "Checking .NET Desktop Runtime…",
                ".NET 10 Desktop Runtime already installed"));
            return null;
        }

        string tempFile = Path.Combine(Path.GetTempPath(),
            $"windowsdesktop-runtime-10-win-x64.{Environment.ProcessId}.exe");
        double downloadSpan = (pctTo - pctFrom) * 0.7;

        try
        {
            using var http = new HttpClient();
            http.Timeout = TimeSpan.FromMinutes(10);

            using (var response = await http.GetAsync(DownloadUrl,
                       HttpCompletionOption.ResponseHeadersRead, ct))
            {
                response.EnsureSuccessStatusCode();
                long? totalBytes = response.Content.Headers.ContentLength;

                await using var source = await response.Content.ReadAsStreamAsync(ct);
                await using var target = File.Create(tempFile);

                var buffer = new byte[81920];
                long readTotal = 0;
                int read;
                while ((read = await source.ReadAsync(buffer, ct)) > 0)
                {
                    await target.WriteAsync(buffer.AsMemory(0, read), ct);
                    readTotal += read;
                    double frac = totalBytes is > 0 ? (double)readTotal / totalBytes.Value : 0;
                    progress.Report(new InstallProgress(
                        pctFrom + frac * downloadSpan,
                        "Downloading .NET Desktop Runtime…",
                        totalBytes is > 0
                            ? $"{readTotal / 1048576} / {totalBytes.Value / 1048576} MB"
                            : $"{readTotal / 1048576} MB"));
                }
            }

            ct.ThrowIfCancellationRequested();
            progress.Report(new InstallProgress(pctFrom + downloadSpan,
                "Installing .NET Desktop Runtime…", "Running Microsoft installer (silent)"));

            var psi = new ProcessStartInfo(tempFile, "/install /quiet /norestart")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
            };
            using var proc = Process.Start(psi)
                ?? throw new InvalidOperationException("Runtime installer could not be started.");
            await proc.WaitForExitAsync(ct);

            // 0 = ok, 1638 = newer version already present, 3010 = ok + reboot pending
            if (proc.ExitCode is not (0 or 1638 or 3010))
                return $".NET Desktop Runtime installer exited with code {proc.ExitCode}.";

            progress.Report(new InstallProgress(pctTo,
                "Installing .NET Desktop Runtime…", "Done"));
            return null;
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception ex)
        {
            return $".NET Desktop Runtime could not be installed ({ex.Message}). "
                 + "The Settings app needs it — install it later from https://dotnet.microsoft.com.";
        }
        finally
        {
            try { if (File.Exists(tempFile)) File.Delete(tempFile); } catch { }
        }
    }
}
