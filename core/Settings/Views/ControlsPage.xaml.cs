using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

public partial class ControlsPage : UserControl
{
    private bool _syncingToggle;

    public ControlsPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { UpdateIgnoredSummary(); SyncToggleMode(); };
        Unloaded += (_, _) => HotkeyService.StopCapture();
        App.Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(Models.SettingsModel.IgnoredApps) or null)
                UpdateIgnoredSummary();
            if (e.PropertyName is nameof(Models.SettingsModel.ActivationHotkey)
                               or nameof(Models.SettingsModel.HotkeyToggleMode)
                               or null)
                SyncToggleMode();
        };
    }

    // ---- Toggle activation row ---------------------------------------------
    // A binding without modifiers (bare key, bare mouse button, bare Win)
    // is inherently toggle in the core, so the switch is shown ON and
    // grayed out — without writing the forced state into the persisted
    // HotkeyToggleMode value.  For combos the switch edits the model
    // normally.

    private static bool IsSingleKeyBinding(string combo) =>
        !combo.Contains('+');

    private void SyncToggleMode()
    {
        _syncingToggle = true;
        if (IsSingleKeyBinding(App.Settings.ActivationHotkey))
        {
            ToggleModeCheck.IsChecked = true;
            ToggleModeCheck.IsEnabled = false;
            ToggleModeRow.Opacity = 0.55;
            ToggleModeHint.Text = "Single-key bindings always toggle — the cascade "
                + "stays open until Enter commits or Esc cancels.";
        }
        else
        {
            ToggleModeCheck.IsEnabled = true;
            ToggleModeCheck.IsChecked = App.Settings.HotkeyToggleMode;
            ToggleModeRow.Opacity = 1.0;
            ToggleModeHint.Text = "Keeps the cascade open after the hotkey is released "
                + "— commit with Enter, cancel with Esc, and keep cycling with the main "
                + "key. Off restores the classic hold-to-keep-open behaviour.";
        }
        _syncingToggle = false;
    }

    private void ToggleMode_Changed(object sender, RoutedEventArgs e)
    {
        if (_syncingToggle || !ToggleModeCheck.IsEnabled) return;
        App.Settings.HotkeyToggleMode = ToggleModeCheck.IsChecked == true;
    }

    // ---- Activation hotkey capture -----------------------------------------

    private void ChangeHotkey_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is not MainWindow main)
            return;

        var display = new TextBlock
        {
            Text = "…",
            FontFamily = new FontFamily("Consolas"),
            FontSize = 20,
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 14, 0, 14),
        };
        display.SetResourceReference(TextBlock.ForegroundProperty, "AccentBrush");

        var body = new StackPanel();
        body.Children.Add(new TextBlock
        {
            Text = "Press the new activation combination — keyboard keys, mouse "
                 + "buttons or both (mouse movement and wheel are ignored). "
                 + "A bare left click cannot be bound. Esc cancels.",
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 12,
            Foreground = (Brush)FindResource("TextPrimaryBrush"),
        });
        body.Children.Add(display);

        HotkeyService.StartCapture(
            onPreview: text => display.Text = text,
            onCaptured: combo => Dispatcher.BeginInvoke(() => ConfirmAndAssign(main, combo)),
            onCancelled: () => Dispatcher.BeginInvoke(main.CloseModal));

        main.ShowModal("Set activation hotkey", body,
            ("Cancel", false, HotkeyService.StopCapture));
    }

    private static void ConfirmAndAssign(MainWindow main, string combo)
    {
        string? warning = HotkeyService.GetWarning(combo);
        if (warning == null)
        {
            main.CloseModal();
            App.Settings.ActivationHotkey = combo;
            return;
        }

        // Swap the still-open capture modal's content in place — closing it
        // first and immediately reopening lets the close fade's Completed
        // handler collapse the freshly shown confirmation.

        var body = new TextBlock
        {
            Text = $"Detected: {combo}\n\n{warning}",
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 12,
            Foreground = (Brush)main.FindResource("TextPrimaryBrush"),
        };
        main.ShowModal("Use this combination?", body,
            ("Use anyway", true, () => App.Settings.ActivationHotkey = combo),
            ("Cancel", false, null));
    }

    private void UpdateIgnoredSummary()
    {
        int count = App.Settings.IgnoredAppsList.Count;
        IgnoredSummary.Text = count == 0
            ? "No applications are ignored. Win + Tab is passed through to Windows while a listed program is in the foreground."
            : $"{count} application(s) ignored. Win + Tab is passed through to Windows while any of them is in the foreground.";
    }

    private void ManageIgnored_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is MainWindow main)
            main.PushSubPage(new IgnoredAppsPage(), "Ignored applications");
    }

    private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.ActivationHotkey = "Win+Tab";
        App.Settings.HotkeyToggleMode = false;
        App.Settings.IgnoreFullscreen = false;
        App.Settings.MouseWheelCycle = true;
        App.Settings.KeyboardNav = true;
        App.Settings.IgnoredApps = "";
    }
}
