using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Touchpad gesture customisation, reached from Controls → Hotkeys.
///
/// The activity card is fed by <see cref="TouchpadService"/>, which reads the
/// pad's raw HID reports directly — so the preview works with or without a
/// running CKFlip3D core, and its recogniser uses the values currently in the
/// UI (not the saved ones), letting the sensitivity be dialled in before Apply.
/// </summary>
public partial class TouchpadPage : UserControl
{
    private bool _syncing;
    private readonly List<Ellipse> _dots = new();
    private readonly DispatcherTimer _badgeTimer = new() { Interval = TimeSpan.FromMilliseconds(1400) };
    // Keeps the pointer clip pinned to the panel through window moves, and
    // hands it back the moment the window stops being the active one — so a
    // missed Esc can never leave the pointer boxed in.
    private readonly DispatcherTimer _holdWatchdog = new() { Interval = TimeSpan.FromMilliseconds(200) };
    // Refreshes the core's activation hold while the panel is live.  The hold
    // is deliberately short-lived on the core's side (see
    // ConfigService.SuspendCoreActivation), so it has to be re-sent — the
    // upside is that it cannot outlive this window.
    private readonly DispatcherTimer _suspendRefresh = new() { Interval = TimeSpan.FromMilliseconds(1000) };
    private Window? _host;
    private bool _live;

    public TouchpadPage()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        _badgeTimer.Tick += (_, _) => { _badgeTimer.Stop(); FadeBadge(0); };
        _holdWatchdog.Tick += (_, _) => HoldTick();
        _suspendRefresh.Tick += (_, _) => ConfigService.SuspendCoreActivation(true);
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        SyncFromModel();
        SetLive(false);

        App.Settings.PropertyChanged += OnSettingsChanged;
        TouchpadService.FrameReceived += OnFrame;
        TouchpadService.GestureRecognised += OnGesture;
        TouchpadService.StartMonitoring(App.Settings);

        if (_host == null && Window.GetWindow(this) is { } win)
        {
            _host = win;
            // handledEventsToo: Esc is the only way out of the take-over, so it
            // must reach us even if something along the route claimed it first.
            win.AddHandler(PreviewKeyDownEvent, new KeyEventHandler(OnHostKeyDown), true);
            win.AddHandler(KeyDownEvent, new KeyEventHandler(OnHostKeyDown), true);
            win.Deactivated += OnWindowDeactivated;
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        App.Settings.PropertyChanged -= OnSettingsChanged;
        TouchpadService.FrameReceived -= OnFrame;
        TouchpadService.GestureRecognised -= OnGesture;
        SetLive(false);
        Mouse.OverrideCursor = null;   // belt and braces: never leave the app cursorless
        TouchpadService.StopMonitoring();
        if (_host is { } win)
        {
            win.RemoveHandler(PreviewKeyDownEvent, new KeyEventHandler(OnHostKeyDown));
            win.RemoveHandler(KeyDownEvent, new KeyEventHandler(OnHostKeyDown));
            win.Deactivated -= OnWindowDeactivated;
            _host = null;
        }
        _badgeTimer.Stop();
        // Belt and braces: SetLive(false) above already released the hold,
        // but a page torn down without ever going live must not leave one
        // armed either.
        _suspendRefresh.Stop();
        ConfigService.SuspendCoreActivation(false);
    }

