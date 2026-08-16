using System.Windows.Media.Media3D;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// A managed port of the core's stack geometry — scene/FlipScene.cpp
/// (cascade) and scene/CoverFlowLayout.cpp (carousel) — used ONLY by the
/// Appearance preview.
///
/// It is a port, not a re-imagining: the constants, the dynamic-density
/// spacing, the adaptive camera and the per-window tile sizing are the same
/// formulas, so the miniature is laid out exactly like the real thing at the
/// viewer's own screen aspect. Nothing here is compiled into the core, and
/// the core does not read anything from here — keep the two in step by hand
/// if the layout ever changes.
/// </summary>
internal sealed class PreviewScene
{
    // ---- SceneConfig (scene/FlipScene.hpp) ---------------------------------
    private const double TileHeight = 5.0;
    private const double MinAlpha = 0.88;
    private const double TiltYBase = 3.596;
    private const double GlobalScale = 0.563;
    private const double GlobalScaleSlope = 0.031;
    private const double CamDist = 17.098;
    public const double FovDeg = 16.385;
    private const double StepZ10 = 2.216;
    private const double RiseRatio10 = 0.117;
    private const double SpreadRatio10 = 0.300;
    private const double RearDip10 = 1.113;
    private const double RiseGamma = 0.100;
    private const double BaseXfrac = 24.994;
    private const double EyeXfrac = 0.956;
    private const double EyeYconst = 1.428;
    private const double LookFrac = 0.262;
    private const double LookYoff = -0.621;
    private const double CamXextra = -3.716;
    private const double CamYextra = -0.537;
    private const double LookYextra = 1.261;
    private const double CamZmin = 1.140;
    private const double CamZgamma = 2.962;
    private const double CamTW = 4.603;
    private const double BaseY = 23.303;
    private const double DepthPower = 0.965;
    private const int MaxVisible = 10;
    private const double ExpMax = 2.679;
    private const int FramingFloor = 5;
    private const int FadeStart = 8;
    private const double RefAspect = 3440.0 / 1440.0;

    // ---- CoverFlowLayout constants -----------------------------------------
    private const double CfSideAngleDeg = 30.0;
    private const double CfSideDepth = 1.20;
    private const double CfDepthStep = 0.35;
    private const double CfTileScale = 0.86;
    private const double CfCamZoom = 1.22;
    private const double CfEyeLift = 1.10;
    private const double CfLookDrop = 0.35;
    private const double CfCenterGapPad = 0.30;
    private const double CfGapProjFrac = 0.60;
    private const double CfSpacingFrac = 0.62;
    private const double CfFitMargin = 0.98;
    private const double CfTypAspect = 16.0 / 9.0;
    // Long rows recede instead of being squeezed — see CoverFlowLayout.cpp.
    private const int CfComfortSide = 2;
    private const double CfLongDepthAdd = 0.50;
    private const double CfLongFade = 0.45;
    private const double CfLongEdgeMargin = 0.99;
    private const double CfMaxSpanStrips = 1.90;
    private const double CfMinRowFit = 0.42;

    private static int CfSides(int visible) => Math.Max(1, visible / 2);

    /// <summary>Managed twin of CoverFlowLayout::PlaceOuterEdge.</summary>
    private (double ax, double span, double inner) CfPlaceOuterEdge(
        double halfW, double z, double edge, double leanDeg, double k0)
    {
        double rad = leanDeg * Math.PI / 180.0;
        double c = Math.Cos(rad), sn = Math.Sin(rad);
        double dOut = Math.Max(0.001, z - halfW * sn - EyeZ);
        double dIn = Math.Max(0.001, z + halfW * sn - EyeZ);
        double ax = edge * k0 * dOut - halfW * c;
        return (ax,
                (ax + halfW * c) / (k0 * dOut) - (ax - halfW * c) / (k0 * dIn),
                (ax - halfW * c) / (k0 * dIn));
    }

