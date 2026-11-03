using System.Diagnostics;
using SolarNX.Native;

namespace SolarNX.Capture;

/// <summary>
/// Captures the real primary monitor directly via ffmpeg's own "gfxcapture" filter (Windows.Graphics.Capture,
/// WinRT/UWP-based) and encodes it to H.265 or H.264 (<see cref="Config.Codec"/>).
///
/// gfxcapture (not gdigrab/DXGI) is required because it reads directly from the compositor - GDI
/// BitBlt-based capture misses DirectComposition-rendered layers (Steam overlay, some UI, hardware-
/// accelerated elements). Requires ffmpeg 8.x+.
///
/// ffmpeg's output is RTP to <see cref="VideoSender"/> on loopback, which rebuilds each frame and
/// sends it on to the Switch in SolarNX's own framing with Reed-Solomon parity - see that class.
///
/// gfxcapture returns frames at whatever (highly variable) rate the compositor actually delivers
/// them, including 0fps during static periods, so an explicit "fps" filter follows it to pace
/// output at a steady rate for the encoder/stream.
///
/// UDP, not TCP: a TCP connection carrying video/audio/HID together dies every ~7-20 seconds under
/// load; UDP has no connection to drop, and the parity rebuilds most lost datagrams. The Switch
/// client (Client/video/video.c) reassembles the frames and decodes them with libavcodec.
/// </summary>
public sealed class VideoEncoder : IDisposable
{
    private readonly Config _config;
    private Process? _ffmpeg;
    private VideoSender? _sender;

    // Updated from ffmpeg's own periodic stderr progress lines ("frame=... fps=... time=...") -
    // used by AppOrchestrator's watchdog to detect a hung capture. Confirmed live: video can freeze
    // for many seconds (audio/gamepad input kept working throughout) with nothing in this class
    // noticing or recovering on its own - unlike AudioCaptureSender's UDP session, which just
    // re-opens every second on its own, this process was never restarted unless the whole client
    // session dropped. The most likely cause is Windows.Graphics.Capture itself briefly losing its
    // capture source during a game's fullscreen-mode transition (a known WGC quirk) - gfxcapture
    // then just stops handing ffmpeg new frames until WGC recovers on its own, however long that
    // takes, with no visible error on this side to react to.
    private DateTime _lastProgressUtc = DateTime.UtcNow;
    private DateTime _startedUtc = DateTime.UtcNow;
    private bool _hasSeenProgress;

    // Confirmed live: the watchdog's own restart could itself cause exactly the freeze it was meant
    // to fix. IsStalled used the same threshold (5s) for "no progress yet since this process was
    // just spawned" as for "was running fine, then went quiet" - but cold start (NVENC/WGC capture
    // session re-init, right after the previous process was just killed, often under load) can
    // legitimately take longer than 5s to produce its first frame. A cold start that runs a little
    // long got misread as a hang, triggering another restart before the process ever got a chance
    // to produce output - repeating indefinitely, which explains the reported "video became very
    // slow" (it was stuck restarting, never actually streaming) and the audio degradation alongside
    // it (constant process spawn/NVENC re-init churn is real CPU/GPU load that competes with the
    // real-time audio threads). A separate, longer grace period before the *first* frame fixes this
    // without weakening detection of a genuine mid-stream hang.
    private static readonly TimeSpan StartupGrace = TimeSpan.FromSeconds(15);

    public VideoEncoder(Config config)
    {
        _config = config;
    }

    /// <summary>The bitrate actually in use by the currently-running ffmpeg process, or null if
    /// none is running - <see cref="Config.BitrateKbps"/> alone isn't enough once
    /// AdaptiveBitrateController can start a process at a lower value than the user's configured
    /// ceiling (see Start's bitrateKbpsOverride parameter).</summary>
    public int? ActiveBitrateKbps { get; private set; }

    /// <summary>The encoder the running ffmpeg process uses (resolved from
    /// <see cref="Config.Encoder"/> - see <see cref="EncoderSelection.Resolve"/>), or null if none is
    /// running.</summary>
    public EncoderKind? ActiveEncoder { get; private set; }

