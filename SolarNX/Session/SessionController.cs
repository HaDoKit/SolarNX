using System.Net.Sockets;
using System.Threading.Channels;
using SolarNX.Audio;
using SolarNX.Input;

namespace SolarNX.Session;

/// <summary>
/// Mirrors the original client's connectHID() loop: repeatedly dial the client's HID port,
/// and treat a successful connection as "session started" / its closure as "session stopped".
/// This is the single component driving the rest of the app's lifecycle.
/// </summary>
public sealed class SessionController : ISessionController
{
    private readonly Config _config;
    private readonly SwitchCompatibility _compatibility;

    // The client sends a HID packet every ~23ms for as long as it's alive (see main.c's
    // HID_SEND_INTERVAL_NS), so this much silence means it's gone - typically the Switch went to
    // sleep or dropped off Wi-Fi without a FIN/RST ever reaching this side. Without it, ReadAsync
    // waited forever: the session never ended, Big Picture stayed open and ffmpeg kept streaming
    // to a dead address.
    private static readonly TimeSpan HidIdleTimeout = TimeSpan.FromSeconds(3);

    // Before the first packet the Switch is legitimately silent: it accepts connections only after
    // the player has picked something in its menu, and the kernel keeps this connect waiting in the
    // listen backlog until then. Confirmed live: with the 3s timeout applied here too, the host gave
    // up on the waiting connection at the exact moment the player picked a game, the Switch then
    // accepted that half-dead connection, and the session never started. A longer pre-session timeout
    // keeps the waiting connection alive through most menu visits; when it does expire the socket is
    // closed immediately (see `new TcpClient()` below), so the Switch recognizes it as stale and skips
    // it. Not much longer than 15s: a connection the Switch silently lost would otherwise block the
    // next attempt for that long.
    private static readonly TimeSpan PreSessionIdleTimeout = TimeSpan.FromSeconds(15);

    private CancellationTokenSource? _runCts;
    private Task? _runTask;
    private TcpClient? _currentClient;

    // Control messages for the current connection (Session/ControlWire.cs) - queued by SendControl
    // from any thread and written by one writer task, so frames never interleave on the stream. Null
    // while no Switch that understands them is connected. Bounded with drop-oldest: if the Switch stops
    // draining, stale rumble levels are the thing to lose, not the HID read loop's thread.
    private volatile Channel<byte[]>? _controlQueue;
    private volatile PeerVersion? _sessionPeer;
    private bool _loggedBlocked;

    public event Action<HidPacket>? SessionStarted;
    public event Action? SessionStopped;
    public event Action<HidPacket>? PacketReceived;
    public event Action<ControlWire.ClientMessage, byte[]>? ControlReceived;

    public PeerVersion? SessionPeer => _sessionPeer;

    public SessionController(Config config, SwitchCompatibility compatibility)
    {
        _config = config;
        _compatibility = compatibility;
    }

    public bool SendControl(ControlWire.HostMessage type, ReadOnlySpan<byte> payload)
    {
        var queue = _controlQueue;
        var peer = _sessionPeer;
        if (queue is null || peer is null || !ProtocolVersion.Supports(peer.Protocol, ProtocolVersion.ControlMessages))
            return false;
        return queue.Writer.TryWrite(ControlWire.TcpFrame(type, payload));
    }

    /// <summary>Forcibly drops the current client connection (if any), without disarming the
    /// whole controller - RunLoopAsync's outer loop keeps running and goes back to listening for
    /// a fresh reconnect, exactly as if the client itself had disconnected. Used by AppOrchestrator
    /// when Big Picture's window disappears unexpectedly (closed by the first user, or crashed) -
    /// the second person's session shouldn't be able to keep "controlling" a Big Picture that no
    /// longer exists.</summary>
    public void ForceDisconnect()
    {
        try { _currentClient?.Close(); } catch { /* best-effort */ }
    }

