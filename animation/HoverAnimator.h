#pragma once

#include <vector>
#include <cstdint>

/// Pointer-hover lift.
///
/// The tile under the pointer rises slightly off the cascade and settles back
/// when the pointer leaves — the first half of the close transition's fall,
/// played upward, so the two motions read as one vocabulary.  Purely a DRAW
/// offset: this animator owns no slot state and never writes to FlipScene, so
/// it composes with the cycle, close and entry/exit animators instead of
/// competing with them for the same fields (which is what makes it safe to run
/// during any of them).
///
/// Timing is wall-clock QPC like every other animator here, so the rise is
/// frame-rate independent.  With the animation switched off (Appearance →
/// Animations → Hover lift) the lift snaps, which still reads as a highlight
/// but costs no motion.
class HoverAnimator {
public:
    /// Lift height as a fraction of the tile's own world height.  Deliberately
    /// well under CloseAnimator::kDropFactor (0.60) — this is a hint that the
    /// tile is live under the pointer, not an event in its own right.
    static constexpr float kRiseFactor = 0.20f;

    void Reset();

    /// Slot the pointer is over, or -1 for none.  Cheap to call every frame.
    void SetTarget(int slot);
    int  Target() const { return m_target; }

    /// Advance toward the target.  `slotCount` re-sizes the lift table (a
    /// rebuilt scene drops the stale entries); `animate` false snaps instead.
    void Tick(uint32_t slotCount, bool animate);

    /// Put every lifted tile back down on a fixed, short curve.
    ///
    /// Called when the stack starts moving: the raised tile settles back down
    /// WHILE the cascade carries it along, which is the one reading of "the
    /// window drops and the stack moves" that does not make either motion wait
    /// for the other.  Nothing here blocks or defers anything.
    ///
    /// A timed fall rather than the exponential approach because the approach
    /// needs ~6 tau — over half a second — to reach its own settle tolerance,
    /// and a fall that outlasts several cycles is a fall you can see landing on
    /// the wrong window.  100 ms fits inside one cycle by construction.
    ///
    /// A no-op when nothing is lifted, which is every frame of a session with
    /// the pointer feature off.
    void BeginDrop(bool animate);
    bool IsDropping() const { return m_dropping; }

    /// Follow the stack's own rotation: slot i's lift moves to the slot that
    /// window moves to.
    ///
    /// The lift is indexed by SLOT, and a cycle re-assigns every slot to a
    /// different window.  Without this, a fall in progress would finish on
    /// whichever window inherited the slot — one tile sagging for no reason in
    /// the middle of a transition it has nothing to do with, which is the
    /// twitch.  Rotated in step with the window and capture arrays, the falling
    /// tile is the one that was actually raised, all the way to its new place.
    ///
    /// `forward` matches the caller's std::rotate direction: true = begin+1
    /// (element 0 to the end), false = the reverse.  Exact wherever slot order
    /// is window order, which is the cascade preset and any stack whose windows
    /// all have slots.  Cover Flow with MORE windows than slots permutes the row
    /// at its two ends, so the arriving outer tile can inherit a few
    /// milliseconds of someone else's fall — on a tile that is fading in from
    /// alpha 0 at that moment, and for at most one drop's length.
    void Rotate(bool forward);

    /// 0 (flat) … 1 (fully lifted) for a slot.  Out-of-range reads 0, so the
    /// draw pass never has to bounds-check.
    float Lift(uint32_t slot) const {
        return slot < m_lift.size() ? m_lift[slot] : 0.0f;
    }

    /// True while anything is off the floor — lets the caller skip the whole
    /// offset path on the overwhelmingly common no-hover frame.
    bool AnyLift() const { return m_anyLift; }

private:
    // Exponential approach: ~63 % of the way in one tau, visually settled in
    // three.  90 ms reads as immediate without snapping.
    static constexpr float kTauSec = 0.090f;
    // The gated fall (see BeginDrop).  Short enough that the stack does not
    // feel held back, long enough that the tile is seen to land rather than
    // vanish — and eased out, so most of the distance is covered in the first
    // third of it.
    static constexpr float kDropSec = 0.100f;

    std::vector<float> m_lift;
    // Lift each slot was at when the drop began — the fall interpolates from
    // these, so a tile caught half way up drops from half way up.
    std::vector<float> m_dropFrom;
    int      m_target   = -1;
    bool     m_anyLift  = false;
    bool     m_dropping = false;
    int64_t  m_dropQPC  = 0;
    int64_t  m_lastQPC  = 0;
    int64_t  m_qpcFreq  = 0;

    void EnsureFreq();
};
