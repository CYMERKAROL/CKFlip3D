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
    // tileScale removed — all tiles same world-space size
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
    void     GetDrawCall(uint32_t index, float viewportAspect,
                         DirectX::XMFLOAT4X4& outMVP, float& outAlpha) const;
    uint32_t SlotCount() const { return static_cast<uint32_t>(m_slots.size()); }

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

private:
    SceneConfig           m_cfg;
    std::vector<TileSlot> m_slots;
    float m_viewportAspect = 1.78f;

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
};
