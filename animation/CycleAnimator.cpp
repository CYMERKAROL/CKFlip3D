// ---------------------------------------------------------------------------
// One step of the cascade, from the wrap tile's swing through the plain slides
// of everything else.  The same interpolation serves both the timed cycle and
// the free scrub, so the stack looks identical whether a clock or a finger is
// driving it.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#define NOMINMAX
#include <Windows.h>
#include "CycleAnimator.h"
#include <algorithm>
#include <cmath>

void CycleAnimator::FinishImmediate(FlipScene& scene)
{
    uint32_t n = scene.SlotCount();
    uint32_t count = std::min(n, static_cast<uint32_t>(m_targetSlots.size()));
    for (uint32_t i = 0; i < count; ++i)
        scene.GetSlotMut(i) = m_targetSlots[i];
    m_active   = false;
    m_rawT     = 1.0f;
    m_justDone = true;
    m_scrub    = false;
    m_settling = false;
}

void CycleAnimator::ComputeN0()
{
    // Virtual Slot 0: extrapolate from slot 0 AWAY from the cascade.
    // Direction: more Right (+X), slightly Down (-Y), closer to camera (−Z).
    if (m_startSlots.size() < 2) {
        m_n0Slot = m_startSlots[0];
        m_n0Slot.alpha = 0.0f;
        return;
    }
    const TileSlot& s0 = m_startSlots[0];   // front tile
    const TileSlot& s1 = m_startSlots[1];   // second tile
    float f = kN0Fwd;
    // Extrapolate position: opposite direction from the cascade
    m_n0Slot.x      = s0.x      + f * (s0.x      - s1.x);
    m_n0Slot.y      = s0.y      + f * (s0.y      - s1.y);
    m_n0Slot.z      = s0.z      + f * (s0.z      - s1.z);   // closer to camera
    m_n0Slot.scaleX = s0.scaleX * kScaleBoost;
    m_n0Slot.scaleY = s0.scaleY * kScaleBoost;
    m_n0Slot.rotY   = s0.rotY;
    m_n0Slot.alpha  = 0.0f;
}

void CycleAnimator::ComputeBackSpawn()
{
    // Spawn point for backward wrapping: slightly past the last visible slot
    // (further into the cascade — higher Z, more left, more up).
    uint32_t n = static_cast<uint32_t>(m_targetSlots.size());
    if (n < 2) {
        m_backSpawn = m_targetSlots[0];
        m_backSpawn.alpha = 0.0f;
        return;
    }
    const TileSlot& sLast = m_targetSlots[n - 1];
    const TileSlot& sPrev = m_targetSlots[n - 2];
    float f = kBackSpawn;
    m_backSpawn.x      = sLast.x      + f * (sLast.x      - sPrev.x);
    m_backSpawn.y      = sLast.y      + f * (sLast.y      - sPrev.y);
    m_backSpawn.z      = sLast.z      + f * (sLast.z      - sPrev.z);
    m_backSpawn.scaleX = sLast.scaleX;
    m_backSpawn.scaleY = sLast.scaleY;
    m_backSpawn.rotY   = sLast.rotY;
    m_backSpawn.alpha  = 0.0f;
}

void CycleAnimator::SwitchToDecel()
{
    if (!m_active || !m_chained)
        return;
    // Switch from Linear (constant velocity) to OutCubic (deceleration).
    // This makes the current animation ease to a stop instead of gliding.
    m_chained = false;
    m_chainCount = 0;
}

uint32_t CycleAnimator::SourceSlot(uint32_t i) const
{
    const uint32_t n = static_cast<uint32_t>(m_startSlots.size());
    if (n == 0) return 0;
    return m_forward ? (i + 1) % n : (i == 0 ? n - 1 : i - 1);
}

bool CycleAnimator::IsSideSwapSlot(uint32_t i) const
{
    if (!m_active || !m_carousel)
        return false;
    const uint32_t n = static_cast<uint32_t>(m_startSlots.size());
    if (i >= n || m_targetSlots.size() != n)
        return false;
    return m_startSlots[SourceSlot(i)].rotY * m_targetSlots[i].rotY < -0.001f;
}

