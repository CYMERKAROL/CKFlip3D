using Slot = CKFlip3D.Settings.Views.PreviewScene.Slot;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// A managed port of animation/CycleAnimator's forward, non-chained
/// transition — the single Tab press the Appearance preview replays.
///
/// Kept faithful on purpose: the cascade's two wrap phases (swing out toward
/// the virtual N0 slot while fading, then slide in from behind the last slot),
/// Cover Flow's carousel behaviour and its end-of-row side swap, the OutCubic
/// slide for every other tile, and the alpha channel that goes with all of it.
/// The preview is only convincing if it fades the way the real one does.
/// </summary>
internal sealed class PreviewCycle
{
    public const double DurationMs = 220.0;      // CycleAnimator::kDurationMs

    private const double WrapFadeSplit = 0.40;   // 40 % swing-out, 60 % fade-in
    private const double N0Fwd = 0.15;           // virtual slot extrapolation
    private const double BackSpawn = 0.50;       // spawn 50 % past the last slot
    private const double ScaleBoost = 1.02;      // departing tile scale bump

    private Slot[] _start = [];
    private Slot[] _target = [];
    private Slot _n0, _backSpawn;
    private bool _carousel;

    public void Begin(Slot[] start, Slot[] target, bool carousel)
    {
        _start = start;
        _target = target;
        _carousel = carousel;
        ComputeN0();
        ComputeBackSpawn();
    }

    /// <summary>Forward rotation: destination slot i is fed by start slot i+1.</summary>
    private int SourceSlot(int i) => _start.Length == 0 ? 0 : (i + 1) % _start.Length;

    private bool IsSideSwapSlot(int i) =>
        _carousel && _start.Length == _target.Length
        && _start[SourceSlot(i)].RotY * _target[i].RotY < -0.001;

    private void ComputeN0()
    {
        if (_start.Length < 2) { _n0 = _start.Length > 0 ? _start[0] : default; _n0.Alpha = 0; return; }
        Slot s0 = _start[0], s1 = _start[1];
        _n0.X = s0.X + N0Fwd * (s0.X - s1.X);
        _n0.Y = s0.Y + N0Fwd * (s0.Y - s1.Y);
        _n0.Z = s0.Z + N0Fwd * (s0.Z - s1.Z);
        _n0.ScaleX = s0.ScaleX * ScaleBoost;
        _n0.ScaleY = s0.ScaleY * ScaleBoost;
        _n0.RotY = s0.RotY;
        _n0.Alpha = 0.0;
    }

    private void ComputeBackSpawn()
    {
        int n = _target.Length;
        if (n < 2) { _backSpawn = n > 0 ? _target[0] : default; _backSpawn.Alpha = 0; return; }
        Slot last = _target[n - 1], prev = _target[n - 2];
        _backSpawn.X = last.X + BackSpawn * (last.X - prev.X);
        _backSpawn.Y = last.Y + BackSpawn * (last.Y - prev.Y);
        _backSpawn.Z = last.Z + BackSpawn * (last.Z - prev.Z);
        _backSpawn.ScaleX = last.ScaleX;
        _backSpawn.ScaleY = last.ScaleY;
        _backSpawn.RotY = last.RotY;
        _backSpawn.Alpha = 0.0;
    }

    private static double OutQuad(double t) => t * (2.0 - t);
    private static double OutCubic(double t) { double u = t - 1.0; return u * u * u + 1.0; }

    /// <summary>Write the pose at raw time <paramref name="rawT"/> ∈ [0,1].</summary>
    public void Sample(double rawT, Slot[] output)
    {
        int n = _start.Length;
        if (n == 0 || _target.Length != n || output.Length < n) return;
        rawT = Math.Clamp(rawT, 0.0, 1.0);

        for (int i = 0; i < n; i++)
        {
            Slot src = _start[SourceSlot(i)];
            Slot dst = _target[i];
            ref Slot slot = ref output[i];

            // In the carousel there is no cascade wrap: the tile the cascade
            // would wrap simply slides sideways to the inner-left position.
            bool wrapping = !_carousel && i == n - 1;

            if (wrapping)
            {
                if (rawT < WrapFadeSplit)
                {
                    // Phase 1 — swing toward the camera and fade out. Z stays
                    // put so the fading tile cannot sort over the ones behind.
                    double p = rawT / WrapFadeSplit;
                    slot.X = src.X + (_n0.X - src.X) * p;
                    slot.Y = src.Y + (_n0.Y - src.Y) * p;
                    slot.Z = src.Z;
                    double bump = 1.0 + (ScaleBoost - 1.0) * 4.0 * p * (1.0 - p);
                    slot.ScaleX = src.ScaleX * bump;
                    slot.ScaleY = src.ScaleY * bump;
                    slot.RotY = src.RotY;
                    slot.Alpha = src.Alpha * (1.0 - OutQuad(p));
                }
                else
                {
                    // Phase 2 — appear behind the stack and slide into the back.
                    double p = (rawT - WrapFadeSplit) / (1.0 - WrapFadeSplit);
                    slot.X = _backSpawn.X + (dst.X - _backSpawn.X) * p;
                    slot.Y = _backSpawn.Y + (dst.Y - _backSpawn.Y) * p;
                    slot.Z = _backSpawn.Z + (dst.Z - _backSpawn.Z) * p;
                    slot.ScaleX = dst.ScaleX;
                    slot.ScaleY = dst.ScaleY;
                    slot.RotY = dst.RotY;
                    slot.Alpha = dst.Alpha * OutQuad(p);
                }
            }
            else if (IsSideSwapSlot(i))
            {
                // Cover Flow's row ends hand over to each other: fade out at the
                // source pose, fade in at the destination, never drag across.
                if (rawT < WrapFadeSplit)
                {
                    double p = rawT / WrapFadeSplit;
                    slot = src;
                    slot.Alpha = src.Alpha * (1.0 - OutQuad(p));
                }
                else
                {
                    double p = (rawT - WrapFadeSplit) / (1.0 - WrapFadeSplit);
                    slot = dst;
                    slot.Alpha = dst.Alpha * OutQuad(p);
                }
            }
            else
            {
                double ec = OutCubic(rawT);
                slot.X = src.X + (dst.X - src.X) * ec;
                slot.Y = src.Y + (dst.Y - src.Y) * ec;
                slot.Z = src.Z + (dst.Z - src.Z) * ec;
                slot.ScaleX = src.ScaleX + (dst.ScaleX - src.ScaleX) * ec;
                slot.ScaleY = src.ScaleY + (dst.ScaleY - src.ScaleY) * ec;
                slot.RotY = src.RotY + (dst.RotY - src.RotY) * ec;
                slot.Alpha = src.Alpha + (dst.Alpha - src.Alpha) * ec;
            }
        }
    }
}
