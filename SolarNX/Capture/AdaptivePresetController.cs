namespace SolarNX.Capture;

/// <summary>
/// Adaptive NVENC preset (Config.NvencAdaptive): starts at the best quality, p7 / hq, and steps down
/// a ladder when the encoder can't keep up - the frames per second VideoSender actually gets from
/// ffmpeg fall short of the target. The fps filter in front of the encoder pads a static picture up to
/// the full rate, so a shortfall means the encoder itself is too slow, not a quiet screen. Steps back
/// up after a while of running smoothly; an upgrade that fails right away makes the next attempt wait
/// twice as long (up to 20 minutes).
///
/// The network is not this class's business: a bad link is handled by the adaptive bitrate
/// (Session/AdaptiveBitrateController), and a better preset only helps there - it gets more picture
/// out of the same bitrate.
///
/// Each step restarts the encoder (the picture freezes for about a second), so it moves slowly. The
/// level survives sessions within one run of the app.
/// </summary>
public sealed class AdaptivePresetController
{
    public static readonly (string Preset, string Tune)[] Ladder =
        [("p7", "hq"), ("p6", "hq"), ("p5", "hq"), ("p4", "hq"), ("p4", "ll"), ("p3", "ll"), ("p1", "ll")];

    private const double ShortfallRatio = 0.92;
    private const int BadWindowsToStepDown = 2;
    private static readonly TimeSpan FirstUpgradeWait = TimeSpan.FromMinutes(2);
    private static readonly TimeSpan MaxUpgradeWait = TimeSpan.FromMinutes(20);
    private static readonly TimeSpan FailedUpgradeWindow = TimeSpan.FromSeconds(30);

    private readonly object _lock = new();
    private readonly Action<string> _restart;
    private int _level;
    private int _badWindows;
    private bool _skipNextWindow = true;
    private DateTime _lastChangeUtc = DateTime.UtcNow;
    private DateTime? _lastUpgradeUtc;
    private TimeSpan _upgradeWait = FirstUpgradeWait;

    /// <param name="restart">Restarts the encoder with <see cref="Current"/> - called on a step,
    /// from the thread that reported the window (VideoSender's).</param>
    public AdaptivePresetController(Action<string> restart) => _restart = restart;

    public (string Preset, string Tune) Current
    {
        get { lock (_lock) return Ladder[_level]; }
    }

    /// <summary>A new encoder started: its first window includes the start-up, so it doesn't count.</summary>
    public void EncoderStarted()
    {
        lock (_lock)
        {
            _skipNextWindow = true;
            _badWindows = 0;
        }
    }

    /// <summary>One of VideoSender's stats windows: the frames per second it got from ffmpeg.</summary>
    public void OnWindow(double fps, int targetFps)
    {
        string? reason = null;
        lock (_lock)
        {
            if (_skipNextWindow)
            {
                _skipNextWindow = false;
                return;
            }
            var now = DateTime.UtcNow;
            if (fps < targetFps * ShortfallRatio)
            {
                if (++_badWindows < BadWindowsToStepDown || _level == Ladder.Length - 1) return;
                if (_lastUpgradeUtc is { } up && now - up < FailedUpgradeWindow)
                    _upgradeWait = TimeSpan.FromTicks(Math.Min(_upgradeWait.Ticks * 2, MaxUpgradeWait.Ticks));
                _level++;
                reason = $"adaptive NVENC preset: encoder at {fps:0.#} of {targetFps} fps -> {Ladder[_level].Preset}/{Ladder[_level].Tune}";
            }
            else
            {
                _badWindows = 0;
                if (_level == 0 || now - _lastChangeUtc < _upgradeWait) return;
                _level--;
                _lastUpgradeUtc = now;
                reason = $"adaptive NVENC preset: steady for {(now - _lastChangeUtc).TotalMinutes:0} min -> trying {Ladder[_level].Preset}/{Ladder[_level].Tune}";
            }
            _lastChangeUtc = now;
            _badWindows = 0;
        }
        Logger.Log("AdaptivePreset", reason);
        _restart(reason);
    }
}
