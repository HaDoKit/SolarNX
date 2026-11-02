using System.Diagnostics;
using SolarNX.Native;

namespace SolarNX.Capture;

/// <summary>
/// Captures the real primary monitor directly via ffmpeg's own "gfxcapture" filter (Windows.Graphics.Capture,
/// WinRT/UWP-based) and encodes it to H264, pushed over udp://clientIp:videoPort as an MPEG-TS stream.
///
/// gfxcapture (not gdigrab/DXGI) is required because it reads directly from the compositor - GDI
/// BitBlt-based capture misses DirectComposition-rendered layers (Steam overlay, some UI, hardware-
/// accelerated elements). Requires ffmpeg 8.x+.
///
/// gfxcapture returns frames at whatever (highly variable) rate the compositor actually delivers
/// them, including 0fps during static periods, so an explicit "fps" filter follows it to pace
/// output at a steady rate for the encoder/stream.
///
/// UDP, not TCP: a TCP connection carrying video/audio/HID together dies every ~7-20 seconds under
/// load; UDP has no connection to drop. MPEG-TS's self-synchronizing packet format (sync bytes,
/// continuity counters) tolerates the resulting packet loss. The Switch client
/// (SolarNX/source/video.c, network.h) decodes via libav (avformat_open_input/avcodec_*).
/// </summary>
public sealed class VideoEncoder : IDisposable
{
    private readonly Config _config;
    private Process? _ffmpeg;

    // Cached for the process lifetime - the check itself takes a moment (spawns ffmpeg) and the
    // answer can't change without a driver update, which needs a restart of this app anyway.
    private static bool? _nvencAvailable;

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

    /// <param name="useNvenc">null (default) auto-detects: probes once whether h264_nvenc can
    /// actually open (this ffmpeg build needs NVIDIA driver 610+ for its NVENC Video Codec SDK
    /// 13.1, or it fails to open at all), and falls back to libx264 (software) if not. Pass
    /// true/false to force one or the other.</param>
    /// <param name="bitrateKbpsOverride">Used by AdaptiveBitrateController (Path 1, ffmpeg
    /// restart-based) to start this process at a lower/raised bitrate than
    /// <see cref="Config.BitrateKbps"/> without touching (and risking persisting) that field -
    /// null (default) uses <see cref="Config.BitrateKbps"/> as-is, matching every non-adaptive
    /// caller.</param>
    /// <param name="captureWindow">Capture just this window (library games), letterboxed into the
    /// stream size with its aspect ratio kept, instead of the whole primary monitor. A game running
    /// in a small window - or behind the first user's own windows - then still fills the Switch
    /// screen. IntPtr.Zero (default) = monitor 0, as for Big Picture.</param>
    public void Start(bool? useNvenc = null, int? bitrateKbpsOverride = null, IntPtr captureWindow = default)
    {
        var ffmpegPath = Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe");
        var bitrateKbps = bitrateKbpsOverride ?? _config.BitrateKbps;
        var bitrate = $"{bitrateKbps}k";
        ActiveBitrateKbps = bitrateKbps;
        // pkt_size=1316 = 7*188 bytes - a whole number of MPEG-TS packets per UDP datagram, sized
        // to stay under a standard 1500-byte-MTU LAN/Wi-Fi link's payload limit and avoid IP
        // fragmentation (a fragmented UDP datagram only needs to lose one fragment to lose the
        // whole packet, which defeats the point of switching off TCP for resilience).
        var target = $"udp://{_config.ClientIp}:{_config.VideoPort}?pkt_size=1316";
        var nvenc = useNvenc ?? IsNvencAvailable(ffmpegPath);

        // -g (keyframe interval): ~0.5s (shorter than the ~2s used under TCP) - bounds how long a
        // lost packet's corruption can visibly propagate via motion compensation before the next
        // keyframe resyncs it, at the cost of more frequent (bigger) keyframes eating into the
        // bitrate budget. A reasonable hedge against ordinary UDP packet loss.
        var keyframeInterval = Math.Max(1, _config.Fps / 2);
        // rc "cbr" (not the original's "-rc vbr -cq:v 19"): a quality-target rate mode fighting the
        // simultaneously-set hard bitrate bounds below causes the encoder to fall behind real time.
        // "p4" (mid-tier NVENC preset, not "p1"/fastest) keeps real motion-search effort while
        // still being a hardware encoder well within real-time budget for cbr. NVENC here requires
        // a fairly recent NVIDIA driver for this ffmpeg build (Video Codec SDK 13.1, driver 610+) -
        // falls back to libx264 (software) if unavailable/unwanted.
        // -delay 0 -rc-lookahead 0 -bf 0: squeezes out NVENC's own internal frame buffering -
        // without these it can hold back a few frames before emitting output even in a
        // zerolatency/low-latency preset, adding a small but real amount of extra input-to-video
        // delay on top of everything else already tuned for latency.
        // -profile:v high (was baseline): confirmed live that at this bitrate the encoder ran at
        // q=32-36 during gameplay (from ffmpeg's own progress lines) - bit-starved enough that
        // text and fine detail visibly smeared. High adds CABAC and the 8x8 transform, typically
        // 15-25% better quality at the same bitrate, with no latency cost: B-frames stay off
        // (-bf 0 / x264's zerolatency tune), which is the part of High that would add delay. The
        // Switch client decodes High fine - NVTEGRA (Tegra X1 NVDEC) supports it, and so does its
        // libavcodec software fallback, just at somewhat more CPU than baseline.
        var encodeArgs = nvenc
            ? $"-c:v h264_nvenc -gpu 0 -preset p4 -tune ll -rc cbr -zerolatency 1 -delay 0 -rc-lookahead 0 -bf 0 -profile:v high -g {keyframeInterval}"
            : $"-c:v libx264 -preset faster -tune zerolatency -profile:v high -crf 18 -g {keyframeInterval}";

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
            var fit = WindowGeometry.Fit(clientW, clientH, _config.TargetWidth, _config.TargetHeight);
            filterChain =
                $"gfxcapture=hwnd={(ulong)captureWindow.ToInt64()}:capture_border=0:capture_cursor=0:resize_mode=scale_aspect:max_framerate={_config.Fps}:width={fit.Width}:height={fit.Height}," +
                $"hwdownload,format=bgra,pad={_config.TargetWidth}:{_config.TargetHeight}:{fit.OffsetX}:{fit.OffsetY}:black,fps={_config.Fps},format=yuv420p";
            CaptureSize = (clientW, clientH);
        }
        else
        {
            if (captureWindow != IntPtr.Zero) Logger.Log("ffmpeg-video", $"window {captureWindow} has no client area - capturing the monitor instead");
            CaptureWindow = IntPtr.Zero;
            filterChain = $"gfxcapture=monitor_idx=0:resize_mode=scale:max_framerate={_config.Fps}:width={_config.TargetWidth}:height={_config.TargetHeight},hwdownload,format=bgra,fps={_config.Fps},format=yuv420p";
        }

