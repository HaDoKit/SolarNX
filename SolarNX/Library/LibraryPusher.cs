using System.Net.Sockets;

namespace SolarNX.Library;

/// <summary>
/// Keeps the Switch's copy of the library current. While armed it dials ClientIp:LibraryPort every
/// few seconds (the Switch listens from app start, so the list is there even on the very first menu,
/// before any HID session exists), offers the current content hash, and sends the payload only if
/// the Switch asks for it - see LibraryWire for the exchange. Separate from the HID connection on
/// purpose: HID only exists during a session, and the menu needs the list before one starts.
///
/// The payload (covers re-encoded as Switch-sized JPEGs) is built lazily and cached until the
/// library changes, so an idle armed host costs one connect + 32 bytes per interval.
/// </summary>
public sealed class LibraryPusher : IDisposable
{
    private static readonly TimeSpan Interval = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan ReplyTimeout = TimeSpan.FromSeconds(5);

    // A 0.2.4 Switch (library version 2) drops a version-3 header without answering; it then gets
    // version 2. Version 3 is tried again now and then - the Switch app may have been updated since.
    private static readonly TimeSpan LegacyRecheck = TimeSpan.FromSeconds(30);
    private bool _legacySwitch;
    private DateTime _nextVersionCheckUtc;

    private readonly Config _config;
    private readonly GameLibrary _library;
    private readonly Action<bool, uint> _onSwitchButtonLayout;
    private readonly Func<Session.InternetSession?> _internetSession;
    private readonly Session.SwitchCompatibility _compatibility;
    private readonly object _payloadLock = new();
    private (byte[] Payload, ulong Hash)? _cached;

    private CancellationTokenSource? _runCts;
    private bool? _lastAttemptOk; // only log on transitions - the Switch being off is the normal case

    /// <param name="onSwitchButtonLayout">Called (on the thread pool) with the Switch's button layout
    /// and its revision after every exchange - the owner adopts it if it is newer.</param>
    /// <param name="internetSession">The current session when in Internet mode (null in Wi-Fi mode) -
    /// the library then goes over its UDP channel to wherever the Switch is, not over TCP.</param>
    /// <param name="compatibility">Gets the Switch's version from every version-3 reply.</param>
    public LibraryPusher(Config config, GameLibrary library, Action<bool, uint> onSwitchButtonLayout, Func<Session.InternetSession?> internetSession, Session.SwitchCompatibility compatibility)
    {
        _config = config;
        _library = library;
        _onSwitchButtonLayout = onSwitchButtonLayout;
        _internetSession = internetSession;
        _compatibility = compatibility;
        _library.Changed += () => { lock (_payloadLock) _cached = null; };
    }

    public void Start()
    {
        Stop();
        _runCts = new CancellationTokenSource();
        var ct = _runCts.Token;
        _ = Task.Run(() => RunAsync(ct));
    }

    public void Stop()
    {
        _runCts?.Cancel();
        _runCts?.Dispose();
        _runCts = null;
    }

    private async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (_config.IsValid())
            {
                try
                {
                    await PushOnceAsync(ct);
                    if (_lastAttemptOk != true) Logger.Log("LibraryPusher", $"library channel to {Target} is up");
                    _lastAttemptOk = true;
                }
                catch (Exception ex) when (ex is not OperationCanceledException || !ct.IsCancellationRequested)
                {
                    if (_lastAttemptOk != false) Logger.Log("LibraryPusher", $"library channel to {Target} unavailable: {ex.Message}");
                    _lastAttemptOk = false;
                }
            }

            try { await Task.Delay(Interval, ct); } catch (OperationCanceledException) { return; }
        }
    }

    private string Target => _config.ConnectionMode == Session.ConnectionMode.Internet
        ? $"the Switch over UDP {Session.InternetSession.Port}"
        : $"{_config.ClientIp}:{_config.LibraryPort}";

    private async Task PushOnceAsync(CancellationToken ct)
    {
        var (payload, hash) = GetPayload();

        if (_config.ConnectionMode == Session.ConnectionMode.Internet)
        {
            var session = _internetSession();
            if (session?.Peer is null) return; // no Switch has said hello yet - nothing to push to
            var offer = await session.OfferLibraryAsync(hash, _config.SwitchButtonLayout, _config.ButtonLayoutRevision, ct)
                ?? throw new IOException("the Switch did not answer the library offer");
            _onSwitchButtonLayout(offer.SwapAbxy, offer.LayoutRevision);
            if (!offer.NeedsPayload) return;
            await session.SendLibraryAsync(hash, payload, ct);
            Logger.Log("LibraryPusher", $"sent library to the Switch over UDP: {payload.Length / 1024}KB, hash {hash:X16}");
            return;
        }

        var legacy = _legacySwitch && DateTime.UtcNow < _nextVersionCheckUtc;
        try
        {
            await ExchangeAsync(payload, hash, legacy, ct);
        }
        catch (Exception ex) when (!legacy && ex is EndOfStreamException or IOException && !ct.IsCancellationRequested)
        {
            // The connection was taken and then dropped (closed or reset - it leaves the rest of our
            // header unread) without a reply: a Switch that only knows version 2. Ask it that way.
            if (!_legacySwitch) Logger.Log("LibraryPusher", "the Switch dropped the version-3 header - an older Switch app, retrying with version 2");
            _legacySwitch = true;
            _nextVersionCheckUtc = DateTime.UtcNow + LegacyRecheck;
            _compatibility.Report(Session.PeerVersion.Legacy);
            await ExchangeAsync(payload, hash, legacy: true, ct);
        }
    }

    private async Task ExchangeAsync(byte[] payload, ulong hash, bool legacy, CancellationToken ct)
    {
        using var client = new TcpClient { NoDelay = true };
        using (var connectCts = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            connectCts.CancelAfter(ConnectTimeout);
            await client.ConnectAsync(_config.ClientIp, _config.LibraryPort, connectCts.Token);
        }

        var stream = client.GetStream();
        using var replyCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        replyCts.CancelAfter(ReplyTimeout);

        await stream.WriteAsync(LibraryWire.BuildHeader(hash, _config.SwitchButtonLayout, _config.ButtonLayoutRevision, legacy), replyCts.Token);

        var replyBytes = new byte[legacy ? LibraryWire.LegacyReplySize : LibraryWire.ReplySize];
        await stream.ReadExactlyAsync(replyBytes, replyCts.Token);
        if (!legacy)
        {
            _legacySwitch = false;
            var (version, extra) = LibraryWire.ParseReplyVersion(replyBytes);
            _compatibility.Report(version);
            if (!version.Compatible) return; // the Switch skips the payload too - nothing more to say
            if (extra > 0) await stream.ReadExactlyAsync(new byte[Math.Min(extra, 4096u)], replyCts.Token);
        }
        var reply = LibraryWire.ParseReply(replyBytes);
        _onSwitchButtonLayout(reply.SwapAbxy, reply.LayoutRevision);

        if (!reply.NeedsPayload) return; // already has this exact library

        replyCts.CancelAfter(TimeSpan.FromSeconds(30)); // the payload itself can be a few MB with many covers
        await stream.WriteAsync(payload, replyCts.Token);
        Logger.Log("LibraryPusher", $"sent library to the Switch: {payload.Length / 1024}KB, hash {hash:X16}");
    }

    private (byte[] Payload, ulong Hash) GetPayload()
    {
        lock (_payloadLock)
        {
            _cached ??= LibraryWire.BuildPayload(_library.Snapshot());
            return _cached.Value;
        }
    }

    public void Dispose() => Stop();
}
