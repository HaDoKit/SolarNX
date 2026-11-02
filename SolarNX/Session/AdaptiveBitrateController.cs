using SolarNX.Input;

namespace SolarNX.Session;

/// <summary>
/// Watches the link-quality telemetry the client stamps onto every HidPacket (see
/// Input/HidProtocol.cs's VideoReconnectCount/VideoLostFrameCount/AudioReconnectCount/
/// AudioConcealedFrameCount) and, when Config.AdaptiveBitrate is on, lowers or raises the running
/// ffmpeg video encoder's bitrate between <see cref="MinBitrateKbps"/> and Config.BitrateKbps (the
/// ceiling) by restarting it - VideoEncoder wraps ffmpeg.exe as a subprocess with no live-reconfigure
/// hook of its own, so a restart (a brief, usually sub-second glitch the client's existing UDP
/// reconnect/fifo logic already tolerates) is the only way to change its bitrate mid-session.
///
/// Bitrate changes are decided on a fixed evaluation window (<see cref="EvaluationInterval"/>), not
/// per-packet (HidPacket arrives at ~120Hz - reacting per-packet would be noisy and would restart
/// ffmpeg far too often). <see cref="OnPacketReceived"/> just remembers the latest packet; an
/// internal timer ticks independently and diffs it against the previous window's snapshot.
/// </summary>
public sealed class AdaptiveBitrateController : IDisposable
{
    private readonly Config _config;
    private readonly Action<int> _applyBitrate;

    private static readonly TimeSpan EvaluationInterval = TimeSpan.FromSeconds(5);

    // Minimum time between two bitrate changes, in either direction - a change is itself a brief
    // disruption (an ffmpeg restart), so this must not fire every single evaluation tick under
    // sustained bad conditions or the encoder would spend more time restarting than encoding.
    private static readonly TimeSpan MinChangeInterval = TimeSpan.FromSeconds(8);

    // Consecutive clean windows required before cautiously raising bitrate back up - much more
    // conservative than lowering (which reacts to the very next bad window) since raising risks
    // re-triggering the exact congestion that caused the previous drop.
    private const int GoodWindowsBeforeRaise = 6;

    // A handful of concealed (PLC) audio frames in one window is ordinary jitter, not real loss -
    // Opus frames are 20ms each, so this is ~200ms of concealed audio across a 5s window before the
    // window counts as "bad".
    private const int ConcealedFrameBadThreshold = 10;

    // A lost video frame is one the Reed-Solomon parity could not save - the picture then holds
    // until the next keyframe (up to ~0.5s). One in a window is a stray burst; a few mean the link
    // loses more than the ~20% parity covers, and a lower bitrate (fewer datagrams) helps.
    private const int LostVideoFrameBadThreshold = 3;

    private const double LowerFactor = 0.75;
    private const double RaiseFactor = 1.10;

    // Never sent to ffmpeg below this regardless of how bad the link looks - a picture is still
    // meant to be visible. Not user-configurable (unlike Config.BitrateKbps, the ceiling this scales
    // down from) - there's no evidence yet a fixed floor needs to vary per setup.
    private const int MinBitrateKbps = 1500;

    private readonly System.Threading.Timer _timer;
    private readonly object _gate = new();

    private uint _lastVideoReconnectCount;
    private uint _lastVideoLostFrameCount;
    private uint _lastAudioReconnectCount;
    private uint _lastAudioConcealedFrameCount;
    private bool _baselineCaptured;
    private bool _wasEnabledLastTick;

    private HidPacket? _latestPacket;

    private int _currentBitrateKbps;
    private DateTime _lastChangeUtc = DateTime.MinValue;
    private int _goodWindowStreak;

