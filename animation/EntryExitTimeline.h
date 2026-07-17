#pragma once

/// Win7 Flip3D entry/exit timeline — 16 hand-authored samples driving six
/// blend channels.  Spec §4: the bake IS the curve.  No runtime easing
/// functions are layered on top — elapsed time selects an interval between
/// adjacent samples, and the channel values are linearly interpolated.
///
/// Channels (spec §4.2):
///   planarBlend  — fastest, drives x/y
///   depthBlend   — moderate lag, drives z and alpha
///   scaleBlend   — tracks depth, drives scaleX/scaleY
///   tiltBlend    — slightly delayed, drives scene-wide tilt
///   rotBlend     — most delayed, drives per-tile rotY
///                  (CKFlip's TileSlot has no rotY; sampled but inactive)
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

/// 16-sample authored timeline (spec §4.1).  Defined in EntryExitTimeline.cpp.
extern const Sample kTimeline[kSampleCount];

/// Map continuous timelineT in [0,1] to a Sample by linear interpolation
/// between the two adjacent authored rows (spec §5.2).  Pure function,
/// no state.
Sample SampleAt(float timelineT);

} // namespace EntryExitTimeline
