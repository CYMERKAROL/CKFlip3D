using System.Windows;
using System.Windows.Controls;

namespace CKFlip3D.Settings.Views;

public partial class AppearancePage : UserControl
{
    private bool _syncing;

    public AppearancePage()
    {
        InitializeComponent();
        Loaded += (_, _) => SyncFromModel();
        App.Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(Models.SettingsModel.AppTheme)
                               or nameof(Models.SettingsModel.VisualPreset)
                               or nameof(Models.SettingsModel.VsyncLivePreview)
                               or nameof(Models.SettingsModel.Animations)
                               or nameof(Models.SettingsModel.AnimCycle)
                               or null)
                SyncFromModel();
        };
    }

    private void SyncFromModel()
    {
        _syncing = true;
        ThemeCombo.SelectedIndex = Math.Clamp(App.Settings.AppTheme, 0, 4);
        PresetCombo.SelectedIndex = Math.Clamp(App.Settings.VisualPreset, 0, 1);
        // Performance warning mirrors the start-delay pattern: visible only
        // while the option is actually on.
        VsyncWarning.Visibility = App.Settings.VsyncLivePreview
            ? Visibility.Visible : Visibility.Collapsed;
        // The miniature runs the core's own geometry and cycle animation, so
        // say which of the two states it is in.
        PreviewHint.Text = App.Settings.Animations && App.Settings.AnimCycle
            ? "A miniature of the switcher at this screen's proportions, replaying one "
              + "Tab press every couple of seconds. It follows the visual preset, the "
              + "background opacity and blur, reflections, the desktop tile and the "
              + "window count."
            : "Cycle animation is off, so the miniature holds its rest pose — exactly "
              + "what the switcher does: it snaps between windows instead of gliding.";
        _syncing = false;
    }

    private void ThemeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || ThemeCombo.SelectedIndex < 0) return;
        // MainWindow listens for this property and runs the faded swap.
        App.Settings.AppTheme = ThemeCombo.SelectedIndex;
    }

    /// <summary>
    /// Stack size each preset is designed around. Cover Flow is a row that
    /// has to fit across the screen, so it reads best at five; the cascade
    /// recedes into depth and carries ten comfortably.
    /// </summary>
    private static uint DefaultWindowsFor(int preset) => preset == 1 ? 5u : 10u;

    private void PresetCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || PresetCombo.SelectedIndex < 0) return;
        int previous = App.Settings.VisualPreset;
        if (previous == PresetCombo.SelectedIndex) return;

        App.Settings.VisualPreset = PresetCombo.SelectedIndex;

        // Move the window count to the new preset's default, but never
        // overwrite a number the user chose themselves: only a count still
        // sitting on the OLD preset's default gets carried across.
        if (App.Settings.MaxWindows == DefaultWindowsFor(previous))
            App.Settings.MaxWindows = DefaultWindowsFor(PresetCombo.SelectedIndex);
    }
}
