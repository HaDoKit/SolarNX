using System.Net.Sockets;
using System.Runtime.InteropServices;
using Concentus;
using Concentus.Enums;
using NAudio.CoreAudioApi;
using NAudio.Wave;
using SolarNX.Native;

namespace SolarNX.Audio;

/// <summary>
/// Captures system audio via WASAPI loopback (no third-party driver) and registers its own capture
/// and network-send threads with MMCSS's "Pro Audio" task (see Native/Mmcss.cs) for consistently
/// prompt scheduling under load.
///
/// Wire format: Opus over RTP (RFC 7587), sent over plain UDP. There is no hand-rolled loss
/// handling here - RTP framing and Opus decoding both run through the Switch client's own libav
/// (SolarNX/source/audio.c), and Opus's built-in packet-loss concealment synthesizes plausible audio
/// for a missing frame instead of silence or a naive repeat. Encoding happens in-process via
/// Concentus (a pure C# Opus port).
///
/// UDP has no connection to drop, so there's no reconnect logic here - a failed send is just
/// logged and the loop continues; the client's RTP jitter buffer/Opus PLC absorbs gaps.
/// </summary>
public sealed class AudioCaptureSender : IDisposable
{
    private const string MmcssTaskName = "Pro Audio";

    // 20ms is the conventional real-time-streaming default for Opus - short enough to keep latency
    // low, long enough that per-packet overhead (RTP header, UDP/IP headers) stays a small fraction
    // of the payload.
    private const int FrameDurationMs = 20;

    // Dynamic RTP payload type (RFC 3551 reserves 96-127 for this) - the exact number only has to
    // agree with what the client's static SDP declares (SolarNX/source/audio.c).
    private const byte OpusPayloadType = 97;

    // Comfortably transparent for stereo general (non-speech) audio at Opus's efficiency - well
    // under the video stream's own bitrate, so this isn't competing meaningfully for bandwidth.
    // internal, not private: SessionController references this when building the SessionConfig
    // it sends the client, so the two stay in sync instead of duplicating the literal.
    internal const int OpusBitrateBps = 128_000;

    private readonly Config _config;

    private WasapiLoopbackCapture? _capture;
    private BufferedWaveProvider? _buffer;
    private IWaveProvider? _pcmSource;
    private Thread? _senderThread;
    private volatile bool _stopping;

    private UdpClient? _udpClient;
    private WaveFileWriter? _debugWriter;
    private IOpusEncoder? _opusEncoder;
    private RtpPacketizer? _rtpPacketizer;

    private IntPtr _captureMmcssHandle;
    private bool _captureMmcssApplied;
    private readonly ManualResetEventSlim _captureStoppedEvent = new(false);

    // Throughput visibility: how many bytes/packets actually went out and over what wall-clock
    // span, logged periodically rather than per-packet (at 50 packets/sec that would be far too
    // noisy to read) - this is the only place in the app that answers "how much audio data is
    // actually being sent, and how fast" instead of just "did a send fail".
    private static readonly TimeSpan ThroughputLogInterval = TimeSpan.FromSeconds(5);
    private long _bytesSinceLastLog;
    private long _packetsSinceLastLog;
    private DateTime _lastThroughputLogUtc;

    public AudioCaptureSender(Config config)
    {
        _config = config;
    }

