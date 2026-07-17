#include "FlipScene.hpp"
#include "../core/DebugLog.h"
#include <cstdio>
#include <algorithm>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

using namespace DirectX;

// ---------------------------------------------------------------------------
// Win7 Flip3D layout — DYNAMIC DENSITY placement with adaptive camera.
//
// Three camera innovations match Win7 behaviour across all window counts:
//   1. Rise power law: riseRatio = riseRatio10 * pow(N/10, riseGamma)
//      Gives nearly zero rise at 2 windows, full rise at 10.
//   2. Camera XY blending: for vis ≤ framingFloor, camera XY is locked
//      (stable front-tile position / left margins).  For vis > framingFloor,
//      camera XY smoothly blends toward the actual-vis camera.
//   3. Adaptive camera Z: camera gets *closer* for fewer windows,
//      amplifying perspective separation between front and rear tiles.
//      zoom = camZmin + (1 - camZmin) * pow(vis/10, camZgamma)
// ---------------------------------------------------------------------------
void FlipScene::BuildSlots(uint32_t count, float vpW, float vpH)
{
    m_viewportAspect = vpW / vpH;

    // Dynamic sizing: all SceneConfig constants were tuned for 3440×1440.
    // Scale world positions to maintain consistent screen-space layout
    // across different viewport aspect ratios.
    static constexpr float kRefAspect = 3440.0f / 1440.0f;  // ~2.389
    const float hScale = m_viewportAspect / kRefAspect;

    // Depth compensation: scale Z-spacing so tile-to-tile visual separation
    // stays perceptually matched.  sqrt() gives partial scaling — tiles don't
    // bunch up too tight on narrower screens.
    const float depthScale = sqrtf(std::max(hScale, 0.1f));

    // Rise compensation: match depth scale so the cascade angle is consistent.
    // Portrait mode: boost rise to use extra vertical space.
    const float riseScale = (m_viewportAspect < 1.0f)
        ? std::min(1.0f / hScale, 3.0f)
        : depthScale;

    const uint32_t visible = std::min(count, m_cfg.maxVisible);
    if (visible == 0) { m_slots.clear(); return; }

    const float N = static_cast<float>(visible);

    // -----------------------------------------------------------------------
    // DYNAMIC DENSITY: adapt spacing/spread/rise to window count.
    // -----------------------------------------------------------------------
    const float expansion = std::clamp(10.0f / std::max(N, 1.0f), 1.0f, m_cfg.expMax);
    const float stepZ = m_cfg.stepZ10 * expansion * depthScale;

    // Rise: power law — nearly zero at low N, full at N=10.
    const float riseFrac  = powf(N / 10.0f, m_cfg.riseGamma);
    const float riseRatio = m_cfg.riseRatio10 * riseFrac;

    // FLAT spread — no expansion factor.
    const float spreadRatio = m_cfg.spreadRatio10;

    // Rear dip (downward tunnel) — only at 7+ windows.
    const float dipBlend = std::clamp((N - 7.0f) / 3.0f, 0.0f, 1.0f);
    const float rearDip  = m_cfg.rearDip10 * dipBlend;

    // -----------------------------------------------------------------------
    // Tile cascade geometry
    // -----------------------------------------------------------------------
    const float frontZ       = m_cfg.camDist;
    const float totalDepth   = (visible - 1) * stepZ;
    const float cascadeRise  = totalDepth * riseRatio * riseScale;
    const float totalSpreadX = totalDepth * spreadRatio * hScale;

    // -----------------------------------------------------------------------
    // CAMERA XY: blend between framingFloor camera and actual-vis camera.
    // This keeps the front tile at a stable screen position for vis ≤ FF,
    // then smoothly transitions as more windows appear.
    // -----------------------------------------------------------------------
    auto computeCamXY = [&](uint32_t cv) {
        struct CXY { float bX, eX, eY, tX, tY, tZ; };
        const float cN  = static_cast<float>(cv);
        const float cEx = std::clamp(10.0f / std::max(cN, 1.0f), 1.0f, m_cfg.expMax);
        const float csZ = m_cfg.stepZ10 * cEx * depthScale;
        const float crf = powf(cN / 10.0f, m_cfg.riseGamma);
        const float crr = m_cfg.riseRatio10 * crf;
        const float csr = m_cfg.spreadRatio10;
        const float ctd = (cv - 1) * csZ;
        const float ccr = ctd * crr * riseScale;
        const float ctx = ctd * csr * hScale;
        const float bX  = ctx * m_cfg.baseXfrac;
        const float lf  = m_cfg.lookFrac;
        return CXY {
            bX,
            bX * m_cfg.eyeXfrac,
            m_cfg.baseY + m_cfg.eyeYconst + ccr * m_cfg.eyeYbase,
            bX - ctx * lf,
            m_cfg.baseY + ccr * lf + m_cfg.lookYoff,
            frontZ + ctd * lf,
        };
    };
    auto cFF  = computeCamXY(m_cfg.framingFloor);
    auto cACT = computeCamXY(visible);
    const float bl = std::clamp(
        (static_cast<float>(visible) - static_cast<float>(m_cfg.framingFloor))
            / m_cfg.camTW,
        0.0f, 1.0f);
    auto lerp = [](float a, float b, float t){ return a + (b - a) * t; };

    const float baseX   = lerp(cFF.bX, cACT.bX, bl);
    const float camTgtX = lerp(cFF.tX, cACT.tX, bl);
    const float camTgtZ = lerp(cFF.tZ, cACT.tZ, bl);

    // Vis-dependent offsets: strongest at low N, zero at N=10
    const float n_frac = static_cast<float>(visible) / 10.0f;
    const float visFade = 1.0f - n_frac;   // 0.8 at vis=2, 0 at vis=10

    const float camEyeX = lerp(cFF.eX, cACT.eX, bl) + m_cfg.camXextra * visFade * hScale;
    const float camEyeY = lerp(cFF.eY, cACT.eY, bl) + m_cfg.camYextra * visFade * riseScale;
    const float camTgtY = lerp(cFF.tY, cACT.tY, bl) + m_cfg.lookYextra * visFade * riseScale;

    // Vis-dependent rotation
    m_tiltY_actual = m_cfg.tiltY + m_cfg.tiltYslope * n_frac;
    m_tiltX_actual = m_cfg.tiltX + m_cfg.tiltXslope * n_frac;

    // Vis-dependent global scale
    const float GS_actual = m_cfg.globalScale * (1.0f + m_cfg.globalScaleSlope * visFade);
    m_globalScale_actual = GS_actual;

    // -----------------------------------------------------------------------
    // ADAPTIVE CAMERA Z: closer for fewer windows → more perspective.
    // depthScale shrinks Z-separation on narrower screens, so also pull
    // the camera slightly closer to maintain the same visual size.
    // -----------------------------------------------------------------------
    const float nf   = powf(static_cast<float>(visible) / 10.0f, m_cfg.camZgamma);
    const float zoom = m_cfg.camZmin + (1.0f - m_cfg.camZmin) * nf;
    const float camEyeZ = frontZ - m_cfg.camDist * zoom * depthScale;

    // -----------------------------------------------------------------------
    // Place tiles
    // -----------------------------------------------------------------------
    const float minZGap = stepZ * 0.30f;
    m_slots.resize(visible);
    float prevZ = -1e9f;

    for (uint32_t i = 0; i < visible; ++i) {
        TileSlot& s = m_slots[i];

        float t = (visible > 1)
            ? static_cast<float>(i) / static_cast<float>(visible - 1)
            : 0.0f;

        float dt = powf(t, m_cfg.depthPower);
        float z = frontZ + totalDepth * dt;
        if (i > 0 && z < prevZ + minZGap)
            z = prevZ + minZGap;
        prevZ = z;

        float actualDt = (totalDepth > 0.01f)
            ? std::clamp((z - frontZ) / totalDepth, 0.0f, 1.0f)
            : 0.0f;

        s.x = baseX - totalSpreadX * actualDt;
        s.z = z;

        float yRise = cascadeRise * actualDt;
        float yDip  = rearDip * t * t;
        s.y = m_cfg.baseY + yRise - yDip;

        // Aspect-conditional shrink for narrow viewports — keeps the front
        // cascade tile inside the right edge of the screen at 16:9 / 16:10.
        // No-op at 3440×1440 (hScale = 1).  See TileSizeAspectScale().
        const float kRefAspect2 = 3440.0f / 1440.0f;
        const float hScaleTile = m_viewportAspect / kRefAspect2;
        const float tileAspectScale = (hScaleTile >= 1.0f)
            ? 1.0f
            : sqrtf(std::max(hScaleTile, 0.1f));
        s.scaleY = m_cfg.tileHeight * GS_actual * tileAspectScale;
        s.scaleX = m_cfg.tileHeight * GS_actual * tileAspectScale * m_viewportAspect;

        float baseAlpha = 1.0f - t * (1.0f - m_cfg.minAlpha);
        if (i >= m_cfg.fadeStart && visible > m_cfg.fadeStart) {
            float fadeFrac = static_cast<float>(i - m_cfg.fadeStart)
                           / static_cast<float>(m_cfg.maxVisible - m_cfg.fadeStart);
            baseAlpha *= (1.0f - std::clamp(fadeFrac, 0.0f, 1.0f));
        }
        s.alpha = std::max(baseAlpha, 0.0f);
    }

    // -----------------------------------------------------------------------
    // Store camera
    // -----------------------------------------------------------------------
    m_eyeX    = camEyeX;
    m_eyeY    = camEyeY;
    m_eyeZ    = camEyeZ;
    m_targetX = camTgtX;
    m_targetY = camTgtY;
    m_targetZ = camTgtZ;

    // --- Diagnostic ---
    {
        wchar_t buf[512];
        swprintf_s(buf,
            L"CKFlip LAYOUT: vp=%.0fx%.0f vis=%u hScale=%.3f depthScale=%.3f riseScale=%.2f blend=%.2f zoom=%.3f | "
            L"stepZ=%.3f rise=%.4f spread=%.2f depth=%.2f\n",
            vpW, vpH, visible, hScale, depthScale, riseScale, bl, zoom, stepZ, riseRatio, spreadRatio, totalDepth);
        CKLog::Log(buf);
        swprintf_s(buf,
            L"CKFlip CAMERA: Eye=(%.2f,%.2f,%.2f) Target=(%.2f,%.2f,%.2f)\n",
            m_eyeX, m_eyeY, m_eyeZ,
            m_targetX, m_targetY, m_targetZ);
        CKLog::Log(buf);
    }

    // Initialise per-window scale cache (will be filled by SetSlotScale).
    m_windowScales.resize(visible);
    for (uint32_t i = 0; i < visible; ++i)
        m_windowScales[i] = { m_slots[i].scaleX, m_slots[i].scaleY };
}

