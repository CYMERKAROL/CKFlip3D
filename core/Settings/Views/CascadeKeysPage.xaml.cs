// ---------------------------------------------------------------------------
// Every key the open cascade listens to, and the editor for rebinding them.
// Navigation is a LIST rather than a single key, because moving forward is
// four keys at once and people reach for different ones.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using CKFlip3D.Settings.Models;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// Every key the open cascade listens to, reached from Controls → Mouse &amp;
/// keyboard.
///
/// All five bindings are LISTS, side by side: navigate forward, navigate back,
/// commit, cancel, close. Add a key by pressing it, remove one with the X, or
/// park one with its switch to keep it for later. Navigation is four keys at
/// once out of the box and people reach for different ones, so a single picker
/// per action was never enough. Seeing them together is half the point: this is
/// the only page that knows a key is taken and can say by what.
///
/// One key does ONE job. A press already spoken for is refused rather than
/// warned about. Two bindings on one mouse button are survivable, since the
/// core resolves them in a fixed order and the page says so, but a navigation
/// key sitting on top of Escape leaves a cascade that steps beautifully and
/// will not close, and the way out of that is this page, which is behind the
/// cascade. For the same reason the commit and cancel lists refuse to be
/// emptied: they are the ways out.
/// </summary>
public partial class CascadeKeysPage : UserControl
{
    /// <summary>
    /// One list on this page: its rows, where they live in the model, the
    /// chrome that describes them, and what it is called when another list has
    /// to explain the key is taken.
    /// </summary>
    private sealed class KeyList
    {
        public required string Role;            // "moves forward", "commits" …
        public required string AddTitle;
        public required string AddPrompt;
        public required ObservableCollection<BindingItem> Items;
        public required Func<List<Binding>> Read;
        public required Action<IEnumerable<Binding>> Write;
        public required ItemsControl View;
        public required UIElement Empty;
        public required TextBlock Status;
        public required Button Add;
        /// <summary>True for the two lists that are the way out of a cascade.</summary>
        public bool KeepOne;
    }

    private readonly List<KeyList> _lists;
    private bool _loading;

    public CascadeKeysPage()
    {
        InitializeComponent();

        _lists =
        [
            new KeyList
            {
                Role = "moves the stack forward",
                AddTitle = "Add a key that moves forward",
                AddPrompt = "Press the key that should move to the NEXT window while the "
                          + "cascade is open. A single key, or Shift with a key. Esc cancels.",
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.NavForwardKeyList,
                Write = keys => App.Settings.SetNavForwardKeyList(keys),
                View = ForwardList, Empty = ForwardEmpty, Status = ForwardStatus, Add = ForwardAdd,
            },
            new KeyList
            {
                Role = "moves the stack back",
                AddTitle = "Add a key that moves back",
                AddPrompt = "Press the key that should move to the PREVIOUS window while the "
                          + "cascade is open. A single key, or Shift with a key — Shift+Tab "
                          + "is the classic one. Esc cancels.",
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.NavBackKeyList,
                Write = keys => App.Settings.SetNavBackKeyList(keys),
                View = BackList, Empty = BackEmpty, Status = BackStatus, Add = BackAdd,
            },
            new KeyList
            {
                Role = "commits the selection",
                AddTitle = "Add a commit key",
                AddPrompt = "Press the key that should switch to the selected window. "
                          + "A single key, or Shift with a key. Esc cancels.",
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.CommitKeyList,
                Write = keys => App.Settings.SetCommitKeyList(keys),
                View = CommitList, Empty = CommitEmpty, Status = CommitStatus, Add = CommitAdd,
                KeepOne = true,
            },
            new KeyList
            {
                Role = "cancels the cascade",
                AddTitle = "Add a cancel key",
                AddPrompt = "Press the key that should close the cascade without switching. "
                          + "A single key, or Shift with a key. Esc cancels.",
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.CancelKeyList,
                Write = keys => App.Settings.SetCancelKeyList(keys),
                View = CancelList, Empty = CancelEmpty, Status = CancelStatus, Add = CancelAdd,
                KeepOne = true,
            },
            new KeyList
            {
                Role = "closes a window",
                AddTitle = "Add a close-window key",
                AddPrompt = "Press the key that should close the window you are pointing at. "
                          + "A single key, or Shift with a key. Esc cancels.",
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.CloseKeyList,
                Write = keys => App.Settings.SetCloseKeyList(keys),
                View = CloseList, Empty = CloseEmpty, Status = CloseStatus, Add = CloseAdd,
            },
        ];

        foreach (var list in _lists)
            list.View.ItemsSource = list.Items;

        // Per VISIT, like MouseKeyboardPage: this page is built fresh each time
        // it is opened, and a handler left behind would keep a dead page alive
        // and syncing for the rest of the session.
        Loaded += (_, _) =>
        {
            App.Settings.PropertyChanged += OnSettingsChanged;
            LoadFromModel();
        };
        Unloaded += (_, _) =>
        {
            App.Settings.PropertyChanged -= OnSettingsChanged;
            HotkeyService.StopCapture();
        };
    }

