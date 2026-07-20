#pragma once

/// Smooths the selected-window label's screen-space anchor.  Windows of
/// different sizes swap through the front slot while cycling, so the
/// projected tile top (and with it the label position) moves — without
/// smoothing the label teleports.  Deliberately a standalone animator:
/// the label is an overlay element, not cascade geometry, so it must not
/// live inside CycleAnimator.
///
/// Usage: feed the instantaneous target once per rendered frame via
/// Update(); read the smoothed position back with X()/Y().  The first
/// Update() after Reset() snaps straight to the target (session start,
/// no stale glide).
class LabelAnimator {
public:
    /// Forget the current position — the next Update() snaps to target.
    void Reset();

    /// Advance toward (targetX, targetY).  dt is measured internally via
    /// QPC, so call cadence does not need to be uniform.
    ///
    /// `suppressed` fades the label out (held-key rapid cycling — chasing
    /// every intermediate pose would make the label dart around) and back
    /// in when the hold ends.  While fully faded the position SNAPS to the
    /// target, so the fade-in always happens at the final position.
    void Update(float targetX, float targetY, bool suppressed = false);

    bool  HasPos() const { return m_hasPos; }
    float X() const { return m_x; }
    float Y() const { return m_y; }
    /// Current fade opacity in [0,1] (1 = fully visible).
    float Alpha() const { return m_alpha; }

private:
    bool      m_hasPos  = false;
    float     m_x       = 0.0f;
    float     m_y       = 0.0f;
    float     m_alpha   = 1.0f;
    long long m_lastQpc = 0;
};
