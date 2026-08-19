// ---------------------------------------------------------------------------
// Moving the cascade one window forward or back.  Handles the timed cycle, the
// held-key chain that accelerates it, and the free scrub where the pointer
// owns the parameter instead of the clock.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#include "../scene/FlipScene.hpp"
#include "../animation/Easing.h"
#include <vector>
#include <cstdint>

/// Win7-style cycle transition animator.
///
/// Forward wrapping (front→back): Phase 1 — front window swings toward
/// N0 (virtual slot in front of camera) with scale boost while fading out.
/// Phase 2 — appears at backSpawn (behind last visible slot) and slides
/// into the back of the cascade while fading in.
///
/// Backward wrapping (back→front): Phase 1 — back window slides deeper
/// toward backSpawn while fading out.  Phase 2 — appears at N0 (in front
/// of camera) and slides into the front slot while fading in.
///
/// Non-wrapping tiles: smooth OutQuad slide for position + alpha.
/// Scale stays at source value until FinishImmediate snaps to destination.
///
/// Queue-based cooldown: at most 1 pending cycle is queued.
class CycleAnimator {
public:
    bool IsActive() const { return m_active; }
    bool IsChained() const { return m_chained; }
    bool IsForward() const { return m_forward; }
    /// Carousel mode (Cover Flow) has no cascade-style wrap phases — the
    /// centre ↔ inner-left journey is a plain slide of one window — so
    /// the cascade's phase-1 texture selection must never trigger there.
    bool IsInWrapPhase1() const {
        return m_active && !m_carousel && m_rawT < kWrapFadeSplit;
    }

    /// True while slot `i` is playing the carousel SIDE SWAP: its window
    /// leaves one end of the row and a DIFFERENT window arrives at the
    /// opposite end (outermost-left ↔ outermost-right).  Detected from a
    /// rotY sign flip between the slot's source and destination poses, so
    /// it is structurally impossible in the cascade preset (every cascade
    /// slot's rotY is 0).  Phase 1 fades the departing window out at the
    /// source pose, phase 2 fades the arriving one in at the destination;
    /// the controller swaps the texture at the α=0 boundary.
    bool IsSideSwapSlot(uint32_t i) const;
    bool IsInSideSwapPhase1() const {
        return m_active && m_rawT < kWrapFadeSplit;
    }
    uint32_t SlotCount() const { return static_cast<uint32_t>(m_startSlots.size()); }

    /// `durationMsOverride` > 0 replaces the built-in timing for this one
    /// transition — used by the click-to-select spin, which has to cross a
    /// known number of windows in a fixed, short span however many they are.
    /// 0 (the default) keeps the normal/chained durations untouched.
    void Begin(FlipScene& scene, bool forward, bool chained = false,
               float durationMsOverride = 0.0f);
    void SetTarget(const FlipScene& scene);
    void Tick(FlipScene& scene);
    void Cancel() {
        m_active = false; m_justDone = false;
        m_scrub = false; m_settling = false; m_settledToStart = false;
        m_settleM0 = 0.0f; m_durationOverrideMs = 0.0f;
        // The chain state describes an animation that is now gone.  Leaving it
        // behind was a real bug, not a tidiness point: Begin() was the only
        // thing that ever cleared it, so a cancelled chained cycle (the last
        // step of a click-to-select spin, cancelled by the teardown that the
        // spin's own commit starts) carried "chained" into the next session.
        // Anything reading it as "the stack is streaming past" then believed
        // so with the stack standing perfectly still — which is exactly how
        // the selected-window label came up invisible until the first
        // keyboard cycle.  m_chainCount goes with it: a fresh chain must not
        // inherit a previous session's acceleration either.
        m_chained = false; m_chainCount = 0;
    }

    // ---- Free scrub (config windowSnap = false) ---------------------------
    // The transition is the same one Begin() sets up; the difference is who
    // drives its parameter.  In scrub mode the CALLER does — one pointer
    // delta at a time — so the stack rides the finger and can sit anywhere
    // between two windows.  Releasing hands it back to the clock, which
    // eases the remaining distance to whichever end is nearer.  Nothing in
    // the timed path changes: with Window snap on none of this is reached.
    void BeginScrub(FlipScene& scene, bool forward);
    void SetScrubT(FlipScene& scene, float t);
    /// Release: run from the current t to `target` (0 = abandon the step,
    /// 1 = complete it).
    ///
    /// `entryVel` is the speed the stack is ALREADY travelling at, in steps
    /// per second along the step's own direction (positive = toward 1).  It
    /// matters because the settle is not always a release from rest: a thrown
    /// stack hands over while it is still moving, and an ease that starts from
    /// zero velocity there is exactly the "the last window snaps back into
    /// place" jolt.  With a velocity supplied the settle runs a Hermite whose
    /// starting slope MATCHES the incoming motion and whose ending slope is
    /// zero, so the stack decelerates into the window instead of jumping to a
    /// new curve.  It still lands squarely on a whole window — only the
    /// approach changed.
    void BeginSettle(float target, float entryVel = 0.0f);
    bool IsScrubbing() const { return m_scrub; }
    /// True once after a settle that landed back on the START pose — the
    /// controller then has to undo the array rotation Begin() already did.
    bool JustSettledToStart();
    /// Switch the current animation from chained (Linear) to non-chained
    /// (OutCubic) for a smooth deceleration stop. No-op if not active.
    void SwitchToDecel();

