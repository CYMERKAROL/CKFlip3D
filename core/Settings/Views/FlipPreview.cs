// ---------------------------------------------------------------------------
// The live miniature on the Appearance page.  Real geometry rather than a
// mock-up: the same layout, the same camera chain and the same transition as
// the switcher, so a setting can be judged by looking at it.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
using System.ComponentModel;
using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Media.Imaging;
using System.Windows.Media.Media3D;
using System.Windows.Threading;
using CKFlip3D.Settings.Models;

namespace CKFlip3D.Settings.Views;

/// <summary>
/// The Appearance page's live miniature of the switcher.
///
/// It is the real geometry, not a mock-up: <see cref="PreviewScene"/> ports
/// the core's cascade and Cover Flow layouts, the tiles are drawn through the
/// same world → LookAtLH → PerspectiveFovLH chain (fed to a MatrixCamera), and
/// the transition below is a port of animation/CycleAnimator — wrap phases,
/// OutCubic slide and alpha included. The preview therefore reacts to the
/// visual preset, background opacity and blur, reflections, the desktop tile
/// and the window count, and it is laid out for the viewer's own screen aspect.
///
/// With animations off (master switch or the per-animation Cycle flag) nothing
/// ticks — the miniature simply holds its rest pose, exactly like the core.
/// </summary>
public sealed class FlipPreview : UserControl
{
    // ---- One tile's window ---------------------------------------------------
    private sealed class Win
    {
        public double W, H;              // pixel size on the mock desktop
        public Brush Face = null!;       // tile texture
        public Brush Mirror = null!;     // flipped + faded copy for the floor
    }

    private const double GapMs = 2200;        // rest between two switches
    private const int MaxTiles = 10;

    // Deterministic window proportions (fractions of the desktop) so the stack
    // has the uneven, real-world silhouette the cascade actually produces.
    private static readonly (double W, double H)[] Shapes =
    [
        (1.00, 1.00), (0.62, 0.78), (0.86, 0.92), (0.48, 0.66), (0.74, 0.85),
        (0.55, 0.50), (0.92, 0.70), (0.40, 0.74), (0.68, 0.60), (0.80, 0.95),
    ];

    private static readonly Color[] Accents =
    [
        Color.FromRgb(0x4F, 0x8B, 0xC4), Color.FromRgb(0x5F, 0xA8, 0x7A),
        Color.FromRgb(0xC4, 0x8B, 0x4F), Color.FromRgb(0x8B, 0x6F, 0xC4),
        Color.FromRgb(0x4F, 0xB0, 0xB8), Color.FromRgb(0xC4, 0x5F, 0x74),
        Color.FromRgb(0x9C, 0xB0, 0x4F), Color.FromRgb(0x6F, 0x7F, 0xC4),
        Color.FromRgb(0xB8, 0x8B, 0x5F), Color.FromRgb(0x5F, 0x9C, 0xC4),
    ];

    private readonly SettingsModel _model;
    private readonly Border _frame;
    private readonly Border _wallpaper;
    private readonly BlurEffect _wallpaperBlur = new() { Radius = 0 };
    private readonly Viewport3D _viewport;
    private readonly Model3DGroup _root = new();
    private readonly MatrixCamera _camera = new();

    private readonly List<Win> _windows = new();
    private readonly List<GeometryModel3D> _tiles = new();
    private readonly List<GeometryModel3D> _mirrors = new();
    // One transform per model, reused for the life of the scene: assigning a
    // fresh MatrixTransform3D every frame allocated 2 × N freezables per frame
    // and re-parented them through the property system for nothing.
    private readonly List<MatrixTransform3D> _tileXf = new();
    private readonly List<MatrixTransform3D> _mirrorXf = new();
    private double[] _tileAlpha = [];             // last opacity actually pushed
    private double[] _mirrorAlpha = [];
    private readonly PreviewScene _scene = new();
    private readonly PreviewCycle _cycle = new();

