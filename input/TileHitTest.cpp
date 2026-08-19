// ---------------------------------------------------------------------------
// The hit test itself: project each slot's unit quad into overlay pixels, then
// walk the slots from front to back and return the first one the pointer is
// inside.  Same matrices as the draw pass, so the answer always matches what
// the user can actually see.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "TileHitTest.hpp"
#include <cmath>

using namespace DirectX;

namespace TileHitTest {
namespace {

// The unit quad QuadRenderer draws, corner for corner (see kQuadVertices).
constexpr float kCornerX[4] = { -0.5f, -0.5f, +0.5f, +0.5f };
constexpr float kCornerY[4] = { -0.5f, +0.5f, +0.5f, -0.5f };

} // namespace

bool ProjectQuad(FXMMATRIX mvp, float vpW, float vpH, Quad& out)
{
    if (vpW <= 0.0f || vpH <= 0.0f)
        return false;

    for (int i = 0; i < 4; ++i) {
        const XMVECTOR corner = XMVectorSet(kCornerX[i], kCornerY[i], 0.0f, 1.0f);
        // Clip-space first: the w component is what tells us the corner is in
        // front of the camera.  XMVector3TransformCoord would divide by it
        // unconditionally and hand back a plausible-looking point for a corner
        // that is actually behind the eye.
        const XMVECTOR clip = XMVector4Transform(corner, mvp);
        const float w = XMVectorGetW(clip);
        if (!(w > 1e-4f) || !std::isfinite(w))
            return false;

        const float ndcX = XMVectorGetX(clip) / w;
        const float ndcY = XMVectorGetY(clip) / w;
        if (!std::isfinite(ndcX) || !std::isfinite(ndcY))
            return false;

        out.x[i] = (ndcX * 0.5f + 0.5f) * vpW;
        out.y[i] = (0.5f - ndcY * 0.5f) * vpH;
    }
    return true;
}

bool PointInQuad(const Quad& q, float px, float py)
{
    bool anyPositive = false;
    bool anyNegative = false;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const float ex = q.x[j] - q.x[i];
        const float ey = q.y[j] - q.y[i];
        const float cross = ex * (py - q.y[i]) - ey * (px - q.x[i]);
        if (cross > 0.0f) anyPositive = true;
        if (cross < 0.0f) anyNegative = true;
        if (anyPositive && anyNegative)
            return false;   // the point falls outside this edge
    }
    return true;
}

int PickSlot(const FlipScene& scene, FXMMATRIX monRemap,
             float cascadeAspect, float vpW, float vpH,
             float px, float py, const SlotOffsets& drawOffsets,
             float minAlpha)
{
    const uint32_t count = scene.SlotCount();
    int   best  = -1;
    float bestZ = 0.0f;

    for (uint32_t i = 0; i < count; ++i) {
        const TileSlot& slot = scene.GetSlot(i);
        if (slot.alpha < minAlpha)
            continue;

        // Offset the DRAW pass is applying to this tile (the hover lift).
        const float liftY = (drawOffsets.y != nullptr && i < drawOffsets.count)
            ? drawOffsets.y[i] : 0.0f;

        auto covers = [&](float offsetY) {
            XMFLOAT4X4 mvpStore{};
            float alpha = 0.0f;
            scene.GetDrawCall(i, cascadeAspect, mvpStore, alpha, offsetY);
            if (alpha < minAlpha)
                return false;
            Quad quad;
            if (!ProjectQuad(XMLoadFloat4x4(&mvpStore) * monRemap, vpW, vpH, quad))
                return false;
            return PointInQuad(quad, px, py);
        };

        // The UNION of the resting pose and the lifted one.
        //
        // Testing only the lifted pose would close the loop the lift is
        // deliberately outside of: the lift would decide the hit, the hit would
        // decide the hover target, and the target would decide the lift.  A
        // pointer parked in the band a rising tile vacates would then lose the
        // tile, drop the lift, catch it again and flicker at the hover
        // frequency.  Testing only the resting pose is what this fixes — the
        // top of a lifted tile answered for whatever stood behind it.
        //
        // The union has neither problem: it does not move as the lift animates,
        // so there is nothing to oscillate, and it is a SUPERSET of the region
        // that responded before, so no click that used to land stops landing.
        // The first test is the resting pose precisely so the common
        // nothing-lifted frame costs exactly what it always did.
        if (!covers(0.0f) && !(liftY != 0.0f && covers(liftY)))
            continue;

        // Nearest wins: slot Z grows toward the back of the cascade, so the
        // smallest Z is the tile painted last and therefore the visible one.
        if (best < 0 || slot.z < bestZ) {
            best  = static_cast<int>(i);
            bestZ = slot.z;
        }
    }
    return best;
}

} // namespace TileHitTest
