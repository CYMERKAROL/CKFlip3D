// ---------------------------------------------------------------------------
// Placing the search field by dragging it, on a still miniature of the real
// cascade.  The core stores the position as percentages of the display, which
// is what keeps one setting right at 1080p and at 4K and is also exactly the
// kind of number nobody can picture.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using CKFlip3D.Settings.Interop;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Where the search field will sit, drawn over a still miniature of the real
/// cascade.
///
/// The core places the field from PERCENTAGES of the primary display (X is the
/// field's centre, Y its bottom edge), which is what makes one setting read
/// the same at 1080p and at 4K — but percentages are exactly the kind of
/// number nobody can picture. So this shows the actual stack, at the viewer's
/// own screen aspect, with the field where it will land, and lets it be
/// dragged there instead of dialled in.
///
/// The stack itself is <see cref="FlipPreview"/> in <see cref="FlipPreview.Still"/>
/// mode — the same geometry the Appearance page shows, following the same
/// Cascade / Cover Flow setting, just not cycling. Sketching a second cascade
/// here would have been a second thing to keep in step with the renderer, and
/// it would have looked subtly wrong the first time either changed.
/// </summary>
public sealed class SearchPreview : UserControl
{
    private readonly FlipPreview _stack = new() { Still = true };
    private readonly Canvas _overlay = new();
    private readonly Border _field = new();
    private readonly TextBlock _fieldText = new();
    private readonly TextBlock _caption = new();

    private bool _dragging;
    private bool _clamping;      // guards the keep-out write-back's re-entry
    private Point _grabOffset;   // pointer → field top-left, at grab time

    // The field's natural size in core pixels at 100 %: SearchBox builds
    // padX + glassSide + gap + fieldW + padX wide and padY + content + padY
    // tall at uiScale 1.  Mirrored here so the model is proportionally honest
    // rather than a guess.
    private const double kFieldPxW = 16 + 15 + 9 + 340 + 16;
    private const double kFieldPxH = 9 + 21 + 9;
    /// <summary>Mirrors SearchBox.cpp's kBaseScale — what "100 %" means.</summary>
    private const double kBaseScale = 1.30;

    private double _monW = 1920, _monH = 1080;

    public SearchPreview()
    {
        Focusable = false;

        _fieldText.Text = "Type to search...";
        _fieldText.FontFamily = new FontFamily("Segoe UI");
        _fieldText.VerticalAlignment = VerticalAlignment.Center;
        _fieldText.Margin = new Thickness(8, 0, 6, 0);
        _fieldText.TextTrimming = TextTrimming.CharacterEllipsis;
        _fieldText.Foreground = new SolidColorBrush(Color.FromRgb(0xDD, 0xE6, 0xEF));

        _field.CornerRadius = new CornerRadius(4);
        _field.BorderThickness = new Thickness(1);
        _field.Child = _fieldText;
        _field.Cursor = Cursors.SizeAll;
        // Fixed colours rather than theme brushes: this sits on the miniature
        // cascade, not on the settings surface, so it has to read against the
        // preview's own wallpaper whichever theme the app is wearing.
        _field.Background = new SolidColorBrush(Color.FromArgb(0xC8, 0x1C, 0x26, 0x33));
        _field.BorderBrush = new SolidColorBrush(Color.FromArgb(0xFF, 0x7A, 0xC4, 0xF0));

        _field.MouseLeftButtonDown += OnFieldDown;
        _field.MouseMove += OnFieldMove;
        _field.MouseLeftButtonUp += OnFieldUp;

        _overlay.ClipToBounds = true;
        // Transparent rather than null: the canvas has to be hit-testable so a
        // click anywhere on the screen model moves the field there — dragging
        // the box is the precise gesture, this is the quick one.
        _overlay.Background = Brushes.Transparent;
        _overlay.HorizontalAlignment = HorizontalAlignment.Left;
        _overlay.VerticalAlignment = VerticalAlignment.Top;
        _overlay.MouseLeftButtonDown += OnOverlayDown;
        _overlay.Children.Add(_field);

        _caption.FontFamily = new FontFamily("Segoe UI");
        _caption.FontSize = 11;
        _caption.FontStyle = FontStyles.Italic;
        _caption.Margin = new Thickness(0, 6, 0, 0);
        _caption.HorizontalAlignment = HorizontalAlignment.Left;
        _caption.SetResourceReference(TextBlock.ForegroundProperty, "TextFaintBrush");

        // The overlay must sit in exactly the miniature's coordinate space, so
        // the host is pinned to the miniature's own size — no slack for either
        // to drift in.
        var screen = new Grid { HorizontalAlignment = HorizontalAlignment.Left };
        screen.Children.Add(_stack);
        screen.Children.Add(_overlay);

        var host = new StackPanel { HorizontalAlignment = HorizontalAlignment.Left };
        host.Children.Add(screen);
        host.Children.Add(_caption);
        Content = host;

        _stack.ScreenSurface.SizeChanged += (_, _) => Relayout();

        Loaded += (_, _) =>
        {
            App.Settings.PropertyChanged += OnSettingsChanged;
            ReadMonitor();
            Relayout();
        };
        Unloaded += (_, _) => App.Settings.PropertyChanged -= OnSettingsChanged;
    }