    private void OnSettingsChanged(object? sender, PropertyChangedEventArgs e)
    {
        // Reload only for changes made elsewhere (Revert, Reset, the Controls
        // page's Restore defaults). Our own edits already match what is on
        // screen, and rebuilding on them would be a rebuild per keystroke.
        if (_loading) return;
        if (e.PropertyName is nameof(SettingsModel.NavForwardKeys)
                           or nameof(SettingsModel.NavBackKeys)
                           or nameof(SettingsModel.CommitKeys)
                           or nameof(SettingsModel.CancelKeys)
                           or nameof(SettingsModel.CloseKeys)
                           or null)
            LoadFromModel();
        else if (e.PropertyName is nameof(SettingsModel.SearchEnabled))
            UpdateSearchClash();
    }

    // ---- Model → lists ------------------------------------------------------

    private void LoadFromModel()
    {
        _loading = true;
        foreach (var list in _lists)
            Fill(list.Items, list.Read());
        _loading = false;
        UpdateChrome();
    }

    private void Fill(ObservableCollection<BindingItem> target, List<Binding> keys)
    {
        foreach (var old in target)
            old.PropertyChanged -= OnItemChanged;
        target.Clear();
        foreach (var key in keys)
        {
            var item = new BindingItem(key);
            item.PropertyChanged += OnItemChanged;
            target.Add(item);
        }
    }

    /// <summary>
    /// A row's switch moved. The only refusal here is parking the last live
    /// entry of a list that has to keep one — the switch goes back on, because
    /// the alternative is a cascade with no way out.
    /// </summary>
    private void OnItemChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (_loading || sender is not BindingItem item) return;

