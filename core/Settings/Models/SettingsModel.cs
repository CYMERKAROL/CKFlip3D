using System.ComponentModel;
using System.Runtime.CompilerServices;

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
    private bool _startWithWindows;        // mirrors the HKCU Run entry
    private string _excludedApps = "";     // [Core] excludedApps — ';'-separated exe paths/names

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
    private bool _keyboardNav = true;      // [Core] keyboardNav
    private string _ignoredApps = "";      // [Core] ignoredApps — ';'-separated exe paths/names
    private bool _hotkeyToggleMode;        // [Core] hotkeyToggleMode — combo bindings toggle instead of hold
    private string _activationHotkey = "Win+Tab"; // [Core] activationHotkey — see HotkeyService

    public bool IgnoreFullscreen { get => _ignoreFullscreen; set => Set(ref _ignoreFullscreen, value); }
    public bool MouseWheelCycle  { get => _mouseWheelCycle;  set => Set(ref _mouseWheelCycle, value); }
    public bool KeyboardNav      { get => _keyboardNav;      set => Set(ref _keyboardNav, value); }
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
    private string _commitHotkey = "Enter";    // [Core] commitHotkey
    private string _cancelHotkey = "Escape";   // [Core] cancelHotkey
    private string _closeHotkey = "Delete";    // [Core] closeHotkey
    private bool _pointerInCascade;            // [Core] pointerInCascade — master switch, default OFF
    private bool _mouseSelect = true;          // [Core] mouseSelect — hover + click to pick
    private int _mouseSelectButton = 1;        // [Core] mouseSelectButton
    private bool _mouseDragEnabled = true;     // [Core] mouseDragEnabled
    private int _mouseDragButton = 2;          // [Core] mouseDragButton — free drag (Window snap off)
    private bool _closeFromCascade = true;     // [Core] closeFromCascade — close CLICK only
    private int _mouseCloseButton = 3;         // [Core] mouseCloseButton
    private bool _closeKeyEnabled = true;      // [Core] closeKeyEnabled — close KEY, own switch

    public string CommitHotkey
    {
        get => _commitHotkey;
        set => Set(ref _commitHotkey, string.IsNullOrWhiteSpace(value) ? "Enter" : value);
    }
    public string CancelHotkey
    {
        get => _cancelHotkey;
        set => Set(ref _cancelHotkey, string.IsNullOrWhiteSpace(value) ? "Escape" : value);
    }
    public string CloseHotkey
    {
        get => _closeHotkey;
        set => Set(ref _closeHotkey, string.IsNullOrWhiteSpace(value) ? "Delete" : value);
    }
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
    public bool CloseKeyEnabled   { get => _closeKeyEnabled;   set => Set(ref _closeKeyEnabled, value); }

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
    private bool _touchpadNav = true;          // [Core] touchpadNav — master switch
    private int _touchpadCycleFingers = 2;     // [Core] 2 or 4 fingers, left/right = cycle
    private bool _touchpadReverse;             // [Core] invert the cycle direction
    private int _touchpadSensitivity = 50;     // [Core] 1..100, swipe distance per step
    private int _touchpadActivateGesture = 1;  // [Core] 0=off,1=2↘,2=2↙,3=4↘,4=4↙
    private int _touchpadCommitGesture = 1;    // [Core] 0=off,1=1-tap,2=2-tap,3=2-down
    private int _touchpadSmoothing = 35;       // [Core] 0..100 jitter filter
    private bool _touchpadCancelSwipe = true;  // [Core] reverse activation swipe cancels
    private bool _windowSnap = true;           // [Core] windowSnap — free drag vs whole-window steps

    public bool TouchpadNav             { get => _touchpadNav;             set => Set(ref _touchpadNav, value); }
    // Two or four fingers only — three is Windows' own (Alt+Tab / Task View),
    // so a config written by an older build folds onto the nearest survivor.
    public int TouchpadCycleFingers     { get => _touchpadCycleFingers;    set => Set(ref _touchpadCycleFingers, value >= 4 ? 4 : 2); }
    public bool TouchpadReverse         { get => _touchpadReverse;         set => Set(ref _touchpadReverse, value); }
    public int TouchpadSensitivity      { get => _touchpadSensitivity;     set => Set(ref _touchpadSensitivity, Math.Clamp(value, 1, 100)); }
    public int TouchpadActivateGesture  { get => _touchpadActivateGesture; set => Set(ref _touchpadActivateGesture, Math.Clamp(value, 0, 4)); }
    public int TouchpadCommitGesture    { get => _touchpadCommitGesture;   set => Set(ref _touchpadCommitGesture, Math.Clamp(value, 0, 3)); }
    public int TouchpadSmoothing        { get => _touchpadSmoothing;       set => Set(ref _touchpadSmoothing, Math.Clamp(value, 0, 100)); }
    public bool TouchpadCancelSwipe     { get => _touchpadCancelSwipe;     set => Set(ref _touchpadCancelSwipe, value); }
    // OFF needs the pointer master — see WindowSnapSatisfied, which is what
    // Apply consults.  The value itself is left exactly as set: the block is
    // on saving it, not on choosing it.
    public bool WindowSnap
    {
        get => _windowSnap;
        set { Set(ref _windowSnap, value); RaiseWindowSnapSatisfied(); }
    }

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
        KeyboardNav = s.KeyboardNav;
        HotkeyToggleMode = s.HotkeyToggleMode;
        IgnoredApps = s.IgnoredApps;
        ExcludedApps = s.ExcludedApps;
        ActivationHotkey = s.ActivationHotkey;
        CommitHotkey = s.CommitHotkey;
        CancelHotkey = s.CancelHotkey;
        CloseHotkey = s.CloseHotkey;
        PointerInCascade = s.PointerInCascade;
        MouseSelect = s.MouseSelect;
        MouseSelectButton = s.MouseSelectButton;
        MouseDragEnabled = s.MouseDragEnabled;
        MouseDragButton = s.MouseDragButton;
        CloseFromCascade = s.CloseFromCascade;
        CloseKeyEnabled = s.CloseKeyEnabled;
        MouseCloseButton = s.MouseCloseButton;
        SearchEnabled = s.SearchEnabled;
        SearchBox = s.SearchBox;
        SearchMatchProcess = s.SearchMatchProcess;
        SearchPosX = s.SearchPosX;
        SearchPosY = s.SearchPosY;
        SearchScale = s.SearchScale;
        TouchpadNav = s.TouchpadNav;
        TouchpadCycleFingers = s.TouchpadCycleFingers;
        TouchpadReverse = s.TouchpadReverse;
        TouchpadSensitivity = s.TouchpadSensitivity;
        TouchpadActivateGesture = s.TouchpadActivateGesture;
        TouchpadCommitGesture = s.TouchpadCommitGesture;
        TouchpadSmoothing = s.TouchpadSmoothing;
        TouchpadCancelSwipe = s.TouchpadCancelSwipe;
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
        KeyboardNav == s.KeyboardNav &&
        HotkeyToggleMode == s.HotkeyToggleMode &&
        IgnoredApps == s.IgnoredApps &&
        ExcludedApps == s.ExcludedApps &&
        ActivationHotkey == s.ActivationHotkey &&
        CommitHotkey == s.CommitHotkey &&
        CancelHotkey == s.CancelHotkey &&
        CloseHotkey == s.CloseHotkey &&
        PointerInCascade == s.PointerInCascade &&
        MouseSelect == s.MouseSelect &&
        MouseSelectButton == s.MouseSelectButton &&
        MouseDragEnabled == s.MouseDragEnabled &&
        MouseDragButton == s.MouseDragButton &&
        CloseFromCascade == s.CloseFromCascade &&
        CloseKeyEnabled == s.CloseKeyEnabled &&
        MouseCloseButton == s.MouseCloseButton &&
        SearchEnabled == s.SearchEnabled &&
        SearchBox == s.SearchBox &&
        SearchMatchProcess == s.SearchMatchProcess &&
        SearchPosX == s.SearchPosX &&
        SearchPosY == s.SearchPosY &&
        SearchScale == s.SearchScale &&
        TouchpadNav == s.TouchpadNav &&
        TouchpadCycleFingers == s.TouchpadCycleFingers &&
        TouchpadReverse == s.TouchpadReverse &&
        TouchpadSensitivity == s.TouchpadSensitivity &&
        TouchpadActivateGesture == s.TouchpadActivateGesture &&
        TouchpadCommitGesture == s.TouchpadCommitGesture &&
        TouchpadSmoothing == s.TouchpadSmoothing &&
        TouchpadCancelSwipe == s.TouchpadCancelSwipe &&
        WindowSnap == s.WindowSnap &&
        MaxWindows == s.MaxWindows &&
        AutoPerfTune == s.AutoPerfTune &&
        PerfProfile == s.PerfProfile &&
        StartDelayMs == s.StartDelayMs &&
        ShowDebugInfo == s.ShowDebugInfo;
}