    private void OnSettingsChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(Models.SettingsModel.SearchPosX)
                           or nameof(Models.SettingsModel.SearchPosY)
                           or nameof(Models.SettingsModel.SearchScale)
                           or nameof(Models.SettingsModel.SearchBox)
                           or null)
        {
            Relayout();
        }
        else if (e.PropertyName is nameof(Models.SettingsModel.VisualPreset)
                                or nameof(Models.SettingsModel.MaxWindows)
                                or nameof(Models.SettingsModel.ShowDesktopTile))
        {
            // The stack — and therefore the keep-out area — is about to change
            // shape.  Deferred one turn because the miniature listens to the
            // same event and has to have rebuilt before its bounds mean
            // anything; handler order between two subscribers is not something
            // to rely on.
            Dispatcher.BeginInvoke(new Action(Relayout),
                                   System.Windows.Threading.DispatcherPriority.Background);
        }
    }

    private void ReadMonitor()
    {
        var monitors = MonitorInterop.EnumerateMonitors();
        var primary = monitors.FirstOrDefault(m => m.IsPrimary) ?? monitors.FirstOrDefault();
        if (primary is { Width: > 0, Height: > 0 })
        {
            _monW = primary.Width;
            _monH = primary.Height;
            _caption.Text = $"{primary.Width} × {primary.Height} ({primary.AspectLabel}) — "
                          + "drag the field to place it";
        }
        else
        {
            _caption.Text = "Drag the field to place it";
        }
    }

    private void Relayout()
    {
        if (_clamping) return;   // re-entered by our own write-back

        double w = _stack.ScreenSurface.ActualWidth;
        double h = _stack.ScreenSurface.ActualHeight;
        if (w <= 0 || h <= 0)
        {
            w = _stack.ScreenSurface.Width;
            h = _stack.ScreenSurface.Height;
        }
        if (double.IsNaN(w) || w <= 0 || double.IsNaN(h) || h <= 0)
            return;

        _overlay.Width = w;
        _overlay.Height = h;

        // The field's size in MODEL units: its core pixel size, scaled by the
        // user's setting and by the same cascade-host factor the core applies
        // (clamp(monitorHeight / 1080, 1, 2.5)), then mapped onto the model.
        double uiScale = Math.Clamp(_monH / 1080.0, 1.0, 2.5)
                       * kBaseScale
                       * (App.Settings.SearchScale / 100.0);
        double fw = Math.Max(kFieldPxW * uiScale * (w / _monW), 24);
        double fh = Math.Max(kFieldPxH * uiScale * (h / _monH), 8);

        _field.Width = fw;
        _field.Height = fh;
        _field.Opacity = App.Settings.SearchBox ? 1.0 : 0.6;
        _fieldText.FontSize = Math.Clamp(fh * 0.5, 6, 13);

        // X is the centre, Y the bottom edge — the core's convention, so what
        // is dragged here is literally the number that gets saved.
        double cx = w * App.Settings.SearchPosX / 100.0;
        double by = h * App.Settings.SearchPosY / 100.0;
        var field = new Rect(Clamp(cx - fw / 2, 0, w - fw),
                             Clamp(by - fh, 0, h - fh), fw, fh);

        // Keep it off the windows it is filtering.  The forbidden area is the
        // stack's own projected bounds, so it is the right shape for whichever
        // layout is selected without either shape being described here.
        Rect cleared = PushOutOfStack(field, w, h);
        if (Math.Abs(cleared.X - field.X) > 0.5 || Math.Abs(cleared.Y - field.Y) > 0.5)
        {
            // Write the correction back, so what is stored is always a legal
            // placement — a slider dragged into the stack snaps out of it
            // rather than saving a position the cascade would sit on top of.
            _clamping = true;
            App.Settings.SearchPosX =
                (int)Math.Round(Math.Clamp((cleared.X + fw / 2) / w, 0, 1) * 100);
            App.Settings.SearchPosY =
                (int)Math.Round(Math.Clamp((cleared.Y + fh) / h, 0, 1) * 100);
            _clamping = false;
            field = cleared;
        }

        Canvas.SetLeft(_field, field.X);
        Canvas.SetTop(_field, field.Y);
    }

    /// <summary>
    /// Slide <paramref name="field"/> clear of the stack by the shortest move
    /// that still fits on screen.  All four directions are tried because the
    /// stack is not always at the bottom — a Cover Flow row sits mid-screen,
    /// and a two-window cascade leaves room above it.
    /// </summary>
    private Rect PushOutOfStack(Rect field, double w, double h)
    {
        Rect keepOut = _stack.StackBounds();
        if (keepOut.IsEmpty || !field.IntersectsWith(keepOut))
            return field;

        // Candidate positions, each just clear of one edge of the stack.
        Span<double> ys = [keepOut.Top - field.Height, keepOut.Bottom];
        Span<double> xs = [keepOut.Left - field.Width, keepOut.Right];

        Rect best = field;
        double bestCost = double.MaxValue;

        void Consider(double x, double y)
        {
            if (x < 0 || y < 0 || x + field.Width > w || y + field.Height > h)
                return;   // would leave the screen — not a placement at all
            double cost = Math.Abs(x - field.X) + Math.Abs(y - field.Y);
            if (cost < bestCost) { bestCost = cost; best = new Rect(x, y, field.Width, field.Height); }
        }

        foreach (double y in ys) Consider(field.X, y);
        foreach (double x in xs) Consider(x, field.Y);

        if (bestCost < double.MaxValue)
            return best;

        // The stack leaves nowhere that fits (a very tall keep-out on a short
        // model): park it at the bottom rather than refuse to place it.
        return new Rect(field.X, Math.Max(0, h - field.Height), field.Width, field.Height);
    }

    private static double Clamp(double v, double lo, double hi) =>
        hi < lo ? lo : Math.Clamp(v, lo, hi);

    // ---- Dragging -----------------------------------------------------------

    private void OnFieldDown(object sender, MouseButtonEventArgs e)
    {
        _dragging = true;
        _grabOffset = e.GetPosition(_field);
        _field.CaptureMouse();
        e.Handled = true;
    }

    private void OnFieldMove(object sender, MouseEventArgs e)
    {
        if (!_dragging) return;
        Point p = e.GetPosition(_overlay);
        MoveFieldTo(p.X - _grabOffset.X + _field.Width / 2,
                    p.Y - _grabOffset.Y + _field.Height);
    }

    private void OnFieldUp(object sender, MouseButtonEventArgs e)
    {
        if (!_dragging) return;
        _dragging = false;
        _field.ReleaseMouseCapture();
        e.Handled = true;
    }

    private void OnOverlayDown(object sender, MouseButtonEventArgs e)
    {
        Point p = e.GetPosition(_overlay);
        MoveFieldTo(p.X, p.Y + _field.Height / 2);
    }

    /// <summary>Write a model-space (centre X, bottom Y) back as percentages.</summary>
    private void MoveFieldTo(double centreX, double bottomY)
    {
        double w = _overlay.Width, h = _overlay.Height;
        if (w <= 0 || h <= 0) return;
        App.Settings.SearchPosX = (int)Math.Round(Math.Clamp(centreX / w, 0, 1) * 100);
        App.Settings.SearchPosY = (int)Math.Round(Math.Clamp(bottomY / h, 0, 1) * 100);
    }
}
