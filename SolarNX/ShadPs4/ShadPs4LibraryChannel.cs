using System.Net.Sockets;

namespace SolarNX.ShadPs4;

/// <summary>
/// DEPRECATED / not currently wired up: AppOrchestrator no longer constructs or calls this class -
/// shadPS4 sessions now launch straight into shadPS4's own `--big-picture` mode (see
/// ShadPs4Launcher.LaunchOrFocusAsync) instead of picking a game via the Switch-side library
/// picker screen this class talks to (library.c on the Switch, also deprecated/unwired). Left
/// intact rather than deleted in case that approach is worth reviving - if so, re-add
/// construction + Arm/Disarm/OnSessionStarted/OnSessionStopped wiring in AppOrchestrator.cs (see
/// git history for the exact previous wiring) and pass a real titleId into LaunchOrFocusAsync
/// again.
///
/// Dials the Switch's shadPS4 game-library port and serves one list/selection exchange per
/// connection - modeled directly on Session/SessionController.cs's dial-retry shape, but kept as
/// its own independent service rather than folded into that class.
///
/// Deliberately starts (and keeps retrying) in parallel with SessionController's own HID dial, not
/// sequenced after its SessionStarted event: the natural point where the Switch needs a game
/// picked is *before* it ever sends a single HID packet (see main.c's init() - the library screen
/// runs, and its listener closes, before startInput() ever runs), so gating this channel's dial
/// behind SessionStarted would deadlock - HID would never start while the Switch waits for this
/// channel to connect, and this channel would never be dialed while waiting for HID. Instead, this
/// keeps trying independently the whole time the app is armed; if the Switch isn't offering this
/// port right now (Steam path, or the picker screen already finished), connect attempts just fail
/// and retry harmlessly, the same way SessionController's own HID retry loop already tolerates
/// "nothing there yet."
/// </summary>
public sealed class ShadPs4LibraryChannel : IAsyncDisposable
{
    private static readonly TimeSpan SelectionReadTimeout = TimeSpan.FromSeconds(120);

    private readonly Config _config;
    private readonly ShadPs4LibraryScanner _scanner;

    private CancellationTokenSource? _runCts;
    private Task? _runTask;

    private readonly object _resultLock = new();
    private TaskCompletionSource<string?>? _pendingWaiter;
    private bool _hasUnconsumedResult;
    private string? _unconsumedResult;

    public ShadPs4LibraryChannel(Config config, ShadPs4LibraryScanner scanner)
    {
        _config = config;
        _scanner = scanner;
    }

    public void Start()
    {
        Stop();
        _runCts = new CancellationTokenSource();
        // Task.Run rather than a direct call, same reasoning as SessionController.Start(): avoid
        // capturing the WinForms UI thread's SynchronizationContext for every downstream await.
        _runTask = Task.Run(() => RunLoopAsync(_runCts.Token));
    }

    public void Stop()
    {
        _runCts?.Cancel();
        _runCts = null;

        // Discards any unconsumed result from a previous cycle - Stop() marks a clean boundary
        // (either "this cycle's answer was just consumed" or "the whole session ended"), so a
        // stale leftover here should never be handed to a later, unrelated WaitForResultAsync call.
        lock (_resultLock)
        {
            _hasUnconsumedResult = false;
            _unconsumedResult = null;
        }
    }

    /// <summary>Returns the most recently completed exchange's result immediately if one is already
    /// sitting unconsumed (the common case - the Switch-side ordering guarantees the library
    /// exchange finishes before HID can even start, see the class doc comment), otherwise waits up
    /// to timeout for one to arrive. Returns null on timeout - this bound exists specifically to
    /// cover the rare race between this channel's own connection and the separate HID connection,
    /// not as the primary mechanism.</summary>
    public async Task<string?> WaitForResultAsync(TimeSpan timeout, CancellationToken ct)
    {
        TaskCompletionSource<string?> waiter;
        lock (_resultLock)
        {
            if (_hasUnconsumedResult)
            {
                var result = _unconsumedResult;
                _hasUnconsumedResult = false;
                _unconsumedResult = null;
                return result;
            }

            _pendingWaiter = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
            waiter = _pendingWaiter;
        }

        using var delayCts = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var delayTask = Task.Delay(timeout, delayCts.Token);
        var completed = await Task.WhenAny(waiter.Task, delayTask);

        if (completed == waiter.Task)
        {
            delayCts.Cancel();
            return await waiter.Task;
        }

        Logger.Log("ShadPs4LibraryChannel", $"WaitForResultAsync timed out after {timeout.TotalSeconds:0}s");
        lock (_resultLock)
        {
            if (ReferenceEquals(_pendingWaiter, waiter)) _pendingWaiter = null;
        }
        return null;
    }

    private void PublishResult(string? titleId)
    {
        lock (_resultLock)
        {
            if (_pendingWaiter is not null)
            {
                _pendingWaiter.TrySetResult(titleId);
                _pendingWaiter = null;
            }
            else
            {
                _hasUnconsumedResult = true;
                _unconsumedResult = titleId;
            }
        }
    }

    private async Task RunLoopAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            if (!_config.IsValid())
            {
                try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
                continue;
            }

            using var client = new TcpClient();
            try
            {
                await client.ConnectAsync(_config.ClientIp, _config.LibraryPort, ct);
                client.NoDelay = true;
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                // Expected steady-state whenever the Switch isn't offering this port right now -
                // must stay silent/harmless, not spam errors, mirroring SessionController's own HID
                // dial-retry loop's tolerance for "nothing there yet."
                try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
                continue;
            }

            Logger.Log("ShadPs4LibraryChannel", $"connected to {_config.ClientIp}:{_config.LibraryPort}");

            string? titleId = null;
            try
            {
                titleId = await ServeOneExchangeAsync(client, ct);
            }
            catch (Exception ex) when (ex is not OperationCanceledException)
            {
                Logger.Log("ShadPs4LibraryChannel", $"exchange failed: {ex.Message}");
            }

            PublishResult(titleId);

            try { await Task.Delay(1000, ct); } catch (TaskCanceledException) { }
        }
    }

    private async Task<string?> ServeOneExchangeAsync(TcpClient client, CancellationToken ct)
    {
        var games = _scanner.ScanLibrary();
        Logger.Log("ShadPs4LibraryChannel", $"scanned {games.Count} shadPS4 game(s)");

        var stream = client.GetStream();
        var listMessage = LibraryProtocol.BuildListMessage(games);
        await stream.WriteAsync(listMessage, ct);

        return await LibraryProtocol.ReadSelectionResponseAsync(stream, SelectionReadTimeout, ct);
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
