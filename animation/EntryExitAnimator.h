#pragma once

#include "../scene/FlipScene.hpp"
#include "../capture/windowscanner.h"
#include <vector>
#include <cstdint>

/// How the desktop pseudo-tile behaves during the entry morph.
enum class DesktopEntryMode : uint8_t {
    HiddenUntilCascade,   // normal app activation (default)
    FadeFromFlat,         // activation from desktop
    SelectedDesktop,      // (reserved for future / Dismiss-path mirror)
};

/// Entry + exit animator (2D-rect ↔ 3D-cascade morph with concurrent dim).
/// Kept separate from CycleAnimator so activation/teardown morphing and
/// cycle wrapping don't share state.  Read-only with respect to real
/// windows — only reads placement / rect / aspect via WindowInfo and
/// GetWindowPlacement; never writes to windows.
///
/// Direction is encoded by `m_reverse`:
///   forward (entry) — flat → cascade. BeginEntry seeds m_flatSlots and
///                     overwrites the scene with flat state so the first
///                     frame matches the user's real desktop.
///   reverse (exit)  — cascade → flat. BeginExit snapshots the current
///                     cascade as start and computes fresh flat rects as
///                     the end target. Scene is left in cascade state and
///                     the morph plays backward.
///
/// Timeline (total 220–320 ms, scaled to slot count) — single phase, everything
/// starts at rawT=0.  Small stacks animate faster; large stacks get the full
/// 320 ms.  Tick computes a unified forwardT = m_reverse ? (1-rawT) : rawT
/// that drives every interpolation, so reverse is naturally a mirror.
///   Motion (position/scale/alpha/rotY): InOutCubic(forwardT) — symmetric
///          ease, zero velocity at both ends; no jump at frame 0, no snap
///          at the end.
///   Dim    (wallpaper + black backdrop): OutCubic(forwardT) — fast ramp
///          so the wallpaper is visibly dimmed by the time gaps between
///          tiles open up.
///   Tilt   (scene-wide Y/X): rides the same morphT as motion — no
///          delay (Round-6 Fix 18).  Per-tile rotY (fanned deck) also
///          lerps with motion alongside the other slot fields.
class EntryExitAnimator {
public:
    bool IsActive() const { return m_active; }

    /// Snapshot the current (cascade) scene state, compute flat 2D-rect slots,
    /// and overwrite the scene with the flat state so the first rendered frame
    /// shows tiles where the real windows were.  Tilt is reset to 0.
    ///
    /// `desktopHwnd` is the Progman/WorkerW handle — the desktop tile always
    /// participates in the morph.
    /// `desktopMode` — controls the desktop pseudo-tile's entry behaviour.
    /// HiddenUntilCascade / FadeFromFlat force the desktop tile's flat-state
    /// alpha to 0 and apply a per-frame smoothstep cascade-α multiplier in
    /// Tick (late vs early fade respectively).  SelectedDesktop leaves the
    /// desktop tile untouched.  No effect on the wallpaper background layer
    /// or on the cascade snapshot.
    ///
    /// `taskbarRectOverrides` — if non-empty (must equal windows.size() when
    /// non-empty), index i with a non-empty RECT replaces the i-th window's
    /// flat source rect.  Used by the caller to inject taskbar-button rects
    /// for minimized windows so they emerge from the taskbar.  Empty rects
    /// (right<=left or bottom<=top) are ignored.
    /// `keyPressQPC` — the QPC tick at which the user pressed the
    /// activation key (or as close to it as the controller can capture).
    /// Drives the "launch timing" dim system: DimFactor() samples the
    /// dim curve at `(now - keyPressQPC) / duration` instead of from
    /// the cascade animation's local rawT, so the first visible frame
    /// already shows dim ramped to where it would have been had the
    /// dim animation started at the keypress — including any time
    /// spent on EnsureFrame / first-content-render / Show after this
    /// BeginEntry call returns.  Cascade tile motion still uses the
    /// local `m_rawT`, so geometry timing is unaffected.  Pass 0 (the
    /// default) to disable launch timing — DimFactor will then use
    /// `m_rawT` exactly as before.
    void BeginEntry(FlipScene& scene,
                    const std::vector<WindowInfo>& windows,
                    float vpW, float vpH, float desktopW, float desktopH,
                    HWND desktopHwnd,
                    float cascadeAspect,
                    float originX, float originY,
                    const DirectX::XMMATRIX& remapNDC,
                    DesktopEntryMode desktopMode,
                    const std::vector<RECT>& taskbarRectOverrides,
                    int64_t keyPressQPC = 0);

