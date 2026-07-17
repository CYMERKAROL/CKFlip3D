using System.Runtime.InteropServices;
using System.Windows;
using CKFlip3D.Settings.Models;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings;

public partial class App : Application
{
    /// <summary>Single shared settings instance, loaded once at startup.</summary>
    public static SettingsModel Settings { get; } = ConfigService.Load();

    private Mutex? _singleInstanceMutex;

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? className, string? windowName);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hwnd, int cmd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hwnd);

    private const int SW_RESTORE = 9;

    protected override void OnStartup(StartupEventArgs e)
    {
        // Elevated helper mode: "--set-startup on <corePath>" / "--set-startup off".
        // Performs only the HKLM Run write (StartupService elevates into this),
        // no UI, then exits.
        if (e.Args.Length >= 2 && e.Args[0] == "--set-startup")
        {
            int code = 0;
            try
            {
                bool enable = e.Args[1] == "on";
                StartupService.WriteEntry(enable, e.Args.Length >= 3 ? e.Args[2] : null);
            }
            catch
            {
                code = 1;
            }
            Shutdown(code);
            return;
        }

        // Single instance: a second launch activates the existing window
        // (the core's tray menu also checks this before spawning us).
        _singleInstanceMutex = new Mutex(true, @"Local\CKFlip3D.Settings.SingleInstance", out bool createdNew);
        if (!createdNew)
        {
            IntPtr existing = FindWindowW(null, "CKFlip3D Settings");
            if (existing != IntPtr.Zero)
            {
                if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            Shutdown();
            return;
        }

        // Saved theme before any window shows (DynamicResource picks it up).
        ThemeService.Apply(Settings.AppTheme);

        base.OnStartup(e);

        var window = new MainWindow();
        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _singleInstanceMutex?.Dispose();
        base.OnExit(e);
    }
}