void CycleAnimator::Begin(FlipScene& scene, bool forward, bool chained,
                          float durationMsOverride)
{
    if (m_active)
        FinishImmediate(scene);

    m_durationOverrideMs = (durationMsOverride > 0.0f) ? durationMsOverride
                                                       : 0.0f;

    uint32_t n = scene.SlotCount();
    m_startSlots.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_startSlots[i] = scene.GetSlot(i);

    m_forward  = forward;
    m_chained  = chained;
    m_carousel = (scene.GetVisualPreset() == VisualPreset::CoverFlow);
    m_active   = true;
    m_justDone = false;
    m_rawT     = 0.0f;
    m_scrub    = false;
    m_settling = false;
    m_settledToStart = false;

    // Track chain count for held-key acceleration.
    // Non-chained resets; chained increments.
    if (chained)
        ++m_chainCount;
    else
        m_chainCount = 0;

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    m_qpcFreq  = freq.QuadPart;
    m_startQPC = now.QuadPart;
}

void CycleAnimator::SetTarget(const FlipScene& scene)
{
    uint32_t n = scene.SlotCount();
    m_targetSlots.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_targetSlots[i] = scene.GetSlot(i);

    ComputeN0();
    ComputeBackSpawn();
}

bool CycleAnimator::JustFinished()
{
    if (m_justDone) { m_justDone = false; return true; }
    return false;
}

float CycleAnimator::GetMotionIntensity() const
{
    if (!m_active) return 0.0f;
    float u = 1.0f - m_rawT;
    return u * u;
}

// ---------------------------------------------------------------------------
// Free scrub — the caller owns the parameter (see the header).  BeginScrub
// reuses Begin()'s snapshot machinery wholesale and only swaps who advances
// m_rawT; chained=true selects the Linear inner easing so the stack tracks
// the pointer one-to-one instead of easing under it.
// ---------------------------------------------------------------------------
void CycleAnimator::BeginScrub(FlipScene& scene, bool forward)
{
    Begin(scene, forward, /*chained*/ true);
    m_chainCount = 0;   // no held-key acceleration: distance comes from the hand
    m_scrub      = true;
}

void CycleAnimator::SetScrubT(FlipScene& scene, float t)
{
    if (!m_active || !m_scrub)
        return;
    m_rawT = std::min(std::max(t, 0.0f), 1.0f);
    ApplyPose(scene, m_rawT);
}

void CycleAnimator::BeginSettle(float target, float entryVel)
{
    if (!m_active || !m_scrub)
        return;
    m_settleFrom = m_rawT;
    m_settleTo   = (target >= 0.5f) ? 1.0f : 0.0f;
    // Proportional to the distance left, so a nearly finished step snaps
    // shut and a barely started one still eases back.
    m_settleMs   = std::max(60.0, static_cast<double>(kSettleMs)
                                 * std::fabs(m_settleTo - m_settleFrom));
    m_settleM0   = 0.0f;

    // Handed over while still moving (a thrown stack coming to rest): pick a
    // duration the incoming speed would ITSELF take to cover the remaining
    // distance — slightly stretched, because the tail of a deceleration is
    // slower than its average — and hand the Hermite the matching slope.  The
    // stack then carries on at the speed it already had and eases to a stop.
    const float signedDist = m_settleTo - m_settleFrom;
    const float dist       = std::fabs(signedDist);
    const float towardGoal = (signedDist >= 0.0f) ? entryVel : -entryVel;
    if (dist > 0.001f && towardGoal > 0.01f) {
        const double natural = static_cast<double>(dist)
                             / static_cast<double>(towardGoal) * 1000.0 * 1.55;
        m_settleMs = std::clamp(natural, kSettleMinMs, kSettleMaxMs);
        // Slope in normalised units: how much of the remaining distance the
        // entry speed covers over the whole settle.
        m_settleM0 = std::min(kSettleMaxSlope,
            towardGoal * static_cast<float>(m_settleMs) * 0.001f / dist);
    }

    m_settling   = true;
    m_settledToStart = false;

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    m_qpcFreq  = freq.QuadPart;
    m_startQPC = now.QuadPart;
}

bool CycleAnimator::JustSettledToStart()
{
    if (m_settledToStart) { m_settledToStart = false; return true; }
    return false;
}

