// ---------------------------------------------------------------------------
// The settings window itself: custom chrome, the navigation between pages and
// sub-pages, the Apply bar that only shows up when something is unsaved, and
// the modal layer the pages borrow for confirmations.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using CKFlip3D.Settings.Interop;
using CKFlip3D.Settings.Services;
using CKFlip3D.Settings.Views;

namespace CKFlip3D.Settings;

public partial class MainWindow : Window
{
    private readonly Dictionary<string, UserControl> _pageCache = new();
    private readonly Stack<(UserControl page, string title)> _subPageStack = new();
    private string _rootTitle = "General";

    public MainWindow()
    {
        InitializeComponent();
        DataContext = App.Settings;

        App.Settings.DirtyChanged += UpdateApplyBar;
        SourceInitialized += OnSourceInitialized;
        Loaded += OnLoaded;
        StateChanged += OnWindowStateChanged;

        // The log is read from disk and followed while the window lives, so
        // anything the core records during this session reaches the sidebar
        // without the user reopening anything.
        DiagnosticsLog.Changed += () => Dispatcher.BeginInvoke(UpdateLogBadges);
        DiagnosticsLog.StartWatching();

        // Live theme switching: the model property changes (combo box,
        // Revert, Reset) and the window fades through the swap.
        App.Settings.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(Models.SettingsModel.AppTheme)
                && App.Settings.AppTheme != ThemeService.CurrentIndex)
            {
                ThemeService.SwitchWithFade(App.Settings.AppTheme, Root, ApplyBackdrop);
            }
        };
    }

    // =====================================================================
    // Window chrome: glass, hit testing, caption buttons
    // =====================================================================

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        if (PresentationSource.FromVisual(this) is HwndSource source)
        {
            source.AddHook(WndProc);
            // The backdrop only shows through where the WPF surface is
            // genuinely transparent.
            source.CompositionTarget.BackgroundColor = Colors.Transparent;
        }

        ApplyBackdrop();
        AeroGlass.SetRoundedCorners(this, true);
    }

    /// <summary>
    /// Real transparency chain: Win11 22H2+ system-backdrop acrylic (blurs
    /// other apps/desktop behind the window, like Windows Terminal's
    /// "acrylic material") → Win10 SetWindowCompositionAttribute acrylic →
    /// opaque smoked-glass fallback so the UI never becomes a hole.
    /// Re-run on theme change (tint + dark/light hint differ per theme).
    /// </summary>
    private void ApplyBackdrop()
    {
        var theme = ThemeService.Current;

        if (AeroGlass.TryEnableSystemBackdrop(this, theme.Dark)
            || AeroGlass.Enable(this, theme.Tint, theme.TintAlpha))
        {
            // (Re)attach the translucent themed glass over the live blur.
            WindowBorder.SetResourceReference(Border.BackgroundProperty, "WindowGlassBrush");
            return;
        }

        // No composition at all: opaque smoked glass keeps the UI readable.
        WindowBorder.SetResourceReference(Border.BackgroundProperty, "WindowGlassFallbackBrush");
    }

    private const int WM_NCHITTEST = 0x0084;
    private const int WM_GETMINMAXINFO = 0x0024;
    private const int HTCLIENT = 1, HTCAPTION = 2,
        HTLEFT = 10, HTRIGHT = 11, HTTOP = 12, HTTOPLEFT = 13, HTTOPRIGHT = 14,
        HTBOTTOM = 15, HTBOTTOMLEFT = 16, HTBOTTOMRIGHT = 17;

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        switch (msg)
        {
            case WM_NCHITTEST:
            {
                // Custom resize borders for the borderless window.
                if (WindowState == WindowState.Maximized) break;

                var pos = PointFromScreen(new Point(
                    unchecked((short)(lParam.ToInt64() & 0xFFFF)),
                    unchecked((short)((lParam.ToInt64() >> 16) & 0xFFFF))));

                const double edge = 6;
                bool left = pos.X < edge;
                bool right = pos.X > ActualWidth - edge;
                bool top = pos.Y < edge;
                bool bottom = pos.Y > ActualHeight - edge;

                int hit = HTCLIENT;
                if (top && left) hit = HTTOPLEFT;
                else if (top && right) hit = HTTOPRIGHT;
                else if (bottom && left) hit = HTBOTTOMLEFT;
                else if (bottom && right) hit = HTBOTTOMRIGHT;
                else if (left) hit = HTLEFT;
                else if (right) hit = HTRIGHT;
                else if (top) hit = HTTOP;
                else if (bottom) hit = HTBOTTOM;

                if (hit != HTCLIENT)
                {
                    handled = true;
                    return new IntPtr(hit);
                }
                break;
            }
            case WM_GETMINMAXINFO:
                // Keep a borderless maximize inside the monitor work area
                // so the taskbar is not covered.
                ClampMaximizeToWorkArea(hwnd, lParam);
                break;
        }
        return IntPtr.Zero;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct POINTAPI { public int X, Y; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MINMAXINFO
    {
        public POINTAPI ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    private struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor, rcWork;
        public uint dwFlags;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, uint flags);

    [DllImport("user32.dll")]
    private static extern bool GetMonitorInfoW(IntPtr hMonitor, ref MONITORINFO info);

    private static void ClampMaximizeToWorkArea(IntPtr hwnd, IntPtr lParam)
    {
        const uint MONITOR_DEFAULTTONEAREST = 2;
        IntPtr monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (monitor == IntPtr.Zero) return;

        var info = new MONITORINFO { cbSize = Marshal.SizeOf<MONITORINFO>() };
        if (!GetMonitorInfoW(monitor, ref info)) return;

        var mmi = Marshal.PtrToStructure<MINMAXINFO>(lParam);
        mmi.ptMaxPosition.X = info.rcWork.Left - info.rcMonitor.Left;
        mmi.ptMaxPosition.Y = info.rcWork.Top - info.rcMonitor.Top;
        mmi.ptMaxSize.X = info.rcWork.Right - info.rcWork.Left;
        mmi.ptMaxSize.Y = info.rcWork.Bottom - info.rcWork.Top;
        Marshal.StructureToPtr(mmi, lParam, true);
    }

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount == 2)
        {
            ToggleMaximize();
            return;
        }
        if (e.ButtonState == MouseButtonState.Pressed)
            DragMove();
    }

    private void BtnMinimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void BtnMaximize_Click(object sender, RoutedEventArgs e) => ToggleMaximize();
    private void BtnClose_Click(object sender, RoutedEventArgs e) => Close();

    private void ToggleMaximize() =>
        WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private void OnWindowStateChanged(object? sender, EventArgs e)
    {
        bool max = WindowState == WindowState.Maximized;
        WindowBorder.CornerRadius = new CornerRadius(max ? 0 : 8);
    }

    // =====================================================================
    // Entry animation
    // =====================================================================

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseOut };

        Root.BeginAnimation(OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(200)) { EasingFunction = ease });
        RootScale.BeginAnimation(ScaleTransform.ScaleXProperty,
            new DoubleAnimation(0.975, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });
        RootScale.BeginAnimation(ScaleTransform.ScaleYProperty,
            new DoubleAnimation(0.975, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });

        // sidebar slides in shortly after the frame appears
        var navFade = new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(280))
        { BeginTime = TimeSpan.FromMilliseconds(90), EasingFunction = ease };
        var navSlide = new DoubleAnimation(-18, 0, TimeSpan.FromMilliseconds(300))
        { BeginTime = TimeSpan.FromMilliseconds(90), EasingFunction = ease };
        NavPanel.BeginAnimation(OpacityProperty, navFade);
        NavShift.BeginAnimation(TranslateTransform.XProperty, navSlide);

        // Ask the questions only this app can answer (an unsupported Windows,
        // a core that is not where it should be), then show what is there.
        DiagnosticsLog.RunEnvironmentChecks();
        UpdateLogBadges();

        // Multi-monitor settings only make sense with 2+ monitors.
        if (MonitorInterop.EnumerateMonitors().Count < 2)
        {
            NavItemMultiMonitor.IsEnabled = false;
            NavItemMultiMonitor.ToolTip = "Requires more than one monitor.";
        }

        NavList.SelectedIndex = 0; // triggers first navigation

        // Offer to start the core when it isn't running — the switcher and
        // its hotkey are dead until then, and Apply only reaches a live
        // process.  Slightly delayed so the entry animation settles first.
        if (!CoreLocator.IsCoreRunning())
        {
            var timer = new System.Windows.Threading.DispatcherTimer
            {
                Interval = TimeSpan.FromMilliseconds(650),
            };
            timer.Tick += (_, _) => { timer.Stop(); OfferCoreLaunch(); };
            timer.Start();
        }

        StartCoreWatch();
    }

    // =====================================================================
    // Core liveness while the window is open
    // =====================================================================

    private System.Windows.Threading.DispatcherTimer? _coreWatch;
    private bool _coreWasRunning;

    /// <summary>
    /// RunEnvironmentChecks asks whether the core is running exactly once, as
    /// the window opens. A core that DIED while someone was sitting in front
    /// of the settings therefore went unreported: the Diagnostics page said
    /// "not running" in its runtime rows — those are re-read on every visit —
    /// while the log, which is the thing whose whole job is to report this,
    /// stayed silent and the sidebar stayed clean.
    ///
    /// Edge-triggered deliberately. Appending on every tick would turn one
    /// fact into a count climbing forever, and the log groups by code, so it
    /// would read "CKFlip3D is not running ×300". Only the transitions are
    /// events. The rising edge clears for the same reason the Launch button
    /// does: the entry describes a state, and the state is over.
    /// </summary>
    private void StartCoreWatch()
    {
        _coreWasRunning = CoreLocator.IsCoreRunning();

        _coreWatch = new System.Windows.Threading.DispatcherTimer
        {
            Interval = TimeSpan.FromSeconds(2),
        };
        _coreWatch.Tick += (_, _) =>
        {
            bool now = CoreLocator.IsCoreRunning();
            if (now == _coreWasRunning) return;
            _coreWasRunning = now;

            if (now) DiagnosticsLog.ClearCode(DiagnosticsLog.Code.CoreNotRunning);
            else     DiagnosticsLog.ReportCoreNotRunning();
        };
        _coreWatch.Start();

        // The timer holds a reference to this window; without stopping it the
        // dispatcher keeps polling a window that is on its way out.
        Closed += (_, _) => _coreWatch?.Stop();
    }

    // =====================================================================
    // Core-not-running offer
    // =====================================================================

    private void OfferCoreLaunch()
    {
        if (CoreLocator.IsCoreRunning()) return;   // started in the meantime

        ShowModal("CKFlip3D is not running",
            MakeModalText("The CKFlip3D core process is not active — the 3D "
                + "switcher and its hotkey are unavailable, and applied "
                + "settings only take effect the next time it runs.\n\n"
                + "Do you want to launch it now? Administrator approval "
                + "may be required."),
            ("Launch CKFlip3D", true, LaunchCore),
            ("Not now", false, null));
    }

    private async void LaunchCore()
    {
        string? exe = CoreLocator.FindCoreExe();
        if (exe == null)
        {
            ShowModal("Launch failed",
                MakeModalText("CKFlip3D.exe could not be located. Rebuild the "
                    + "core (build.bat) so it sits next to the settings app."),
                ("OK", true, null));
            return;
        }

        try
        {
            // The core's manifest requires administrator — ShellExecute
            // raises the UAC prompt; a declined prompt throws and is simply
            // treated as "not now".
            System.Diagnostics.Process.Start(
                new System.Diagnostics.ProcessStartInfo(exe) { UseShellExecute = true });
        }
        catch
        {
            // UAC declined / launch failure — nothing to repair.
            return;
        }

        // CK0602 was raised when this window opened and the core was absent.
        // Starting it answers that entry, but nothing was retracting it, so the
        // warning and its sidebar count sat there over a switcher that was by
        // then running — the log stating something untrue about the machine.
        //
        // ShellExecute returns as soon as the elevation prompt is answered and
        // the elevated process starts SEPARATELY, so its being up cannot be
        // inferred from the call returning: poll instead, and clear only on the
        // evidence that it is actually there. If it never arrives (declined at
        // a second prompt, immediate exit) the entry is left exactly as it was,
        // because then it is still true.
        for (int i = 0; i < 40 && !CoreLocator.IsCoreRunning(); i++)
            await System.Threading.Tasks.Task.Delay(250);

        if (CoreLocator.IsCoreRunning())
            DiagnosticsLog.ClearCode(DiagnosticsLog.Code.CoreNotRunning);
    }

    private TextBlock MakeModalText(string text) => new()
    {
        Text = text,
        TextWrapping = TextWrapping.Wrap,
        FontFamily = new FontFamily("Segoe UI"),
        FontSize = 13,
        MaxWidth = 430,
        Foreground = (Brush)FindResource("TextPrimaryBrush"),
    };

    // =====================================================================
    // Navigation
    // =====================================================================

    // Pressed-before-commit: MouseDown only shows the pressed visual; the
    // selection (and the page switch) is committed on MouseUp inside the
    // same item. Releasing elsewhere cancels.
    private ListBoxItem? _pendingNavItem;

    private void NavList_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        var item = ItemsControl.ContainerFromElement(NavList, (DependencyObject)e.OriginalSource) as ListBoxItem;
        if (item == null || !item.IsEnabled) return;

        if (!item.IsSelected)
        {
            _pendingNavItem = item;
            NavBehavior.SetIsPressed(item, true);
            item.CaptureMouse();
            e.Handled = true; // defer SelectionChanged until MouseUp
        }
    }

    private void NavList_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_pendingNavItem == null) return;

        var pressed = _pendingNavItem;
        _pendingNavItem = null;
        NavBehavior.SetIsPressed(pressed, false);
        pressed.ReleaseMouseCapture();

        // Commit only when released over the same item.
        if (pressed.IsMouseOver)
        {
            pressed.IsSelected = true;   // fires SelectionChanged → navigation
            e.Handled = true;
        }
    }

    private void NavList_MouseLeave(object sender, MouseEventArgs e)
    {
        if (_pendingNavItem == null) return;
        NavBehavior.SetIsPressed(_pendingNavItem, false);
        _pendingNavItem.ReleaseMouseCapture();
        _pendingNavItem = null;
    }

    private void NavList_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (NavList.SelectedItem is not ListBoxItem item || item.Tag is not string tag)
            return;

        _subPageStack.Clear();
        HideBackOrb();

        (UserControl page, string title) = tag switch
        {
            "Appearance"   => ((UserControl)GetPage<AppearancePage>(), "Appearance"),
            "MultiMonitor" => (GetPage<MultiMonitorPage>(), "Multi-monitor"),
            "Controls"     => (GetPage<ControlsPage>(), "Controls"),
            "Search"       => (GetPage<SearchPage>(), "Search"),
            "Diagnostics"  => (GetPage<DiagnosticsPage>(), "Diagnostics"),
            "Recovery"     => (GetPage<RecoveryPage>(), "Recovery"),
            "About"        => (GetPage<AboutPage>(), "About"),
            _              => (GetPage<GeneralPage>(), "General"),
        };

        _rootTitle = title;
        SetPage(page, title);
    }

    private T GetPage<T>() where T : UserControl, new()
    {
        string key = typeof(T).Name;
        if (!_pageCache.TryGetValue(key, out var page))
        {
            page = new T { DataContext = App.Settings };
            _pageCache[key] = page;
        }
        return (T)page;
    }

    /// <summary>Open a child page (e.g. the monitor selector). Shows the back orb.</summary>
    public void PushSubPage(UserControl page, string title)
    {
        if (PageHost.Content is UserControl current)
            _subPageStack.Push((current, PageTitle.Text));

        page.DataContext = App.Settings;
        ShowBackOrb();
        SetPage(page, title);
    }

    // =====================================================================
    // Log marks (sidebar, above the version)
    // =====================================================================

    /// <summary>
    /// Keep the marks in step with the log. Called on every change, including
    /// ones the core makes while this window is open — a failure that happens
    /// during a session should announce itself, not wait for a restart.
    /// </summary>
    private void UpdateLogBadges()
    {
        int errors = DiagnosticsLog.CountOf(DiagSeverity.Critical);
        int warnings = DiagnosticsLog.CountOf(DiagSeverity.Warning);
        int infos = DiagnosticsLog.CountOf(DiagSeverity.Info);

        void Apply(Button button, TextBlock label, int count)
        {
            button.Visibility = count > 0 ? Visibility.Visible : Visibility.Collapsed;
            label.Text = count.ToString(System.Globalization.CultureInfo.CurrentCulture);
        }

        Apply(BadgeCritical, BadgeCriticalCount, errors);
        Apply(BadgeWarning, BadgeWarningCount, warnings);
        Apply(BadgeInfo, BadgeInfoCount, infos);

        LogBadges.Visibility = (errors + warnings + infos) > 0
            ? Visibility.Visible : Visibility.Collapsed;

        // Unread entries pulse once so a mark that was already on screen still
        // registers when its count goes up; read ones simply sit there.
        LogBadges.Opacity = DiagnosticsLog.HasUnseen ? 1.0 : 0.72;
    }

    private void LogBadge_Click(object sender, RoutedEventArgs e)
    {
        if (PageHost.Content is LogPage)
            return;                       // already looking at it
        PushSubPage(new LogPage(), "Log");
    }

    private void BackOrb_Click(object sender, RoutedEventArgs e)
    {
        if (_subPageStack.Count == 0) return;

        var (page, title) = _subPageStack.Pop();
        if (_subPageStack.Count == 0)
            HideBackOrb();
        SetPage(page, title);
    }

    private void ShowBackOrb()
    {
        BackOrb.Visibility = Visibility.Visible;
        var ease = new BackEase { EasingMode = EasingMode.EaseOut, Amplitude = 0.4 };
        BackOrb.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(180)));
        BackOrbScale.BeginAnimation(ScaleTransform.ScaleXProperty,
            new DoubleAnimation(0.6, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });
        BackOrbScale.BeginAnimation(ScaleTransform.ScaleYProperty,
            new DoubleAnimation(0.6, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });
    }

    private void HideBackOrb() => BackOrb.Visibility = Visibility.Collapsed;

    private void SetPage(UserControl page, string title)
    {
        PageTitle.Text = title;
        PageHost.Content = page;

        // gentle slide+fade entrance for the new page
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseOut };
        PageHost.BeginAnimation(OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
        PageShift.BeginAnimation(TranslateTransform.YProperty,
            new DoubleAnimation(12, 0, TimeSpan.FromMilliseconds(260)) { EasingFunction = ease });
    }

    // =====================================================================
    // Apply bar
    // =====================================================================

    private void UpdateApplyBar()
    {
        double target = App.Settings.IsDirty ? 46 : 0;
        ApplyBar.BeginAnimation(HeightProperty,
            new DoubleAnimation(target, TimeSpan.FromMilliseconds(220))
            { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut } });

        // A combination that cannot work is not offered for saving: Apply
        // LEAVES rather than greys out, so the bar reads as "there is nothing
        // to press here yet" instead of "press harder".  Revert stays — it is
        // the way back out — and the message says which setting is holding it.
        string? clash = App.Settings.ActivationBindingClash;
        bool ok = App.Settings.WindowSnapSatisfied && clash == null;
        ApplyButton.Visibility = ok ? Visibility.Visible : Visibility.Collapsed;
        ApplyBarText.Text =
            !App.Settings.WindowSnapSatisfied
                ? "Window snap can only be off with “Let the mouse act on the stack” on."
            : clash != null
                ? $"{App.Settings.ActivationHotkey} opens the cascade and is also bound "
                  + $"to {clash}. Change one of them, or revert."
                : "You have unsaved changes. Apply to update CKFlip3D.";
    }

    private void BtnApply_Click(object sender, RoutedEventArgs e)
    {
        ConfigService.Save(App.Settings);

        // Save broadcast the live reload; a full restart also picks up
        // anything the core only reads at startup.
        ConfigService.RestartCore();

        // Settings that were saved but never reached the switcher are the
        // quietest way for this button to lie: the file is right, the running
        // program is not, and nothing on screen says so.
        if (CoreLocator.FindCoreExe() != null && !CoreLocator.IsCoreRunning())
        {
            DiagnosticsLog.Append(DiagnosticsLog.Code.CoreUnreachable,
                DiagSeverity.Warning,
                "The running CKFlip3D did not accept the new settings",
                "the settings were saved, but CKFlip3D was not running to receive "
                + "them; start it for them to take effect.");
        }

        // "Start with Windows" lives in the Task Scheduler, not in config.json.
        string? startupError = StartupService.Apply(App.Settings.StartWithWindows);
        if (startupError != null)
        {
            DiagnosticsLog.Append(DiagnosticsLog.Code.StartupTaskFailed,
                DiagSeverity.Warning,
                "The startup entry could not be created",
                "CKFlip3D will not start with Windows — " + startupError);
            var body = new TextBlock
            {
                Text = $"Settings were saved, but the startup entry could not be updated:\n\n{startupError}",
                TextWrapping = TextWrapping.Wrap,
                FontSize = 13,
            };
            if (TryFindResource("TextPrimaryBrush") is Brush b) body.Foreground = b;
            ShowModal("Startup entry failed", body, ("OK", true, null));
        }
    }

    private void BtnRevert_Click(object sender, RoutedEventArgs e) => App.Settings.RevertToSnapshot();

    // =====================================================================
    // In-app modal (license, confirmations)
    // =====================================================================

    // Generation counter guards against the close-fade's Completed handler
    // collapsing a modal that was re-shown while the fade was in flight
    // (e.g. LaunchCore's error modal right after the offer modal closed).
    private int _modalGeneration;

    public void ShowModal(string title, UIElement body, params (string label, bool accent, Action? onClick)[] buttons)
    {
        _modalGeneration++;
        ModalTitle.Text = title;
        ModalBody.Content = body;
        ModalButtons.Children.Clear();

        foreach (var (label, accent, onClick) in buttons)
        {
            var btn = new Button
            {
                Content = label,
                Style = (Style)FindResource(accent ? "AccentButton" : "AeroButton"),
                Margin = new Thickness(8, 0, 0, 0),
                MinWidth = 86,
            };
            btn.Click += (_, _) => { CloseModal(); onClick?.Invoke(); };
            ModalButtons.Children.Add(btn);
        }

        ModalLayer.Visibility = Visibility.Visible;
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseOut };
        ModalLayer.BeginAnimation(OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(160)) { EasingFunction = ease });
        ModalScale.BeginAnimation(ScaleTransform.ScaleXProperty,
            new DoubleAnimation(0.94, 1, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
        ModalScale.BeginAnimation(ScaleTransform.ScaleYProperty,
            new DoubleAnimation(0.94, 1, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
    }

    /// <summary>
    /// Screen rectangle of the modal's button strip.
    ///
    /// A capture that binds a BARE left click cannot let one through to the
    /// UI, so it needs somewhere the click is left alone — otherwise the only
    /// way out of the modal is the keyboard. Returns an empty rect when the
    /// modal is not up or has not been laid out yet, which the capture reads
    /// as "no dead zone".
    /// </summary>
    public Rect ModalButtonsScreenRect()
    {
        if (ModalLayer.Visibility != Visibility.Visible
            || ModalButtons.ActualWidth <= 0 || ModalButtons.ActualHeight <= 0)
            return Rect.Empty;
        try
        {
            Point tl = ModalButtons.PointToScreen(new Point(0, 0));
            Point br = ModalButtons.PointToScreen(
                new Point(ModalButtons.ActualWidth, ModalButtons.ActualHeight));
            return new Rect(tl, br);
        }
        catch (InvalidOperationException)
        {
            return Rect.Empty;   // no presentation source yet
        }
    }

    public void CloseModal()
    {
        int generation = ++_modalGeneration;
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(140));
        fade.Completed += (_, _) =>
        {
            if (_modalGeneration == generation)
                ModalLayer.Visibility = Visibility.Collapsed;
        };
        ModalLayer.BeginAnimation(OpacityProperty, fade);
    }
}
