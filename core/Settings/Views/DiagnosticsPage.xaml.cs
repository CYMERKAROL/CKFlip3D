// ---------------------------------------------------------------------------
// The Diagnostics page: what the machine looks like from CKFlip3D's side, a
// summary of the log, and a report the user can copy out in one click.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
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
        Loaded += (_, _) => { Refresh(); RefreshLogSummary(); };
    }

    // ---- Log ---------------------------------------------------------------

    private void RefreshLogSummary()
    {
        DiagnosticsLog.Refresh();
        int errors = DiagnosticsLog.CountOf(DiagSeverity.Critical);
        int warnings = DiagnosticsLog.CountOf(DiagSeverity.Warning);
        int infos = DiagnosticsLog.CountOf(DiagSeverity.Info);
        int total = errors + warnings + infos;

        LogSummary.Text = total == 0
            ? "Everything CKFlip3D records going wrong ends up here. Nothing is "
              + "listed at the moment."
            : $"Everything CKFlip3D records going wrong ends up here — currently "
              + $"{errors} error(s), {warnings} warning(s) and {infos} notice(s).";
    }

    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is MainWindow main)
            main.PushSubPage(new LogPage(), "Log");
    }

    private void Refresh()
    {
        _snapshot = DiagnosticsService.Collect();
        var s = _snapshot;

        RuntimeRows.Children.Clear();
        AddRow(RuntimeRows, "Version", s.SettingsVersion);
        AddRow(RuntimeRows, "Windows", s.WindowsVersion);
        AddRow(RuntimeRows, "Settings process", s.ProcessInfo);
        AddRow(RuntimeRows, "Core executable", s.CoreExe);
        AddRow(RuntimeRows, "Core process", s.CoreProcess);
        AddRow(RuntimeRows, "Core message window", s.CoreWindow);
        AddRow(RuntimeRows, "D3D / WGC / hook", s.RuntimeStatusNote, last: true);

        DisplayRows.Children.Clear();
        AddRow(DisplayRows, "Monitor count", s.MonitorCount.ToString());
        AddRow(DisplayRows, "Virtual screen rect", s.VirtualScreenRect);
        AddRow(DisplayRows, "Primary monitor rect", s.PrimaryMonitorRect);
        AddRow(DisplayRows, "DPI / scaling", s.DpiInfo);
        foreach (var m in s.Monitors)
            AddRow(DisplayRows, $"Monitor {m.Index + 1}",
                $"{m.DeviceName}  {m.Width}x{m.Height} {m.AspectLabel} "
                + $"at ({m.Left},{m.Top}){(m.IsPrimary ? "  [primary]" : "")}");
        AddRow(DisplayRows, "Taskbar detection", s.TaskbarInfo, last: true);

        InputRows.Children.Clear();
        AddRow(InputRows, "Summary", s.PointingSummary);
        foreach (var d in s.InputDevices)
            AddRow(InputRows, d.Kind, $"{d.Name}  —  {d.Detail}");
        AddRow(InputRows, "Windows gesture", s.TouchpadGestureState);
        AddRow(InputRows, "Trigger settings", s.TriggerSummary, last: true);

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