    public void Start()
    {
        Stop();
        _runCts = new CancellationTokenSource();
        // Task.Run, not a direct call: Start() runs on the WinForms UI thread before
        // Application.Run() begins pumping messages, so a direct call here would capture that
        // thread's SynchronizationContext for every downstream await in this whole session/capture
        // chain (OnSessionStarted, SteamLauncher, ...). If the UI thread is
        // ever busy (e.g. a modal dialog), every one of those continuations queues up and the whole
        // chain silently stalls with no exception. Starting from a thread-pool thread instead means
        // nothing in this chain ever needs the UI thread's message loop to make progress.
        _runTask = Task.Run(() => RunLoopAsync(_runCts.Token));
    }

    public void Stop()
    {
        _runCts?.Cancel();
        _runCts = null;
    }

    private async Task RunLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (!_config.IsValid())
            {
                await Task.Delay(1000, ct).ContinueWith(_ => { }, TaskScheduler.Default);
                continue;
            }

            // The Switch speaks another major protocol (learned over the library channel) - a session
            // would only show a black screen. The UI says which side to update; the library channel
            // keeps checking, so this lifts by itself once the Switch app is updated.
            if (_compatibility.Blocks)
            {
                if (!_loggedBlocked) Logger.Log("SessionController", $"not connecting: the Switch's protocol is incompatible ({_compatibility.Switch})");
                _loggedBlocked = true;
                await Task.Delay(1000, ct).ContinueWith(_ => { }, TaskScheduler.Default);
                continue;
            }
            _loggedBlocked = false;

            // Disposed explicitly (the finally blocks below) as soon as the connection is over - not by
            // `using` at the end of the iteration, which only ran after the 1s retry delay: for that
            // second the socket stayed open with nobody reading it, and the Switch could accept it as
            // a live session and send into the void until the host finally closed it.
            var client = new TcpClient();
            try
            {
                await client.ConnectAsync(_config.ClientIp, _config.HidPort, ct);
                client.NoDelay = true;
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Logger.Log("SessionController", $"HID connect to {_config.ClientIp}:{_config.HidPort} failed: {ex.Message}");
                client.Dispose();
                try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
                continue;
            }

            Logger.Log("SessionController", $"HID connected to {_config.ClientIp}:{_config.HidPort}");
            _currentClient = client;

            // Sent once, immediately, before any HID input traffic - lets the client size its
            // video/audio buffers from this session's real values instead of guessing. See
            // Input/HidProtocol.cs's SessionConfig doc comment. A failure here means the
            // connection isn't really usable (the client will be blocked waiting for these bytes
            // before it opens its own video/audio streams - see network.h's
            // networkWaitForSessionConfig on the client), so it's treated the same as a failed
            // connect: log and retry from the top rather than proceeding into ReadLoopAsync.
            try
            {
                // Hello right behind the SessionConfig, as the first control message: a Switch that
                // understands them answers with its own and from then on gets the rest (SendControl);
                // a 0.2.4 Switch never reads past the SessionConfig, so it never even sees this.
                byte[] opening = [.. BuildSessionConfig(_config).ToBytes(), .. ControlWire.TcpFrame(ControlWire.HostMessage.Hello, PeerVersion.Own())];
                await client.GetStream().WriteAsync(opening, ct);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Logger.Log("SessionController", $"failed to send session config to {_config.ClientIp}: {ex.Message}");
                _currentClient = null;
                client.Dispose();
                try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
                continue;
            }

            // A bare TCP accept doesn't confirm the real SolarNX client is on the other end - some
            // other device on the network can end up holding this IP (DHCP lease reassignment,
            // typo'd address, etc.) and accept-then-immediately-close the connection, which used
            // to be enough to fire SessionStarted (and re-open Big Picture) on every such blip.
            // Only treat this as a genuine session once at least one real HID packet has actually
            // been parsed off the wire - see ReadLoopAsync's sawFirstPacket callback below.
            var sessionStarted = false;
            var controlQueue = Channel.CreateBounded<byte[]>(new BoundedChannelOptions(256) { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true });
            var writer = Task.Run(() => WriteControlLoopAsync(client, controlQueue.Reader, ct));
            _controlQueue = controlQueue;
            try
            {
                await ReadLoopAsync(client, ct, firstPacket =>
                {
                    sessionStarted = true;
                    Logger.Log("SessionController", "first HID packet received - session started");
                    SessionStarted?.Invoke(firstPacket);
                });
                Logger.Log("SessionController", "HID stream ended (remote closed)");
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                // fall through to SessionStopped + retry below. A plain network drop/reset gets one
                // line - e.g. while the Switch sits in its menu every retry is reset, once a second,
                // and full stack traces for that buried everything else in SolarNX.log.
                Logger.Log("SessionController", ex is IOException { InnerException: SocketException se }
                    ? $"HID connection dropped by the Switch ({se.SocketErrorCode})"
                    : $"HID read loop ended with exception: {ex}");
            }
            finally
            {
                _controlQueue = null;
                _sessionPeer = null;
                controlQueue.Writer.TryComplete();
                _currentClient = null;
                client.Dispose(); // close now, before the retry delay - see the comment at `new TcpClient()`
                try { await writer; } catch { /* the connection is gone anyway */ }
                if (sessionStarted)
                {
                    Logger.Log("SessionController", "session stopped");
                    SessionStopped?.Invoke();
                }
            }

