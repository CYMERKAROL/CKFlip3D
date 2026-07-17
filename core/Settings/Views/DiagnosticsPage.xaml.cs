using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

public partial class DiagnosticsPage : UserControl
{
    private DiagnosticsSnapshot? _snapshot;

    public DiagnosticsPage()
    {
        InitializeComponent();
        Loaded += (_, _) => Refresh();
    }

    private void Refresh()
    {
        _snapshot = DiagnosticsService.Collect();
        var s = _snapshot;

        RuntimeRows.Children.Clear();
        AddRow(RuntimeRows, "Version", s.SettingsVersion);
        AddRow(RuntimeRows, "Core executable", s.CoreExe);
        AddRow(RuntimeRows, "Core process", s.CoreProcess);
        AddRow(RuntimeRows, "Core message window", s.CoreWindow);
        AddRow(RuntimeRows, "D3D / WGC / hook", s.RuntimeStatusNote, last: true);

        DisplayRows.Children.Clear();
        AddRow(DisplayRows, "Monitor count", s.MonitorCount.ToString());
        AddRow(DisplayRows, "Virtual screen rect", s.VirtualScreenRect);
        AddRow(DisplayRows, "Primary monitor rect", s.PrimaryMonitorRect);
        AddRow(DisplayRows, "Taskbar detection", s.TaskbarInfo, last: true);

        ConfigRows.Children.Clear();
        AddRow(ConfigRows, "Config path", s.ConfigPath, last: true);
    }

    private void AddRow(StackPanel host, string label, string value, bool last = false)
    {
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(180) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var lbl = new TextBlock
        {
            Text = label,
            Style = (Style)FindResource("SettingLabelText"),
            Foreground = (Brush)FindResource("TextSecondaryBrush"),
        };
        var val = new TextBlock
        {
            Text = value,
            Style = (Style)FindResource("MonoValueText"),
            TextWrapping = TextWrapping.Wrap,
        };
        Grid.SetColumn(val, 1);
        grid.Children.Add(lbl);
        grid.Children.Add(val);
        host.Children.Add(grid);

        if (!last)
            host.Children.Add(new System.Windows.Shapes.Rectangle { Style = (Style)FindResource("RowSeparator") });
    }

    private void Refresh_Click(object sender, RoutedEventArgs e) => Refresh();

    private void Copy_Click(object sender, RoutedEventArgs e)
    {
        if (_snapshot == null) Refresh();
        try
        {
            Clipboard.SetText(_snapshot!.ToClipboardText());
            CopyFeedback.BeginAnimation(OpacityProperty, new DoubleAnimation
            {
                From = 1, To = 0,
                BeginTime = TimeSpan.FromSeconds(1.2),
                Duration = TimeSpan.FromMilliseconds(600),
            });
            CopyFeedback.Opacity = 1;
        }
        catch
        {
            // clipboard can be locked by another process; ignore
        }
    }
}