        var args = string.Join(' ', new[]
        {
            "-fflags nobuffer -flags low_delay -probesize 32",
            $"-filter_complex \"{filterChain}\"",
            "-f mpegts",
            encodeArgs,
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
        Logger.Log("ffmpeg-video", $"starting: {ffmpegPath} {args}");
        _ffmpeg.Start();
        ChildProcessJob.Assign(_ffmpeg); // dies with this process even on a crash/debugger stop - see ChildProcessJob
        _ffmpeg.BeginErrorReadLine();
    }

    /// <summary>Encodes one throwaway frame with h264_nvenc as a real test, since a too-old NVIDIA
    /// driver makes this ffmpeg build's h264_nvenc fail to open at all ("Driver does not support
    /// the required nvenc API version") - a quick real probe is more reliable than trying to parse
    /// driver/GPU info ourselves. 256x256, not smaller: 64x64 fails with "Frame Dimension less than
    /// the minimum supported value", an NVENC resolution floor unrelated to driver compatibility
    /// that would otherwise look like a false negative.</summary>
    private static bool IsNvencAvailable(string ffmpegPath)
    {
        if (_nvencAvailable is not null) return _nvencAvailable.Value;

        try
        {
            using var process = new Process
            {
                StartInfo = new ProcessStartInfo(ffmpegPath,
                    "-f lavfi -i color=black:s=256x256:d=0.1 -frames:v 1 -c:v h264_nvenc -f null -")
                {
                    RedirectStandardError = true,
                    UseShellExecute = false,
                    CreateNoWindow = true,
                },
            };
            process.Start();
            process.StandardError.ReadToEnd();
            process.WaitForExit(5000);
            _nvencAvailable = process.ExitCode == 0;
        }
        catch (Exception ex)
        {
            Logger.Log("ffmpeg-video", $"NVENC probe failed: {ex}");
            _nvencAvailable = false;
        }

        Logger.Log("ffmpeg-video", _nvencAvailable.Value
            ? "NVENC probe succeeded - using h264_nvenc"
            : "NVENC probe failed - falling back to libx264 (software)");
        return _nvencAvailable.Value;
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
        ActiveBitrateKbps = null;
        CaptureWindow = IntPtr.Zero;
        CaptureSize = default;
    }

    public void Dispose() => Stop();
}
