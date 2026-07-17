#define NOMINMAX
#include <Windows.h>
#include "CloseAnimator.h"
#include <DirectXMath.h>
#include <algorithm>

// ---------------------------------------------------------------------------
void CloseAnimator::ComputeBackSpawn()
{
    // Spawn point for overflow refill: slightly past the last visible slot
    // (further into the cascade), exactly like CycleAnimator's back-spawn.
    uint32_t n = static_cast<uint32_t>(m_targetSlots.size());
    if (n == 0) {
        m_backSpawn = TileSlot{};
        m_backSpawn.alpha = 0.0f;
        return;
    }
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
void CloseAnimator::Begin(FlipScene& scene,
                          const std::vector<TileSlot>& startSlots,
                          const std::vector<uint32_t>& dyingSlotIndices,
                          const CameraPose& oldCam)
{
    // Multi-close merge: if a transition is already running, its dying
    // tiles are carried over so they CONTINUE from their current mid-fall
    // pose — alpha only ever decreases, so the fade can never replay from
    // full opacity (the "window pops back in and vanishes again" loop).
    // Only the drop distance not yet travelled is kept.  The merged run
    // restarts rawT, so N windows closed in a burst cost one fresh pass
    // from the merge point — not N stacked passes.  Order is preserved
    // (carried first, new appended) to stay parallel with
    // FlipController::m_closingCaptures, which appends the same way.
    std::vector<TileSlot> carriedStart;
    std::vector<float>    carriedDrop;
    if (m_active) {
        float fallP = std::min(m_rawT / kFallEnd, 1.0f);
        float fallE = Easing::InQuad(fallP);
        carriedStart = m_dyingSlots;              // current mid-fall poses
        carriedDrop.reserve(m_dyingDrop.size());
        for (float drop : m_dyingDrop)
            carriedDrop.push_back(drop * (1.0f - fallE));   // remaining travel
    }

    // Hard reset — no state from a previous run may leak into this one
    // (the CycleAnimator cross-session leak taught us this the hard way).
    Cancel();
    m_dyingStart = std::move(carriedStart);
    m_dyingDrop  = std::move(carriedDrop);

    const uint32_t newCount = scene.SlotCount();
    m_targetSlots.resize(newCount);
    for (uint32_t i = 0; i < newCount; ++i)
        m_targetSlots[i] = scene.GetSlot(i);

    // Camera re-frame compensation (see class comment): BuildSlots just
    // re-derived the camera for the new count, but every start pose was
    // authored under the OLD camera.  Re-express them through
    // (oldView × newView⁻¹) so their view-space pose — and therefore
    // their on-screen projection (FOV and aspect are count-independent) —
    // is preserved exactly on frame 1.  View depth is preserved too, so
    // perspective scale and the relative Z-sort carry over unchanged.
    // When the camera didn't move (count above the framing range), the
    // product is the identity and this is a no-op.
    using namespace DirectX;
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX oldView = XMMatrixLookAtLH(
        XMVectorSet(oldCam.eyeX,    oldCam.eyeY,    oldCam.eyeZ,    1.0f),
        XMVectorSet(oldCam.targetX, oldCam.targetY, oldCam.targetZ, 1.0f),
        up);
    XMMATRIX newView = XMMatrixLookAtLH(
        XMVectorSet(scene.GetCamEyeX(),    scene.GetCamEyeY(),    scene.GetCamEyeZ(),    1.0f),
        XMVectorSet(scene.GetCamTargetX(), scene.GetCamTargetY(), scene.GetCamTargetZ(), 1.0f),
        up);
    XMMATRIX comp = oldView * XMMatrixInverse(nullptr, newView);
    auto compensate = [&comp](TileSlot& s) {
        XMVECTOR p = XMVector3Transform(XMVectorSet(s.x, s.y, s.z, 1.0f), comp);
        s.x = XMVectorGetX(p);
        s.y = XMVectorGetY(p);
        s.z = XMVectorGetZ(p);
    };

    // Carried dying poses live in the pre-rebuild camera frame too (the
    // camera only ever changes at BuildSlots) — compensate them the same.
    for (TileSlot& s : m_dyingStart)
        compensate(s);

    // Survivor mapping: old slot indices not closed, ascending.  Erase
    // preserves relative order, so NEW slot i shows the window that held
    // OLD slot survivors[i].  Any remaining new slots (overflow refill)
    // have no old pose and spawn in from the back instead.
    std::vector<uint32_t> survivors;
    survivors.reserve(startSlots.size());
    size_t d = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(startSlots.size()); ++i) {
        if (d < dyingSlotIndices.size() && dyingSlotIndices[d] == i) {
            ++d;
            continue;
        }
        survivors.push_back(i);
    }
    m_survivorCount = std::min(newCount,
                               static_cast<uint32_t>(survivors.size()));
    m_survivorStart.resize(m_survivorCount);
    for (uint32_t i = 0; i < m_survivorCount; ++i) {
        m_survivorStart[i] = startSlots[survivors[i]];
        compensate(m_survivorStart[i]);
    }

    // Dying tiles: camera-compensated pre-removal pose + a straight-down
    // slide (constant scale — the minimized-window read from the entry/
    // exit morph, minus the shrink).  Drop distance scales with the
    // tile's own world height, so it reads the same at any resolution.
    m_dyingStart.reserve(m_dyingStart.size() + dyingSlotIndices.size());
    m_dyingDrop.reserve(m_dyingDrop.size() + dyingSlotIndices.size());
    for (uint32_t idx : dyingSlotIndices) {
        if (idx >= startSlots.size())
            continue;   // defensive — caller filters to visible slots
        TileSlot src = startSlots[idx];
        compensate(src);
        m_dyingStart.push_back(src);
        m_dyingDrop.push_back(src.scaleY * kDropFactor);
    }
    m_dyingSlots = m_dyingStart;

    if (m_targetSlots.empty() && m_dyingStart.empty())
        return;   // nothing to animate — stay idle

    ComputeBackSpawn();

    // First-frame state: the scene currently holds the rebuilt TARGET
    // slots.  Write the start pose now so a delayed first Tick can never
    // present one frame of the snapped end state (survivors at their
    // compensated old pose, spawn-ins hidden at the back-spawn point).
    for (uint32_t i = 0; i < newCount; ++i) {
        TileSlot& slot = scene.GetSlotMut(i);
        if (i < m_survivorCount) {
            slot = m_survivorStart[i];
        } else {
            slot = m_targetSlots[i];
            slot.x     = m_backSpawn.x;
            slot.y     = m_backSpawn.y;
            slot.z     = m_backSpawn.z;
            slot.alpha = 0.0f;
        }
    }

    m_active   = true;
    m_justDone = false;
    m_rawT     = 0.0f;

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    m_qpcFreq  = freq.QuadPart;
    m_startQPC = now.QuadPart;
}