// ---------------------------------------------------------------------------
void FlipScene::SetSlotAspect(uint32_t index, float aspect)
{
    if (index < m_slots.size()) {
        m_slots[index].scaleX = m_slots[index].scaleY * aspect;
        // Cache the scale for rotation
        if (index < m_windowScales.size())
            m_windowScales[index] = { m_slots[index].scaleX, m_slots[index].scaleY };
    }
}

// ---------------------------------------------------------------------------
// Aspect-conditional tile-size correction.
//
// The cascade's X-spread (`baseX`, `totalSpreadX`) scales linearly with the
// viewport aspect via `hScale = vp_aspect / kRefAspect`, but per-window tile
// world-width depends only on the WINDOW aspect (independent of the viewport).
// At narrower viewports the perspective (proj[0][0] = 1/(aspect*tan(fov/2)))
// effectively magnifies tile screen coverage by ~1/sqrt(hScale), pushing the
// front cascade tile off the right edge for N >= 8 at 16:9 (1920×1080).
//
// This correction shrinks tiles only when the viewport is narrower than the
// 3440×1440 reference; at hScale >= 1 it is the identity, preserving the
// calibrated 3440×1440 look.  sqrt() is the empirical match for the
// projection-magnification factor.
static inline float TileSizeAspectScale(float viewportAspect)
{
    constexpr float kRefAspect = 3440.0f / 1440.0f;
    float hScale = viewportAspect / kRefAspect;
    if (hScale >= 1.0f) return 1.0f;
    return std::sqrt(std::max(hScale, 0.1f));
}

