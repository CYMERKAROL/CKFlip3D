#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <vector>

#include "../scene/FlipScene.hpp"
#include "../capture/windowscanner.h"

/// Builds the flat (2D-rect) endpoint of the entry/exit morph.
/// Flat geometry is derived ONLY from read-only window rects plus the scene's
/// camera projection.  Cascade slot data is never used as the source of flat
/// geometry — the only thing shared with the cascade is the camera (so screen
/// positions are continuous across the morph).
namespace FlatStackBuilder {

/// Z spacing between adjacent flat slots in world space (spec §3.1, kFlatZStep).
constexpr float kFlatZStep = 0.08f;

/// Resolve a 2D screen rect for one window with ordered fallbacks
/// (spec §3.1 step 1):
///   1. WindowInfo.rect
///   2. GetWindowPlacement -> rcNormalPosition (for minimised windows)
///   3. GetWindowRect
///   4. Centred 52% fallback at desktopW/desktopH (preserves aspect)
RECT ResolveSourceRect(const WindowInfo& w, float desktopW, float desktopH);

/// Resolve each window's flat rect (= its real on-screen rect, no stacking
/// offsets).  The resolved rects are also returned via `outHandoffRects`
/// (identical to `outStackRects` in the current implementation — kept as a
/// separate output so an exit-handoff variant can diverge without changing
/// callers).
void BuildStackRects(const std::vector<WindowInfo>& windows,
                     float vpW, float vpH,
                     float desktopW, float desktopH,
                     std::vector<RECT>& outStackRects,
                     std::vector<RECT>& outHandoffRects);

/// Convert pixel rects to world-space TileSlots by inverse-projecting each
/// rect's NDC corners through the scene's camera onto a fixed flat-Z plane.
/// Each window keeps its own aspect; cascade slot data is not consulted.
///
/// Z assignment: slot[i].z = flatZ + i * kFlatZStep (strictly monotonic in i,
/// preserving painter's-algorithm draw order across the entire morph).
/// alpha = 1.0 for every slot (spec §3.1 StabilizeFlatStack).
///
/// `flatZOverride` ≤ 0 (the default) anchors the flat plane at the cascade's
/// natural focal depth (camEyeZ + camDist).  A positive value pins the plane
/// at that world Z instead — used by the overflow path to push out-of-bounds
/// tiles behind the back-most cascade slot so they cannot render in front of
/// (or pop through the front of) the visible deck during the entry morph.
/// The inverse-projection still maintains the correct screen position
/// regardless of plane depth.
std::vector<TileSlot> BuildFlatSlotsFromRects(
    const std::vector<RECT>& stackRects,
    const FlipScene& scene,
    float vpW, float vpH,
    float cascadeAspect,
    float originX, float originY,
    const DirectX::XMMATRIX& remapNDC,
    float flatZOverride = -1.0f);

} // namespace FlatStackBuilder
