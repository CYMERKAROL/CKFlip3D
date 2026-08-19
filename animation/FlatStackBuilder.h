// ---------------------------------------------------------------------------
// The flat end of the entry/exit morph: where every tile sits when it is still
// pretending to be the real window on the desktop.  Derived from window rects
// inverse-projected through the cascade camera, so the two endpoints line up
// on screen and the morph between them has nothing to hide.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <vector>

#include "../scene/FlipScene.hpp"
#include "../capture/windowscanner.h"

/// Cascade slot data is never a source of flat geometry.  The camera is the
/// only thing the two endpoints share, which is what keeps screen positions
/// continuous across the morph.
namespace FlatStackBuilder {

/// Z spacing between adjacent flat slots in world space.
constexpr float kFlatZStep = 0.08f;

/// Resolve a 2D screen rect for one window with ordered fallbacks:
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
/// Z assignment: slot[i].z = flatZ + rank(i) * kFlatZStep, where rank(i) is
/// `depthRanks[i]` when supplied and plain `i` otherwise.
///
/// The bare index works only while SLOT ORDER IS DEPTH ORDER, which holds for
/// the classic cascade (FlipScene::BuildSlots emits monotonically increasing z)
/// but NOT for Cover Flow, whose depth follows |carousel offset| — its
/// left-hand slots have rising indices at falling depth.  Feeding the cascade
/// endpoint's depth ranking through `depthRanks` keeps the flat and cascade
/// endpoints in the SAME relative order, so no two tiles swap painter's order
/// mid-morph.  Passing the identity permutation (or nullptr) falls back to
/// plain index order.
///
/// The rank participates in the inverse projection, so x/y/scale are solved at
/// the final z — the on-screen rect is exact regardless of which rank a slot
/// receives.  alpha = 1.0 for every slot.
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
    float flatZOverride = -1.0f,
    const std::vector<uint32_t>* depthRanks = nullptr);

/// Depth ranking of a cascade endpoint: rank[i] = position of slot i when the
/// slots are ordered near-to-far by z (stable, ties broken by slot index).
/// For the classic cascade this is the identity permutation, so passing it to
/// BuildFlatSlotsFromRects changes nothing there.
std::vector<uint32_t> DepthRanks(const std::vector<TileSlot>& slots);

} // namespace FlatStackBuilder
