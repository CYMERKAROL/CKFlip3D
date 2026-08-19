using System.Globalization;
using System.Windows;
using System.Windows.Data;
using System.Windows.Media;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

// ---------------------------------------------------------------------------
// The log's severity colours and marks.
//
// Fixed colours rather than theme resources: red, amber and blue mean the same
// thing in every one of the five app themes, and a warning that took on the
// accent colour of a theme would stop reading as a warning.  Each is picked to
// stay legible on both the dark and the light surfaces.
//
// The marks are drawn as geometry rather than typed as characters. A glyph
// depends on a font being installed and on the platform's idea of what an
// emoji looks like; a Path is the same 18 pixels everywhere.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
internal static class SeverityPalette
{
    public static readonly Color Info     = Color.FromRgb(0x4C, 0x9A, 0xE0);
    public static readonly Color Warning  = Color.FromRgb(0xE8, 0xA3, 0x3D);
    public static readonly Color Critical = Color.FromRgb(0xE0, 0x5C, 0x50);

    public static Color Of(object? value) => value is DiagSeverity s ? Of(s) : Info;

    public static Color Of(DiagSeverity severity) => severity switch
    {
        DiagSeverity.Critical => Critical,
        DiagSeverity.Warning => Warning,
        _ => Info,
    };
}

/// <summary>Severity → the solid colour of its mark, border and code chip.</summary>
public sealed class SeverityBrushConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        var brush = new SolidColorBrush(SeverityPalette.Of(value));
        brush.Freeze();
        return brush;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}

/// <summary>
/// Severity → the tile's fill: the same hue at low alpha, so the card carries
/// its colour without competing with the text on it.
/// </summary>
public sealed class SeverityTintConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        Color c = SeverityPalette.Of(value);
        var brush = new SolidColorBrush(Color.FromArgb(0x22, c.R, c.G, c.B));
        brush.Freeze();
        return brush;
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}

/// <summary>Severity → the word for it.</summary>
public sealed class SeverityLabelConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => value is DiagSeverity s
            ? s switch
            {
                DiagSeverity.Critical => "Error",
                DiagSeverity.Warning => "Warning",
                _ => "Information",
            }
            : "Information";

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}

/// <summary>
/// Severity → its mark, on an 18×18 field: a warning triangle, a circled
/// cross for an error, a circled i for information.
/// </summary>
public sealed class SeverityGeometryConverter : IValueConverter
{
    // Triangle with a bang; circle with a cross; circle with an i.
    private const string Warning  = "M9,1.6 L17,15.4 H1 Z M9,6.6 V10.6 M9,12.6 V13.1";
    private const string Critical = "M9,1.6 A7.4,7.4 0 1 1 8.99,1.6 M6.2,6.2 L11.8,11.8 M11.8,6.2 L6.2,11.8";
    private const string Info     = "M9,1.6 A7.4,7.4 0 1 1 8.99,1.6 M9,8 V12.6 M9,5.4 V5.7";

    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        string data = value is DiagSeverity s
            ? s switch
            {
                DiagSeverity.Critical => Critical,
                DiagSeverity.Warning => Warning,
                _ => Info,
            }
            : Info;
        return Geometry.Parse(data);
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}

/// <summary>
/// The line under a tile: when it last happened, and how many times if that is
/// more than once. A count is only worth printing when it says something.
/// </summary>
public sealed class CountLabelConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
    {
        if (value is not DiagGroup g) return string.Empty;
        string when = string.IsNullOrEmpty(g.LastSeen) ? "" : Pretty(g.LastSeen);
        if (g.Count <= 1) return when;
        return string.IsNullOrEmpty(when)
            ? $"{g.Count} times"
            : $"{when} · {g.Count} times since {Pretty(g.FirstSeen)}";
    }

    /// <summary>"2026-08-13T10:04:11" → "13 Aug 10:04", today → "10:04".</summary>
    private static string Pretty(string stamp)
    {
        if (!DateTime.TryParse(stamp, CultureInfo.InvariantCulture,
                               DateTimeStyles.None, out var t))
            return stamp;
        return t.Date == DateTime.Today
            ? t.ToString("HH:mm", CultureInfo.CurrentCulture)
            : t.ToString("d MMM HH:mm", CultureInfo.CurrentCulture);
    }

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}

public sealed class BoolToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => value is true ? Visibility.Visible : Visibility.Collapsed;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}

/// <summary>Hides a row whose text is empty, instead of leaving a gap.</summary>
public sealed class StringToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        => string.IsNullOrWhiteSpace(value as string) ? Visibility.Collapsed : Visibility.Visible;

    public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        => Binding.DoNothing;
}
