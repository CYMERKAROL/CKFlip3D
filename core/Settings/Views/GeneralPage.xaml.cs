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
        Loaded += (_, _) => SyncFromModel();
        App.Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(Models.SettingsModel.PerfProfile)
                               or nameof(Models.SettingsModel.AutoPerfTune)
                               or nameof(Models.SettingsModel.StartDelayMs)
                               or null)
                SyncFromModel();
        };
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
        _syncing = false;
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
