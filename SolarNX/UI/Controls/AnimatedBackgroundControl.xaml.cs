using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using Color = System.Windows.Media.Color;
using Point = System.Windows.Point;

namespace SolarNX.UI.Controls;

/// <summary>
/// Purely decorative Home-screen background: a starfield with a few soft nebula blobs behind a
/// glowing sun and two groups of small "planets" (red/blue) orbiting it at different
/// radii/speeds/directions, each trailing a fading comet-tail. No dependency on
/// Config/AppOrchestrator - safe to drop into any XAML tree.
///
/// Driven by CompositionTarget.Rendering (frame-synced, smoother than a DispatcherTimer) instead
/// of a coarse timer. All Ellipses (nebulae, stars, sun, planet heads, trail dots) are created
/// exactly once in BuildScene() and only repositioned/resized/faded per frame - no per-frame
/// allocation.
/// </summary>
public partial class AnimatedBackgroundControl : System.Windows.Controls.UserControl
{
    // 10 frames of history at ~60fps is only ~166ms - barely perceptible as a tail rather than a
    // handful of separate dots. 80 frames (~1.3s) reads as an actual comet trail.
    private const int TrailLength = 80;
    private const double SunRadius = 22;
    private const double MaxTrailOpacity = 0.8;
    private const int StarCount = 90;
    private const int NebulaCount = 3;

    private readonly List<Planet> _planets = new();
    private readonly List<Star> _stars = new();
    private readonly List<Nebula> _nebulae = new();
    private readonly Random _random = new();
    private readonly Stopwatch _stopwatch = new();

    private Ellipse _sunGlow = null!;
    private Ellipse _sunCore = null!;
    private double _lastElapsedSeconds;
    private bool _built;

    private sealed class Planet
    {
        public double OrbitRadiusFraction;
        public double AngularSpeedRadPerSec;
        public double Angle;
        public double HeadRadius;
        public Ellipse Head = null!;
        // Ring buffer of past positions, written every frame at TrailWriteIndex - see OnRendering.
        public Point[] TrailPositions = null!;
        public Ellipse[] TrailDots = null!;
        public int TrailWriteIndex;
        public int TrailFilledCount;
    }

    // Fixed canvas-fraction position (resize-safe, same trick as Planet.OrbitRadiusFraction) plus
    // a sine-wave opacity flicker - phase/speed/amplitude differ per star so they don't all pulse
    // in lockstep.
    private sealed class Star
    {
        public Ellipse Dot = null!;
        public double XFraction;
        public double YFraction;
        public double Phase;
        public double SpeedRadPerSec;
        public double BaseOpacity;
        public double AmplitudeOpacity;
    }

    // Static (no twinkle, no motion) - just a soft colored glow repositioned on resize like
    // everything else here.
    private sealed class Nebula
    {
        public Ellipse Blob = null!;
        public double XFraction;
        public double YFraction;
    }

