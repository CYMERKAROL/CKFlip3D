// ---------------------------------------------------------------------------
// Every setting the Settings app can write and the core has to read, in one
// struct.  The defaults here are what a fresh install behaves like, so they
// are also the answer whenever the config file is missing or unreadable.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdint>
#include <string>

/// Runtime configuration — loaded from %APPDATA%\CKFlip3D\config.json.
/// Default values are the "high quality" defaults.
struct AppConfig {
    // --- Quality settings ---
    bool     antialiasing     = true;     // Anisotropic tile filtering (false = point)
    bool     animations       = true;     // Master switch: every animation (false = instant snap)
    // Per-animation selection under the master switch — an animation plays
    // only when `animations` AND its own flag are both true.  Lets the user
    // keep e.g. cycling animated while entry/exit and close snap instantly.
    bool     animEntryExit    = true;     // enter/exit morph (flat ↔ cascade)
    bool     animCycle        = true;     // Tab/Shift-Tab cycle rotation
    bool     animClose        = true;     // window-closed-mid-cascade reflow
    // Selected-window label motion: the smooth glide between differently
    // sized front tiles and the fade out/in on held-key rapid cycling.
    // Off = the label snaps instantly and shows/hides without fades.
    bool     animLabel        = true;
    // Pointer-hover lift: the tile under the mouse rises off the cascade and
    // settles back when the pointer leaves.  Off = the highlight snaps.
    bool     animHover        = true;
    bool     motionBlur       = true;     // Motion blur during animation
    bool     livePreview      = true;     // Live WGC window thumbnails (false = static snapshots)
    // Live wallpaper backdrop: stream the desktop capture behind the
    // cascade every frame so animated wallpapers (Wallpaper Engine, Lively)
    // keep playing — including during the cycle animation.  Off = one
    // static snapshot taken when the cascade opens (dedicated GPU copy, so
    // a live desktop tile can't mutate it through the shared texture).
    bool     liveBackground   = true;
    // Pace overlay rendering with Present(1) so live previews update once
    // per monitor refresh.  Costs GPU time on high-refresh displays.
    bool     vsyncLivePreview = false;
    // Capture the real taskbar live during the session instead of freezing
    // the pre-hide frame.  Requires a bar-sized WGC capture (Win11 25H2+);
    // on builds that deliver the tall 24H2-style capture the core falls
    // back to the frozen snapshot automatically.
    bool     taskbarLivePreview = false;
    // Draw a taskbar preview inside the overlay at all.  Off = the overlay
    // still hides/holds the real taskbar for the session but renders nothing
    // in its place (and taskbarLivePreview is implicitly disabled).
    bool     taskbarPreview   = true;
    uint32_t maxWindows       = 10;       // Max windows displayed in stack (2-10)

    // --- Appearance ---
    // Visual preset for the 3D switcher (Appearance → Visual preset):
    // 0 = Cascade (classic Win7 Flip3D layout), 1 = Cover Flow (centred
    // carousel, scene/CoverFlowLayout).  Latched at Activate — a reload
    // mid-session applies on the next activation.
    int      visualPreset     = 0;
    // Glass floor reflection under the stack (Appearance → Reflections):
    // each tile draws a faint mirrored copy below its bottom edge with a
    // glassy falloff.  Works in both visual presets.  Off (default) keeps
    // the classic look and costs nothing.
    bool     reflections      = false;
    // Background opacity target while the cascade is shown (0-100 %).
    // 0 = fully black backdrop, 100 = wallpaper fully visible.
    // 28 matches the original kBgAlpha = 0.28f cascade look.
    uint32_t backgroundOpacity = 28;
    // Wallpaper backdrop blur while the cascade is shown (0-100 %).
    // 0 (default) = no blur — the shader takes the single-sample path, so
    // the feature costs nothing unless enabled.
    uint32_t backgroundBlur = 0;
    // Include the desktop pseudo-window as the last tile of the cascade
    // (the classic Win7 Flip3D behaviour).  Off removes the tile — the
    // freed slot goes to the next real window — while the wallpaper
    // backdrop keeps working through a dedicated capture.
    bool     showDesktopTile  = true;
    // Selected-window label on the front slot (title + program icon) for
    // readability.  `selectedLabel` is the master switch (default OFF —
    // the classic clean cascade look); the flags below select the parts
    // independently (Appearance → Customize) once it's enabled.
    bool     selectedLabel      = false;
    bool     selectedLabelTitle = true;
    bool     selectedLabelIcon  = true;
    // Aero-glass plate behind the label.  Off draws the text/icon directly
    // with a stronger drop shadow instead.
    bool     selectedLabelBox   = true;

