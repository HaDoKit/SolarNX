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
/// Only a paired Switch (<see cref="PairedDevices"/>) gets a session; everything after the Hello /
/// Welcome handshake is encrypted and authenticated with keys fresh for each session
/// (InternetCrypto). Pairing happens only while <see cref="OpenPairing"/> is in effect - the player
/// opens it from the settings and confirms the 6-digit code both screens show.
///
/// Video and audio are produced exactly as in Wi-Fi mode, just aimed at two loopback relay ports
/// (<see cref="VideoRelayPort"/>, <see cref="AudioRelayPort"/>) - this class picks every datagram up
/// there, seals it and forwards it to the Switch, so ffmpeg and AudioCaptureSender need no other change.
/// </summary>
public sealed class InternetSession : ISessionController
{
    public const int Port = 19010;
    public const int VideoRelayPort = 19011;
    public const int AudioRelayPort = 19012;

    private const string IdAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no 0/O, 1/I
    private static readonly TimeSpan InputIdleTimeout = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan PeerIdleTimeout = TimeSpan.FromSeconds(10);
    private const int LibraryChunkSize = 1100;
    private const int LibraryWindowChunks = 48;

    private readonly Config _config;
    private readonly SwitchCompatibility _compatibility;
    private readonly object _lock = new();
    private CancellationTokenSource? _runCts;
    private UdpClient? _socket;
    private UdpClient? _videoRelay;
    private UdpClient? _audioRelay;

    // The current peer and its session keys.
    private IPEndPoint? _peer;
    private byte[]? _peerDeviceId;
    private byte[]? _peerNonce;
    private byte[]? _welcome;
    private InternetCrypto.Channel? _toSwitch;
    private InternetCrypto.Channel? _fromSwitch;
    private InternetCrypto.ReplayWindow _replay = new();
    private long _sendCounter;
    private DateTime _lastFromPeerUtc;
    private DateTime _lastInputUtc;
    private bool _sessionActive;

    // What the current peer's Hello said about its version (PeerVersion.Legacy for a 0.2.4 Switch,
    // whose Hello is just device id + nonce).
    private PeerVersion? _peerVersion;

    // Pairing.
    private DateTime _pairingOpenUntil;
    private PairingAttempt? _pairing;

    // Rendezvous server (optional): where it is, what it says our public address is, and which
    // Switch endpoints reach us through its relay.
    private IPEndPoint? _server;
    private DateTime _serverResolvedUtc;
    private readonly HashSet<IPEndPoint> _relayed = new();

    // Library transfer.
    private uint _offerId;
    private TaskCompletionSource<LibraryWire.Reply>? _pendingOffer;
    private uint _pendingOfferId;
    private ulong _ackHash;
    private long _ackedBytes;
    private readonly SemaphoreSlim _ackSignal = new(0);

    private sealed class PairingAttempt
    {
        public required IPEndPoint From { get; init; }
        public required byte[] DeviceId { get; init; }
        public required string Name { get; init; }
        public required byte[] SwitchPublic { get; init; }
        public required byte[] PcPrivate { get; init; }
        public required byte[] PcPublic { get; init; }
        public required byte[] PcNonce { get; init; }
        public byte[]? SwitchNonce { get; set; }
        public byte[]? PairKey { get; set; }
        public int Code { get; set; }
        public byte[]? Result { get; set; } // the PairResult, once the player decided
    }

    public event Action<HidPacket>? SessionStarted;
    public event Action? SessionStopped;
    public event Action<HidPacket>? PacketReceived;
    public event Action<ControlWire.ClientMessage, byte[]>? ControlReceived;

    /// <summary>A Switch asks to pair: its name and the 6-digit code to compare with its screen.
    /// Answer with <see cref="CompletePairing"/>. Raised on the network thread.</summary>
    public event Action<string, int>? PairingRequested;

    public InternetSession(Config config, SwitchCompatibility compatibility)
    {
        _config = config;
        _compatibility = compatibility;
    }

    public PeerVersion? SessionPeer
    {
        get { lock (_lock) return _peerVersion is { IsLegacy: false } version && _toSwitch is not null ? version : null; }
    }

