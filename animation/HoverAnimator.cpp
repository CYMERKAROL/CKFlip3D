// ---------------------------------------------------------------------------
// Per-slot hover lift: the exponential rise toward the hovered tile, the timed
// fall that runs when the stack starts moving, and the rotation that keeps a
// fall attached to the window it started on.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#define NOMINMAX
#include <Windows.h>
#include "HoverAnimator.h"
#include <algorithm>
#include <cmath>

void HoverAnimator::Reset()
{
    m_lift.clear();
    m_dropFrom.clear();
    m_target   = -1;
    m_anyLift  = false;
    m_dropping = false;
    m_dropQPC  = 0;
    m_lastQPC  = 0;
    m_qpcFreq  = 0;
}

void HoverAnimator::EnsureFreq()
{
    if (m_qpcFreq == 0) {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        m_qpcFreq = freq.QuadPart;
    }
}

void HoverAnimator::SetTarget(int slot)
{
    m_target = slot;
}

void HoverAnimator::BeginDrop(bool animate)
{
    // Nothing off the floor — nothing to bring down.  This is the branch every
    // session with the pointer feature off takes.
    if (!m_anyLift) {
        m_dropping = false;
        return;
    }
    // Hover animation switched off (Appearance → Animations → Hover lift):
    // the lift snaps, so the fall does too.
    if (!animate) {
        std::fill(m_lift.begin(), m_lift.end(), 0.0f);
        m_anyLift  = false;
        m_dropping = false;
        m_target   = -1;
        return;
    }
    if (m_dropping)
        return;                 // already on its way down

    EnsureFreq();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    m_dropFrom = m_lift;
    m_dropQPC  = now.QuadPart;
    m_dropping = true;
    m_target   = -1;            // and it must not be re-raised mid-fall
}

void HoverAnimator::Rotate(bool forward)
{
    auto rotate = [forward](std::vector<float>& v) {
        if (v.size() < 2)
            return;
        if (forward) std::rotate(v.begin(),  v.begin()  + 1, v.end());
        else         std::rotate(v.rbegin(), v.rbegin() + 1, v.rend());
    };
    rotate(m_lift);
    rotate(m_dropFrom);
    // The pointer has not moved, but the slot it is over now holds a different
    // window, so a target carried across the rotation would mean the wrong
    // tile.  The per-frame hit test re-answers it as soon as the stack settles.
    m_target = -1;
}

void HoverAnimator::Tick(uint32_t slotCount, bool animate)
{
    if (m_lift.size() != slotCount) {
        // A rebuilt scene invalidates the mapping wholesale — the same slot
        // index now belongs to a different window, so carrying the old lift
        // over would highlight the wrong tile.  Start flat.
        m_lift.assign(slotCount, 0.0f);
        m_anyLift = false;
        // The fall was aimed at tiles that no longer exist; the flat start it
        // was heading for has already been reached by other means.
        m_dropping = false;
        m_dropFrom.clear();
    }
    if (slotCount == 0) {
        m_anyLift  = false;
        m_dropping = false;
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    EnsureFreq();

    float dt = 0.0f;
    if (m_lastQPC != 0 && m_qpcFreq > 0) {
        dt = static_cast<float>(
            static_cast<double>(now.QuadPart - m_lastQPC)
            / static_cast<double>(m_qpcFreq));
    }
    m_lastQPC = now.QuadPart;
    // A stall (the render loop blocked on a capture) must not teleport the
    // lift; a negative dt is a QPC oddity and simply holds the pose.
    dt = std::clamp(dt, 0.0f, 0.10f);

    // ---- Timed fall (see BeginDrop) ---------------------------------------
    // Runs to its own clock rather than the frame-to-frame dt, so a stalled
    // frame cannot stretch the fall past the transition it belongs to.
    if (m_dropping) {
        float t = 1.0f;
        if (m_qpcFreq > 0 && kDropSec > 0.0f) {
            const double elapsed =
                static_cast<double>(now.QuadPart - m_dropQPC)
                / static_cast<double>(m_qpcFreq);
            t = static_cast<float>(elapsed / static_cast<double>(kDropSec));
        }
        t = std::clamp(t, 0.0f, 1.0f);
        // OutCubic: most of the distance early, so the tile is visibly back
        // down well before the wait is formally over.
        const float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        bool any = false;
        for (uint32_t i = 0; i < slotCount; ++i) {
            const float from = (i < m_dropFrom.size()) ? m_dropFrom[i] : 0.0f;
            float& l = m_lift[i];
            l = from * (1.0f - e);
            if (l < 0.002f) l = 0.0f;
            if (l > 0.001f) any = true;
        }
        m_anyLift = any;
        if (t >= 1.0f) {
            m_dropping = false;
            m_dropFrom.clear();
            m_anyLift = false;
        }
        return;
    }

    const float k = animate ? (1.0f - std::exp(-dt / kTauSec)) : 1.0f;

    bool any = false;
    for (uint32_t i = 0; i < slotCount; ++i) {
        const float want = (static_cast<int>(i) == m_target) ? 1.0f : 0.0f;
        float& l = m_lift[i];
        l += (want - l) * k;
        if (std::fabs(l - want) < 0.002f)
            l = want;               // settle exactly, so idle frames do no work
        if (l > 0.001f)
            any = true;
    }
    m_anyLift = any;
}
