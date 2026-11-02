using System.Diagnostics;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Effects;
using System.Windows.Shapes;
using Color = System.Windows.Media.Color;

namespace SolarNX.UI.Controls;

/// <summary>
/// Purely decorative main-window background: the pre-rendered eclipse scene (Assets/background.png,
/// made by tools/branding/make_branding.py - corona, orbits, stars) with two planets moving along
/// its orbits. No dependency on Config/AppOrchestrator - safe to drop into any XAML tree.
///
/// The image is shown UniformToFill, so the planets are placed by mapping the scene's own
/// coordinates (fractions of the image height around its centre - SCENE in that script, mirrored
/// below) through the same scale and centring the Image applies. Driven by
/// CompositionTarget.Rendering; the two Ellipses are created once and only moved per frame.
/// </summary>
public partial class AnimatedBackgroundControl : System.Windows.Controls.UserControl
{
    // Scene geometry in fractions of the image height - identical to SCENE in make_branding.py.
    private const double SunRadius = 0.13;
    private const double TiltDegrees = -12.0;
    private static readonly (double Rx, double Ry)[] Orbits = { (0.46, 0.105), (0.74, 0.17) };

    private readonly List<Planet> _planets = new();
    private readonly Stopwatch _stopwatch = new();
    private double _lastElapsedSeconds;
    private bool _built;

    private sealed class Planet
    {
        public int Orbit;
        public double Angle;
        public double AngularSpeedRadPerSec;
        public double Radius;
        public Ellipse Dot = null!;
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
            var random = new Random();
            AddPlanet(orbit: 0, Color.FromRgb(0xFF, 0x3C, 0x28), radius: 7, speed: 0.22, random);
            AddPlanet(orbit: 1, Color.FromRgb(0x0A, 0xB9, 0xE6), radius: 6, speed: 0.13, random);
            _built = true;
        }
        _stopwatch.Restart();
        _lastElapsedSeconds = 0;
        CompositionTarget.Rendering += OnRendering;
    }

    // CompositionTarget.Rendering fires for every live subscriber on every composed frame for the
    // whole process's lifetime - a missing Unloaded unsubscribe would keep animating (and burning
    // CPU on) a control no longer in the visual tree.
    private void OnUnloaded(object sender, RoutedEventArgs e) => CompositionTarget.Rendering -= OnRendering;

    private void AddPlanet(int orbit, Color color, double radius, double speed, Random random)
    {
        var dot = new Ellipse
        {
            Width = radius * 2,
            Height = radius * 2,
            Fill = new SolidColorBrush(color),
            Effect = new DropShadowEffect { Color = color, BlurRadius = radius * 3, ShadowDepth = 0, Opacity = 0.8 },
        };
        RootCanvas.Children.Add(dot);
        _planets.Add(new Planet
        {
            Orbit = orbit,
            Angle = random.NextDouble() * Math.PI * 2,
            AngularSpeedRadPerSec = speed,
            Radius = radius,
            Dot = dot,
        });
    }

    private void OnRendering(object? sender, EventArgs e)
    {
        var elapsed = _stopwatch.Elapsed.TotalSeconds;
        var dt = elapsed - _lastElapsedSeconds;
        _lastElapsedSeconds = elapsed;
        // Guards the first frame (elapsed==0) and a stall (e.g. the window being dragged) from
        // making the planets jump instead of smoothly continuing.
        if (dt <= 0 || dt > 0.25) dt = 1.0 / 60.0;

        var width = ActualWidth;
        var height = ActualHeight;
        if (width <= 0 || height <= 0 || SceneImage.Source is not ImageSource source) return;

        // UniformToFill: the image is scaled to cover the control and centred, so the scene's
        // height in control units is the image height times that scale.
        var scale = Math.Max(width / source.Width, height / source.Height);
        var sceneHeight = source.Height * scale;
        var centerX = width / 2;
        var centerY = height / 2;
        var tilt = TiltDegrees * Math.PI / 180;
        var sunRadius = SunRadius * sceneHeight;

        foreach (var planet in _planets)
        {
            planet.Angle = (planet.Angle + planet.AngularSpeedRadPerSec * dt) % (Math.PI * 2);
            var (rx, ry) = Orbits[planet.Orbit];
            var ex = Math.Cos(planet.Angle) * rx * sceneHeight;
            var ey = Math.Sin(planet.Angle) * ry * sceneHeight;
            var x = centerX + ex * Math.Cos(tilt) - ey * Math.Sin(tilt);
            var y = centerY + ex * Math.Sin(tilt) + ey * Math.Cos(tilt);

            // The far half of the orbit (sin < 0) runs behind the eclipse disc.
            var behind = Math.Sin(planet.Angle) < 0;
            planet.Dot.Visibility = behind && Math.Sqrt((x - centerX) * (x - centerX) + (y - centerY) * (y - centerY)) < sunRadius + planet.Radius
                ? Visibility.Hidden
                : Visibility.Visible;
            System.Windows.Controls.Canvas.SetLeft(planet.Dot, x - planet.Radius);
            System.Windows.Controls.Canvas.SetTop(planet.Dot, y - planet.Radius);
        }
    }
}
