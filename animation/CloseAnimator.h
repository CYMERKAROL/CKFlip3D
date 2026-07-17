#pragma once

#include "../scene/FlipScene.hpp"
#include "../animation/Easing.h"
#include <vector>
#include <cstdint>

/// Close-transition animator: plays when a window is closed (destroyed by
/// the OS) while the cascade is on screen.  Previously the stack snapped
/// instantly to the smaller layout; this animates the reflow instead.
///
/// Choreography (one 340 ms pass; the reflow window inside it is 220 ms —
/// the length of a single non-chained cycle, so the pacing matches):
///   - Dying tile(s): slide straight DOWN at constant scale (the same
///     visual read as minimized windows in the entry/exit morph, minus
///     the shrink), then vanish through a smooth alpha ramp that starts
///     after the fall visibly reads and completes exactly when the fall
///     does.  Z stays fixed at the source slot — the cycle-wrap lesson:
///     a moving Z re-sorts against sliding neighbours and produces
///     overlap artifacts.
///   - Survivors: hold still until kReflowStart (mid-fall), then slide
///     to the freshly rebuilt (N-k) slots with InOutCubic — the same
///     symmetric ease the entry/exit morph uses, so the stack settles
///     with zero velocity at both ends (no jerk at the staggered start,
///     no snap at the end).  Both axes animate together: the new slots
///     come straight from FlipScene::BuildSlots for the new count, so
///     horizontal AND vertical placement land exactly where FlipScene
///     dictates.
///   - Spawn-ins (overflow refill): when the stack had more windows than
///     visible slots, the window inheriting the freed back slot rides
///     the reflow window, sliding in from the back-spawn extrapolation
///     point while fading in (forward-cycle wrap phase 2 look).
///
/// Camera re-frame compensation: BuildSlots re-derives the camera for
/// the new count (framing/zoom depend on N), and FlipScene has no camera
/// setters (and must stay untouched) — so the camera itself snaps at the
/// rebuild.  Begin() cancels the visible effect exactly: every start
/// pose is re-expressed through (oldView × newView⁻¹), i.e. its VIEW-
/// SPACE pose under the old camera is preserved under the new one.  FOV
/// and viewport aspect don't change with N, so an identical view-space
/// pose projects to identical pixels — frame 1 of the transition matches
/// the last pre-close frame exactly, and the camera change is absorbed
/// smoothly into the slot lerp instead of appearing as a lateral /
/// vertical jump at the start.
///
/// Ownership contract (mirrors CycleAnimator):
///   - FlipController detects the closed window, erases it from its
///     arrays, rebuilds the scene for the smaller count, then calls
///     Begin() with the PRE-removal slot snapshot + PRE-rebuild camera.
///     FlipScene is never modified beyond GetSlotMut writes.
///   - Dying tiles are freestanding (not scene slots); FlipController
///     draws them like entry/exit overflow tiles, textured from the
///     closed windows' frozen last WGC frames (parallel by index).
///   - Timing is wall-clock QPC sampled every Tick — frame-rate /
///     VSYNC independent like CycleAnimator and EntryExitAnimator.
///
/// Race-condition guarantees are enforced by the caller's gating (see
/// FlipController::RemoveClosedWindows): a close transition never starts
/// while a cycle (active or queued) or the entry/exit morph runs, and
/// cycling is blocked while this animator is active.  A closed window
/// leaves m_windows the moment its transition starts, so it can never be
/// re-detected and re-animated — the fade-out plays exactly once.
/// Multi-close: windows closed in the SAME sweep share one pass; windows
/// closed while a pass is running MERGE into it (see Begin) instead of
/// queuing serially — a burst of closes never multiplies the duration.
///
/// Resolution / multi-monitor: everything here is world-space math over
/// endpoints that are already viewport-adapted (start = live scene slots,
/// target = BuildSlots for the current cascade space), and the constants
/// are fractions of inter-slot deltas or of the tile's own world height —
/// no absolute pixel values.  The controller draws dying tiles with the
/// same cascadeAspect + monitor NDC remap as cascade/overflow tiles, so
/// any resolution or monitor layout the cascade renders correctly on,
/// this transition does too.
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

    /// Start the transition — or MERGE into a running one.  When called
    /// while active (more windows closed mid-transition), the in-flight
    /// dying tiles carry over and continue falling/fading from their
    /// current pose (alpha strictly monotonic — a fade can never replay),
    /// and the survivors re-route from their current mid-lerp pose to the
    /// new targets.  The merged run costs ONE fresh pass from the merge
    /// point, so an N-window burst never stacks N sequential passes.
    ///   scene            — ALREADY rebuilt for the smaller count; its
    ///                      slots are read as the target end state, then
    ///                      overwritten with the first-frame start state
    ///                      (survivors at their camera-compensated old
    ///                      pose, spawn-ins hidden) so no snapped frame
    ///                      can ever present.
    ///   startSlots       — slot snapshot taken BEFORE the removal/rebuild
    ///                      (mid-transition pose when merging).
    ///   dyingSlotIndices — ascending indices into startSlots of the
    ///                      newly closed windows' visible tiles.
    ///   oldCam           — camera pose BEFORE the rebuild (the frame the
    ///                      startSlots poses were authored under).
    void Begin(FlipScene& scene,
               const std::vector<TileSlot>& startSlots,
               const std::vector<uint32_t>& dyingSlotIndices,
               const CameraPose& oldCam);

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

    uint32_t m_survivorCount = 0;           // target slots with a mapped start pose
    std::vector<TileSlot> m_survivorStart;  // per NEW slot i (i < m_survivorCount), camera-compensated
    std::vector<TileSlot> m_targetSlots;    // per NEW slot (rebuilt scene snapshot)
    std::vector<TileSlot> m_dyingStart;     // dying tiles' camera-compensated start poses
    std::vector<TileSlot> m_dyingSlots;     // dying tiles' animated current poses
    std::vector<float>    m_dyingDrop;      // per-dying world-space drop distance (screen-down)
    TileSlot m_backSpawn{};                 // spawn-in entry point behind the last slot

    void ComputeBackSpawn();
};
