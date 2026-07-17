using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;
using CKFlip3D.Settings.Interop;

namespace CKFlip3D.Settings.Services;

/// <summary>
/// Application theme switcher. Each theme is a ResourceDictionary in
/// Theme/Themes/ defining the same brush keys; consumers use DynamicResource
/// so replacing the dictionary at MergedDictionaries[0] re-skins live UI.
/// </summary>
public static class ThemeService
{
    public sealed record ThemeDef(string Name, string File, Color Tint, byte TintAlpha, bool Dark);

    public static readonly ThemeDef[] Themes =
    {
        new("Skeuomorphic Dark Mode",  "SkeuoDark.xaml",     Color.FromRgb(0x0C, 0x14, 0x1E), 0x9A, true),
        new("Skeuomorphic White Mode", "SkeuoWhite.xaml",    Color.FromRgb(0xF2, 0xF6, 0xFA), 0x80, false),
        new("Minimalism Dark Mode",    "MinimalDark.xaml",   Color.FromRgb(0x00, 0x00, 0x00), 0xB4, true),
        new("Minimalism White Mode",   "MinimalWhite.xaml",  Color.FromRgb(0xFF, 0xFF, 0xFF), 0xA8, false),
        new("Glassmorphism",           "Glassmorphism.xaml", Color.FromRgb(0x10, 0x18, 0x22), 0x48, true),
    };

    public static int CurrentIndex { get; private set; }

    public static ThemeDef Current => Themes[CurrentIndex];

    /// <summary>Swap the theme dictionary (slot 0 of the app merged dictionaries).</summary>
    public static void Apply(int index)
    {
        index = Math.Clamp(index, 0, Themes.Length - 1);
        var dict = new ResourceDictionary
        {
            Source = new Uri($"pack://application:,,,/Theme/Themes/{Themes[index].File}"),
        };
        Application.Current.Resources.MergedDictionaries[0] = dict;
        CurrentIndex = index;
    }

    /// <summary>
    /// Theme switch with a smooth fade: dip the window content, swap the
    /// dictionary + DWM backdrop hints at the dip, fade back in.
    /// </summary>
    public static void SwitchWithFade(int index, FrameworkElement? fadeTarget, Action? onSwapped = null)
    {
        index = Math.Clamp(index, 0, Themes.Length - 1);
        if (index == CurrentIndex)
        {
            onSwapped?.Invoke();
            return;
        }

        if (fadeTarget == null)
        {
            Apply(index);
            onSwapped?.Invoke();
            return;
        }

        var dip = new DoubleAnimation(0.35, TimeSpan.FromMilliseconds(140))
        { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseIn } };
        dip.Completed += (_, _) =>
        {
            Apply(index);
            onSwapped?.Invoke();
            fadeTarget.BeginAnimation(UIElement.OpacityProperty,
                new DoubleAnimation(1, TimeSpan.FromMilliseconds(260))
                { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut } });
        };
        fadeTarget.BeginAnimation(UIElement.OpacityProperty, dip);
    }
}
