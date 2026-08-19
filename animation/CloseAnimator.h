// ---------------------------------------------------------------------------
// What the cascade does when a window disappears out from under it.  The dying
// tile falls and fades, the survivors reflow into the smaller layout, and any
// window that was hidden behind the stack slides in to fill the gap.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#include "../scene/FlipScene.hpp"
#include "../animation/Easing.h"
#include <vector>
#include <cstdint>

/// Close-transition animator: plays when a window is closed (destroyed by
/// the OS) while the cascade is on screen.
///
/// Choreography (one pass; see the constants for the timings):
///   - Dying tile(s): slide straight DOWN at constant scale, then fade out
///     on a ramp that finishes exactly when the fall does.  Z stays pinned
///     to the source slot, because a moving Z re-sorts against sliding
///     neighbours and produces overlap artifacts.
///   - Survivors: hold still until the fall is half done, then slide to the
///     freshly rebuilt slots with InOutCubic, so the stack settles with zero
///     velocity at both ends.  The targets come straight from
///     FlipScene::BuildSlots for the new count, so both axes land where
///     FlipScene says rather than where this animator guesses.
///   - Spawn-ins: when the stack had more windows than visible slots, the one
///     inheriting the freed back slot rides the same window, sliding in from
///     the back-spawn point while it fades in.
///
/// Camera re-frame compensation.  BuildSlots re-derives the camera for the new
/// count, and FlipScene has no camera setters, so the camera itself snaps at
/// the rebuild.  Begin() cancels the visible effect: every start pose is
/// re-expressed through (oldView × newView⁻¹), preserving its VIEW-SPACE pose
/// across the change.  FOV and viewport aspect do not depend on N, so an
/// identical view-space pose projects to identical pixels: frame 1 of the
/// transition matches the last pre-close frame, and the camera change is
/// absorbed into the slot lerp instead of showing up as a jump.
///
/// Ownership contract (mirrors CycleAnimator):
///   - FlipController detects the closed window, erases it from its arrays,
///     rebuilds the scene for the smaller count, then calls Begin() with the
///     PRE-removal slot snapshot and PRE-rebuild camera.  FlipScene is never
///     modified beyond GetSlotMut writes.
///   - Dying tiles are freestanding rather than scene slots; the controller
///     draws them like entry/exit overflow tiles, textured from the closed
///     windows' frozen last frames (parallel by index).
///   - Timing is wall-clock QPC sampled every Tick, so it is frame-rate and
///     VSYNC independent like the other animators.
///
/// The caller's gating is what makes this race-free (see
/// FlipController::RemoveClosedWindows): a close never starts while a cycle or
/// the entry/exit morph runs, and cycling is blocked while it does.  A closed
/// window leaves m_windows the moment its transition starts, so it can never be
/// re-detected and the fade-out plays exactly once.  Windows closed in the same
/// sweep share one pass, and windows closed DURING a pass merge into it rather
/// than queuing, so a burst of closes never multiplies the duration.
///
/// Everything here is world-space math over endpoints that are already
/// viewport-adapted, with constants expressed as fractions rather than pixels,
/// so any resolution or monitor layout the cascade renders on, this does too.
class CloseAnimator {
public:
    /// Camera pose snapshot (eye + look-at target), captured by the
    /// caller BEFORE the scene rebuild so Begin() can compensate the
    /// camera re-frame (see class comment).
    struct CameraPose {
        float eyeX, eyeY, eyeZ;
        float targetX, targetY, targetZ;
    };

    bool IsActive() const { return m_active; }
    float GetRawT() const { return m_rawT; }

    /// Freestanding dying-tile states, ascending old-slot order (parallel
    /// to FlipController::m_closingCaptures).  Empty whenever inactive.
    const std::vector<TileSlot>& GetDyingSlots() const { return m_dyingSlots; }

