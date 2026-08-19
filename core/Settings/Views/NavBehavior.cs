// ---------------------------------------------------------------------------
// An attached pressed-state flag for the navigation items.  ListBoxItem has no
// pressed state of its own, and selection must not change on MouseDown, since
// the page switch commits on MouseUp inside the same item (see MainWindow), so
// the nav template animates its pressed layer off this instead.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Windows;

namespace CKFlip3D.Settings.Views;

public static class NavBehavior
{
    public static readonly DependencyProperty IsPressedProperty =
        DependencyProperty.RegisterAttached(
            "IsPressed", typeof(bool), typeof(NavBehavior),
            new PropertyMetadata(false));

    public static bool GetIsPressed(DependencyObject obj) => (bool)obj.GetValue(IsPressedProperty);
    public static void SetIsPressed(DependencyObject obj, bool value) => obj.SetValue(IsPressedProperty, value);
}
