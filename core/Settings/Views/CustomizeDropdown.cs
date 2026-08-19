// ---------------------------------------------------------------------------
// Housekeeping for the "Customize..." dropdowns.  A Popup lives in a window of
// its own, so disabling the row it belongs to, or navigating away from the
// page, has to close it explicitly or it hangs there over nothing.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Windows;
using System.Windows.Controls.Primitives;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Housekeeping for the "Customize…" dropdowns — the AeroComboToggle-faced
/// ToggleButtons whose <see cref="System.Windows.Controls.Primitives.Popup"/>
/// is bound to their IsChecked (Appearance → Selected window label,
/// Animations).
///
/// Those rows sit inside a container whose IsEnabled follows the feature's
/// MASTER switch, so turning the feature off greys the whole dropdown out.
/// What it did NOT do was close it: IsChecked is a local value set by the
/// click, nothing was clearing it, and the popup is a window of its own — so
/// the panel stayed on screen, greyed and inert, hanging over a feature that
/// was no longer there.  Disabling a control has to take its popup with it.
///
/// Unloading does too.  A Popup lives in its own top-level window rather than
/// in the page's visual tree, so navigating away from a page with one open
/// left it floating above whatever page came next.
/// </summary>
public static class CustomizeDropdown
{
    /// <summary>
    /// Attach to the ToggleButton that faces the popup: it unchecks itself —
    /// and so closes the popup — whenever it is disabled or unloaded.
    /// </summary>
    public static readonly DependencyProperty AutoCloseProperty =
        DependencyProperty.RegisterAttached(
            "AutoClose", typeof(bool), typeof(CustomizeDropdown),
            new PropertyMetadata(false, OnAutoCloseChanged));

    public static bool GetAutoClose(DependencyObject obj) => (bool)obj.GetValue(AutoCloseProperty);
    public static void SetAutoClose(DependencyObject obj, bool value) => obj.SetValue(AutoCloseProperty, value);

    private static void OnAutoCloseChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ToggleButton toggle) return;

        // The handlers are attached once, with the property, and the toggle
        // outlives neither — these pages are rebuilt per visit, so there is
        // nothing here to detach later.
        if ((bool)e.NewValue)
        {
            toggle.IsEnabledChanged += OnIsEnabledChanged;
            toggle.Unloaded += OnUnloaded;
        }
        else
        {
            toggle.IsEnabledChanged -= OnIsEnabledChanged;
            toggle.Unloaded -= OnUnloaded;
        }
    }

    private static void OnIsEnabledChanged(object sender, DependencyPropertyChangedEventArgs e)
    {
        if (!(bool)e.NewValue) Close(sender);
    }

    private static void OnUnloaded(object sender, RoutedEventArgs e) => Close(sender);

    private static void Close(object sender)
    {
        if (sender is ToggleButton toggle && toggle.IsChecked == true)
            toggle.IsChecked = false;   // the popup's IsOpen is bound to this
    }
}