// ---------------------------------------------------------------------------
void FlipScene::SetSlotScale(uint32_t index, float widthPx, float heightPx,
                              float desktopW, float desktopH)
{
    if (index >= m_slots.size()) return;
    if (widthPx <= 0 || heightPx <= 0 || desktopW <= 0 || desktopH <= 0) return;

    TileSlot& s = m_slots[index];

    // Fraction of the desktop this window occupies vertically.
    // A maximized window ≈ 1.0, a small dialog ≈ 0.3.
    float vFrac = heightPx / desktopH;
    vFrac = std::max(0.15f, std::min(vFrac, 1.0f));  // clamp

    // Scale height relative to "full size" tile, with global shrink.
    float scale = m_globalScale_actual;

    // Aspect-conditional shrink for narrow viewports.  No-op at 3440×1440.
    const float tileAspectScale = TileSizeAspectScale(m_viewportAspect);
    scale *= tileAspectScale;

    // Scale height relative to "full size" tile, with global shrink
    s.scaleY = m_cfg.tileHeight * vFrac * scale;
    s.scaleX = s.scaleY * (widthPx / heightPx);

    // Cap aspect ratio to prevent overflow on ultrawide monitors.
    // 2.22 ≈ optimal for Win7-matching tile proportions at 3440×1440.
    const float kMaxAspect = std::min(2.22f, std::max(m_viewportAspect * 0.93f, 16.0f / 9.0f));
    if (s.scaleX > s.scaleY * kMaxAspect)
        s.scaleX = s.scaleY * kMaxAspect;

    // Cache the computed scale for rotation
    if (index < m_windowScales.size())
        m_windowScales[index] = { s.scaleX, s.scaleY };
}

