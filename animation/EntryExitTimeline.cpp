#include "EntryExitTimeline.h"

#include <algorithm>
#include <cmath>

namespace EntryExitTimeline {

// Round 4 — synchronised scale+tilt, front-loaded to match Win7 visual
// cadence.  Position (planar) lags behind so XY drift is muted and the
// cascade "spread" emerges via Z + tilt-driven perspective rather than
// translation.  All channels strictly monotonic; smooth tail prevents
// last-frame snapping.
//
// Sampled at frame N (0-indexed, 60fps capture, 17 frames spanning the
// 266.67ms morph): rawT = N / 16, falling between samples ⌊N×15/16⌋ and
// ⌈N×15/16⌉.  Frame 8 → rawT=0.4375 → samples 6-7; frame 16 → rawT=0.9375
// → samples 14-15; frame 17 → rawT=1.0 → final cascade.
const Sample kTimeline[kSampleCount] = {
    // planar  depth   scale   tilt    rot     dim
    // Bug 4 (v8.2) rebalance — row 1's tilt/planar ratio dropped from 33×
    // to 0.23× so the very first entry frame no longer snaps tilt while
    // position has barely moved.  All channels strictly non-decreasing
    // row-over-row; row 15 lands exactly at {1,1,1,1,0,1}.
    // Rows 1-7 reauthored (v8.5 Patch B) — much gentler front third so the
    // entry no longer pops into a strong 3D pose within the first ~quarter.
    // Tilt is near-zero in rows 1-2 (scale-coupled feel — see Win7 6nwin7).
    // Rows 8-15 unchanged (the tail was already fine).
    { 0.000f, 0.000f, 0.000f, 0.000f, 0.000f, 0.000f },  //  0
    { 0.025f, 0.020f, 0.025f, 0.001f, 0.000f, 0.090f },  //  1
    { 0.075f, 0.060f, 0.080f, 0.008f, 0.000f, 0.190f },  //  2
    { 0.150f, 0.125f, 0.165f, 0.030f, 0.000f, 0.295f },  //  3
    { 0.255f, 0.215f, 0.290f, 0.085f, 0.000f, 0.405f },  //  4
    { 0.385f, 0.330f, 0.420f, 0.175f, 0.000f, 0.510f },  //  5
    { 0.520f, 0.450f, 0.555f, 0.310f, 0.000f, 0.605f },  //  6
    { 0.645f, 0.560f, 0.670f, 0.470f, 0.000f, 0.685f },  //  7
    { 0.736f, 0.665f, 0.758f, 0.607f, 0.000f, 0.724f },  //  8
    { 0.803f, 0.728f, 0.823f, 0.691f, 0.000f, 0.781f },  //  9
    { 0.858f, 0.781f, 0.876f, 0.766f, 0.000f, 0.830f },  // 10
    { 0.903f, 0.826f, 0.919f, 0.832f, 0.000f, 0.872f },  // 11
    { 0.938f, 0.864f, 0.951f, 0.888f, 0.000f, 0.907f },  // 12
    { 0.965f, 0.898f, 0.974f, 0.933f, 0.000f, 0.937f },  // 13
    { 0.984f, 0.937f, 0.989f, 0.969f, 0.000f, 0.965f },  // 14
    { 1.000f, 1.000f, 1.000f, 1.000f, 0.000f, 1.000f },  // 15
};

Sample SampleAt(float timelineT)
{
    // Clamp to [0,1].  Spec §5.2: index = floor(t * 15), frac = t*15 - index.
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