    public bool SendControl(ControlWire.HostMessage type, ReadOnlySpan<byte> payload)
    {
        IPEndPoint? peer;
        PeerVersion? version;
        lock (_lock)
        {
            peer = _peer;
            version = _peerVersion;
        }
        if (peer is null || version is null || !ProtocolVersion.Supports(version.Protocol, ProtocolVersion.ControlMessages) || _socket is not { } socket)
            return false;
        SendSealed(socket, InternetWire.Type.Control, ControlWire.DatagramPayload(type, payload), peer);
        return true;
    }

    /// <summary>This PC's public address as the rendezvous server sees it (null without a server or
    /// before it answered).</summary>
    public IPEndPoint? PublicEndpoint { get; private set; }

    /// <summary>A fresh 8-character id without look-alike characters.</summary>
    public static string NewId() =>
        string.Concat(Enumerable.Range(0, 8).Select(_ => IdAlphabet[RandomNumberGenerator.GetInt32(IdAlphabet.Length)]));

    /// <summary>Where the Switch currently talks from, or null if no Switch is connected.</summary>
    public IPEndPoint? Peer
    {
        get { lock (_lock) return _toSwitch is null ? null : _peer; }
    }

    public void Start()
    {
        Stop();
        if (string.IsNullOrWhiteSpace(_config.HostId))
        {
            _config.HostId = NewId();
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
        if (!string.IsNullOrWhiteSpace(_config.RendezvousServer))
            _ = Task.Run(() => RendezvousLoopAsync(ct));
        Logger.Log("InternetSession", $"listening on UDP {Port} (Internet mode, experimental)");
    }

    public void Stop()
    {
        _runCts?.Cancel();
        _runCts?.Dispose();
        _runCts = null;
        EndSession("stopped", notifySwitch: true);
        lock (_lock) ForgetPeer();
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

    // Caller holds _lock.
    private void ForgetPeer()
    {
        _peer = null;
        _peerVersion = null;
        _peerDeviceId = null;
        _peerNonce = null;
        _welcome = null;
        _toSwitch?.Dispose();
        _fromSwitch?.Dispose();
        _toSwitch = _fromSwitch = null;
    }

    // --- Pairing (the settings screen) ---

    /// <summary>Lets Switches pair for the given time. While closed, PairStart is refused.</summary>
    public void OpenPairing(TimeSpan duration)
    {
        lock (_lock)
        {
            _pairingOpenUntil = DateTime.UtcNow + duration;
            _pairing = null;
        }
        Logger.Log("InternetSession", $"pairing open for {duration.TotalSeconds:0}s");
    }

    public void ClosePairing()
    {
        lock (_lock)
        {
            _pairingOpenUntil = DateTime.MinValue;
            _pairing = null;
        }
    }

    /// <summary>The player's answer to <see cref="PairingRequested"/>. Accepting stores the Switch.</summary>
    public void CompletePairing(bool accept)
    {
        byte[] result;
        IPEndPoint to;
        lock (_lock)
        {
            if (_pairing?.PairKey is null) return;
            var attempt = _pairing;
            if (accept)
            {
                var hostId = Encoding.ASCII.GetBytes(_config.HostId);
                var proof = InternetCrypto.PairProof(attempt.PairKey, attempt.DeviceId, hostId);
                result = InternetWire.Plain(InternetWire.Type.PairResult, [1, .. hostId, .. proof]);
                PairedDevices.Add(attempt.DeviceId, attempt.Name, attempt.PairKey);
                Logger.Log("InternetSession", $"paired '{attempt.Name}'");
            }
            else
            {
                result = InternetWire.Plain(InternetWire.Type.PairResult, [0]);
                Logger.Log("InternetSession", $"pairing with '{attempt.Name}' refused");
            }
            attempt.Result = result;
            to = attempt.From;
            _pairingOpenUntil = DateTime.MinValue;
        }
        if (_socket is { } socket) Send(socket, result, to);
    }

    // --- Receive ---

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

    private void Handle(UdpClient socket, byte[] data, IPEndPoint from, bool viaRelay = false)
    {
        if (!viaRelay && _server is not null && from.Equals(_server) && SolarNX.Rendezvous.RendezvousWire.Parse(data) is { } rvType)
        {
            HandleRendezvous(socket, rvType, data.AsSpan(SolarNX.Rendezvous.RendezvousWire.HeaderSize));
            return;
        }
        if (!viaRelay)
        {
            lock (_lock) _relayed.Remove(from); // the Switch reaches us directly (again)
        }

        var type = InternetWire.Parse(data, out var version);
        if (type is null) return;
        if (version != InternetWire.Version)
        {
            if (type is InternetWire.Type.Hello or InternetWire.Type.PairStart)
                Send(socket, InternetWire.Plain(InternetWire.Type.Reject, [(byte)InternetWire.RejectReason.WrongVersion, .. PeerVersion.Own()]), from);
            return;
        }
        var payload = data.AsSpan(InternetWire.HeaderSize);

        switch (type)
        {
            case InternetWire.Type.Hello:
                HandleHello(socket, payload, from);
                return;
            case InternetWire.Type.PairStart:
                HandlePairStart(socket, payload, from);
                return;
            case InternetWire.Type.PairNonceS:
                HandlePairNonce(socket, payload, from);
                return;
        }

        // Everything else is sealed and only accepted from the current peer.
        byte[]? plain;
        lock (_lock)
        {
            if (_peer is null || !_peer.Equals(from) || _fromSwitch is null) return;
            plain = _fromSwitch.Open(data, InternetWire.HeaderSize, out var counter);
            if (plain is null || !_replay.Accept(counter)) return; // forged, damaged or replayed
            _lastFromPeerUtc = DateTime.UtcNow;
        }
        HandleSealed(socket, type.Value, plain, from);
    }

    private void HandleSealed(UdpClient socket, InternetWire.Type type, byte[] plain, IPEndPoint from)
    {
        HidPacket? started = null;
        HidPacket? input = null;
        switch (type)
        {
            case InternetWire.Type.Input when plain.Length >= HidPacket.PayloadSize:
                input = HidPacket.Parse(plain.AsSpan(0, HidPacket.PayloadSize));
                lock (_lock)
                {
                    _lastInputUtc = DateTime.UtcNow;
                    if (!_sessionActive)
                    {
                        _sessionActive = true;
                        started = input;
                    }
                }
                break;
            case InternetWire.Type.Bye:
                EndSession("the Switch went back to its menu", notifySwitch: false);
                return;
            case InternetWire.Type.Keepalive:
                SendSealed(socket, InternetWire.Type.KeepaliveAck, [], from);
                return;
            case InternetWire.Type.ClientControl when plain.Length >= 1:
                ControlReceived?.Invoke((ControlWire.ClientMessage)plain[0], plain[1..]);
                return;
            case InternetWire.Type.LibReply when plain.Length >= 12:
                lock (_lock)
                {
                    var offerId = BinaryPrimitives.ReadUInt32LittleEndian(plain);
                    if (_pendingOffer is not null && offerId == _pendingOfferId)
                        _pendingOffer.TrySetResult(new LibraryWire.Reply(plain[4] == 1, plain[5] != 0, BinaryPrimitives.ReadUInt32LittleEndian(plain.AsSpan(8))));
                }
                return;
            case InternetWire.Type.LibAck when plain.Length >= 12:
                lock (_lock)
                {
                    var hash = BinaryPrimitives.ReadUInt64LittleEndian(plain);
                    var received = BinaryPrimitives.ReadUInt32LittleEndian(plain.AsSpan(8));
                    if (hash == _ackHash && received > _ackedBytes)
                    {
                        _ackedBytes = received;
                        _ackSignal.Release();
                    }
                }
                return;
        }

        if (started is not null)
        {
            Logger.Log("InternetSession", $"first input from {from} - session started");
            SessionStarted?.Invoke(started);
        }
        if (input is not null) PacketReceived?.Invoke(input);
    }

    private void HandleHello(UdpClient socket, ReadOnlySpan<byte> payload, IPEndPoint from)
    {
        if (payload.Length < InternetWire.DeviceIdSize + InternetCrypto.NonceSize) return;
        var deviceId = payload[..InternetWire.DeviceIdSize].ToArray();
        var switchNonce = payload.Slice(InternetWire.DeviceIdSize, InternetCrypto.NonceSize).ToArray();

        // Protocol 1.1 appends its version (PeerVersion wire form); a 0.2.4 Switch's Hello ends here.
        var versionAt = InternetWire.DeviceIdSize + InternetCrypto.NonceSize;
        var version = payload.Length >= versionAt + PeerVersion.WireSize
            ? PeerVersion.Read(payload.Slice(versionAt, PeerVersion.WireSize))
            : PeerVersion.Legacy;
        _compatibility.Report(version);
        if (!version.Compatible)
        {
            // Another major protocol: a session would only show a black screen. The Reject carries
            // this PC's version so the Switch can say which side to update.
            Send(socket, InternetWire.Plain(InternetWire.Type.Reject, [(byte)InternetWire.RejectReason.WrongVersion, .. PeerVersion.Own()]), from);
            return;
        }

        var device = PairedDevices.Find(deviceId);
        if (device is null)
        {
            Send(socket, InternetWire.Plain(InternetWire.Type.Reject, [(byte)InternetWire.RejectReason.NotPaired]), from);
            return;
        }

        byte[] welcome;
        var endOld = false;
        lock (_lock)
        {
            var sameDevice = _peerDeviceId is not null && _peerDeviceId.AsSpan().SequenceEqual(deviceId);
            var peerGone = _peer is null || DateTime.UtcNow - _lastFromPeerUtc > PeerIdleTimeout;
            if (!sameDevice && !peerGone && _sessionActive)
            {
                welcome = InternetWire.Plain(InternetWire.Type.Reject, [(byte)InternetWire.RejectReason.Busy]);
            }
            else if (sameDevice && _peer!.Equals(from) && _peerNonce is not null && _peerNonce.AsSpan().SequenceEqual(switchNonce) && _welcome is not null)
            {
                welcome = _welcome; // the same Hello again - our Welcome got lost
                _lastFromPeerUtc = DateTime.UtcNow;
            }
            else
            {
                // A new handshake: fresh keys. The same Switch saying Hello with a new nonce lost its
                // session state (restart, or it stopped hearing us) - the old session is over.
                endOld = _sessionActive;
                var pcNonce = InternetCrypto.RandomBytes(InternetCrypto.NonceSize);
                var keys = InternetCrypto.DeriveSessionKeys(device.Key, switchNonce, pcNonce);
                ForgetPeer();
                _peer = from;
                _peerVersion = version;
                _peerDeviceId = deviceId;
                _peerNonce = switchNonce;
                _toSwitch = new InternetCrypto.Channel(keys.EncryptPcToSwitch, keys.MacPcToSwitch);
                _fromSwitch = new InternetCrypto.Channel(keys.EncryptSwitchToPc, keys.MacSwitchToPc);
                _replay = new InternetCrypto.ReplayWindow();
                _sendCounter = 1;
                _lastFromPeerUtc = DateTime.UtcNow;

                var hostId = Encoding.ASCII.GetBytes(_config.HostId);
                byte[] aad = [.. InternetWire.Header(InternetWire.Type.Welcome), .. pcNonce, .. hostId];
                // The SessionConfig, then (1.1) this PC's version - a 0.2.4 Switch reads the config and
                // ignores the rest.
                _welcome = _toSwitch.Seal(1, aad, [.. SessionController.BuildSessionConfig(_config).ToBytes(), .. PeerVersion.Own()]);
                welcome = _welcome;
                Logger.Log("InternetSession", $"'{device.Name}' connected from {from}");
            }
        }
        if (endOld) EndSession("the Switch reconnected", notifySwitch: false);
        Send(socket, welcome, from);
    }

    private void HandlePairStart(UdpClient socket, ReadOnlySpan<byte> payload, IPEndPoint from)
    {
        if (payload.Length < InternetWire.DeviceIdSize + InternetCrypto.KeySize + InternetWire.NameSize) return;
        var deviceId = payload[..InternetWire.DeviceIdSize].ToArray();
        var switchPublic = payload.Slice(InternetWire.DeviceIdSize, InternetCrypto.KeySize).ToArray();
        var name = Encoding.UTF8.GetString(payload.Slice(InternetWire.DeviceIdSize + InternetCrypto.KeySize, InternetWire.NameSize)).TrimEnd('\0').Trim();
        if (name.Length == 0) name = "Nintendo Switch";

        byte[] reply;
        lock (_lock)
        {
            if (DateTime.UtcNow > _pairingOpenUntil)
            {
                reply = InternetWire.Plain(InternetWire.Type.Reject, [(byte)InternetWire.RejectReason.PairingClosed]);
            }
            else
            {
                // A repeated PairStart for the attempt in progress gets the same PairKey; anything else
                // starts over (one attempt at a time).
                if (_pairing is null || !_pairing.From.Equals(from) || !_pairing.SwitchPublic.AsSpan().SequenceEqual(switchPublic))
                {
                    var (priv, pub) = InternetCrypto.NewKeyPair();
                    _pairing = new PairingAttempt
                    {
                        From = from,
                        DeviceId = deviceId,
                        Name = name,
                        SwitchPublic = switchPublic,
                        PcPrivate = priv,
                        PcPublic = pub,
                        PcNonce = InternetCrypto.RandomBytes(InternetCrypto.NonceSize),
                    };
                    Logger.Log("InternetSession", $"pairing request from '{name}' ({from})");
                }
                var commit = InternetCrypto.PairCommit(_pairing.PcPublic, switchPublic, _pairing.PcNonce);
                reply = InternetWire.Plain(InternetWire.Type.PairKey, [.. _pairing.PcPublic, .. commit]);
            }
        }
        Send(socket, reply, from);
    }

    private void HandlePairNonce(UdpClient socket, ReadOnlySpan<byte> payload, IPEndPoint from)
    {
        if (payload.Length < InternetWire.DeviceIdSize + InternetCrypto.NonceSize) return;
        var switchNonce = payload.Slice(InternetWire.DeviceIdSize, InternetCrypto.NonceSize).ToArray();

        byte[] reply;
        (string Name, int Code)? announce = null;
        lock (_lock)
        {
            var attempt = _pairing;
            if (attempt is null || !attempt.From.Equals(from)) return;
            if (attempt.Result is not null)
            {
                reply = attempt.Result; // already decided - the Switch is still waiting for it
            }
            else
            {
                if (attempt.SwitchNonce is null)
                {
                    attempt.SwitchNonce = switchNonce;
                    var shared = InternetCrypto.SharedSecret(attempt.PcPrivate, attempt.SwitchPublic);
                    attempt.PairKey = InternetCrypto.PairKey(shared, switchNonce, attempt.PcNonce);
                    attempt.Code = InternetCrypto.PairCode(attempt.SwitchPublic, attempt.PcPublic, switchNonce, attempt.PcNonce);
                    announce = (attempt.Name, attempt.Code);
                }
                else if (!attempt.SwitchNonce.AsSpan().SequenceEqual(switchNonce))
                {
                    return; // a different nonce in the same attempt - ignore, the Switch starts over if it must
                }
                reply = InternetWire.Plain(InternetWire.Type.PairNonceP, attempt.PcNonce);
            }
        }
        Send(socket, reply, from);
        if (announce is { } a) PairingRequested?.Invoke(a.Name, a.Code);
    }

    // --- Send ---

    private void SendSealed(UdpClient socket, InternetWire.Type type, ReadOnlySpan<byte> payload, IPEndPoint to)
    {
        InternetCrypto.Channel? channel;
        lock (_lock) channel = _toSwitch;
        if (channel is null) return;
        var counter = (ulong)Interlocked.Increment(ref _sendCounter);
        Send(socket, channel.Seal(counter, InternetWire.Header(type), payload), to);
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
            if (peer is null || _socket is not { } socket) continue;
            SendSealed(socket, type, result.Buffer, peer);
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
                lock (_lock) ForgetPeer();
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
            for (var i = 0; i < 3; i++) SendSealed(socket, InternetWire.Type.End, [], peer); // a lost End would leave the Switch waiting
        }
        Logger.Log("InternetSession", $"session stopped: {reason}");
        SessionStopped?.Invoke();
    }

    // Directly, or - for a Switch that reaches us through the rendezvous server's relay - wrapped in
    // RelayBack to the server.
    private void Send(UdpClient socket, byte[] packet, IPEndPoint to)
    {
        bool relayed;
        lock (_lock) relayed = _relayed.Contains(to);
        try
        {
            if (relayed && _server is { } server)
            {
                var wrapped = SolarNX.Rendezvous.RendezvousWire.Packet(SolarNX.Rendezvous.RendezvousWire.Type.RelayBack, SolarNX.Rendezvous.RendezvousWire.EndpointSize + packet.Length);
                SolarNX.Rendezvous.RendezvousWire.WriteEndpoint(wrapped.AsSpan(SolarNX.Rendezvous.RendezvousWire.HeaderSize), to);
                packet.CopyTo(wrapped, SolarNX.Rendezvous.RendezvousWire.HeaderSize + SolarNX.Rendezvous.RendezvousWire.EndpointSize);
                socket.Send(wrapped, wrapped.Length, server);
            }
            else
            {
                socket.Send(packet, packet.Length, to);
            }
        }
        catch (Exception ex) when (ex is SocketException or ObjectDisposedException) { /* lost like any datagram */ }
    }

    // --- Rendezvous server ---

    // Registers every 15s from the session socket itself, so the server learns the public address the
    // router maps exactly this socket to - the address a Switch must punch towards.
    private async Task RendezvousLoopAsync(CancellationToken ct)
    {
        var (host, port) = SplitHostPort(_config.RendezvousServer, 19020);
        while (!ct.IsCancellationRequested)
        {
            try
            {
                if (_server is null || DateTime.UtcNow - _serverResolvedUtc > TimeSpan.FromMinutes(5))
                {
                    var addresses = await Dns.GetHostAddressesAsync(host, AddressFamily.InterNetwork, ct);
                    if (addresses.Length > 0)
                    {
                        var resolved = new IPEndPoint(addresses[0], port);
                        if (!resolved.Equals(_server)) Logger.Log("InternetSession", $"rendezvous server {_config.RendezvousServer} -> {resolved}");
                        _server = resolved;
                        _serverResolvedUtc = DateTime.UtcNow;
                    }
                }
                if (_server is { } server && _socket is { } socket)
                {
                    var register = SolarNX.Rendezvous.RendezvousWire.Packet(SolarNX.Rendezvous.RendezvousWire.Type.Register,
                        SolarNX.Rendezvous.RendezvousWire.HostIdSize + SolarNX.Rendezvous.RendezvousWire.KeySize + SolarNX.Rendezvous.RendezvousWire.EndpointSize);
                    var body = register.AsSpan(SolarNX.Rendezvous.RendezvousWire.HeaderSize);
                    Encoding.ASCII.GetBytes(_config.HostId).AsSpan(0, Math.Min(_config.HostId.Length, SolarNX.Rendezvous.RendezvousWire.HostIdSize)).CopyTo(body);
                    var key = Encoding.UTF8.GetBytes(_config.RendezvousKey);
                    key.AsSpan(0, Math.Min(key.Length, SolarNX.Rendezvous.RendezvousWire.KeySize)).CopyTo(body[SolarNX.Rendezvous.RendezvousWire.HostIdSize..]);
                    if (LocalAddressTowards(server) is { } local)
                        SolarNX.Rendezvous.RendezvousWire.WriteEndpoint(body[(SolarNX.Rendezvous.RendezvousWire.HostIdSize + SolarNX.Rendezvous.RendezvousWire.KeySize)..], new IPEndPoint(local, Port));
                    socket.Send(register, register.Length, server);
                }
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Logger.Log("InternetSession", $"rendezvous server {_config.RendezvousServer}: {ex.Message}");
            }
            try { await Task.Delay(TimeSpan.FromSeconds(15), ct); } catch (OperationCanceledException) { return; }
        }
    }

    private void HandleRendezvous(UdpClient socket, SolarNX.Rendezvous.RendezvousWire.Type type, ReadOnlySpan<byte> payload)
    {
        const int endpointSize = SolarNX.Rendezvous.RendezvousWire.EndpointSize;
        switch (type)
        {
            case SolarNX.Rendezvous.RendezvousWire.Type.Registered when payload.Length >= endpointSize:
                var publicEndpoint = SolarNX.Rendezvous.RendezvousWire.ReadEndpoint(payload);
                if (!publicEndpoint.Equals(PublicEndpoint)) Logger.Log("InternetSession", $"registered on the rendezvous server - public address {publicEndpoint}");
                PublicEndpoint = publicEndpoint;
                break;
            case SolarNX.Rendezvous.RendezvousWire.Type.Introduce when payload.Length >= endpointSize:
                // A Switch is about to send to our public address: send towards it too, so our router
                // has an outgoing mapping its packets match (hole punching).
                var switchEndpoint = SolarNX.Rendezvous.RendezvousWire.ReadEndpoint(payload);
                var punch = InternetWire.Plain(InternetWire.Type.Punch, []);
                for (var i = 0; i < 3; i++)
                {
                    try { socket.Send(punch, punch.Length, switchEndpoint); }
                    catch (SocketException) { }
                }
                Logger.Log("InternetSession", $"rendezvous: Switch at {switchEndpoint} is coming - punching towards it");
                break;
            case SolarNX.Rendezvous.RendezvousWire.Type.RelayDown when payload.Length > endpointSize:
                var from = SolarNX.Rendezvous.RendezvousWire.ReadEndpoint(payload);
                lock (_lock)
                {
                    if (_relayed.Add(from)) Logger.Log("InternetSession", $"Switch {from} reaches us through the server's relay");
                }
                Handle(socket, payload[endpointSize..].ToArray(), from, viaRelay: true);
                break;
        }
    }

    // The LAN address this PC uses towards the server - what a Switch behind the same router should
    // dial (see RendezvousWire). Connecting a UDP socket sends nothing; it only picks the route.
    private static IPAddress? LocalAddressTowards(IPEndPoint server)
    {
        try
        {
            using var probe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
            probe.Connect(server);
            return (probe.LocalEndPoint as IPEndPoint)?.Address;
        }
        catch (SocketException)
        {
            return null;
        }
    }

    private static (string Host, int Port) SplitHostPort(string address, int defaultPort)
    {
        var text = address.Trim();
        var colon = text.LastIndexOf(':');
        return colon > 0 && int.TryParse(text[(colon + 1)..], out var port) ? (text[..colon], port) : (text, defaultPort);
    }

    // --- Library (used by LibraryPusher in Internet mode) ---

    /// <summary>Offers the library header to the Switch; null if there's no Switch or it didn't answer.</summary>
    internal async Task<LibraryWire.Reply?> OfferLibraryAsync(ulong hash, bool swapAbxy, uint layoutRevision, CancellationToken ct)
    {
        var peer = Peer;
        if (peer is null || _socket is not { } socket) return null;

        var tcs = new TaskCompletionSource<LibraryWire.Reply>(TaskCreationOptions.RunContinuationsAsynchronously);
        var payload = new byte[20];
        lock (_lock)
        {
            _pendingOfferId = ++_offerId;
            _pendingOffer = tcs;
            BinaryPrimitives.WriteUInt32LittleEndian(payload, _pendingOfferId);
        }
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(4), hash);
        payload[12] = swapAbxy ? (byte)1 : (byte)0;
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(16), layoutRevision);

        for (var attempt = 0; attempt < 3 && !tcs.Task.IsCompleted; attempt++)
        {
            SendSealed(socket, InternetWire.Type.LibOffer, payload, peer);
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
                var chunk = new byte[16 + length];
                BinaryPrimitives.WriteUInt64LittleEndian(chunk, hash);
                BinaryPrimitives.WriteUInt32LittleEndian(chunk.AsSpan(8), (uint)payload.Length);
                BinaryPrimitives.WriteUInt32LittleEndian(chunk.AsSpan(12), (uint)next);
                payload.AsSpan((int)next, length).CopyTo(chunk.AsSpan(16));
                SendSealed(socket, InternetWire.Type.LibChunk, chunk, peer);
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
