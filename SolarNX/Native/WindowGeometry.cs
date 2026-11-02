using System.Runtime.InteropServices;

namespace SolarNX.Native;

/// <summary>
/// Client-area size of another process's window, and the letterbox fit used when that window is
/// streamed. Shared by VideoEncoder (to scale the captured window to exactly the fitted size and
/// centre it), TouchInjector (to map Switch touches back through the same fit) and AppOrchestrator
/// (to restart the capture when the game window changes size), so all three agree on the geometry.
/// </summary>
internal static class WindowGeometry
{
    public static bool TryGetClientSize(IntPtr hwnd, out int width, out int height)
    {
        width = height = 0;
        if (hwnd == IntPtr.Zero || !GetClientRect(hwnd, out var r)) return false;
        width = r.Right - r.Left;
        height = r.Bottom - r.Top;
        return width > 0 && height > 0;
    }

    /// <summary>Largest size with the source's aspect ratio that fits targetW×targetH, rounded down to
    /// even numbers (yuv420p/NVENC need even dimensions), plus the offsets that centre it.</summary>
    public static (int Width, int Height, int OffsetX, int OffsetY) Fit(int sourceW, int sourceH, int targetW, int targetH)
    {
        var scale = Math.Min((double)targetW / sourceW, (double)targetH / sourceH);
        var w = Math.Max(2, (int)(sourceW * scale) & ~1);
        var h = Math.Max(2, (int)(sourceH * scale) & ~1);
        return (w, h, (targetW - w) / 2, (targetH - h) / 2);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")]
    private static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
}
