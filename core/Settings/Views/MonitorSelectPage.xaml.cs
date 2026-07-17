using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using CKFlip3D.Settings.Interop;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Visual monitor selector: draws each physical monitor to scale (position,
/// resolution, aspect, orientation) and lets the user pick the cascade target.
/// </summary>
public partial class MonitorSelectPage : UserControl
{
    private List<MonitorEntry> _monitors = new();

    public MonitorSelectPage()
    {
        InitializeComponent();
        Loaded += (_, _) => BuildMap();
    }

    private void BuildMap()
    {
        _monitors = MonitorInterop.EnumerateMonitors();
        MonitorCanvas.Children.Clear();

        if (_monitors.Count == 0) return;

        // Fit the virtual desktop bounding box into the canvas.
        int minX = _monitors.Min(m => m.Left), minY = _monitors.Min(m => m.Top);
        int maxX = _monitors.Max(m => m.Right), maxY = _monitors.Max(m => m.Bottom);
        double vw = maxX - minX, vh = maxY - minY;
        double scale = Math.Min(MonitorCanvas.Width / vw, MonitorCanvas.Height / vh) * 0.92;
        double offX = (MonitorCanvas.Width - vw * scale) / 2;
        double offY = (MonitorCanvas.Height - vh * scale) / 2;

        foreach (var m in _monitors)
        {
            var tile = BuildTile(m);
            tile.Width = Math.Max(40, m.Width * scale - 6);
            tile.Height = Math.Max(30, m.Height * scale - 6);
            Canvas.SetLeft(tile, offX + (m.Left - minX) * scale + 3);
            Canvas.SetTop(tile, offY + (m.Top - minY) * scale + 3);
            MonitorCanvas.Children.Add(tile);
        }

        UpdateSelectionVisuals();
    }

    private Border BuildTile(MonitorEntry m)
    {
        var info = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        info.Children.Add(new TextBlock
        {
            Text = (m.Index + 1).ToString(),
            FontFamily = new FontFamily("Segoe UI Light"),
            FontSize = 26,
            Foreground = Brushes.White,
            HorizontalAlignment = HorizontalAlignment.Center,
        });
        info.Children.Add(new TextBlock
        {
            Text = $"{m.Width} x {m.Height}",
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 11.5,
            Foreground = new SolidColorBrush(Color.FromArgb(0xD8, 0xFF, 0xFF, 0xFF)),
            HorizontalAlignment = HorizontalAlignment.Center,
        });
        info.Children.Add(new TextBlock
        {
            Text = m.AspectLabel + (m.IsPortrait ? " • portrait" : " • landscape"),
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 10.5,
            Foreground = new SolidColorBrush(Color.FromArgb(0xA8, 0xFF, 0xFF, 0xFF)),
            HorizontalAlignment = HorizontalAlignment.Center,
        });

        var badges = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 5, 0, 0),
        };
        if (m.IsPrimary) badges.Children.Add(MakeBadge("PRIMARY", Color.FromRgb(0x4F, 0x7E, 0xA6)));
        badges.Children.Add(MakeBadge("CKFLIP CASCADE", Color.FromRgb(0x2D, 0x7D, 0xC1))); // shown/hidden in UpdateSelectionVisuals
        badges.Children[^1].Visibility = Visibility.Collapsed;
        info.Children.Add(badges);

        var tile = new Border
        {
            CornerRadius = new CornerRadius(4),
            BorderThickness = new Thickness(1.5),
            BorderBrush = new SolidColorBrush(Color.FromArgb(0x90, 0x9D, 0xC2, 0xDE)),
            Cursor = Cursors.Hand,
            Tag = m.Index,
            Child = info,
            Background = new LinearGradientBrush(
                Color.FromArgb(0x58, 0x6E, 0xA5, 0xD0),
                Color.FromArgb(0x30, 0x3A, 0x60, 0x82), 90),
        };

        tile.MouseLeftButtonUp += (_, _) =>
        {
            App.Settings.CascadeMonitor = m.IsPrimary ? -1 : m.Index;
            UpdateSelectionVisuals();
        };
        tile.MouseEnter += (_, _) => { if (!IsSelected(m)) tile.BorderBrush = new SolidColorBrush(Color.FromRgb(0x8F, 0xC8, 0xF2)); };
        tile.MouseLeave += (_, _) => { if (!IsSelected(m)) tile.BorderBrush = new SolidColorBrush(Color.FromArgb(0x90, 0x9D, 0xC2, 0xDE)); };

        return tile;
    }

    private static Border MakeBadge(string text, Color bg) => new()
    {
        CornerRadius = new CornerRadius(3),
        Background = new SolidColorBrush(bg),
        Padding = new Thickness(5, 1.5, 5, 2),
        Margin = new Thickness(2, 0, 2, 0),
        Child = new TextBlock
        {
            Text = text,
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 9,
            FontWeight = FontWeights.SemiBold,
            Foreground = Brushes.White,
        },
    };

    private bool IsSelected(MonitorEntry m) =>
        App.Settings.CascadeMonitor == m.Index ||
        (App.Settings.CascadeMonitor < 0 && m.IsPrimary);

    private void UpdateSelectionVisuals()
    {
        foreach (var child in MonitorCanvas.Children)
        {
            if (child is not Border tile || tile.Tag is not int idx) continue;
            var m = _monitors[idx];
            bool selected = IsSelected(m);

            tile.BorderBrush = selected
                ? new SolidColorBrush(Color.FromRgb(0x63, 0xC6, 0xFF))
                : new SolidColorBrush(Color.FromArgb(0x90, 0x9D, 0xC2, 0xDE));
            tile.Effect = selected
                ? new System.Windows.Media.Effects.DropShadowEffect
                  { Color = Color.FromRgb(0x63, 0xC6, 0xFF), BlurRadius = 14, ShadowDepth = 0, Opacity = 0.8 }
                : null;

            // last badge in the tile is the cascade badge
            if (tile.Child is StackPanel info && info.Children[^1] is StackPanel badges)
                badges.Children[^1].Visibility = selected ? Visibility.Visible : Visibility.Collapsed;
        }

        int target = App.Settings.CascadeMonitor;
        SelectionText.Text = target < 0 || target >= _monitors.Count
            ? "Cascade target: Primary monitor"
            : $"Cascade target: Monitor {target + 1} ({_monitors[target].Width} x {_monitors[target].Height})";
    }

    private void UsePrimary_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.CascadeMonitor = -1;
        UpdateSelectionVisuals();
    }
}