// ---------------------------------------------------------------------------
void CloseAnimator::FinishImmediate(FlipScene& scene)
{
    uint32_t n = scene.SlotCount();
    uint32_t count = std::min(n, static_cast<uint32_t>(m_targetSlots.size()));
    for (uint32_t i = 0; i < count; ++i)
        scene.GetSlotMut(i) = m_targetSlots[i];

    // Empty the dying tiles so no stale fade-out can draw after the end.
    m_dyingSlots.clear();
    m_dyingStart.clear();
    m_dyingDrop.clear();
    m_survivorStart.clear();
    m_survivorCount = 0;

    m_active   = false;
    m_rawT     = 1.0f;
    m_justDone = true;
}

// ---------------------------------------------------------------------------
void CloseAnimator::Cancel()
{
    m_active   = false;
    m_justDone = false;
    m_rawT     = 0.0f;
    m_survivorCount = 0;
    m_survivorStart.clear();
    m_targetSlots.clear();
    m_dyingStart.clear();
    m_dyingSlots.clear();
    m_dyingDrop.clear();
}

// ---------------------------------------------------------------------------
bool CloseAnimator::JustFinished()
{
    if (m_justDone) { m_justDone = false; return true; }
    return false;
}

// ---------------------------------------------------------------------------
float CloseAnimator::GetMotionIntensity() const
{
    if (!m_active) return 0.0f;
    // Bell over the reflow window: zero while the cascade holds still
    // during the pre-reflow fall, peaks mid-slide, zero again at landing.
    float p = 0.0f;
    if (m_rawT > kReflowStart)
        p = std::min((m_rawT - kReflowStart) / (1.0f - kReflowStart), 1.0f);
    return 4.0f * p * (1.0f - p);
}

