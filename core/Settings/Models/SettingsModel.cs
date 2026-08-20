// ---------------------------------------------------------------------------
// Everything the settings window binds to, in one observable object.  It also
// tracks whether anything has changed since the last save, which is what makes
// the Apply bar appear and disappear on its own.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.ComponentModel;
using System.Runtime.CompilerServices;
using CKFlip3D.Settings.Services;

namespace CKFlip3D.Settings.Models;

/// <summary>
/// All settings shown by the UI. Properties marked [Core] map 1:1 onto keys the
/// C++ core reads (core/Config.h). Properties marked [ComingSoon] are persisted
/// but their controls are disabled in the UI until the core consumes them.
/// StartWithWindows is special: its source of truth is the HKCU Run registry
/// key (StartupService), not config.json.
/// </summary>
public sealed class SettingsModel : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>Raised whenever any value diverges from / returns to the last saved snapshot.</summary>
    public event Action? DirtyChanged;

    private bool _suppressDirty;
    private SettingsModel? _snapshot;

    private void Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        if (!_suppressDirty)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsDirty)));
            DirtyChanged?.Invoke();
        }
    }

    // ---- General ----------------------------------------------------------
    private bool _startWithWindows;        // mirrors the scheduled logon task
    private string _excludedApps = "";     // [Core] excludedApps — ';'-separated exe paths/names

    // The launch shortcut is deliberately NOT here: it is a button that writes
    // a file, not a setting with a saved state (see LaunchShortcutService).

    public bool StartWithWindows { get => _startWithWindows; set => Set(ref _startWithWindows, value); }
    public string ExcludedApps   { get => _excludedApps;     set => Set(ref _excludedApps, value); }

    /// <summary>Split helper for the exclusion-list editor page.</summary>
    public List<string> ExcludedAppsList =>
        _excludedApps.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();

    public void SetExcludedAppsList(IEnumerable<string> entries) =>
        ExcludedApps = string.Join(';', entries.Where(e => !string.IsNullOrWhiteSpace(e)));

    // ---- Appearance --------------------------------------------------------
    private int _appTheme;                 // Settings-app theme (ThemeService.Themes index)
    private int _visualPreset;             // [Core] visualPreset — 0 = Cascade, 1 = Cover Flow
    private bool _reflections;             // [Core] reflections — glass floor mirror under the stack
    private int _backgroundOpacity = 28;   // [Core] 0..100 %; 28 == original kBgAlpha look
    private int _backgroundBlur;           // [Core] backgroundBlur — 0..100 %, 0 = off
    private bool _antialiasing = true;     // [Core] antialiasing
    private bool _motionBlur = true;       // [Core] motionBlur
    private bool _animations = true;       // [Core] animations — master switch
    private bool _animEntryExit = true;    // [Core] animEntryExit — enter/exit morph
    private bool _animCycle = true;        // [Core] animCycle — Tab/Shift-Tab cycling
    private bool _animClose = true;        // [Core] animClose — window-close reflow
    private bool _animLabel = true;        // [Core] animLabel — selected-label glide + hold fade
    private bool _animHover = true;        // [Core] animHover — pointer-hover tile lift
    private bool _livePreview = true;      // [Core] livePreview — live WGC thumbnails
    private bool _liveBackground = true;   // [Core] liveBackground — live wallpaper backdrop
    private bool _vsyncLivePreview;        // [Core] vsyncLivePreview — Present(1) pacing
    private bool _taskbarLivePreview;      // [Core] taskbarLivePreview — live Shell_TrayWnd capture
    private bool _taskbarPreview = true;   // [Core] taskbarPreview — draw a taskbar preview at all
    private bool _showDesktopTile = true;  // [Core] showDesktopTile — desktop pseudo-window in the cascade
    private bool _selectedLabel;           // [Core] selectedLabel — front-slot label master switch (default off)
    private bool _selectedLabelTitle = true; // [Core] selectedLabelTitle — window title part
    private bool _selectedLabelIcon = true;  // [Core] selectedLabelIcon — program icon part
    private bool _selectedLabelBox = true;   // [Core] selectedLabelBox — aero-glass plate behind the label

    public int AppTheme          { get => _appTheme;          set => Set(ref _appTheme, Math.Clamp(value, 0, 4)); }
    public int VisualPreset      { get => _visualPreset;      set => Set(ref _visualPreset, Math.Clamp(value, 0, 1)); }
    public bool Reflections      { get => _reflections;       set => Set(ref _reflections, value); }
    public int BackgroundOpacity { get => _backgroundOpacity; set => Set(ref _backgroundOpacity, Math.Clamp(value, 0, 100)); }
    public int BackgroundBlur    { get => _backgroundBlur;    set => Set(ref _backgroundBlur, Math.Clamp(value, 0, 100)); }
    public bool Antialiasing     { get => _antialiasing;      set => Set(ref _antialiasing, value); }
    public bool MotionBlur       { get => _motionBlur;        set => Set(ref _motionBlur, value); }
    public bool Animations       { get => _animations;        set => Set(ref _animations, value); }
    public bool AnimEntryExit    { get => _animEntryExit;     set => Set(ref _animEntryExit, value); }
    public bool AnimCycle        { get => _animCycle;         set => Set(ref _animCycle, value); }
    public bool AnimClose        { get => _animClose;         set => Set(ref _animClose, value); }
    public bool AnimLabel        { get => _animLabel;         set => Set(ref _animLabel, value); }
    public bool AnimHover        { get => _animHover;         set => Set(ref _animHover, value); }
    public bool LivePreview      { get => _livePreview;       set => Set(ref _livePreview, value); }
    public bool LiveBackground   { get => _liveBackground;    set => Set(ref _liveBackground, value); }
    public bool VsyncLivePreview   { get => _vsyncLivePreview;   set => Set(ref _vsyncLivePreview, value); }
    public bool TaskbarLivePreview { get => _taskbarLivePreview; set => Set(ref _taskbarLivePreview, value); }
    public bool TaskbarPreview     { get => _taskbarPreview;     set => Set(ref _taskbarPreview, value); }
    public bool ShowDesktopTile    { get => _showDesktopTile;    set => Set(ref _showDesktopTile, value); }
    public bool SelectedLabel      { get => _selectedLabel;      set => Set(ref _selectedLabel, value); }
    public bool SelectedLabelTitle { get => _selectedLabelTitle; set => Set(ref _selectedLabelTitle, value); }
    public bool SelectedLabelIcon  { get => _selectedLabelIcon;  set => Set(ref _selectedLabelIcon, value); }
    public bool SelectedLabelBox   { get => _selectedLabelBox;   set => Set(ref _selectedLabelBox, value); }

    // ---- Multi-monitor -----------------------------------------------------
    private int _cascadeMonitor = -1;      // [ComingSoon] -1 = primary
    private int _secondaryTaskbarMode;     // [ComingSoon] 0=Dim, 1=Hide, 2=Keep visible

    public int CascadeMonitor       { get => _cascadeMonitor;       set => Set(ref _cascadeMonitor, value); }
    public int SecondaryTaskbarMode { get => _secondaryTaskbarMode; set => Set(ref _secondaryTaskbarMode, value); }

    // ---- Controls -----------------------------------------------------------
    private bool _ignoreFullscreen;        // [Core] ignoreFullscreen
    private bool _mouseWheelCycle = true;  // [Core] mouseWheelCycle
    // [Core] navForwardKeys / navBackKeys — ';'-separated key tokens that step
    // through the open stack, the activation hotkey's own key included (hence
    // Tab in the defaults).  A token may carry Shift and nothing else.  A '!'
    // prefix means "kept, but switched off", so a binding can be parked without
    // being retyped.  Empty is legitimate: it means that direction has no key
    // at all.  See NavKeyList / SetNavKeyList.
    private string _navForwardKeys = DefaultNavForwardKeys;
    private string _navBackKeys = DefaultNavBackKeys;
    private string _ignoredApps = "";      // [Core] ignoredApps — ';'-separated exe paths/names
    private bool _hotkeyToggleMode;        // [Core] hotkeyToggleMode — combo bindings toggle instead of hold
    private string _activationHotkey = "Win+Tab"; // [Core] activationHotkey — see HotkeyService

    public bool IgnoreFullscreen { get => _ignoreFullscreen; set => Set(ref _ignoreFullscreen, value); }
    public bool MouseWheelCycle  { get => _mouseWheelCycle;  set => Set(ref _mouseWheelCycle, value); }
    public string NavForwardKeys { get => _navForwardKeys;   set => Set(ref _navForwardKeys, value ?? ""); }
    public string NavBackKeys    { get => _navBackKeys;      set => Set(ref _navBackKeys, value ?? ""); }
    public bool HotkeyToggleMode
    {
        get => _hotkeyToggleMode;
        set { Set(ref _hotkeyToggleMode, value); EnforceSearchRequirement(); }
    }
    public string IgnoredApps    { get => _ignoredApps;      set => Set(ref _ignoredApps, value); }
    public string ActivationHotkey
    {
        get => _activationHotkey;
        set
        {
            Set(ref _activationHotkey, string.IsNullOrWhiteSpace(value) ? "Win+Tab" : value);
            EnforceSearchRequirement();
        }
    }

    /// <summary>
    /// Searching needs a cascade that survives the hotkey being released: a
    /// word cannot be typed while Win+Tab is held, and the instant the
    /// modifier goes up a classic binding commits — taking the rest of the
    /// word into Windows as shortcuts.  A binding with no modifier is
    /// inherently toggle in the core and already satisfies this.
    /// </summary>
    private bool ToggleSatisfied => _hotkeyToggleMode || !_activationHotkey.Contains('+');

    /// <summary>
    /// Keep the dependency true in the MODEL rather than on the page that
    /// happens to show it.  Enforcing it in the view meant turning Toggle
    /// activation off and pressing Apply still wrote searchEnabled: true —
    /// the correction only happened when someone later opened the Search
    /// page, and then needed a second Apply of its own.
    /// </summary>
    private void EnforceSearchRequirement()
    {
        if (_searchEnabled && !ToggleSatisfied)
            SearchEnabled = false;
    }

    // ---- Mouse & keyboard (Controls → Mouse & keyboard) ---------------------
    // Button ids match the core 1:1: 0 off, 1 left, 2 right, 3 middle,
    // 4 X1, 5 X2.
    //
    // The commit / cancel / close KEYS are lists, next to the navigation ones —
    // see "Keys in the cascade" below.
    private bool _pointerInCascade;            // [Core] pointerInCascade — master switch, default OFF
    private bool _mouseSelect = true;          // [Core] mouseSelect — hover + click to pick
    private int _mouseSelectButton = 1;        // [Core] mouseSelectButton
    private bool _mouseDragEnabled = true;     // [Core] mouseDragEnabled
    private int _mouseDragButton = 2;          // [Core] mouseDragButton — free drag (Window snap off)
    private bool _closeFromCascade = true;     // [Core] closeFromCascade — close CLICK only
    private int _mouseCloseButton = 3;         // [Core] mouseCloseButton

    public bool PointerInCascade
    {
        get => _pointerInCascade;
        set { Set(ref _pointerInCascade, value); RaiseWindowSnapSatisfied(); }
    }
    public bool MouseSelect       { get => _mouseSelect;       set => Set(ref _mouseSelect, value); }
    public int MouseSelectButton  { get => _mouseSelectButton; set => Set(ref _mouseSelectButton, Math.Clamp(value, 0, 5)); }
    public bool MouseDragEnabled  { get => _mouseDragEnabled;  set => Set(ref _mouseDragEnabled, value); }
    public int MouseDragButton    { get => _mouseDragButton;   set => Set(ref _mouseDragButton, Math.Clamp(value, 0, 5)); }
    public bool CloseFromCascade  { get => _closeFromCascade;  set => Set(ref _closeFromCascade, value); }
    public int MouseCloseButton   { get => _mouseCloseButton;  set => Set(ref _mouseCloseButton, Math.Clamp(value, 0, 5)); }

    /// <summary>
    /// Free stack movement (Window snap OFF) is a POINTER feature: the whole
    /// of it is "hold the drag button and move", and that button lives behind
    /// the pointer master.  Asking for one without the other is a combination
    /// that cannot do anything, so it is a state the settings may be IN but
    /// never a state they can be SAVED in — MainWindow takes Apply away while
    /// it holds, leaving Revert, and the General page says why.
    ///
    /// Deliberately not a forced correction (which is how Search handles its
    /// own dependency).  Silently flipping Window snap back on the moment the
    /// pointer master went off would undo a choice the user is in the middle
    /// of making, with the switch under their hand moving by itself.  Blocking
    /// the save says the same thing without touching their setting: nothing
    /// invalid reaches config.json either way.
    ///
    /// Lives in the MODEL rather than on the General page because Apply is
    /// global — the combination has to be visible from wherever it was made,
    /// not only from the page that happens to show one half of it.
    /// </summary>
    public bool WindowSnapSatisfied => _windowSnap || _pointerInCascade;

    private void RaiseWindowSnapSatisfied() =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(WindowSnapSatisfied)));

    // ---- Search (type-to-filter) --------------------------------------------
    private bool _searchEnabled;               // [Core] searchEnabled — default off
    private bool _searchBox = true;            // [Core] searchBox — draw the field
    private bool _searchMatchProcess = true;   // [Core] searchMatchProcess — match exe names too
    // Placement on the cascade host as PERCENTAGES of the primary monitor —
    // X is the field's centre, Y its bottom edge — so one setting reads the
    // same on every resolution.
    private int _searchPosX = 50;              // [Core] searchPosX
    private int _searchPosY = 94;              // [Core] searchPosY
    private int _searchScale = 100;            // [Core] searchScale — 50..200 %

    // Never true without Toggle activation — the other half of the same
    // invariant, so a stale config or a stray assignment cannot bring the
    // feature back on in a session where it would eat the user's keystrokes.
    public bool SearchEnabled      { get => _searchEnabled;      set => Set(ref _searchEnabled, value && ToggleSatisfied); }
    public bool SearchBox          { get => _searchBox;          set => Set(ref _searchBox, value); }
    public bool SearchMatchProcess { get => _searchMatchProcess; set => Set(ref _searchMatchProcess, value); }
    public int SearchPosX          { get => _searchPosX;         set => Set(ref _searchPosX, Math.Clamp(value, 0, 100)); }
    public int SearchPosY          { get => _searchPosY;         set => Set(ref _searchPosY, Math.Clamp(value, 0, 100)); }
    public int SearchScale         { get => _searchScale;        set => Set(ref _searchScale, Math.Clamp(value, 50, 200)); }

    // ---- Touchpad (precision touchpad gestures) -----------------------------
    // The three gesture settings are LISTS in the same '!'-parking, ';'-separated
    // form the key bindings use (see ParseBindings); the token vocabulary lives
    // in TouchpadGestures. Empty means the action has no gesture at all, which
    // is what the old single-value "Off" said.
    private bool _touchpadNav = true;          // [Core] touchpadNav — master switch
    private string _touchpadActivateGestures = DefaultTouchpadActivateGestures;  // [Core]
    private string _touchpadCycleGestures = DefaultTouchpadCycleGestures;        // [Core]
    private string _touchpadCommitGestures = DefaultTouchpadCommitGestures;      // [Core]
    private bool _touchpadReverse;             // [Core] invert the cycle direction
    private int _touchpadSensitivity = 50;     // [Core] 1..100, swipe distance per step
    private int _touchpadSmoothing = 35;       // [Core] 0..100 jitter filter
    private bool _touchpadCancelSwipe = true;  // [Core] reverse activation swipe cancels
    private bool _touchpadContinuous;          // [Core] several gestures out of one touch
    private bool _windowSnap = true;           // [Core] windowSnap — free drag vs whole-window steps

    public bool TouchpadNav             { get => _touchpadNav;             set => Set(ref _touchpadNav, value); }
    public string TouchpadActivateGestures { get => _touchpadActivateGestures; set => Set(ref _touchpadActivateGestures, value ?? ""); }
    public string TouchpadCycleGestures    { get => _touchpadCycleGestures;    set => Set(ref _touchpadCycleGestures, value ?? ""); }
    public string TouchpadCommitGestures   { get => _touchpadCommitGestures;   set => Set(ref _touchpadCommitGestures, value ?? ""); }
    public bool TouchpadReverse         { get => _touchpadReverse;         set => Set(ref _touchpadReverse, value); }
    public int TouchpadSensitivity      { get => _touchpadSensitivity;     set => Set(ref _touchpadSensitivity, Math.Clamp(value, 1, 100)); }
    public int TouchpadSmoothing        { get => _touchpadSmoothing;       set => Set(ref _touchpadSmoothing, Math.Clamp(value, 0, 100)); }
    public bool TouchpadCancelSwipe     { get => _touchpadCancelSwipe;     set => Set(ref _touchpadCancelSwipe, value); }
    public bool TouchpadContinuous      { get => _touchpadContinuous;      set => Set(ref _touchpadContinuous, value); }

    /// <summary>Shipped gestures — keep in step with core/Config.h.</summary>
    public const string DefaultTouchpadActivateGestures = "TwoDownRight";
    public const string DefaultTouchpadCycleGestures = "TwoSwipe";
    public const string DefaultTouchpadCommitGestures = "OneTap";

    public List<Binding> TouchpadActivateGestureList => ParseBindings(_touchpadActivateGestures);
    public List<Binding> TouchpadCycleGestureList    => ParseBindings(_touchpadCycleGestures);
    public List<Binding> TouchpadCommitGestureList   => ParseBindings(_touchpadCommitGestures);

    public void SetTouchpadActivateGestureList(IEnumerable<Binding> g) => TouchpadActivateGestures = FormatBindings(g);
    public void SetTouchpadCycleGestureList(IEnumerable<Binding> g)    => TouchpadCycleGestures = FormatBindings(g);
    public void SetTouchpadCommitGestureList(IEnumerable<Binding> g)   => TouchpadCommitGestures = FormatBindings(g);
    // OFF needs the pointer master — see WindowSnapSatisfied, which is what
    // Apply consults.  The value itself is left exactly as set: the block is
    // on saving it, not on choosing it.
    public bool WindowSnap
    {
        get => _windowSnap;
        set { Set(ref _windowSnap, value); RaiseWindowSnapSatisfied(); }
    }

    // ---- Cascade Keybindings (Mouse & keyboard → Cascade Keybindings) -------
    // Five lists: the two navigation directions and the three bindings that used
    // to be one key apiece (commit, cancel, close). The stored form is one
    // string per list so the whole model stays snapshot/compare-able the way
    // every other setting is; the page works in Binding objects and hands the
    // string back.

    /// <summary>Matches KeyboardHook::kMaxBindingKeys — what one packed word holds.</summary>
    public const int MaxBindingKeys = 5;

    /// <summary>Shipped bindings — keep in step with core/Config.h.</summary>
    public const string DefaultNavForwardKeys = "Tab;Down;Right";
    public const string DefaultNavBackKeys = "Shift+Tab;Up;Left";
    public const string DefaultCommitKeys = "Enter";
    public const string DefaultCancelKeys = "Escape";
    public const string DefaultCloseKeys = "Delete";

    private string _commitKeys = DefaultCommitKeys;   // [Core] commitKeys
    private string _cancelKeys = DefaultCancelKeys;   // [Core] cancelKeys
    private string _closeKeys = DefaultCloseKeys;     // [Core] closeKeys — empty = no close key

    public string CommitKeys { get => _commitKeys; set => Set(ref _commitKeys, value ?? ""); }
    public string CancelKeys { get => _cancelKeys; set => Set(ref _cancelKeys, value ?? ""); }
    public string CloseKeys  { get => _closeKeys;  set => Set(ref _closeKeys, value ?? ""); }

    // ---- One press, one job -------------------------------------------------

    /// <summary>
    /// Is this binding the very press that opens the cascade?
    ///
    /// EXACT, and nothing looser: same key or button, same modifiers. Win+Tab
    /// and a bare Tab are two different presses, and the shipped configuration
    /// is precisely that pair, so sharing a key cannot be the bar. Nor can
    /// sharing a modifier, which only makes a binding awkward, never doubled.
    ///
    /// No list is exempt, navigation included. One press does one job, and the
    /// press that opens the cascade already has its job.
    /// </summary>
    public bool ClashesWithActivation(string? token)
    {
        uint vk = HotkeyService.MainBindingVk(token);
        return vk != 0
            && HotkeyService.MainBindingVk(_activationHotkey) == vk
            && HotkeyService.ModsOf(_activationHotkey) == HotkeyService.ModsOf(token);
    }

    /// <summary>
    /// The cascade action bound to the activation hotkey itself, or null.
    ///
    /// Both pickers refuse this combination, and it is still reachable: pointing
    /// the hotkey at a key a list already holds is one move, and rebinding the
    /// hotkey carries the navigation entries onto its new key besides. So the
    /// last word is here, where Apply can see it however it was arrived at.
    ///
    /// Blocking the save rather than correcting anything, for the reason
    /// WindowSnapSatisfied gives: the bindings belong to the user, and Revert is
    /// already the way back.
    /// </summary>
    public string? ActivationBindingClash
    {
        get
        {
            if (Holds(_navForwardKeys)) return "Next window";
            if (Holds(_navBackKeys))    return "Previous window";
            if (Holds(_commitKeys))     return "Confirm";
            if (Holds(_cancelKeys))     return "Cancel";
            if (Holds(_closeKeys))      return "Close window";
            return null;

            bool Holds(string list) =>
                ParseBindings(list).Any(b => ClashesWithActivation(b.Token));
        }
    }

    public List<Binding> NavForwardKeyList => ParseBindings(_navForwardKeys);
    public List<Binding> NavBackKeyList    => ParseBindings(_navBackKeys);
    public List<Binding> CommitKeyList     => ParseBindings(_commitKeys);
    public List<Binding> CancelKeyList     => ParseBindings(_cancelKeys);
    public List<Binding> CloseKeyList      => ParseBindings(_closeKeys);

    public void SetNavForwardKeyList(IEnumerable<Binding> keys) => NavForwardKeys = FormatBindings(keys);
    public void SetNavBackKeyList(IEnumerable<Binding> keys)    => NavBackKeys = FormatBindings(keys);
    public void SetCommitKeyList(IEnumerable<Binding> keys)     => CommitKeys = FormatBindings(keys);
    public void SetCancelKeyList(IEnumerable<Binding> keys)     => CancelKeys = FormatBindings(keys);
    public void SetCloseKeyList(IEnumerable<Binding> keys)      => CloseKeys = FormatBindings(keys);

    /// <summary>
    /// ';'-separated tokens → bindings. A leading '!' is the "kept but off"
    /// mark. Blank entries are dropped rather than becoming empty bindings, so
    /// a hand-edited trailing ';' costs nothing. Shared by the key lists and the
    /// touchpad gesture lists — the file format is the same one twice.
    /// </summary>
    public static List<Binding> ParseBindings(string? list)
    {
        var result = new List<Binding>();
        if (string.IsNullOrWhiteSpace(list)) return result;

        foreach (string raw in list.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            bool enabled = !raw.StartsWith('!');
            string token = (enabled ? raw : raw[1..]).Trim();
            if (token.Length == 0) continue;
            // The same entry twice would only ever fire once; keep the first.
            if (result.Any(k => string.Equals(k.Token, token, StringComparison.OrdinalIgnoreCase)))
                continue;
            result.Add(new Binding(token, enabled));
        }
        return result;
    }

    /// <summary>
    /// Follow a change of activation hotkey through the navigation lists:
    /// wherever the OLD hotkey's key was an entry, it becomes the new one,
    /// keeping its direction, its Shift qualifier and whether it was switched
    /// on.
    ///
    /// The hotkey's key is an ordinary entry here — that is what makes Tab
    /// removable — and the price of that is a hotkey rebound to a key nobody
    /// ever put on a list, which would open the cascade and then not step it.
    /// Carrying the entry across is the quiet fix: rebinding Win+Tab to
    /// Ctrl+Alt+F leaves F stepping the stack exactly as Tab did, and someone
    /// who had already REMOVED Tab has nothing to carry, so nothing comes back.
    ///
    /// A hotkey with no modifiers is the exception, and the body says why.
    ///
    /// Called when the user assigns a hotkey, never while loading: on load the
    /// lists in the file are the truth.
    /// </summary>
    public void RepointNavKeysToHotkey(string? oldCombo, string? newCombo)
    {
        // A hotkey with NO modifiers would be repointed onto as a bare token,
        // which is the hotkey itself — the one binding no list may hold (see
        // ClashesWithActivation).  Nothing to point at, then: the lists keep the
        // keys they already name, all of which still step.
        if (HotkeyService.ModsOf(newCombo) == 0) return;

        string oldKey = MainToken(oldCombo);
        string newKey = MainToken(newCombo);
        if (oldKey.Length == 0 || newKey.Length == 0
            || string.Equals(oldKey, newKey, StringComparison.OrdinalIgnoreCase))
            return;

        NavForwardKeys = FormatBindings(Repoint(ParseBindings(_navForwardKeys)));
        NavBackKeys    = FormatBindings(Repoint(ParseBindings(_navBackKeys)));

        List<Binding> Repoint(List<Binding> keys) => keys.ConvertAll(k =>
        {
            string[] parts = k.Token.Split('+', StringSplitOptions.TrimEntries);
            if (!string.Equals(parts[^1], oldKey, StringComparison.OrdinalIgnoreCase))
                return k;
            parts[^1] = newKey;
            return k with { Token = string.Join('+', parts) };
        });

        static string MainToken(string? combo) =>
            string.IsNullOrWhiteSpace(combo)
                ? ""
                : combo.Split('+', StringSplitOptions.TrimEntries)[^1];
    }

    public static string FormatBindings(IEnumerable<Binding> keys) =>
        string.Join(';', keys.Where(k => !string.IsNullOrWhiteSpace(k.Token))
                             .Select(k => (k.Enabled ? "" : "!") + k.Token.Trim()));

    /// <summary>Split helper for the ignored-apps editor page.</summary>
    public List<string> IgnoredAppsList =>
        _ignoredApps.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList();

    public void SetIgnoredAppsList(IEnumerable<string> entries) =>
        IgnoredApps = string.Join(';', entries.Where(e => !string.IsNullOrWhiteSpace(e)));

    // ---- Performance / misc (consumed flags only where the core reads them) -
    private uint _maxWindows = 10;         // [Core] maxWindows (2-10)
    private bool _autoPerfTune = true;     // [ComingSoon] autoPerfTune (key exists, core ignores it)
    private int _perfProfile = -1;         // [ComingSoon] perfProfile (key exists, core ignores it)
    private int _startDelayMs = 16;        // [Core] startDelayMs — WGC warm-up budget (1-1000 ms)
    private bool _showDebugInfo;           // [Core] showDebugInfo

    public uint MaxWindows    { get => _maxWindows;    set => Set(ref _maxWindows, Math.Clamp(value, 2u, 10u)); }
    public bool AutoPerfTune  { get => _autoPerfTune;  set => Set(ref _autoPerfTune, value); }
    public int PerfProfile    { get => _perfProfile;   set => Set(ref _perfProfile, Math.Clamp(value, -1, 2)); }
    public int StartDelayMs   { get => _startDelayMs;  set => Set(ref _startDelayMs, Math.Clamp(value, 1, 1000)); }
    public bool ShowDebugInfo { get => _showDebugInfo; set => Set(ref _showDebugInfo, value); }

    // ---- Dirty tracking -----------------------------------------------------

    public bool IsDirty => _snapshot != null && !ValuesEqual(_snapshot);

    /// <summary>Remember the current state as "saved".</summary>
    public void TakeSnapshot()
    {
        _snapshot = (SettingsModel)MemberwiseClone();
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsDirty)));
        DirtyChanged?.Invoke();
    }

    /// <summary>Restore every value from the last saved snapshot.</summary>
    public void RevertToSnapshot()
    {
        if (_snapshot == null) return;
        _suppressDirty = true;
        CopyFrom(_snapshot);
        _suppressDirty = false;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsDirty)));
        DirtyChanged?.Invoke();
    }

    public void ResetToDefaults()
    {
        _suppressDirty = true;
        CopyFrom(new SettingsModel());
        _suppressDirty = false;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsDirty)));
        DirtyChanged?.Invoke();
    }

    private void CopyFrom(SettingsModel s)
    {
        StartWithWindows = s.StartWithWindows;
        AppTheme = s.AppTheme;
        VisualPreset = s.VisualPreset;
        Reflections = s.Reflections;
        BackgroundOpacity = s.BackgroundOpacity;
        BackgroundBlur = s.BackgroundBlur;
        Antialiasing = s.Antialiasing;
        MotionBlur = s.MotionBlur;
        Animations = s.Animations;
        AnimEntryExit = s.AnimEntryExit;
        AnimCycle = s.AnimCycle;
        AnimClose = s.AnimClose;
        AnimLabel = s.AnimLabel;
        AnimHover = s.AnimHover;
        LivePreview = s.LivePreview;
        LiveBackground = s.LiveBackground;
        VsyncLivePreview = s.VsyncLivePreview;
        TaskbarLivePreview = s.TaskbarLivePreview;
        TaskbarPreview = s.TaskbarPreview;
        ShowDesktopTile = s.ShowDesktopTile;
        SelectedLabel = s.SelectedLabel;
        SelectedLabelTitle = s.SelectedLabelTitle;
        SelectedLabelIcon = s.SelectedLabelIcon;
        SelectedLabelBox = s.SelectedLabelBox;
        CascadeMonitor = s.CascadeMonitor;
        SecondaryTaskbarMode = s.SecondaryTaskbarMode;
        IgnoreFullscreen = s.IgnoreFullscreen;
        MouseWheelCycle = s.MouseWheelCycle;
        NavForwardKeys = s.NavForwardKeys;
        NavBackKeys = s.NavBackKeys;
        HotkeyToggleMode = s.HotkeyToggleMode;
        IgnoredApps = s.IgnoredApps;
        ExcludedApps = s.ExcludedApps;
        ActivationHotkey = s.ActivationHotkey;
        CommitKeys = s.CommitKeys;
        CancelKeys = s.CancelKeys;
        CloseKeys = s.CloseKeys;
        PointerInCascade = s.PointerInCascade;
        MouseSelect = s.MouseSelect;
        MouseSelectButton = s.MouseSelectButton;
        MouseDragEnabled = s.MouseDragEnabled;
        MouseDragButton = s.MouseDragButton;
        CloseFromCascade = s.CloseFromCascade;
        MouseCloseButton = s.MouseCloseButton;
        SearchEnabled = s.SearchEnabled;
        SearchBox = s.SearchBox;
        SearchMatchProcess = s.SearchMatchProcess;
        SearchPosX = s.SearchPosX;
        SearchPosY = s.SearchPosY;
        SearchScale = s.SearchScale;
        TouchpadNav = s.TouchpadNav;
        TouchpadActivateGestures = s.TouchpadActivateGestures;
        TouchpadCycleGestures = s.TouchpadCycleGestures;
        TouchpadCommitGestures = s.TouchpadCommitGestures;
        TouchpadReverse = s.TouchpadReverse;
        TouchpadSensitivity = s.TouchpadSensitivity;
        TouchpadSmoothing = s.TouchpadSmoothing;
        TouchpadCancelSwipe = s.TouchpadCancelSwipe;
        TouchpadContinuous = s.TouchpadContinuous;
        WindowSnap = s.WindowSnap;
        MaxWindows = s.MaxWindows;
        AutoPerfTune = s.AutoPerfTune;
        PerfProfile = s.PerfProfile;
        StartDelayMs = s.StartDelayMs;
        ShowDebugInfo = s.ShowDebugInfo;
    }

    private bool ValuesEqual(SettingsModel s) =>
        StartWithWindows == s.StartWithWindows &&
        AppTheme == s.AppTheme &&
        VisualPreset == s.VisualPreset &&
        Reflections == s.Reflections &&
        BackgroundOpacity == s.BackgroundOpacity &&
        BackgroundBlur == s.BackgroundBlur &&
        Antialiasing == s.Antialiasing &&
        MotionBlur == s.MotionBlur &&
        Animations == s.Animations &&
        AnimEntryExit == s.AnimEntryExit &&
        AnimCycle == s.AnimCycle &&
        AnimClose == s.AnimClose &&
        AnimLabel == s.AnimLabel &&
        AnimHover == s.AnimHover &&
        LivePreview == s.LivePreview &&
        LiveBackground == s.LiveBackground &&
        VsyncLivePreview == s.VsyncLivePreview &&
        TaskbarLivePreview == s.TaskbarLivePreview &&
        TaskbarPreview == s.TaskbarPreview &&
        ShowDesktopTile == s.ShowDesktopTile &&
        SelectedLabel == s.SelectedLabel &&
        SelectedLabelTitle == s.SelectedLabelTitle &&
        SelectedLabelIcon == s.SelectedLabelIcon &&
        SelectedLabelBox == s.SelectedLabelBox &&
        CascadeMonitor == s.CascadeMonitor &&
        SecondaryTaskbarMode == s.SecondaryTaskbarMode &&
        IgnoreFullscreen == s.IgnoreFullscreen &&
        MouseWheelCycle == s.MouseWheelCycle &&
        NavForwardKeys == s.NavForwardKeys &&
        NavBackKeys == s.NavBackKeys &&
        HotkeyToggleMode == s.HotkeyToggleMode &&
        IgnoredApps == s.IgnoredApps &&
        ExcludedApps == s.ExcludedApps &&
        ActivationHotkey == s.ActivationHotkey &&
        CommitKeys == s.CommitKeys &&
        CancelKeys == s.CancelKeys &&
        CloseKeys == s.CloseKeys &&
        PointerInCascade == s.PointerInCascade &&
        MouseSelect == s.MouseSelect &&
        MouseSelectButton == s.MouseSelectButton &&
        MouseDragEnabled == s.MouseDragEnabled &&
        MouseDragButton == s.MouseDragButton &&
        CloseFromCascade == s.CloseFromCascade &&
        MouseCloseButton == s.MouseCloseButton &&
        SearchEnabled == s.SearchEnabled &&
        SearchBox == s.SearchBox &&
        SearchMatchProcess == s.SearchMatchProcess &&
        SearchPosX == s.SearchPosX &&
        SearchPosY == s.SearchPosY &&
        SearchScale == s.SearchScale &&
        TouchpadNav == s.TouchpadNav &&
        TouchpadActivateGestures == s.TouchpadActivateGestures &&
        TouchpadCycleGestures == s.TouchpadCycleGestures &&
        TouchpadCommitGestures == s.TouchpadCommitGestures &&
        TouchpadReverse == s.TouchpadReverse &&
        TouchpadSensitivity == s.TouchpadSensitivity &&
        TouchpadSmoothing == s.TouchpadSmoothing &&
        TouchpadCancelSwipe == s.TouchpadCancelSwipe &&
        TouchpadContinuous == s.TouchpadContinuous &&
        WindowSnap == s.WindowSnap &&
        MaxWindows == s.MaxWindows &&
        AutoPerfTune == s.AutoPerfTune &&
        PerfProfile == s.PerfProfile &&
        StartDelayMs == s.StartDelayMs &&
        ShowDebugInfo == s.ShowDebugInfo;
}

/// <summary>
/// One entry of a binding list — a key that acts on the open cascade (Controls
/// → Mouse &amp; keyboard → Cascade Keybindings) or a touchpad gesture (Controls
/// → Touchpad gestures). <paramref name="Token"/> is the vocabulary the core
/// parses ("Down", "PageUp", "F13", "Ctrl+W", "MButton"; "TwoDownRight",
/// "OneTap");
/// <paramref name="Enabled"/> false is a binding the user parked rather than
/// removed, so switching it back on does not mean recording it again.
/// </summary>
/// <param name="Token">Key name in KeyboardHook::ParseHotkey's vocabulary, or a
/// TouchpadHook gesture token.</param>
/// <param name="Enabled">False = remembered in config.json, ignored by the core.</param>
public sealed record Binding(string Token, bool Enabled);
