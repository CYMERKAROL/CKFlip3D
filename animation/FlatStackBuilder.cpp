#define NOMINMAX
#include "FlatStackBuilder.h"

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace FlatStackBuilder {

// ---------------------------------------------------------------------------
RECT ResolveSourceRect(const WindowInfo& w, float desktopW, float desktopH)
{
    // 1. Live WindowInfo.rect when valid (non-zero, non-empty).
    if (w.rect.right > w.rect.left && w.rect.bottom > w.rect.top) {
        return w.rect;
    }

    // 2. Minimised window — use the rect it would restore to.
    if (w.hwnd && IsIconic(w.hwnd)) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(WINDOWPLACEMENT);
        if (GetWindowPlacement(w.hwnd, &wp)) {
            const RECT& r = wp.rcNormalPosition;
            if (r.right > r.left && r.bottom > r.top)
                return r;
        }
    }

    // 3. GetWindowRect fallback.
    if (w.hwnd) {
        RECT r{};
        if (GetWindowRect(w.hwnd, &r) && r.right > r.left && r.bottom > r.top)
            return r;
    }

    // 4. Centred 52% fallback (preserves desktop aspect).
    float halfW = desktopW * 0.26f;   // 52% / 2
    float halfH = desktopH * 0.26f;
    float cx    = desktopW * 0.5f;
    float cy    = desktopH * 0.5f;
    RECT r{
        static_cast<LONG>(cx - halfW),
        static_cast<LONG>(cy - halfH),
        static_cast<LONG>(cx + halfW),
        static_cast<LONG>(cy + halfH)
    };
    return r;
}

// ---------------------------------------------------------------------------
// `vpW` / `vpH` aren't used in this implementation (kept in the
// signature so an inverse-projection refactor can restore them
// without rippling through callers).  Suppress C4100 explicitly.
void BuildStackRects(const std::vector<WindowInfo>& windows,
                     float /*vpW*/, float /*vpH*/,
                     float desktopW, float desktopH,
                     std::vector<RECT>& outStackRects,
                     std::vector<RECT>& outHandoffRects)
{
    const size_t n = windows.size();
    outHandoffRects.resize(n);
    outStackRects.resize(n);

    // Resolve raw rects first (the handoff set — unmodified, spec §7.2).
    for (size_t i = 0; i < n; ++i) {
        outHandoffRects[i] = ResolveSourceRect(windows[i], desktopW, desktopH);
    }

    if (n == 0) return;

    // Each window's flat rect = its real on-screen rect.  No rank shift,
    // no centre clamp, no monitor stretch.  Tiles start where the user
    // actually has them on the desktop and animate toward their cascade
    // slots; Z-spacing in BuildFlatSlotsFromRects gives painter's-algorithm
    // draw order.  Both endpoints (real position, cascade slot) are inside
    // the viewport, so the lerp stays bounded — no out-of-bounds excursions.
    for (size_t i = 0; i < n; ++i) {
        outStackRects[i] = outHandoffRects[i];
    }
}

// ---------------------------------------------------------------------------
// Inverse-project a screen-space pixel point to a world-space point on the
// z = flatZ plane.  Uses the scene's camera (view * proj) — the same camera
// that renders the cascade, so flat tiles project to the same on-screen
// positions as the original windows.
static XMFLOAT3 ScreenToWorldOnPlane(float pxX, float pxY,
                                     float vpW, float vpH,
                                     float flatZ,
                                     const XMMATRIX& viewProjInv)
{
    // Pixel -> NDC.  D3D NDC: x in [-1,+1] L->R, y in [-1,+1] B->T.
    // Screen Y is top-down, so flip.
    float nx =  (pxX / vpW) * 2.0f - 1.0f;
    float ny =  1.0f - (pxY / vpH) * 2.0f;

    // Two points along the picking ray (near plane at NDC z=0, far at z=1
    // for D3D LH).  Inverse-project both, divide by w, and intersect the
    // segment with the z = flatZ world plane.
    XMVECTOR pNear = XMVectorSet(nx, ny, 0.0f, 1.0f);
    XMVECTOR pFar  = XMVectorSet(nx, ny, 1.0f, 1.0f);
    XMVECTOR wNear = XMVector4Transform(pNear, viewProjInv);
    XMVECTOR wFar  = XMVector4Transform(pFar,  viewProjInv);

    float wnW = XMVectorGetW(wNear);
    float wfW = XMVectorGetW(wFar);
    if (std::fabs(wnW) < 1e-6f) wnW = 1e-6f;
    if (std::fabs(wfW) < 1e-6f) wfW = 1e-6f;
    XMFLOAT3 nearW{ XMVectorGetX(wNear)/wnW, XMVectorGetY(wNear)/wnW, XMVectorGetZ(wNear)/wnW };
    XMFLOAT3 farW { XMVectorGetX(wFar )/wfW, XMVectorGetY(wFar )/wfW, XMVectorGetZ(wFar )/wfW };

    float dz = farW.z - nearW.z;
    if (std::fabs(dz) < 1e-6f) {
        // Ray parallel to the flat plane — degenerate; fall back to nearW.
        return nearW;
    }
    float t = (flatZ - nearW.z) / dz;
    return XMFLOAT3{
        nearW.x + (farW.x - nearW.x) * t,
        nearW.y + (farW.y - nearW.y) * t,
        flatZ
    };
}

