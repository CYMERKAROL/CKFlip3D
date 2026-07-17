using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace CKFlip3D.Installer.Views;

public partial class FinishPage : UserControl
{
    private readonly MainWindow _owner;
    private readonly MainWindow.FinishState _state;

    public FinishPage(MainWindow owner, MainWindow.FinishState state)
    {
        InitializeComponent();
        _owner = owner;
        _state = state;

        TitleBlock.Text = state.Title;
        MessageBlock.Text = state.Message;
        LaunchPanel.Visibility = state.ShowLaunchButtons ? Visibility.Visible : Visibility.Collapsed;

        if (!state.ShowLaunchButtons
            && !state.Title.Contains("complete", StringComparison.OrdinalIgnoreCase))
        {
            // Warning glyph + danger tint for the cancelled/failed/interrupted states.
            GlyphBlock.Text = "";
            if (TryFindResource("DangerTextBrush") is Brush danger)
                GlyphBlock.Foreground = danger;
        }
    }

    private void BtnLaunchCore_Click(object sender, RoutedEventArgs e) =>
        Launch(_state.Context?.CoreExePath);

    private void BtnLaunchSettings_Click(object sender, RoutedEventArgs e) =>
        Launch(_state.Context?.SettingsExePath);

    private void Launch(string? path)
    {
        if (path == null || !File.Exists(path)) return;
        try
        {
            Process.Start(new ProcessStartInfo(path)
            {
                UseShellExecute = true,
                WorkingDirectory = Path.GetDirectoryName(path)!,
            });
            _owner.Close();
        }
        catch { /* declined UAC or similar — stay on the finish page */ }
    }

    private void BtnClose_Click(object sender, RoutedEventArgs e) => _owner.Close();
}
