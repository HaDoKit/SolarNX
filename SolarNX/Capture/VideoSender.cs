using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;

namespace SolarNX.Capture;

/// <summary>
/// Sends the encoded video to the Switch in SolarNX's own framing, with Reed-Solomon forward error
/// correction: a lost datagram is rebuilt on the Switch from the parity that came with its frame,
/// instead of corrupting the picture until the next keyframe (what a lost MPEG-TS datagram did).
///
/// ffmpeg sends RTP (H.264, RFC 6184, or H.265, RFC 7798) to <see cref="RtpPort"/> on loopback - RTP only because its
/// marker bit says where each frame ends the moment the frame's last packet arrives (a raw H.264
/// pipe would only show that when the next frame starts, a whole frame later). Each frame is
/// rebuilt as Annex B, cut into <see cref="ShardSize"/>-byte data shards, split into blocks of at
/// most <see cref="MaxDataShardsPerBlock"/>, and every block gets ~20% parity shards (at least
/// <see cref="MinParityShards"/>).
///
/// The blocks' datagrams go out interleaved (shard 0 of every block, then shard 1, ...), so a
/// burst of consecutive losses in a big frame - a keyframe is several blocks - is spread across
/// its blocks instead of overwhelming one. A frame of more than <see cref="PaceAbovePackets"/>
/// datagrams - in practice a keyframe - is also paced over a few milliseconds, in groups of
/// <see cref="PaceGroupPackets"/>: a whole keyframe sent back to back is exactly what overflows a
/// router's or access point's queue and loses a burst in the first place. Ordinary frames go out
/// at once - pacing them would add its delay to every frame.
///
/// Wire format - every datagram is <see cref="HeaderSize"/> + <see cref="ShardSize"/> bytes,
/// little-endian, mirrored by the Switch's Client/video/videowire.h:
///   0  u8  magic 0x53 ('S')         8  u32 frame number (starts random, +1 per frame)
///   1  u8  version (2)             12  u32 frame size in bytes (Annex B, before padding)
///   2  u8  flags (bit 0: keyframe, bit 1: H.265 - else H.264)
///   3  u8  block index
///   4  u8  block count
///   5  u8  shard index in the block (data shards first, then parity)
///   6  u8  parity shards in this block
///   7  u8  reserved (0)
/// The frame's N = ceil(size / ShardSize) data shards (the last one zero-padded) are split into
/// blockCount blocks as evenly as possible: block b gets N / blockCount shards, plus one more when
/// b &lt; N % blockCount - the Switch derives every block's data shard count from that rule.
///
/// Both connection modes: Wi-Fi sends straight to the Switch; Internet mode sends to
/// InternetSession's loopback video relay, which seals each datagram and forwards it.
/// </summary>
public sealed class VideoSender : IDisposable
{
    public const int HeaderSize = 16;
    // 16 + 1280 = 1296 bytes, under the 1316 the Internet relay and a 1500-byte MTU have room for
    // after their own headers - an IP-fragmented datagram is lost when any fragment is.
    public const int ShardSize = 1280;
    public const byte Magic = 0x53;
    // 2 since the codec flag (0.2.4): a 0.2.3 Switch would decode H.265 as H.264 - this way it
    // rejects the stream and logs why instead.
    public const byte Version = 2;
    public const byte FlagKeyframe = 0x01;
    public const byte FlagHevc = 0x02;

    private const int MaxDataShardsPerBlock = 64;
    private const double ParityRatio = 0.20;
    // A small frame of 1-5 shards would get 0-1 parity shards from the ratio alone - not enough
    // to survive the occasional pair of losses.
    private const int MinParityShards = 2;
    // ~60KB: an ordinary frame up to ~30Mbps at 60fps. Above that it's a keyframe (or a scene cut).
    private const int PaceAbovePackets = 48;
    private const int PaceGroupPackets = 16;
    private const byte RtpPayloadType = 96; // ffmpeg's rtp muxer default; anything else is its RTCP

    private readonly bool _hevc;
    private readonly Socket _rtpSocket;
    private readonly Socket _sendSocket;
    private readonly IDisposable? _qosFlow;
    private readonly TimeSpan _paceWindow;
    private readonly Thread _thread;
    private volatile bool _stopping;

    // Frame being rebuilt from RTP (Annex B).
    private byte[] _frame = new byte[512 * 1024];
    private int _frameLength;
    private bool _frameKeyframe;
    private bool _frameDamaged;
    private int _nextRtpSequence = -1;

    private uint _frameNumber = (uint)Random.Shared.Next();
    private byte[] _parity = new byte[ReedSolomon.MaxParity * ShardSize];
    private readonly byte[] _datagram = new byte[HeaderSize + ShardSize];