    /// Begin the EXIT morph (cascade → flat).  Snapshots the current scene
    /// as the cascade start state and computes fresh flat rects from live
    /// window data as the end target.  Read-only: never writes to a real
    /// window.  Tick then plays the entry morph in reverse.
    /// `tileFadeOutOnExit` — per-window flag (parallel to `windows`).  Tiles
    /// with the flag set have their α multiplied by an OutQuad decay across
    /// the reverse morph so they reach α=0 by the end.  Used for windows
    /// that won't be visible after the overlay hides:
    ///   - Non-selected minimized windows (they stay minimized).
    ///   - The desktop pseudo-tile when something else is being picked.
    ///   - Non-desktop tiles when the desktop itself is the pick.
    /// May be empty / shorter than `windows` — out-of-range tiles default
    /// to no fade (kept fully visible).
    /// `animateOverflow` — when true, windows beyond the visible slot count
    /// get the mirrored overflow choreography: they fade IN from behind the
    /// cascade and land at their real desktop positions by the end of the
    /// exit, so the overlay hides onto an identical-looking desktop.  Pass
    /// false when those windows won't be visible afterwards (desktop pick).
    void BeginExit(FlipScene& scene,
                   const std::vector<WindowInfo>& windows,
                   const std::vector<uint32_t>& zRanks,
                   float vpW, float vpH, float desktopW, float desktopH,
                   HWND desktopHwnd,
                   float cascadeAspect,
                   float originX, float originY,
                   const DirectX::XMMATRIX& remapNDC,
                   const std::vector<bool>& tileFadeOutOnExit,
                   bool animateOverflow = true);

    /// Advance the animation using a QPC clock.  Interpolates scene slots
    /// and scene-wide tilt toward the appropriate end state (cascade for
    /// entry, flat for exit).  On rawT ≥ 1, snaps to the end and marks
    /// itself Idle.
    void Tick(FlipScene& scene);

    /// Snap instantly to the end-state (cascade for entry, flat for exit).
    /// Used when the user dismisses mid-animation.
    void Finalize(FlipScene& scene);

    /// If currently in a forward (entry) morph, flip direction immediately
    /// while preserving the visual frame: the new exit rawT is set to
    /// (1 - current_entry_rawT) and the QPC anchor is shifted so timelineT
    /// keeps its current value and continues backward toward 0.  Returns
    /// true if a reversal was performed.  No-op when idle or already in
    /// reverse.  Both endpoints (m_flatSlots, m_cascadeSlots) remain valid
    /// so the same lerp drives the rewind.
    bool ReverseInPlace();

    /// True iff currently running in reverse (exit) mode.
    bool IsReverse() const { return m_active && m_reverse; }

    /// Current raw morph progress in [0,1] (wall-clock driven, pre-timeline).
    float GetRawT() const { return m_rawT; }
    float GetMorphBlend() const { return m_morphBlend; }

    /// Returns true exactly once after an exit animation finalizes; resets
    /// on read.  Same edge-trigger pattern as CycleAnimator::JustFinished.
    bool JustFinishedExit();

    /// Background dim multiplier (multiplied against kBgAlpha at render
    /// time).  Eased with OutCubic on a unified forwardT so dim catches
    /// on quickly during entry and ramps down quickly during exit.
    float DimFactor() const;

    /// Per-frame overflow tile state — extra windows beyond the cascade's
    /// `maxVisible` slot count.  During entry they fly from their real
    /// desktop positions toward the back of the stack while fading out;
    /// during exit they mirror that (fade in from behind the cascade back
    /// to their real positions).  The returned slots and HWNDs are
    /// parallel; FlipController draws them via the same QuadDrawCall path
    /// as visible cascade tiles.  Empty between sessions.
    const std::vector<TileSlot>& GetOverflowSlots() const { return m_overflowCurrent; }
    const std::vector<HWND>&     GetOverflowHwnds() const { return m_overflowHwnds; }
    const std::vector<RECT>&     GetFlatSourceRects() const { return m_flatSourceRects; }

private:
    // Round-7: fixed Win7 timing — 16 frames at 60 Hz = 266.67 ms,
    // independent of window count.  Win7 reference uses 16 frames for
    // both 4-window and 10-window cases; CKFlip's earlier per-N scaling
    // (220–320 ms) made the morph feel inconsistent across stacks.
    static constexpr float kDurationMs = 16.0f * 1000.0f / 60.0f;

    // Round-6 overflow incineration constants.  Each overflow tile begins
    // its alpha decay at rawT = k * kOverflowStaggerStep (clamped) and
    // finishes by rawT = start + kOverflowFadeSpan (clamped to 1.0).
    // Tile k=0 fades at the start of entry; later overflow tiles incinerate
    // sequentially so the whole pile is gone by the time the cascade lands.
    static constexpr float kOverflowStaggerStep = 0.05f;
    static constexpr float kOverflowFadeSpan    = 0.45f;

    void ApplyState(FlipScene& scene,
                    const std::vector<TileSlot>& slots,
                    float tiltY, float tiltX) const;

    void ApplyWarmupEndpoint(FlipScene& scene);