    public void Start()
    {
        if (_config.AudioDebugRecordToFile)
        {
            // Diagnostic-only: isolates this app's own capture chain from the network/Opus/client
            // decode entirely by recording the pre-encode PCM straight to a local file.
            _debugWriter = new WaveFileWriter(Path.Combine(AppContext.BaseDirectory, "audio_debug.wav"), TargetFormat());
        }
        else
        {
            _udpClient = new UdpClient();
            // Internet mode: to the loopback relay of Session/InternetSession, which forwards to the Switch.
            if (_config.ConnectionMode == Session.ConnectionMode.Internet)
                _udpClient.Connect("127.0.0.1", Session.InternetSession.AudioRelayPort);
            else
                _udpClient.Connect(_config.ClientIp, _config.AudioPort);
            // OpusCodecFactory (not the OpusEncoder constructor directly) - lets Concentus pick a
            // native implementation if the platform supports one, falling back to its pure-managed
            // encoder otherwise; the constructor-based API is obsolete precisely because it always
            // forces the managed path.
            _opusEncoder = OpusCodecFactory.CreateEncoder(_config.AudioSampleRate, _config.AudioChannels, OpusApplication.OPUS_APPLICATION_AUDIO, messageLogger: null);
            _opusEncoder.Bitrate = OpusBitrateBps;
            _rtpPacketizer = new RtpPacketizer(OpusPayloadType);
        }

        _capture = new WasapiLoopbackCapture();
        _buffer = new BufferedWaveProvider(_capture.WaveFormat)
        {
            // ReadFully defaults to true, which pads a Read() with silence instead of returning
            // fewer bytes when nothing new has arrived yet - that would let SenderLoop's tight pull
            // loop race far ahead of real audio arrival, sending mostly silence-padded garbage at
            // whatever rate the CPU allowed instead of real time. false makes Read() return only
            // what's actually been captured (down to 0), which is what SenderLoop's "sleep if
            // nothing came back" logic needs to pace itself against real audio.
            ReadFully = false,
            DiscardOnBufferOverflow = true,
            // Short on purpose: bounds how much lag can silently build up before the overflow
            // discard kicks in and catches back up.
            BufferDuration = TimeSpan.FromMilliseconds(300),
        };
        _pcmSource = CreatePcmSource(_buffer, _capture.WaveFormat, TargetFormat());

        _captureStoppedEvent.Reset();
        _capture.DataAvailable += OnDataAvailable;
        _capture.RecordingStopped += (_, e) =>
        {
            if (e.Exception is not null) Logger.Log("AudioCaptureSender", $"WASAPI capture stopped unexpectedly: {e.Exception}");
            _captureStoppedEvent.Set();
        };

        Logger.Log("AudioCaptureSender", $"starting: source={_capture.WaveFormat}, target={TargetFormat()}, pcmSource={_pcmSource.GetType().Name}, gain={_config.AudioGain}, debugFile={_config.AudioDebugRecordToFile}");
        _capture.StartRecording();

        _stopping = false;
        _senderThread = new Thread(SenderLoop) { IsBackground = true, Priority = ThreadPriority.Highest };
        _senderThread.Start();
    }

    private WaveFormat TargetFormat() => new(_config.AudioSampleRate, 16, _config.AudioChannels);

    /// <summary>The device's shared-mode mix format is essentially always 32-bit IEEE float at
    /// whatever the endpoint's native rate/channel count is - which, by default, already matches
    /// TargetFormat() (48000Hz/stereo, see Config.AudioSampleRate/AudioChannels). Routing that
    /// common case through MediaFoundationResampler anyway meant paying its transform/buffering
    /// overhead on every single buffer for a "conversion" that's really just a bit-depth cast - one
    /// more thing competing for CPU time exactly when it's already scarce (heavy game/GPU load).
    /// FloatToInt16Provider below does only that cast, directly, with no resampling machinery at
    /// all. Only falls back to the real resampler when the source doesn't already match - e.g. a
    /// device whose native rate isn't 48000Hz, or AudioSampleRate/AudioChannels changed away from
    /// the client-matching defaults.</summary>
    private static IWaveProvider CreatePcmSource(IWaveProvider source, WaveFormat sourceFormat, WaveFormat targetFormat)
    {
        var sourceIsPlainFloatMatchingRateAndChannels =
            sourceFormat.Encoding == WaveFormatEncoding.IeeeFloat &&
            sourceFormat.BitsPerSample == 32 &&
            sourceFormat.SampleRate == targetFormat.SampleRate &&
            sourceFormat.Channels == targetFormat.Channels;

        if (sourceIsPlainFloatMatchingRateAndChannels)
        {
            return new FloatToInt16Provider(source, targetFormat);
        }

        return new MediaFoundationResampler(source, targetFormat) { ResamplerQuality = 30 };
    }

