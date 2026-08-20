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
        /// <summary>Card header, and what another list calls this one.</summary>
        public required string Name;
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

        // One prompt for all five: the dialog title already says which action
        // is being bound, so repeating it costs a line and says nothing.
        const string prompt = "Press a key or mouse button to set the binding. "
                            + "Hold Ctrl, Shift, Alt, or Win for combinations. "
                            + "Press Cancel to exit.";

        _lists =
        [
            new KeyList
            {
                Name = "Next window",
                AddTitle = "Add a key that moves forward",
                AddPrompt = prompt,
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.NavForwardKeyList,
                Write = keys => App.Settings.SetNavForwardKeyList(keys),
                View = ForwardList, Empty = ForwardEmpty, Status = ForwardStatus, Add = ForwardAdd,
            },
            new KeyList
            {
                Name = "Previous window",
                AddTitle = "Add a key that moves back",
                AddPrompt = prompt,
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.NavBackKeyList,
                Write = keys => App.Settings.SetNavBackKeyList(keys),
                View = BackList, Empty = BackEmpty, Status = BackStatus, Add = BackAdd,
            },
            new KeyList
            {
                Name = "Confirm",
                AddTitle = "Add a confirm key",
                AddPrompt = prompt,
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.CommitKeyList,
                Write = keys => App.Settings.SetCommitKeyList(keys),
                View = CommitList, Empty = CommitEmpty, Status = CommitStatus, Add = CommitAdd,
                KeepOne = true,
            },
            new KeyList
            {
                Name = "Cancel",
                AddTitle = "Add a cancel key",
                AddPrompt = prompt,
                Items = new ObservableCollection<BindingItem>(),
                Read = () => App.Settings.CancelKeyList,
                Write = keys => App.Settings.SetCancelKeyList(keys),
                View = CancelList, Empty = CancelEmpty, Status = CancelStatus, Add = CancelAdd,
                KeepOne = true,
            },
            new KeyList
            {
                Name = "Close window",
                AddTitle = "Add a close-window key",
                AddPrompt = prompt,
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
        else if (e.PropertyName is nameof(SettingsModel.ActivationHotkey)
                                or nameof(SettingsModel.HotkeyToggleMode))
            UpdateHotkeyClash();
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
        // The switch is the only edit a row can make. Warning is written BY
        // this page a moment later, and treating it as one would commit the
        // lists again for every mark that appeared.
        if (e.PropertyName != nameof(BindingItem.Enabled)) return;

        KeyList? owner = _lists.FirstOrDefault(l => l.Items.Contains(item));
        if (owner is { KeepOne: true } && !item.Enabled
            && !owner.Items.Any(i => i.Enabled))
        {
            _loading = true;              // the correction is not an edit
            item.Enabled = true;
            _loading = false;
            ShowRefusal($"“{item.Token}” is the only key left for {owner.Name}. "
                      + "Add another one first, or the cascade has no way out.");
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
        UpdateHotkeyClash();
        UpdateSearchClash();
    }

    /// <summary>The key or button half of an entry, its modifiers aside.</summary>
    private static uint KeyVk(string token) => HotkeyService.MainBindingVk(token);

    /// <summary>The modifiers this entry needs held with it.</summary>
    private static uint Mods(string token) => HotkeyService.ModsOf(token);

    /// <summary>
    /// The modifiers the activation hotkey holds down, or none when nothing is
    /// being held: a hotkey with no modifier is inherently toggle, and Toggle
    /// activation makes every combination behave that way.
    /// </summary>
    private static uint HeldByHotkey() =>
        App.Settings.HotkeyToggleMode ? 0 : HotkeyService.ModsOf(App.Settings.ActivationHotkey);

    /// <summary>
    /// A binding that asks for a modifier the hotkey is already holding cannot
    /// fire: the core discounts those, because a combination keeps them down
    /// for the whole session and counting them would turn every forward step
    /// into a backward one. Said out loud rather than refused, because Toggle
    /// activation is one switch away and the binding is fine the moment it is
    /// on.
    ///
    /// The same pass marks a row that IS the activation hotkey, which is a
    /// different fault with a different answer: the hotkey moved onto it, or
    /// the config was edited by hand. Apply is gone until one of the two gives.
    /// </summary>
    private void UpdateHotkeyClash()
    {
        uint held = HeldByHotkey();
        string hotkey = App.Settings.ActivationHotkey;
        var blocked = new List<string>();

        // Every row is visited, not just the ones that clash: a mark left on a
        // row that has since been freed (Toggle activation switched on, hotkey
        // rebound) would be the page stating something untrue.
        foreach (var list in _lists)
        foreach (var item in list.Items)
        {
            item.Warning = string.Empty;
            if (!item.Enabled) continue;

            // The exact hotkey is refused on the way in, so a row carrying it
            // arrived some other way: a hand-edited config, or the hotkey being
            // pointed at it afterwards.  Marked rather than corrected — the
            // lists belong to the user — and Apply is gone until it is dealt
            // with (SettingsModel.ActivationBindingClash).
            if (App.Settings.ClashesWithActivation(item.Token))
            {
                item.Warning = $"This binding is the activation hotkey. {hotkey} "
                             + "opens the cascade, so it cannot also act on it.";
                continue;
            }

            uint shared = Mods(item.Token) & held;
            if (shared == 0) continue;

            blocked.Add(item.Token);
            item.Warning = $"This binding will not fire. {hotkey} holds "
                         + $"{ModNames(shared)} down for the whole session. "
                         + "Turn on Toggle activation to use it.";
        }

        HotkeyClashText.Text = blocked.Count == 0 ? string.Empty
            : $"{string.Join(", ", blocked)} will not fire. {hotkey} holds "
            + $"{ModNames(held)} down for the whole session. Turn on Toggle "
            + "activation to use them.";
        HotkeyClash.Visibility = blocked.Count == 0
            ? Visibility.Collapsed : Visibility.Visible;
    }

    private static string ModNames(uint mods)
    {
        var names = new List<string>(4);
        if ((mods & HotkeyService.ModCtrl) != 0) names.Add("Ctrl");
        if ((mods & HotkeyService.ModShift) != 0) names.Add("Shift");
        if ((mods & HotkeyService.ModAlt) != 0) names.Add("Alt");
        if ((mods & HotkeyService.ModWin) != 0) names.Add("Win");
        return names.Count <= 1 ? string.Concat(names)
            : string.Join(" and ", string.Join(", ", names.SkipLast(1)), names[^1]);
    }

    /// <summary>
    /// A bound key that also TYPES something is claimed here first, so with
    /// Search on that character can never reach the query. Said out loud rather
    /// than refused: the key works perfectly well for someone who does not use
    /// Search, and Search is off by default.
    ///
    /// Only bindings with NO modifier are worth saying it about. Anything held
    /// with the key is a combination the user reached for deliberately, and one
    /// they cannot type by accident while filtering the stack.
    /// </summary>
    private void UpdateSearchClash()
    {
        if (!App.Settings.SearchEnabled)
        {
            SearchClash.Visibility = Visibility.Collapsed;
            return;
        }

        string typing = string.Join(", ", _lists.SelectMany(l => l.Items)
            .Where(i => i.Enabled && Mods(i.Token) == 0 && Types(KeyVk(i.Token)))
            .Select(i => i.Token));

        SearchClashText.Text = typing.Length == 0 ? string.Empty
            : $"{typing} cannot be typed into the search field. Bindings are claimed "
              + "before typing, so those characters will not filter the stack.";
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
            ShowRefusal($"“{item.Token}” is the only key left for {owner.Name}. "
                      + "Add another one first, or the cascade has no way out.");
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

        main.ShowModal(target.AddTitle, body, ("Cancel", false, HotkeyService.StopCapture));

        // The way out is the Cancel button, never a key: Esc and Enter are the
        // cascade's own cancel and commit keys, and a picker that swallowed
        // them could not bind the very keys it exists to rebind. The button
        // strip goes over as a dead zone, so it keeps taking clicks while a
        // click anywhere else is a binding. Measuring it needs the modal laid
        // out, so arming waits for the layout pass ShowModal just queued.
        Dispatcher.BeginInvoke(new Action(() =>
        {
            HotkeyService.StartCapture(
                onPreview: text => display.Text = text,
                onCaptured: combo => Dispatcher.BeginInvoke(() =>
                {
                    main.CloseModal();
                    TryAdd(target, combo);
                }),
                onCancelled: () => Dispatcher.BeginInvoke(main.CloseModal),
                allowReservedKeys: true,
                deadZoneScreen: main.ModalButtonsScreenRect(),
                allowBareModifiers: false);
        }), System.Windows.Threading.DispatcherPriority.Loaded);
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
    /// What is refused is a press already spoken for — by another list here, or
    /// by the activation hotkey itself — and a binding the core cannot route at
    /// all. A press the hotkey merely makes awkward, by holding a modifier it
    /// also asks for, is a warning instead (see UpdateHotkeyClash): the answer
    /// there is a switch on another page, not a binding the user may not have.
    /// </summary>
    private string? WhyNot(KeyList target, string combo)
    {
        if (string.IsNullOrWhiteSpace(combo))
            return "Nothing was recorded. Try again.";

        uint vk = KeyVk(combo);
        uint mods = Mods(combo);

        if (HotkeyService.IsBareModifier(combo))
            return $"“{combo}” cannot be bound on its own. The cascade answers "
                 + "modifier keys before any binding. Press it together with a key "
                 + "or a mouse button instead.";

        if (vk == 0)
            return $"“{combo}” is not something the cascade can bind.";

        bool Same(BindingItem i) => KeyVk(i.Token) == vk && Mods(i.Token) == mods;

        if (target.Items.Any(Same))
            return $"“{combo}” is already on this list.";

        foreach (var other in _lists)
        {
            if (ReferenceEquals(other, target) || !other.Items.Any(Same)) continue;
            return $"“{combo}” is already bound to {other.Name}. Remove it from that "
                 + "action first if you want to assign it here.";
        }

        // The press that opens the cascade is as spoken for as one already on a
        // list here, navigation included.
        if (App.Settings.ClashesWithActivation(combo))
            return $"“{combo}” is already the activation hotkey. Change it on the "
                 + "Mouse & keyboard page first if you want to assign it here.";

        if (target.Items.Count >= SettingsModel.MaxBindingKeys)
            return $"One list holds at most {SettingsModel.MaxBindingKeys} keys.";

        return null;
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
        main.ShowModal("Binding not applied", body, ("OK", true, null));
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

    /// <summary>
    /// Why this binding cannot reach the cascade as things stand, or "" when it
    /// can. Drives the amber mark on the row and its tooltip; the page rewrites
    /// it whenever the lists or the activation hotkey change.
    /// </summary>
    private string _warning = "";
    public string Warning
    {
        get => _warning;
        set
        {
            if (_warning == value) return;
            _warning = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Warning)));
        }
    }

    public Binding ToBinding() => new(Token, Enabled);

    public event PropertyChangedEventHandler? PropertyChanged;
}