struct ScreenBounds {
    float minX;
    float maxX;
    float minY;
    float maxY;
};

static ScreenBounds ProjectSlotBounds(float cx, float cy,
                                      float scaleX, float scaleY,
                                      float slotZ,
                                      float vpW, float vpH,
                                      const XMMATRIX& viewProj)
{
    const float local[4][2] = {
        { -0.5f,  0.5f },
        {  0.5f,  0.5f },
        { -0.5f, -0.5f },
        {  0.5f, -0.5f }
    };

    ScreenBounds b{
        1.0e30f, -1.0e30f,
        1.0e30f, -1.0e30f
    };

    for (int i = 0; i < 4; ++i) {
        const float x = cx + local[i][0] * scaleX;
        const float y = cy + local[i][1] * scaleY;
        XMVECTOR p = XMVectorSet(x, y, slotZ, 1.0f);
        XMVECTOR clip = XMVector4Transform(p, viewProj);
        float w = XMVectorGetW(clip);
        if (std::fabs(w) < 1.0e-6f)
            w = (w < 0.0f) ? -1.0e-6f : 1.0e-6f;

        const float ndcX = XMVectorGetX(clip) / w;
        const float ndcY = XMVectorGetY(clip) / w;
        const float px = (ndcX + 1.0f) * 0.5f * vpW;
        const float py = (1.0f - ndcY) * 0.5f * vpH;

        if (px < b.minX) b.minX = px;
        if (px > b.maxX) b.maxX = px;
        if (py < b.minY) b.minY = py;
        if (py > b.maxY) b.maxY = py;
    }

    return b;
}

static void ComputeEdgeError(const ScreenBounds& b, const RECT& r, float out[4])
{
    out[0] = b.minX - static_cast<float>(r.left);
    out[1] = b.maxX - static_cast<float>(r.right);
    out[2] = b.minY - static_cast<float>(r.top);
    out[3] = b.maxY - static_cast<float>(r.bottom);
}

static bool Solve4x4(float a[4][5], float out[4])
{
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float best = std::fabs(a[col][col]);
        for (int row = col + 1; row < 4; ++row) {
            const float v = std::fabs(a[row][col]);
            if (v > best) {
                best = v;
                pivot = row;
            }
        }
        if (best < 1.0e-6f)
            return false;
        if (pivot != col) {
            for (int k = col; k < 5; ++k) {
                const float tmp = a[col][k];
                a[col][k] = a[pivot][k];
                a[pivot][k] = tmp;
            }
        }

        const float div = a[col][col];
        for (int k = col; k < 5; ++k)
            a[col][k] /= div;

        for (int row = 0; row < 4; ++row) {
            if (row == col)
                continue;
            const float f = a[row][col];
            for (int k = col; k < 5; ++k)
                a[row][k] -= f * a[col][k];
        }
    }

    for (int i = 0; i < 4; ++i)
        out[i] = a[i][4];
    return true;
}

static void RefineSlotToScreenBounds(float& cx, float& cy,
                                     float& scaleX, float& scaleY,
                                     float slotZ,
                                     const RECT& r,
                                     float vpW, float vpH,
                                     const XMMATRIX& viewProj)
{
    float params[4] = { cx, cy, scaleX, scaleY };
    for (int iter = 0; iter < 8; ++iter) {
        ScreenBounds b = ProjectSlotBounds(params[0], params[1], params[2], params[3],
                                           slotZ, vpW, vpH, viewProj);
        float err[4];
        ComputeEdgeError(b, r, err);

        float maxErr = 0.0f;
        for (float e : err) {
            const float ae = std::fabs(e);
            if (ae > maxErr) maxErr = ae;
        }
        if (maxErr < 0.25f)
            break;

        float jac[4][5] = {};
        for (int j = 0; j < 4; ++j) {
            float step = std::fabs(params[j]) * 0.001f;
            if (step < 0.001f) step = 0.001f;
            float test[4] = { params[0], params[1], params[2], params[3] };
            test[j] += step;
            if (j >= 2 && test[j] < 0.001f)
                test[j] = 0.001f;

            ScreenBounds tb = ProjectSlotBounds(test[0], test[1], test[2], test[3],
                                                slotZ, vpW, vpH, viewProj);
            float terr[4];
            ComputeEdgeError(tb, r, terr);
            for (int row = 0; row < 4; ++row)
                jac[row][j] = (terr[row] - err[row]) / step;
        }
        for (int row = 0; row < 4; ++row)
            jac[row][4] = -err[row];

        float delta[4] = {};
        if (!Solve4x4(jac, delta))
            break;

        for (int j = 0; j < 4; ++j) {
            float limit = (j < 2) ? 10.0f : std::fabs(params[j]) * 0.5f;
            if (limit < 0.01f) limit = 0.01f;
            if (delta[j] > limit) delta[j] = limit;
            if (delta[j] < -limit) delta[j] = -limit;
            params[j] += delta[j];
        }
        if (params[2] < 0.001f) params[2] = 0.001f;
        if (params[3] < 0.001f) params[3] = 0.001f;
    }

    cx = params[0];
    cy = params[1];
    scaleX = params[2];
    scaleY = params[3];
}

