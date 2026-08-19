// ---------------------------------------------------------------------------
// Wheel scrolling that follows how far the fingers actually moved.  WPF reads
// only the sign of a wheel event, which suits a notched mouse wheel and throws
// a page half a screen on the lightest touchpad nudge.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Wheel scrolling that is PROPORTIONAL to how far the fingers moved, attached
/// to every settings page through the AeroScrollViewer style.
///
/// WPF's ScrollViewer reads only the SIGN of a wheel event: whatever the delta,
/// <c>OnMouseWheel</c> scrolls one fixed step (WheelScrollLines × 16 px). That
/// suits a notched wheel, which only ever sends ±120, and is wrong for a
/// precision touchpad, which reports the real travel dozens of times a second.
/// Every one of those reports became a full three-line jump, so the lightest
/// two-finger nudge threw the page half a screen.
///
/// So the two are told apart and each gets the mapping it wants:
///
///   • A NOTCHED WHEEL, every delta a whole multiple of 120, keeps WPF's own
///     arithmetic untouched. It was never the thing that felt wrong.
///
///   • HIGH-RESOLUTION input keeps the same proportional arithmetic but
///     measures its lines against 32 px instead of 16. Honouring the delta
///     against WPF's unit was the opposite error: a full finger stroke moved
///     less than one viewport. 16 px is a 1995 text line, not a unit of finger
///     travel.
///
/// The gain is DEVICE-NEUTRAL on purpose, and that is worth defending. Curving
/// it by finger speed, the way Windows and macOS do, was tried and withdrawn: a
/// speed ramp has to be pinned to absolute reported-units-per-second, that
/// scale belongs to one pad's driver, and on hardware that reports differently
/// the ramp saturates and silently becomes the wrong flat multiplier. A fixed
/// line height cannot do that.
///
/// Two things sit on top, both small enough not to add lag: a short glide
/// toward the target offset, and a deadband on direction measured in reported
/// units. Both are documented at the constants that govern them.
///
/// The wheel is aimed at the innermost thing under the pointer that can take
/// it, so a list inside a page scrolls the list, with all of the above rather
/// than with WPF's fixed step. A list that scrolls by ITEM is left to WPF: it
/// has no pixel offset to glide along.
/// </summary>
public static class SmoothScroll
{
    // ---- units ------------------------------------------------------------
    // WPF's own numbers, so a mouse notch still moves exactly what it moved
    // before: ScrollViewer scrolls SystemParameters.WheelScrollLines lines of
    // 16 px per notch of 120. High-resolution input measures the same lines
    // against a row of this UI instead of against a 1995 text line — see the
    // remarks; it is the whole of the "too small a unit" fix.
    private const double LineHeight = 16.0;
    private const double HiResLineHeight = 32.0;
    private const int WheelNotch = 120;

    // ---- glide ------------------------------------------------------------
    // The fraction of the remaining distance covered per millisecond is
    // 1 − e^(−dt/Tau). 38 ms puts ~95 % of a mouse notch in about 110 ms, which
    // reads as a slide rather than a wait.
    //
    // ONE value, for the pad as much as for the wheel. A shorter one was tried
    // for high-resolution input, on the theory that a stream arriving 70 times
    // a second wants tracking rather than filtering. The recording says
    // otherwise: a pad does not report evenly. Measured gaps ran 7 ms at the
    // first quartile, 10 at the median, 16 at the third and 35 at the ninth
    // decile, so some frames get two reports and some none, and report size
    // swung between 1 and 613 units. Track that stream closely and the page
    // inherits every bit of its unevenness as visible stutter. Bridging those
    // gaps is the whole job of the filter, so the time constant has to be of
    // the same order as the gaps.
    private const double TauMs = 38.0;
    private const double NominalFrameMs = 16.7;