    /// <summary>Managed twin of CoverFlowLayout::RowFitsTwoDeep.</summary>
    private bool CfRowFitsTwoDeep(double halfW, int rightCount, int leftCount,
                                  double frontZ, double depthStep, double k0)
    {
        if (k0 <= 0.0 || halfW <= 0.0) return true;
        double centreEdge = halfW / (k0 * Math.Max(0.001, frontZ - EyeZ));
        double budget = CfLongEdgeMargin - centreEdge;
        if (budget <= 0.0) return false;
        for (int pass = 0; pass < 2; pass++)
        {
            int count = pass == 0 ? rightCount : leftCount;
            if (count == 0) continue;
            double strip = budget / count;
            for (int a = 1; a <= count; a++)
            {
                var p = CfPlaceOuterEdge(halfW, frontZ + CfSideDepth + (a - 1) * depthStep,
                                         centreEdge + strip * a, CfSideAngleDeg, k0);
                if (p.span > CfMaxSpanStrips * strip || p.inner < 0.0) return false;
            }
        }
        return true;
    }

    /// <summary>One tile's placement — the managed twin of TileSlot.</summary>
    public struct Slot
    {
        public double X, Y, Z, ScaleX, ScaleY, Alpha, RotY;
    }

    public Slot[] Slots { get; private set; } = [];
    public double EyeX, EyeY, EyeZ, TargetX, TargetY, TargetZ;
    public double TiltY, TiltX;

    private double _aspect = RefAspect;
    private double _globalScaleActual = GlobalScale;
    private double _floorY;
    private bool _coverFlow;

    /// <summary>
    /// Lay the stack out for <paramref name="sizes"/> windows (pixel sizes on
    /// a desktop of <paramref name="desktopW"/> × <paramref name="desktopH"/>),
    /// mirroring BuildSlots + SetSlotScale + RelayoutCoverFlowX.
    /// </summary>
    public void Build(IReadOnlyList<(double W, double H)> sizes, double aspect, bool coverFlow,
                      double desktopW, double desktopH)
    {
        _aspect = aspect;
        _coverFlow = coverFlow;
        int visible = Math.Min(sizes.Count, MaxVisible);
        if (visible == 0) { Slots = []; return; }

        if (coverFlow) BuildCoverFlow(visible);
        else BuildCascade(visible);

        for (int i = 0; i < visible; i++)
            SetSlotScale(i, sizes[i].W, sizes[i].H, desktopW, desktopH);

        if (coverFlow) RelayoutCoverFlowX();
    }

