using System.Windows;
using System.Windows.Controls;
using CKFlip3D.Installer.Engine;

namespace CKFlip3D.Installer.Views;

public partial class UninstallConfirmPage : UserControl
{
    private readonly MainWindow _owner;

    public UninstallConfirmPage(MainWindow owner)
    {
        InitializeComponent();
        _owner = owner;
        AppDataInfo.Text = "Deletes " + InstallContext.AppDataDir;
    }

    private void BtnUninstall_Click(object sender, RoutedEventArgs e) =>
        _owner.StartUninstall(RemoveDataToggle.IsChecked == true);

    private void BtnClose_Click(object sender, RoutedEventArgs e) => _owner.Close();
}
