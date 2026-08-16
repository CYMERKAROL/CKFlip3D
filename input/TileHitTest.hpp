#pragma once

#define NOMINMAX
#include <DirectXMath.h>
#include <cstdint>
#include "../scene/FlipScene.hpp"

// ---------------------------------------------------------------------------
// Pointer → cascade tile.
//
// The cascade is drawn by projecting a UNIT QUAD through each slot's MVP
// (FlipScene::GetDrawCall) — so "which window is under the pointer" is that
// same chain read backwards: project the four corners into overlay pixels and
// test the pointer against the resulting convex quad.  Nothing here recomputes
// geometry, and nothing here mutates anything: the scene is the sole truth,
// exactly as it is for the draw pass, which is precisely why hover and click
// can never disagree with what is on screen.
//
// Overlay pixel space throughout (screen coordinates minus the virtual-screen
// origin) — the same space RenderFrame's viewport uses.
// ---------------------------------------------------------------------------
namespace TileHitTest {

/// A projected unit quad: four screen-space corners in the quad's own winding
/// order (bottom-left, top-left, top-right, bottom-right).
struct Quad {
    float x[4]{};
    float y[4]{};
};

/// Project the unit quad through `mvp` into overlay pixels.  False when any
/// corner lands behind the camera or projects to a non-finite value, which is
/// the caller's cue to treat the tile as un-hittable rather than to guess.
bool ProjectQuad(DirectX::FXMMATRIX mvp, float vpW, float vpH, Quad& out);

/// Convex point-in-quad test (consistent sign of the cross product against
/// every edge).  Winding-agnostic, so a mirrored or back-facing tile — a
/// Cover Flow tile turned past 90° — still tests correctly.
bool PointInQuad(const Quad& q, float px, float py);

/// Per-slot world-space Y offset the DRAW pass is applying on top of the slot
/// pose — today that is the hover lift (see HoverAnimator, which owns no slot
/// state and therefore never reaches FlipScene).  Without it the hit test
/// projects tiles where they would be if nothing were lifted, and a lifted tile
/// is drawn up to kRiseFactor of its own height away from the area that
/// actually responds to a click.
///
/// `nullptr` (the default) means "no offsets", which is the exact pre-hover
/// behaviour and what every non-pointer caller wants.
struct SlotOffsets {
    const float* y     = nullptr;   // one entry per slot
    uint32_t     count = 0;
};

/// The topmost cascade slot under (px, py), or -1 for none.
///
/// "Topmost" is resolved by slot Z, which is the same key the draw pass sorts
/// on (nearest last), so the tile the user sees on top is the tile they hit.
/// Tiles fainter than `minAlpha` are skipped: the cascade's tail fades to
/// nothing and an invisible tile must not swallow a click aimed past it.
int PickSlot(const FlipScene& scene, DirectX::FXMMATRIX monRemap,
             float cascadeAspect, float vpW, float vpH,
             float px, float py,
             const SlotOffsets& drawOffsets = SlotOffsets{},
             float minAlpha = 0.05f);

} // namespace TileHitTest
