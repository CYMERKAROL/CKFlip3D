# Changelog

## 1.5

**The classic cascade is unchanged.** Every new look is a preset you opt into, and the default preset renders exactly as 1.2 did. The new input paths are opt-in too, so upgrading never silently changes the switcher you already know.

### Appearance

- New **Visual preset** selector (Appearance): *Cascade* (classic, default) or **Cover Flow** — a centered carousel with the selected window flat in the middle and the side windows leaning gently outward on a common floor plane. Long rows divide the screen evenly and the deck scales down as the count grows, so every window the counter promises is genuinely visible.
- New Appearance option: **Reflections** (default off) — a soft glass-floor mirror below each tile, with a quadratic falloff. Works in both visual presets; zero extra draw calls when off.
- The Appearance page now shows a **live 3D preview** of your actual settings instead of a static mockup — real cascade and Cover Flow geometry, following the preset, background opacity and blur, reflections, the desktop tile and the window count, and replaying a cycle animation.

### Mouse in the cascade *(default off)*

- New master switch **Let the mouse act on the stack**, with three bindings under it: **pick a window** (left click), **close a window** (middle click) and **drag the stack** (right button).
- Hovering lifts the tile under the pointer; clicking a window deep in the stack spins it to the front on the ordinary cycle path and then commits, so a click and a Tab-and-release land identically.
- New Animations toggle: **Hover lift** — the tile's rise and fall can be switched off for an instant highlight.
- Closing sends `WM_CLOSE`, so the application runs its own close path, save prompts included.

### Search *(default off)*

- New **Search** page and feature: type while the cascade is open and the stack narrows to the matching windows.
- Matches space-separated words in any order against the window title **and** the owning program's name, so `code set` finds *Settings — Visual Studio Code*.
- Filtered-out windows leave on the close animation and **rise back into place** when the query shrinks, rather than flying across the stack.
- A query matching nothing empties the stack, says so, and blocks committing — a typo can never switch you to the wrong window.
- Themed on-screen field with configurable size (50–200%, rendered rather than magnified) and position by screen percentage. Requires Toggle activation.

### Touchpad gestures *(default on)*

- New second input source for Windows Precision Touchpads, reading raw HID reports on its own worker thread and posting the same messages the keyboard hook does — a gesture-opened cascade still takes Tab and Esc, and a hotkey-opened one still takes gestures.
- **Two-finger diagonal** opens the cascade, **two-finger horizontal swipes** cycle it, a **one-finger tap** commits and the **reversed opening stroke** cancels. Diagonals are used deliberately so none of your Windows gesture bindings has to be taken away.
- Configurable finger count, direction, sensitivity and smoothing, on a new **Controls → Touchpad gestures** sub-page with a live activity preview driven by the same recogniser the core uses.
- Horizontal dominance is required for the cycle swipe, so ordinary two-finger scrolling is unaffected.

### Controls

- New **Controls → Mouse & keyboard** sub-page collecting activation, commit, cancel and close keys plus the in-cascade mouse bindings — every one set by pressing it.
- **Commit**, **cancel** and **close-window** keys are now rebindable settings (`Enter`, `Esc`, `Delete` by default) instead of constants; the close key has its own switch, so it can be kept with every mouse feature off, or dropped while keeping the click.
- New **Window snap** option (General → Cascade, on by default). Turn it off and the mouse drag and touchpad swipe **scrub the stack continuously** — following the pointer at whatever speed it moves, holding between two windows and settling on the nearest one when released. The keyboard and wheel keep stepping one window at a time by design.

### Diagnostics

- CKFlip3D now keeps a **log about itself**: every failure it can detect is recorded with a stable code, a severity, a plain-language explanation and a technical line for bug reports — failed captures, windows left cloaked, lost devices, unparsable configs, refused foreground changes and more.
- The Settings sidebar shows a **mark per severity with a count**, and only when there is something to report. Clicking it opens the log — one tile per problem, worst first, with when it last happened and how often.
- Clearing an entry is a **watermark rather than a deletion**, so a problem that recurs is reported again; entries describing the machine rather than an event stay cleared for good.
- A correctly working program produces **no entries at all**, so a mark in the sidebar always means something.
- Two startup message boxes (no hardware acceleration, very low video memory) became log entries. The log lives at `%APPDATA%\CKFlip3D\diagnostics.jsonl` and is never sent anywhere.

### Installer

- Upgrading over a running copy now asks the core to **shut down cleanly** rather than killing it, so it un-cloaks its windows and restores the taskbar before being replaced.

## 1.2

- New General option: **Exclusion list** — windows of the listed applications never appear in the 3D cascade
- **Background blur intensity** implemented (Appearance, default 0%) — frosted-glass blur on the wallpaper behind the cascade, zero GPU cost when off
- New Controls option: **Toggle activation** — combo hotkeys can keep the cascade open after the modifier is released (Enter commits, Esc cancels); single-key bindings stay inherently toggle
- New Appearance option: **Live background** (on by default) — streams the wallpaper live behind the cascade; fixes animated wallpapers (Wallpaper Engine, Lively) freezing during the cycle animation
- New Animations toggle: **Selected label** — the label's glide and hold-cycle fade can now be switched off for instant snapping
- Binaries (core, Settings, installer) now ship Authenticode-signed as publisher **CYMERKAROL**

## 1.1

- Taskbar live preview fixed — the taskbar clock and tray now keep updating while the cascade is open (the old capture-shape gate silently disabled live frames on every Windows build)
- Smoother held-taskbar handling with the Windows auto-hide taskbar — no more per-frame position fighting with the shell's hide animation
- New Appearance option: **Desktop in cascade** — remove the desktop tile from the stack while keeping the wallpaper backdrop
- New Appearance option: **Selected window label** (off by default) — the selected window's title and program icon above the front tile, on a plate styled after the chosen CKSettings theme, with a Customize dropdown (title / icon / background box) and smooth position animation
- Updated repository demo media (50 fps GIFs, new close-animation clip)

## 1.0

- First public release
- Licensed under the PolyForm Noncommercial License 1.0.0
- Auto performance tune reworked: two-way quality ladder with hysteresis and a 60 Hz budget floor — fixes live previews silently freezing on mid-range GPUs and high-refresh (120/144 Hz) displays; quality now recovers automatically when headroom returns
- Enabled D3D11 multithread protection for the shared capture/render device — fixes wedged capture sessions on slower GPUs and virtual machines

## 0.95V Beta

- Critical error fixing & preparation for release
- Stability hardening: tray-icon logon-race retry, settings-app reload/restart handshake, capture teardown fixes
- Repository styling: full README, demo media (`assets/Repo`), published build scripts

## 0.91V Beta

- Codebase cleanup (legacy DWM-thumbnail stack removed)
- Initial public repository

## 0.9V Beta

- Close animation — windows closed while the cascade is open fade out while the stack reflows
- Settings improvements (per-animation toggles, background opacity)

## 0.8V Beta

- Installer (single-file setup with rollback & .NET runtime bootstrap)
- Major improvements across capture and rendering

## Earlier (Alpha)

- 0.75V: settings menu revamp
- 0.65V: full multi-monitor support
- 0.5V: first stable cascade (tilt fix, DWM exit-leak fix)
