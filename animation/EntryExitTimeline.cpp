// ---------------------------------------------------------------------------
// The authored sample table and the interpolator that reads it.  The values
// were matched frame by frame against a 60fps capture of the real Win7 morph.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "EntryExitTimeline.h"

#include <algorithm>
#include <cmath>

namespace EntryExitTimeline {

// Scale and tilt run together and front-loaded, matching the Win7 cadence.
// Position (planar) lags behind, so XY drift stays muted and the cascade
// "spread" emerges out of Z plus tilt-driven perspective rather than out of
// translation.  All channels are strictly monotonic, and the tail is smooth
// enough that the last frame does not snap.
//
// Sampled at frame N (0-indexed, 60fps capture, 17 frames spanning the
// 266.67ms morph): rawT = N / 16, falling between samples ⌊N×15/16⌋ and
// ⌈N×15/16⌉.  Frame 8 → rawT=0.4375 → samples 6-7; frame 16 → rawT=0.9375
// → samples 14-15; frame 17 → rawT=1.0 → final cascade.
const Sample kTimeline[kSampleCount] = {
    // planar  depth   scale   tilt    rot     dim
    // Rows are non-decreasing column by column and row 15 lands exactly on
    // {1,1,1,1,1,1}.  The front third is deliberately gentle: entry must not
    // pop into a strong 3D pose within the first quarter, so tilt stays near
    // zero through rows 1-2 and only picks up once scale has moved.
    //
    // The rot column exists for the Cover Flow preset (per-tile rotY).  It is
    // the most delayed channel: flat through the first fifth, then trailing
    // tilt, so side tiles turn inward late on entry and unwind first on exit.
    // For the cascade preset it is a no-op, since every slot rotY is 0 there
    // and both lerp endpoints are equal no matter what the blend says.
    { 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f },  //  0
    { 0.025f, 0.020f, 0.025f, 0.001f, 0.000f, 0.090f },  //  1
    { 0.075f, 0.060f, 0.080f, 0.008f, 0.000f, 0.190f },  //  2
    { 0.150f, 0.125f, 0.165f, 0.030f, 0.012f, 0.295f },  //  3
    { 0.255f, 0.215f, 0.290f, 0.085f, 0.050f, 0.405f },  //  4
    { 0.385f, 0.330f, 0.420f, 0.175f, 0.125f, 0.510f },  //  5
    { 0.520f, 0.450f, 0.555f, 0.310f, 0.240f, 0.605f },  //  6
    { 0.645f, 0.560f, 0.670f, 0.470f, 0.395f, 0.685f },  //  7
    { 0.736f, 0.665f, 0.758f, 0.607f, 0.540f, 0.724f },  //  8
    { 0.803f, 0.728f, 0.823f, 0.691f, 0.640f, 0.781f },  //  9
    { 0.858f, 0.781f, 0.876f, 0.766f, 0.725f, 0.830f },  // 10
    { 0.903f, 0.826f, 0.919f, 0.832f, 0.800f, 0.872f },  // 11
    { 0.938f, 0.864f, 0.951f, 0.888f, 0.866f, 0.907f },  // 12
    { 0.965f, 0.898f, 0.974f, 0.933f, 0.922f, 0.937f },  // 13
    { 0.984f, 0.937f, 0.989f, 0.969f, 0.968f, 0.965f },  // 14
    { 1.000f, 1.000f, 1.000f, 1.000f, 1.000f, 1.000f },  // 15
};

Sample SampleAt(float timelineT)
{
    float t = std::min(std::max(timelineT, 0.0f), 1.0f);
    float pos = t * static_cast<float>(kSampleCount - 1);
    int   i0  = static_cast<int>(std::floor(pos));
    int   i1  = std::min(i0 + 1, kSampleCount - 1);
    float f   = pos - static_cast<float>(i0);

    const Sample& a = kTimeline[i0];
    const Sample& b = kTimeline[i1];
    Sample out;
    out.planarBlend = a.planarBlend + (b.planarBlend - a.planarBlend) * f;
    out.depthBlend  = a.depthBlend  + (b.depthBlend  - a.depthBlend)  * f;
    out.scaleBlend  = a.scaleBlend  + (b.scaleBlend  - a.scaleBlend)  * f;
    out.tiltBlend   = a.tiltBlend   + (b.tiltBlend   - a.tiltBlend)   * f;
    out.rotBlend    = a.rotBlend    + (b.rotBlend    - a.rotBlend)    * f;
    out.dimBlend    = a.dimBlend    + (b.dimBlend    - a.dimBlend)    * f;
    return out;
}

} // namespace EntryExitTimeline
