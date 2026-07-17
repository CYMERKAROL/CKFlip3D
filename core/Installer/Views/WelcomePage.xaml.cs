using System.Windows;
using System.Windows.Controls;

namespace CKFlip3D.Installer.Views;

public partial class WelcomePage : UserControl
{
    private readonly MainWindow _owner;

    public WelcomePage(MainWindow owner)
    {
        InitializeComponent();
        _owner = owner;
    }

    private void BtnNext_Click(object sender, RoutedEventArgs e) => _owner.NavigateOptions();

    private void BtnCancel_Click(object sender, RoutedEventArgs e) => _owner.Close();
}
