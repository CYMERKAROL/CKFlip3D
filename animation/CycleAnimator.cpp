#define NOMINMAX
#include <Windows.h>
#include "CycleAnimator.h"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
void CycleAnimator::FinishImmediate(FlipScene& scene)
{
    uint32_t n = scene.SlotCount();
    uint32_t count = std::min(n, static_cast<uint32_t>(m_targetSlots.size()));
    for (uint32_t i = 0; i < count; ++i)
        scene.GetSlotMut(i) = m_targetSlots[i];
    m_active   = false;
    m_rawT     = 1.0f;
    m_justDone = true;
}

// ---------------------------------------------------------------------------
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
    m_n0Slot.alpha  = 0.0f;
}

// ---------------------------------------------------------------------------
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
    m_backSpawn.alpha  = 0.0f;
}

// ---------------------------------------------------------------------------
void CycleAnimator::SwitchToDecel()
{
    if (!m_active || !m_chained)
        return;
    // Switch from Linear (constant velocity) to OutCubic (deceleration).
    // This makes the current animation ease to a stop instead of gliding.
    m_chained = false;
    m_chainCount = 0;
}

// ---------------------------------------------------------------------------
void CycleAnimator::Begin(FlipScene& scene, bool forward, bool chained)
{
    if (m_active)
        FinishImmediate(scene);

    uint32_t n = scene.SlotCount();
    m_startSlots.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_startSlots[i] = scene.GetSlot(i);

    m_forward  = forward;
    m_chained  = chained;
    m_active   = true;
    m_justDone = false;
    m_rawT     = 0.0f;

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

// ---------------------------------------------------------------------------
void CycleAnimator::SetTarget(const FlipScene& scene)
{
    uint32_t n = scene.SlotCount();
    m_targetSlots.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_targetSlots[i] = scene.GetSlot(i);

    ComputeN0();
    ComputeBackSpawn();
}

// ---------------------------------------------------------------------------
bool CycleAnimator::JustFinished()
{
    if (m_justDone) { m_justDone = false; return true; }
    return false;
}

// ---------------------------------------------------------------------------
float CycleAnimator::GetMotionIntensity() const
{
    if (!m_active) return 0.0f;
    float u = 1.0f - m_rawT;
    return u * u;
}

// ---------------------------------------------------------------------------
void CycleAnimator::Tick(FlipScene& scene)
{
    if (!m_active)
        return;

    uint32_t n = scene.SlotCount();
    if (n == 0 || m_startSlots.size() != n || m_targetSlots.size() != n) {
        m_active = false;
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Chained duration decreases with consecutive chains (held-key acceleration).
    double duration;
    if (m_chained) {
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

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t srcIdx;
        if (m_forward)
            srcIdx = (i + 1) % n;
        else
            srcIdx = (i == 0) ? (n - 1) : (i - 1);

        const TileSlot& src = m_startSlots[srcIdx];
        const TileSlot& dst = m_targetSlots[i];
        TileSlot& slot      = scene.GetSlotMut(i);

        bool isWrapping = (m_forward && i == n - 1) ||
                          (!m_forward && i == 0);

        // Both chained (held-key) and non-chained animations get the full
        // N0 push-forward + backSpawn fade-in wrap effect.  For chained,
        // the shorter duration makes the effect quick but still visible.

        if (isWrapping && m_forward) {
            // ---- Forward wrap: swing toward N0 then slide from backSpawn ----
            if (m_rawT < kWrapFadeSplit) {
                // Phase 1: swing toward camera (N0) + fade out.
                // Z stays at source to prevent Z-sort overlap with tiles behind
                // (otherwise the fading tile draws ON TOP and other tiles bleed through).
                float p = m_rawT / kWrapFadeSplit;
                slot.x      = src.x + (m_n0Slot.x - src.x) * p;
                slot.y      = src.y + (m_n0Slot.y - src.y) * p;
                slot.z      = src.z;   // keep Z fixed — prevents overlap artifacts
                float sBump = 1.0f + (kScaleBoost - 1.0f) * 4.0f * p * (1.0f - p);
                slot.scaleX = src.scaleX * sBump;
                slot.scaleY = src.scaleY * sBump;
                slot.alpha  = src.alpha * (1.0f - Easing::OutQuad(p));
            } else {
                // Phase 2: appear at backSpawn, slide into back slot
                float p = (m_rawT - kWrapFadeSplit) / (1.0f - kWrapFadeSplit);
                slot.x      = m_backSpawn.x + (dst.x - m_backSpawn.x) * p;
                slot.y      = m_backSpawn.y + (dst.y - m_backSpawn.y) * p;
                slot.z      = m_backSpawn.z + (dst.z - m_backSpawn.z) * p;
                slot.scaleX = dst.scaleX;
                slot.scaleY = dst.scaleY;
                slot.alpha  = dst.alpha * Easing::OutQuad(p);
            }
        } else if (isWrapping && !m_forward) {
            // ---- Backward wrap: slide toward backSpawn then slide from N0 ----
            if (m_rawT < kWrapFadeSplit) {
                // Phase 1: slide deeper toward backSpawn + fade out.
                // Z stays at source to prevent Z-sort overlap artifacts.
                float p = m_rawT / kWrapFadeSplit;
                slot.x      = src.x + (m_backSpawn.x - src.x) * p;
                slot.y      = src.y + (m_backSpawn.y - src.y) * p;
                slot.z      = src.z;   // keep Z fixed
                slot.scaleX = src.scaleX;
                slot.scaleY = src.scaleY;
                slot.alpha  = src.alpha * (1.0f - Easing::OutQuad(p));
            } else {
                // Phase 2: appear at N0 (in front of camera), slide into front slot.
                // Z stays at destination to prevent overlap during fade-in.
                float p = (m_rawT - kWrapFadeSplit) / (1.0f - kWrapFadeSplit);
                slot.x      = m_n0Slot.x + (dst.x - m_n0Slot.x) * p;
                slot.y      = m_n0Slot.y + (dst.y - m_n0Slot.y) * p;
                slot.z      = dst.z;   // keep Z at destination
                float sBump = 1.0f + (kScaleBoost - 1.0f) * 4.0f * (1.0f - p) * p;
                slot.scaleX = dst.scaleX * sBump;
                slot.scaleY = dst.scaleY * sBump;
                slot.alpha  = dst.alpha * Easing::InQuad(p);
            }
        } else {
            // ---- Non-wrapping: smooth slide + alpha + scale ----
            // Chained (held key) uses Linear for constant velocity across
            // blend boundaries; non-chained uses OutCubic for precise landing.
            float ec = m_chained ? Easing::Linear(m_rawT)
                                : Easing::OutCubic(m_rawT);
            slot.x      = src.x      + (dst.x      - src.x)      * ec;
            slot.y      = src.y      + (dst.y      - src.y)      * ec;
            slot.z      = src.z      + (dst.z      - src.z)      * ec;
            slot.scaleX = src.scaleX + (dst.scaleX - src.scaleX) * ec;
            slot.scaleY = src.scaleY + (dst.scaleY - src.scaleY) * ec;
            slot.alpha  = src.alpha  + (dst.alpha  - src.alpha)  * ec;
        }
    }

    if (m_rawT >= 1.0f) {
        FinishImmediate(scene);
    }
}
