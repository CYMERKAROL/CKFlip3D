// ---------------------------------------------------------------------------
// Building the Cover Flow row: slot poses, per-tile lean, and the camera that
// frames them.  The spacing is solved against the projected width of each tile
// rather than assumed, so the row holds its shape across aspect ratios.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#define NOMINMAX
#include "CoverFlowLayout.hpp"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Cover Flow layout constants, tuned at the program's default of 10 windows:
// a flat centre tile facing the camera, side tiles leaning gently outward,
// light shingle overlap on the sides (inner tile over outer) and a slight
// recession per step.  Everything width-like is derived adaptively from the
// viewport aspect, so the outermost tiles may bleed off the screen edge.
// ---------------------------------------------------------------------------
namespace {

// Side tiles lean gently OUTWARD — each
// tile's OUTER edge tips toward the viewer, the inner edge recedes — like
// a row of standing panels fanned open.  Steep inward angles (iTunes-style
// cover flow) made the inner edges loom into the centre zone.
constexpr float kSideAngleDeg = 30.0f;  // gentle outward lean of side tiles
constexpr float kSideDepth    = 1.20f;  // Z push of the first side tile —
                                        // sides stay near the centre plane
constexpr float kDepthStep    = 0.35f;  // extra Z per step outward (also
                                        // fixes painter's order: inner
                                        // tiles draw over outer ones)
constexpr float kTileScale    = 0.86f;  // Cover Flow tiles vs cascade size
constexpr float kCamZoom      = 1.22f;  // camera pull-back vs cascade dist
constexpr float kEyeLift      = 1.10f;  // eye above tile centre → mild
                                        // downward look onto the floor
constexpr float kLookDrop     = 0.35f;  // look-at below tile centre
constexpr float kCenterGapPad = 0.30f;  // clear space between centre tile
                                        // edge and the first side tile
constexpr float kGapProjFrac  = 0.60f;  // centre-gap share of a side
                                        // tile's projected half-width
constexpr float kSpacingFrac  = 0.62f;  // preferred side spacing as a
                                        // fraction of a side tile's
                                        // projected width — a light
                                        // shingle overlap like the
                                        // reference frame
constexpr float kFitMargin    = 0.98f;  // the OUTERMOST side tile's centre
                                        // lands at the screen edge — its
                                        // outer half bleeds off-screen,
                                        // exactly like the reference
constexpr float kTypAspect    = 16.0f / 9.0f;  // typical window aspect used
                                        // for the fit estimate (real per-
                                        // window sizes come later through
                                        // FlipScene::SetSlotScale)

// --- Long rows -------------------------------------------------------------
// Cover Flow is designed around five windows: the row fits across the screen
// with the outermost tile's centre at the edge.  Ask for more and the fit
// compression below has to fit the extra tiles into the same width — and
// compression only moves tiles CLOSER, so past a point the newcomers simply
// hide behind their neighbours and a ten-window row looks exactly like a
// five-window one.  The window count is meant to be the number of windows you
// can SEE, so the tiles themselves have to give ground:
//   - kFitShrink pulls the tile size down as the row lengthens (this is what
//     actually makes room; it feeds globalScaleActual, so SetSlotScale keeps
//     the real windows in proportion),
//   - each further step out also recedes a little deeper and fades, so the
//     row reads as a carousel disappearing into the distance.
// Rows of five or fewer never reach any of this and are unchanged.
constexpr uint32_t kComfortSide  = 2;     // |offset| that still gets full weight
constexpr float    kLongDepthAdd = 0.50f; // extra depth-step per side beyond it
constexpr float    kLongFade     = 0.45f; // alpha lost at the outermost tile
// Where the OUTERMOST tile's outer edge lands, as a fraction of the screen
// half-width.  Not kFitMargin: that one places the outermost tile's CENTRE at
// the edge and lets its outer half bleed off — the reference framing for a
// short row, and the thing that made the last two tiles of a long row
// invisible (see the even-strip walk in RelayoutX).
constexpr float    kLongEdgeMargin = 0.99f;
// --- Depth of the pile -----------------------------------------------------
// A short row overlaps two tiles deep: each side tile is half-hidden by the
// one in front of it and that is the whole shingle.  A long row has to hold
// the same rule, or tiles start stacking three and four deep on one column of
// pixels — the outer ones fade (kLongFade), so what shows there is two or
// three windows blended into each other rather than any single one.
//
// A tile reaches its neighbour-but-one exactly when it is wider than TWO of
// the strips the row hands out, so that is the ceiling — 1.9 for a margin
// against float drift.  The lean is NOT the lever used to respect it: turning
// the cards edge-on does narrow them, but past about forty degrees the row
// stops looking like Cover Flow and starts looking like books stood on a
// shelf.  The tiles give ground in SIZE instead, which is what the reference
// does when it has more covers than room.
constexpr float    kMaxSpanStrips = 1.90f;
// Floor on that shrink: past this the deck reads as "far away" rather than as
// a row of windows, and a count that cannot be shown honestly is better shown
// slightly crowded than microscopic.
constexpr float    kMinRowFit     = 0.42f;

// Slots either side of centre for a row of `visible` tiles — matches the
// split SlotOffset uses.
inline uint32_t RowSides(uint32_t visible)
{
    return std::max<uint32_t>(1u, visible / 2);
}

// --- The one placement rule, shared by the size solve and the row walk ------
// Where a tile of half-width `halfW` standing at depth `z` ends up when its
// OUTER edge is asked for at screen position `edge`, what it then covers, and
// where its inner edge lands.  Both corners go through the perspective divide
// SEPARATELY: a turned quad has its two edges at different depths, and the
// near one projects noticeably wider than a flat estimate would say.
struct Placement { float ax, span, inner; };

inline Placement PlaceOuterEdge(float halfW, float z, float edge,
                                float leanDeg, float K, float camEyeZ)
{
    const float rad  = leanDeg * 3.14159265f / 180.0f;
    const float c    = std::cos(rad);
    const float sn   = std::sin(rad);
    const float dOut = std::max(0.001f, z - halfW * sn - camEyeZ);
    const float dIn  = std::max(0.001f, z + halfW * sn - camEyeZ);
    const float ax   = edge * K * dOut - halfW * c;
    return { ax,
             (ax + halfW * c) / (K * dOut) - (ax - halfW * c) / (K * dIn),
             (ax - halfW * c) / (K * dIn) };
}

// Depth of the side slot `a` steps out from the centre.
inline float SideZ(float frontZ, float depthStep, uint32_t a)
{
    return frontZ + kSideDepth + static_cast<float>(a - 1) * depthStep;
}

// Would a row whose every tile is at most `halfW` wide stay two deep?
//
// Two conditions, and they are the whole of it:
//   * no tile wider than kMaxSpanStrips of the strip it is handed — wider than
//     two strips and it reaches its neighbour-but-one;
//   * no side tile whose inner edge crosses the middle — one that does meets
//     the tile coming the other way UNDER the centre tile, which is the same
//     pile arriving from both sides at once.
//
// Asked about the WIDEST tile the row can ever hold, not the arrangement
// currently on screen: every cycle step permutes the windows through the
// slots, so a size that only fits the present order would stop fitting on the
// next Tab.
inline bool RowFitsTwoDeep(float halfW, uint32_t rightCount, uint32_t leftCount,
                           float frontZ, float depthStep,
                           float K, float camEyeZ)
{
    if (K <= 0.0f || halfW <= 0.0f) return true;
    const float centreEdge = halfW / (K * std::max(0.001f, frontZ - camEyeZ));
    const float budget = kLongEdgeMargin - centreEdge;
    if (budget <= 0.0f) return false;

    for (int pass = 0; pass < 2; ++pass) {
        const uint32_t count = (pass == 0) ? rightCount : leftCount;
        if (count == 0) continue;
        const float strip = budget / static_cast<float>(count);
        for (uint32_t a = 1; a <= count; ++a) {
            const Placement p = PlaceOuterEdge(
                halfW, SideZ(frontZ, depthStep, a),
                centreEdge + strip * static_cast<float>(a),
                kSideAngleDeg, K, camEyeZ);
            if (p.span > kMaxSpanStrips * strip || p.inner < 0.0f)
                return false;
        }
    }
    return true;
}

// Same aspect-conditional tile shrink the cascade applies (see
// FlipScene.cpp TileSizeAspectScale) — keeps Cover Flow tile sizes
// consistent with what SetSlotScale will compute for real windows.
inline float TileAspectScale(float viewportAspect)
{
    constexpr float kRefAspect = 3440.0f / 1440.0f;
    float hScale = viewportAspect / kRefAspect;
    if (hScale >= 1.0f) return 1.0f;
    return std::sqrt(std::max(hScale, 0.1f));
}

} // namespace

