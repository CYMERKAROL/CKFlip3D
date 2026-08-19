// ---------------------------------------------------------------------------
// The log page: everything CKFlip3D has recorded going wrong, folded one tile
// per problem.  Reached from the marks above the version in the sidebar rather
// than from the navigation list, because it is not a settings page and has
// nothing to apply.  Opening it is what marks its contents as read, so the
// marks stop drawing attention once someone has looked.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Windows;
using System.Windows.Controls;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

public partial class LogPage : UserControl
{
    public LogPage()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            DiagnosticsLog.Changed += OnLogChanged;
            DiagnosticsLog.Refresh();
            Render();
            // Read, therefore no longer new.
            DiagnosticsLog.MarkAllSeen();
        };
        Unloaded += (_, _) => DiagnosticsLog.Changed -= OnLogChanged;
    }

    private void OnLogChanged() => Dispatcher.BeginInvoke(Render);

    private void Render()
    {
        var groups = DiagnosticsLog.Groups;
        EntryList.ItemsSource = null;
        EntryList.ItemsSource = groups;

        bool any = groups.Count > 0;
        EmptyState.Visibility = any ? Visibility.Collapsed : Visibility.Visible;
        ClearAllButton.Visibility = any ? Visibility.Visible : Visibility.Collapsed;

        int errors = DiagnosticsLog.CountOf(DiagSeverity.Critical);
        int warnings = DiagnosticsLog.CountOf(DiagSeverity.Warning);
        int infos = DiagnosticsLog.CountOf(DiagSeverity.Info);

        var parts = new List<string>(3);
        if (errors > 0) parts.Add(errors == 1 ? "1 error" : $"{errors} errors");
        if (warnings > 0) parts.Add(warnings == 1 ? "1 warning" : $"{warnings} warnings");
        if (infos > 0) parts.Add(infos == 1 ? "1 notice" : $"{infos} notices");

        Summary.Text = parts.Count == 0
            ? "CKFlip3D has nothing to report."
            : string.Join(", ", parts) + ".";
    }

    private void ClearOne_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: DiagGroup group })
            DiagnosticsLog.Clear(group);
    }

    private void ClearAll_Click(object sender, RoutedEventArgs e) =>
        DiagnosticsLog.ClearAll();
}