// ---------------------------------------------------------------------------
void CloseAnimator::Tick(FlipScene& scene)
{
    if (!m_active)
        return;

    uint32_t n = scene.SlotCount();
    if (n == 0 || n != static_cast<uint32_t>(m_targetSlots.size())) {
        // Scene was rebuilt underneath us — its slots are the new truth.
        // Bail out without writing anything (stale world positions under
        // a re-derived camera are exactly the mid-morph removal bug the
        // old RemoveClosedWindows guard documents).
        Cancel();
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMs = static_cast<double>(now.QuadPart - m_startQPC) * 1000.0
                     / static_cast<double>(m_qpcFreq);
    m_rawT = static_cast<float>(elapsedMs / static_cast<double>(kDurationMs));
    m_rawT = std::min(std::max(m_rawT, 0.0f), 1.0f);

    // --- Cascade slots: hold, then reflow with the entry/exit-style ease.
    // reflowP stays 0 until kReflowStart (mid-fall), so survivors hold
    // their compensated start pose, then InOutCubic gives zero velocity
    // at BOTH ends — no jerk at the staggered start, no snap at landing.
    float reflowP = 0.0f;
    if (m_rawT > kReflowStart)
        reflowP = std::min((m_rawT - kReflowStart) / (1.0f - kReflowStart), 1.0f);
    float ec = Easing::InOutCubic(reflowP);

    for (uint32_t i = 0; i < n; ++i) {
        TileSlot& slot      = scene.GetSlotMut(i);
        const TileSlot& dst = m_targetSlots[i];

        if (i < m_survivorCount) {
            const TileSlot& src = m_survivorStart[i];
            slot.x      = src.x      + (dst.x      - src.x)      * ec;
            slot.y      = src.y      + (dst.y      - src.y)      * ec;
            slot.z      = src.z      + (dst.z      - src.z)      * ec;
            slot.scaleX = src.scaleX + (dst.scaleX - src.scaleX) * ec;
            slot.scaleY = src.scaleY + (dst.scaleY - src.scaleY) * ec;
            slot.alpha  = src.alpha  + (dst.alpha  - src.alpha)  * ec;
        } else if (reflowP <= 0.0f) {
            // Overflow refill, waiting phase: parked hidden at back-spawn
            // while the dying tile's fall is the eye's focus.
            slot        = dst;
            slot.x      = m_backSpawn.x;
            slot.y      = m_backSpawn.y;
            slot.z      = m_backSpawn.z;
            slot.alpha  = 0.0f;
        } else {
            // Overflow refill, entry phase: ride the reflow window from
            // back-spawn into the freed back slot while fading in — the
            // forward-cycle wrap phase 2 look.
            float ep = Easing::OutQuad(reflowP);
            slot.x      = m_backSpawn.x + (dst.x - m_backSpawn.x) * ep;
            slot.y      = m_backSpawn.y + (dst.y - m_backSpawn.y) * ep;
            slot.z      = m_backSpawn.z + (dst.z - m_backSpawn.z) * ep;
            slot.scaleX = dst.scaleX;
            slot.scaleY = dst.scaleY;
            slot.alpha  = dst.alpha * ep;
        }
    }

    // --- Dying tiles: accelerating straight-down slide at constant scale,
    // then a smooth alpha ramp that completes exactly when the fall does
    // (the tile never visibly stops mid-air).  Z fixed — Z-sort stability.
    float fallP = std::min(m_rawT / kFallEnd, 1.0f);
    float fallE = Easing::InQuad(fallP);   // gravity read: slow start, accelerating
    float fadeMul = 1.0f;
    if (m_rawT > kFadeStart) {
        float fadeP = std::min((m_rawT - kFadeStart) / (kFallEnd - kFadeStart), 1.0f);
        fadeMul = 1.0f - Easing::InOutQuad(fadeP);
    }
    for (size_t k = 0; k < m_dyingSlots.size(); ++k) {
        const TileSlot& src = m_dyingStart[k];
        TileSlot& dt = m_dyingSlots[k];
        dt.x      = src.x;
        dt.y      = src.y - m_dyingDrop[k] * fallE;
        dt.z      = src.z;   // keep Z fixed — prevents Z-sort overlap artifacts
        dt.scaleX = src.scaleX;   // constant scale — pure slide-down, no shrink
        dt.scaleY = src.scaleY;
        dt.alpha  = src.alpha * fadeMul;
    }

    if (m_rawT >= 1.0f)
        FinishImmediate(scene);
}