    private void OnSettingsChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(Models.SettingsModel.TouchpadActivateGesture)
                           or nameof(Models.SettingsModel.TouchpadCycleFingers)
                           or nameof(Models.SettingsModel.TouchpadReverse)
                           or nameof(Models.SettingsModel.TouchpadCommitGesture)
                           or null)
            SyncFromModel();
    }

    // ---- Taking the panel over -----------------------------------------------
    // Reading the pad is one thing; the pointer is another.  WPF's
    // Mouse.Capture is not enough on its own and never was: it re-routes
    // events but leaves the physical pointer free, and because the wheel event
    // it re-routes still BUBBLES from the panel up through the page's
    // ScrollViewer, a two-finger scroll aimed at the pad scrolled the settings
    // behind it anyway.  So the take-over does three things — ClipCursor pins
    // the pointer inside the panel, the tunnelling wheel handler swallows the
    // scroll before any ancestor sees it, and Mouse.Capture keeps clicks here.
    //
    // The pointer is also HIDDEN, the way a first-person game or a focused text
    // box hides it: a visible arrow sitting in the middle of a panel that is
    // reading finger positions is just noise, and seeing it wander is what made
    // the take-over look like it had not happened at all.
    //
    // Everything hangs off _live, not off the raw-input registration: even if
    // arming WM_INPUT fails, Esc still has something to release, so the panel
    // can never end up holding the pointer with no way out.

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool ClipCursor(ref RECT rect);

    [DllImport("user32.dll", EntryPoint = "ClipCursor", SetLastError = true)]
    private static extern bool ReleaseCursorClip(IntPtr rect);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetCursorPos(int x, int y);

    private void SetLive(bool on)
    {
        if (on == _live) { UpdateLiveVisuals(); return; }
        _live = on;

        TouchpadService.SetCapturing(on);

        if (on)
        {
            // The core is listening to the very same pad and keyboard — hold
            // its activation off so practising the opening diagonal (or
            // brushing the hotkey) cannot throw the real cascade over this
            // window.  Sent before anything else, so there is no gap.
            ConfigService.SuspendCoreActivation(true);
            _suspendRefresh.Start();
            Mouse.Capture(PadSurface, CaptureMode.SubTree);
            Keyboard.Focus(PadSurface);
            // Application-wide rather than PadSurface.Cursor: the pointer is
            // boxed into the panel anyway, and this leaves nothing — a child
            // element's own cursor, a drag adorner — able to bring it back.
            Mouse.OverrideCursor = Cursors.None;
            CentrePointer();
            PinPointer();
            _holdWatchdog.Start();
        }
        else
        {
            _holdWatchdog.Stop();
            _suspendRefresh.Stop();
            ConfigService.SuspendCoreActivation(false);
            ReleaseCursorClip(IntPtr.Zero);
            Mouse.OverrideCursor = null;
            if (Mouse.Captured == PadSurface) Mouse.Capture(null);
        }
        UpdateLiveVisuals();
    }

    /// <summary>Park the (now invisible) pointer in the middle of the panel.</summary>
    private void CentrePointer()
    {
        try
        {
            Point c = PadSurface.PointToScreen(
                new Point(PadSurface.ActualWidth / 2, PadSurface.ActualHeight / 2));
            SetCursorPos((int)Math.Round(c.X), (int)Math.Round(c.Y));
        }
        catch (InvalidOperationException) { /* no presentation source yet */ }
    }

    private void UpdateLiveVisuals()
    {
        FocusVeil.Visibility = _live ? Visibility.Collapsed : Visibility.Visible;
        ReadingHint.Visibility = _live ? Visibility.Visible : Visibility.Collapsed;
        PadIdleHint.Visibility = _live ? Visibility.Visible : Visibility.Collapsed;
        if (!_live) ContactCount.Text = string.Empty;
    }

    /// <summary>Box the pointer into the panel (screen pixels, 2 px inset).</summary>
    private void PinPointer()
    {
        if (!PadSurface.IsVisible || PadSurface.ActualWidth <= 4 || PadSurface.ActualHeight <= 4)
            return;
        try
        {
            // PointToScreen already yields physical pixels, which is the space
            // ClipCursor works in — no DPI arithmetic needed.
            Point tl = PadSurface.PointToScreen(new Point(0, 0));
            Point br = PadSurface.PointToScreen(new Point(PadSurface.ActualWidth, PadSurface.ActualHeight));
            var box = new RECT
            {
                Left = (int)Math.Round(tl.X) + 2,
                Top = (int)Math.Round(tl.Y) + 2,
                Right = (int)Math.Round(br.X) - 2,
                Bottom = (int)Math.Round(br.Y) - 2,
            };
            if (box.Right > box.Left && box.Bottom > box.Top) ClipCursor(ref box);
        }
        catch (InvalidOperationException)
        {
            // No presentation source yet (page torn down mid-tick) — nothing
            // to pin, and the watchdog will try again or release.
        }
    }

    private void HoldTick()
    {
        if (!_live) { _holdWatchdog.Stop(); return; }
        if (_host is not { IsActive: true } || !IsVisible || !PadSurface.IsVisible)
        {
            SetLive(false);
            return;
        }
        // Re-pin: the window may have moved, changed DPI or lost the clip to
        // another app's foreground switch.
        PinPointer();
        if (Mouse.Captured != PadSurface) Mouse.Capture(PadSurface, CaptureMode.SubTree);
    }

    private void PadSurface_Click(object sender, MouseButtonEventArgs e)
    {
        // One press takes the panel over; Esc is the way out, so a stray
        // second click cannot silently stop the reading.
        if (!_live) SetLive(true);
        e.Handled = true;
    }

    /// <summary>
    /// While the panel holds the pointer the wheel belongs to it alone. Handled
    /// on the tunnelling event, which is what actually keeps the enclosing
    /// ScrollViewer — an ancestor of the panel — from scrolling the page.
    /// </summary>
    private void OnPreviewWheel(object sender, MouseWheelEventArgs e)
    {
        if (_live) e.Handled = true;
    }

    private void OnHostKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape && _live)
        {
            SetLive(false);
            e.Handled = true;
        }
    }

    private void OnWindowDeactivated(object? sender, EventArgs e) => SetLive(false);

    // ---- Model ↔ combo boxes -----------------------------------------------

    private void SyncFromModel()
    {
        _syncing = true;
        ActivateCombo.SelectedIndex = Math.Clamp(App.Settings.TouchpadActivateGesture, 0, 4);
        FingersCombo.SelectedIndex = App.Settings.TouchpadCycleFingers >= 4 ? 1 : 0;
        DirectionCombo.SelectedIndex = App.Settings.TouchpadReverse ? 1 : 0;
        CommitCombo.SelectedIndex = Math.Clamp(App.Settings.TouchpadCommitGesture, 0, 3);

        // Nothing to reverse while there is no opening stroke.
        bool armed = App.Settings.TouchpadActivateGesture != 0;
        CancelRow.IsEnabled = armed;
        CancelRow.Opacity = armed ? 1.0 : 0.45;
        if (armed)
            CancelHint.Text = $"Drawing the {StrokeName(App.Settings.TouchpadActivateGesture)} "
                + "diagonal backwards closes the cascade without switching windows — "
                + "the touchpad's Esc.";
        _syncing = false;
    }

    private static string StrokeName(int gesture) => gesture switch
    {
        1 or 3 => "↘",
        2 or 4 => "↙",
        _ => "",
    };

    private void ActivateCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || ActivateCombo.SelectedIndex < 0) return;
        App.Settings.TouchpadActivateGesture = ActivateCombo.SelectedIndex;
    }

    private void FingersCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || FingersCombo.SelectedIndex < 0) return;
        App.Settings.TouchpadCycleFingers = FingersCombo.SelectedIndex == 1 ? 4 : 2;
    }

    private void DirectionCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || DirectionCombo.SelectedIndex < 0) return;
        App.Settings.TouchpadReverse = DirectionCombo.SelectedIndex == 1;
    }

    private void CommitCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_syncing || CommitCombo.SelectedIndex < 0) return;
        App.Settings.TouchpadCommitGesture = CommitCombo.SelectedIndex;
    }

    private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.TouchpadActivateGesture = 1;   // two fingers, "\"
        App.Settings.TouchpadCancelSwipe = true;
        App.Settings.TouchpadCycleFingers = 2;
        App.Settings.TouchpadReverse = false;
        App.Settings.TouchpadSensitivity = 50;
        App.Settings.TouchpadSmoothing = 35;
        App.Settings.TouchpadCommitGesture = 1;     // one-finger tap
    }

    // ---- Live activity ------------------------------------------------------
    // Raw input arrives on this thread (the monitor window is created on the
    // UI dispatcher), so the drawing below is already on the right thread.

    private void OnFrame(IReadOnlyList<Point> contacts)
    {
        double w = PadCanvas.ActualWidth;
        double h = PadCanvas.ActualHeight;
        if (w <= 0 || h <= 0) return;

        // Grow the pool once; afterwards a frame is pure property assignment.
        while (_dots.Count < contacts.Count)
        {
            var dot = new Ellipse
            {
                Width = 26,
                Height = 26,
                StrokeThickness = 1.5,
                Opacity = 0,
                IsHitTestVisible = false,
            };
            dot.SetResourceReference(Shape.FillProperty, "NavActiveBrush");
            dot.SetResourceReference(Shape.StrokeProperty, "AccentBrush");
            _dots.Add(dot);
            PadCanvas.Children.Add(dot);
        }

        for (int i = 0; i < _dots.Count; i++)
        {
            if (i < contacts.Count)
            {
                Canvas.SetLeft(_dots[i], contacts[i].X * w - _dots[i].Width / 2);
                Canvas.SetTop(_dots[i], contacts[i].Y * h - _dots[i].Height / 2);
                _dots[i].Opacity = 0.9;
            }
            else
            {
                _dots[i].Opacity = 0;
            }
        }

        if (!TouchpadService.IsCapturing) return;
        PadIdleHint.Visibility = contacts.Count > 0 ? Visibility.Collapsed : Visibility.Visible;
        ContactCount.Text = contacts.Count switch
        {
            0 => string.Empty,
            1 => "1 finger",
            _ => $"{contacts.Count} fingers",
        };
    }

    private void OnGesture(string text)
    {
        GestureText.Text = text;
        FadeBadge(1);
        _badgeTimer.Stop();
        _badgeTimer.Start();
    }

    private void FadeBadge(double to) =>
        GestureBadge.BeginAnimation(OpacityProperty,
            new DoubleAnimation(to, TimeSpan.FromMilliseconds(to > 0 ? 120 : 320))
            { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut } });
}
