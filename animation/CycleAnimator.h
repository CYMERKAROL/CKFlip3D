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
    bool IsInWrapPhase1() const { return m_active && m_rawT < kWrapFadeSplit; }
    uint32_t SlotCount() const { return static_cast<uint32_t>(m_startSlots.size()); }

    void Begin(FlipScene& scene, bool forward, bool chained = false);
    void SetTarget(const FlipScene& scene);
    void Tick(FlipScene& scene);
    void Cancel() { m_active = false; m_justDone = false; }
    /// Switch the current animation from chained (Linear) to non-chained
    /// (OutCubic) for a smooth deceleration stop. No-op if not active.
    void SwitchToDecel();

    bool JustFinished();
    float GetRawT() const { return m_rawT; }
    float GetMotionIntensity() const;

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
    bool     m_justDone = false;
    float    m_rawT     = 0.0f;
    int64_t  m_startQPC = 0;
    int64_t  m_qpcFreq  = 0;
    int      m_chainCount = 0;   // consecutive chained animations (for acceleration)

    std::vector<TileSlot> m_startSlots;
    std::vector<TileSlot> m_targetSlots;
    TileSlot m_n0Slot;       // forward: virtual slot in front of N1
    TileSlot m_backSpawn;    // backward: spawn behind last visible slot

    void FinishImmediate(FlipScene& scene);
    void ComputeN0();
    void ComputeBackSpawn();
};
