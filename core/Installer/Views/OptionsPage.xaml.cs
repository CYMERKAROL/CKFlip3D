using System.IO;
using System.Windows;
using System.Windows.Controls;
using CKFlip3D.Installer.Engine;
using Microsoft.Win32;

namespace CKFlip3D.Installer.Views;

public partial class OptionsPage : UserControl
{
    private readonly MainWindow _owner;
    private readonly InstallContext _context;

    public OptionsPage(MainWindow owner, InstallContext context)
    {
        InitializeComponent();
        _owner = owner;
        _context = context;

        PathBox.Text = _context.InstallDir;
        StartMenuToggle.IsChecked = _context.CreateStartMenuFolder;
        DesktopToggle.IsChecked = _context.CreateDesktopShortcut;
        UpdateSpaceInfo();
    }

    private void BtnBrowse_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog
        {
            Title = "Choose the CKFlip3D install folder",
            InitialDirectory = Directory.Exists(PathBox.Text)
                ? PathBox.Text
                : Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
        };
        if (dialog.ShowDialog(_owner) == true)
        {
            // Selecting e.g. "Program Files" directly appends the app folder,
            // so a generic pick never installs loose into a shared dir.
            string chosen = dialog.FolderName;
            if (!Path.GetFileName(chosen.TrimEnd('\\'))
                    .Equals(InstallContext.AppName, StringComparison.OrdinalIgnoreCase))
                chosen = Path.Combine(chosen, InstallContext.AppName);
            PathBox.Text = chosen;
        }
    }

    private void PathBox_TextChanged(object sender, TextChangedEventArgs e) => UpdateSpaceInfo();

    private void UpdateSpaceInfo()
    {
        try
        {
            string? root = Path.GetPathRoot(Path.GetFullPath(PathBox.Text));
            if (root != null)
            {
                var drive = new DriveInfo(root);
                SpaceInfo.Text = $"Free space on {drive.Name.TrimEnd('\\')}: "
                    + $"{drive.AvailableFreeSpace / (1024.0 * 1024 * 1024):0.0} GB";
                return;
            }
        }
        catch { }
        SpaceInfo.Text = "Enter a full path, e.g. C:\\Program Files\\CKFlip3D";
    }

    private void BtnInstall_Click(object sender, RoutedEventArgs e)
    {
        _context.InstallDir = PathBox.Text.Trim();
        _context.CreateStartMenuFolder = StartMenuToggle.IsChecked == true;
        _context.CreateDesktopShortcut = DesktopToggle.IsChecked == true;

        // Update flow: the chosen folder already holds a CKFlip3D install —
        // confirm before stopping the running app and replacing its files.
        if (InstallContext.IsExistingInstall(_context.InstallDir))
        {
            _owner.ShowModal("Update existing installation?",
                $"CKFlip3D is already installed in:\n{_context.InstallDir}\n\n"
                + "Do you want to update it? Any running CKFlip3D instance will be "
                + "closed and its files replaced with the new version. Your settings "
                + "are kept.",
                ("Update", true, () =>
                {
                    _context.IsUpdate = true;
                    _owner.StartInstall();
                }),
                ("Cancel", false, null));
            return;
        }

        _context.IsUpdate = false;
        _owner.StartInstall();
    }

    private void BtnBack_Click(object sender, RoutedEventArgs e) => _owner.NavigateWelcome();

    private void BtnCancel_Click(object sender, RoutedEventArgs e) => _owner.Close();
}
