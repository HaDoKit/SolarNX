using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Serialization;
using SolarNX.Input;
using SolarNX.Library;

namespace SolarNX.Session;

[JsonConverter(typeof(JsonStringEnumConverter))]
public enum ConnectionMode
{
    /// <summary>Local network: the PC dials the Switch (TCP control + library, UDP video/audio).</summary>
    Wifi,
    /// <summary>Experimental: the Switch dials the PC on one UDP port (see <see cref="InternetSession"/>).</summary>
    Internet,
}

/// <summary>
/// The experimental "Internet" mode: one UDP socket on <see cref="Port"/> carries the whole session
/// (InternetWire), and the Switch sends first - so the PC only needs that one port forwarded on its
/// router, and the reply traffic gets back through the Switch's own router because it goes to the
/// address the Switch's packets came from. No NAT hole punching yet.
///
/// Video and audio are produced exactly as in Wi-Fi mode, just aimed at two loopback relay ports
/// (<see cref="VideoRelayPort"/>, <see cref="AudioRelayPort"/>) - this class picks every datagram up
/// there and forwards it to the Switch, so ffmpeg and AudioCaptureSender need no other change.
///
/// A Switch is accepted only with the right connection code (<see cref="Config.ConnectionCode"/>):
/// the port is open to the whole internet, and without it anyone who found it would get a gamepad on
/// this PC. The traffic itself is not encrypted yet.
/// </summary>
public sealed class InternetSession : ISessionController
{
    public const int Port = 19010;
    public const int VideoRelayPort = 19011;
    public const int AudioRelayPort = 19012;

    private const string CodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0/O, 1/I
    private static readonly TimeSpan InputIdleTimeout = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan PeerIdleTimeout = TimeSpan.FromSeconds(10);
    private const int LibraryChunkSize = 1100;
    private const int LibraryWindowChunks = 48;

    private readonly Config _config;
    private readonly object _lock = new();
    private CancellationTokenSource? _runCts;
    private UdpClient? _socket;
    private UdpClient? _videoRelay;
    private UdpClient? _audioRelay;

    private IPEndPoint? _peer;
    private DateTime _lastFromPeerUtc;
    private DateTime _lastInputUtc;
    private bool _sessionActive;

    private uint _offerId;
    private TaskCompletionSource<LibraryWire.Reply>? _pendingOffer;
    private uint _pendingOfferId;
    private ulong _ackHash;
    private long _ackedBytes;
    private readonly SemaphoreSlim _ackSignal = new(0);

    public event Action<HidPacket>? SessionStarted;
    public event Action? SessionStopped;
    public event Action<HidPacket>? PacketReceived;

    public InternetSession(Config config)
    {
        _config = config;
    }

    /// <summary>A fresh 8-character connection code, without look-alike characters.</summary>
    public static string NewCode() =>
        string.Concat(Enumerable.Range(0, 8).Select(_ => CodeAlphabet[RandomNumberGenerator.GetInt32(CodeAlphabet.Length)]));

    /// <summary>Where the Switch currently talks from, or null if no Switch is connected.</summary>
    public IPEndPoint? Peer
    {
        get { lock (_lock) return _peer; }
    }

    public void Start()
    {
        Stop();
        if (string.IsNullOrWhiteSpace(_config.ConnectionCode))
        {
            _config.ConnectionCode = NewCode();
            _config.Save();
        }

        try
        {
            _socket = new UdpClient(new IPEndPoint(IPAddress.Any, Port));
            _videoRelay = new UdpClient(new IPEndPoint(IPAddress.Loopback, VideoRelayPort));
            _audioRelay = new UdpClient(new IPEndPoint(IPAddress.Loopback, AudioRelayPort));
            // A datagram the Switch couldn't take (ICMP port unreachable) must not kill the receive
            // loop: on Windows it surfaces as a ConnectionReset on the next ReceiveAsync.
            const int SioUdpConnReset = -1744830452;
            _socket.Client.IOControl(SioUdpConnReset, new byte[] { 0 }, null);
        }
        catch (SocketException ex)
        {
            Logger.Log("InternetSession", $"cannot open UDP port {Port} (or relays {VideoRelayPort}/{AudioRelayPort}): {ex.Message}");
            DisposeSockets();
            return;
        }

        _runCts = new CancellationTokenSource();
        var ct = _runCts.Token;
        _ = Task.Run(() => ReceiveLoopAsync(_socket, ct));
        _ = Task.Run(() => RelayLoopAsync(_videoRelay, InternetWire.Type.Video, ct));
        _ = Task.Run(() => RelayLoopAsync(_audioRelay, InternetWire.Type.Audio, ct));
        _ = Task.Run(() => WatchdogLoopAsync(ct));
        Logger.Log("InternetSession", $"listening on UDP {Port} (Internet mode, experimental)");
    }