    public AdaptiveBitrateController(Config config, Action<int> applyBitrate)
    {
        _config = config;
        _applyBitrate = applyBitrate;
        _timer = new System.Threading.Timer(_ => Evaluate(), null, Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
    }

    /// <summary>Call once whenever the capture pipeline (re)starts (AppOrchestrator.OnSessionStarted)
    /// - clears the running baseline (the client's own telemetry counters keep climbing while no
    /// one is connected, e.g. video.c's reconnect-wait loop, so diffing against a pre-connect value
    /// would look like a burst of link failures on the very first evaluation window) and starts the
    /// evaluation timer.</summary>
    public void Start(int startingBitrateKbps)
    {
        lock (_gate)
        {
            _baselineCaptured = false;
            _wasEnabledLastTick = false;
            _currentBitrateKbps = startingBitrateKbps;
            _goodWindowStreak = 0;
            _lastChangeUtc = DateTime.MinValue;
            _latestPacket = null;
        }
        _timer.Change(EvaluationInterval, EvaluationInterval);
    }

    /// <summary>Call once whenever the capture pipeline stops (AppOrchestrator.OnSessionStopped) -
    /// stops the evaluation timer until the next Start().</summary>
    public void Stop()
    {
        _timer.Change(Timeout.InfiniteTimeSpan, Timeout.InfiniteTimeSpan);
    }

    /// <summary>Cheap - just remembers the latest packet for the next timer tick to diff against.
    /// Called from SessionController's PacketReceived event (~43Hz). No-ops while adaptive mode is
    /// off so a disabled controller costs nothing beyond the field write check.</summary>
    public void OnPacketReceived(HidPacket packet)
    {
        if (!_config.AdaptiveBitrate) return;
        lock (_gate) { _latestPacket = packet; }
    }

    private void Evaluate()
    {
        if (!_config.AdaptiveBitrate)
        {
            lock (_gate) { _wasEnabledLastTick = false; }
            return;
        }

        HidPacket? packet;
        lock (_gate) { packet = _latestPacket; }
        if (packet is null) return; // no client connected / no packet seen yet this window

        lock (_gate)
        {
            // Also re-baselines on every off->on transition (the checkbox can be toggled
            // mid-session) - otherwise the first window after re-enabling would diff against
            // counters from whenever it was last on, misreading stale history as a fresh burst of
            // link failures.
            if (!_baselineCaptured || !_wasEnabledLastTick)
            {
                _lastVideoReconnectCount = packet.VideoReconnectCount;
                _lastVideoLostFrameCount = packet.VideoLostFrameCount;
                _lastAudioReconnectCount = packet.AudioReconnectCount;
                _lastAudioConcealedFrameCount = packet.AudioConcealedFrameCount;
                _baselineCaptured = true;
                _wasEnabledLastTick = true;
                return; // this window only establishes the baseline, no decision yet
            }
            _wasEnabledLastTick = true;

            var deltaVideoReconnects = Unwrap(packet.VideoReconnectCount, _lastVideoReconnectCount);
            var deltaLostFrames = Unwrap(packet.VideoLostFrameCount, _lastVideoLostFrameCount);
            var deltaAudioReconnects = Unwrap(packet.AudioReconnectCount, _lastAudioReconnectCount);
            var deltaConcealed = Unwrap(packet.AudioConcealedFrameCount, _lastAudioConcealedFrameCount);

            _lastVideoReconnectCount = packet.VideoReconnectCount;
            _lastVideoLostFrameCount = packet.VideoLostFrameCount;
            _lastAudioReconnectCount = packet.AudioReconnectCount;
            _lastAudioConcealedFrameCount = packet.AudioConcealedFrameCount;

            var bad = deltaVideoReconnects > 0 || deltaLostFrames >= LostVideoFrameBadThreshold
                || deltaAudioReconnects > 0 || deltaConcealed > ConcealedFrameBadThreshold;
            var sinceLastChange = DateTime.UtcNow - _lastChangeUtc;

            if (bad)
            {
                _goodWindowStreak = 0;
                if (sinceLastChange < MinChangeInterval) return;

                var lowered = Math.Max(MinBitrateKbps, (int)(_currentBitrateKbps * LowerFactor));
                if (lowered == _currentBitrateKbps) return; // already at the floor
                Logger.Log("AdaptiveBitrateController",
                    $"lowering bitrate {_currentBitrateKbps} -> {lowered} kbps (videoReconnects+{deltaVideoReconnects}, lostFrames+{deltaLostFrames}, audioReconnects+{deltaAudioReconnects}, concealed+{deltaConcealed})");
                ApplyLocked(lowered);
            }
            else
            {
                _goodWindowStreak++;
                if (_currentBitrateKbps >= _config.BitrateKbps) return; // already at the ceiling
                if (_goodWindowStreak < GoodWindowsBeforeRaise) return;
                if (sinceLastChange < MinChangeInterval) return;

                var raised = Math.Min(_config.BitrateKbps, (int)(_currentBitrateKbps * RaiseFactor));
                if (raised == _currentBitrateKbps) return;
                Logger.Log("AdaptiveBitrateController",
                    $"raising bitrate {_currentBitrateKbps} -> {raised} kbps ({_goodWindowStreak} clean windows)");
                ApplyLocked(raised);
            }
        }
    }

    private void ApplyLocked(int newBitrateKbps)
    {
        _currentBitrateKbps = newBitrateKbps;
        _lastChangeUtc = DateTime.UtcNow;
        _goodWindowStreak = 0;
        _applyBitrate(newBitrateKbps);
    }

    // Counters are free-running uint32 on the client (see network.h's telemetry field doc comment)
    // - a session long enough to actually wrap one isn't realistic (billions of reconnects), but
    // subtracting unsigned values the naive way would still misbehave if "current" ever ended up
    // smaller than "last" (e.g. the client process itself restarting mid-session) - clamp to zero
    // instead of wrapping to a huge positive delta that would look like a catastrophic link failure.
    private static uint Unwrap(uint current, uint last) => current >= last ? current - last : 0;

    public void Dispose() => _timer.Dispose();
}