    /// <summary>Casts 32-bit float samples straight to 16-bit PCM, no sample-rate conversion - see
    /// CreatePcmSource for when this applies instead of the full resampler.</summary>
    private sealed class FloatToInt16Provider : IWaveProvider
    {
        private readonly IWaveProvider _source;
        private byte[] _floatBuffer = [];

        public FloatToInt16Provider(IWaveProvider source, WaveFormat outputFormat)
        {
            _source = source;
            WaveFormat = outputFormat;
        }

        public WaveFormat WaveFormat { get; }

        public int Read(byte[] buffer, int offset, int count)
        {
            var floatBytesNeeded = count * 2; // 4 bytes/float sample -> 2 bytes/int16 sample
            if (_floatBuffer.Length < floatBytesNeeded) _floatBuffer = new byte[floatBytesNeeded];

            var floatBytesRead = _source.Read(_floatBuffer, 0, floatBytesNeeded);
            var sampleCount = floatBytesRead / 4;
            for (var i = 0; i < sampleCount; i++)
            {
                var sample = Math.Clamp(BitConverter.ToSingle(_floatBuffer, i * 4), -1f, 1f);
                var pcm = (short)(sample * short.MaxValue);
                buffer[offset + i * 2] = (byte)(pcm & 0xFF);
                buffer[offset + i * 2 + 1] = (byte)((pcm >> 8) & 0xFF);
            }
            return sampleCount * 2;
        }
    }

    // Runs on NAudio's own WASAPI capture thread - registering MMCSS here (once) is what actually
    // affects that thread, since MMCSS characteristics are per-thread and can't be set from outside.
    private void OnDataAvailable(object? sender, WaveInEventArgs e)
    {
        if (!_captureMmcssApplied)
        {
            _captureMmcssHandle = Mmcss.Register(MmcssTaskName);
            _captureMmcssApplied = true;
        }
        _buffer?.AddSamples(e.Buffer, 0, e.BytesRecorded);
    }

    private void SenderLoop()
    {
        var senderMmcssHandle = Mmcss.Register(MmcssTaskName);
        try
        {
            var samplesPerChannel = _config.AudioSampleRate * FrameDurationMs / 1000;
            var frame = new byte[samplesPerChannel * _config.AudioChannels * 2]; // 2 = bytes/s16 sample
            // A 20ms Opus frame is nowhere near this large even at a high bitrate - generous
            // headroom rather than a tightly-computed worst case.
            var opusBuffer = _opusEncoder is null ? null : new byte[4000];

            _bytesSinceLastLog = 0;
            _packetsSinceLastLog = 0;
            _lastThroughputLogUtc = DateTime.UtcNow;

            while (!_stopping)
            {
                var filled = 0;
                while (filled < frame.Length && !_stopping)
                {
                    var read = _pcmSource!.Read(frame, filled, frame.Length - filled);
                    if (read <= 0)
                    {
                        Thread.Sleep(2); // no data resampled yet - avoid busy-spinning a core
                        continue;
                    }
                    filled += read;
                }
                if (_stopping) break;

                ApplyGain(frame, filled, _config.AudioGain);

                if (_debugWriter is not null)
                {
                    _debugWriter.Write(frame, 0, filled);
                    continue;
                }

                var pcmShorts = MemoryMarshal.Cast<byte, short>(frame.AsSpan(0, filled));
                var encodedLength = _opusEncoder!.Encode(pcmShorts, samplesPerChannel, opusBuffer.AsSpan()!, opusBuffer!.Length);
                var packet = _rtpPacketizer!.Wrap(opusBuffer.AsSpan(0, encodedLength), samplesPerChannel);

                try
                {
                    _udpClient!.Send(packet, packet.Length);
                    _bytesSinceLastLog += packet.Length;
                    _packetsSinceLastLog++;
                }
                catch (Exception ex)
                {
                    // UDP send failures are a local/routing problem, not a "connection" dropping -
                    // unlike the old TCP path, there's nothing to reconnect. Log and keep going; the
                    // client's own RTP jitter buffer/Opus PLC is what's meant to absorb any gap this
                    // causes on the wire, not a host-side retry.
                    Logger.Log("AudioCaptureSender", $"UDP send failed: {ex.Message}");
                }

                var now = DateTime.UtcNow;
                var sinceLastLog = now - _lastThroughputLogUtc;
                if (sinceLastLog >= ThroughputLogInterval)
                {
                    var kbps = _bytesSinceLastLog * 8.0 / 1000.0 / sinceLastLog.TotalSeconds;
                    Logger.Log("AudioCaptureSender", $"throughput: {_bytesSinceLastLog} bytes / {_packetsSinceLastLog} packets over {sinceLastLog.TotalSeconds:F1}s ({kbps:F1} kbit/s)");
                    _bytesSinceLastLog = 0;
                    _packetsSinceLastLog = 0;
                    _lastThroughputLogUtc = now;
                }
            }
        }
        catch (Exception ex)
        {
            if (!_stopping) Logger.Log("AudioCaptureSender", $"sender loop stopped unexpectedly: {ex}");
        }
        finally
        {
            Mmcss.Revert(senderMmcssHandle);
        }
    }

