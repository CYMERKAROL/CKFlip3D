using System.Windows;
using System.Windows.Controls;
using System.Windows.Media.Animation;
using CKFlip3D.Installer.Engine;

namespace CKFlip3D.Installer.Views;

public partial class ProgressPage : UserControl
{
    private readonly MainWindow _owner;

    public ProgressPage(MainWindow owner, string title)
    {
        InitializeComponent();
        _owner = owner;
        TitleBlock.Text = title;
    }

    public void Report(InstallProgress p)
    {
        // Animate toward the new value so per-file jumps read smoothly.
        Bar.BeginAnimation(System.Windows.Controls.Primitives.RangeBase.ValueProperty,
            new DoubleAnimation(p.Percent, TimeSpan.FromMilliseconds(180))
            { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut } });

        StatusBlock.Text = p.Status;
        DetailBlock.Text = p.Detail;
        PercentBlock.Text = $"{p.Percent:0}%";
    }

    private void BtnCancel_Click(object sender, RoutedEventArgs e)
    {
        BtnCancel.IsEnabled = false;
        StatusBlock.Text = "Cancelling…";
        DetailBlock.Text = "rolling back changes";
        _owner.RequestCancel();
    }
}
