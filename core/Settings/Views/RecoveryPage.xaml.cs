using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Animation;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

public partial class RecoveryPage : UserControl
{
    public RecoveryPage()
    {
        InitializeComponent();
    }

    private void RecoveryAction_Click(object sender, RoutedEventArgs e)
    {
        string action = (sender as Button)?.Tag as string ?? "?";
        try
        {
            switch (action)
            {
                case "panic":
                    ShowStatus(RecoveryService.PanicRestore());
                    break;

                case "forcequit":
                    Confirm("Force quit CKFlip3D?",
                        "This kills the CKFlip3D core process immediately and then repairs " +
                        "the desktop state (uncloak, taskbars, icons).\n\nContinue?",
                        () => ShowStatus(RecoveryService.ForceQuitCore()));
                    break;

                case "uncloak":
                {
                    int n = RecoveryService.ForceUncloakAllWindows();
                    ShowStatus(n > 0
                        ? $"Uncloaked {n} window(s)."
                        : "No cloaked windows found — cloak flag cleared everywhere anyway.");
                    break;
                }

                case "taskbars":
                {
                    int n = RecoveryService.RestoreTaskbars();
                    ShowStatus(n > 0
                        ? $"Restored {n} hidden taskbar(s)."
                        : "All taskbars were already visible.");
                    break;
                }

                case "icons":
                    ShowStatus(RecoveryService.RestoreDesktopIcons()
                        ? "Desktop icons restored."
                        : "Desktop icons were already visible.");
                    break;

                case "safemode":
                    Confirm("Launch Safe Mode?",
                        "Any running CKFlip3D instance is terminated first, then the core is " +
                        "started with --safe-mode (animations and motion blur off).\n\nContinue?",
                        () => ShowStatus(RecoveryService.LaunchSafeMode()));
                    break;
            }
        }
        catch (Exception ex)
        {
            ShowStatus($"Action failed: {ex.Message}");
        }
    }

    private void Confirm(string title, string text, Action onConfirm)
    {
        if (Window.GetWindow(this) is not MainWindow main) { onConfirm(); return; }

        var body = new TextBlock
        {
            Text = text,
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new System.Windows.Media.FontFamily("Segoe UI"),
            FontSize = 13,
            Foreground = (System.Windows.Media.Brush)FindResource("TextPrimaryBrush"),
        };
        main.ShowModal(title, body,
            ("Cancel", false, null),
            ("Continue", true, onConfirm));
    }

    private void ResetConfig_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is not MainWindow main) return;

        var body = new TextBlock
        {
            Text = "This restores every CKFlip3D setting to its default value and "
                 + "overwrites config.json immediately.\n\nAre you sure you want to continue?",
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new System.Windows.Media.FontFamily("Segoe UI"),
            FontSize = 13,
            Foreground = (System.Windows.Media.Brush)FindResource("TextPrimaryBrush"),
        };

        main.ShowModal("Reset configuration?", body,
            ("Cancel", false, null),
            ("Reset to defaults", true, () =>
            {
                App.Settings.ResetToDefaults();
                ConfigService.Save(App.Settings);
                string? startupError = StartupService.Apply(App.Settings.StartWithWindows);
                ShowStatus(startupError == null
                    ? "Configuration was reset to defaults and saved."
                    : $"Configuration reset, but the startup entry failed: {startupError}");
            }));
    }

    private void ShowStatus(string text)
    {
        ActionStatus.Text = text;
        ActionStatus.BeginAnimation(OpacityProperty, new DoubleAnimation
        {
            From = 1, To = 0,
            BeginTime = TimeSpan.FromSeconds(4),
            Duration = TimeSpan.FromMilliseconds(700),
        });
        ActionStatus.Opacity = 1;
    }
}
