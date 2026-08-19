// ---------------------------------------------------------------------------
// The cascade's geometry and camera.  Every animator writes into the slots
// this scene owns, and every draw call comes out of it, which is why the whole
// program can stay layout-agnostic while the visual preset changes underneath.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

#define NOMINMAX
#include <DirectXMath.h>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Win7-matched constants (hand-tuned against reference footage of the
// original Flip3D until the layout and motion matched frame-for-frame)
// ---------------------------------------------------------------------------
struct SceneConfig {
    // --- Tile appearance ---
    float tileHeight =  5.0f;    // world-unit tile height
    float minAlpha   =  0.88f;   // opacity of the furthest tile
    float tiltY      =  3.596f;  // base Y-rotation (perspective trapezoid)
    float tiltYslope =   0.000f;  // vis-dependent Y-rotation: tiltY + tiltYslope*(vis/10)
    float tiltX      =   0.000f;  // base X-rotation (forward pitch)
    float tiltXslope =   0.000f;  // vis-dependent X-rotation: tiltX + tiltXslope*(vis/10)
    float globalScale=  0.563f;  // scale factor for tile world-size
    float globalScaleSlope = 0.031f; // vis-dependent scale: GS*(1 + slope*(1 - vis/10))

    // --- Camera ---
    float camDist    = 17.098f;  // Z-distance from camera to front tile
    float fovDeg     = 16.385f;  // vertical field-of-view

    // --- Dynamic density base values (tuned for 10 windows) ---
    float stepZ10     =  2.216f;  // Z-step at 10 windows
    float riseRatio10 =  0.117f;  // Y-rise/depth at 10 windows
    float spreadRatio10= 0.300f;  // X-spread/depth at 10 windows
    float rearDip10   =  1.113f;  // quadratic rear dip at 10 windows

    // --- Rise power law ---
    float riseGamma  =  0.100f;  // rise = riseRatio10 * pow(N/10, riseGamma)

    // --- Camera framing ---
    float baseXfrac  = 24.994f;   // baseX = totalSpreadX * this
    float eyeXfrac   =  0.956f;   // eyeX = baseX * this (replaces eyeXmul)
    float eyeYconst  =  1.428f;   // constant Y offset for camera eye
    float eyeYbase   =  0.000f;   // eyeY cascade-dependent offset (fraction of rise)
    float lookFrac   =  0.262f;   // look-at depth fraction
    float lookYoff   = -0.621f;   // Y offset for look-at target

    // --- Vis-dependent camera offsets (strongest at low N, zero at N=10) ---
    float camXextra  = -3.716f;   // camera X offset * (1 - vis/10)
    float camYextra  = -0.537f;   // camera Y offset * (1 - vis/10)
    float lookYextra =  1.261f;   // look-at Y offset * (1 - vis/10)

    // --- Adaptive camera Z (zoom out for fewer windows) ---
    float camZmin    =  1.140f;   // zoom factor at vis=1 (>1 = farther for few windows)
    float camZgamma  =  2.962f;   // zoom curve power: zoom = camZmin + (1-camZmin)*(N/10)^gamma
    float camTW      =  4.603f;   // camera XY blend transition width (ff → actual)

    // --- Front tile Y ---
    float baseY      = 23.303f;  // front tile Y in world

    // --- Spline shape ---
    float depthPower =  0.965f;  // depth curve exponent
    uint32_t maxVisible = 10;    // max tiles with distinct on-screen positions
    float expMax     =  2.679f;  // maximum expansion factor for dynamic density
    uint32_t framingFloor = 5;   // minimum effective count for camera XY framing

    // --- Framing floor ---
    float framingFloorF = 1.499f; // continuous framing floor for optimizer

    // --- Fade tail ---
    uint32_t fadeStart = 8;      // slot index where alpha starts fading to 0
};

