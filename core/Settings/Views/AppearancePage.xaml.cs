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
                               or nameof(Models.SettingsModel.VsyncLivePreview)
                               or null)
                SyncFromModel();
        };
    }

    private void SyncFromModel()
    {
        _syncing = true;
        ThemeCombo.SelectedIndex = Math.Clamp(App.Settings.AppTheme, 0, 4);
        // Performance warning mirrors the start-delay pattern: visible only
        // while the option is actually on.
        VsyncWarning.Visibility = App.Settings.VsyncLivePreview
            ? Visibility.Visible : Visibility.Collapsed;
        _syncing = false;
    }

    private void ThemeCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || ThemeCombo.SelectedIndex < 0) return;
        // MainWindow listens for this property and runs the faded swap.
        App.Settings.AppTheme = ThemeCombo.SelectedIndex;
    }
}