        KeyList? owner = _lists.FirstOrDefault(l => l.Items.Contains(item));
        if (owner is { KeepOne: true } && !item.Enabled
            && !owner.Items.Any(i => i.Enabled))
        {
            _loading = true;              // the correction is not an edit
            item.Enabled = true;
            _loading = false;
            ShowRefusal($"“{item.Token}” is the only key that {owner.Role}. "
                      + "Add another one first — a cascade with no way out is not "
                      + "something these settings can save.");
            return;
        }
        Commit();
    }

    // ---- Lists → model ------------------------------------------------------

    private void Commit()
    {
        _loading = true;   // our own write must not bounce back as a reload
        foreach (var list in _lists)
            list.Write(list.Items.Select(i => i.ToBinding()));
        _loading = false;
        UpdateChrome();
    }

    private void UpdateChrome()
    {
        foreach (var list in _lists)
        {
            list.Empty.Visibility = list.Items.Count == 0
                ? Visibility.Visible : Visibility.Collapsed;

            bool full = list.Items.Count >= SettingsModel.MaxBindingKeys;
            list.Add.IsEnabled = !full;
            list.Status.Text = full
                ? $"{SettingsModel.MaxBindingKeys} keys is the most one list can hold. "
                  + "Remove one to add another."
                : string.Empty;
        }
        UpdateSearchClash();
    }

    /// <summary>The key half of an entry, ignoring any Shift prefix.</summary>
    private static uint KeyVk(string token) => HotkeyService.MainKeyVk(token);

    /// <summary>Does this entry need Shift held?</summary>
    private static bool NeedsShift(string token) =>
        token.Split('+', StringSplitOptions.TrimEntries)
             .SkipLast(1)
             .Any(p => string.Equals(p, "Shift", StringComparison.OrdinalIgnoreCase));

    /// <summary>
    /// A bound key that also TYPES something is claimed here first, so with
    /// Search on that character can never reach the query. Said out loud rather
    /// than refused: the key works perfectly well for someone who does not use
    /// Search, and Search is off by default.
    /// </summary>
    private void UpdateSearchClash()
    {
        if (!App.Settings.SearchEnabled)
        {
            SearchClash.Visibility = Visibility.Collapsed;
            return;
        }

        string typing = string.Join(", ", _lists.SelectMany(l => l.Items)
            .Where(i => i.Enabled && Types(KeyVk(i.Token)))
            .Select(i => i.Token));

        SearchClashText.Text = typing.Length == 0 ? string.Empty
            : $"{typing} would normally type into the search field. While Search is on, "
              + "these bindings are claimed first, so those characters cannot be typed "
              + "to filter the stack.";
        SearchClash.Visibility = typing.Length == 0
            ? Visibility.Collapsed : Visibility.Visible;

        static bool Types(uint vk) =>
            vk is (>= 0x41 and <= 0x5A)      // A-Z
               or (>= 0x30 and <= 0x39)      // 0-9
               or (>= 0x60 and <= 0x6F)      // numpad digits and operators
               or (>= 0xBA and <= 0xC0)      // ;=,-./`
               or (>= 0xDB and <= 0xDE)      // [\]'
               or 0x20;                      // Space
    }

    private void RemoveKey_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: BindingItem item }) return;

        KeyList? owner = _lists.FirstOrDefault(l => l.Items.Contains(item));
        if (owner == null) return;

        if (owner.KeepOne && owner.Items.Count(i => i.Enabled) <= 1 && item.Enabled)
        {
            ShowRefusal($"“{item.Token}” is the only key that {owner.Role}. "
                      + "Add another one first — a cascade with no way out is not "
                      + "something these settings can save.");
            return;
        }

        item.PropertyChanged -= OnItemChanged;
        owner.Items.Remove(item);
        Commit();
    }

    private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        App.Settings.NavForwardKeys = SettingsModel.DefaultNavForwardKeys;
        App.Settings.NavBackKeys = SettingsModel.DefaultNavBackKeys;
        App.Settings.CommitKeys = SettingsModel.DefaultCommitKeys;
        App.Settings.CancelKeys = SettingsModel.DefaultCancelKeys;
        App.Settings.CloseKeys = SettingsModel.DefaultCloseKeys;
        LoadFromModel();
    }

    // ---- Adding a key -------------------------------------------------------

    private void AddForward_Click(object sender, RoutedEventArgs e) => CaptureFor(_lists[0]);
    private void AddBack_Click(object sender, RoutedEventArgs e) => CaptureFor(_lists[1]);
    private void AddCommit_Click(object sender, RoutedEventArgs e) => CaptureFor(_lists[2]);
    private void AddCancel_Click(object sender, RoutedEventArgs e) => CaptureFor(_lists[3]);
    private void AddClose_Click(object sender, RoutedEventArgs e) => CaptureFor(_lists[4]);

    private void CaptureFor(KeyList target)
    {
        if (Window.GetWindow(this) is not MainWindow main)
            return;

        var (body, display) = MakeCaptureBody(target.AddPrompt);

        // allowReservedKeys stays FALSE: Esc is the way out of this dialog and
        // Enter is how a modal is accepted, so letting the capture take them
        // would trap the user in the dialog they are trying to leave. Both are
        // already bound by default anyway, and a key that is bound is refused.
        HotkeyService.StartCapture(
            onPreview: text => display.Text = text,
            onCaptured: combo => Dispatcher.BeginInvoke(() =>
            {
                main.CloseModal();
                TryAdd(target, combo);
            }),
            onCancelled: () => Dispatcher.BeginInvoke(main.CloseModal),
            allowReservedKeys: false);

        main.ShowModal(target.AddTitle, body, ("Cancel", false, HotkeyService.StopCapture));
    }

    private void TryAdd(KeyList target, string combo)
    {
        string? refusal = WhyNot(target, combo);
        if (refusal != null)
        {
            ShowRefusal(refusal);
            return;
        }

        var item = new BindingItem(new Binding(combo.Trim(), true));
        item.PropertyChanged += OnItemChanged;
        target.Items.Add(item);
        Commit();
    }

    /// <summary>
    /// Reason this key cannot join the list, or null.
    ///
    /// The bar is EXACT collision, not overlap. Win+Tab and a bare Tab share a
    /// key and are still two different presses — refusing the second because of
    /// the first would take Tab away from the very list it belongs on. What is
    /// refused is a binding that is the same press as one already spoken for:
    /// an entry on another list here, or a bare key that IS the whole
    /// activation hotkey.
    /// </summary>
    private string? WhyNot(KeyList target, string combo)
    {
        if (string.IsNullOrWhiteSpace(combo))
            return "Nothing was recorded. Try again.";

        string[] parts = combo.Split('+', StringSplitOptions.TrimEntries);
        uint vk = HotkeyService.TokenToVk(parts[^1]);
        bool shift = NeedsShift(combo);

        if (vk == 0)
            return $"“{combo}” is not a key the cascade can bind. Mouse buttons have "
                 + "their own settings on the Mouse & keyboard page.";

        // Shift, and nothing else. The others would have to be held alongside a
        // hotkey that is itself a combination.
        if (parts.Length > (shift ? 2 : 1))
            return $"“{combo}” uses a modifier other than Shift. Only Shift is free "
                 + "here — the rest may already be held down as part of the hotkey "
                 + "that opened the cascade.";

        bool Same(BindingItem i) => KeyVk(i.Token) == vk && NeedsShift(i.Token) == shift;

        if (target.Items.Any(Same))
            return $"“{combo}” is already on this list.";

        foreach (var other in _lists)
        {
            if (ReferenceEquals(other, target) || !other.Items.Any(Same)) continue;
            return $"“{combo}” already {other.Role}. One key does one job — remove it "
                 + "there first if it belongs here instead.";
        }

        // Exact 1:1 with the binding that opens the cascade.
        if (!shift && IsWholeBinding(App.Settings.ActivationHotkey, vk))
            return $"“{combo}” is the whole activation hotkey. Pressing it would open "
                 + "the cascade rather than act on it.";

        if (target.Items.Count >= SettingsModel.MaxBindingKeys)
            return $"One list holds at most {SettingsModel.MaxBindingKeys} keys.";

        return null;
    }

    /// <summary>
    /// Is <paramref name="binding"/> exactly this bare key — no modifiers?
    /// "Win+Tab" is not Tab; "Escape" is Escape.
    /// </summary>
    private static bool IsWholeBinding(string binding, uint vk)
    {
        string[] parts = (binding ?? "").Split('+', StringSplitOptions.TrimEntries);
        return parts.Length == 1 && HotkeyService.TokenToVk(parts[0]) == vk;
    }

    private void ShowRefusal(string text)
    {
        if (Window.GetWindow(this) is not MainWindow main) return;

        var body = new TextBlock
        {
            Text = text,
            TextWrapping = TextWrapping.Wrap,
            FontFamily = new FontFamily("Segoe UI"),
            FontSize = 12,
            MaxWidth = 420,
            Foreground = (Brush)main.FindResource("TextPrimaryBrush"),
        };
        main.ShowModal("That key is taken", body, ("OK", true, null));
    }

    /// <summary>Same capture dialog body as the other binding pickers.</summary>
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
            MaxWidth = 420,
            Foreground = (Brush)FindResource("TextPrimaryBrush"),
        });
        body.Children.Add(display);
        return (body, display);
    }
}

/// <summary>
/// One row of a binding list. A view object rather than the model's own
/// <see cref="Binding"/> record because the switch writes back into it and the
/// row has to hear about it; the model stores the whole list as one string.
/// </summary>
public sealed class BindingItem : INotifyPropertyChanged
{
    public BindingItem(Binding key)
    {
        Token = key.Token;
        _enabled = key.Enabled;
    }

    public string Token { get; }

    private bool _enabled;
    public bool Enabled
    {
        get => _enabled;
        set
        {
            if (_enabled == value) return;
            _enabled = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Enabled)));
        }
    }

    public Binding ToBinding() => new(Token, Enabled);

    public event PropertyChangedEventHandler? PropertyChanged;
}