    /// <summary>The codec the running ffmpeg process streams, or null if none is running.</summary>
    public VideoCodec? ActiveCodec { get; private set; }
    /// <summary>Stream size the running ffmpeg process encodes at, or null if none is running.</summary>
    public (int Width, int Height)? ActiveSize { get; private set; }

    /// <summary>The NVENC preset and tuning the running process uses (meaningful with NVENC only).</summary>
    public (string Preset, string Tune)? ActiveNvenc { get; private set; }

    /// <summary>Frames per second VideoSender got from ffmpeg, every 5s (VideoSender.WindowFps).</summary>
    public event Action<double>? OutputFps;

    /// <summary>The window being captured instead of the monitor (IntPtr.Zero = whole primary
    /// monitor). Lets a restart (watchdog / adaptive bitrate) carry the same capture target over.</summary>
    public IntPtr CaptureWindow { get; private set; }

    /// <summary>Client size of <see cref="CaptureWindow"/> the current fit was computed for - when the
    /// window's size moves away from this, the capture needs restarting with a new fit.</summary>
    public (int Width, int Height) CaptureSize { get; private set; }

    /// <summary>True once ffmpeg has gone too long without reporting encode progress - before the
    /// first frame, "too long" is <see cref="StartupGrace"/> (cold start can legitimately take a
    /// while); afterward it's <paramref name="threshold"/>, the caller-supplied steady-state limit.
    /// See the doc comments on <see cref="_lastProgressUtc"/>/<see cref="StartupGrace"/> for why
    /// these are different.</summary>
    public bool IsStalled(TimeSpan threshold)
    {
        if (_ffmpeg is null) return false;
        var (reference, effectiveThreshold) = _hasSeenProgress
            ? (_lastProgressUtc, threshold)
            : (_startedUtc, StartupGrace);
        return DateTime.UtcNow - reference > effectiveThreshold;
    }