// ---------------------------------------------------------------------------
std::vector<TileSlot> BuildFlatSlotsFromRects(
    const std::vector<RECT>& stackRects,
    const FlipScene& scene,
    float vpW, float vpH,
    float cascadeAspect,
    float originX, float originY,
    const DirectX::XMMATRIX& remapNDC,
    float flatZOverride)
{
    std::vector<TileSlot> out;
    const size_t n = stackRects.size();
    out.resize(n);
    if (n == 0 || vpW <= 0.0f || vpH <= 0.0f) return out;

    // Build the same view*proj that GetDrawCall uses, but without any scene
    // tilt (flat state has tilt=0).  Then invert it.
    XMVECTOR eye    = XMVectorSet(scene.GetCamEyeX(),    scene.GetCamEyeY(),    scene.GetCamEyeZ(),    1.0f);
    XMVECTOR target = XMVectorSet(scene.GetCamTargetX(), scene.GetCamTargetY(), scene.GetCamTargetZ(), 1.0f);
    XMVECTOR up     = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view   = XMMatrixLookAtLH(eye, target, up);
    XMMATRIX proj   = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(scene.GetFovDeg()), cascadeAspect, 0.1f, 200.0f);
    XMMATRIX viewProj    = view * proj * remapNDC;
    XMMATRIX viewProjInv = XMMatrixInverse(nullptr, viewProj);

    // Anchor the flat plane at the cascade's natural focal depth (camDist
    // forward of the camera eye, matching where cascade slot 0 sits).
    // This keeps slot 0's flat Z ≈ cascade Z so the morph only changes
    // X/Y/scale for the front tile, and back-slot depth lerps cleanly
    // from flatZ + i*kFlatZStep into the cascade's depth spline.
    // Anchored relative to the camera only — no cascade slot data read.
    //
    // Caller may override the plane depth (used by overflow path so out-
    // of-bounds tiles project at a deeper Z and can never paint in front
    // of the visible cascade).  The inverse-projection still picks the
    // correct world (x,y) for the rect's screen pixels at the chosen Z.
    float flatZ = (flatZOverride > 0.0f)
        ? flatZOverride
        : (scene.GetCamEyeZ() + scene.GetCamDist());

    for (size_t i = 0; i < n; ++i) {
        const RECT& r = stackRects[i];
        RECT overlayRect{
            static_cast<LONG>(static_cast<float>(r.left) - originX),
            static_cast<LONG>(static_cast<float>(r.top) - originY),
            static_cast<LONG>(static_cast<float>(r.right) - originX),
            static_cast<LONG>(static_cast<float>(r.bottom) - originY)
        };
        float L = static_cast<float>(overlayRect.left);
        float T = static_cast<float>(overlayRect.top);
        float R = static_cast<float>(overlayRect.right);
        float B = static_cast<float>(overlayRect.bottom);

        const float slotZ = flatZ + static_cast<float>(i) * kFlatZStep;

        // 4 corners projected to world-space at the same Z this slot will
        // render at.  Re-projecting per slot preserves screen corners with
        // the off-axis perspective camera.
        XMFLOAT3 wTL = ScreenToWorldOnPlane(L, T, vpW, vpH, slotZ, viewProjInv);
        XMFLOAT3 wTR = ScreenToWorldOnPlane(R, T, vpW, vpH, slotZ, viewProjInv);
        XMFLOAT3 wBL = ScreenToWorldOnPlane(L, B, vpW, vpH, slotZ, viewProjInv);
        XMFLOAT3 wBR = ScreenToWorldOnPlane(R, B, vpW, vpH, slotZ, viewProjInv);

        // World-space centre and full extents (mesh quad spans -0.5..+0.5,
        // so scaleX/scaleY equal the full world-space width/height).
        float cx = (wTL.x + wTR.x + wBL.x + wBR.x) * 0.25f;
        float cy = (wTL.y + wTR.y + wBL.y + wBR.y) * 0.25f;
        float worldW = std::fabs(((wTR.x - wTL.x) + (wBR.x - wBL.x)) * 0.5f);
        float worldH = std::fabs(((wTL.y - wBL.y) + (wTR.y - wBR.y)) * 0.5f);
        RefineSlotToScreenBounds(cx, cy, worldW, worldH, slotZ,
                                 overlayRect, vpW, vpH, viewProj);

        TileSlot& s = out[i];
        s.x      = cx;
        s.y      = cy;
        s.z      = slotZ;
        s.scaleX = worldW;
        s.scaleY = worldH;
        s.alpha  = 1.0f;
    }

    return out;
}

} // namespace FlatStackBuilder
