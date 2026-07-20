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

    public bool StartWithWindows { get => _startWithWindows; set => Set(ref _startWithWindows, value); }

    // ---- Appearance --------------------------------------------------------
    private int _appTheme;                 // Settings-app theme (ThemeService.Themes index)
    private int _visualPreset;             // [ComingSoon] 0 = Cascade
    private int _backgroundOpacity = 28;   // [Core] 0..100 %; 28 == original kBgAlpha look
    private int _backgroundBlur;           // [ComingSoon] 0..100
    private bool _antialiasing = true;     // [Core] antialiasing
    private bool _motionBlur = true;       // [Core] motionBlur
    private bool _animations = true;       // [Core] animations — master switch
    private bool _animEntryExit = true;    // [Core] animEntryExit — enter/exit morph
    private bool _animCycle = true;        // [Core] animCycle — Tab/Shift-Tab cycling
    private bool _animClose = true;        // [Core] animClose — window-close reflow
    private bool _livePreview = true;      // [Core] livePreview — live WGC thumbnails
    private bool _vsyncLivePreview;        // [Core] vsyncLivePreview — Present(1) pacing
    private bool _taskbarLivePreview;      // [Core] taskbarLivePreview — live Shell_TrayWnd capture
    private bool _taskbarPreview = true;   // [Core] taskbarPreview — draw a taskbar preview at all
    private bool _showDesktopTile = true;  // [Core] showDesktopTile — desktop pseudo-window in the cascade
    private bool _selectedLabel;           // [Core] selectedLabel — front-slot label master switch (default off)
    private bool _selectedLabelTitle = true; // [Core] selectedLabelTitle — window title part
    private bool _selectedLabelIcon = true;  // [Core] selectedLabelIcon — program icon part
    private bool _selectedLabelBox = true;   // [Core] selectedLabelBox — aero-glass plate behind the label

    public int AppTheme          { get => _appTheme;          set => Set(ref _appTheme, Math.Clamp(value, 0, 4)); }
    public int VisualPreset      { get => _visualPreset;      set => Set(ref _visualPreset, value); }
    public int BackgroundOpacity { get => _backgroundOpacity; set => Set(ref _backgroundOpacity, Math.Clamp(value, 0, 100)); }
    public int BackgroundBlur    { get => _backgroundBlur;    set => Set(ref _backgroundBlur, Math.Clamp(value, 0, 100)); }
    public bool Antialiasing     { get => _antialiasing;      set => Set(ref _antialiasing, value); }
    public bool MotionBlur       { get => _motionBlur;        set => Set(ref _motionBlur, value); }
    public bool Animations       { get => _animations;        set => Set(ref _animations, value); }
    public bool AnimEntryExit    { get => _animEntryExit;     set => Set(ref _animEntryExit, value); }
    public bool AnimCycle        { get => _animCycle;         set => Set(ref _animCycle, value); }
    public bool AnimClose        { get => _animClose;         set => Set(ref _animClose, value); }
    public bool LivePreview      { get => _livePreview;       set => Set(ref _livePreview, value); }
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
    private string _activationHotkey = "Win+Tab"; // [Core] activationHotkey — see HotkeyService

    public bool IgnoreFullscreen { get => _ignoreFullscreen; set => Set(ref _ignoreFullscreen, value); }
    public bool MouseWheelCycle  { get => _mouseWheelCycle;  set => Set(ref _mouseWheelCycle, value); }
    public bool KeyboardNav      { get => _keyboardNav;      set => Set(ref _keyboardNav, value); }
    public string IgnoredApps    { get => _ignoredApps;      set => Set(ref _ignoredApps, value); }
    public string ActivationHotkey
    {
        get => _activationHotkey;
        set => Set(ref _activationHotkey, string.IsNullOrWhiteSpace(value) ? "Win+Tab" : value);
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
        BackgroundOpacity = s.BackgroundOpacity;
        BackgroundBlur = s.BackgroundBlur;
        Antialiasing = s.Antialiasing;
        MotionBlur = s.MotionBlur;
        Animations = s.Animations;
        AnimEntryExit = s.AnimEntryExit;
        AnimCycle = s.AnimCycle;
        AnimClose = s.AnimClose;
        LivePreview = s.LivePreview;
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
        IgnoredApps = s.IgnoredApps;
        ActivationHotkey = s.ActivationHotkey;
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
        BackgroundOpacity == s.BackgroundOpacity &&
        BackgroundBlur == s.BackgroundBlur &&
        Antialiasing == s.Antialiasing &&
        MotionBlur == s.MotionBlur &&
        Animations == s.Animations &&
        AnimEntryExit == s.AnimEntryExit &&
        AnimCycle == s.AnimCycle &&
        AnimClose == s.AnimClose &&
        LivePreview == s.LivePreview &&
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
        IgnoredApps == s.IgnoredApps &&
        ActivationHotkey == s.ActivationHotkey &&
        MaxWindows == s.MaxWindows &&
        AutoPerfTune == s.AutoPerfTune &&
        PerfProfile == s.PerfProfile &&
        StartDelayMs == s.StartDelayMs &&
        ShowDebugInfo == s.ShowDebugInfo;
}