/// Placement of a single tile in the 3D scene.
struct TileSlot {
    float x      = 0.0f;
    float y      = 0.0f;
    float z      = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float alpha  = 1.0f;
    // Per-tile Y rotation in degrees, ADDED to the scene-wide tilt.
    // The classic cascade leaves it at 0 for every slot (all tiles share
    // the scene tilt — bit-identical to the pre-rotY pipeline); the Cover
    // Flow preset uses it to turn side tiles toward the centre.  All
    // animators interpolate it like any other slot field.
    float rotY   = 0.0f;
    // tileScale removed — all tiles same world-space size
};

/// Visual preset selector (config `visualPreset`).  Cascade is the classic
/// Win7 Flip3D layout; CoverFlow is the centred carousel (scene/
/// CoverFlowLayout).  Latched by the controller at Activate.
enum class VisualPreset : int {
    Cascade  = 0,
    CoverFlow = 1,
};

/// Builds the 3D Flip3D stack layout and produces draw-ready MVP matrices.
/// Layout adapts to viewport dimensions for resolution independence.
class FlipScene {
public:
    explicit FlipScene(SceneConfig cfg = {}) : m_cfg(cfg) {}

    /// Build slots for the given tile count, adapting to viewport dimensions.
    void     BuildSlots(uint32_t count, float viewportWidth, float viewportHeight);
    void     SetSlotAspect(uint32_t index, float aspect);
    /// Scale a tile relative to the desktop.  The slot position stays centred
    /// so small windows float in the middle of their assigned 3D position.
    void     SetSlotScale(uint32_t index, float widthPx, float heightPx,
                          float desktopW, float desktopH);
    /// Rotate the cached per-window scale factors to match a window rotation.
    /// forward=true: shift left (element 0 → end), matching std::rotate begin+1.
    /// forward=false: shift right (last element → front).
    void     RotateAspects(bool forward);
    /// `worldYOffset` lifts (or sinks) the tile along world Y for this draw
    /// only — the pointer-hover highlight, which must not touch slot state
    /// because the cycle / close / entry-exit animators own it.  0 (the
    /// default) is the untouched transform every other call site gets.
    void     GetDrawCall(uint32_t index, float viewportAspect,
                         DirectX::XMFLOAT4X4& outMVP, float& outAlpha,
                         float worldYOffset = 0.0f) const;
    /// Draw-ready MVP for the tile's floor reflection: the same transform
    /// chain as GetDrawCall with a unit-quad-space shift one tile-height
    /// down (the quad then occupies the mirror position below the tile's
    /// bottom edge; the caller flips the texture V via the UV crop).  No
    /// negative scaling — winding is preserved, so the default rasterizer
    /// state draws it like any other quad.
    void     GetReflectionDrawCall(uint32_t index, float viewportAspect,
                                   DirectX::XMFLOAT4X4& outMVP,
                                   float& outAlpha,
                                   float worldYOffset = 0.0f) const;
    uint32_t SlotCount() const { return static_cast<uint32_t>(m_slots.size()); }

    /// Select the layout the next BuildSlots produces.  Latched by the
    /// controller at Activate (a mid-session config reload never switches
    /// the preset under a live cascade).
    void         SetVisualPreset(VisualPreset p) { m_preset = p; }
    VisualPreset GetVisualPreset() const         { return m_preset; }

    /// Cover Flow: re-space the row from the tiles' ACTUAL widths, once the
    /// real window proportions have been applied through SetSlotScale /
    /// SetSlotAspect.  BuildSlots can only estimate them (a nominal 16:9
    /// rect), which leaves too much overlap for wide windows and holes for
    /// narrow ones.  Rewrites slot x, and on long rows the side lean (rotY)
    /// that keeps the deck two tiles deep.  No-op in the cascade preset.
    void RelayoutCoverFlowX();

    /// Direct access to slot data for animation interpolation.
    const TileSlot& GetSlot(uint32_t index) const { return m_slots[index]; }
    TileSlot&       GetSlotMut(uint32_t index)    { return m_slots[index]; }

    /// Override scene-wide tilt for animation interpolation.  Mutates only
    /// the cached values consumed by GetDrawCall — does NOT touch BuildSlots'
    /// computation.  Pure animation hook into already-cached state.
    void SetSceneTilt(float tiltY, float tiltX) {
        m_tiltY_actual = tiltY;
        m_tiltX_actual = tiltX;
    }

