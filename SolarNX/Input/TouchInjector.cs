using System.Diagnostics;
using System.Runtime.Versioning;
using SolarNX.Native;

namespace SolarNX.Input;

/// <summary>
/// Turns the HID packet's single touch slot (TouchX1/TouchY1) into a real Windows digitizer touch
/// contact via the Touch Injection API - a genuine touchscreen tap/drag at an absolute screen
/// position, mapped directly to where the second person tapped on their Switch's screen (Steam/
/// games are shown on the real screen, so this is a direct mapping, not a workaround).
///
/// Delegates the actual InjectTouchInput call to a small native helper process (Native/TouchHelper,
/// touchhelper.exe) rather than calling user32.dll directly via P/Invoke from this managed
/// process: calling InjectTouchInput via P/Invoke from SolarNX.exe itself consistently fails
/// with ERROR_INVALID_PARAMETER (87), while the identical call from native code succeeds - so
/// native code owns the actual call, and this class just feeds it coordinates over stdin, one
/// line at a time.
///
/// Single-touch only, by design: holding two fingers down on the Switch makes TouchX1/Y1 itself
/// flicker between the two contact points (the client's single touch slot has no stable
/// per-finger identity when more than one finger is down). TouchX2/Y2 (a second slot in the wire
/// packet) is intentionally never read here.
/// </summary>
[SupportedOSPlatform("windows8.0")]
public sealed class TouchInjector : IDisposable
{
    // The Switch's touch coordinates arrive already normalized into a 1280x720 reference frame
    // (confirmed against the original main.js: it scales via touchX1 * (screenWidth/1280)) -
    // regardless of the configured stream resolution, so this reference size is fixed, not tied to
    // Config.TargetWidth/Height.
    private const int ReferenceWidth = 1280;
    private const int ReferenceHeight = 720;

    private Process? _helper;
    private bool _wasDown;
    private bool _startFailed;

    /// <summary>Feeds one HID packet's touch fields. Coordinates of (0,0) mean "not touching" -
    /// matches the original client behavior (main.js checked `if (touchX1 &amp;&amp; touchY1)`).</summary>
    /// <summary>When the stream shows a single window (library games - see VideoEncoder's
    /// captureWindow), touches map into that window's client area, accounting for the letterbox the
    /// capture adds to keep its aspect ratio. IntPtr.Zero = whole monitor 0.</summary>
    public IntPtr TargetWindow { get; set; }

    public void Update(uint touchX1, uint touchY1)
    {
        var isDown = touchX1 != 0 && touchY1 != 0;

        if (!isDown)
        {
            if (_wasDown) Send("U");
            _wasDown = false;
            return;
        }

        if (!EnsureHelperStarted()) return;

        // Deliberately Screen.AllScreens[0], not Screen.PrimaryScreen: the video actually shown on
        // the Switch comes from ffmpeg's "gfxcapture=monitor_idx=0" (see VideoEncoder.cs) - monitor
        // index 0 in DXGI/WGC's own enumeration order, which isn't guaranteed to be the same
        // physical display as whichever one Windows' Display Settings has marked "primary" on a
        // multi-monitor system. Confirmed live: touches landed with a constant offset in one
        // direction, consistent with capturing one monitor but computing touch coordinates against
        // a different one that happens to sit at a different position in the Windows virtual
        // desktop. AllScreens[0] is a closer match to "monitor 0" than PrimaryScreen is.
        if (TargetWindow != IntPtr.Zero)
        {
            if (!TryMapIntoWindow(TargetWindow, touchX1, touchY1, out var wx, out var wy))
            {
                // Touch on the letterbox bars (or the window is gone) - treat as no touch.
                if (_wasDown) Send("U");
                _wasDown = false;
                return;
            }
            Send(_wasDown ? $"M {wx} {wy}" : $"D {wx} {wy}");
            _wasDown = true;
            return;
        }

        var bounds = System.Windows.Forms.Screen.AllScreens is { Length: > 0 } screens
            ? screens[0].Bounds
            : new System.Drawing.Rectangle(0, 0, ReferenceWidth, ReferenceHeight);
        var x = bounds.X + (int)(touchX1 * (bounds.Width / (double)ReferenceWidth));
        var y = bounds.Y + (int)(touchY1 * (bounds.Height / (double)ReferenceHeight));

        Send(_wasDown ? $"M {x} {y}" : $"D {x} {y}");
        _wasDown = true;
    }

    private static bool TryMapIntoWindow(IntPtr hwnd, uint touchX, uint touchY, out int screenX, out int screenY)
    {
        screenX = screenY = 0;
        if (!WindowGeometry.TryGetClientSize(hwnd, out var clientW, out var clientH)) return false;
        var origin = new POINT();
        if (!ClientToScreen(hwnd, ref origin)) return false;

        // The exact fit VideoEncoder streams (WindowGeometry.Fit, centred with pad), mapped back.
        // Touches arrive in the Switch's 1280x720 frame, which is the stream frame scaled.
        var fit = WindowGeometry.Fit(clientW, clientH, ReferenceWidth, ReferenceHeight);
        var cx = (touchX - fit.OffsetX) * clientW / (double)fit.Width;
        var cy = (touchY - fit.OffsetY) * clientH / (double)fit.Height;
        if (cx < 0 || cy < 0 || cx >= clientW || cy >= clientH) return false;

        screenX = origin.X + (int)cx;
        screenY = origin.Y + (int)cy;
        return true;
    }

    [System.Runtime.InteropServices.StructLayout(System.Runtime.InteropServices.LayoutKind.Sequential)]
    private struct POINT { public int X, Y; }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);

    private bool EnsureHelperStarted()
    {
        if (_helper is { HasExited: false }) return true;
        if (_startFailed) return false;

        try
        {
            var exePath = Path.Combine(AppContext.BaseDirectory, "touchhelper.exe");
            _helper = new Process
            {
                StartInfo = new ProcessStartInfo(exePath)
                {
                    RedirectStandardInput = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    WorkingDirectory = AppContext.BaseDirectory,
                },
            };
            _helper.Start();
            ChildProcessJob.Assign(_helper); // see ChildProcessJob - otherwise orphaned on a crash/debugger stop
            Logger.Log("TouchInjector", "touchhelper.exe started");
            return true;
        }
        catch (Exception ex)
        {
            _startFailed = true;
            Logger.Log("TouchInjector", $"failed to start touchhelper.exe: {ex}");
            return false;
        }
    }

    private void Send(string command)
    {
        try
        {
            _helper?.StandardInput.WriteLine(command);
        }
        catch (Exception ex)
        {
            Logger.Log("TouchInjector", $"failed to send command to touchhelper.exe: {ex}");
        }
    }

    public void Dispose()
    {
        try
        {
            if (_helper is { HasExited: false })
            {
                _helper.StandardInput.Close();
                _helper.WaitForExit(500);
            }
        }
        catch { /* best-effort teardown */ }
        _helper?.Dispose();
        _helper = null;
    }
}