    private PreviewScene.Slot[] _rest = [];       // pose the stack sits in
    private PreviewScene.Slot[] _live = [];       // pose currently on screen
    private readonly List<int> _order = new();    // scratch, refilled per frame
    private readonly List<int> _drawOrder = new();
    private readonly Comparison<int> _farthestFirst;
    private bool _reflectionsInTree;

    private readonly DispatcherTimer _gap = new();
    private readonly Stopwatch _clock = new();
    private bool _animating;
    private double _aspect = 16.0 / 9.0;
    private double _desktopW = 1920, _desktopH = 1080;

    public FlipPreview() : this(App.Settings) { }

    public FlipPreview(SettingsModel model)
    {
        _model = model;
        _farthestFirst = (a, b) => _live[b].Z.CompareTo(_live[a].Z);

        // A true miniature: same aspect as the viewer's primary screen, so the
        // aspect-driven parts of the layout (spread, tile shrink, Cover Flow
        // fit) land where they do in the real thing.
        double sw = SystemParameters.PrimaryScreenWidth;
        double sh = SystemParameters.PrimaryScreenHeight;
        if (sw > 0 && sh > 0) { _aspect = sw / sh; _desktopW = sw; _desktopH = sh; }
        double h = Math.Min(300, 440 / _aspect);
        double w = h * _aspect;

        _wallpaper = new Border
        {
            Effect = _wallpaperBlur,
            Background = new LinearGradientBrush(
                [
                    new GradientStop(Color.FromRgb(0x1E, 0x5A, 0x8E), 0.0),
                    new GradientStop(Color.FromRgb(0x3E, 0x8C, 0xC2), 0.45),
                    new GradientStop(Color.FromRgb(0x77, 0xB8, 0xDD), 0.75),
                    new GradientStop(Color.FromRgb(0x2B, 0x6E, 0x55), 1.0),
                ],
                new Point(0, 0), new Point(1, 1)),
        };

        _viewport = new Viewport3D { ClipToBounds = true, Camera = _camera };
        _viewport.Children.Add(new ModelVisual3D { Content = _root });

        _frame = new Border
        {
            Width = w,
            Height = h,
            CornerRadius = new CornerRadius(5),
            ClipToBounds = true,
            BorderThickness = new Thickness(1),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x44, 0x61, 0x7C)),
            Background = new SolidColorBrush(Color.FromRgb(0x05, 0x08, 0x0C)),
            Child = new Grid { Children = { _wallpaper, _viewport } },
        };
        Content = new Grid { HorizontalAlignment = HorizontalAlignment.Left, Children = { _frame } };

        _gap.Interval = TimeSpan.FromMilliseconds(GapMs);
        _gap.Tick += (_, _) => { _gap.Stop(); StartCycle(); };

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    // =====================================================================
    // Lifecycle
    // =====================================================================

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        _model.PropertyChanged += OnSettingChanged;
        Rebuild();
        ApplyBackdrop();
        RestartLoop();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _model.PropertyChanged -= OnSettingChanged;
        StopLoop();
    }

    private void OnSettingChanged(object? sender, PropertyChangedEventArgs e)
    {
        switch (e.PropertyName)
        {
            case nameof(SettingsModel.VisualPreset):
            case nameof(SettingsModel.MaxWindows):
            case nameof(SettingsModel.ShowDesktopTile):
            case nameof(SettingsModel.Reflections):
            case nameof(SettingsModel.Antialiasing):
            case null:
                Rebuild();
                ApplyBackdrop();
                RestartLoop();
                break;
            case nameof(SettingsModel.BackgroundOpacity):
            case nameof(SettingsModel.BackgroundBlur):
                ApplyBackdrop();
                break;
            case nameof(SettingsModel.Animations):
            case nameof(SettingsModel.AnimCycle):
                RestartLoop();
                break;
        }
    }

    /// <summary>
    /// A STILL miniature: the same geometry, laid out for the same settings,
    /// but never cycling.
    ///
    /// The Search page's placement preview draws on top of this, and the stack
    /// underneath has to stay put while the field is dragged over it — a
    /// rotating stack there is a distraction, not information. Reusing this
    /// control rather than sketching a second cascade is the point: one
    /// layout, so the two previews can never disagree about what Cover Flow
    /// or a 4-window stack looks like.
    /// </summary>
    public bool Still { get; init; }

    /// <summary>
    /// The screen-shaped surface the stack is drawn on. Exposed so an overlay
    /// can be placed in exactly the same coordinate space.
    /// </summary>
    public FrameworkElement ScreenSurface => _frame;

    /// <summary>
    /// Where the stack actually lands inside <see cref="ScreenSurface"/>, as
    /// a rectangle, or <see cref="Rect.Empty"/> before it has been built.
    ///
    /// Projected from the REST pose through the same camera the tiles are
    /// drawn with, so it fits whatever the layout currently is — a cascade, a
    /// Cover Flow row, two windows or ten — without anyone having to describe
    /// those shapes a second time. The Search page uses it as a keep-out area
    /// so the field cannot be placed on top of the windows it is filtering.
    /// </summary>
    public Rect StackBounds()
    {
        if (_rest.Length == 0) return Rect.Empty;
        double w = _frame.Width, h = _frame.Height;
        if (double.IsNaN(w) || w <= 0 || double.IsNaN(h) || h <= 0)
            return Rect.Empty;

        Matrix3D view = _scene.ViewMatrix();
        Matrix3D proj = PreviewScene.ProjectionMatrix(_aspect);

        double minX = double.MaxValue, minY = double.MaxValue;
        double maxX = double.MinValue, maxY = double.MinValue;
        bool any = false;

        // The unit quad QuadRenderer draws, corner for corner.
        ReadOnlySpan<(double X, double Y)> corners =
            [(-0.5, -0.5), (-0.5, 0.5), (0.5, 0.5), (0.5, -0.5)];

        for (int i = 0; i < _rest.Length; i++)
        {
            // A tile faded to nothing is not part of the shape the user sees.
            if (_rest[i].Alpha <= 0.02) continue;

            Matrix3D mvp = _scene.WorldOf(_rest[i]);
            mvp.Append(view);
            mvp.Append(proj);

            foreach (var (cx, cy) in corners)
            {
                Point4D p = mvp.Transform(new Point4D(cx, cy, 0, 1));
                if (p.W <= 1e-6) continue;   // behind the camera
                double sx = (p.X / p.W * 0.5 + 0.5) * w;
                double sy = (0.5 - p.Y / p.W * 0.5) * h;
                minX = Math.Min(minX, sx); maxX = Math.Max(maxX, sx);
                minY = Math.Min(minY, sy); maxY = Math.Max(maxY, sy);
                any = true;
            }
        }

        if (!any || maxX <= minX || maxY <= minY) return Rect.Empty;
        return new Rect(minX, minY, maxX - minX, maxY - minY);
    }

    private bool CycleAnimated => !Still && _model.Animations && _model.AnimCycle;

    private void RestartLoop()
    {
        StopLoop();
        if (CycleAnimated) _gap.Start();
    }

    private void StopLoop()
    {
        _gap.Stop();
        if (_animating)
        {
            CompositionTarget.Rendering -= OnRendering;
            _animating = false;
        }
        _clock.Reset();
        if (_rest.Length > 0)
        {
            _live = (PreviewScene.Slot[])_rest.Clone();
            PushPose();
        }
    }

    // =====================================================================
    // Scene construction
    // =====================================================================

    private void ApplyBackdrop()
    {
        _wallpaper.Opacity = Math.Clamp(_model.BackgroundOpacity, 0, 100) / 100.0;
        // The core's blur is a percentage of a screen-sized kernel; on a
        // ~440 px miniature the same visual weight lands around a fifth of it.
        _wallpaperBlur.Radius = Math.Clamp(_model.BackgroundBlur, 0, 100) * 0.18;
    }

    private void Rebuild()
    {
        int count = Math.Clamp((int)_model.MaxWindows, 2, MaxTiles);
        bool coverFlow = _model.VisualPreset == 1;

        if (_windows.Count != count)
        {
            _windows.Clear();
            for (int i = 0; i < count; i++)
            {
                var shape = Shapes[i % Shapes.Length];
                _windows.Add(new Win { W = shape.W * _desktopW, H = shape.H * _desktopH });
            }
        }

        // The desktop pseudo-window is the last tile of the stack, like the
        // original Flip 3D; off, its slot goes to an ordinary window.
        bool aa = _model.Antialiasing;
        for (int i = 0; i < count; i++)
        {
            bool desktop = _model.ShowDesktopTile && i == count - 1;
            var (face, mirror) = MakeBrushes(Accents[i % Accents.Length], desktop, aa);
            _windows[i].Face = face;
            _windows[i].Mirror = mirror;
            if (desktop) { _windows[i].W = _desktopW; _windows[i].H = _desktopH; }
        }
        // Geometry edges follow the same switch: smooth when the sampler is,
        // hard when it is not.
        RenderOptions.SetEdgeMode(_viewport, aa ? EdgeMode.Unspecified : EdgeMode.Aliased);

        BuildModels(count);
        BuildRest();
        _live = (PreviewScene.Slot[])_rest.Clone();
        PushPose();
    }

    private void BuildRest()
    {
        var sizes = _windows.Select(win => (win.W, win.H)).ToArray();
        _scene.Build(sizes, _aspect, _model.VisualPreset == 1, _desktopW, _desktopH);
        _rest = (PreviewScene.Slot[])_scene.Slots.Clone();
        _camera.ViewMatrix = _scene.ViewMatrix();
        _camera.ProjectionMatrix = PreviewScene.ProjectionMatrix(_aspect);
    }

    private void BuildModels(int count)
    {
        _root.Children.Clear();
        _tiles.Clear();
        _mirrors.Clear();
        _tileXf.Clear();
        _mirrorXf.Clear();
        _root.Children.Add(new AmbientLight(Colors.White));

        var mesh = UnitQuad();
        for (int i = 0; i < count; i++)
        {
            var mirrorXf = new MatrixTransform3D(Matrix3D.Identity);
            var tileXf = new MatrixTransform3D(Matrix3D.Identity);
            _mirrors.Add(new GeometryModel3D { Geometry = mesh, Transform = mirrorXf });
            _tiles.Add(new GeometryModel3D { Geometry = mesh, Transform = tileXf });
            _mirrorXf.Add(mirrorXf);
            _tileXf.Add(tileXf);
        }
        _tileAlpha = new double[count];
        _mirrorAlpha = new double[count];
        for (int i = 0; i < count; i++) _tileAlpha[i] = _mirrorAlpha[i] = -1.0;
        _drawOrder.Clear();
        ApplyMaterials();
    }

    /// <summary>Point the materials at the (possibly rotated) window order.</summary>
    private void ApplyMaterials()
    {
        for (int i = 0; i < _tiles.Count; i++)
        {
            var face = new DiffuseMaterial(_windows[i].Face);
            _tiles[i].Material = face;
            _tiles[i].BackMaterial = face;   // LH matrices flip the winding
            var mirror = new DiffuseMaterial(_windows[i].Mirror);
            _mirrors[i].Material = mirror;
            _mirrors[i].BackMaterial = mirror;
            // Fresh brushes start fully opaque — forget what the old ones held.
            if (i < _tileAlpha.Length) _tileAlpha[i] = _mirrorAlpha[i] = -1.0;
        }
    }

    private static MeshGeometry3D UnitQuad()
    {
        // Centred unit quad — the same one the core scales into tile size, so
        // the reflection's "shift one tile-height down" trick ports directly.
        var mesh = new MeshGeometry3D
        {
            Positions =
            [
                new Point3D(-0.5,  0.5, 0), new Point3D(0.5,  0.5, 0),
                new Point3D( 0.5, -0.5, 0), new Point3D(-0.5, -0.5, 0),
            ],
            TextureCoordinates =
            [
                new Point(0, 0), new Point(1, 0), new Point(1, 1), new Point(0, 1),
            ],
            TriangleIndices = [0, 1, 2, 0, 2, 3],
        };
        mesh.Freeze();
        return mesh;
    }

    // =====================================================================
    // Pose → models
    // =====================================================================

    // Below this a tile contributes nothing but still WRITES DEPTH, and WPF 3D
    // has no way to turn that off per model — an invisible tile left in the
    // scene punches a hole through everything drawn after it (the half-tiles
    // and black boxes a fading window used to leave behind). Drop it instead.
    private const double AlphaFloor = 0.012;

    // Runs once per composition frame for the whole length of a switch, so it
    // allocates nothing and touches no dependency property whose value has not
    // actually moved — a redundant Brush.Opacity write dirties the material and
    // costs a re-realisation on the render thread.
    private void PushPose()
    {
        int n = Math.Min(_live.Length, _tiles.Count);
        if (n == 0) return;

        bool reflections = _model.Reflections;

        // Painter's order: farthest first, so the near-opaque tiles blend over
        // what is behind them instead of the depth buffer rejecting it.
        // Reflections form their own pass ahead of the tiles, like the core.
        _order.Clear();
        for (int i = 0; i < n; i++)
            if (_live[i].Alpha > AlphaFloor) _order.Add(i);
        _order.Sort(_farthestFirst);

        for (int k = 0; k < _order.Count; k++)
        {
            int i = _order[k];
            ref PreviewScene.Slot s = ref _live[i];
            _tileXf[i].Matrix = _scene.WorldOf(s);
            SetAlpha(_tiles[i], ref _tileAlpha[i], Math.Clamp(s.Alpha, 0, 1));

            if (reflections)
            {
                _mirrorXf[i].Matrix = _scene.WorldOf(s, reflected: true);
                SetAlpha(_mirrors[i], ref _mirrorAlpha[i], Math.Clamp(s.Alpha * 0.34, 0, 1));
            }
        }

        if (reflections != _reflectionsInTree || !SameOrder())
        {
            _drawOrder.Clear();
            _drawOrder.AddRange(_order);
            _reflectionsInTree = reflections;
            _root.Children.Clear();
            _root.Children.Add(new AmbientLight(Colors.White));
            if (reflections)
                foreach (int i in _order) _root.Children.Add(_mirrors[i]);
            foreach (int i in _order) _root.Children.Add(_tiles[i]);
        }
    }

    private static void SetAlpha(GeometryModel3D model, ref double last, double alpha)
    {
        if (alpha == last) return;
        ((DiffuseMaterial)model.Material).Brush.Opacity = alpha;
        last = alpha;
    }

    /// <summary>SequenceEqual without the enumerators — this runs every frame.</summary>
    private bool SameOrder()
    {
        if (_order.Count != _drawOrder.Count) return false;
        for (int i = 0; i < _order.Count; i++)
            if (_order[i] != _drawOrder[i]) return false;
        return true;
    }

    // =====================================================================
    // The switch itself
    // =====================================================================

    private void StartCycle()
    {
        if (!CycleAnimated || _windows.Count < 2) return;

        var start = (PreviewScene.Slot[])_rest.Clone();

        // Same order of operations as FlipController::ExecuteCycleForward:
        // rotate the window array, rebuild the layout for the new arrangement,
        // then animate the old pose into the new one.
        var first = _windows[0];
        _windows.RemoveAt(0);
        _windows.Add(first);
        ApplyMaterials();
        BuildRest();

        _cycle.Begin(start, _rest, _model.VisualPreset == 1);
        _clock.Restart();
        _animating = true;
        CompositionTarget.Rendering += OnRendering;
    }

    private void OnRendering(object? sender, EventArgs e)
    {
        double t = _clock.Elapsed.TotalMilliseconds / PreviewCycle.DurationMs;
        if (t >= 1.0)
        {
            CompositionTarget.Rendering -= OnRendering;
            _animating = false;
            _clock.Stop();
            _live = (PreviewScene.Slot[])_rest.Clone();
            PushPose();
            if (CycleAnimated) _gap.Start();
            return;
        }

        if (_live.Length != _rest.Length) _live = new PreviewScene.Slot[_rest.Length];
        _cycle.Sample(t, _live);
        PushPose();
    }

    // =====================================================================
    // Tile textures
    // =====================================================================

    // The art is authored in a 200x120 box and baked once into a bitmap.
    //
    // WHY BAKE IT. A DiffuseMaterial over a DrawingBrush has to be realised
    // into a texture before it can be drawn, and the per-tile alpha that the
    // switch animates dirties the brush on every single frame — so every frame
    // re-rasterised the vector art of every tile AND every reflection. On this
    // machine that halved the composition rate (68 fps against 136 for the
    // same scene fed from frozen bitmaps); the drop was the whole reason the
    // switch stuttered. A frozen bitmap costs the rasterisation once.
    //
    // The bake resolution is comfortably above the tile's on-screen size even
    // at 200 % scaling, so what the viewer sees is unchanged.
    private const int BakeW = 320, BakeH = 192;
    private const int PointW = 88, PointH = 53;   // antialiasing off, see below

    // Keyed by everything the art depends on, so flipping Antialiasing or the
    // desktop tile back and forth never pays for the same bitmap twice.
    private static readonly Dictionary<(uint Accent, bool Desktop, bool Aa, bool Mirror),
                                       BitmapSource> _bakedArt = new();

    /// <summary>
    /// Tile textures. <paramref name="antialiasing"/> mirrors what the core's
    /// Antialiasing toggle actually switches: the TILE SAMPLER, anisotropic
    /// versus point. On, the art is baked at high resolution and filtered
    /// smoothly; off, it is baked small and magnified with nearest-neighbour,
    /// so the texels show — the same trade the real switcher makes.
    /// </summary>
    private static (Brush face, Brush mirror) MakeBrushes(Color accent, bool desktop,
                                                          bool antialiasing)
    {
        // A brush per tile, never shared: the pose loop writes each one's
        // Opacity, so two tiles behind one brush would fight over the alpha.
        Brush face = new ImageBrush(Bake(accent, desktop, antialiasing, mirror: false))
        {
            Stretch = Stretch.Fill,
        };
        // The mirror is the same art, flipped and faded out downwards.
        Brush mirror = new ImageBrush(Bake(accent, desktop, antialiasing, mirror: true))
        {
            Stretch = Stretch.Fill,
            RelativeTransform = new ScaleTransform(1, -1, 0.5, 0.5),
        };

        if (!antialiasing)
        {
            RenderOptions.SetBitmapScalingMode(face, BitmapScalingMode.NearestNeighbor);
            RenderOptions.SetBitmapScalingMode(mirror, BitmapScalingMode.NearestNeighbor);
        }
        return (face, mirror);
    }

    private static BitmapSource Bake(Color accent, bool desktop, bool antialiasing, bool mirror)
    {
        // The desktop tile ignores the accent, so every index must land on the
        // same cache entry rather than baking ten identical wallpapers.
        uint key = desktop ? 0u : ((uint)accent.R << 16) | ((uint)accent.G << 8) | accent.B;
        if (_bakedArt.TryGetValue((key, desktop, antialiasing, mirror), out var cached))
            return cached;

        Drawing art = desktop ? DesktopDrawing() : WindowDrawing(accent);
        if (mirror)
        {
            // The stops trace alpha = t² from the tile's bottom edge, matching
            // PSReflection's quadratic falloff rather than a flat linear ramp.
            var faded = new DrawingGroup();
            faded.Children.Add(art);
            faded.OpacityMask = new LinearGradientBrush(
                [
                    new GradientStop(Color.FromArgb(0x00, 0xFF, 0xFF, 0xFF), 0.00),
                    new GradientStop(Color.FromArgb(0x10, 0xFF, 0xFF, 0xFF), 0.25),
                    new GradientStop(Color.FromArgb(0x40, 0xFF, 0xFF, 0xFF), 0.50),
                    new GradientStop(Color.FromArgb(0x90, 0xFF, 0xFF, 0xFF), 0.75),
                    new GradientStop(Color.FromArgb(0xFF, 0xFF, 0xFF, 0xFF), 1.00),
                ],
                new Point(0, 0), new Point(0, 1));
            art = faded;
        }
        art.Freeze();

        int w = antialiasing ? BakeW : PointW;
        int h = antialiasing ? BakeH : PointH;
        var host = new DrawingVisual();
        using (var dc = host.RenderOpen())
        {
            dc.PushTransform(new ScaleTransform(w / 200.0, h / 120.0));
            dc.DrawDrawing(art);
            dc.Pop();
        }
        var bmp = new RenderTargetBitmap(w, h, 96, 96, PixelFormats.Pbgra32);
        bmp.Render(host);
        bmp.Freeze();

        _bakedArt[(key, desktop, antialiasing, mirror)] = bmp;
        return bmp;
    }

    private static DrawingGroup WindowDrawing(Color accent)
    {
        var g = new DrawingGroup();
        var body = new LinearGradientBrush(
            Color.FromRgb(0xF7, 0xFA, 0xFD), Color.FromRgb(0xDE, 0xE9, 0xF3),
            new Point(0, 0), new Point(0, 1));
        g.Children.Add(new GeometryDrawing(body,
            new Pen(new SolidColorBrush(Color.FromRgb(0x89, 0xA8, 0xC2)), 1.2),
            new RectangleGeometry(new Rect(0, 0, 200, 120))));

        // title bar + a light content skeleton
        var titleBar = new LinearGradientBrush(
            accent, Color.FromArgb(0xD0, accent.R, accent.G, accent.B),
            new Point(0, 0), new Point(0, 1));
        g.Children.Add(new GeometryDrawing(titleBar, null,
            new RectangleGeometry(new Rect(1, 1, 198, 18))));
        g.Children.Add(new GeometryDrawing(
            new SolidColorBrush(Color.FromArgb(0xCC, 0xFF, 0xFF, 0xFF)), null,
            new RectangleGeometry(new Rect(8, 7, 64, 6), 2, 2)));

        var line = new SolidColorBrush(Color.FromRgb(0xB9, 0xCC, 0xDD));
        double[] widths = [150, 128, 140, 112, 134];
        for (int i = 0; i < widths.Length; i++)
            g.Children.Add(new GeometryDrawing(line, null,
                new RectangleGeometry(new Rect(12, 34 + i * 13, widths[i], 5))));
        return g;
    }

    private static DrawingGroup DesktopDrawing()
    {
        var g = new DrawingGroup();
        g.Children.Add(new GeometryDrawing(
            new LinearGradientBrush(
                [
                    new GradientStop(Color.FromRgb(0x1E, 0x5A, 0x8E), 0.0),
                    new GradientStop(Color.FromRgb(0x3E, 0x8C, 0xC2), 0.45),
                    new GradientStop(Color.FromRgb(0x77, 0xB8, 0xDD), 0.75),
                    new GradientStop(Color.FromRgb(0x2B, 0x6E, 0x55), 1.0),
                ],
                new Point(0, 0), new Point(1, 1)),
            new Pen(new SolidColorBrush(Color.FromRgb(0x89, 0xA8, 0xC2)), 1.2),
            new RectangleGeometry(new Rect(0, 0, 200, 120))));

        var icon = new SolidColorBrush(Color.FromArgb(0x99, 0xFF, 0xFF, 0xFF));
        for (int i = 0; i < 3; i++)
            g.Children.Add(new GeometryDrawing(icon, null,
                new RectangleGeometry(new Rect(10, 10 + i * 26, 18, 18), 3, 3)));
        return g;
    }
}
