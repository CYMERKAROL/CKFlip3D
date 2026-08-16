#pragma once

#include "FlipScene.hpp"
#include <vector>
#include <cstdint>

/// Cover Flow visual preset — centred carousel layout (config
/// `visualPreset` = 1).  Flat centre tile, side tiles leaning gently
/// OUTWARD (outer edge toward the viewer, Vista-wall style), all standing
/// on a common glass-floor plane.  Pure geometry: given the shared
/// SceneConfig, the visible tile count and the viewport aspect it produces
/// the slot poses (position, per-slot rotY, bottom-aligned onto the floor
/// plane) and the centred camera.  FlipScene::BuildSlotsCoverFlow copies the result
/// into the same cached members the cascade path fills, so every consumer
/// of FlipScene (animators, controller, FlatStackBuilder's inverse
/// projection) is layout-agnostic.
///
/// Slot-index mapping keeps the controller's cycle semantics: slot 0 is
/// the SELECTED tile (centre), slots 1..R fan out to the right (the
/// upcoming windows), and the tail wraps to the left of centre (the most
/// recently cycled-away windows) — the row is a circular carousel, so a
/// forward cycle slides everything one position left.
namespace CoverFlowLayout {

struct Result {
    std::vector<TileSlot> slots;
    // Camera pose (world space, LH — same conventions as FlipScene).
    float eyeX = 0, eyeY = 0, eyeZ = 0;
    float targetX = 0, targetY = 0, targetZ = 0;
    // Effective global tile scale for FlipScene::SetSlotScale.
    float globalScaleActual = 1.0f;
    // World Y of the common tile-bottom plane (the "glass shelf").
    float floorY = 0.0f;
};

/// Build the Cover Flow layout for `visible` tiles (1..maxVisible).
/// Tile sizes are nominal here (the real windows are not known yet); the
/// controller re-runs RelayoutX once they are.
Result Build(const SceneConfig& cfg, uint32_t visible, float viewportAspect);

/// Re-space the row from the tiles' ACTUAL widths.
///
/// Build() has to size the row before the real windows are known, so it
/// estimates every tile as a nominal 16:9 rect.  Once FlipScene::SetSlotScale
/// has applied each window's true proportions — up to the 2.22:1 cap, i.e.
/// ~25 % wider than the estimate, or far narrower for a small window — the
/// nominal gaps read as too much overlap or as holes in the row.  This pass
/// re-derives the x coordinates using each tile's own projected half-width,
/// so the shingle overlap looks identical whatever mix of window shapes the
/// row happens to hold.
///
/// For LONG rows it also re-derives the side lean (rotY): how far a card has
/// to turn to stay inside its share of the row is a question about the real
/// tile widths, so it can only be answered here.  Short rows keep Build()'s
/// kSideAngleDeg untouched.  y, z, scale, alpha and the camera are left
/// exactly as Build() produced them in every case.
void RelayoutX(const SceneConfig& cfg, std::vector<TileSlot>& slots,
               float viewportAspect, float camEyeZ);

/// Signed carousel offset of slot `i` for `visible` tiles: 0 = centre,
/// positive = right of centre, negative = left.  Exposed for tests /
/// diagnostics; Build() uses it internally.
int SlotOffset(uint32_t i, uint32_t visible);

} // namespace CoverFlowLayout