    // =====================================================================
    // Cascade (scene/FlipScene.cpp BuildSlots)
    // =====================================================================
    private void BuildCascade(int visible)
    {
        double hScale = _aspect / RefAspect;
        double depthScale = Math.Sqrt(Math.Max(hScale, 0.1));
        double riseScale = _aspect < 1.0 ? Math.Min(1.0 / hScale, 3.0) : depthScale;

        double n = visible;
        double expansion = Math.Clamp(10.0 / Math.Max(n, 1.0), 1.0, ExpMax);
        double stepZ = StepZ10 * expansion * depthScale;
        double riseRatio = RiseRatio10 * Math.Pow(n / 10.0, RiseGamma);
        double rearDip = RearDip10 * Math.Clamp((n - 7.0) / 3.0, 0.0, 1.0);

        double frontZ = CamDist;
        double totalDepth = (visible - 1) * stepZ;
        double cascadeRise = totalDepth * riseRatio * riseScale;
        double totalSpreadX = totalDepth * SpreadRatio10 * hScale;

        (double bX, double eX, double eY, double tX, double tY, double tZ) CamXY(int cv)
        {
            double cN = cv;
            double cEx = Math.Clamp(10.0 / Math.Max(cN, 1.0), 1.0, ExpMax);
            double csZ = StepZ10 * cEx * depthScale;
            double crr = RiseRatio10 * Math.Pow(cN / 10.0, RiseGamma);
            double ctd = (cv - 1) * csZ;
            double ccr = ctd * crr * riseScale;
            double ctx = ctd * SpreadRatio10 * hScale;
            double bX = ctx * BaseXfrac;
            return (bX,
                    bX * EyeXfrac,
                    BaseY + EyeYconst,          // eyeYbase is 0 in SceneConfig
                    bX - ctx * LookFrac,
                    BaseY + ccr * LookFrac + LookYoff,
                    frontZ + ctd * LookFrac);
        }

        var ff = CamXY(FramingFloor);
        var act = CamXY(visible);
        double bl = Math.Clamp((visible - (double)FramingFloor) / CamTW, 0.0, 1.0);
        static double Lerp(double a, double b, double t) => a + (b - a) * t;

        double baseX = Lerp(ff.bX, act.bX, bl);
        TargetX = Lerp(ff.tX, act.tX, bl);
        TargetZ = Lerp(ff.tZ, act.tZ, bl);

        double visFade = 1.0 - visible / 10.0;
        EyeX = Lerp(ff.eX, act.eX, bl) + CamXextra * visFade * hScale;
        EyeY = Lerp(ff.eY, act.eY, bl) + CamYextra * visFade * riseScale;
        TargetY = Lerp(ff.tY, act.tY, bl) + LookYextra * visFade * riseScale;

        TiltY = TiltYBase;
        TiltX = 0.0;
        _globalScaleActual = GlobalScale * (1.0 + GlobalScaleSlope * visFade);

        double zoom = CamZmin + (1.0 - CamZmin) * Math.Pow(visible / 10.0, CamZgamma);
        EyeZ = frontZ - CamDist * zoom * depthScale;

        double minZGap = stepZ * 0.30;
        Slots = new Slot[visible];
        double prevZ = -1e9;
        double tileAspectScale = TileSizeAspectScale(_aspect);

        for (int i = 0; i < visible; i++)
        {
            double t = visible > 1 ? i / (double)(visible - 1) : 0.0;
            double z = frontZ + totalDepth * Math.Pow(t, DepthPower);
            if (i > 0 && z < prevZ + minZGap) z = prevZ + minZGap;
            prevZ = z;

            double actualDt = totalDepth > 0.01
                ? Math.Clamp((z - frontZ) / totalDepth, 0.0, 1.0) : 0.0;

            ref Slot s = ref Slots[i];
            s.X = baseX - totalSpreadX * actualDt;
            s.Z = z;
            s.Y = BaseY + cascadeRise * actualDt - rearDip * t * t;
            s.RotY = 0.0;
            s.ScaleY = TileHeight * _globalScaleActual * tileAspectScale;
            s.ScaleX = s.ScaleY * _aspect;

            double alpha = 1.0 - t * (1.0 - MinAlpha);
            if (i >= FadeStart && visible > FadeStart)
                alpha *= 1.0 - Math.Clamp((i - FadeStart) / (double)(MaxVisible - FadeStart), 0.0, 1.0);
            s.Alpha = Math.Max(alpha, 0.0);
        }
    }

    private static double TileSizeAspectScale(double aspect)
    {
        double hScale = aspect / RefAspect;
        return hScale >= 1.0 ? 1.0 : Math.Sqrt(Math.Max(hScale, 0.1));
    }

    private void SetSlotScale(int index, double widthPx, double heightPx,
                              double desktopW, double desktopH)
    {
        if (index >= Slots.Length || widthPx <= 0 || heightPx <= 0
            || desktopW <= 0 || desktopH <= 0) return;

        ref Slot s = ref Slots[index];
        double vFrac = Math.Clamp(heightPx / desktopH, 0.15, 1.0);
        double scale = _globalScaleActual * TileSizeAspectScale(_aspect);

        s.ScaleY = TileHeight * vFrac * scale;
        s.ScaleX = s.ScaleY * (widthPx / heightPx);

        double maxAspect = Math.Min(2.22, Math.Max(_aspect * 0.93, 16.0 / 9.0));
        if (s.ScaleX > s.ScaleY * maxAspect) s.ScaleX = s.ScaleY * maxAspect;

        if (_coverFlow) s.Y = _floorY + s.ScaleY * 0.5;
    }

