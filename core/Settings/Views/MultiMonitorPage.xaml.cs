using System.Text;
using System.Windows;
using System.Windows.Controls;
using CKFlip3D.Settings.Interop;

namespace CKFlip3D.Settings.Views;

public partial class MultiMonitorPage : UserControl
{
    private bool _syncing;

    public MultiMonitorPage()
    {
        InitializeComponent();
        Loaded += (_, _) => Refresh();
    }

    private void Refresh()
    {
        _syncing = true;
        TaskbarCombo.SelectedIndex = Math.Clamp(App.Settings.SecondaryTaskbarMode, 0, 2);
        _syncing = false;

        var monitors = MonitorInterop.EnumerateMonitors();

        var sb = new StringBuilder();
        sb.Append(monitors.Count == 1
            ? "1 monitor detected. "
            : $"{monitors.Count} monitors detected. ");
        foreach (var m in monitors)
        {
            sb.Append($"Monitor {m.Index + 1}: {m.Width} x {m.Height} ({m.AspectLabel}");
            if (m.IsPortrait) sb.Append(", portrait");
            if (m.IsPrimary) sb.Append(", primary");
            sb.Append(").  ");
        }
        sb.Append($"All monitors dim to the Appearance “Background opacity” level ({App.Settings.BackgroundOpacity}%).");
        SetupSummary.Text = sb.ToString();
    }

    private void TaskbarCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || TaskbarCombo.SelectedIndex < 0) return;
        App.Settings.SecondaryTaskbarMode = TaskbarCombo.SelectedIndex;
    }

    // Disabled until the core supports a configurable cascade target; the
    // monitor-selector sub-page (MonitorSelectPage) stays in the project for
    // that milestone.
    private void ChooseMonitor_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is MainWindow main)
            main.PushSubPage(new MonitorSelectPage(), "Choose cascade monitor");
    }
}
