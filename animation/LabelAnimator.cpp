#include "LabelAnimator.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <algorithm>
#include <cmath>

void LabelAnimator::Reset()
{
    m_hasPos  = false;
    m_alpha   = 1.0f;
    m_lastQpc = 0;
}

void LabelAnimator::Update(float targetX, float targetY, bool suppressed)
{
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);

    if (!m_hasPos) {
        m_x = targetX;
        m_y = targetY;
        m_alpha   = suppressed ? 0.0f : 1.0f;
        m_hasPos  = true;
        m_lastQpc = now.QuadPart;
        return;
    }

    double dt = freq.QuadPart > 0
        ? static_cast<double>(now.QuadPart - m_lastQpc)
          / static_cast<double>(freq.QuadPart)
        : 1.0 / 60.0;
    m_lastQpc = now.QuadPart;
    dt = std::clamp(dt, 0.0, 0.25);

    // Exponential chase with a ~70 ms time constant: ~95% of a position
    // change is absorbed within ~0.21 s, so the label settles together
    // with the 220 ms cycle animation instead of trailing it.  Framerate-
    // independent by construction (k derives from measured dt).
    constexpr double kTau = 0.070;
    const float k = static_cast<float>(1.0 - std::exp(-dt / kTau));

    // Fade is a touch quicker than the motion so a hold reads as an
    // immediate hide rather than a lagging ghost.
    constexpr double kTauFade = 0.055;
    const float kf = static_cast<float>(1.0 - std::exp(-dt / kTauFade));
    const float alphaTarget = suppressed ? 0.0f : 1.0f;
    m_alpha += (alphaTarget - m_alpha) * kf;
    if (m_alpha < 0.001f) m_alpha = 0.0f;
    if (m_alpha > 0.999f) m_alpha = 1.0f;

    // Invisible → teleport: the glide is purely a visual nicety, and the
    // fade-in must happen at the final position, never mid-flight.
    if (m_alpha <= 0.02f) {
        m_x = targetX;
        m_y = targetY;
        return;
    }

    m_x += (targetX - m_x) * k;
    m_y += (targetY - m_y) * k;
}