    // --- Performance ---
    bool     autoPerfTune     = true;     // Auto-detect and lower quality if needed
    int      perfProfile      = -1;       // -1=auto, 0=low, 1=medium, 2=high
    // Activation warm-up budget (ms): how long Activate keeps pumping
    // DwmFlush cycles for WGC first-frame delivery before showing the
    // overlay (leaves early once every capture has a frame).  16 ms ≈ the
    // original single DwmFlush at 60 Hz.  Auto perf tune overrides this
    // with a device-derived value (see EffectiveStartDelayMs).
    uint32_t startDelayMs     = 16;       // 1-1000

    // --- Input / triggers ---
    bool     ignoreFullscreen = false;    // Don't capture Win+Tab over fullscreen apps
    bool     mouseWheelCycle  = true;     // Mouse wheel cycles the cascade
    // EVERY key that steps through the stack while the cascade is open
    // (Controls → Mouse & keyboard → Navigation keys), the activation hotkey's
    // own key included, which is why the defaults name Tab.  It is an ordinary
    // entry here, removable like the arrows.
    //
    // Each list is ';'-separated and uses the same token names as
    // activationHotkey (KeyboardHook::ParseHotkey): a bare key, or SHIFT plus a
    // key, Shift being the one modifier a hand is free to add while the
    // activation combination is still held.
    //
    // A token prefixed with '!' is REMEMBERED BUT OFF, so a binding can be
    // parked without being retyped later.  Anything unparsable is skipped.
    // Empty lists are legitimate and mean "no keyboard navigation"; Config::Load
    // folds a legacy `keyboardNav: false` into them.
    //
    // Up to KeyboardHook::kMaxBindingKeys per direction, because the hook reads
    // each list from a single packed word, which is what keeps the
    // per-keystroke lookup lock-free.
    std::wstring navForwardKeys = L"Tab;Down;Right";       // next window
    std::wstring navBackKeys    = L"Shift+Tab;Up;Left";    // previous window
    std::wstring ignoredApps;             // ';'-separated exe names/paths to ignore
    // Exclusion list (General): windows of these executables never appear
    // in the cascade — the hotkey still works, the windows are simply left
    // out of the stack (they stay cloaked behind the overlay like any
    // other non-cascade surface and are restored on dismiss).
    std::wstring excludedApps;            // ';'-separated exe names/paths
    // Toggle activation for combo bindings: releasing the combo modifier
    // does NOT commit — the cascade stays open until Enter (commit) or
    // Escape (cancel), exactly like single-key bindings.  Single-key
    // bindings are inherently toggle, so this flag only matters when the
    // combination has at least one modifier.
    bool     hotkeyToggleMode = false;
    // Activation combination, '+'-separated tokens: modifiers Ctrl/Shift/
    // Alt/Win plus one main key ("Win+Tab", "Ctrl+Alt+F", "MButton",
    // "Win+XButton1", ...).  Parsed by KeyboardHook::ParseHotkey; invalid
    // strings fall back to Win+Tab.
    std::wstring activationHotkey = L"Win+Tab";
    // Keys that COMMIT the selection while the cascade is open, the ones that
    // CANCEL it, and the ones that CLOSE the hovered (or selected) window.
    //
    // LISTS, in exactly the form navForwardKeys uses: ';'-separated tokens, a
    // bare key or Shift+key each, '!' for "kept but switched off".  They were
    // one key apiece until 1.6 Build 3, which is the only reason they ever
    // read as single strings — someone who commits with Enter AND with Space,
    // or closes with both Delete and Backspace, was writing that binding twice
    // in their head and could only have one of them.
    //
    // Empty means the action has no key at all.  For the close key that is
    // exactly what the old `closeKeyEnabled` switch said, which is why that
    // switch is gone (Config::Load folds it into this list).  Commit and cancel
    // are different: a cascade with neither can only be closed with the mouse
    // or the touchpad, so the Settings page keeps one entry on each and this
    // side reports a file that says otherwise.
    std::wstring commitKeys = L"Enter";
    std::wstring cancelKeys = L"Escape";
    std::wstring closeKeys  = L"Delete";

