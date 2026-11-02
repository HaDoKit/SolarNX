using System.Drawing.Imaging;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace SolarNX.UI;

/// <summary>
/// Independent, best-effort local preview of "what the desktop currently looks like" for the
/// Home screen's "Показать экран" toggle. Deliberately unrelated to VideoEncoder/the ffmpeg
/// pipeline - that class has no local frame tap at all (only stderr text for stall detection) and
/// is a fragile, already-tuned hot area not to be touched here. This has zero bearing on what's
/// actually sent to the Switch; it's just a rough visual for whoever is sitting at this PC.
/// </summary>
internal sealed class ScreenPreviewSource : IDisposable
{
    private const int TargetFps = 12;

    private readonly DispatcherTimer _timer;
    private readonly Rectangle _screenBounds;
    private Bitmap? _gdiBitmap;
    private Graphics? _gdiGraphics;
    private bool _running;

    /// <summary>Named "Preview", not "Bitmap" - a property named the same as the System.Drawing.Bitmap
    /// type used throughout this class would shadow that type name inside the class body.</summary>
    public WriteableBitmap Preview { get; }

    public ScreenPreviewSource()
    {
        _screenBounds = Screen.PrimaryScreen?.Bounds ?? new Rectangle(0, 0, 1920, 1080);
        Preview = new WriteableBitmap(_screenBounds.Width, _screenBounds.Height, 96, 96, PixelFormats.Bgr32, null);

        _timer = new DispatcherTimer(DispatcherPriority.Background)
        {
            Interval = TimeSpan.FromMilliseconds(1000.0 / TargetFps),
        };
        _timer.Tick += (_, _) => CaptureFrame();
    }

    public void Start()
    {
        if (_running) return;
        _gdiBitmap = new Bitmap(_screenBounds.Width, _screenBounds.Height, System.Drawing.Imaging.PixelFormat.Format32bppRgb);
        _gdiGraphics = Graphics.FromImage(_gdiBitmap);
        _running = true;
        _timer.Start();
    }

    public void Stop()
    {
        if (!_running) return;
        _timer.Stop();
        _running = false;
        _gdiGraphics?.Dispose();
        _gdiGraphics = null;
        _gdiBitmap?.Dispose();
        _gdiBitmap = null;
    }

    // One GDI Bitmap/Graphics and one WriteableBitmap, all created once (in Start()) and reused
    // every tick - only pixel bytes move per frame, no per-frame allocation/GC pressure.
    private void CaptureFrame()
    {
        if (_gdiGraphics is null || _gdiBitmap is null) return;
        try
        {
            // Location (not a hardcoded (0,0)) matters on multi-monitor setups where the primary
            // monitor isn't positioned at the desktop origin.
            _gdiGraphics.CopyFromScreen(_screenBounds.Location, Point.Empty, _screenBounds.Size);

            var locked = _gdiBitmap.LockBits(
                new Rectangle(Point.Empty, _screenBounds.Size),
                ImageLockMode.ReadOnly,
                System.Drawing.Imaging.PixelFormat.Format32bppRgb);
            try
            {
                Preview.WritePixels(
                    new System.Windows.Int32Rect(0, 0, _screenBounds.Width, _screenBounds.Height),
                    locked.Scan0,
                    locked.Stride * _screenBounds.Height,
                    locked.Stride);
            }
            finally
            {
                _gdiBitmap.UnlockBits(locked);
            }
        }
        catch (Exception ex)
        {
            Logger.Log("ScreenPreviewSource", $"screen capture failed, stopping preview: {ex}");
            Stop();
        }
    }

    public void Dispose() => Stop();
}
