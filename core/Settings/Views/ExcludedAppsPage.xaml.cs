using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Animation;
using Microsoft.Win32;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Editor for the cascade exclusion list (config key `excludedApps`,
/// ';'-separated). Windows of the listed executables never appear in the
/// cascade. Changes go through the regular dirty/Apply flow.
/// </summary>
public partial class ExcludedAppsPage : UserControl
{
    public ExcludedAppsPage()
    {
        InitializeComponent();
        Loaded += (_, _) => RefreshList();
    }

    private void RefreshList()
    {
        AppList.Items.Clear();
        foreach (var entry in App.Settings.ExcludedAppsList)
            AppList.Items.Add(entry);
        EmptyHint.Visibility = AppList.Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void CommitList()
    {
        App.Settings.SetExcludedAppsList(AppList.Items.Cast<string>());
        EmptyHint.Visibility = AppList.Items.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void AddExe_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog
        {
            Title = "Choose an application to exclude",
            Filter = "Applications (*.exe)|*.exe|All files (*.*)|*.*",
            CheckFileExists = true,
        };
        if (dlg.ShowDialog(Window.GetWindow(this)) != true) return;

        string path = dlg.FileName;
        if (path.Contains(';'))
        {
            ShowStatus("Paths containing ';' are not supported.");
            return;
        }

        bool dup = AppList.Items.Cast<string>()
            .Any(x => string.Equals(x, path, StringComparison.OrdinalIgnoreCase));
        if (dup)
        {
            ShowStatus("That application is already on the list.");
            return;
        }

        AppList.Items.Add(path);
        CommitList();
        ShowStatus("Added. Press Apply to make it active.");
    }

    private void RemoveSelected_Click(object sender, RoutedEventArgs e)
    {
        if (AppList.SelectedItem == null)
        {
            ShowStatus("Select an entry to remove first.");
            return;
        }
        AppList.Items.Remove(AppList.SelectedItem);
        CommitList();
        ShowStatus("Removed. Press Apply to make it active.");
    }

    private void ShowStatus(string text)
    {
        ListStatus.Text = text;
        ListStatus.BeginAnimation(OpacityProperty, new DoubleAnimation
        {
            From = 1,
            To = 0,
            BeginTime = TimeSpan.FromSeconds(2.5),
            Duration = TimeSpan.FromMilliseconds(600),
        });
        ListStatus.Opacity = 1;
    }
}