    // ---- deadband ---------------------------------------------------------
    // Reported units, not pixels: this is a statement about what the fingers
    // asked for, and it must not move when the line height does. Written as
    // fractions of a notch, the one unit every device agrees on. A notched
    // wheel skips the deadband entirely — it has no jitter to filter.
    private const double StartDeadbandUnits = WheelNotch / 3.0;    // 40
    private const double ReverseDeadbandUnits = WheelNotch;        // 120

    // …and a reversal must also be SUSTAINED: this many reports in a row, not
    // one bad sample. Size alone provably cannot separate the two. Traced
    // report by report while the twitch was reproduced: single-report bursts
    // the wrong way reached 190 units and two-report bursts 239, while the
    // shortest genuine stroke was 34, so a bar in units would have to sit above
    // 239 and below 34 at once. What tells them apart is duration, not size.
    // Three reports is about 20 ms at the rate a pad reports, so a reversal
    // that is meant still turns the page inside a frame or two.
    private const int ReverseMinReports = 3;
    // How long a gesture stays "in progress". After this much quiet the next
    // report starts a fresh one, deadband and all.
    private const double GestureIdleMs = 350.0;

    private static readonly double TicksPerMs = Stopwatch.Frequency / 1000.0;

    /// <summary>
    /// Monotonic milliseconds. NOT Environment.TickCount: it steps in 15.6 ms,
    /// which is most of a frame and most of the gap between two touchpad
    /// reports, so both the glide and the rate estimate would be reading noise.
    /// </summary>
    private static double NowMs() => Stopwatch.GetTimestamp() / TicksPerMs;

    public static readonly DependencyProperty EnabledProperty =
        DependencyProperty.RegisterAttached(
            "Enabled", typeof(bool), typeof(SmoothScroll),
            new PropertyMetadata(false, OnEnabledChanged));

    public static bool GetEnabled(DependencyObject obj) => (bool)obj.GetValue(EnabledProperty);
    public static void SetEnabled(DependencyObject obj, bool value) => obj.SetValue(EnabledProperty, value);

