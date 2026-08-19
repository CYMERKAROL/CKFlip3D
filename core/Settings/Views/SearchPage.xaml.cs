// ---------------------------------------------------------------------------
// Type-to-filter settings, and the draggable preview that places the search
// field.  A page of its own because it changes what the stack contains rather
// than what a key is bound to.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;

namespace CKFlip3D.Settings.Views;

public partial class SearchPage : UserControl
{
    private bool _syncing;

    public SearchPage()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            App.Settings.PropertyChanged += OnSettingsChanged;
            Sync();
        };
        Unloaded += (_, _) => App.Settings.PropertyChanged -= OnSettingsChanged;
    }

    private void OnSettingsChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(Models.SettingsModel.SearchEnabled)
                           or nameof(Models.SettingsModel.HotkeyToggleMode)
                           or nameof(Models.SettingsModel.ActivationHotkey)
                           or null)
            Sync();
    }

    /// <summary>
    /// Searching depends on the cascade surviving the hotkey being released:
    /// a word cannot be typed while Win+Tab is held down, and the moment the
    /// modifier goes up the classic binding commits — taking the rest of the
    /// word with it, straight into Windows as Win+key shortcuts.
    ///
    /// A binding with no modifier is inherently toggle in the core, so it
    /// already satisfies this and no warning is shown for it.
    ///
    /// The dependency itself is ENFORCED in the model (SettingsModel), not
    /// here — this page only has to explain it.  Enforcing it on the page it
    /// happens to appear on meant the correction waited for someone to open
    /// that page, and then needed an Apply of its own.
    /// </summary>
    private static bool ToggleSatisfied =>
        App.Settings.HotkeyToggleMode
        || !App.Settings.ActivationHotkey.Contains('+');

    private void Sync()
    {
        _syncing = true;

        bool ok = ToggleSatisfied;
        SearchToggle.IsEnabled = ok;
        SearchToggle.IsChecked = App.Settings.SearchEnabled;
        ToggleRequirement.Visibility = ok ? Visibility.Collapsed : Visibility.Visible;

        _syncing = false;
    }

    private void SearchToggle_Changed(object sender, RoutedEventArgs e)
    {
        if (_syncing || !SearchToggle.IsEnabled) return;
        App.Settings.SearchEnabled = SearchToggle.IsChecked == true;
    }

    private void EnableToggle_Click(object sender, RoutedEventArgs e) =>
        App.Settings.HotkeyToggleMode = true;

    private void RestorePlacement_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.SearchPosX = 50;
        App.Settings.SearchPosY = 94;
        App.Settings.SearchScale = 100;
    }

    private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.SearchEnabled = false;
        App.Settings.SearchBox = true;
        App.Settings.SearchMatchProcess = true;
        RestorePlacement_Click(sender, e);
    }
}