    // =====================================================================
    // Cover Flow (scene/CoverFlowLayout.cpp)
    // =====================================================================
    public static int SlotOffset(int i, int visible)
    {
        if (visible <= 1 || i == 0) return 0;
        int right = visible / 2;                 // ceil((N-1)/2)
        return i <= right ? i : i - visible;
    }

    private void BuildCoverFlow(int visible)
    {
        double aspectScale = TileSizeAspectScale(_aspect);
        int sides = CfSides(visible);
        double centerY = BaseY;
        double frontZ = CamDist;

        EyeX = 0.0;
        EyeY = centerY + CfEyeLift;
        EyeZ = frontZ - CamDist * CfCamZoom;
        TargetX = 0.0;
        TargetY = centerY - CfLookDrop;
        TargetZ = frontZ + 2.0;
        TiltY = 0.0;
        TiltX = 0.0;

        double overrun = sides > CfComfortSide ? sides - CfComfortSide : 0.0;
        double depthStep = CfDepthStep * (1.0 + CfLongDepthAdd * overrun / 3.0);

        // Tile size for long rows is SOLVED, not guessed — see the same solve
        // in CoverFlowLayout::Build.
        double rowFit = 1.0;
        if (sides > CfComfortSide)
        {
            int rightCount = visible / 2;
            int leftCount = visible - 1 - rightCount;
            double k0 = Math.Tan(FovDeg * 0.5 * Math.PI / 180.0) * _aspect;
            double maxAspect = Math.Min(2.22, Math.Max(_aspect * 0.93, 16.0 / 9.0));
            double HalfWidthAt(double fit) =>
                0.5 * TileHeight * GlobalScale * CfTileScale * fit * aspectScale * maxAspect;
            bool Fits(double fit) => CfRowFitsTwoDeep(
                HalfWidthAt(fit), rightCount, leftCount, frontZ, depthStep, k0);
            if (!Fits(1.0))
            {
                rowFit = CfMinRowFit;
                if (Fits(CfMinRowFit))
                {
                    double lo = CfMinRowFit, hi = 1.0;
                    for (int it = 0; it < 24; it++)
                    {
                        double mid = (lo + hi) * 0.5;
                        if (Fits(mid)) lo = mid; else hi = mid;
                    }
                    rowFit = lo;
                }
            }
        }

        double gs = GlobalScale * CfTileScale * rowFit;
        _globalScaleActual = gs;

        double tileH = TileHeight * gs * aspectScale;
        double tileW = tileH * CfTypAspect;
        _floorY = centerY - tileH * 0.5;

        Slots = new Slot[visible];
        for (int i = 0; i < visible; i++)
        {
            int off = SlotOffset(i, visible);
            int aoff = Math.Abs(off);
            ref Slot s = ref Slots[i];
            s.ScaleY = tileH;
            s.ScaleX = tileW;
            s.Y = _floorY + s.ScaleY * 0.5;
            s.X = 0.0;

            s.Alpha = 1.0;
            if (overrun > 0.0 && aoff > CfComfortSide)
                s.Alpha = 1.0 - Math.Clamp((aoff - CfComfortSide) / overrun, 0.0, 1.0) * CfLongFade;

            if (off == 0)
            {
                s.Z = frontZ;
                s.RotY = 0.0;
            }
            else
            {
                s.Z = frontZ + CfSideDepth + (aoff - 1) * depthStep;
                s.RotY = off > 0 ? CfSideAngleDeg : -CfSideAngleDeg;
            }
        }
        RelayoutCoverFlowX();
    }

