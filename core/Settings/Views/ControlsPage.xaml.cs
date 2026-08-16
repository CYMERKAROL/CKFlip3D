using System.Windows;
using System.Windows.Controls;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

public partial class ControlsPage : UserControl
{
    public ControlsPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { UpdateIgnoredSummary(); SyncBindings(); SyncTouchpad(); };
        App.Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(Models.SettingsModel.IgnoredApps) or null)
                UpdateIgnoredSummary();
            if (e.PropertyName is nameof(Models.SettingsModel.ActivationHotkey)
                               or nameof(Models.SettingsModel.PointerInCascade)
                               or nameof(Models.SettingsModel.MouseSelect)
                               or nameof(Models.SettingsModel.CloseFromCascade)
                               or null)
                SyncBindings();
            if (e.PropertyName is nameof(Models.SettingsModel.TouchpadNav)
                               or nameof(Models.SettingsModel.TouchpadActivateGesture)
                               or nameof(Models.SettingsModel.TouchpadCycleFingers)
                               or null)
                SyncTouchpad();
        };
    }

    // ---- Mouse & keyboard row -----------------------------------------------
    // A one-line summary of what is bound, so the page still answers "what
    // opens it?" without opening the sub-page.

    private void SyncBindings()
    {
        string mouse = !App.Settings.PointerInCascade
            ? "The mouse does not act on the stack."
            : (App.Settings.MouseSelect, App.Settings.CloseFromCascade) switch
            {
                (true, true) => "Clicking picks a window and another button closes one.",
                (true, false) => "Clicking picks a window.",
                (false, true) => "Clicking closes a window.",
                _ => "The mouse does not act on the stack.",
            };
        BindingsHint.Text = $"{App.Settings.ActivationHotkey} opens the cascade. {mouse} "
                          + "Commit and cancel keys live in here too.";
    }

    private void ManageBindings_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is MainWindow main)
            main.PushSubPage(new MouseKeyboardPage(), "Mouse & keyboard");
    }

    // ---- Touchpad rows ------------------------------------------------------
    // The customisation menu needs both a touchpad to configure and the
    // navigation master switch on; either missing grays it out (and the
    // switch itself grays out without a touchpad).

    private void SyncTouchpad()
    {
        bool present = TouchpadService.IsPresent;
        bool enabled = present && App.Settings.TouchpadNav;

        TouchpadNavRow.IsEnabled = present;
        TouchpadNavRow.Opacity = present ? 1.0 : 0.55;
        if (!present)
            TouchpadNavHint.Text = "No precision touchpad was found on this device.";

        TouchpadRow.IsEnabled = enabled;
        TouchpadRow.Opacity = enabled ? 1.0 : 0.55;
        TouchpadHint.Text =
            !present ? "No precision touchpad was found on this device."
            : !App.Settings.TouchpadNav
                ? "Turn Touchpad navigation on (Navigation, below) to customise the gestures."
                : $"{DescribeActivation()}, {Count(App.Settings.TouchpadCycleFingers)} fingers "
                  + "left and right move through the stack. Sensitivity, direction, "
                  + "tap-to-commit and a live activity preview live in here.";
    }

    private static string Count(int n) => n switch
    {
        2 => "two", 4 => "four", _ => n.ToString(),
    };

    private static string DescribeActivation() => App.Settings.TouchpadActivateGesture switch
    {
        1 => "A two-finger ↘ diagonal opens the cascade",
        2 => "A two-finger ↙ diagonal opens the cascade",
        3 => "A four-finger ↘ diagonal opens the cascade",
        4 => "A four-finger ↙ diagonal opens the cascade",
        _ => "No opening gesture",
    };

    private void ManageTouchpad_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is MainWindow main)
            main.PushSubPage(new TouchpadPage(), "Touchpad gestures");
    }

    // ---- Ignored applications -----------------------------------------------

    private void UpdateIgnoredSummary()
    {
        int count = App.Settings.IgnoredAppsList.Count;
        IgnoredSummary.Text = count == 0
            ? "No applications are ignored. The hotkey is passed through to Windows while a listed program is in the foreground."
            : $"{count} application(s) ignored. The hotkey is passed through to Windows while any of them is in the foreground.";
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
        App.Settings.CommitHotkey = "Enter";
        App.Settings.CancelHotkey = "Escape";
        App.Settings.CloseHotkey = "Delete";
        App.Settings.CloseKeyEnabled = true;
        App.Settings.IgnoreFullscreen = false;
        App.Settings.MouseWheelCycle = true;
        App.Settings.KeyboardNav = true;
        App.Settings.IgnoredApps = "";
        // Opt-in by default — see MouseKeyboardPage.RestoreDefaults_Click.
        App.Settings.PointerInCascade = false;
        App.Settings.MouseSelect = true;
        App.Settings.MouseSelectButton = 1;
        App.Settings.CloseFromCascade = true;
        App.Settings.MouseCloseButton = 3;
        App.Settings.MouseDragEnabled = true;
        App.Settings.MouseDragButton = 2;
        App.Settings.TouchpadNav = true;
        App.Settings.TouchpadActivateGesture = 1;
        App.Settings.TouchpadCancelSwipe = true;
        App.Settings.TouchpadCycleFingers = 2;
        App.Settings.TouchpadReverse = false;
        App.Settings.TouchpadSensitivity = 50;
        App.Settings.TouchpadSmoothing = 35;
        App.Settings.TouchpadCommitGesture = 1;
        App.Settings.WindowSnap = true;
    }
}