    /// <summary>Linear gain with a soft ceiling at 90% of full scale (mirrors the old ffmpeg chain's
    /// "volume=X,alimiter=limit=0.9" - turn the whole signal down/up, then hard-clip only the very
    /// tips of whatever peaks that leaves) - operates in place on interleaved 16-bit PCM.</summary>
    private static void ApplyGain(byte[] buffer, int count, double gain)
    {
        if (gain == 1.0) return;
        const int ceiling = (int)(short.MaxValue * 0.9);
        for (var i = 0; i + 1 < count; i += 2)
        {
            var sample = (short)(buffer[i] | (buffer[i + 1] << 8));
            var scaled = (int)(sample * gain);
            scaled = Math.Clamp(scaled, -ceiling, ceiling);
            buffer[i] = (byte)(scaled & 0xFF);
            buffer[i + 1] = (byte)((scaled >> 8) & 0xFF);
        }
    }

    public void Stop()
    {
        _stopping = true;
        try
        {
            _capture?.StopRecording();
            // StopRecording() only requests the stop - it doesn't block until NAudio's internal
            // capture thread has actually unwound and let go of its WASAPI COM interfaces.
            // Confirmed live (repeated restarts across a session behaving progressively worse):
            // Disposing _capture right after StopRecording() raced that internal thread, risking a
            // torn-down-while-still-in-use IAudioClient/IAudioCaptureClient. RecordingStopped only
            // fires once that thread is genuinely done, so waiting for it here (capped, in case it
            // never fires for some reason) makes teardown actually deterministic before Dispose().
            if (_capture is not null) _captureStoppedEvent.Wait(1000);
        }
        catch { /* best-effort teardown */ }

        _senderThread?.Join(500);
        Mmcss.Revert(_captureMmcssHandle);
        _captureMmcssHandle = IntPtr.Zero;
        _captureMmcssApplied = false;

        try { (_pcmSource as IDisposable)?.Dispose(); } catch { /* best-effort teardown */ }
        try { _capture?.Dispose(); } catch { /* best-effort teardown */ }
        try { _opusEncoder?.Dispose(); } catch { /* best-effort teardown */ }
        try { _udpClient?.Dispose(); } catch { /* best-effort teardown */ }
        try { _debugWriter?.Dispose(); } catch { /* best-effort teardown */ }

        _pcmSource = null;
        _capture = null;
        _buffer = null;
        _opusEncoder = null;
        _udpClient = null;
        _debugWriter = null;
        _rtpPacketizer = null;
        _senderThread = null;
    }

    public void Dispose() => Stop();
}