    private void RelayoutCoverFlowX()
    {
        int visible = Slots.Length;
        if (visible < 2) { if (visible == 1) Slots[0].X = 0; return; }

        double ProjHalfW(in Slot s) => s.RotY == 0.0
            ? s.ScaleX * 0.5
            : s.ScaleX * 0.5 * Math.Abs(Math.Cos(s.RotY * Math.PI / 180.0));

        var right = new List<int>();
        var left = new List<int>();
        for (int i = 1; i < visible; i++)
        {
            int off = SlotOffset(i, visible);
            if (off > 0) right.Add(i);
            else if (off < 0) left.Add(i);
        }
        right.Sort((a, b) => Math.Abs(SlotOffset(a, visible)).CompareTo(Math.Abs(SlotOffset(b, visible))));
        left.Sort((a, b) => Math.Abs(SlotOffset(a, visible)).CompareTo(Math.Abs(SlotOffset(b, visible))));

        double centreHalfW = ProjHalfW(Slots[0]);

        // Long rows: distribute the tiles' OUTER EDGES evenly in screen
        // measure, so every window in the row shows the same strip.  The lean
        // is left where Build put it — room is made by SIZE, in the solve
        // BuildCoverFlow runs (see CoverFlowLayout.cpp for the reasoning and
        // the measurements).
        if (CfSides(visible) > CfComfortSide)
        {
            double k0 = Math.Tan(FovDeg * 0.5 * Math.PI / 180.0) * _aspect;
            double centreDepth = Math.Max(0.001, Slots[0].Z - EyeZ);
            double centreEdge = k0 > 0.0 ? centreHalfW / (k0 * centreDepth) : 0.0;

            for (int pass = 0; pass < 2; pass++)
            {
                var side = pass == 0 ? right : left;
                if (side.Count == 0) continue;
                double sign = pass == 0 ? 1.0 : -1.0;

                // Start the grid further out when the centre tile is much
                // narrower than the one beside it, so the first side tile
                // cannot cross the middle — see CoverFlowLayout.cpp.
                double startEdge = centreEdge;
                {
                    ref Slot first = ref Slots[side[0]];
                    double hw = first.ScaleX * 0.5;
                    double rad = Math.Abs(first.RotY) * Math.PI / 180.0;
                    double dOut = Math.Max(0.001, first.Z - hw * Math.Sin(rad) - EyeZ);
                    double innerFloor = k0 > 0.0
                        ? 2.0 * hw * Math.Cos(rad) / (k0 * dOut) : 0.0;
                    if (side.Count >= 2)
                        startEdge = Math.Max(startEdge,
                            (innerFloor * side.Count - CfLongEdgeMargin) / (side.Count - 1));
                    startEdge = Math.Clamp(startEdge, 0.0, CfLongEdgeMargin * 0.9);
                }
                double strip = Math.Max(0.0, CfLongEdgeMargin - startEdge) / side.Count;

                for (int k = 0; k < side.Count; k++)
                {
                    ref Slot s = ref Slots[side[k]];
                    s.X = sign * Math.Max(0.0, CfPlaceOuterEdge(
                        s.ScaleX * 0.5, s.Z, startEdge + strip * (k + 1),
                        Math.Abs(s.RotY), k0).ax);
                }
            }
            return;
        }

        double spacingFrac = CfSpacingFrac;
        double firstGap = 0.0, maxSpread = 0.0, outermostZ = Slots[0].Z;

        for (int pass = 0; pass < 2; pass++)
        {
            var side = pass == 0 ? right : left;
            double sign = pass == 0 ? 1.0 : -1.0;
            double prevX = 0.0, prevHalfW = centreHalfW;
            for (int k = 0; k < side.Count; k++)
            {
                int i = side[k];
                double halfW = ProjHalfW(Slots[i]);
                double ax = k == 0
                    ? centreHalfW + halfW * CfGapProjFrac + CfCenterGapPad
                    : prevX + (prevHalfW + halfW) * spacingFrac;
                Slots[i].X = sign * ax;
                if (k == 0 && ax > firstGap) firstGap = ax;
                if (ax - firstGap > maxSpread) { maxSpread = ax - firstGap; outermostZ = Slots[i].Z; }
                prevX = ax;
                prevHalfW = halfW;
            }
        }

        if (maxSpread <= 0.0) return;
        double halfWidthAtSide = Math.Tan(FovDeg * 0.5 * Math.PI / 180.0)
                               * _aspect * (outermostZ - EyeZ) * CfFitMargin;
        double budget = halfWidthAtSide - firstGap;
        if (budget > 0.0 && maxSpread > budget)
        {
            double k = budget / maxSpread;
            for (int i = 0; i < visible; i++)
            {
                double ax = Math.Abs(Slots[i].X);
                if (ax <= firstGap) continue;
                double sign = Slots[i].X < 0 ? -1.0 : 1.0;
                Slots[i].X = sign * (firstGap + (ax - firstGap) * k);
            }
        }
    }