    // --- Mouse in the cascade (Controls → Mouse & keyboard) ---
    // Mouse button identifiers shared by the three bindings below:
    //   0 = off, 1 = left, 2 = right, 3 = middle, 4 = X1, 5 = X2.
    //
    // Master switch for everything the pointer does to the stack — hover,
    // picking, closing and dragging.  Off (default) = the cascade takes
    // keyboard, wheel and touchpad exactly as it always has, with no pointer
    // message posted and no hit test run.
    //
    // Off by default deliberately: an upgrade must not silently hand a
    // familiar switcher new mouse behaviour, least of all a binding that
    // CLOSES a window on a middle click.  Everything below stays available,
    // one switch away.
    bool     pointerInCascade  = false;
    // Hover + click to pick a window.  The tile under the pointer lifts (see
    // animHover) and clicking it commits — a distant window first spins to
    // the front, then the ordinary exit morph plays.
    bool     mouseSelect       = true;
    int      mouseSelectButton = 1;      // left
    // DRAG the stack while Window snap is off.  Right button by default,
    // because left now picks a window.  The touchpad swipe is unaffected by
    // this toggle — it has its own gestures.
    bool     mouseDragEnabled  = true;
    int      mouseDragButton   = 2;      // right
    // Close the hovered window with the mouse (mouseCloseButton).  Sends
    // WM_CLOSE — the app decides what happens next, exactly as if its own close
    // button had been pressed.  A pointer feature, so pointerInCascade gates it.
    bool     closeFromCascade  = true;
    int      mouseCloseButton  = 3;      // middle
    // The same action from the KEYBOARD is `closeKeys` above, deliberately NOT
    // gated on anything here: one needs a mouse and the other does not, and a
    // keyboard-only user must be able to keep the key while every pointer
    // feature is off — or drop the key while keeping the click.  An empty
    // closeKeys is how the key is switched off now.

    // --- Search (Settings → Search) ---
    // Type while the cascade is open and it narrows to the matching windows;
    // clearing the query brings the rest back.  Off (default) = printable keys
    // are swallowed as strays exactly as before.
    bool     searchEnabled     = false;
    // Draw the themed field below the cascade.  Off = the filtering still
    // works, it just has no on-screen field (for a deliberately bare look).
    bool     searchBox         = true;
    // Match the owning executable's name as well as the window title, so
    // "chrome" finds a tab whose title mentions neither.
    bool     searchMatchProcess = true;
    // Where the field sits on the cascade host, as a percentage of the
    // primary monitor: X is the field's CENTRE, Y its BOTTOM edge, so the
    // default (50, 94) is centred just above the taskbar.  Percentages rather
    // than pixels so one setting reads the same on every display.
    uint32_t searchPosX        = 50;
    uint32_t searchPosY        = 94;
    // Field size, 50-200 % of its natural size.
    uint32_t searchScale       = 100;

