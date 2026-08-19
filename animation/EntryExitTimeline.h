// ---------------------------------------------------------------------------
// The baked entry/exit curve.  Sixteen hand-authored samples drive six blend
// channels, and the table itself is the curve: no easing function runs on top
// of it, elapsed time just picks an interval and the values are interpolated.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#pragma once

/// Win7 Flip3D entry/exit timeline: 16 hand-authored samples driving six
/// blend channels.  The bake IS the curve.  No runtime easing functions are
/// layered on top — elapsed time selects an interval between adjacent
/// samples, and the channel values are linearly interpolated.
///
/// Channels:
///   planarBlend  — fastest, drives x/y
///   depthBlend   — moderate lag, drives z and alpha
///   scaleBlend   — tracks depth, drives scaleX/scaleY
///   tiltBlend    — slightly delayed, drives scene-wide tilt
///   rotBlend     — most delayed, drives per-tile rotY (flat 0 → slot
///                  rotY; a no-op in the cascade preset where every slot
///                  rotY is 0, active for Cover Flow side tiles)
///   dimBlend     — fastest of all, drives background dim multiplier
namespace EntryExitTimeline {

struct Sample {
    float planarBlend;
    float depthBlend;
    float scaleBlend;
    float tiltBlend;
    float rotBlend;
    float dimBlend;
};

constexpr int kSampleCount = 16;

/// 16-sample authored timeline.  Defined in EntryExitTimeline.cpp.
extern const Sample kTimeline[kSampleCount];

/// Map continuous timelineT in [0,1] to a Sample by linear interpolation
/// between the two adjacent authored rows.  Pure function, no state.
Sample SampleAt(float timelineT);

} // namespace EntryExitTimeline