    /// Populate the overflow choreography (m_overflow* vectors) for the
    /// windows beyond the visible slot count.  `stackRects` are the
    /// UNTRIMMED flat rects parallel to `windows`.  Direction-agnostic:
    /// m_overflowFlat is the flat-endpoint pose (real position, α=1),
    /// m_overflowCollapsed the cascade-endpoint pose (behind the back
    /// slot, α=0); Tick lerps between them with the shared blends.
    void BuildOverflowChoreography(const FlipScene& scene,
                                   const std::vector<WindowInfo>& windows,
                                   const std::vector<RECT>& stackRects,
                                   HWND desktopHwnd,
                                   float vpW, float vpH,
                                   float cascadeAspect,
                                   float originX, float originY,
                                   const DirectX::XMMATRIX& remapNDC);

    void ClearOverflow();

    bool     m_active        = false;
    bool     m_reverse       = false;   // false = entry (flat→cascade), true = exit (cascade→flat)
    bool     m_justDoneExit  = false;   // edge-trigger flag, consumed by JustFinishedExit()
    bool     m_firstTick     = false;   // very first Tick after Begin* — see Tick warmup

    std::vector<bool> m_exitFadeOut;       // exit-only: per-slot α-fade flag
    DesktopEntryMode m_desktopEntryMode = DesktopEntryMode::HiddenUntilCascade;
    int      m_desktopSlotIndex = -1;   // visible-slot index of the desktop tile, -1 if none
    float    m_rawT          = 0.0f;
    float    m_morphBlend    = 0.0f;
    float    m_durationMs    = kDurationMs;
    int64_t  m_startQPC      = 0;
    int64_t  m_qpcFreq       = 0;
    // Launch-timing anchor for the dim animation.  Captured from the
    // controller at BeginEntry() time and held until Finalize.  Non-zero
    // = launch timing is active and DimFactor samples the dim curve at
    // (now - m_keyPressQPC) / duration instead of `m_rawT`.  Zero =
    // disabled, falls back to the cascade-local rawT (default).
    int64_t  m_keyPressQPC = 0;

    std::vector<TileSlot> m_flatSlots;     // forward(t=0) / reverse(t=1) state — 2D rects
    std::vector<TileSlot> m_cascadeSlots;  // forward(t=1) / reverse(t=0) state — 3D cascade
    std::vector<RECT>     m_flatSourceRects; // SCREEN-space flat endpoint rects
    float    m_tiltYFinal  = 0.0f;
    float    m_tiltXFinal  = 0.0f;

    // Round-6 Fix 19: per-HWND flat geometry captured at BeginEntry.  On
    // BeginExit we look up flat slots by current HWND so the exit lerp
    // is HWND-matched on both ends — cycle rotation can't desync the
    // cascade aspect from the flat target aspect.  Linear scan; ≤16
    // windows.
    std::vector<HWND>     m_entryFlatHwnds;
    std::vector<TileSlot> m_entryFlatByHwnd;   // parallel to m_entryFlatHwnds
    std::vector<RECT>     m_entryFlatRectsByHwnd;
    // Set true for HWNDs whose entry-time flat rect was a taskbar-button
    // override (minimized windows emerging from the taskbar).  On BeginExit
    // we DO NOT restore these from cache — the picked window must animate
    // to its real on-screen restore rect, not back to the taskbar.
    std::vector<bool>     m_entryFlatHadTbOverride;

    // Overflow choreography — populated in BeginEntry AND BeginExit,
    // lerped in Tick (direction-agnostic: blends are sampled at timelineT,
    // so exit naturally plays the entry path backward).  All vectors share
    // the same length (one per overflow window beyond scene.SlotCount()).
    std::vector<TileSlot> m_overflowFlat;       // flat endpoint: real position, α=1
    std::vector<TileSlot> m_overflowCollapsed;  // cascade endpoint: behind back slot, α=0
    std::vector<TileSlot> m_overflowCurrent;    // per-frame interpolated slot
    std::vector<HWND>     m_overflowHwnds;      // parallel to above
    std::vector<float>    m_overflowFadeStart;  // timelineT at which alpha decay starts
    std::vector<float>    m_overflowFadeEnd;    // timelineT at which alpha hits 0

public:
    /// Clear cached entry flat geometry.  Called from Dismiss/Escape so
    /// stale HWNDs don't survive into the next session.
    void ClearEntryFlatCache() {
        m_entryFlatHwnds.clear();
        m_entryFlatByHwnd.clear();
        m_entryFlatRectsByHwnd.clear();
        m_entryFlatHadTbOverride.clear();
        m_flatSourceRects.clear();
        m_overflowFlat.clear();
        m_overflowCollapsed.clear();
        m_overflowCurrent.clear();
        m_overflowHwnds.clear();
        m_overflowFadeStart.clear();
        m_overflowFadeEnd.clear();
    }
};