    private static readonly TimeSpan StatsInterval = TimeSpan.FromSeconds(5);
    private DateTime _lastStatsUtc = DateTime.UtcNow;
    private long _statFrames, _statKeyframes, _statBytes, _statDatagrams, _statParity, _statDamaged;

    /// <param name="target">The Switch's video port (Wi-Fi) or the Internet relay on loopback.</param>
    /// <param name="fps">Stream frame rate - the pacing window is a quarter of a frame.</param>
    /// <param name="codec">What ffmpeg encodes - decides the RTP payload format.</param>
    public VideoSender(IPEndPoint target, int fps, VideoCodec codec)
    {
        _hevc = codec == VideoCodec.Hevc;
        _paceWindow = TimeSpan.FromMilliseconds(Math.Min(4.0, 1000.0 / Math.Max(1, fps) / 4));

        _rtpSocket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        // A keyframe arrives from ffmpeg in one burst while the previous frame may still be pacing out.
        _rtpSocket.ReceiveBufferSize = 4 * 1024 * 1024;
        _rtpSocket.ReceiveTimeout = 200; // lets the thread notice Dispose
        _rtpSocket.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        RtpPort = ((IPEndPoint)_rtpSocket.LocalEndPoint!).Port; // bound just above, so never null

        _sendSocket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        _sendSocket.SendBufferSize = 1024 * 1024;
        _sendSocket.Connect(target); // UDP: only fixes the destination, nothing is sent
        _qosFlow = Native.QosMarking.Mark(_sendSocket, Native.QosMarking.DscpVideo, "video");

        _thread = new Thread(Run) { IsBackground = true, Name = "VideoSender", Priority = ThreadPriority.Highest };
        _thread.Start();
        Logger.Log("VideoSender", $"{EncoderSelection.DisplayName(codec)} RTP in on 127.0.0.1:{RtpPort}, frames out to {target}, pacing over {_paceWindow.TotalMilliseconds:0.#}ms");
    }

    /// <summary>The loopback port ffmpeg's RTP output goes to.</summary>
    public int RtpPort { get; }