    /// <param name="encoderOverride">null (default) uses <see cref="Config.Encoder"/>. Either way the
    /// choice goes through <see cref="EncoderSelection.Resolve"/>, which falls back to whatever really
    /// works here (hardware encoders are probed once per process).</param>
    /// <param name="bitrateKbpsOverride">Used by AdaptiveBitrateController (Path 1, ffmpeg
    /// restart-based) to start this process at a lower/raised bitrate than
    /// <see cref="Config.BitrateKbps"/> without touching (and risking persisting) that field -
    /// null (default) uses <see cref="Config.BitrateKbps"/> as-is, matching every non-adaptive
    /// caller.</param>
    /// <param name="captureWindow">Capture just this window (library games), letterboxed into the
    /// stream size with its aspect ratio kept, instead of the whole primary monitor. A game running
    /// in a small window - or behind the first user's own windows - then still fills the Switch
    /// screen. IntPtr.Zero (default) = monitor 0, as for Big Picture.</param>
    /// <param name="streamSize">Stream resolution - AppOrchestrator picks it (the Switch's screen with
    /// automatic resolution); null (default) uses <see cref="Config.TargetWidth"/>/<see cref="Config.TargetHeight"/>.</param>
    /// <param name="nvencOverride">NVENC preset and tuning from the adaptive preset; null uses
    /// <see cref="Config.NvencPreset"/> / <see cref="Config.NvencTune"/>.</param>
    public void Start(EncoderKind? encoderOverride = null, int? bitrateKbpsOverride = null, IntPtr captureWindow = default,
        (int Width, int Height)? streamSize = null, (string Preset, string Tune)? nvencOverride = null)
    {
        var (streamWidth, streamHeight) = streamSize ?? (_config.TargetWidth, _config.TargetHeight);
        ActiveSize = (streamWidth, streamHeight);
        var ffmpegPath = Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe");
        var bitrateKbps = bitrateKbpsOverride ?? _config.BitrateKbps;
        var bitrate = $"{bitrateKbps}k";
        ActiveBitrateKbps = bitrateKbps;
        // Internet mode: to the loopback relay, which forwards each datagram to the Switch over the
        // session's one UDP port (Session/InternetSession).
        var switchEndpoint = _config.ConnectionMode == Session.ConnectionMode.Internet
            ? new System.Net.IPEndPoint(System.Net.IPAddress.Loopback, Session.InternetSession.VideoRelayPort)
            : VideoSender.ResolveTarget(_config.ClientIp, _config.VideoPort);
        var encoder = EncoderSelection.Resolve(encoderOverride ?? _config.Encoder);
        var codec = EncoderSelection.ResolveCodec(encoder, _config.Codec);
        ActiveEncoder = encoder;
        ActiveCodec = codec;
        var pixelFormat = EncoderSelection.PixelFormat(encoder);
        _sender = new VideoSender(switchEndpoint, _config.Fps, codec);
        _sender.WindowFps += fps => OutputFps?.Invoke(fps);
        // RTP over plain udp:// (not rtp://, which would also send RTCP to a second port nobody
        // listens on). Loopback has no 1500-byte MTU: big packets keep a frame to a few datagrams.
        var target = $"udp://127.0.0.1:{_sender.RtpPort}?pkt_size=16384";

        // -g (keyframe interval): ~0.5s (shorter than the ~2s used under TCP) - bounds how long a
        // lost packet's corruption can visibly propagate via motion compensation before the next
        // keyframe resyncs it, at the cost of more frequent (bigger) keyframes eating into the
        // bitrate budget. A reasonable hedge against ordinary UDP packet loss.
        var keyframeInterval = Math.Max(1, _config.Fps / 2);
        // Constant bitrate, High (H.264) / Main (H.265) profile, no B-frames/lookahead for every encoder - see
        // EncoderSelection.EncodeArgs. -profile:v high (was baseline): confirmed live that the encoder
        // ran at q=32-36 during gameplay at this bitrate; High (CABAC, 8x8 transform) gives ~15-25%
        // better quality at the same bitrate with no latency cost. The Switch decodes High (NVTEGRA).
        // A quality-target rate mode (the original "-rc vbr -cq:v 19") fought the hard bitrate bounds
        // below and made the encoder fall behind real time - keep rate control constant-bitrate.
        var nvenc = nvencOverride ?? (_config.NvencPreset, _config.NvencTune);
        ActiveNvenc = nvenc;
        var encodeArgs = EncoderSelection.EncodeArgs(encoder, codec, keyframeInterval, nvenc.Preset, nvenc.Tune);

        // gfxcapture scales to the stream's target resolution itself (resize_mode=scale) - no
        // separate -vf scale step needed. hwdownload+format=bgra brings the D3D11 hardware frame
        // back to system memory (gfxcapture always returns hardware frames); the trailing "fps"
        // filter re-paces the compositor's inherently variable frame delivery to a steady rate.
        CaptureWindow = captureWindow;
        string filterChain;
        if (captureWindow != IntPtr.Zero && WindowGeometry.TryGetClientSize(captureWindow, out var clientW, out var clientH))
        {
            // Window capture: gfxcapture scales on the GPU to exactly the aspect-preserving fit of the
            // window's client area, then pad centres it in the stream frame. Letting gfxcapture fit into
            // the full stream size itself (resize_mode=scale_aspect with width/height = stream size) was
            // confirmed to pin the picture to the top-left with a bar only on the right/bottom - off
            // centre, and out of step with TouchInjector's mapping. resize_mode=scale_aspect is kept for
            // a resize mid-capture (content shrinks inside the fixed size instead of being cropped) until
            // AppOrchestrator notices the new size and restarts with a new fit. capture_cursor=0: the
            // first user's mouse pointer has no business in the Switch player's picture.
            var fit = WindowGeometry.Fit(clientW, clientH, streamWidth, streamHeight);
            filterChain =
                $"gfxcapture=hwnd={(ulong)captureWindow.ToInt64()}:capture_border=0:capture_cursor=0:resize_mode=scale_aspect:max_framerate={_config.Fps}:width={fit.Width}:height={fit.Height}," +
                $"hwdownload,format=bgra,pad={streamWidth}:{streamHeight}:{fit.OffsetX}:{fit.OffsetY}:black,fps={_config.Fps},format={pixelFormat}";
            CaptureSize = (clientW, clientH);
        }
        else
        {
            if (captureWindow != IntPtr.Zero) Logger.Log("ffmpeg-video", $"window {captureWindow} has no client area - capturing the monitor instead");
            CaptureWindow = IntPtr.Zero;
            filterChain = $"gfxcapture=monitor_idx=0:resize_mode=scale:max_framerate={_config.Fps}:width={streamWidth}:height={streamHeight},hwdownload,format=bgra,fps={_config.Fps},format={pixelFormat}";
        }

        var args = string.Join(' ', new[]
        {
            "-fflags nobuffer -flags low_delay -probesize 32",
            $"-filter_complex \"{filterChain}\"",
            "-f rtp",
            encodeArgs,
            // SPS/PPS in front of every keyframe, whichever encoder - the Switch has no other way to
            // get them (no SDP), and needs them to start decoding at any keyframe.
            "-bsf:v dump_extra=freq=keyframe",
            $"-b:v {bitrate} -minrate {bitrate} -maxrate {bitrate} -bufsize {bitrate}",
            // -flush_packets 1: pushes each encoded packet onto the socket immediately instead of
            // letting ffmpeg's own output muxer batch them up first.
            "-flush_packets 1",
            target,
        });

        _ffmpeg = new Process
        {
            StartInfo = new ProcessStartInfo(ffmpegPath, args)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
            },
        };
        _startedUtc = DateTime.UtcNow;
        _hasSeenProgress = false;
        _ffmpeg.ErrorDataReceived += (_, e) =>
        {
            if (e.Data is null) return;
            Logger.Log("ffmpeg-video", e.Data);
            // ffmpeg emits a "frame=... fps=... q=... size=... time=... bitrate=... speed=..."
            // progress line on stderr roughly every time it flushes a status update - any line
            // containing this is proof the encode loop is still actually running, which is exactly
            // what IsStalled needs regardless of the specific numbers in it.
            if (e.Data.Contains("frame=", StringComparison.Ordinal))
            {
                _lastProgressUtc = DateTime.UtcNow;
                _hasSeenProgress = true;
            }
        };
        // Captures the local, not the _ffmpeg field: Exited is raised on a thread-pool thread after
        // Stop() has already Kill()ed, Dispose()d and nulled _ffmpeg - reading the field there was a
        // NullReferenceException (or InvalidOperationException from a disposed Process), and an
        // unhandled exception in a thread-pool callback terminates the whole app.
        var process = _ffmpeg;
        process.Exited += (_, _) =>
        {
            try { Logger.Log("ffmpeg-video", $"process exited, code={process.ExitCode}"); }
            catch (Exception) { Logger.Log("ffmpeg-video", "process exited (exit code unavailable - already disposed)"); }
        };
        _ffmpeg.EnableRaisingEvents = true;
        Logger.Log("ffmpeg-video", $"encoder: {EncoderSelection.DisplayName(encoder)}, {EncoderSelection.DisplayName(codec)}, {streamWidth}x{streamHeight}");
        Logger.Log("ffmpeg-video", $"starting: {ffmpegPath} {args}");
        _ffmpeg.Start();
        ChildProcessJob.Assign(_ffmpeg); // dies with this process even on a crash/debugger stop - see ChildProcessJob
        _ffmpeg.BeginErrorReadLine();
        _ffmpeg.BeginOutputReadLine(); // the rtp muxer prints its SDP there - read so the pipe can't fill
    }

    public void Stop()
    {
        try
        {
            if (_ffmpeg is { HasExited: false })
            {
                _ffmpeg.Kill();
                _ffmpeg.WaitForExit(2000);
            }
        }
        catch { /* best-effort teardown */ }
        _ffmpeg?.Dispose();
        _ffmpeg = null;
        _sender?.Dispose();
        _sender = null;
        ActiveBitrateKbps = null;
        ActiveEncoder = null;
        ActiveCodec = null;
        ActiveSize = null;
        CaptureWindow = IntPtr.Zero;
        CaptureSize = default;
    }

    public void Dispose() => Stop();
}
