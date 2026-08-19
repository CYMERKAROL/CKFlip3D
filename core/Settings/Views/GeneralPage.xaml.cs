// ---------------------------------------------------------------------------
// The General page: startup behaviour, the launch shortcut, quality options,
// and the state of the core process itself.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

public partial class GeneralPage : UserControl
{
    private bool _syncing;

    public GeneralPage()
    {
        InitializeComponent();
        LoadUacShield();
        Loaded += (_, _) => { SyncFromModel(); UpdateExcludedSummary(); UpdateMaxWindowsHint(); };
        App.Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(Models.SettingsModel.PerfProfile)
                               or nameof(Models.SettingsModel.AutoPerfTune)
                               or nameof(Models.SettingsModel.StartDelayMs)
                               or nameof(Models.SettingsModel.WindowSnap)
                               or nameof(Models.SettingsModel.PointerInCascade)
                               or null)
                SyncFromModel();
            if (e.PropertyName is nameof(Models.SettingsModel.ExcludedApps) or null)
                UpdateExcludedSummary();
            if (e.PropertyName is nameof(Models.SettingsModel.VisualPreset)
                               or nameof(Models.SettingsModel.MaxWindows)
                               or null)
                UpdateMaxWindowsHint();
        };
    }

    // ---- Window snap -------------------------------------------------------
    // Free movement (snap OFF) is the drag button's feature, and that button
    // lives behind the pointer master, so the two only make sense together.
    //
    // The switch is NOT disabled while the master is off, and nothing is said
    // in advance: snapping ON is the default and works perfectly well without
    // a mouse in the cascade, so a warning sitting under a setting that is
    // behaving itself is just noise. It appears the moment the combination is
    // actually asked for — and then Apply steps aside until it is resolved
    // (MainWindow.UpdateApplyBar, SettingsModel.WindowSnapSatisfied), so the
    // state can be reached and looked at, but never saved.

    private void SyncWindowSnap()
    {
        SnapToggle.IsChecked = App.Settings.WindowSnap;
        SnapRequirement.Visibility = App.Settings.WindowSnapSatisfied
            ? Visibility.Collapsed : Visibility.Visible;
    }

    private void SnapToggle_Changed(object sender, RoutedEventArgs e)
    {
        if (_syncing) return;
        App.Settings.WindowSnap = SnapToggle.IsChecked == true;
    }

    private void EnablePointer_Click(object sender, RoutedEventArgs e) =>
        App.Settings.PointerInCascade = true;

    /// <summary>
    /// The count means something different per preset: the cascade recedes
    /// into depth, while Cover Flow is one row across one screen and has to
    /// divide it. Say which one is in play instead of leaving the jump to 5 on
    /// preset change looking arbitrary.
    /// </summary>
    private void UpdateMaxWindowsHint() =>
        MaxWindowsHint.Text = App.Settings.VisualPreset == 1
            ? "How many windows the switcher shows at once (2–10). Cover Flow "
              + "divides one screen between them evenly, so every window in the "
              + "row shows the same slice of itself and the whole deck gets "
              + "smaller as you ask for more. Five is the size it opens at; ten "
              + "still shows ten."
            : "How many windows the 3D cascade shows at once (2–10).";

    // ---- Cascade exclusion list --------------------------------------------

    private void UpdateExcludedSummary()
    {
        int count = App.Settings.ExcludedAppsList.Count;
        ExcludedSummary.Text = count == 0
            ? "No applications are excluded. Windows of listed applications never appear in the 3D cascade."
            : $"{count} application(s) excluded. Their windows never appear in the 3D cascade.";
    }

    private void ManageExcluded_Click(object sender, RoutedEventArgs e)
    {
        if (Window.GetWindow(this) is MainWindow main)
            main.PushSubPage(new ExcludedAppsPage(), "Exclusion list");
    }

    // ---- stock UAC shield icon (asked from Windows, never extracted) ------

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct SHSTOCKICONINFO
    {
        public uint cbSize;
        public IntPtr hIcon;
        public int iSysImageIndex;
        public int iIcon;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)]
        public string szPath;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern int SHGetStockIconInfo(uint siid, uint flags, ref SHSTOCKICONINFO info);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr hIcon);

    private const uint SIID_SHIELD = 77;
    private const uint SHGSI_ICON = 0x100;
    private const uint SHGSI_SMALLICON = 0x1;

    private void LoadUacShield()
    {
        var info = new SHSTOCKICONINFO { cbSize = (uint)Marshal.SizeOf<SHSTOCKICONINFO>() };
        if (SHGetStockIconInfo(SIID_SHIELD, SHGSI_ICON | SHGSI_SMALLICON, ref info) != 0
            || info.hIcon == IntPtr.Zero)
            return;
        try
        {
            UacShield.Source = Imaging.CreateBitmapSourceFromHIcon(
                info.hIcon, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
        }
        finally
        {
            DestroyIcon(info.hIcon);
        }
    }

    private void SyncFromModel()
    {
        _syncing = true;
        PerfCombo.SelectedIndex = App.Settings.PerfProfile + 1; // -1=Auto → index 0
        SyncStartDelay();
        SyncWindowSnap();

        // Warn up front when the startup toggle cannot work because the
        // core exe is nowhere to be found.
        if (CoreLocator.FindCoreExe() == null)
        {
            StartupWarning.Text = "CKFlip3D.exe could not be located — the startup entry cannot be created.";
            StartupWarning.Visibility = Visibility.Visible;
        }
        else
        {
            StartupWarning.Visibility = Visibility.Collapsed;
        }

        // Same courtesy for the launch shortcut: say up front that there is
        // nothing to point it at, rather than failing on the click.
        bool haveLauncher = CoreLocator.FindLauncherExe() != null;
        LaunchShortcutButton.IsEnabled = haveLauncher;
        LaunchShortcutWarning.Text = haveLauncher ? string.Empty
            : "CKFlip3D.Launch.exe could not be located — the shortcut cannot be created. "
              + "It ships with CKFlip3D 1.6 and later; reinstall to get it.";
        LaunchShortcutWarning.Visibility = haveLauncher
            ? Visibility.Collapsed : Visibility.Visible;
        _syncing = false;
    }

    // ---- Launch shortcut ----------------------------------------------------
    // Not part of the Apply flow: this button writes a file and that is the
    // whole of it. There is deliberately no "remove" here — once the shortcut
    // is on the desktop it is an ordinary shortcut, and deleting it is what
    // Explorer is for. A settings page offering to delete a file the user may
    // have since moved, renamed or pinned would be claiming an ownership it
    // does not have.

    private void CreateLaunchShortcut_Click(object sender, RoutedEventArgs e)
    {
        string? error = LaunchShortcutService.Create();
        if (error == null)
        {
            LaunchShortcutNote.Text = "Created on your desktop as “CKFlip3D Cascade”. "
                + "Drag it onto the taskbar to keep the cascade one click away.";
            return;
        }

        DiagnosticsLog.Append(DiagnosticsLog.Code.LaunchShortcutFailed,
            DiagSeverity.Warning, "The launch shortcut could not be created", error);

        var body = new TextBlock
        {
            Text = "The launch shortcut could not be created:\n\n" + error,
            TextWrapping = TextWrapping.Wrap,
            FontSize = 13,
        };
        if (TryFindResource("TextPrimaryBrush") is Brush b) body.Foreground = b;
        if (Window.GetWindow(this) is MainWindow main)
            main.ShowModal("Launch shortcut failed", body, ("OK", true, null));
    }

    // ---- Start delay row ---------------------------------------------------
    // Colour bands mirror what the core can realistically do: WGC delivers
    // first frames on compositor ticks, so anything under one vsync (~16 ms)
    // risks tiles missing their capture; under ~6 ms it is almost certain.
    // While Auto performance tune is on, the core derives the value from the
    // measured refresh rate + perf tier and the slider is informational only.

    private static readonly Brush DelayRed    = new SolidColorBrush(Color.FromRgb(0xE0, 0x5C, 0x50));
    private static readonly Brush DelayOrange = new SolidColorBrush(Color.FromRgb(0xE8, 0xA3, 0x3D));
    private static readonly Brush DelayGreen  = new SolidColorBrush(Color.FromRgb(0x7F, 0xCB, 0x6F));

    private void SyncStartDelay()
    {
        bool auto = App.Settings.AutoPerfTune;
        int v = App.Settings.StartDelayMs;

        StartDelaySlider.IsEnabled = !auto;
        StartDelayPanel.Opacity = auto ? 0.45 : 1.0;

        if (auto)
        {
            StartDelayValue.Text = "Auto";
            StartDelayValue.Foreground =
                TryFindResource("TextSecondaryBrush") as Brush ?? DelayGreen;
            StartDelayWarning.Visibility = Visibility.Collapsed;
            return;
        }

        StartDelayValue.Text = $"{v} ms";
        StartDelayValue.Foreground = v < 6 ? DelayRed : v < 16 ? DelayOrange : DelayGreen;
        StartDelayWarning.Visibility = v < 16 ? Visibility.Visible : Visibility.Collapsed;
    }

    private void PerfCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || PerfCombo.SelectedIndex < 0) return;
        App.Settings.PerfProfile = PerfCombo.SelectedIndex - 1;
    }
}