    /// <summary>Resolves the Switch address from the config (normally a plain IPv4 address).</summary>
    public static IPEndPoint ResolveTarget(string host, int port)
    {
        if (IPAddress.TryParse(host, out var address)) return new IPEndPoint(address, port);
        var resolved = Dns.GetHostAddresses(host).FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork)
            ?? throw new SocketException((int)SocketError.HostNotFound);
        return new IPEndPoint(resolved, port);
    }

    private void Run()
    {
        var packet = new byte[65536];
        while (!_stopping)
        {
            int length;
            try
            {
                length = _rtpSocket.Receive(packet);
            }
            catch (SocketException ex) when (ex.SocketErrorCode is SocketError.TimedOut or SocketError.ConnectionReset)
            {
                continue;
            }
            catch (Exception) when (_stopping)
            {
                break;
            }
            catch (Exception ex)
            {
                Logger.Log("VideoSender", $"RTP receive failed, stopping: {ex.Message}");
                break;
            }

            try
            {
                OnRtpPacket(packet.AsSpan(0, length));
            }
            catch (Exception ex) when (!_stopping)
            {
                // One bad frame must not end the stream.
                Logger.Log("VideoSender", $"frame dropped: {ex.Message}");
                ResetFrame();
            }
        }
    }

    private void OnRtpPacket(ReadOnlySpan<byte> rtp)
    {
        if (rtp.Length < 12 || rtp[0] >> 6 != 2) return;
        if ((rtp[1] & 0x7F) != RtpPayloadType) return; // RTCP sender reports share the port

        var sequence = BinaryPrimitives.ReadUInt16BigEndian(rtp[2..]);
        if (_nextRtpSequence >= 0 && sequence != _nextRtpSequence) _frameDamaged = true;
        _nextRtpSequence = (sequence + 1) & 0xFFFF;

        var offset = 12 + 4 * (rtp[0] & 0x0F);
        if ((rtp[0] & 0x10) != 0)
        {
            if (rtp.Length < offset + 4) return;
            offset += 4 + 4 * BinaryPrimitives.ReadUInt16BigEndian(rtp[(offset + 2)..]);
        }
        var end = rtp.Length;
        if ((rtp[0] & 0x20) != 0) end -= rtp[^1];
        if (offset < end)
        {
            if (_hevc) AppendHevcPayload(rtp[offset..end]);
            else AppendPayload(rtp[offset..end]);
        }

        if ((rtp[1] & 0x80) != 0) FinishFrame();
    }

    // RFC 6184 payloads -> Annex B NAL units.
    private void AppendPayload(ReadOnlySpan<byte> payload)
    {
        var type = payload[0] & 0x1F;
        switch (type)
        {
            case >= 1 and <= 23:
                AppendNal(payload, startCode: true);
                break;
            case 24: // STAP-A: [u16 size][NAL] ...
                for (var i = 1; i + 2 <= payload.Length;)
                {
                    var size = BinaryPrimitives.ReadUInt16BigEndian(payload[i..]);
                    i += 2;
                    if (size == 0 || i + size > payload.Length) { _frameDamaged = true; break; }
                    AppendNal(payload.Slice(i, size), startCode: true);
                    i += size;
                }
                break;
            case 28: // FU-A: indicator, header (S/E bits + the NAL's type), fragment
                if (payload.Length < 2) { _frameDamaged = true; break; }
                var header = payload[1];
                if ((header & 0x80) != 0)
                {
                    Span<byte> nalHeader = [(byte)((payload[0] & 0xE0) | (header & 0x1F))];
                    AppendNal(nalHeader, startCode: true);
                }
                AppendNal(payload[2..], startCode: false);
                break;
            default:
                _frameDamaged = true; // STAP-B/MTAP/FU-B: ffmpeg's rtp muxer never sends these
                break;
        }
    }

    // RFC 7798 payloads -> Annex B NAL units. The NAL header is 2 bytes, its type is bits 1-6 of the
    // first. No DONL fields: ffmpeg's muxer sends in decoding order (sprop-max-don-diff 0).
    private void AppendHevcPayload(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 2) { _frameDamaged = true; return; }
        var type = (payload[0] >> 1) & 0x3F;
        switch (type)
        {
            case < 48:
                AppendNal(payload, startCode: true);
                break;
            case 48: // aggregation packet: payload header, then [u16 size][NAL] ...
                for (var i = 2; i + 2 <= payload.Length;)
                {
                    var size = BinaryPrimitives.ReadUInt16BigEndian(payload[i..]);
                    i += 2;
                    if (size < 2 || i + size > payload.Length) { _frameDamaged = true; break; }
                    AppendNal(payload.Slice(i, size), startCode: true);
                    i += size;
                }
                break;
            case 49: // fragmentation unit: payload header, FU header (S/E bits + the NAL's type), fragment
                if (payload.Length < 3) { _frameDamaged = true; break; }
                var header = payload[2];
                if ((header & 0x80) != 0)
                {
                    Span<byte> nalHeader = [(byte)((payload[0] & 0x81) | ((header & 0x3F) << 1)), payload[1]];
                    AppendNal(nalHeader, startCode: true);
                }
                AppendNal(payload[3..], startCode: false);
                break;
            default:
                _frameDamaged = true; // PACI: ffmpeg's rtp muxer never sends it
                break;
        }
    }

    private void AppendNal(ReadOnlySpan<byte> bytes, bool startCode)
    {
        var needed = _frameLength + bytes.Length + 4;
        if (needed > _frame.Length) Array.Resize(ref _frame, Math.Max(needed, _frame.Length * 2));
        if (startCode)
        {
            _frame.AsSpan(_frameLength, 4).Clear();
            _frame[_frameLength + 3] = 1;
            _frameLength += 4;
            // A frame the Switch can start decoding at: H.264 IDR slice; H.265 IRAP (BLA, IDR, CRA - 16..21).
            if (_hevc ? ((bytes[0] >> 1) & 0x3F) is >= 16 and <= 21 : (bytes[0] & 0x1F) == 5) _frameKeyframe = true;
        }
        bytes.CopyTo(_frame.AsSpan(_frameLength));
        _frameLength += bytes.Length;
    }

    private void ResetFrame()
    {
        _frameLength = 0;
        _frameKeyframe = false;
        _frameDamaged = false;
    }

    private void FinishFrame()
    {
        var number = _frameNumber++;
        if (_frameDamaged || _frameLength == 0)
        {
            // Lost on loopback (practically never). The frame number is still used up, so the
            // Switch sees the gap and waits for a keyframe instead of decoding on a broken reference.
            _statDamaged++;
            ResetFrame();
            return;
        }
        SendFrame(number);
        ResetFrame();
    }

    private void SendFrame(uint number)
    {
        var size = _frameLength;
        var dataShards = (size + ShardSize - 1) / ShardSize;
        var blockCount = (dataShards + MaxDataShardsPerBlock - 1) / MaxDataShardsPerBlock;
        if (blockCount > 255) throw new InvalidOperationException($"frame of {size} bytes is too big");

        var padded = dataShards * ShardSize;
        if (padded > _frame.Length) Array.Resize(ref _frame, padded);
        _frame.AsSpan(size, padded - size).Clear();

        Span<int> firstShard = stackalloc int[blockCount];
        Span<int> blockData = stackalloc int[blockCount];
        Span<int> blockParity = stackalloc int[blockCount];
        Span<int> parityOffset = stackalloc int[blockCount];
        var parityTotal = 0;
        var maxShardsInBlock = 0;
        for (int b = 0, first = 0; b < blockCount; b++)
        {
            blockData[b] = dataShards / blockCount + (b < dataShards % blockCount ? 1 : 0);
            blockParity[b] = Math.Max(MinParityShards, (int)Math.Ceiling(blockData[b] * ParityRatio));
            firstShard[b] = first;
            parityOffset[b] = parityTotal;
            first += blockData[b];
            parityTotal += blockParity[b];
            maxShardsInBlock = Math.Max(maxShardsInBlock, blockData[b] + blockParity[b]);
        }

        if (_parity.Length < parityTotal * ShardSize) _parity = new byte[parityTotal * ShardSize];
        for (var b = 0; b < blockCount; b++)
        {
            ReedSolomon.Encode(
                _frame.AsSpan(firstShard[b] * ShardSize, blockData[b] * ShardSize), blockData[b],
                _parity.AsSpan(parityOffset[b] * ShardSize, blockParity[b] * ShardSize), blockParity[b], ShardSize);
        }

        var header = _datagram.AsSpan(0, HeaderSize);
        header[0] = Magic;
        header[1] = Version;
        header[2] = (byte)((_frameKeyframe ? FlagKeyframe : 0) | (_hevc ? FlagHevc : 0));
        header[4] = (byte)blockCount;
        header[7] = 0;
        BinaryPrimitives.WriteUInt32LittleEndian(header[8..], number);
        BinaryPrimitives.WriteUInt32LittleEndian(header[12..], (uint)size);

        var total = dataShards + parityTotal;
        var paced = total > PaceAbovePackets;
        var start = Stopwatch.GetTimestamp();
        var sent = 0;
        for (var shard = 0; shard < maxShardsInBlock; shard++)
        {
            for (var b = 0; b < blockCount; b++)
            {
                if (shard >= blockData[b] + blockParity[b]) continue;
                var source = shard < blockData[b]
                    ? _frame.AsSpan((firstShard[b] + shard) * ShardSize, ShardSize)
                    : _parity.AsSpan((parityOffset[b] + shard - blockData[b]) * ShardSize, ShardSize);
                header[3] = (byte)b;
                header[5] = (byte)shard;
                header[6] = (byte)blockParity[b];
                source.CopyTo(_datagram.AsSpan(HeaderSize));

                if (paced && sent > 0 && sent % PaceGroupPackets == 0)
                    WaitUntil(start, _paceWindow * ((double)sent / total));
                Send();
                sent++;
            }
        }

        _statFrames++;
        if (_frameKeyframe) _statKeyframes++;
        _statBytes += size;
        _statDatagrams += total;
        _statParity += parityTotal;
        LogStatsIfDue();
    }

    private void Send()
    {
        try
        {
            _sendSocket.Send(_datagram);
        }
        catch (SocketException ex) when (ex.SocketErrorCode is SocketError.ConnectionReset or SocketError.NoBufferSpaceAvailable or SocketError.WouldBlock)
        {
            // ICMP "port unreachable" from a Switch that isn't listening yet, or a full send buffer:
            // UDP, the datagram is simply gone - the parity or the next keyframe covers it.
        }
    }

    // Sleeps while more than a scheduler tick is left (1ms with TimerResolution active during a
    // session), then yields the rest away.
    private static void WaitUntil(long start, TimeSpan offset)
    {
        var due = start + (long)(offset.TotalSeconds * Stopwatch.Frequency);
        while (true)
        {
            var left = due - Stopwatch.GetTimestamp();
            if (left <= 0) return;
            if (left > Stopwatch.Frequency / 667) Thread.Sleep(1); // > 1.5ms
            else Thread.Yield();
        }
    }

    /// <summary>Frames per second received from ffmpeg in each stats window (every 5s) - the
    /// adaptive NVENC preset's measure of whether the encoder keeps up. Raised on the sender thread.</summary>
    public event Action<double>? WindowFps;

    private void LogStatsIfDue()
    {
        var now = DateTime.UtcNow;
        if (now - _lastStatsUtc < StatsInterval) return;
        var seconds = (now - _lastStatsUtc).TotalSeconds;
        WindowFps?.Invoke(_statFrames / seconds);
        Logger.Log("VideoSender",
            $"{_statFrames / seconds:0.#} fps ({_statKeyframes} keyframes), {_statBytes * 8 / seconds / 1000:0} kbps video, " +
            $"{_statDatagrams} datagrams of which {_statParity} parity ({100.0 * _statParity / Math.Max(1, _statDatagrams - _statParity):0}%), " +
            $"{_statDamaged} frames lost on loopback");
        _lastStatsUtc = now;
        _statFrames = _statKeyframes = _statBytes = _statDatagrams = _statParity = _statDamaged = 0;
    }

    public void Dispose()
    {
        _stopping = true;
        _rtpSocket.Dispose();
        _thread.Join(1000);
        _qosFlow?.Dispose();
        _sendSocket.Dispose();
    }
}