    float GetSceneTiltY() const { return m_tiltY_actual; }
    float GetSceneTiltX() const { return m_tiltX_actual; }

    /// Read-only camera accessors used by the entry/exit animator to
    /// inverse-project 2D screen rects into the cascade's world-space frame.
    /// FlipScene remains the sole geometry truth — these are simple getters
    /// over already-cached state, no recomputation, no setters.
    float GetCamEyeX()    const { return m_eyeX; }
    float GetCamEyeY()    const { return m_eyeY; }
    float GetCamEyeZ()    const { return m_eyeZ; }
    float GetCamTargetX() const { return m_targetX; }
    float GetCamTargetY() const { return m_targetY; }
    float GetCamTargetZ() const { return m_targetZ; }
    float GetFovDeg()     const { return m_cfg.fovDeg; }
    float GetCamDist()    const { return m_cfg.camDist; }

    /// View and projection for the current camera and the given viewport
    /// aspect, rebuilt only when one of those inputs actually changed.
    ///
    /// Both are a pure function of the camera, the FOV and the aspect, none of
    /// which move between the tiles of a frame, while GetDrawCall runs once per
    /// tile per pass and the hit test runs it again for every slot.  Uncached,
    /// that is one look-at and one perspective built twenty to forty times a
    /// frame for the same answer.
    ///
    /// Keyed on the inputs rather than invalidated by hand, so no future write
    /// to the camera can forget to clear it.  The multiply order is unchanged,
    /// only the two operands are reused, so the resulting MVP is bit-identical
    /// to the uncached one rather than merely equivalent.
    ///
    /// Public because the controller builds the same chain by hand for tiles
    /// FlipScene does not own: the entry morph's overflow tiles and the close
    /// transition's dying tiles.
    void CameraMatrices(float viewportAspect,
                        DirectX::XMMATRIX& outView,
                        DirectX::XMMATRIX& outProj) const;

private:
    /// Cover Flow variant of BuildSlots — delegates the geometry to
    /// scene/CoverFlowLayout and stores the results in the same cached
    /// members the cascade path fills.  The public API (SetSlotScale,
    /// RotateAspects, GetDrawCall, camera getters) is layout-agnostic.
    void BuildSlotsCoverFlow(uint32_t visible, float vpW, float vpH);

    SceneConfig           m_cfg;
    std::vector<TileSlot> m_slots;
    float m_viewportAspect = 1.78f;
    VisualPreset          m_preset = VisualPreset::Cascade;
    // Cover Flow floor plane (world Y of every tile's bottom edge).  Tiles
    // are bottom-aligned onto it so differently sized windows stand on a
    // common "glass shelf" and reflections emanate from one line.  Unused
    // (0) in the cascade preset.
    float m_floorY = 0.0f;

    // Cached camera parameters (computed once in BuildSlots, used in GetDrawCall)
    float m_eyeX = 0, m_eyeY = 0, m_eyeZ = 0;
    float m_targetX = 0, m_targetY = 0, m_targetZ = 0;

    // Cached vis-dependent rotation (computed in BuildSlots, used in GetDrawCall)
    float m_tiltY_actual = 0;
    float m_tiltX_actual = 0;
    float m_globalScale_actual = 1.0f;

    // Per-window scale cache (indices track window order, not slot position).
    // Rotated in RotateAspects() and applied to m_slots in BuildSlots().
    struct ScaleEntry { float sx, sy; };
    std::vector<ScaleEntry> m_windowScales;

    // Memoised camera matrices — see CameraMatrices().  Stored unaligned and
    // reloaded on use: a store/load round-trip of sixteen floats is exact,
    // and it keeps FlipScene free of the 16-byte alignment an XMMATRIX member
    // would impose on everything that owns one.
    struct CameraCache {
        float eyeX = 0.0f, eyeY = 0.0f, eyeZ = 0.0f;
        float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;
        float fovDeg = 0.0f, aspect = 0.0f;
        bool  valid = false;
        DirectX::XMFLOAT4X4 view{};
        DirectX::XMFLOAT4X4 proj{};
    };
    mutable CameraCache m_camCache;
};