int CoverFlowLayout::SlotOffset(uint32_t i, uint32_t visible)
{
    if (visible <= 1 || i == 0) return 0;
    // Right side takes ceil((N-1)/2) slots, the rest wrap to the left.
    const uint32_t right = (visible - 1 + 1) / 2;   // ceil((N-1)/2)
    if (i <= right) return static_cast<int>(i);
    return static_cast<int>(i) - static_cast<int>(visible);
}

CoverFlowLayout::Result CoverFlowLayout::Build(const SceneConfig& cfg,
                                               uint32_t visible,
                                               float viewportAspect)
{
    Result r;
    if (visible == 0) return r;

    const float aspectScale = TileAspectScale(viewportAspect);
    const uint32_t sides    = RowSides(visible);
    const float frontZ0     = cfg.camDist;
    const float camEyeZ0    = frontZ0 - cfg.camDist * kCamZoom;

    // --- How big the tiles are allowed to be ------------------------------
    // Short rows keep the tuned size exactly.  Longer ones give ground: the
    // row still has to fit across one screen, and the only honest way to show
    // ten windows there is for each of them to be smaller.  The size is SOLVED
    // rather than guessed at with an exponent — the largest one that still
    // leaves every tile a strip of its own and keeps the deck two deep (see
    // RowFitsTwoDeep) is exactly the answer the count is asking for.
    //
    // Solved against the WIDEST tile the row could ever hold — a full-height
    // window at SetSlotScale's aspect cap — because the tile size is baked in
    // here, before the real windows are known, and every cycle step shuffles
    // which window sits where.
    float rowFit = 1.0f;
    if (sides > kComfortSide) {
        const uint32_t rightCount = visible / 2;
        const uint32_t leftCount  = visible - 1 - rightCount;
        const float overrun0 = static_cast<float>(sides - kComfortSide);
        const float depthStep0 = kDepthStep * (1.0f + kLongDepthAdd * overrun0 / 3.0f);
        const float K = std::tan(cfg.fovDeg * 0.5f * 3.14159265f / 180.0f)
                      * viewportAspect;
        // Mirror of the cap in FlipScene::SetSlotScale — the widest a tile can
        // come back as once a real window's proportions are applied.
        const float maxAspect = std::min(2.22f,
            std::max(viewportAspect * 0.93f, 16.0f / 9.0f));
        auto halfWidthAt = [&](float fit) {
            return 0.5f * cfg.tileHeight * cfg.globalScale * kTileScale * fit
                        * aspectScale * maxAspect;
        };
        auto fits = [&](float fit) {
            return RowFitsTwoDeep(halfWidthAt(fit), rightCount, leftCount,
                                  frontZ0, depthStep0, K, camEyeZ0);
        };
        if (!fits(1.0f)) {
            rowFit = kMinRowFit;
            if (fits(kMinRowFit)) {
                float lo = kMinRowFit, hi = 1.0f;
                for (int it = 0; it < 24; ++it) {
                    const float mid = (lo + hi) * 0.5f;
                    if (fits(mid)) lo = mid; else hi = mid;
                }
                rowFit = lo;
            }
        }
    }

    const float GS      = cfg.globalScale * kTileScale * rowFit;
    r.globalScaleActual = GS;

    // Default tile size (SetSlotScale refines per window later).
    const float tileH = cfg.tileHeight * GS * aspectScale;
    const float tileW = tileH * kTypAspect;

    const float centerY = cfg.baseY;
    const float frontZ  = frontZ0;
    r.floorY = centerY - tileH * 0.5f;

    // --- Camera: centred, pulled back, looking slightly down ---------------
    r.eyeX = 0.0f;
    r.eyeY = centerY + kEyeLift;
    r.eyeZ = camEyeZ0;
    r.targetX = 0.0f;
    r.targetY = centerY - kLookDrop;
    r.targetZ = frontZ + 2.0f;

    // --- Place tiles --------------------------------------------------------
    // Depth, lean and size only; the row's x coordinates come from RelayoutX
    // below, which is also re-run once the real window proportions are known
    // (FlipScene::RelayoutCoverFlowX).  One spacing rule, one place.
    // How much of the row sits past the comfortable width (see kComfortSide).
    const float overrun = (sides > kComfortSide)
        ? static_cast<float>(sides - kComfortSide) : 0.0f;
    const float depthStep = kDepthStep * (1.0f + kLongDepthAdd * overrun / 3.0f);

    r.slots.resize(visible);
    for (uint32_t i = 0; i < visible; ++i) {
        TileSlot& s = r.slots[i];
        const int off  = SlotOffset(i, visible);
        const int aoff = std::abs(off);

        s.scaleY = tileH;
        s.scaleX = tileW;
        s.y      = r.floorY + s.scaleY * 0.5f;
        s.x      = 0.0f;

        // Tiles beyond the comfortable width fade with distance.  SetSlotScale
        // later rewrites the sizes from the real windows but never touches
        // alpha, so this survives into the live stack.
        s.alpha = 1.0f;
        if (overrun > 0.0f && static_cast<uint32_t>(aoff) > kComfortSide) {
            const float t = static_cast<float>(aoff - static_cast<int>(kComfortSide))
                          / overrun;
            s.alpha = 1.0f - std::clamp(t, 0.0f, 1.0f) * kLongFade;
        }

        if (off == 0) {
            s.z    = frontZ;
            s.rotY = 0.0f;
        } else {
            s.z    = frontZ + kSideDepth
                   + static_cast<float>(aoff - 1) * depthStep;
            // Outward lean (reference-matched): the tile's OUTER edge tips
            // toward the camera.  XMMatrixRotationY(+θ) brings the +X edge
            // toward the camera, so the right side (outer edge = +X) gets
            // +angle and the left side (outer edge = -X) gets -angle.
            s.rotY = (off > 0) ? kSideAngleDeg : -kSideAngleDeg;
        }
    }

    RelayoutX(cfg, r.slots, viewportAspect, r.eyeZ);
    return r;
}