void CycleAnimator::Tick(FlipScene& scene)
{
    if (!m_active)
        return;

    uint32_t n = scene.SlotCount();
    if (n == 0 || m_startSlots.size() != n || m_targetSlots.size() != n) {
        m_active = false;
        m_scrub = false;
        m_settling = false;
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Scrub in progress: the pointer drives it, the clock does not.
    if (m_scrub && !m_settling)
        return;

    if (m_settling) {
        double elapsed = static_cast<double>(now.QuadPart - m_startQPC) * 1000.0
                       / static_cast<double>(m_qpcFreq);
        float p = static_cast<float>(elapsed / m_settleMs);
        p = std::min(std::max(p, 0.0f), 1.0f);
        // Cubic Hermite with h(0)=0, h(1)=1, h'(1)=0 and h'(0)=m0 — the
        // velocity-matched approach (see BeginSettle).  Released from rest
        // there is no velocity to match and the original OutCubic stands.
        const float e = (m_settleM0 > 0.001f)
            ? (m_settleM0 * (p * p * p - 2.0f * p * p + p)
               + (3.0f * p * p - 2.0f * p * p * p))
            : Easing::OutCubic(p);
        m_rawT = m_settleFrom + (m_settleTo - m_settleFrom) * e;
        ApplyPose(scene, m_rawT);
        if (p >= 1.0f) {
            if (m_settleTo >= 1.0f) {
                FinishImmediate(scene);      // step completed
            } else {
                // Back at the start pose: the controller undoes the rotation
                // Begin() performed.  Nothing moves — t = 0 already renders it.
                ApplyPose(scene, 0.0f);
                m_active   = false;
                m_scrub    = false;
                m_settling = false;
                m_settledToStart = true;
                m_rawT     = 0.0f;
            }
        }
        return;
    }
    // Chained duration decreases with consecutive chains (held-key acceleration).
    double duration;
    if (m_durationOverrideMs > 0.0f) {
        duration = static_cast<double>(m_durationOverrideMs);
    } else if (m_chained) {
        duration = static_cast<double>(kChainDurationMs - m_chainCount * kChainAccelMs);
        if (duration < static_cast<double>(kMinChainMs))
            duration = static_cast<double>(kMinChainMs);
    } else {
        duration = static_cast<double>(kDurationMs);
    }
    double elapsedMs = static_cast<double>(now.QuadPart - m_startQPC) * 1000.0
                     / static_cast<double>(m_qpcFreq);
    m_rawT = static_cast<float>(elapsedMs / duration);
    m_rawT = std::min(std::max(m_rawT, 0.0f), 1.0f);

    ApplyPose(scene, m_rawT);

    if (m_rawT >= 1.0f) {
        FinishImmediate(scene);
    }
}

// ---------------------------------------------------------------------------
// The transition itself, as a pure function of the raw parameter.  Extracted
// from Tick so the scrub path can drive the very same interpolation from a
// pointer instead of a clock — there is exactly one description of how a
// cycle looks.
// ---------------------------------------------------------------------------
void CycleAnimator::ApplyPose(FlipScene& scene, float rawT)
{
    const uint32_t n = scene.SlotCount();
    if (n == 0 || m_startSlots.size() != n || m_targetSlots.size() != n)
        return;

    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t srcIdx = SourceSlot(i);

        const TileSlot& src = m_startSlots[srcIdx];
        const TileSlot& dst = m_targetSlots[i];
        TileSlot& slot      = scene.GetSlotMut(i);

        // Carousel (Cover Flow): no cascade wrap phases.  The slot the
        // cascade treats as "wrapping" carries the old centre window to
        // the inner-left position — a short sideways slide of ONE window,
        // handled by the plain lerp branch.  The genuine discontinuity in
        // a carousel is the SIDE SWAP at the row's ends, detected below
        // by the rotY sign flip.  See m_carousel.
        bool isWrapping = !m_carousel &&
                          ((m_forward && i == n - 1) ||
                           (!m_forward && i == 0));

        // Both chained (held-key) and non-chained animations get the full
        // N0 push-forward + backSpawn fade-in wrap effect.  For chained,
        // the shorter duration makes the effect quick but still visible.

        if (isWrapping && m_forward) {
            // ---- Forward wrap: swing toward N0 then slide from backSpawn ----
            if (rawT < kWrapFadeSplit) {
                // Phase 1: swing toward camera (N0) + fade out.
                // Z stays at source to prevent Z-sort overlap with tiles behind
                // (otherwise the fading tile draws ON TOP and other tiles bleed through).
                float p = rawT / kWrapFadeSplit;
                slot.x      = src.x + (m_n0Slot.x - src.x) * p;
                slot.y      = src.y + (m_n0Slot.y - src.y) * p;
                slot.z      = src.z;   // keep Z fixed — prevents overlap artifacts
                float sBump = 1.0f + (kScaleBoost - 1.0f) * 4.0f * p * (1.0f - p);
                slot.scaleX = src.scaleX * sBump;
                slot.scaleY = src.scaleY * sBump;
                slot.rotY   = src.rotY;
                slot.alpha  = src.alpha * (1.0f - Easing::OutQuad(p));
            } else {
                // Phase 2: appear at backSpawn, slide into back slot
                float p = (rawT - kWrapFadeSplit) / (1.0f - kWrapFadeSplit);
                slot.x      = m_backSpawn.x + (dst.x - m_backSpawn.x) * p;
                slot.y      = m_backSpawn.y + (dst.y - m_backSpawn.y) * p;
                slot.z      = m_backSpawn.z + (dst.z - m_backSpawn.z) * p;
                slot.scaleX = dst.scaleX;
                slot.scaleY = dst.scaleY;
                slot.rotY   = dst.rotY;
                slot.alpha  = dst.alpha * Easing::OutQuad(p);
            }
        } else if (isWrapping && !m_forward) {
            // ---- Backward wrap: slide toward backSpawn then slide from N0 ----
            if (rawT < kWrapFadeSplit) {
                // Phase 1: slide deeper toward backSpawn + fade out.
                // Z stays at source to prevent Z-sort overlap artifacts.
                float p = rawT / kWrapFadeSplit;
                slot.x      = src.x + (m_backSpawn.x - src.x) * p;
                slot.y      = src.y + (m_backSpawn.y - src.y) * p;
                slot.z      = src.z;   // keep Z fixed
                slot.scaleX = src.scaleX;
                slot.scaleY = src.scaleY;
                slot.rotY   = src.rotY;
                slot.alpha  = src.alpha * (1.0f - Easing::OutQuad(p));
            } else {
                // Phase 2: appear at N0 (in front of camera), slide into front slot.
                // Z stays at destination to prevent overlap during fade-in.
                float p = (rawT - kWrapFadeSplit) / (1.0f - kWrapFadeSplit);
                slot.x      = m_n0Slot.x + (dst.x - m_n0Slot.x) * p;
                slot.y      = m_n0Slot.y + (dst.y - m_n0Slot.y) * p;
                slot.z      = dst.z;   // keep Z at destination
                float sBump = 1.0f + (kScaleBoost - 1.0f) * 4.0f * (1.0f - p) * p;
                slot.scaleX = dst.scaleX * sBump;
                slot.scaleY = dst.scaleY * sBump;
                slot.rotY   = dst.rotY;
                slot.alpha  = dst.alpha * Easing::InQuad(p);
            }
        } else if (IsSideSwapSlot(i)) {
            // ---- Cover Flow side swap ----
            // A rotY sign flip between source and destination means this
            // slot spans the two ENDS of the carousel row.  With windows
            // to spare that is a genuine hand-over: the window leaving
            // the outermost-left position drops out of the visible set
            // while a different one surfaces at the outermost-right (with
            // no spare windows it is the same window crossing over).
            // Either way a positional lerp would drag a tile across the
            // whole screen behind the deck — fade out at the source pose,
            // fade in at the destination instead.  The controller swaps
            // the texture at the α=0 boundary (IsInSideSwapPhase1), so
            // the departing and arriving windows never show each other's
            // frames.  Unreachable in the cascade preset, where every
            // slot's rotY is 0.
            if (rawT < kWrapFadeSplit) {
                float p = rawT / kWrapFadeSplit;
                slot       = src;
                slot.alpha = src.alpha * (1.0f - Easing::OutQuad(p));
            } else {
                float p = (rawT - kWrapFadeSplit) / (1.0f - kWrapFadeSplit);
                slot       = dst;
                slot.alpha = dst.alpha * Easing::OutQuad(p);
            }
        } else {
            // ---- Non-wrapping: smooth slide + alpha + scale ----
            // Chained (held key) uses Linear for constant velocity across
            // blend boundaries; non-chained uses OutCubic for precise landing.
            float ec = m_chained ? Easing::Linear(rawT)
                                : Easing::OutCubic(rawT);
            slot.x      = src.x      + (dst.x      - src.x)      * ec;
            slot.y      = src.y      + (dst.y      - src.y)      * ec;
            slot.z      = src.z      + (dst.z      - src.z)      * ec;
            slot.scaleX = src.scaleX + (dst.scaleX - src.scaleX) * ec;
            slot.scaleY = src.scaleY + (dst.scaleY - src.scaleY) * ec;
            slot.rotY   = src.rotY   + (dst.rotY   - src.rotY)   * ec;
            slot.alpha  = src.alpha  + (dst.alpha  - src.alpha)  * ec;
        }
    }
}
