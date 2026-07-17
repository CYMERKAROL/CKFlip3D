#define NOMINMAX
#include <Windows.h>
#include "EntryExitAnimator.h"
#include "EntryExitTimeline.h"
#include "FlatStackBuilder.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// EntryExitAnimator — pure animation.  Knows nothing about window switching,
// foregrounding, capture lifecycle, or which tile is "selected".  It only:
//   - snapshots the cascade endpoint
//   - computes the flat endpoint from window rects + camera (FlatStackBuilder)
//   - interpolates between the two by sampling the 16-row authored timeline
//     (EntryExitTimeline) — no runtime easing, no sub-phases
//   - writes the interpolated TileSlots and scene tilt back to FlipScene
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

static inline float SmoothStep(float edge0, float edge1, float x)
{
    float t = (edge1 > edge0) ? (x - edge0) / (edge1 - edge0) : 0.0f;
    t = std::min(std::max(t, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

namespace {
constexpr float kFlatGateStart = 0.00f;   // Patch A2 spatial gate lower edge
constexpr float kFlatGateEnd   = 0.22f;   // Patch A2 spatial gate upper edge
constexpr float kMaxRawTStep   = 0.12f;   // v8.5 anti-stall per-Tick rawT cap
} // namespace

// ---------------------------------------------------------------------------
// Decide whether an overflow tile should be animated during entry.
//
// A window in m_windows beyond the cascade's `maxVisible` count is
// "overflow" — it doesn't have a cascade slot.  Animating it the same
// way as visible tiles would mash the back of the cascade with windows
// the user might or might not see; doing nothing makes a real, partly-
// visible window instantly vanish on activation.
//
// Compromise:
//   - Iconic windows: never animated (no visible on-screen geometry to
//     animate FROM, they live in the taskbar button).
//   - Desktop pseudo-window: never animated (the wallpaper backdrop
//     layer already covers it).
//   - Everything else: animated.  The previous strict region-subtraction
//     check excluded fully-occluded overflow windows, but in the user's
//     test (VS Code fullscreen behind ten maximised Explorers, all
//     genuinely fullscreen → vscode fully covered) the user still
//     expected vscode's tile to morph through the cascade.  Improving
//     the OVERFLOW animation curve (scaleBlend-driven α decay, same
//     planar/depth/scale blends as visible tiles) handles the mash
//     concern without dropping windows the user wants to see.
static bool ShouldAnimateOverflow(HWND hwnd, HWND desktopHwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;
    if (hwnd == desktopHwnd)
        return false;
    if (IsIconic(hwnd))
        return false;
    return true;
}

// ---------------------------------------------------------------------------
void EntryExitAnimator::ApplyState(FlipScene& scene,
                                    const std::vector<TileSlot>& slots,
                                    float tiltY, float tiltX) const
{
    uint32_t n = scene.SlotCount();
    uint32_t count = std::min(n, static_cast<uint32_t>(slots.size()));
    for (uint32_t i = 0; i < count; ++i)
        scene.GetSlotMut(i) = slots[i];
    scene.SetSceneTilt(tiltY, tiltX);
}

void EntryExitAnimator::ApplyWarmupEndpoint(FlipScene& scene)
{
    if (m_reverse) {
        // Exit's t=0 endpoint is the full cascade (timelineT = 1 - rawT = 1).
        ApplyState(scene, m_cascadeSlots, m_tiltYFinal, m_tiltXFinal);
        m_morphBlend = 1.0f;
        // Overflow tiles start the exit at their vanished cascade pose
        // (α=0) and fade in as the morph plays.
        for (size_t i = 0; i < m_overflowCurrent.size()
                        && i < m_overflowCollapsed.size(); ++i)
            m_overflowCurrent[i] = m_overflowCollapsed[i];
    } else {
        // Entry's t=0 endpoint is the flat 2D-rect stack, scene tilt 0.
        ApplyState(scene, m_flatSlots, 0.0f, 0.0f);
        m_morphBlend = 0.0f;
        // Hold overflow ("incinerating") tiles at their flat start pose so
        // they don't drift while the morph clock is pinned.
        for (size_t i = 0; i < m_overflowCurrent.size()
                        && i < m_overflowFlat.size(); ++i)
            m_overflowCurrent[i] = m_overflowFlat[i];
    }
}

// ---------------------------------------------------------------------------
void EntryExitAnimator::ClearOverflow()
{
    m_overflowFlat.clear();
    m_overflowCollapsed.clear();
    m_overflowCurrent.clear();
    m_overflowHwnds.clear();
    m_overflowFadeStart.clear();
    m_overflowFadeEnd.clear();
}

// ---------------------------------------------------------------------------
// Overflow choreography (windows beyond the visible slot count).
//
// Flat endpoint (timelineT = 0): the window's REAL desktop position —
// projected onto a plane strictly BEHIND the back-most cascade slot so
// painter's order can never put an overflow tile in front of the deck —
// at full alpha.  Frame 0 of entry therefore looks exactly like the
// desktop, overflow included.
//
// Cascade endpoint (timelineT = 1): a stack-adjacent card behind the back
// slot (per-tile deeper Z, slight shrink, source aspect preserved) at
// alpha 0.
//
// Tick lerps between the endpoints with the SAME planar/depth/scale blends
// as visible tiles and applies a per-tile staggered alpha window, so the
// overflow motion reads as part of the entry/exit morph instead of a
// one-frame teleport to the back of the stack.  Exit naturally mirrors
// entry because every input is a function of timelineT.
// ---------------------------------------------------------------------------
void EntryExitAnimator::BuildOverflowChoreography(
    const FlipScene& scene,
    const std::vector<WindowInfo>& windows,
    const std::vector<RECT>& stackRects,
    HWND desktopHwnd,
    float vpW, float vpH,
    float cascadeAspect,
    float originX, float originY,
    const DirectX::XMMATRIX& remapNDC)
{
    ClearOverflow();

    const size_t n = m_cascadeSlots.size();
    if (stackRects.size() <= n || n == 0)
        return;

    // Build the eligible-overflow list.  See ShouldAnimateOverflow()
    // for filtering rationale.
    std::vector<size_t> animatedIdx;
    animatedIdx.reserve(stackRects.size() - n);
    for (size_t k = 0; k + n < windows.size(); ++k) {
        const WindowInfo& w = windows[n + k];
        if (ShouldAnimateOverflow(w.hwnd, desktopHwnd))
            animatedIdx.push_back(k);
    }
    if (animatedIdx.empty())
        return;

    std::vector<RECT> overflowRects;
    overflowRects.reserve(animatedIdx.size());
    for (size_t k : animatedIdx) {
        if (n + k < stackRects.size())
            overflowRects.push_back(stackRects[n + k]);
    }

    // Project overflow tiles onto a plane BEHIND the back-most cascade
    // slot.  Using the cascade's natural focal depth (camDist) would put
    // them at the same Z as visible slot 0, rendering them in front of
    // the deck under painter's order (the old "out-of-bounds window
    // appears at N+1 / N-1" flicker).  The inverse projection keeps the
    // on-screen position exact regardless of plane depth.
    const TileSlot& backSlot = m_cascadeSlots[n - 1];
    const float overflowFlatZ = backSlot.z + FlatStackBuilder::kFlatZStep;

    m_overflowFlat = FlatStackBuilder::BuildFlatSlotsFromRects(
        overflowRects, scene, vpW, vpH, cascadeAspect,
        originX, originY, remapNDC,
        overflowFlatZ);

    const float vanishPushZ = FlatStackBuilder::kFlatZStep * 1.15f;

    m_overflowCollapsed.resize(m_overflowFlat.size());
    m_overflowHwnds.resize(m_overflowFlat.size());
    m_overflowCurrent.resize(m_overflowFlat.size());
    m_overflowFadeStart.resize(m_overflowFlat.size(), 0.0f);
    m_overflowFadeEnd.resize(m_overflowFlat.size(), 1.0f);

    for (size_t kk = 0; kk < m_overflowFlat.size(); ++kk) {
        // Flat endpoint: real-rect flat slot, stepped per tile so deeper
        // overflow stays strictly behind shallower one, fully opaque (the
        // alpha window in Tick owns the fade).
        TileSlot& flatPose = m_overflowFlat[kk];
        flatPose.z = overflowFlatZ
                   + static_cast<float>(kk) * FlatStackBuilder::kFlatZStep;
        flatPose.alpha = 1.0f;

        // Preserve the source window's aspect for the collapsed card.
        float sourceAspect = 1.0f;
        if (flatPose.scaleY > 0.001f)
            sourceAspect = flatPose.scaleX / flatPose.scaleY;
        sourceAspect = std::clamp(sourceAspect, 0.35f, 3.50f);

        // Cascade endpoint: stack-adjacent card behind the back slot,
        // slightly shrunk, fully transparent.  Z = flat-pose Z + a fixed
        // push so end.z > start.z for EVERY tile and the per-tile Z order
        // never crosses mid-lerp.
        float depthScaleMul = std::max(
            0.82f, 1.0f - 0.025f * static_cast<float>(kk));
        float baseH = backSlot.scaleY * depthScaleMul;

        TileSlot endPose = backSlot;
        endPose.z = flatPose.z + vanishPushZ;
        endPose.scaleY = baseH * 0.94f;
        endPose.scaleX = baseH * sourceAspect * 0.94f;
        endPose.alpha  = 0.0f;

        m_overflowCollapsed[kk] = endPose;
        m_overflowHwnds[kk]     = windows[n + animatedIdx[kk]].hwnd;
        m_overflowCurrent[kk]   = flatPose;
    }

    // Per-tile alpha windows in timelineT.  Stagger start by
    // kOverflowStaggerStep, clamped so the last tile still fades out
    // within the morph.
    constexpr float kStartClamp = 0.35f;
    constexpr float kEndClamp   = 0.85f;
    for (size_t kk = 0; kk < m_overflowFadeStart.size(); ++kk) {
        float start = std::min(
            kStartClamp,
            kOverflowStaggerStep * static_cast<float>(kk));
        float end = start + kOverflowFadeSpan;
        if (end > kEndClamp) end = kEndClamp;
        if (end <= start + 0.001f) end = start + 0.001f;
        m_overflowFadeStart[kk] = start;
        m_overflowFadeEnd[kk]   = end;
    }
}

// ---------------------------------------------------------------------------
void EntryExitAnimator::BeginEntry(FlipScene& scene,
                                    const std::vector<WindowInfo>& windows,
                                    float vpW, float vpH,
                                    float desktopW, float desktopH,
                                    HWND desktopHwnd,
                                    float cascadeAspect,
                                    float originX, float originY,
                                    const DirectX::XMMATRIX& remapNDC,
                                    DesktopEntryMode desktopMode,
                                    const std::vector<RECT>& taskbarRectOverrides,
                                    int64_t keyPressQPC)
{
    // 1. Snapshot the cascade as the t=1 endpoint.
    uint32_t n = scene.SlotCount();
    m_cascadeSlots.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_cascadeSlots[i] = scene.GetSlot(i);
    m_tiltYFinal = scene.GetSceneTiltY();
    m_tiltXFinal = scene.GetSceneTiltX();

    // 2. Build flat rects + flat slots from live window data + camera.
    //    Cascade slot data is NOT consulted — flat is its own endpoint.
    std::vector<RECT> stackRects, handoffRects;
    FlatStackBuilder::BuildStackRects(windows, vpW, vpH, desktopW, desktopH,
                                      stackRects, handoffRects);

    // 2b. Apply per-window taskbar-button rect overrides (minimized→taskbar
    //     emerge) — but ONLY for windows that actually have a visible
    //     cascade slot (i < n).  Overflow tiles incinerate from their
    //     real positions; stamping them at the taskbar just makes them
    //     flash through the whole cascade on the way to the back slot.
    if (!taskbarRectOverrides.empty() &&
        taskbarRectOverrides.size() == windows.size())
    {
        const size_t visibleN = static_cast<size_t>(n);
        for (size_t i = 0;
             i < stackRects.size()
             && i < taskbarRectOverrides.size()
             && i < visibleN;
             ++i)
        {
            const RECT& o = taskbarRectOverrides[i];
            if (o.right > o.left && o.bottom > o.top)
                stackRects[i] = o;
        }
    }

    // 2c. Build overflow choreography (indices >= n) from full stackRects
    //     BEFORE trimming — tiles start at their REAL desktop positions and
    //     fly toward the back of the stack while fading out, driven by the
    //     same blend channels as visible tiles.
    BuildOverflowChoreography(scene, windows, stackRects, desktopHwnd,
                              vpW, vpH, cascadeAspect,
                              originX, originY, remapNDC);

    // Trim to the visible slot count so flat parallels cascade in size.
    if (stackRects.size() > n) stackRects.resize(n);
    m_flatSourceRects = stackRects;
    m_flatSlots = FlatStackBuilder::BuildFlatSlotsFromRects(
        stackRects, scene, vpW, vpH, cascadeAspect,
        originX, originY, remapNDC);

    // 2d. Desktop tile entry mode (Bug 6).  Locate the desktop tile's
    //     visible slot.  For the fade modes, force its flat α to 0 so the
    //     per-frame smoothstep multiplier in Tick fully owns its
    //     appearance (late fade for HiddenUntilCascade, early fade for
    //     FadeFromFlat).  SelectedDesktop leaves the tile untouched.
    m_desktopEntryMode = desktopMode;
    m_desktopSlotIndex = -1;
    if (desktopHwnd) {
        for (size_t i = 0; i < m_flatSlots.size() && i < windows.size(); ++i) {
            if (windows[i].hwnd == desktopHwnd) {
                m_desktopSlotIndex = static_cast<int>(i);
                if (desktopMode != DesktopEntryMode::SelectedDesktop)
                    m_flatSlots[i].alpha = 0.0f;
                break;
            }
        }
    }

    // 2e. Minimized-from-taskbar fade-in: any visible-slot window whose
    //     flat rect was replaced by a taskbar-button override also gets
    //     α=0 at t=0 so the alpha lerp produces a 0→1 fade-in matching
    //     the desktop-tile behaviour.  m_flatSlots is already trimmed to
    //     the visible n, so the bounds check is implicit.
    if (!taskbarRectOverrides.empty() &&
        taskbarRectOverrides.size() == windows.size())
    {
        for (size_t i = 0; i < m_flatSlots.size() && i < taskbarRectOverrides.size(); ++i) {
            const RECT& o = taskbarRectOverrides[i];
            if (o.right > o.left && o.bottom > o.top)
                m_flatSlots[i].alpha = 0.0f;
        }
    }

    // 3. Cache by HWND for round-trip stability across cycles (Round-6 Fix 19).
    //    Also remember whether the entry-time flat rect was a taskbar override
    //    so BeginExit can skip the cache restoration for those HWNDs (the
    //    picked window must morph to its real restore rect on exit, not
    //    back into the taskbar).
    m_entryFlatHwnds.clear();
    m_entryFlatByHwnd.clear();
    m_entryFlatRectsByHwnd.clear();
    m_entryFlatHadTbOverride.clear();
    const bool overridesProvided =
        !taskbarRectOverrides.empty()
        && taskbarRectOverrides.size() == windows.size();
    for (size_t i = 0; i < m_flatSlots.size() && i < windows.size(); ++i) {
        m_entryFlatHwnds.push_back(windows[i].hwnd);
        m_entryFlatByHwnd.push_back(m_flatSlots[i]);
        m_entryFlatRectsByHwnd.push_back(
            (i < m_flatSourceRects.size()) ? m_flatSourceRects[i] : RECT{});
        bool hadTb = false;
        if (overridesProvided) {
            const RECT& o = taskbarRectOverrides[i];
            hadTb = (o.right > o.left && o.bottom > o.top);
        }
        m_entryFlatHadTbOverride.push_back(hadTb);
    }

    // 4. Overwrite scene with flat state so the very first rendered frame
    //    shows tiles at their real desktop positions (tilt = 0).
    ApplyState(scene, m_flatSlots, 0.0f, 0.0f);
    m_morphBlend = 0.0f;

    // 5. Anchor QPC.  m_firstTick re-anchors on the first Tick to absorb
    //    setup latency between Begin and the first RenderFrame.
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    m_qpcFreq      = freq.QuadPart;
    m_startQPC     = now.QuadPart;
    m_durationMs   = kDurationMs;
    m_active       = true;
    m_reverse      = false;
    m_justDoneExit = false;
    m_firstTick    = true;
    m_rawT         = 0.0f;

    // Launch-timing dim anchor: see BeginEntry() doc + DimFactor().
    m_keyPressQPC = keyPressQPC;
}

// ---------------------------------------------------------------------------
void EntryExitAnimator::BeginExit(FlipScene& scene,
                                   const std::vector<WindowInfo>& windows,
                                   const std::vector<uint32_t>& zRanks,
                                   float vpW, float vpH,
                                   float desktopW, float desktopH,
                                   HWND desktopHwnd,
                                   float cascadeAspect,
                                   float originX, float originY,
                                   const DirectX::XMMATRIX& remapNDC,
                                   const std::vector<bool>& tileFadeOutOnExit,
                                   bool animateOverflow)
{
    m_exitFadeOut.assign(scene.SlotCount(), false);
    for (size_t i = 0; i < m_exitFadeOut.size() && i < tileFadeOutOnExit.size(); ++i)
        m_exitFadeOut[i] = tileFadeOutOnExit[i];


    // 1. Snapshot cascade as the start (timelineT = 1) state.
    uint32_t n = scene.SlotCount();
    m_cascadeSlots.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        m_cascadeSlots[i] = scene.GetSlot(i);
    m_tiltYFinal = scene.GetSceneTiltY();
    m_tiltXFinal = scene.GetSceneTiltX();

    // 2. Compute fresh flat rects (windows may have moved since entry).
    std::vector<RECT> stackRects, handoffRects;
    FlatStackBuilder::BuildStackRects(windows, vpW, vpH, desktopW, desktopH,
                                      stackRects, handoffRects);

    // 2a. Mirror the entry's overflow choreography: windows beyond the
    //     visible slot count fade in from behind the cascade and land at
    //     their real desktop positions as the exit completes (the blends
    //     run on timelineT, so the same data plays backward).  Skipped
    //     when the picked target hides them anyway (desktop selection).
    if (animateOverflow)
        BuildOverflowChoreography(scene, windows, stackRects, desktopHwnd,
                                  vpW, vpH, cascadeAspect,
                                  originX, originY, remapNDC);
    else
        ClearOverflow();

    if (stackRects.size() > n) stackRects.resize(n);
    m_flatSourceRects = stackRects;
    m_flatSlots = FlatStackBuilder::BuildFlatSlotsFromRects(
        stackRects, scene, vpW, vpH, cascadeAspect,
        originX, originY, remapNDC);

    // 3. Round-6 Fix 19 — prefer cached entry flat slots for matching HWNDs
    //    so the exit lerp's flat aspect matches what the entry started from.
    //
    //    Cache-skip rule: only the SELECTED tile (slot 0) skips the cache
    //    when its entry-flat was a taskbar-button override.  That makes the
    //    picked minimized window animate to its real restore rect instead
    //    of shrinking back into the taskbar.  Non-selected minimized windows
    //    KEEP the cached taskbar emerge rect so their tiles shrink+slide
    //    toward the taskbar while their α decays — the "minimize preview"
    //    visual the user wants.
    for (size_t i = 0; i < m_flatSlots.size() && i < windows.size(); ++i) {
        HWND h = windows[i].hwnd;
        for (size_t j = 0; j < m_entryFlatHwnds.size(); ++j) {
            if (m_entryFlatHwnds[j] == h) {
                bool hadTb = (j < m_entryFlatHadTbOverride.size())
                             && m_entryFlatHadTbOverride[j];
                bool isSelected = (i == 0);
                if (!(hadTb && isSelected)) {
                    m_flatSlots[i] = m_entryFlatByHwnd[j];
                    if (j < m_entryFlatRectsByHwnd.size()
                        && i < m_flatSourceRects.size())
                        m_flatSourceRects[i] = m_entryFlatRectsByHwnd[j];
                }
                break;
            }
        }
    }

    // All tiles must be fully opaque at the flat endpoint.  Entry may
    // have set α=0 on some flat slots (desktop fade-in, taskbar-emerge);
    // carrying that into exit would lerp those tiles toward invisible,
    // breaking the "no opacity changes during exit" invariant.
    for (auto& f : m_flatSlots)
        f.alpha = 1.0f;

    // Bug 8'' — endpoint flat Z override.  Override only m_flatSlots[i].z;
    // cascade snapshot, frozen SRVs, m_windows/m_captures order, and
    // texture identity are NOT touched.  The cascade-to-flat lerp draws
    // each tile from its original cascade Z to the rewritten flat Z.
    // Empty zRanks (Escape path) skips the override entirely.
    if (!zRanks.empty()) {
        const float flatBase = scene.GetCamEyeZ() + scene.GetCamDist();
        const size_t limit = std::min(m_flatSlots.size(), zRanks.size());
        for (size_t i = 0; i < limit; ++i) {
            m_flatSlots[i].z = flatBase +
                static_cast<float>(zRanks[i]) * FlatStackBuilder::kFlatZStep;
        }
    }

    // 4. Scene stays in cascade state (it's the t=1 / start of reverse).
    //    No ApplyState call — the cascade is already what's rendered.

    // 5. Anchor QPC.
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    m_qpcFreq      = freq.QuadPart;
    m_startQPC     = now.QuadPart;
    m_durationMs   = kDurationMs;
    m_active       = true;
    m_reverse      = true;
    m_justDoneExit = false;
    m_firstTick    = true;
    m_rawT         = 0.0f;
    m_morphBlend   = 1.0f;
}

// ---------------------------------------------------------------------------
void EntryExitAnimator::Tick(FlipScene& scene)
{
    if (!m_active)
        return;

    uint32_t n = scene.SlotCount();
    if (n == 0 || m_flatSlots.size() != n || m_cascadeSlots.size() != n) {
        // Route through Finalize so m_active flips to false via the same
        // path that lands the scene at a clean endpoint and (for exit)
        // sets m_justDoneExit.  Closes the softlock vector where the
        // scene froze mid-morph with m_active=false but no Finalize.
        Finalize(scene);
        return;
    }

    // Absorb setup latency between Begin*() and the first RenderFrame(), and
    // guarantee the first rendered animation frame is an exact endpoint.
    if (m_firstTick) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        m_startQPC  = now.QuadPart;
        m_firstTick = false;
        ApplyWarmupEndpoint(scene);
        m_rawT = 0.0f;
        return;
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMs = static_cast<double>(now.QuadPart - m_startQPC) * 1000.0
                     / static_cast<double>(m_qpcFreq);

    // Hard wall-clock ceiling.  If QPC behaves oddly (process suspend,
    // debugger break, GPU stall pushing elapsed past 2× the morph duration)
    // snap to the endpoint so we never get stuck on a partial-morph frame.
    if (elapsedMs >= static_cast<double>(m_durationMs) * 2.0) {
        Finalize(scene);
        return;
    }

    float newRawT = static_cast<float>(elapsedMs / m_durationMs);
    newRawT = std::min(std::max(newRawT, 0.0f), 1.0f);

    // v8.5 anti-stall clamp.  The morph is wall-clock driven, so a single
    // slow rendered frame (PrintWindow capture, GPU hitch, compositor
    // stall) would otherwise let rawT teleport deep into the morph between
    // two visible frames — the perceived "snap into 3D".  Cap how far rawT
    // may advance per Tick and re-anchor m_startQPC so the morph simply
    // continues smoothly from the clamped point (it takes marginally longer
    // in wall time, but never jumps).  On smooth playback (≤~30 fps) the
    // per-frame step stays under the cap, so this is a no-op.
    if (newRawT > m_rawT + kMaxRawTStep) {
        newRawT = m_rawT + kMaxRawTStep;
        m_startQPC = now.QuadPart - static_cast<int64_t>(
            static_cast<double>(newRawT) * static_cast<double>(m_durationMs)
            * static_cast<double>(m_qpcFreq) / 1000.0);
    }
    m_rawT = newRawT;

    float timelineT = m_reverse ? (1.0f - m_rawT) : m_rawT;
    EntryExitTimeline::Sample s = EntryExitTimeline::SampleAt(timelineT);

    // v8.5 Patch A2 — flat-endpoint spatial gate.
    //
    // Patch A caps tilt, but planar/depth/scale still enter at full
    // authored strength on the first visible animated frame — so the
    // scene snaps into a strong 3D/perspective pose by frame ~2 (the eye
    // reads the abrupt scale-shrink + depth-separation as a perspective
    // snap even when tilt itself is tiny).  Win7 reference frames keep
    // the first frames near-flat, then ease into 3D.
    //
    // Gate the spatial channels near the flat endpoint, for BOTH
    // directions (entry start AND exit end — timelineT is small at the
    // flat endpoint either way).  dimBlend is intentionally NOT gated:
    // background dimming may legitimately begin early.  Because the tilt
    // coupling below reads s.scaleBlend, the gated scale also pulls the
    // scale-coupled tilt cap down in the first frames.
    const float flatGate = SmoothStep(kFlatGateStart, kFlatGateEnd, timelineT);
    {
        s.planarBlend *= flatGate;
        s.depthBlend  *= flatGate;
        s.scaleBlend  *= flatGate;
    }

    // The endpoint write removes the one-frame cascade flash that
    // made early tilt look like a snap.  Do not add an extra time/scale² cap
    // here: it delays tilt relative to position and scale.  The authored
    // timeline already interpolates tilt frame-to-frame from the flat endpoint.

    // Late-exit smoothing.  Without it, the timeline's first sample row
    // (tilt/scale 0.10, planar 0.003) leaves a small but visibly non-
    // zero residual tilt + planar at exit's last interpolated frame
    // (rawT ≈ 0.97-0.99) — Finalize then snaps everything to true flat
    // and the user sees a one-frame jump.  Fullscreen windows are
    // particularly affected (the planar offset to slot 0's cascade x
    // shows as a right-shift).
    //
    // Bug 1A (v8.2) — smoothing window rawT 0.65 → 1.00 with CUBIC
    // decay and a sub-threshold clamp.  The cubic gives a gentler, longer
    // deceleration into the flat endpoint than the old quadratic; the
    // clamp snaps near-zero residuals fully to zero so Finalize has
    // nothing left to snap.  Entry path (`!m_reverse`) untouched.
    if (m_reverse) {
        constexpr float kFadeStart = 0.65f;
        constexpr float kFadeEnd   = 1.00f;
        if (m_rawT > kFadeStart) {
            float u = (m_rawT - kFadeStart) / (kFadeEnd - kFadeStart);
            if (u > 1.0f) u = 1.0f;
            float inv  = 1.0f - u;
            float ease = inv * inv * inv;     // CUBIC
            if (ease < 0.002f) ease = 0.0f;   // sub-threshold clamp
            s.planarBlend *= ease; s.depthBlend *= ease;
            s.scaleBlend *= ease; s.tiltBlend *= ease;
        }
    }

    // Per spec §5.3 — every tile uses the same blend factors.
    std::vector<TileSlot> work(n);
    for (uint32_t i = 0; i < n; ++i) {
        const TileSlot& F = m_flatSlots[i];
        const TileSlot& C = m_cascadeSlots[i];
        TileSlot& o = work[i];
        o.x      = Lerp(F.x,      C.x,      s.planarBlend);
        o.y      = Lerp(F.y,      C.y,      s.planarBlend);
        o.z      = Lerp(F.z,      C.z,      s.depthBlend);
        o.scaleX = Lerp(F.scaleX, C.scaleX, s.scaleBlend);
        o.scaleY = Lerp(F.scaleY, C.scaleY, s.scaleBlend);
        o.alpha  = Lerp(F.alpha,  C.alpha,  s.depthBlend);
        // s.rotBlend is sampled but unapplied (TileSlot has no rotY in CKFlip).
    }

    // Bug 6 — desktop pseudo-tile entry fade.  In the fade modes the
    // desktop tile starts at flat α=0; a per-frame smoothstep multiplier
    // controls when it becomes visible (late for HiddenUntilCascade,
    // early for FadeFromFlat).  Entry-only; exit desktop fade is handled
    // by m_exitFadeOut.
    if (!m_reverse
        && m_desktopSlotIndex >= 0
        && static_cast<uint32_t>(m_desktopSlotIndex) < n
        && m_desktopEntryMode != DesktopEntryMode::SelectedDesktop)
    {
        float mul = (m_desktopEntryMode == DesktopEntryMode::FadeFromFlat)
                    ? SmoothStep(0.10f, 0.50f, m_rawT)
                    : SmoothStep(0.35f, 0.85f, m_rawT);
        work[static_cast<uint32_t>(m_desktopSlotIndex)].alpha *= mul;
    }

    // Exit uses pure spatial layering — no per-tile alpha changes.
    // Each tile keeps its interpolated alpha (lerp between flat α=1
    // and cascade α) throughout the morph.  Painter's-algorithm draw
    // order (back-to-front by Z) determines visibility; tiles at the
    // same 2D spot are stacked by their unique Z layer, matching Win7
    // Flip3D exit behaviour.

    // Bug 5 — consume m_exitFadeOut.  Tiles flagged by the controller
    // (non-selected minimized windows, desktop tile when not picked, etc.)
    // decay to α=0 across the reverse morph via an inverted OutQuad.
    // BeginExit populates m_exitFadeOut; Escape's empty vector skips this.
    if (m_reverse && !m_exitFadeOut.empty()) {
        float u = m_rawT;
        float fadeMul = 1.0f - u * (2.0f - u);   // inverted OutQuad: 1 → 0
        for (uint32_t i = 0; i < n; ++i) {
            if (i < m_exitFadeOut.size() && m_exitFadeOut[i]) {
                work[i].alpha *= fadeMul;
            }
        }
    }

    float tiltY = m_tiltYFinal * s.tiltBlend;
    float tiltX = m_tiltXFinal * s.tiltBlend;
    ApplyState(scene, work, tiltY, tiltX);
    m_morphBlend = std::clamp(s.scaleBlend, 0.0f, 1.0f);

    // Overflow choreography — direction-agnostic.  Lerp each overflow tile
    // between its real-position flat pose and its behind-the-stack vanish
    // pose using the same blend channels as visible tiles (sampled at
    // timelineT, so exit mirrors entry), then override α with a per-tile
    // staggered OutQuad window over [fadeStart, fadeEnd] in timelineT:
    // entry fades the tiles out on the way back, exit fades them in on the
    // way to their real desktop positions.
    if (!m_overflowFlat.empty()) {
        m_overflowCurrent.resize(m_overflowFlat.size());
        for (size_t k = 0; k < m_overflowFlat.size(); ++k) {
            const TileSlot& F = m_overflowFlat[k];
            const TileSlot& C = m_overflowCollapsed[k];
            TileSlot& o = m_overflowCurrent[k];
            o.x      = Lerp(F.x,      C.x,      s.planarBlend);
            o.y      = Lerp(F.y,      C.y,      s.planarBlend);
            o.z      = Lerp(F.z,      C.z,      s.depthBlend);
            o.scaleX = Lerp(F.scaleX, C.scaleX, s.scaleBlend);
            o.scaleY = Lerp(F.scaleY, C.scaleY, s.scaleBlend);

            // Bug 9'' — per-tile staggered fade window in timelineT.
            float fadeStart = (k < m_overflowFadeStart.size()) ? m_overflowFadeStart[k] : 0.0f;
            float fadeEnd   = (k < m_overflowFadeEnd.size())   ? m_overflowFadeEnd[k]   : 1.0f;
            float windowLen = std::max(0.001f, fadeEnd - fadeStart);
            float fadeT     = std::clamp((timelineT - fadeStart) / windowLen, 0.0f, 1.0f);
            float fadeOut   = 1.0f - fadeT;
            o.alpha = F.alpha * (fadeOut * fadeOut);
        }
    }

    if (m_rawT >= 1.0f)
        Finalize(scene);
}

// ---------------------------------------------------------------------------
void EntryExitAnimator::Finalize(FlipScene& scene)
{
    if (m_reverse) {
        // Exit ends in flat state with no tilt.
        std::vector<TileSlot> finalSlots = m_flatSlots;
        for (uint32_t i = 0; i < finalSlots.size(); ++i) {
            if (i < m_exitFadeOut.size() && m_exitFadeOut[i])
                finalSlots[i].alpha = 0.0f;
        }
        ApplyState(scene, finalSlots, 0.0f, 0.0f);
        m_morphBlend = 0.0f;
        m_justDoneExit = true;
        // Exit's flat endpoint includes the overflow windows at their real
        // positions (α=1) so the final presented frame matches the desktop
        // the overlay is about to reveal.  Vectors are discarded by
        // ClearEntryFlatCache during the controller teardown.
        m_overflowCurrent = m_overflowFlat;
    } else {
        // Entry ends in cascade state with full tilt; overflow tiles have
        // fully incinerated — discard so the controller doesn't draw stale
        // quads at α≈0.
        ApplyState(scene, m_cascadeSlots, m_tiltYFinal, m_tiltXFinal);
        m_morphBlend = 1.0f;
        ClearOverflow();
    }
    m_active    = false;
    m_firstTick = false;
    m_rawT      = m_reverse ? 0.0f : 1.0f;
}

// ---------------------------------------------------------------------------
bool EntryExitAnimator::ReverseInPlace()
{
    if (!m_active || m_reverse)
        return false;

    // Map current entry rawT → exit rawT so the timelineT applied this frame
    // (= rawT_entry for forward, = 1 - rawT for reverse) stays equal to its
    // current value and then decreases toward 0 as exit progresses.
    float rawTNew = 1.0f - m_rawT;
    if (rawTNew < 0.0f) rawTNew = 0.0f;
    if (rawTNew > 1.0f) rawTNew = 1.0f;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    // Anchor m_startQPC so elapsedMs == rawTNew * m_durationMs at "now".
    double offsetMs = static_cast<double>(rawTNew) * static_cast<double>(m_durationMs);
    int64_t offsetTicks = static_cast<int64_t>(offsetMs * static_cast<double>(m_qpcFreq) / 1000.0);
    m_startQPC  = now.QuadPart - offsetTicks;
    m_rawT      = rawTNew;
    m_reverse   = true;
    m_firstTick = false;
    m_justDoneExit = false;
    return true;
}

// ---------------------------------------------------------------------------
bool EntryExitAnimator::JustFinishedExit()
{
    if (m_justDoneExit) {
        m_justDoneExit = false;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
float EntryExitAnimator::DimFactor() const
{
    // Inactive between sessions: full target dim.  Exception: right after
    // an instant-snap entry (animations toggle off → Finalize straight from
    // BeginEntry) the keypress-anchored dim ramp below keeps running, so
    // the wallpaper still fades to the target instead of jumping.  A stale
    // anchor from an old session clamps to timelineT=1 → same 1.0 as before.
    if (!m_active) {
        if (!m_reverse && m_keyPressQPC != 0 && m_qpcFreq > 0) {
            constexpr double kDimDurationMs = 100.0;
            LARGE_INTEGER nowQpc{};
            QueryPerformanceCounter(&nowQpc);
            double elapsedMs = static_cast<double>(nowQpc.QuadPart - m_keyPressQPC)
                             * 1000.0
                             / static_cast<double>(m_qpcFreq);
            float timelineT = static_cast<float>(elapsedMs / kDimDurationMs);
            if (timelineT < 0.0f) timelineT = 0.0f;
            if (timelineT > 1.0f) timelineT = 1.0f;
            return EntryExitTimeline::SampleAt(timelineT).dimBlend;
        }
        return 1.0f;
    }

    float timelineT;
    if (m_reverse) {
        // Exit: standard timeline ramp (1 → 0 as rawT goes 0 → 1).
        timelineT = 1.0f - m_rawT;
    } else if (m_keyPressQPC != 0 && m_qpcFreq > 0) {
        // Entry with launch-timing active.  Dim is anchored to the
        // user's keypress (not BeginEntry / first Tick), so the wall-
        // clock time spent on EnsureFrame / first-content-render / Show
        // counts toward dim progress.  First visible frame's dim is
        // wherever it would have been if the dim had run since keypress.
        //
        // Dim duration is INTENTIONALLY shorter than the cascade morph
        // (266 ms): on slow GPUs the previous full-duration ramp left
        // dim at ~98% on the first visible frame and the user perceived
        // the remaining 2% ramp as a flash.  100 ms means slow loads
        // (≥100 ms keypress→present) clamp to full dim instantly with
        // no visible ramp; fast loads still get a smooth ramp from
        // (loadTime / 100) → 1 over the remainder.  Cascade tile
        // motion timing is unaffected.
        constexpr double kDimDurationMs = 100.0;
        LARGE_INTEGER nowQpc{};
        QueryPerformanceCounter(&nowQpc);
        double elapsedMs = static_cast<double>(nowQpc.QuadPart - m_keyPressQPC)
                         * 1000.0
                         / static_cast<double>(m_qpcFreq);
        timelineT = static_cast<float>(elapsedMs / kDimDurationMs);
    } else {
        // Entry without launch-timing: original behaviour.
        timelineT = m_rawT;
    }
    if (timelineT < 0.0f) timelineT = 0.0f;
    if (timelineT > 1.0f) timelineT = 1.0f;
    EntryExitTimeline::Sample s = EntryExitTimeline::SampleAt(timelineT);
    return s.dimBlend;
}
