using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Mouse &amp; keyboard bindings, reached from Controls → Input.
///
/// Everything the cascade listens to from a hand on the desk lives here: the
/// combination that opens it, the keys that commit, cancel and close, and the
/// three in-cascade mouse buttons.  Grouping them was the point — the
/// alternative was several unrelated rows scattered across the Controls page,
/// none of which told you what the others were bound to.
///
/// Every binding is set by PRESSING it rather than by picking from a list, so
/// there is one way to answer "what should this be?" throughout: do the thing.
/// </summary>
public partial class MouseKeyboardPage : UserControl
{
    private bool _syncing;

    public MouseKeyboardPage()
    {
        InitializeComponent();
        // Subscribed per VISIT, not per construction: this page is built fresh
        // every time Controls → Mouse & keyboard is opened, and a handler left
        // behind would keep a dead page alive and syncing for the rest of the
        // session — once per visit, forever.
        Loaded += (_, _) =>
        {
            App.Settings.PropertyChanged += OnSettingsChanged;
            SyncFromModel();
        };
        Unloaded += (_, _) =>
        {
            App.Settings.PropertyChanged -= OnSettingsChanged;
            HotkeyService.StopCapture();
        };
    }

    private void OnSettingsChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName is nameof(Models.SettingsModel.ActivationHotkey)
                           or nameof(Models.SettingsModel.HotkeyToggleMode)
                           or nameof(Models.SettingsModel.PointerInCascade)
                           or nameof(Models.SettingsModel.MouseSelect)
                           or nameof(Models.SettingsModel.MouseSelectButton)
                           or nameof(Models.SettingsModel.MouseDragEnabled)
                           or nameof(Models.SettingsModel.MouseDragButton)
                           or nameof(Models.SettingsModel.CloseFromCascade)
                           or nameof(Models.SettingsModel.CloseKeyEnabled)
                           or nameof(Models.SettingsModel.MouseCloseButton)
                           or nameof(Models.SettingsModel.WindowSnap)
                           or null)
            SyncFromModel();
    }

    // ---- Model → controls ---------------------------------------------------

    private void SyncFromModel()
    {
        _syncing = true;

        SelectButtonText.Text = ButtonToken(App.Settings.MouseSelectButton);
        CloseButtonText.Text = ButtonToken(App.Settings.MouseCloseButton);
        DragButtonText.Text = ButtonToken(App.Settings.MouseDragButton);

        SyncToggleMode();

        // The close KEY now carries its OWN switch, on its own row.  Nothing
        // here may disable that row: the switch lives inside it, and greying
        // the row out would make the binding impossible to turn back on.  The
        // key-picker button alone follows the switch, through its XAML binding.

        // Dragging the stack only exists while Window snap is off (General →
        // Cascade), so say so rather than offering a binding that does
        // nothing.
        bool dragLive = !App.Settings.WindowSnap;
        DragRow.IsEnabled = dragLive;
        DragRow.Opacity = dragLive ? 1.0 : 0.55;
        DragHint.Text = dragLive
            ? "Hold this button and move to scrub the stack freely, letting it settle onto the nearest window when you let go."
            : "Only used while Window snap is off (General → Cascade). With snapping on, every input steps one whole window at a time.";

        UpdateConflictWarning();
        _syncing = false;
    }

    /// <summary>
    /// Two bindings on one button is not fatal — the core resolves it in a
    /// fixed order (pick, then close, then drag) — but it is almost never what
    /// someone meant, so it is worth saying out loud.
    /// </summary>
    private void UpdateConflictWarning()
    {
        var used = new List<(string name, int button)>();
        if (App.Settings.MouseSelect) used.Add(("Pick a window", App.Settings.MouseSelectButton));
        if (App.Settings.CloseFromCascade) used.Add(("Close a window", App.Settings.MouseCloseButton));
        if (!App.Settings.WindowSnap && App.Settings.MouseDragEnabled)
            used.Add(("Drag the stack", App.Settings.MouseDragButton));

        string? clash = null;
        for (int i = 0; i < used.Count && clash == null; i++)
        {
            for (int j = i + 1; j < used.Count; j++)
            {
                if (used[i].button == used[j].button)
                {
                    clash = $"“{used[i].name}” and “{used[j].name}” are both on "
                          + $"{ButtonName(used[i].button)}. Only “{used[i].name}” will happen.";
                    break;
                }
            }
        }

        // The activation hotkey is a mouse button too when it is bound to one,
        // and it is checked first of all in the core.
        if (clash == null)
        {
            int activationButton = ActivationMouseButton(App.Settings.ActivationHotkey);
            if (activationButton != 0)
            {
                foreach (var (name, button) in used)
                {
                    if (button == activationButton)
                    {
                        clash = $"The activation hotkey uses {ButtonName(button)}, so “{name}” "
                              + "will never fire while the cascade is open.";
                        break;
                    }
                }
            }
        }

        ConflictText.Text = clash ?? string.Empty;
        ConflictWarning.Visibility = clash == null ? Visibility.Collapsed : Visibility.Visible;
    }

    // ---- Button id ↔ shared token -------------------------------------------
    // The tokens are the same ones KeyboardHook::ParseHotkey understands, so
    // the capture can hand its result straight through.

    private static string ButtonToken(int id) => id switch
    {
        1 => "LButton",
        2 => "RButton",
        3 => "MButton",
        4 => "XButton1",
        5 => "XButton2",
        _ => "None",
    };

    private static int TokenToButton(string token) => token.ToLowerInvariant() switch
    {
        "lbutton" => 1,
        "rbutton" => 2,
        "mbutton" or "middlebutton" => 3,
        "xbutton1" or "mouse4" => 4,
        "xbutton2" or "mouse5" => 5,
        _ => 0,
    };

    private static string ButtonName(int id) => id switch
    {
        1 => "the left button",
        2 => "the right button",
        3 => "the middle button",
        4 => "Mouse 4",
        5 => "Mouse 5",
        _ => "no button",
    };

    /// <summary>Mouse-button id the activation combo ends in, or 0.</summary>
    private static int ActivationMouseButton(string combo) =>
        TokenToButton(combo.Split('+', StringSplitOptions.TrimEntries)[^1]);

    // ---- Toggle activation --------------------------------------------------
    // A binding without modifiers (bare key, bare mouse button, bare Win) is
    // inherently toggle in the core, so the switch is shown ON and grayed out
    // without writing the forced state into the persisted value.

    private static bool IsSingleKeyBinding(string combo) => !combo.Contains('+');

    private void SyncToggleMode()
    {
        if (IsSingleKeyBinding(App.Settings.ActivationHotkey))
        {
            ToggleModeCheck.IsChecked = true;
            ToggleModeCheck.IsEnabled = false;
            ToggleModeRow.Opacity = 0.55;
            ToggleModeHint.Text = "Single-key bindings always toggle — the cascade "
                + "stays open until the commit key confirms or the cancel key closes it.";
        }
        else
        {
            ToggleModeCheck.IsEnabled = true;
            ToggleModeCheck.IsChecked = App.Settings.HotkeyToggleMode;
            ToggleModeRow.Opacity = 1.0;
            ToggleModeHint.Text = "Keeps the cascade open after the hotkey is released "
                + "— commit, cancel, and keep cycling with the main key. Off restores the "
                + "classic hold-to-keep-open behaviour. Typing into Search switches a "
                + "session to this on its own, so a word can be typed without the "
                + "modifier release closing the cascade mid-way.";
        }
    }

    private void ToggleMode_Changed(object sender, RoutedEventArgs e)
    {
        if (_syncing || !ToggleModeCheck.IsEnabled) return;
        App.Settings.HotkeyToggleMode = ToggleModeCheck.IsChecked == true;
    }

    // ---- Key capture --------------------------------------------------------

    private void ChangeHotkey_Click(object sender, RoutedEventArgs e) =>
        CaptureKey("Set activation hotkey",
                   "Press the new activation combination — keyboard keys, mouse "
                   + "buttons or both (mouse movement and wheel are ignored). "
                   + "A bare left click cannot be bound. Esc cancels.",
                   allowReserved: false,
                   assign: ConfirmAndAssignActivation);

    private void ChangeCommit_Click(object sender, RoutedEventArgs e) =>
        CaptureKey("Set commit key",
                   "Press the key that should switch to the selected window. "
                   + "Use Cancel below to keep the current one.",
                   allowReserved: true,
                   assign: combo => App.Settings.CommitHotkey = combo);

    private void ChangeCancel_Click(object sender, RoutedEventArgs e) =>
        CaptureKey("Set cancel key",
                   "Press the key that should close the cascade without switching. "
                   + "Use Cancel below to keep the current one.",
                   allowReserved: true,
                   assign: combo => App.Settings.CancelHotkey = combo);

    private void ChangeCloseKey_Click(object sender, RoutedEventArgs e) =>
        CaptureKey("Set close-window key",
                   "Press the key that should close the window you are pointing at. "
                   + "Use Cancel below to keep the current one.",
                   allowReserved: true,
                   assign: combo => App.Settings.CloseHotkey = combo);

    // ---- Mouse-button capture ----------------------------------------------

    private void ChangeSelectButton_Click(object sender, RoutedEventArgs e) =>
        CaptureButton("Set the pick button",
                      "Click the button that should pick a window from the stack.",
                      id => App.Settings.MouseSelectButton = id);

    private void ChangeCloseButton_Click(object sender, RoutedEventArgs e) =>
        CaptureButton("Set the close button",
                      "Click the button that should close the window you are pointing at.",
                      id => App.Settings.MouseCloseButton = id);

    private void ChangeDragButton_Click(object sender, RoutedEventArgs e) =>
        CaptureButton("Set the drag button",
                      "Click the button that should drag the stack while Window snap is off.",
                      id => App.Settings.MouseDragButton = id);

    private void CaptureKey(string title, string prompt, bool allowReserved,
                            Action<string> assign)
    {
        if (Window.GetWindow(this) is not MainWindow main)
            return;

        var (body, display) = MakeCaptureBody(prompt);
        HotkeyService.StartCapture(
            onPreview: text => display.Text = text,
            onCaptured: combo => Dispatcher.BeginInvoke(() =>
            {
                main.CloseModal();
                assign(combo);
            }),
            onCancelled: () => Dispatcher.BeginInvoke(main.CloseModal),
            allowReservedKeys: allowReserved);

        main.ShowModal(title, body, ("Cancel", false, HotkeyService.StopCapture));
    }

    private void CaptureButton(string title, string prompt, Action<int> assign)
    {
        if (Window.GetWindow(this) is not MainWindow main)
            return;

        var (body, display) = MakeCaptureBody(
            prompt + " Click anywhere except this dialog's Cancel button; Esc also cancels.");

        main.ShowModal(title, body, ("Cancel", false, HotkeyService.StopCapture));

        // The dead zone can only be measured once the modal is laid out, so
        // arming waits for the layout pass the ShowModal above queued.
        Dispatcher.BeginInvoke(new Action(() =>
        {
            HotkeyService.StartMouseButtonCapture(
                onPreview: text => display.Text = text,
                onCaptured: token => Dispatcher.BeginInvoke(() =>
                {
                    main.CloseModal();
                    int id = TokenToButton(token);
                    if (id != 0)
                        assign(id);
                }),
                onCancelled: () => Dispatcher.BeginInvoke(main.CloseModal),
                deadZoneScreen: main.ModalButtonsScreenRect());
        }), System.Windows.Threading.DispatcherPriority.Loaded);
    }

    private (StackPanel body, TextBlock display) MakeCaptureBody(string prompt)
    {
        var display = new TextBlock
        {
            Text = "…",
            FontFamily = new FontFamily("Consolas"),
            FontSize = 20,
            HorizontalAlignment = HorizontalAlignment.Center,
            Margin = new Thickness(0, 14, 0, 14),
        };
        display.SetResourceReference(TextBlock.ForegroundProperty, "AccentBrush");

        var body = new StackPanel();
        body.Children.Add(new TextBlock
        {
            Text = prompt,
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 12,
            Foreground = (Brush)FindResource("TextPrimaryBrush"),
        });
        body.Children.Add(display);
        return (body, display);
    }

    private void ConfirmAndAssignActivation(string combo)
    {
        if (Window.GetWindow(this) is not MainWindow main)
            return;

        string? warning = HotkeyService.GetWarning(combo);
        if (warning == null)
        {
            App.Settings.ActivationHotkey = combo;
            return;
        }

        var body = new TextBlock
        {
            Text = $"Detected: {combo}\n\n{warning}",
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 12,
            Foreground = (Brush)main.FindResource("TextPrimaryBrush"),
        };
        main.ShowModal("Use this combination?", body,
            ("Use anyway", true, () => App.Settings.ActivationHotkey = combo),
            ("Cancel", false, null));
    }

    private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.ActivationHotkey = "Win+Tab";
        App.Settings.HotkeyToggleMode = false;
        App.Settings.CommitHotkey = "Enter";
        App.Settings.CancelHotkey = "Escape";
        App.Settings.CloseHotkey = "Delete";
        App.Settings.CloseKeyEnabled = true;
        // Off by default: the pointer bindings are opt-in, so "restore
        // defaults" must restore them to opt-in rather than switch them on.
        // If Window snap is currently off, this leaves the settings in the
        // combination Apply refuses (SettingsModel.WindowSnapSatisfied) —
        // deliberately visible rather than silently corrected, since the two
        // switches belong to the user, not to this button.
        App.Settings.PointerInCascade = false;
        App.Settings.MouseSelect = true;
        App.Settings.MouseSelectButton = 1;
        App.Settings.CloseFromCascade = true;
        App.Settings.MouseCloseButton = 3;
        App.Settings.MouseDragEnabled = true;
        App.Settings.MouseDragButton = 2;
    }
}