    // =====================================================================
    // Matrices — the same chain as FlipScene::GetDrawCall, in WPF's
    // row-vector Matrix3D form (identical convention to DirectXMath).
    // =====================================================================
    public Matrix3D WorldOf(in Slot s, bool reflected = false)
    {
        Matrix3D m = Scale(s.ScaleX, s.ScaleY, 1.0);
        if (reflected)
            m = Translate(0.0, -1.0, 0.0) * m;   // one tile-height down, unit-quad space
        if (TiltX != 0.0) m *= RotationX(TiltX);
        m *= RotationY(TiltY + s.RotY);
        m *= Translate(s.X, s.Y, s.Z);
        return m;
    }

    public Matrix3D ViewMatrix() => LookAtLH(EyeX, EyeY, EyeZ, TargetX, TargetY, TargetZ);

    public static Matrix3D ProjectionMatrix(double aspect) =>
        PerspectiveFovLH(FovDeg * Math.PI / 180.0, aspect, 0.1, 200.0);

    private static Matrix3D Scale(double x, double y, double z) =>
        new(x, 0, 0, 0, 0, y, 0, 0, 0, 0, z, 0, 0, 0, 0, 1);

    private static Matrix3D Translate(double x, double y, double z) =>
        new(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1);

    private static Matrix3D RotationX(double deg)
    {
        double r = deg * Math.PI / 180.0, c = Math.Cos(r), s = Math.Sin(r);
        return new Matrix3D(1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1);
    }

    private static Matrix3D RotationY(double deg)
    {
        double r = deg * Math.PI / 180.0, c = Math.Cos(r), s = Math.Sin(r);
        return new Matrix3D(c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1);
    }

    private static Matrix3D LookAtLH(double ex, double ey, double ez,
                                     double tx, double ty, double tz)
    {
        var eye = new Vector3D(ex, ey, ez);
        var zaxis = new Vector3D(tx - ex, ty - ey, tz - ez);
        zaxis.Normalize();
        var xaxis = Vector3D.CrossProduct(new Vector3D(0, 1, 0), zaxis);
        xaxis.Normalize();
        var yaxis = Vector3D.CrossProduct(zaxis, xaxis);

        return new Matrix3D(
            xaxis.X, yaxis.X, zaxis.X, 0,
            xaxis.Y, yaxis.Y, zaxis.Y, 0,
            xaxis.Z, yaxis.Z, zaxis.Z, 0,
            -Vector3D.DotProduct(xaxis, eye),
            -Vector3D.DotProduct(yaxis, eye),
            -Vector3D.DotProduct(zaxis, eye), 1);
    }

    private static Matrix3D PerspectiveFovLH(double fovY, double aspect, double zn, double zf)
    {
        double yScale = 1.0 / Math.Tan(fovY * 0.5);
        double xScale = yScale / aspect;
        return new Matrix3D(
            xScale, 0, 0, 0,
            0, yScale, 0, 0,
            0, 0, zf / (zf - zn), 1,
            0, 0, -zn * zf / (zf - zn), 0);
    }
}
