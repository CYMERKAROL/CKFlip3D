// ---------------------------------------------------------------------------
// The Controls page.  Mostly a hub: it summarises what is bound where and
// hands off to the sub-pages that actually edit it.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
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
                               or nameof(Models.SettingsModel.TouchpadActivateGestures)
                               or nameof(Models.SettingsModel.TouchpadCycleGestures)
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
                : $"{DescribeActivation()}, {DescribeCycle()}. Sensitivity, direction, "
                  + "tap-to-commit and a live activity preview live in here.";
    }

    /// <summary>
    /// The opening strokes that are actually live, named as the touchpad page
    /// names them — a parked gesture is exactly what a summary must not claim.
    /// </summary>
    private static string DescribeActivation()
    {
        var live = Models.TouchpadGestures.Live(Models.TouchpadGestures.Activate,
                                                App.Settings.TouchpadActivateGestureList);
        return live.Count == 0
            ? "No gesture opens the cascade"
            : string.Join(" or ", live.Select(g => Diagonal(g.Token))) + " opens the cascade";

        static string Diagonal(string token) => token.ToLowerInvariant() switch
        {
            "twodownright"  => "A two-finger ↘ diagonal",
            "twodownleft"   => "A two-finger ↙ diagonal",
            "fourdownright" => "A four-finger ↘ diagonal",
            "fourdownleft"  => "A four-finger ↙ diagonal",
            _ => token,
        };
    }

    private static string DescribeCycle()
    {
        var live = Models.TouchpadGestures.Live(Models.TouchpadGestures.Cycle,
                                                App.Settings.TouchpadCycleGestureList);
        return live.Count == 0
            ? "no swipe steps the stack"
            : string.Join(" and ", live.Select(g => Count(g.Fingers)))
              + " fingers left and right move through the stack";

        static string Count(int n) => n switch { 2 => "two", 4 => "four", _ => n.ToString() };
    }

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

    /// <summary>
    /// Everything this page owns, including what its two sub-pages own — the
    /// button says "the default Controls settings", so it has to mean the whole
    /// category and not the handful of rows that happen to be on this screen.
    /// In page order: the hotkey and its toggle, the five key lists, the
    /// activation filters, the pointer bindings, and every touchpad gesture
    /// setting.  A new setting anywhere under Controls belongs in this list;
    /// one left out is a "restore defaults" that quietly does not.
    /// </summary>
    private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.ActivationHotkey = "Win+Tab";
        App.Settings.HotkeyToggleMode = false;
        App.Settings.IgnoreFullscreen = false;
        App.Settings.MouseWheelCycle = true;
        App.Settings.NavForwardKeys = Models.SettingsModel.DefaultNavForwardKeys;
        App.Settings.NavBackKeys = Models.SettingsModel.DefaultNavBackKeys;
        App.Settings.CommitKeys = Models.SettingsModel.DefaultCommitKeys;
        App.Settings.CancelKeys = Models.SettingsModel.DefaultCancelKeys;
        App.Settings.CloseKeys = Models.SettingsModel.DefaultCloseKeys;
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
        App.Settings.TouchpadActivateGestures = Models.SettingsModel.DefaultTouchpadActivateGestures;
        App.Settings.TouchpadCancelSwipe = true;
        App.Settings.TouchpadCycleGestures = Models.SettingsModel.DefaultTouchpadCycleGestures;
        App.Settings.TouchpadCommitGestures = Models.SettingsModel.DefaultTouchpadCommitGestures;
        App.Settings.TouchpadReverse = false;
        App.Settings.TouchpadSensitivity = 50;
        App.Settings.TouchpadSmoothing = 35;
        App.Settings.TouchpadContinuous = false;
        App.Settings.WindowSnap = true;
    }
}
