#pragma once

#include <cmath>

/// Standard easing functions for animations.
/// All take t in [0,1] and return [0,1].
namespace Easing {

inline float Linear(float t) { return t; }

inline float InQuad(float t)    { return t * t; }
inline float OutQuad(float t)   { return t * (2.0f - t); }
inline float InOutQuad(float t) {
    return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
}

inline float InCubic(float t)    { return t * t * t; }
inline float OutCubic(float t)   { float u = t - 1.0f; return u * u * u + 1.0f; }
inline float InOutCubic(float t) {
    return t < 0.5f ? 4.0f * t * t * t
                    : 1.0f + (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f);
}

/// Organic ease-out: 1 − (1−t)^2.5 — fast start, long gentle deceleration.
/// Mimics the Win7 DWM animation feel.
inline float OutSmooth(float t) {
    float u = 1.0f - t;
    return 1.0f - powf(u, 2.5f);
}

inline float OutBack(float t) {
    const float c = 1.70158f;
    float u = t - 1.0f;
    return 1.0f + (c + 1.0f) * u * u * u + c * u * u;
}

inline float InOutBack(float t) {
    const float c = 1.70158f * 1.525f;
    if (t < 0.5f) {
        return (2.0f * t) * (2.0f * t) * ((c + 1.0f) * 2.0f * t - c) * 0.5f;
    } else {
        float u = 2.0f * t - 2.0f;
        return (u * u * ((c + 1.0f) * u + c) + 2.0f) * 0.5f;
    }
}

/// Typedef for easing function pointer.
using EaseFunc = float(*)(float);

} // namespace Easing
