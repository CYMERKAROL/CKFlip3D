using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using CKFlip3D.Installer.Engine;
using CKFlip3D.Installer.Interop;
using CKFlip3D.Installer.Views;

namespace CKFlip3D.Installer;

public partial class MainWindow : Window
{
    /// <summary>Final state shown on the finish page.</summary>
    public sealed record FinishState(string Title, string Message,
                                     bool ShowLaunchButtons, InstallContext? Context);

    private readonly InstallContext _context = new();
    private CancellationTokenSource? _cts;
    private bool _busy;

    public MainWindow()
    {
        InitializeComponent();
        SourceInitialized += OnSourceInitialized;
        Loaded += OnLoaded;

        if (App.IsUninstallMode)
        {
            Title = "Uninstall CKFlip3D";
            TitleText.Text = "Uninstall CKFlip3D";
            TitleIcon.Source = new System.Windows.Media.Imaging.BitmapImage(
                new Uri("pack://application:,,,/Assets/CKFlip3D.Uninstall.png"));
        }
    }

    // =====================================================================
    // Window chrome: glass backdrop (same chain as the Settings app)
    // =====================================================================

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        if (PresentationSource.FromVisual(this) is HwndSource source)
            source.CompositionTarget.BackgroundColor = Colors.Transparent;