    bool JustFinished();
    float GetRawT() const { return m_rawT; }
    float GetMotionIntensity() const;

    /// Read-only destination pose of a slot — the rest pose this cycle is
    /// animating toward (captured by SetTarget after the array rotation).
    /// nullptr when idle or out of range.  Used by the selected-window
    /// label to aim at the final position instead of riding the wrap
    /// tile's journey (which starts at the BACK of the cascade on
    /// backward cycles).
    const TileSlot* GetTargetSlot(uint32_t i) const {
        return (m_active && i < m_targetSlots.size()) ? &m_targetSlots[i]
                                                      : nullptr;
    }

private:
    static constexpr float kDurationMs       = 220.0f;   // normal animation length
    static constexpr float kChainDurationMs  = 170.0f;   // first chained animation length
    static constexpr float kMinChainMs       = 120.0f;   // fastest chained animation (held key)
    static constexpr float kChainAccelMs     = 10.0f;    // ms reduction per consecutive chain
    static constexpr float kWrapFadeSplit = 0.40f;     // 40% swing-out, 60% fade-in
    static constexpr float kN0Fwd        = 0.15f;     // virtual slot extrapolation factor
    static constexpr float kBackSpawn    = 0.50f;     // back-spawn: 50% past last slot
    static constexpr float kScaleBoost   = 1.02f;     // departing tile scale multiplier

    bool     m_active   = false;
    bool     m_forward  = true;
    bool     m_chained  = false;
    // Cover Flow carousel (latched from the scene's visual preset in
    // Begin): the row is a flat circular conveyor, so the "wrap" tile is
    // a SHORT sideways slide (centre ↔ inner-left neighbour) handled by
    // the plain lerp branch.  The cascade's N0/backSpawn fade phases are
    // skipped — their extrapolated poses drift vertically under Cover
    // Flow's bottom-aligned tiles (the "front tile dips down, then pops
    // in slightly above" artefact) and break the conveyor feel on held-
    // key chained cycling.
    bool     m_carousel = false;
    bool     m_justDone = false;
    float    m_rawT     = 0.0f;
    int64_t  m_startQPC = 0;
    int64_t  m_qpcFreq  = 0;
    int      m_chainCount = 0;   // consecutive chained animations (for acceleration)

    // Free-scrub state (see BeginScrub).  m_settling briefly hands the
    // parameter back to the clock so the release eases instead of snapping.
    static constexpr float kSettleMs = 190.0f;   // for a full 0 → 1 run
    // Bounds on a velocity-matched settle: long enough that a crawling stack
    // does not stop dead, short enough that a fast one does not float.
    static constexpr double kSettleMinMs = 90.0;
    static constexpr double kSettleMaxMs = 280.0;
    // Cap on the Hermite's starting slope.  Above ~1.8 the curve overshoots
    // past the destination window and comes back, which is the one thing a
    // settle must never do.
    static constexpr float kSettleMaxSlope = 1.8f;
    bool     m_scrub          = false;
    bool     m_settling       = false;
    bool     m_settledToStart = false;
    float    m_settleFrom     = 0.0f;
    float    m_settleTo       = 1.0f;
    double   m_settleMs       = kSettleMs;
    // Hermite tangent at p = 0, in normalised settle units.  0 = released
    // from rest, and the settle keeps the plain OutCubic it always had.
    float    m_settleM0       = 0.0f;
    float    m_durationOverrideMs = 0.0f;

    /// Write the pose at raw t into the scene.  Shared by the timed path and
    /// the scrub path — one interpolation, one place.
    void ApplyPose(FlipScene& scene, float rawT);

    std::vector<TileSlot> m_startSlots;
    std::vector<TileSlot> m_targetSlots;
    TileSlot m_n0Slot;       // forward: virtual slot in front of N1
    TileSlot m_backSpawn;    // backward: spawn behind last visible slot

    void FinishImmediate(FlipScene& scene);
    void ComputeN0();
    void ComputeBackSpawn();
    /// Which START slot feeds destination slot `i` (the array rotation's
    /// inverse): forward pulls from i+1, backward from i-1, both wrapping.
    uint32_t SourceSlot(uint32_t i) const;
};