    public AnimatedBackgroundControl()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        if (!_built)
        {
            BuildScene();
            _built = true;
        }
        _stopwatch.Restart();
        _lastElapsedSeconds = 0;
        CompositionTarget.Rendering += OnRendering;
    }

    // CompositionTarget.Rendering fires for every live subscriber on every composed frame for the
    // whole process's lifetime - a missing Unloaded unsubscribe would keep animating (and burning
    // CPU on) a control no longer in the visual tree, e.g. while the Settings view covers it.
    private void OnUnloaded(object sender, RoutedEventArgs e) => CompositionTarget.Rendering -= OnRendering;

    private void BuildScene()
    {
        // Back-to-front: nebulae, then stars, then sun, then planets/trails on top of everything.
        BuildNebulae();
        BuildStars();
        BuildSun();
        // Opposite angular-speed signs give the two groups visibly different orbit directions on
        // top of the color difference.
        AddPlanetGroup(count: 3, coreColor: Color.FromRgb(0xFF, 0x4D, 0x4D), radiusFractionRange: (0.28, 0.55), speedRange: (0.15, 0.32), sizeRange: (5, 9));
        AddPlanetGroup(count: 3, coreColor: Color.FromRgb(0x4D, 0xA6, 0xFF), radiusFractionRange: (0.40, 0.68), speedRange: (-0.28, -0.12), sizeRange: (4, 8));
    }

    private void BuildNebulae()
    {
        var palette = new[]
        {
            Color.FromRgb(0x7A, 0x4D, 0xE0), // purple
            Color.FromRgb(0x4D, 0x7A, 0xE0), // blue
            Color.FromRgb(0xC0, 0x50, 0xA0), // magenta
        };
        for (var i = 0; i < NebulaCount; i++)
        {
            var color = palette[i % palette.Length];
            var size = Lerp(240, 460, _random.NextDouble());
            var blob = new Ellipse
            {
                Width = size,
                Height = size,
                Fill = new RadialGradientBrush
                {
                    GradientStops =
                    {
                        new GradientStop(Color.FromArgb(45, color.R, color.G, color.B), 0.0),
                        new GradientStop(Color.FromArgb(0, color.R, color.G, color.B), 1.0),
                    },
                },
            };
            RootCanvas.Children.Add(blob);
            _nebulae.Add(new Nebula { Blob = blob, XFraction = _random.NextDouble(), YFraction = _random.NextDouble() });
        }
    }

    private void BuildStars()
    {
        for (var i = 0; i < StarCount; i++)
        {
            var diameter = Lerp(1.0, 3.0, _random.NextDouble());
            var dot = new Ellipse
            {
                Width = diameter,
                Height = diameter,
                Fill = new SolidColorBrush(Colors.White),
            };
            RootCanvas.Children.Add(dot);
            _stars.Add(new Star
            {
                Dot = dot,
                XFraction = _random.NextDouble(),
                YFraction = _random.NextDouble(),
                Phase = _random.NextDouble() * Math.PI * 2,
                SpeedRadPerSec = Lerp(0.4, 1.8, _random.NextDouble()),
                BaseOpacity = Lerp(0.25, 0.45, _random.NextDouble()),
                AmplitudeOpacity = Lerp(0.3, 0.5, _random.NextDouble()),
            });
        }
    }

    private void BuildSun()
    {
        _sunGlow = new Ellipse
        {
            Width = SunRadius * 4,
            Height = SunRadius * 4,
            Fill = new RadialGradientBrush
            {
                GradientStops =
                {
                    new GradientStop(Color.FromArgb(120, 255, 214, 120), 0.0),
                    new GradientStop(Color.FromArgb(0, 255, 214, 120), 1.0),
                },
            },
        };
        _sunCore = new Ellipse
        {
            Width = SunRadius * 2,
            Height = SunRadius * 2,
            Fill = new RadialGradientBrush
            {
                GradientStops =
                {
                    new GradientStop(Colors.White, 0.0),
                    new GradientStop(Color.FromRgb(255, 200, 90), 0.6),
                    new GradientStop(Color.FromRgb(255, 150, 40), 1.0),
                },
            },
        };
        RootCanvas.Children.Add(_sunGlow);
        RootCanvas.Children.Add(_sunCore);
    }

    private void AddPlanetGroup(int count, Color coreColor, (double Min, double Max) radiusFractionRange, (double Min, double Max) speedRange, (double Min, double Max) sizeRange)
    {
        for (var i = 0; i < count; i++)
        {
            var headRadius = Lerp(sizeRange.Min, sizeRange.Max, _random.NextDouble());

            var trailDots = new Ellipse[TrailLength];
            for (var k = 0; k < TrailLength; k++)
            {
                // t=0 is the dot right behind the head, t=1 is the oldest/faintest - size and
                // opacity only depend on the slot index, not the frame, so they're set once here
                // and never touched again in OnRendering (only position moves per frame).
                var t = (double)k / (TrailLength - 1);
                var dotDiameter = headRadius * 2 * (1 - t * 0.6);
                var dot = new Ellipse
                {
                    Width = dotDiameter,
                    Height = dotDiameter,
                    Fill = new SolidColorBrush(coreColor),
                    Opacity = 0,
                };
                trailDots[k] = dot;
                RootCanvas.Children.Add(dot);
            }

            var head = new Ellipse
            {
                Width = headRadius * 2,
                Height = headRadius * 2,
                Fill = new SolidColorBrush(coreColor),
                Effect = new DropShadowEffect { Color = coreColor, BlurRadius = headRadius * 3, ShadowDepth = 0, Opacity = 0.9 },
            };
            RootCanvas.Children.Add(head); // added after its trail dots so it renders on top of them

            _planets.Add(new Planet
            {
                OrbitRadiusFraction = Lerp(radiusFractionRange.Min, radiusFractionRange.Max, _random.NextDouble()),
                AngularSpeedRadPerSec = Lerp(speedRange.Min, speedRange.Max, _random.NextDouble()),
                Angle = _random.NextDouble() * Math.PI * 2,
                HeadRadius = headRadius,
                Head = head,
                TrailPositions = new Point[TrailLength],
                TrailDots = trailDots,
            });
        }
    }

    private void OnRendering(object? sender, EventArgs e)
    {
        var elapsed = _stopwatch.Elapsed.TotalSeconds;
        var dt = elapsed - _lastElapsedSeconds;
        _lastElapsedSeconds = elapsed;
        // Guards the first frame (elapsed==0) and a stall (e.g. the window was being dragged) from
        // making planets jump instead of smoothly continuing.
        if (dt <= 0 || dt > 0.25) dt = 1.0 / 60.0;

        var width = ActualWidth;
        var height = ActualHeight;
        if (width <= 0 || height <= 0) return;

        var centerX = width / 2;
        var centerY = height / 2;
        var maxOrbitRadius = Math.Min(width, height) / 2;

        foreach (var nebula in _nebulae)
        {
            PositionEllipse(nebula.Blob, nebula.XFraction * width, nebula.YFraction * height);
        }

        foreach (var star in _stars)
        {
            var opacity = star.BaseOpacity + star.AmplitudeOpacity * Math.Sin(elapsed * star.SpeedRadPerSec + star.Phase);
            star.Dot.Opacity = Math.Clamp(opacity, 0, 1);
            PositionEllipse(star.Dot, star.XFraction * width, star.YFraction * height);
        }

        PositionEllipse(_sunGlow, centerX, centerY);
        PositionEllipse(_sunCore, centerX, centerY);

        foreach (var planet in _planets)
        {
            planet.Angle += planet.AngularSpeedRadPerSec * dt;
            var orbitRadius = planet.OrbitRadiusFraction * maxOrbitRadius;
            var position = new Point(
                centerX + Math.Cos(planet.Angle) * orbitRadius,
                centerY + Math.Sin(planet.Angle) * orbitRadius);

            planet.TrailPositions[planet.TrailWriteIndex] = position;
            planet.TrailWriteIndex = (planet.TrailWriteIndex + 1) % TrailLength;
            if (planet.TrailFilledCount < TrailLength) planet.TrailFilledCount++;

            for (var k = 0; k < TrailLength; k++)
            {
                var age = k + 1; // frames behind "now" - k=0 is the dot right behind the head
                var dot = planet.TrailDots[k];
                if (age > planet.TrailFilledCount)
                {
                    dot.Opacity = 0;
                    continue;
                }
                var t = (double)k / (TrailLength - 1);
                dot.Opacity = MaxTrailOpacity * (1 - t);
                var index = (planet.TrailWriteIndex - 1 - age + TrailLength * 2) % TrailLength;
                PositionEllipse(dot, planet.TrailPositions[index]);
            }

            PositionEllipse(planet.Head, position);
        }
    }

    private static void PositionEllipse(Ellipse ellipse, Point center) => PositionEllipse(ellipse, center.X, center.Y);

    private static void PositionEllipse(Ellipse ellipse, double centerX, double centerY)
    {
        Canvas.SetLeft(ellipse, centerX - ellipse.Width / 2);
        Canvas.SetTop(ellipse, centerY - ellipse.Height / 2);
    }

    private static double Lerp(double min, double max, double t) => min + (max - min) * t;
}
