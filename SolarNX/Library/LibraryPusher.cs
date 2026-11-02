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
/// library changes, so an idle armed host costs one connect + 17 bytes per interval.
/// </summary>
public sealed class LibraryPusher : IDisposable
{
    private static readonly TimeSpan Interval = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(2);
    private static readonly TimeSpan ReplyTimeout = TimeSpan.FromSeconds(5);

    private readonly Config _config;
    private readonly GameLibrary _library;
    private readonly object _payloadLock = new();
    private (byte[] Payload, ulong Hash)? _cached;

    private CancellationTokenSource? _runCts;
    private bool? _lastAttemptOk; // only log on transitions - the Switch being off is the normal case

    public LibraryPusher(Config config, GameLibrary library)
    {
        _config = config;
        _library = library;
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
                    if (_lastAttemptOk != true) Logger.Log("LibraryPusher", $"library channel to {_config.ClientIp}:{_config.LibraryPort} is up");
                    _lastAttemptOk = true;
                }
                catch (Exception ex) when (ex is not OperationCanceledException || !ct.IsCancellationRequested)
                {
                    if (_lastAttemptOk != false) Logger.Log("LibraryPusher", $"library channel to {_config.ClientIp}:{_config.LibraryPort} unavailable: {ex.Message}");
                    _lastAttemptOk = false;
                }
            }

            try { await Task.Delay(Interval, ct); } catch (OperationCanceledException) { return; }
        }
    }

    private async Task PushOnceAsync(CancellationToken ct)
    {
        var (payload, hash) = GetPayload();

        using var client = new TcpClient { NoDelay = true };
        using (var connectCts = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            connectCts.CancelAfter(ConnectTimeout);
            await client.ConnectAsync(_config.ClientIp, _config.LibraryPort, connectCts.Token);
        }

        var stream = client.GetStream();
        using var replyCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        replyCts.CancelAfter(ReplyTimeout);

        await stream.WriteAsync(LibraryWire.BuildHeader(hash), replyCts.Token);

        var reply = new byte[1];
        if (await stream.ReadAsync(reply, replyCts.Token) != 1)
            throw new IOException("Switch closed the library channel before replying");

        if (reply[0] != 1) return; // already has this exact library

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
