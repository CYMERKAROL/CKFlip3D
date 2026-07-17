using System.Windows;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Attached "IsPressed" flag for navigation ListBoxItems. ListBoxItem has no
/// native pressed state, and selection must not change on MouseDown (the page
/// switch is committed on MouseUp inside the same item — see MainWindow).
/// The nav item template animates its pressed layer off this property.
/// </summary>
public static class NavBehavior
{
    public static readonly DependencyProperty IsPressedProperty =
        DependencyProperty.RegisterAttached(
            "IsPressed", typeof(bool), typeof(NavBehavior),
            new PropertyMetadata(false));

    public static bool GetIsPressed(DependencyObject obj) => (bool)obj.GetValue(IsPressedProperty);
    public static void SetIsPressed(DependencyObject obj, bool value) => obj.SetValue(IsPressedProperty, value);
}