        // Minimalism Dark tint — same values ThemeService uses for this theme.
        if (AeroGlass.TryEnableSystemBackdrop(this, darkMode: true)
            || AeroGlass.Enable(this, Color.FromRgb(0x00, 0x00, 0x00), 0xB4))
        {
            WindowBorder.SetResourceReference(Border.BackgroundProperty, "WindowGlassBrush");
        }
        else
        {
            WindowBorder.SetResourceReference(Border.BackgroundProperty, "WindowGlassFallbackBrush");
        }
        AeroGlass.SetRoundedCorners(this, true);
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Entry animation, mirroring the Settings window.
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseOut };
        Root.BeginAnimation(OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(200)) { EasingFunction = ease });
        RootScale.BeginAnimation(ScaleTransform.ScaleXProperty,
            new DoubleAnimation(0.975, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });
        RootScale.BeginAnimation(ScaleTransform.ScaleYProperty,
            new DoubleAnimation(0.975, 1, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });

        if (App.IsUninstallMode)
            SetPage(new UninstallConfirmPage(this));
        else
            SetPage(new WelcomePage(this));
    }

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ButtonState == MouseButtonState.Pressed)
            DragMove();
    }

    private void BtnMinimize_Click(object sender, RoutedEventArgs e) =>
        WindowState = WindowState.Minimized;

    private void BtnClose_Click(object sender, RoutedEventArgs e)
    {
        // While the engine is running, X means "cancel and roll back",
        // never "abandon a half-written install".
        if (_busy)
            RequestCancel();
        else
            Close();
    }

    // =====================================================================
    // Navigation
    // =====================================================================

    public void NavigateWelcome() => SetPage(new WelcomePage(this));

    public void NavigateOptions() => SetPage(new OptionsPage(this, _context));

    public void NavigateFinish(FinishState state) => SetPage(new FinishPage(this, state));

    private void SetPage(UserControl page)
    {
        PageHost.Content = page;

        var ease = new QuadraticEase { EasingMode = EasingMode.EaseOut };
        PageHost.BeginAnimation(OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
        PageShift.BeginAnimation(TranslateTransform.YProperty,
            new DoubleAnimation(12, 0, TimeSpan.FromMilliseconds(260)) { EasingFunction = ease });
    }

    // =====================================================================
    // Install / uninstall orchestration
    // =====================================================================

    public async void StartInstall()
    {
        bool isUpdate = _context.IsUpdate;
        var page = new ProgressPage(this, isUpdate ? "Updating CKFlip3D…" : "Installing CKFlip3D…");
        SetPage(page);
        ShowStatusBar(true);

        _cts = new CancellationTokenSource();
        _busy = true;
        var progress = new Progress<InstallProgress>(p => ReportProgress(page, p));

        try
        {
            var engine = new InstallEngine();
            await Task.Run(() => engine.RunAsync(_context, progress, _cts.Token));

            string message = isUpdate
                ? "CKFlip3D has been updated to version " + InstallContext.AppVersion + "."
                : "CKFlip3D has been installed on this computer.";
            if (_context.RuntimeWarning != null)
                message += "\n\nNote: " + _context.RuntimeWarning;

            NavigateFinish(new FinishState(
                isUpdate ? "Update complete" : "Installation complete", message,
                ShowLaunchButtons: true, _context));
        }
        catch (OperationCanceledException)
        {
            NavigateFinish(new FinishState("Installation cancelled",
                "Setup was cancelled. All changes were rolled back.",
                ShowLaunchButtons: false, null));
        }
        catch (Exception ex)
        {
            NavigateFinish(new FinishState("Installation failed",
                ex.Message + "\n\nAll changes made so far were rolled back.",
                ShowLaunchButtons: false, null));
        }
        finally
        {
            _busy = false;
            _cts.Dispose();
            _cts = null;
            ShowStatusBar(false);
        }
    }

    public async void StartUninstall(bool removeUserData)
    {
        var page = new ProgressPage(this, "Uninstalling CKFlip3D…");
        SetPage(page);
        ShowStatusBar(true);

        _cts = new CancellationTokenSource();
        _busy = true;
        var progress = new Progress<InstallProgress>(p => ReportProgress(page, p));

        try
        {
            var engine = new UninstallEngine();
            await engine.RunAsync(App.UninstallFrom ?? UninstallEngine.ResolveInstallDir(),
                removeUserData, progress, _cts.Token);

            NavigateFinish(new FinishState("Uninstall complete",
                "CKFlip3D has been removed from this computer.",
                ShowLaunchButtons: false, null));
        }
        catch (OperationCanceledException)
        {
            NavigateFinish(new FinishState("Uninstall interrupted",
                "The uninstall was cancelled before it finished. Some components "
                + "may already have been removed — run the uninstaller again to complete it.",
                ShowLaunchButtons: false, null));
        }
        catch (Exception ex)
        {
            NavigateFinish(new FinishState("Uninstall failed", ex.Message,
                ShowLaunchButtons: false, null));
        }
        finally
        {
            _busy = false;
            _cts.Dispose();
            _cts = null;
            ShowStatusBar(false);
        }
    }

    public void RequestCancel()
    {
        if (_cts is { IsCancellationRequested: false })
        {
            StatusBarText.Text = "Cancelling…";
            StatusBarDetail.Text = "rolling back changes";
            _cts.Cancel();
        }
    }

    // =====================================================================
    // In-app modal (update prompt) — same pattern as the Settings window
    // =====================================================================

    public void ShowModal(string title, string body,
                          params (string Label, bool Accent, Action? OnClick)[] buttons)
    {
        ModalTitle.Text = title;
        ModalBody.Text = body;
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

    private void CloseModal()
    {
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(140));
        fade.Completed += (_, _) => ModalLayer.Visibility = Visibility.Collapsed;
        ModalLayer.BeginAnimation(OpacityProperty, fade);
    }

    // =====================================================================
    // Status bar
    // =====================================================================

    private void ReportProgress(ProgressPage page, InstallProgress p)
    {
        page.Report(p);
        StatusBarProgress.Value = p.Percent;
        StatusBarText.Text = p.Status;
        StatusBarDetail.Text = p.Detail;
        StatusBarPercent.Text = $"{p.Percent:0}%";
    }

    private void ShowStatusBar(bool visible)
    {
        StatusBar.BeginAnimation(HeightProperty,
            new DoubleAnimation(visible ? 44 : 0, TimeSpan.FromMilliseconds(220))
            { EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut } });
    }
}
