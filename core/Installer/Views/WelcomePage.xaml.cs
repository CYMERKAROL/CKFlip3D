// ---------------------------------------------------------------------------
// First page of the installer.  Nothing has happened yet, so all it does is
// hand the user on to the options.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
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