void CoverFlowLayout::RelayoutX(const SceneConfig& cfg,
                                std::vector<TileSlot>& slots,
                                float viewportAspect, float camEyeZ)
{
    const uint32_t visible = static_cast<uint32_t>(slots.size());
    if (visible < 2) {
        if (visible == 1) slots[0].x = 0.0f;
        return;
    }

    // Half-width each tile actually covers on the row's axis: a side tile
    // turned by kSideAngleDeg presents cos(angle) of its width.
    auto projHalfW = [](const TileSlot& s) {
        const float c = (s.rotY == 0.0f)
            ? 1.0f
            : std::cos(s.rotY * 3.14159265f / 180.0f);
        return s.scaleX * 0.5f * std::fabs(c);
    };

    // Order each side's slots by |offset| so the walk runs centre-OUTWARD.
    // Ascending slot index alone will not do: the left side's indices climb
    // while its |offset| falls (slot 6 = offset -4 ... slot 9 = offset -1).
    std::vector<uint32_t> right, left;
    right.reserve(visible);
    left.reserve(visible);
    for (uint32_t i = 1; i < visible; ++i) {
        const int off = SlotOffset(i, visible);
        if (off > 0)      right.push_back(i);
        else if (off < 0) left.push_back(i);
    }
    auto byAbsOffset = [visible](uint32_t a, uint32_t b) {
        return std::abs(SlotOffset(a, visible)) < std::abs(SlotOffset(b, visible));
    };
    std::sort(right.begin(), right.end(), byAbsOffset);
    std::sort(left.begin(),  left.end(),  byAbsOffset);

    // Natural spacing: neighbours sit apart by the sum of their own projected
    // half-widths scaled by kSpacingFrac, giving the same shingle overlap
    // however wide or narrow the individual windows are.  The first gap on
    // each side is measured from the centre tile and is treated separately —
    // the fit compression below must never crowd the focal tile.
    const float centreHalfW = projHalfW(slots[0]);

    // --- Long rows: equal visible strips -----------------------------------
    // What a tile actually SHOWS is the band between its own outer edge and
    // the outer edge of the neighbour one step nearer the centre: the inner
    // neighbour sits closer to the camera and is painted over it.  So the
    // thing to distribute evenly is the tiles' OUTER EDGES — spacing their
    // CENTRES evenly, as this used to, spends the budget in the wrong places.
    //
    // Measured on the real layout at 1920x1080 with ten windows: the first
    // side tile showed 406 px, the middle ones ~130 px, and the outermost TWO
    // showed NOTHING — their centres were pushed to the screen edge, so both
    // ran off it before emerging from under the tile in front.  A ten-window
    // row read as five, which is the count it was meant to be.
    //
    // The walk below is in SCREEN measure, not world x: the row recedes as it
    // fans out, so equal steps in x are not equal steps on screen.  For a tile
    // turned by `rotY` the outer edge is the corner tipped TOWARD the camera —
    // world (ax + hw*cos, z - hw*sin) — and its screen offset is that x over
    // its own depth.  Placing the outermost edge on the screen edge and the
    // rest evenly between it and the centre tile's edge gives every window in
    // the row the same slice of what the centre tile leaves: the largest equal
    // slice there is.  Rows of five or fewer never reach this branch and keep
    // the tuned shingle exactly.
    if (RowSides(visible) > kComfortSide) {
        // Screen half-width at unit depth: ndc_x = world_x / (K * depth).
        const float K = std::tan(cfg.fovDeg * 0.5f * 3.14159265f / 180.0f)
                      * viewportAspect;
        const float centreDepth = std::max(0.001f, slots[0].z - camEyeZ);
        // The centre tile faces the camera, so its outer edge is its own
        // projected half-width at its own depth.
        const float centreEdge = (K > 0.0f)
            ? centreHalfW / (K * centreDepth) : 0.0f;

        // The lean is Build's kSideAngleDeg and stays there — nothing here
        // turns a window further to make it fit.  Room is made by SIZE, in the
        // solve Build runs before this (RowFitsTwoDeep), so all that is left
        // to do is hand each tile its strip.  Widths are the REAL ones, so the
        // row fills the screen exactly whatever mix of windows it holds.
        for (int pass = 0; pass < 2; ++pass) {
            const std::vector<uint32_t>& side = (pass == 0) ? right : left;
            if (side.empty()) continue;
            const float sign  = (pass == 0) ? 1.0f : -1.0f;
            const float count = static_cast<float>(side.size());

            // The grid normally starts at the centre tile's own edge.  It has
            // to start further out when the centre tile is much NARROWER than
            // the one beside it — a tall thin dialog in the middle with a
            // maximised window next to it — or the first side tile is placed
            // so far in that it crosses the middle and meets its opposite
            // number under the centre tile.  Build cannot solve for that: it
            // sizes the row before any window is known, and which of them is
            // in the middle changes on every Tab.  Starting the grid a little
            // wider costs the row a sliver of strip and keeps it two deep.
            float startEdge = centreEdge;
            {
                const TileSlot& first = slots[side.front()];
                const float hw  = first.scaleX * 0.5f;
                const float rad = std::fabs(first.rotY) * 3.14159265f / 180.0f;
                const float c   = std::cos(rad);
                const float sn  = std::sin(rad);
                const float dOut = std::max(0.001f, first.z - hw * sn - camEyeZ);
                // Edge at which that tile's inner edge lands exactly on the
                // middle; anything less and it crosses.
                const float innerFloor = (K > 0.0f)
                    ? 2.0f * hw * c / (K * dOut) : 0.0f;
                if (side.size() >= 2) {
                    // edge of slot 1 == startEdge + (margin - startEdge)/count
                    const float need = (innerFloor * count - kLongEdgeMargin)
                                     / (count - 1.0f);
                    startEdge = std::max(startEdge, need);
                }
                startEdge = std::clamp(startEdge, 0.0f, kLongEdgeMargin * 0.9f);
            }
            const float strip = std::max(0.0f, kLongEdgeMargin - startEdge) / count;

            for (size_t k = 0; k < side.size(); ++k) {
                TileSlot& s = slots[side[k]];
                const float edge = startEdge + strip * static_cast<float>(k + 1);
                const Placement p = PlaceOuterEdge(
                    s.scaleX * 0.5f, s.z, edge, std::fabs(s.rotY), K, camEyeZ);
                s.x = sign * std::max(0.0f, p.ax);
            }
        }
        return;
    }

    const float spacingFrac = kSpacingFrac;
    float firstGap  = 0.0f;   // centre → first side tile (both sides equal enough)
    float maxSpread = 0.0f;   // largest (ax - firstGap) over both sides
    float outermostZ = slots[0].z;

    for (int pass = 0; pass < 2; ++pass) {
        const std::vector<uint32_t>& side = (pass == 0) ? right : left;
        const float sign = (pass == 0) ? 1.0f : -1.0f;
        float prevX = 0.0f, prevHalfW = centreHalfW;
        for (size_t k = 0; k < side.size(); ++k) {
            const uint32_t i = side[k];
            const float halfW = projHalfW(slots[i]);
            const float ax = (k == 0)
                ? (centreHalfW + halfW * kGapProjFrac + kCenterGapPad)
                : (prevX + (prevHalfW + halfW) * spacingFrac);
            slots[i].x = sign * ax;
            if (k == 0 && ax > firstGap)
                firstGap = ax;
            if (ax - firstGap > maxSpread) {
                maxSpread  = ax - firstGap;
                outermostZ = slots[i].z;
            }
            prevX     = ax;
            prevHalfW = halfW;
        }
    }

    // Fit: squeeze the row until the OUTERMOST tile's CENTRE sits at the
    // screen edge, so at most its outer half bleeds off — the reference
    // framing.  Only the spread BEYOND the first gap is compressed (the
    // centre tile keeps its breathing room), mirroring how the previous
    // nominal-width estimate compressed its `spacing` increment but never
    // its `centerGap`.  Never expands.
    if (maxSpread <= 0.0f)
        return;
    const float halfWidthAtSide =
        std::tan(cfg.fovDeg * 0.5f * 3.14159265f / 180.0f)
        * viewportAspect * (outermostZ - camEyeZ) * kFitMargin;
    const float budget = halfWidthAtSide - firstGap;
    if (budget > 0.0f && maxSpread > budget) {
        const float k = budget / maxSpread;
        for (uint32_t i = 0; i < visible; ++i) {
            const float ax = std::fabs(slots[i].x);
            if (ax <= firstGap) continue;          // centre + first side tiles
            const float sign = (slots[i].x < 0.0f) ? -1.0f : 1.0f;
            slots[i].x = sign * (firstGap + (ax - firstGap) * k);
        }
    }
}
