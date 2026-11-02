namespace SolarNX;

/// <summary>Minimal file logger for diagnosing ffmpeg/child-process output during development.</summary>
internal static class Logger
{
    private static readonly object Lock = new();
    private static readonly string LogPath = Path.Combine(AppContext.BaseDirectory, "SolarNX.log");

    // Truncated once per process start, not appended forever, so the file only ever reflects the
    // build that's actually running right now.
    private static bool _truncatedThisRun;

    public static void Log(string category, string message)
    {
        var line = $"[{DateTime.Now:HH:mm:ss.fff}] [{category}] {message}";
        lock (Lock)
        {
            try
            {
                if (!_truncatedThisRun)
                {
                    File.WriteAllText(LogPath, string.Empty);
                    _truncatedThisRun = true;
                }
                File.AppendAllText(LogPath, line + Environment.NewLine);
            }
            catch { /* best-effort logging only */ }
        }
    }
}