    // --- Touchpad (Windows Precision Touchpad gestures) ---
    // Master switch (Controls → Navigation → Touchpad navigation).  Off =
    // the raw-input listener is never even registered, so a machine without
    // a touchpad — or a user who wants none of this — pays nothing and the
    // keyboard/mouse paths stay bit-identical.
    bool     touchpadNav        = true;
    // --- The three gesture LISTS -------------------------------------------
    // Same shape as navForwardKeys: ';'-separated tokens, '!' for "kept but
    // switched off", empty = the action has no gesture at all.  Lists because
    // one hand does not always want to draw the same stroke: a pad where the
    // four-finger diagonal is comfortable sitting down and the two-finger one
    // is comfortable on a desk should be able to have both.
    // Parsed case-insensitively by TouchpadHook::Parse*List; unknown tokens
    // are skipped and reported once.
    //
    // Gestures that OPEN the cascade — DIAGONAL strokes, because Windows' own
    // slide recogniser only claims the four cardinal directions, so a diagonal
    // is free for the taking and nothing of the user's Windows configuration
    // has to be touched.  Two or four fingers, never three: three-finger
    // slides are the ones Windows ships bound to Alt+Tab and Task View, and
    // its recogniser is loose enough about the angle that even a diagonal
    // trips them.
    //   TwoDownRight  — two fingers,  "\" (top-left → bottom-right)
    //   TwoDownLeft   — two fingers,  "/" (top-right → bottom-left)
    //   FourDownRight — four fingers, "\"
    //   FourDownLeft  — four fingers, "/"
    std::wstring touchpadActivateGestures = L"TwoDownRight";
    // Swipes that CYCLE the stack while it is open (TwoSwipe / FourSwipe —
    // three is Windows' own, see above).  Horizontal only: |dx| must dominate
    // |dy|, so a two-finger scroll straight up/down still reaches the wheel
    // path unchanged.
    std::wstring touchpadCycleGestures = L"TwoSwipe";
    // Gestures that COMMIT the selection while the cascade is open — the
    // touchpad equivalent of Enter:
    //   OneTap  — one-finger tap
    //   TwoTap  — two-finger tap
    //   TwoDown — two fingers swiped down
    std::wstring touchpadCommitGestures = L"OneTap";
    // Swipe left = next window (false, the default: the row follows the
    // fingers) or previous (true).
    bool     touchpadReverse    = false;
    // Swipe distance per cycle step, 1-100.  100 = a flick of ~2 % of the
    // pad width steps once, 1 = ~14 %.  50 ≈ 8 %.
    int      touchpadSensitivity = 50;
    // Several gestures out of ONE touch.  Off (default), a touch that fires a
    // gesture is finished — whatever else the fingers do before they lift is
    // ignored, which is what keeps the tail of an opening diagonal from
    // stepping the stack it just opened.  On, the stroke is retired and a new
    // one starts under the same fingers: open the cascade with the diagonal,
    // carry straight on sideways to pick a window, and never lift.  The price
    // is that a wandering stroke can now say two things, and the Settings page
    // says so.
    bool     touchpadContinuous = false;
    // Input smoothing, 0-100.  Runs an exponential filter over the contact
    // centroid and drops sub-threshold jitter, so a resting hand or a
    // twitchy pad cannot nudge the stack.  0 = raw contact deltas.
    int      touchpadSmoothing  = 35;
    // The opening stroke drawn backwards cancels the session, like Escape.
    bool     touchpadCancelSwipe = true;

    // --- Free stack movement ---
    // Window snap (Controls → Navigation).  ON (default) = every input step
    // lands the stack on a whole window, exactly as it always has.  OFF =
    // dragging with the mouse or swiping the touchpad scrubs the stack
    // CONTINUOUSLY — it follows the pointer at whatever speed the pointer
    // moves, can be held between two windows, and settles onto the nearest one
    // when released.  Deliberately does NOT apply to the keyboard or the mouse
    // wheel: those are discrete inputs and keep stepping one window at a time
    // either way.
    bool     windowSnap         = true;

    // --- Misc ---
    bool     showDebugInfo    = false;    // Output debug strings
    // Settings-app theme index (0 Skeuo Dark, 1 Skeuo White, 2 Minimal
    // Dark, 3 Minimal White, 4 Glassmorphism).  Owned by the Settings app;
    // the core reads it so the selected-window label's plate matches the
    // chosen CKSettings look.
    int      appTheme         = 0;
};

namespace Config {

/// Get the config file path (%APPDATA%\CKFlip3D\config.json).
std::wstring GetConfigPath();

/// Load config from disk. Returns defaults if file doesn't exist.
AppConfig Load();

/// Save config to disk. Creates directory if needed.
void Save(const AppConfig& cfg);

} // namespace Config