    public void Stop()
    {
        _runCts?.Cancel();
        _runCts?.Dispose();
        _runCts = null;
        EndSession("stopped", notifySwitch: true);
        lock (_lock) _peer = null;
        DisposeSockets();
    }

    public void ForceDisconnect() => EndSession("ended by the PC", notifySwitch: true);

    public ValueTask DisposeAsync()
    {
        Stop();
        return ValueTask.CompletedTask;
    }

    private void DisposeSockets()
    {
        _socket?.Dispose();
        _videoRelay?.Dispose();
        _audioRelay?.Dispose();
        _socket = _videoRelay = _audioRelay = null;
    }

    private async Task ReceiveLoopAsync(UdpClient socket, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult result;
            try
            {
                result = await socket.ReceiveAsync(ct);
            }
            catch (OperationCanceledException) { return; }
            catch (ObjectDisposedException) { return; }
            catch (SocketException ex)
            {
                Logger.Log("InternetSession", $"receive failed: {ex.SocketErrorCode}");
                continue;
            }

            try
            {
                Handle(socket, result.Buffer, result.RemoteEndPoint);
            }
            catch (Exception ex)
            {
                Logger.Log("InternetSession", $"bad datagram from {result.RemoteEndPoint}: {ex.Message}");
            }
        }
    }

    private void Handle(UdpClient socket, byte[] data, IPEndPoint from)
    {
        var type = InternetWire.Parse(data, out var version);
        if (type is null) return;
        var payload = data.AsSpan(InternetWire.HeaderSize);

        if (type == InternetWire.Type.Hello)
        {
            HandleHello(socket, payload, from, version);
            return;
        }

        HidPacket? started = null;
        HidPacket? input = null;
        lock (_lock)
        {
            if (_peer is null || !_peer.Equals(from)) return; // only the accepted Switch
            _lastFromPeerUtc = DateTime.UtcNow;

            switch (type)
            {
                case InternetWire.Type.Input when payload.Length >= HidPacket.PayloadSize:
                    input = HidPacket.Parse(payload[..HidPacket.PayloadSize]);
                    _lastInputUtc = DateTime.UtcNow;
                    if (!_sessionActive)
                    {
                        _sessionActive = true;
                        started = input;
                    }
                    break;
                case InternetWire.Type.LibReply when payload.Length >= 12:
                    var offerId = BinaryPrimitives.ReadUInt32LittleEndian(payload);
                    if (_pendingOffer is not null && offerId == _pendingOfferId)
                        _pendingOffer.TrySetResult(new LibraryWire.Reply(payload[4] == 1, payload[5] != 0, BinaryPrimitives.ReadUInt32LittleEndian(payload[8..])));
                    break;
                case InternetWire.Type.LibAck when payload.Length >= 12:
                    var hash = BinaryPrimitives.ReadUInt64LittleEndian(payload);
                    var received = BinaryPrimitives.ReadUInt32LittleEndian(payload[8..]);
                    if (hash == _ackHash && received > _ackedBytes)
                    {
                        _ackedBytes = received;
                        _ackSignal.Release();
                    }
                    break;
            }
        }

        if (type == InternetWire.Type.Bye)
        {
            EndSession("the Switch went back to its menu", notifySwitch: false);
            return;
        }
        if (started is not null)
        {
            Logger.Log("InternetSession", $"first input from {from} - session started");
            SessionStarted?.Invoke(started);
        }
        if (input is not null) PacketReceived?.Invoke(input);
    }

    private void HandleHello(UdpClient socket, ReadOnlySpan<byte> payload, IPEndPoint from, byte version)
    {
        if (payload.Length < 4 + InternetWire.CodeLength) return;
        var nonce = BinaryPrimitives.ReadUInt32LittleEndian(payload);

        InternetWire.RejectReason? reject = null;
        bool newPeer = false;
        if (version != InternetWire.Version)
        {
            reject = InternetWire.RejectReason.WrongVersion;
        }
        else if (!CodeMatches(payload.Slice(4, InternetWire.CodeLength)))
        {
            reject = InternetWire.RejectReason.WrongCode;
        }
        else
        {
            lock (_lock)
            {
                var peerGone = _peer is null || DateTime.UtcNow - _lastFromPeerUtc > PeerIdleTimeout;
                if (_peer is not null && !_peer.Equals(from) && !peerGone && _sessionActive)
                {
                    reject = InternetWire.RejectReason.Busy;
                }
                else
                {
                    newPeer = _peer is null || !_peer.Equals(from);
                    _peer = from;
                    _lastFromPeerUtc = DateTime.UtcNow;
                }
            }
        }

        if (reject is not null)
        {
            var packet = InternetWire.Packet(InternetWire.Type.Reject, 5);
            BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(InternetWire.HeaderSize), nonce);
            packet[InternetWire.HeaderSize + 4] = (byte)reject.Value;
            Send(socket, packet, from);
            Logger.Log("InternetSession", $"rejected {from}: {reject}");
            return;
        }

        if (newPeer) Logger.Log("InternetSession", $"Switch connected from {from}");
        var config = SessionController.BuildSessionConfig(_config).ToBytes();
        var welcome = InternetWire.Packet(InternetWire.Type.Welcome, 4 + config.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(welcome.AsSpan(InternetWire.HeaderSize), nonce);
        config.CopyTo(welcome.AsSpan(InternetWire.HeaderSize + 4));
        Send(socket, welcome, from);
    }

    // Constant-time, case-insensitive; the code on the wire is ASCII, zero-padded to 16 bytes.
    private bool CodeMatches(ReadOnlySpan<byte> received)
    {
        var expected = new byte[InternetWire.CodeLength];
        Encoding.ASCII.GetBytes(_config.ConnectionCode.Trim().ToUpperInvariant().AsSpan(0, Math.Min(_config.ConnectionCode.Trim().Length, InternetWire.CodeLength)), expected);
        var actual = received.ToArray();
        for (var i = 0; i < actual.Length; i++)
        {
            if (actual[i] is >= (byte)'a' and <= (byte)'z') actual[i] -= 32;
        }
        return expected.Any(b => b != 0) && CryptographicOperations.FixedTimeEquals(expected, actual);
    }

    private async Task RelayLoopAsync(UdpClient relay, InternetWire.Type type, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            UdpReceiveResult result;
            try
            {
                result = await relay.ReceiveAsync(ct);
            }
            catch (OperationCanceledException) { return; }
            catch (ObjectDisposedException) { return; }
            catch (SocketException) { continue; }

            var peer = Peer;
            var socket = _socket;
            if (peer is null || socket is null) continue;
            var packet = InternetWire.Packet(type, result.Buffer.Length);
            result.Buffer.CopyTo(packet.AsSpan(InternetWire.HeaderSize));
            Send(socket, packet, peer);
        }
    }

    private async Task WatchdogLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try { await Task.Delay(500, ct); } catch (OperationCanceledException) { return; }
            bool inputStalled, peerGone;
            lock (_lock)
            {
                inputStalled = _sessionActive && DateTime.UtcNow - _lastInputUtc > InputIdleTimeout;
                peerGone = _peer is not null && DateTime.UtcNow - _lastFromPeerUtc > PeerIdleTimeout;
            }
            if (inputStalled) EndSession($"no input for {InputIdleTimeout.TotalSeconds:0}s - treating the Switch as gone", notifySwitch: false);
            if (peerGone)
            {
                lock (_lock) _peer = null;
                Logger.Log("InternetSession", "Switch silent - forgetting it until it says hello again");
            }
        }
    }

    private void EndSession(string reason, bool notifySwitch)
    {
        IPEndPoint? peer;
        lock (_lock)
        {
            if (!_sessionActive) return;
            _sessionActive = false;
            peer = _peer;
        }
        if (notifySwitch && peer is not null && _socket is { } socket)
        {
            var end = InternetWire.Packet(InternetWire.Type.End, 0);
            for (var i = 0; i < 3; i++) Send(socket, end, peer); // a lost End would leave the Switch waiting
        }
        Logger.Log("InternetSession", $"session stopped: {reason}");
        SessionStopped?.Invoke();
    }

    private static void Send(UdpClient socket, byte[] packet, IPEndPoint to)
    {
        try { socket.Send(packet, packet.Length, to); }
        catch (Exception ex) when (ex is SocketException or ObjectDisposedException) { /* lost like any datagram */ }
    }

    // --- Library (used by LibraryPusher in Internet mode) ---

    /// <summary>Offers the library header to the Switch; null if there's no Switch or it didn't answer.</summary>
    internal async Task<LibraryWire.Reply?> OfferLibraryAsync(ulong hash, bool swapAbxy, uint layoutRevision, CancellationToken ct)
    {
        var peer = Peer;
        if (peer is null || _socket is not { } socket) return null;

        var tcs = new TaskCompletionSource<LibraryWire.Reply>(TaskCreationOptions.RunContinuationsAsynchronously);
        var packet = InternetWire.Packet(InternetWire.Type.LibOffer, 20);
        lock (_lock)
        {
            _pendingOfferId = ++_offerId;
            _pendingOffer = tcs;
            BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(InternetWire.HeaderSize), _pendingOfferId);
        }
        BinaryPrimitives.WriteUInt64LittleEndian(packet.AsSpan(InternetWire.HeaderSize + 4), hash);
        packet[InternetWire.HeaderSize + 12] = swapAbxy ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(InternetWire.HeaderSize + 16), layoutRevision);

        for (var attempt = 0; attempt < 3 && !tcs.Task.IsCompleted; attempt++)
        {
            Send(socket, packet, peer);
            try { await tcs.Task.WaitAsync(TimeSpan.FromSeconds(1), ct); }
            catch (TimeoutException) { }
        }
        return tcs.Task.IsCompletedSuccessfully ? tcs.Task.Result : null;
    }

    /// <summary>Sends the library payload in chunks, go-back-N, until the Switch has acked all of it.</summary>
    internal async Task SendLibraryAsync(ulong hash, byte[] payload, CancellationToken ct)
    {
        lock (_lock)
        {
            _ackHash = hash;
            _ackedBytes = 0;
        }
        while (_ackSignal.CurrentCount > 0) await _ackSignal.WaitAsync(ct);

        long next = 0;
        long acked = 0;
        var stalls = 0;
        while (acked < payload.Length)
        {
            var peer = Peer;
            if (peer is null || _socket is not { } socket) throw new IOException("the Switch went away during the library transfer");

            while (next < payload.Length && next < acked + (long)LibraryWindowChunks * LibraryChunkSize)
            {
                var length = (int)Math.Min(LibraryChunkSize, payload.Length - next);
                var chunk = InternetWire.Packet(InternetWire.Type.LibChunk, 16 + length);
                BinaryPrimitives.WriteUInt64LittleEndian(chunk.AsSpan(InternetWire.HeaderSize), hash);
                BinaryPrimitives.WriteUInt32LittleEndian(chunk.AsSpan(InternetWire.HeaderSize + 8), (uint)payload.Length);
                BinaryPrimitives.WriteUInt32LittleEndian(chunk.AsSpan(InternetWire.HeaderSize + 12), (uint)next);
                payload.AsSpan((int)next, length).CopyTo(chunk.AsSpan(InternetWire.HeaderSize + 16));
                Send(socket, chunk, peer);
                next += length;
            }

            await _ackSignal.WaitAsync(TimeSpan.FromMilliseconds(250), ct);
            long now;
            lock (_lock) now = _ackedBytes;
            if (now > acked)
            {
                acked = now;
                stalls = 0;
            }
            else
            {
                next = acked; // go back: resend everything not yet acknowledged
                if (++stalls > 40) throw new IOException("library transfer stalled - no progress for 10s");
            }
        }
    }
}