            try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
        }
    }

    /// <summary>What the Switch needs to size its video/audio buffers - sent first on every session,
    /// in both connection modes.</summary>
    internal static SessionConfig BuildSessionConfig(Config config) => new()
    {
        VideoBitrateKbps = (uint)config.BitrateKbps,
        VideoWidth = (uint)config.TargetWidth,
        VideoHeight = (uint)config.TargetHeight,
        VideoFps = (uint)config.Fps,
        AudioBitrateKbps = (uint)(AudioCaptureSender.OpusBitrateBps / 1000),
        AudioSampleRate = (uint)config.AudioSampleRate,
        AudioChannels = (uint)config.AudioChannels,
    };

    private async Task ReadLoopAsync(TcpClient client, CancellationToken ct, Action<HidPacket> onFirstPacket)
    {
        var stream = client.GetStream();
        var buffer = new byte[4096];
        var firedStart = false;

        // Per connection, not per controller: a reader shared across connections kept the previous
        // connection's partial trailing frame in its buffer, and the new stream's first bytes got
        // glued onto it - one garbage packet, which is exactly the one that fires SessionStarted
        // (its AppTarget picks the app to launch) and gets applied as gamepad input.
        var frameReader = new HidFrameReader();

        using var readCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        while (!ct.IsCancellationRequested)
        {
            var idleTimeout = firedStart ? HidIdleTimeout : PreSessionIdleTimeout;
            readCts.CancelAfter(idleTimeout); // re-armed before every read - an idle timeout, not a total one
            int read;
            try
            {
                read = await stream.ReadAsync(buffer, readCts.Token);
            }
            catch (OperationCanceledException) when (!ct.IsCancellationRequested)
            {
                Logger.Log("SessionController", $"no HID data for {idleTimeout.TotalSeconds:0}s - treating client as gone");
                break;
            }
            if (read == 0) break; // remote closed

            foreach (var frame in frameReader.Feed(buffer, read))
            {
                if (frame.Packet is not { } packet)
                {
                    if (frame.Message == ControlWire.ClientMessage.Hello && frame.Payload!.Length >= PeerVersion.WireSize)
                    {
                        var peer = PeerVersion.Read(frame.Payload);
                        _sessionPeer = peer;
                        _compatibility.Report(peer);
                        Logger.Log("SessionController", $"the Switch says hello: SolarNX {peer}");
                    }
                    ControlReceived?.Invoke(frame.Message, frame.Payload!);
                    continue;
                }
                if (!firedStart)
                {
                    firedStart = true;
                    onFirstPacket(packet);
                }
                PacketReceived?.Invoke(packet);
            }
        }
    }

    private static async Task WriteControlLoopAsync(TcpClient client, ChannelReader<byte[]> queue, CancellationToken ct)
    {
        try
        {
            var stream = client.GetStream();
            await foreach (var frame in queue.ReadAllAsync(ct))
                await stream.WriteAsync(frame, ct);
        }
        catch (Exception ex) when (ex is OperationCanceledException or IOException or ObjectDisposedException or InvalidOperationException)
        {
            // The read loop notices the dead connection and ends the session.
        }
    }

    public async ValueTask DisposeAsync()
    {
        Stop();
        if (_runTask is not null)
        {
            try { await _runTask; } catch { /* cancellation */ }
        }
    }
}