    private static void OnEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ScrollViewer viewer) return;

        if ((bool)e.NewValue)
        {
            viewer.PreviewMouseWheel += StateFor(viewer).OnPreviewWheel;
        }
        else if (viewer.GetValue(StateProperty) is State state)
        {
            viewer.PreviewMouseWheel -= state.OnPreviewWheel;
            viewer.Unloaded -= state.OnUnloaded;
            state.Stop();
            viewer.ClearValue(StateProperty);
        }
    }

    /// <summary>Per-viewer scroll state. Private: nothing outside sets this.</summary>
    private static readonly DependencyProperty StateProperty =
        DependencyProperty.RegisterAttached(
            "State", typeof(State), typeof(SmoothScroll), new PropertyMetadata(null));

    /// <summary>
    /// The state of one viewer, made on demand. A nested list gets one without
    /// ever being Enabled: the page above it is the only thing subscribed to
    /// the wheel, and it drives whichever viewer the pointer is actually over.
    /// </summary>
    private static State StateFor(ScrollViewer viewer)
    {
        if (viewer.GetValue(StateProperty) is State existing) return existing;

        var fresh = new State(viewer);
        viewer.SetValue(StateProperty, fresh);
        viewer.Unloaded += fresh.OnUnloaded;
        return fresh;
    }

    private sealed class State
    {
        private readonly ScrollViewer _viewer;
        private double _target;          // where the wheel has asked to be
        private double _current;         // where the glide has got to
        private bool _animating;
        private double _lastFrameMs;     // composition-clock time of the last frame
        private int _direction;          // +1 down, −1 up; outlives the gesture
        private bool _fresh = true;      // nothing has moved yet in this gesture
        private double _lastStepMs = double.NegativeInfinity;
        private double _heldStart;       // signed travel of a gesture that has not begun
        private double _heldReverse;     // opposite-direction units absorbed so far
        private int _heldReports;        // …and how many reports in a row said so
        private bool _hiRes;             // this gesture reports finer than a notch

        public State(ScrollViewer viewer) => _viewer = viewer;

        public void OnPreviewWheel(object sender, MouseWheelEventArgs e)
        {
            // Something ahead of us claimed it — the touchpad-activity panel
            // swallows the wheel while it holds the pointer, for one.
            if (e.Handled || e.Delta == 0) return;

            ScrollViewer? target = WheelTarget(e.OriginalSource as DependencyObject);
            if (target is null) return;                      // not ours to take

            if (ReferenceEquals(target, _viewer)) Take(e);
            else StateFor(target).Take(e);                   // a list inside the page
        }

        /// <summary>
        /// Fold one wheel report into the target offset. Claims the event
        /// either way: a report the deadband holds is absorbed, never passed on
        /// for WPF to turn back into a jump.
        /// </summary>
        private void Take(MouseWheelEventArgs e)
        {
            e.Handled = true;

            double now = NowMs();
            bool fresh = now - _lastStepMs > GestureIdleMs;
            _lastStepMs = now;

            if (fresh)
            {
                // A new gesture holds its own travel again — but NOT its own
                // idea of which way the page was going. Forgetting that was a
                // hole straight through the reverse bar: the bar only ever
                // guarded a reversal WITHIN a gesture, so after a third of a
                // second of quiet a stray report the other way faced nothing
                // but the much lower start bar, and went through. Pauses are
                // what scrolling is mostly made of, so that was most of them.
                _fresh = true;
                _heldStart = _heldReverse = 0;
                _heldReports = 0;
                _hiRes = false;
            }

            // Someone else moved the view (scrollbar, keyboard, a focus change
            // scrolling something into view) — carry on from where it actually
            // is rather than from where the glide last left off.
            if (!_animating || Math.Abs(_viewer.VerticalOffset - _current) > 1.0)
                _current = _target = _viewer.VerticalOffset;

            // A pad reports finer than a notch; a wheel never does. One such
            // report is enough to settle the question for the whole gesture.
            if (Math.Abs(e.Delta) % WheelNotch != 0) _hiRes = true;

            double units = -e.Delta;         // positive = further down the page

            if (_hiRes && !ClearsDeadband(ref units)) return;

            int sign = Math.Sign(units);
            if (sign != 0) _direction = sign;

            _target = Math.Clamp(_target + Pixels(units), 0, _viewer.ScrollableHeight);
            Start();
        }

        /// <summary>
        /// Has this report earned the right to move the page? Everything held
        /// below a bar is dropped rather than banked — see the class remarks.
        /// </summary>
        private bool ClearsDeadband(ref double units)
        {
            int sign = Math.Sign(units);

            if (_fresh)
            {
                // Nothing has moved yet in this gesture. Sum SIGNED, so the
                // report or two the wrong way that a hand makes as it lands on
                // the pad is cancelled by the real stroke rather than merely
                // delayed — and hold it against the START bar when it agrees
                // with the way the page was already going, against the far
                // taller REVERSE test when it does not. Carrying on is the
                // ordinary thing; turning round has to be asked for, whether
                // the fingers paused first or not.
                _heldStart += units;
                _heldReports++;

                bool agrees = _direction == 0 || Math.Sign(_heldStart) == _direction;
                double bar = agrees ? StartDeadbandUnits : ReverseDeadbandUnits;
                int need = agrees ? 1 : ReverseMinReports;
                if (Math.Abs(_heldStart) < bar || _heldReports < need) return false;

                // What was held is DROPPED, not banked. Releasing it together
                // with the step that cleared the bar was the first version, and
                // it made the twitch worse: it guaranteed a reversal moved
                // further than any single report would have.
                _direction = Math.Sign(_heldStart);
                _heldStart = _heldReverse = 0;
                _heldReports = 0;
                _fresh = false;
                return true;
            }

            if (sign != 0 && sign != _direction)
            {
                _heldReverse += Math.Abs(units);
                _heldReports++;
                if (_heldReverse < ReverseDeadbandUnits || _heldReports < ReverseMinReports)
                    return false;

                _direction = sign;
                _heldReverse = 0;
                _heldReports = 0;
                return true;
            }

            // Still going the way it was — the reversal budget resets, so
            // absorbed jitter never adds up across a long scroll, and a stray
            // sample followed by the stroke resuming costs nothing at all.
            _heldReverse = 0;
            _heldReports = 0;
            return true;
        }

        /// <summary>
        /// Reported units to pixels of offset: Windows' "lines to scroll" times
        /// the height of a line, with high-resolution input measuring that line
        /// against a row of this UI rather than against a 1995 text line.
        ///
        /// The setting can also be −1, meaning "one screen at a time", which no
        /// line height can express — that resolves against the viewport, and
        /// therefore identically for both sources.
        /// </summary>
        private double Pixels(double units)
        {
            double line = _hiRes ? HiResLineHeight : LineHeight;
            int lines = SystemParameters.WheelScrollLines;
            double perNotch = lines > 0 ? lines * line
                                        : Math.Max(line, _viewer.ViewportHeight * 0.9);
            return units / WheelNotch * perNotch;
        }

        /// <summary>
        /// Which viewer should take this wheel: the innermost one under the
        /// pointer that has somewhere to go. The handler is on the TUNNEL, so
        /// it runs before any inner list ever sees the event — routing here is
        /// what keeps that list scrollable, and gives it the same treatment the
        /// page gets rather than WPF's fixed step. A viewer that scrolls by
        /// item is declined: there is no pixel offset there to glide along.
        /// </summary>
        private ScrollViewer? WheelTarget(DependencyObject? source)
        {
            for (DependencyObject? node = source; node != null; node = ParentOf(node))
            {
                if (node is ScrollViewer viewer && viewer.ScrollableHeight > 0)
                    return viewer.CanContentScroll ? null : viewer;

                if (ReferenceEquals(node, _viewer)) return null;   // nowhere to go
            }
            return null;
        }

        /// <summary>
        /// Up one step of whichever tree the node is in. A ContentElement — a
        /// Hyperlink, say — is a perfectly ordinary wheel source and is NOT a
        /// Visual, and VisualTreeHelper throws rather than returning null for
        /// one.
        /// </summary>
        private static DependencyObject? ParentOf(DependencyObject node) =>
            node is Visual or System.Windows.Media.Media3D.Visual3D
                ? VisualTreeHelper.GetParent(node)
                : LogicalTreeHelper.GetParent(node);

        private void Start()
        {
            if (_animating) return;
            _animating = true;
            _lastFrameMs = 0;
            CompositionTarget.Rendering += OnFrame;
        }

        public void Stop()
        {
            if (!_animating) return;
            _animating = false;
            CompositionTarget.Rendering -= OnFrame;
        }

        private void OnFrame(object? sender, EventArgs e)
        {
            double dt = NominalFrameMs;
            if (e is RenderingEventArgs frame)
            {
                double t = frame.RenderingTime.TotalMilliseconds;
                // WPF can raise Rendering more than once for the same frame.
                if (t == _lastFrameMs) return;
                if (_lastFrameMs > 0) dt = Math.Clamp(t - _lastFrameMs, 0.5, 100.0);
                _lastFrameMs = t;
            }

            // Re-clamp every frame: the content can grow or shrink under us
            // (an expander, a list gaining a row) while the glide is running.
            _target = Math.Clamp(_target, 0, _viewer.ScrollableHeight);

            double remaining = _target - _current;
            if (Math.Abs(remaining) < 0.5)
            {
                _current = _target;
                _viewer.ScrollToVerticalOffset(_current);
                Stop();
                return;
            }

            _current += remaining * (1.0 - Math.Exp(-dt / TauMs));
            _viewer.ScrollToVerticalOffset(_current);
        }

        public void OnUnloaded(object sender, RoutedEventArgs e) => Stop();
    }
}