// ---------------------------------------------------------------------------
void FlipScene::RotateAspects(bool forward)
{
    uint32_t n = static_cast<uint32_t>(m_windowScales.size());
    if (n < 2) return;

    // Rotate the cached per-window scales to match the window rotation
    if (forward)
        std::rotate(m_windowScales.begin(), m_windowScales.begin() + 1, m_windowScales.end());
    else
        std::rotate(m_windowScales.rbegin(), m_windowScales.rbegin() + 1, m_windowScales.rend());

    // Apply rotated scales to the slot grid
    for (uint32_t i = 0; i < n && i < static_cast<uint32_t>(m_slots.size()); ++i) {
        m_slots[i].scaleX = m_windowScales[i].sx;
        m_slots[i].scaleY = m_windowScales[i].sy;
    }
}

// ---------------------------------------------------------------------------
void FlipScene::GetDrawCall(uint32_t index, float viewportAspect,
                             XMFLOAT4X4& outMVP, float& outAlpha) const
{
    const TileSlot& s = m_slots[index];
    outAlpha = s.alpha;

    // --- World transform ---
    // Scale to tile dimensions, rotate X (pitch) then Y (perspective trapezoid),
    // then translate to slot position.
    // ALL tiles share the SAME rotation — parallel planes like Win7.
    // Both the optimizer's mat_rotY and DirectX XMMatrixRotationY share the
    // SAME sign convention: [0,2] = -sin(θ), [2,0] = +sin(θ).
    // No negation needed for either axis.
    XMMATRIX world =
        XMMatrixScaling(s.scaleX, s.scaleY, 1.0f) *
        XMMatrixRotationX(XMConvertToRadians(m_tiltX_actual)) *
        XMMatrixRotationY(XMConvertToRadians(m_tiltY_actual)) *
        XMMatrixTranslation(s.x, s.y, s.z);

    // --- View matrix (camera) ---
    XMVECTOR eye    = XMVectorSet(m_eyeX, m_eyeY, m_eyeZ, 1.0f);
    XMVECTOR target = XMVectorSet(m_targetX, m_targetY, m_targetZ, 1.0f);
    XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view   = XMMatrixLookAtLH(eye, target, up);

    // --- Perspective projection ---
    // Vertical FOV tuned to match the original cascade's perspective.
    // The perspective creates ALL the Flip3D magic:
    //   - front tile appears large (close to camera)
    //   - rear tiles shrink dramatically (far away)
    //   - off-axis camera makes the stack cascade diagonally
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(m_cfg.fovDeg), viewportAspect, 0.1f, 200.0f);

    XMMATRIX mvp = world * view * proj;
    XMStoreFloat4x4(&outMVP, mvp);
}