    /// Start the transition, or MERGE into a running one.  Called while active
    /// (more windows closed mid-transition), the in-flight dying tiles carry on
    /// falling and fading from their current pose, alpha strictly monotonic so
    /// a fade can never replay, and the survivors re-route from their mid-lerp
    /// pose to the new targets.  The merge costs one fresh pass from that
    /// point, so an N-window burst never stacks N sequential passes.
    ///   scene            — ALREADY rebuilt for the smaller count.  Its slots
    ///                      are read as the target end state, then overwritten
    ///                      with the first-frame start state, so no snapped
    ///                      frame can ever present.
    ///   startSlots       — slot snapshot taken BEFORE the removal and rebuild
    ///                      (the mid-transition pose when merging).
    ///   dyingSlotIndices — ascending indices into startSlots of the newly
    ///                      closed windows' visible tiles.
    ///   oldCam           — camera pose BEFORE the rebuild, the frame the
    ///                      startSlots poses were authored under.
    ///   newSlotSource    — optional per-NEW-slot source: the old slot index
    ///                      whose window now occupies it, or -1 to spawn in
    ///                      from the back.  nullptr, the default and always
    ///                      the case for the cascade preset, derives it
    ///                      instead: survivors fill the new slots in ascending
    ///                      order and the remainder spawn in.  Cover Flow has
    ///                      to pass an explicit map, because its row is a
    ///                      carousel: a window can move to a slot the
    ///                      ascending derivation would never pair it with, and
    ///                      windows outside the visible row can surface into
    ///                      one.
    ///   riseIn           — how a slot with no predecessor ARRIVES.  False,
    ///                      the default, slides it in from the back-spawn
    ///                      point, which is right for an overflow refill: the
    ///                      window really was behind the stack.  True rises it
    ///                      from below its own destination, the dying tile's
    ///                      fall played backwards, which is right when the
    ///                      window is RETURNING to a place it left that way,
    ///                      as the search filter's do.  A back-spawn there
    ///                      would fly every returning window the length of the
    ///                      stack, through all the others.
    void Begin(FlipScene& scene,
               const std::vector<TileSlot>& startSlots,
               const std::vector<uint32_t>& dyingSlotIndices,
               const CameraPose& oldCam,
               const std::vector<int>* newSlotSource = nullptr,
               bool riseIn = false);

    /// Advance on the QPC clock and write the interpolated slot states.
    /// On rawT >= 1 snaps to the target and raises the JustFinished edge.
    /// If the scene's slot count no longer matches the target (someone
    /// else rebuilt it), cancels hard without touching the scene.
    void Tick(FlipScene& scene);

    /// Snap to the end state immediately (used when Dismiss/Escape needs
    /// the cascade settled before the exit morph snapshots it).
    void FinishImmediate(FlipScene& scene);

    /// Drop all state without touching the scene (session teardown /
    /// defensive cross-session reset).  Also empties the dying tiles so
    /// no stale fade-out can ever draw again.
    void Cancel();

    /// Edge-triggered completion flag, same pattern as CycleAnimator.
    bool JustFinished();

    /// Motion-blur driver.  Bell-shaped over the reflow window (zero at
    /// both ends) — the cascade is stationary during the pre-reflow fall,
    /// so the start-heavy u² curve the cycle uses would blur still tiles.
    float GetMotionIntensity() const;

private:
    static constexpr float kDurationMs  = 340.0f;  // whole pass; reflow window = 0.65 × 340 ≈ 220 ms (== one cycle)
    static constexpr float kFallEnd     = 0.55f;   // dying tile fully fallen AND fully faded here
    static constexpr float kFadeStart   = 0.18f;   // alpha ramp starts after the fall visibly reads
    static constexpr float kReflowStart = 0.35f;   // survivors start mid-fall
    static constexpr float kDropFactor  = 0.60f;   // drop distance as a fraction of the tile's world height
    static constexpr float kBackSpawn   = 0.50f;   // back-spawn extrapolation (== CycleAnimator)

    bool     m_active   = false;
    bool     m_justDone = false;
    float    m_rawT     = 0.0f;
    int64_t  m_startQPC = 0;
    int64_t  m_qpcFreq  = 0;

    // Per NEW slot: the camera-compensated pose its window held before the
    // close (m_slotHasStart false → nothing held it, so the slot spawns in
    // from the back instead).  The cascade derivation fills a prefix; Cover
    // Flow's carousel map can leave gaps anywhere in the row.
    std::vector<TileSlot> m_slotStart;
    std::vector<bool>     m_slotHasStart;
    std::vector<TileSlot> m_targetSlots;    // per NEW slot (rebuilt scene snapshot)
    std::vector<TileSlot> m_dyingStart;     // dying tiles' camera-compensated start poses
    std::vector<TileSlot> m_dyingSlots;     // dying tiles' animated current poses
    std::vector<float>    m_dyingDrop;      // per-dying world-space drop distance (screen-down)
    TileSlot m_backSpawn{};                 // spawn-in entry point behind the last slot
    bool     m_riseIn = false;              // arriving slots rise from below (see Begin)

    void ComputeBackSpawn();
};
