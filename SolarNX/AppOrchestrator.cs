using System.Diagnostics;
using System.Runtime.Versioning;
using SolarNX.Audio;
using SolarNX.BigPicture;
using SolarNX.Capture;
using SolarNX.Input;
using SolarNX.Library;
using SolarNX.Native;
using SolarNX.Session;
using SolarNX.ShadPs4;

namespace SolarNX;

/// <summary>
/// Wires SessionController's start/stop signals to the capture/encode/input pipelines.
///
/// Steam and any games it launches run on the normal interactive desktop and are captured via
/// ffmpeg's own gfxcapture input, scoped to the real primary monitor. Big Picture/games are
/// visible on the real screen (the first user sees the same thing the second person does), so
/// Big Picture is exited (not just left running) whenever the client disconnects, giving the
/// first user their normal desktop back between sessions.
/// </summary>
[SupportedOSPlatform("windows8.0")]
public sealed class AppOrchestrator : IDisposable
{
    private readonly Config _config;
    private ISessionController _session;
    private readonly SteamLauncher _launcher = new();
    private readonly ShadPs4Launcher _shadLauncher = new();
    private readonly VirtualGamepadHub _gamepads = new();
    private readonly TouchInjector _touch = new();

    private VideoEncoder? _videoEncoder;
    private AudioCaptureSender? _audioEncoder;
    private AppTarget? _activeTarget;
    private bool _pipelineActive;
    private System.Threading.Timer? _contentWindowWatcher;
    private readonly AdaptiveBitrateController _adaptiveBitrate;

    // What the Switch reports about the video link (HidPacket.VideoRecoveredPacketCount /
    // VideoLostFrameCount), logged when it changed - the only view this side has of the parity working.
    private static readonly TimeSpan VideoLinkLogInterval = TimeSpan.FromSeconds(10);
    private DateTime _lastVideoLinkLogUtc = DateTime.MinValue;
    private uint _loggedRecoveredPackets;
    private uint _loggedLostFrames;

    // The Switch's screen height (720 handheld / 1080 docked) the stream size follows with
    // Config.AutoResolution. A change must hold for DisplayChangeSettle before the encoder restarts:
    // the dock reports the mode change once, but a console slid in and out quickly shouldn't restart
    // ffmpeg twice.
    private static readonly TimeSpan DisplayChangeSettle = TimeSpan.FromSeconds(1);
    private volatile uint _switchDisplayHeight;
    private uint _reportedDisplayHeight;
    private DateTime _displayReportedUtc;
    private volatile bool _resizePending;

    // Host-side game library (Library/): edited from the UI, mirrored to the Switch by
    // _libraryPusher while armed, launched when the Switch picks an entry (AppTarget.Library).
    private readonly GameLibrary _library = GameLibrary.Load();
    private readonly LibraryPusher _libraryPusher;
    private GameEntry? _libraryEntry;
    private TrackedProcess? _libraryGame;
    private System.Threading.Timer? _libraryWatcher;
    private IntPtr _libraryWindow; // the game's main window as last focused/captured
    private int _libraryDeadTicks;

    // Rapid client reconnects fire SessionStarted (and SessionStopped) repeatedly - confirmed
    // live: a client that connects/disconnects several times in under a second caused 5 Steam
    // relaunches back to back (fixed by gating the Steam-launch part), and separately caused the
    // whole capture/encode pipeline to be torn down and rebuilt from scratch on every single
    // reconnect, with no serialization against OnSessionStopped running concurrently on another
    // thread. This one gate now serializes both handlers against each other, and _pipelineActive
    // skips rebuilding a pipeline that's already up.
    private readonly SemaphoreSlim _pipelineGate = new(1, 1);

    public AppOrchestrator(Config config)
    {
        _config = config;
        _gamepads.SwapAbxy = config.SwitchButtonLayout;
        _session = CreateSession();
        _gamepads.RumbleChanged += OnGameRumble;
        Native.QosMarking.Enabled = config.TrafficPriority;
        _adaptiveBitrate = new AdaptiveBitrateController(config, ApplyAdaptiveBitrate);
        _libraryPusher = new LibraryPusher(config, _library, AdoptSwitchButtonLayout, () => _session as InternetSession, Compatibility);
        Updates = new Update.UpdateChecker(config, Compatibility);
        Updates.Start();
    }

    /// <summary>Newer releases on GitHub - the home screen and the settings show them.</summary>
    public Update.UpdateChecker Updates { get; }

    // The motor levels last sent to the Switch, per player (large, small) - only changes go out.
    private readonly byte[] _rumble = new byte[8];
    private readonly object _rumbleLock = new();

    private void OnGameRumble(int player, byte large, byte small)
    {
        if (player is < 0 or > 3) return;
        if (!_config.Rumble) large = small = 0;
        byte[] levels;
        lock (_rumbleLock)
        {
            if (_rumble[player * 2] == large && _rumble[player * 2 + 1] == small) return;
            _rumble[player * 2] = large;
            _rumble[player * 2 + 1] = small;
            levels = (byte[])_rumble.Clone();
        }
        _session.SendControl(ControlWire.HostMessage.Rumble, levels);
    }

    // The Switch said Hello: from now on it takes control messages - tell it the current options.
    private void OnControlReceived(ControlWire.ClientMessage type, byte[] payload)
    {
        if (type == ControlWire.ClientMessage.Hello) SendOptions();
    }

    private void SendOptions()
    {
        var flags = ControlWire.OptionFlags.None;
        if (_config.TrafficPriority) flags |= ControlWire.OptionFlags.TrafficPriority;
        _session.SendControl(ControlWire.HostMessage.Options, [(byte)flags, 0, 0, 0]);
    }

    /// <summary>The settings' traffic-priority toggle: new sockets (the next encoder start) and the
    /// Switch's input follow it; the running video/audio sockets keep their mark until then.</summary>
    public void TrafficPrioritySettingChanged()
    {
        Native.QosMarking.Enabled = _config.TrafficPriority;
        SendOptions();
    }

    /// <summary>The settings' rumble toggle: turning it off stops whatever is vibrating now.</summary>
    public void RumbleSettingChanged()
    {
        if (_config.Rumble) return;
        lock (_rumbleLock) Array.Clear(_rumble);
        _session.SendControl(ControlWire.HostMessage.Rumble, new byte[8]);
    }

    /// <summary>What the Switch said about its version (library channel, Internet Hello, session
    /// Hello) and how it relates to this PC's - the home screen shows it, a different major protocol
    /// blocks sessions.</summary>
    public SwitchCompatibility Compatibility { get; } = new();

    /// <summary>The host-side game library, for the UI's library screen.</summary>
    public GameLibrary Library => _library;

    /// <summary>Whether the app is currently armed (listening for a client) - driven by the main
    /// window's toggle button, replacing the old "always start on launch" behavior.</summary>
    public bool IsArmed { get; private set; }

    /// <summary>Whether a client is currently connected and the capture/encode pipeline is
    /// active. Raises <see cref="ConnectionChanged"/> whenever it changes.</summary>
    public bool IsConnected => _pipelineActive;

    /// <summary>Fired on the thread pool (not the UI thread) whenever <see cref="IsConnected"/>
    /// changes - subscribers touching WinForms controls must marshal back via Invoke/BeginInvoke.</summary>
    public event Action? ConnectionChanged;

    /// <summary>Which app is currently launched/focused (null if neither has been launched yet
    /// this run) - lets MainForm show the user which mode is actually active. Set right before
    /// <see cref="ConnectionChanged"/> fires in OnSessionStarted, so it's always current by the
    /// time subscribers read it from that event.</summary>
    public AppTarget? ActiveTarget => _activeTarget;

    /// <summary>Fired on the thread pool when the Switch asked for shadPS4 but
    /// <see cref="Config.ShadPs4ExePath"/> is empty or points at a missing file - MainForm
    /// subscribes to prompt the user to configure it (and optionally open the download page),
    /// since AppOrchestrator itself has no UI to show one.</summary>
    public event Action? ShadPs4NotConfigured;

    /// <summary>A/B X/Y by label (true) or by position. Takes effect on the very next gamepad
    /// packet, no reconnect needed. Settable here (the PC settings) and on the Switch - see
    /// <see cref="SetSwapAbxy"/> and <see cref="AdoptSwitchButtonLayout"/>.</summary>
    public bool SwapAbxy => _gamepads.SwapAbxy;

    /// <summary>Fired on the thread pool when the Switch's newer layout replaced the PC's, so the
    /// settings screen can show it.</summary>
    public event Action? ButtonLayoutChanged;

    private readonly object _layoutLock = new();

    /// <summary>A change made on the PC: saved with a bumped revision, so it beats the Switch's
    /// current value on the next library-channel sync.</summary>
    public void SetSwapAbxy(bool value)
    {
        lock (_layoutLock)
        {
            _config.SwitchButtonLayout = value;
            _config.ButtonLayoutRevision++;
            _config.Save();
            _gamepads.SwapAbxy = value;
        }
    }

    // The Switch's layout from the library channel: adopted only if it was changed there more
    // recently (higher revision). On a tie the PC's value stands - the Switch adopts it instead.
    private void AdoptSwitchButtonLayout(bool swapAbxy, uint revision)
    {
        lock (_layoutLock)
        {
            if (revision <= _config.ButtonLayoutRevision) return;
            _config.SwitchButtonLayout = swapAbxy;
            _config.ButtonLayoutRevision = revision;
            _config.Save();
            _gamepads.SwapAbxy = swapAbxy;
        }
        Logger.Log("AppOrchestrator", $"button layout changed on the Switch: {(swapAbxy ? "by label" : "by position")} (revision {revision})");
        ButtonLayoutChanged?.Invoke();
    }

    /// <summary>Arms the app - starts listening for the client to connect. Actually launching Big
    /// Picture/starting the capture pipeline still only happens once a client really connects (see
    /// OnSessionStarted) - this alone doesn't touch Steam or anything else yet.</summary>
    public void Arm()
    {
        if (IsArmed) return;
        IsArmed = true;
        // The connection mode may have changed in the settings since the last time.
        var wantInternet = _config.ConnectionMode == ConnectionMode.Internet;
        if (wantInternet != _session is InternetSession)
        {
            _session.DisposeAsync().AsTask().GetAwaiter().GetResult();
            _session = CreateSession();
        }
        _session.Start();
        _libraryPusher.Start();
    }

    /// <summary>The running Internet-mode session (null in Wi-Fi mode or while not armed) - the
    /// settings' pairing window works on it.</summary>
    public InternetSession? Internet => IsArmed ? _session as InternetSession : null;

    private ISessionController CreateSession()
    {
        ISessionController session = _config.ConnectionMode == ConnectionMode.Internet
            ? new InternetSession(_config, Compatibility)
            : new SessionController(_config, Compatibility);
        session.SessionStarted += OnSessionStarted;
        session.SessionStopped += OnSessionStopped;
        session.PacketReceived += OnPacketReceived;
        session.ControlReceived += OnControlReceived;
        return session;
    }

    /// <summary>Disarms the app - stops listening entirely. If a client was actually connected,
    /// this also triggers the normal disconnect cleanup (close the game, exit Big Picture, stop
    /// encoders - see OnSessionStopped) since SessionController's own loop observes the
    /// cancellation and fires SessionStopped on its way out.</summary>
    public void Disarm()
    {
        if (!IsArmed) return;
        IsArmed = false;
        _session.Stop();
        _libraryPusher.Stop();
    }

    [SupportedOSPlatform("windows8.0")]
    /// <summary>Stream size for the Switch's current screen - see Config.AutoResolution. A client
    /// that doesn't report its screen (0) gets the configured size.</summary>
    private (int Width, int Height) CurrentStreamSize()
    {
        var height = _switchDisplayHeight;
        if (!_config.AutoResolution || height == 0) return (_config.TargetWidth, _config.TargetHeight);
        return height >= 1080 ? (1920, 1080) : (1280, 720);
    }

    // Network thread, every packet: notices the Switch going in or out of the dock and, once the new
    // mode has held for DisplayChangeSettle, restarts the encoder at the matching size.
    private void FollowSwitchDisplay(HidPacket packet)
    {
        var now = DateTime.UtcNow;
        if (packet.DisplayHeight != _reportedDisplayHeight)
        {
            _reportedDisplayHeight = packet.DisplayHeight;
            _displayReportedUtc = now;
            return;
        }
        if (packet.DisplayHeight == _switchDisplayHeight || now - _displayReportedUtc < DisplayChangeSettle || _resizePending) return;

        _switchDisplayHeight = packet.DisplayHeight;
        if (!_config.AutoResolution) return;
        _resizePending = true;
        _ = Task.Run(async () =>
        {
            await _pipelineGate.WaitAsync();
            try
            {
                var size = CurrentStreamSize();
                if (_pipelineActive && _videoEncoder is { ActiveSize: { } active } && active != size)
                    RestartVideoEncoder($"Switch screen is now {packet.DisplayHeight}p -> {size.Width}x{size.Height}");
            }
            finally
            {
                _resizePending = false;
                _pipelineGate.Release();
            }
        });
    }

    private async void OnSessionStarted(HidPacket firstPacket)
    {
        _switchDisplayHeight = _reportedDisplayHeight = firstPacket.DisplayHeight;
        _displayReportedUtc = DateTime.UtcNow;
        Logger.Log("AppOrchestrator", "OnSessionStarted: entered, waiting for pipeline gate");
        await _pipelineGate.WaitAsync();
        Logger.Log("AppOrchestrator", "OnSessionStarted: gate acquired");
        try
        {
            var target = firstPacket.AppTarget;
            IntPtr hwnd;

            // shadPS4 gets a virtual DualShock4 instead of Xbox360 - the only path that can carry
            // real gyro/accelerometer data through to the game (see VirtualGamepadHub's class doc
            // comment). Steam/Big Picture keeps Xbox360.
            //
            // History: (1) gamepad silently didn't respond in shadPS4 at all; (2) a standalone test
            // creating a ViGEm DS4 and reading it back via Windows.Gaming.Input.RawGameController
            // confirmed the raw report's button/axis/dpad encoding is correct at the Windows HID
            // level, ruling out a bit-layout bug; (3) with DS4 mode re-enabled, live testing showed
            // stutter/artifacts in the video feed, only in shadPS4 sessions, correlated with
            // ffmpeg's video mux thread dying on WSAENOBUFS (-10055, UDP send buffer exhaustion) -
            // i.e. the video breakage was a downstream symptom of system-wide contention, not
            // shadPS4-specific rendering behavior. Leading suspect: shadPS4 sending DS4 output
            // reports (rumble/lightbar color) that nothing was consuming, backing up the report
            // queue - VirtualGamepadHub.PlugInDs4/DrainDs4OutputReports now actively drains it via
            // AwaitRawOutputReport. If WSAENOBUFS/artifacts recur even with that fix, the next
            // suspect is genuine network congestion unrelated to controller mode, not this report
            // queue.
            // Library entries pick their own controller type (GameEntry.Controller) below.
            if (target != AppTarget.Library)
                _gamepads.Mode = target == AppTarget.BigPicture ? ControllerMode.Xbox360 : ControllerMode.DualShock4;

            if (target == AppTarget.Library)
            {
                var entry = _library.Find(firstPacket.LibraryEntryId);
                if (entry is null)
                {
                    // E.g. deleted on the PC after the Switch cached the list - ending the session
                    // sends the Switch back to its menu, where the refreshed list arrives.
                    Logger.Log("AppOrchestrator", $"OnSessionStarted: library entry {firstPacket.LibraryEntryId} not found - disconnecting");
                    _session.ForceDisconnect();
                    return;
                }

                _gamepads.Mode = entry.Controller;
                Logger.Log("AppOrchestrator", $"OnSessionStarted: launching library game '{entry.Name}' ({entry.Type})");
                var game = await GameLauncher.LaunchAsync(entry, _launcher, CancellationToken.None);
                if (game is null)
                {
                    Logger.Log("AppOrchestrator", $"OnSessionStarted: '{entry.Name}' could not be started - disconnecting");
                    _session.ForceDisconnect();
                    return;
                }

                _libraryEntry = entry;
                _libraryGame = game;
                _libraryWindow = IntPtr.Zero;
                _libraryDeadTicks = 0;
                _activeTarget = AppTarget.Library;
                // No window wait: capture is the whole monitor anyway, and some games take far longer
                // than any sensible timeout to show a window (launchers, updates, shader caches). The
                // library watcher hands the game focus once its window exists.
                hwnd = IntPtr.Zero;
            }
            else if (target == AppTarget.BigPicture)
            {
                if (_activeTarget != AppTarget.BigPicture)
                {
                    Logger.Log("AppOrchestrator", "OnSessionStarted: launching Steam");
                    await _launcher.LaunchSteamAsync(CancellationToken.None);
                    Logger.Log("AppOrchestrator", "OnSessionStarted: LaunchSteamAsync returned");
                }
                else
                {
                    // Steam's still running from a previous session but Big Picture was exited on
                    // disconnect (see OnSessionStopped) - just re-open it instead of a full relaunch.
                    _launcher.OpenBigPicture();
                }
                _activeTarget = AppTarget.BigPicture;

                Logger.Log("AppOrchestrator", "OnSessionStarted: waiting for Big Picture window");
                hwnd = await _launcher.WaitForWindowAsync(TimeSpan.FromSeconds(30), CancellationToken.None);
            }
            else
            {
                if (_activeTarget != AppTarget.ShadPs4)
                {
                    // Launches straight into shadPS4's own --big-picture mode (its native,
                    // gamepad-navigable game list/UI) instead of a specific title picked via our own
                    // Switch-side picker screen - see ShadPs4Launcher.LaunchOrFocusAsync's doc
                    // comment. Passing null always takes the --big-picture branch; the
                    // titleId-driven direct launch is kept in the launcher only.
                    Logger.Log("AppOrchestrator", "OnSessionStarted: launching shadPS4 (--big-picture)");
                    var launched = await _shadLauncher.LaunchOrFocusAsync(_config.ShadPs4ExePath, titleId: null, CancellationToken.None);
                    if (!launched)
                    {
                        Logger.Log("AppOrchestrator", "OnSessionStarted: shadPS4 not configured/found");
                        ShadPs4NotConfigured?.Invoke();
                        return;
                    }
                }
                else
                {
                    _shadLauncher.Focus(_config.ShadPs4ExePath);
                }
                _activeTarget = AppTarget.ShadPs4;

                Logger.Log("AppOrchestrator", "OnSessionStarted: waiting for shadPS4 window");
                hwnd = await _shadLauncher.WaitForWindowAsync(_config.ShadPs4ExePath, TimeSpan.FromSeconds(30), CancellationToken.None);
            }

            Logger.Log("AppOrchestrator", $"OnSessionStarted: WaitForWindowAsync returned hwnd={hwnd}");

            if (hwnd == IntPtr.Zero && target != AppTarget.Library) return; // window never appeared; nothing to capture

            if (_pipelineActive)
            {
                // A rapid reconnect fired SessionStarted again while the previous session's
                // pipeline is still up (its matching SessionStopped hasn't arrived yet, or already
                // came and went without us tearing anything down because it raced this call) -
                // nothing new to do, the existing capture/encoders are still perfectly valid.
                return;
            }

            // See TimerResolution's doc comment - requested for the whole session, not just while
            // audio is active, since video capture/encode competing for the same CPU quanta is part
            // of the same scheduling-jitter problem.
            TimerResolution.RequestHighResolution();

            // Audio capture/resample/send now all run as managed threads inside this process (see
            // AudioCaptureSender) rather than a separate ffmpeg process - MMCSS on those specific
            // threads (see Mmcss.Register) handles their own scheduling, but this process's overall
            // priority still affects how promptly the CLR schedules everything else they depend on
            // (GC, thread pool). A modest process-wide bump complements the per-thread MMCSS boost
            // rather than replacing it.
            try
            {
                Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.AboveNormal;
            }
            catch (Exception ex)
            {
                Logger.Log("AppOrchestrator", $"failed to raise process priority: {ex}");
            }

            _videoEncoder = new VideoEncoder(_config);
            _videoEncoder.Start(streamSize: CurrentStreamSize());
            _adaptiveBitrate.Start(_config.BitrateKbps);

            // Captures whatever is currently playing on the first user's real default audio
            // device directly via WASAPI loopback - see AudioCaptureSender.
            RestartAudioEncoder("session started");

            _pipelineActive = true;
            ConnectionChanged?.Invoke();

            // This watcher's whole job is working around Steam Input's own focus/rebinding
            // quirks (see the class doc comment and OnContentWindowWatcherTick) - none
            // of that is known to apply to shadPS4's plain single-window process, so it's only
            // started for the Big Picture target. See the "biggest open risk" note in the plan:
            // whether shadPS4 keeps taking gamepad input while unfocused is unverified.
            if (target == AppTarget.BigPicture)
            {
                _contentWindowWatcher = new System.Threading.Timer(
                    _ => OnContentWindowWatcherTick(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
            }
            else if (target == AppTarget.Library)
            {
                _libraryWatcher = new System.Threading.Timer(
                    _ => OnLibraryWatcherTick(), null, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(1));
            }
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"session start failed: {ex}");
        }
        finally
        {
            _pipelineGate.Release();
        }
    }

    /// <summary>Stops and recreates the audio capture/stream from scratch - a fresh WASAPI capture
    /// and a fresh TCP connection to the client, both of which the client already has to cope with
    /// on the very first connect, so this needs no special handling on that side. Caller must
    /// already hold <see cref="_pipelineGate"/>. Audio failing to (re)start must never take down
    /// video/gamepad, so failures are caught and logged here rather than propagated.</summary>
    private void RestartAudioEncoder(string reason)
    {
        try
        {
            _audioEncoder?.Stop();
            _audioEncoder = new AudioCaptureSender(_config);
            _audioEncoder.Start();
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"audio capture (re)start failed ({reason}): {ex}");
        }
    }

    /// <summary>Kills and restarts the ffmpeg video process from scratch - see
    /// VideoEncoder.IsStalled's doc comment for why this exists (a hung Windows.Graphics.Capture
    /// source that nothing else notices or recovers from), and AdaptiveBitrateController's class
    /// doc comment for the other caller (a deliberate bitrate change - VideoEncoder has no live
    /// reconfigure, so a change means killing and relaunching the process with new args). Caller
    /// must already hold <see cref="_pipelineGate"/>. Mirrors RestartAudioEncoder: failures here
    /// must never take down audio/gamepad, so they're caught and logged rather than propagated.</summary>
    /// <param name="bitrateKbpsOverride">Explicit target for an adaptive-bitrate-driven restart -
    /// null (the watchdog's case) carries over whatever bitrate the outgoing encoder was actually
    /// running at, so a stall-triggered restart doesn't silently undo an adaptive change by jumping
    /// back to Config.BitrateKbps (the ceiling).</param>
    private void RestartVideoEncoder(string reason, int? bitrateKbpsOverride = null, IntPtr? captureWindowOverride = null)
    {
        try
        {
            Logger.Log("AppOrchestrator", $"restarting video encoder ({reason})");
            var carryOverBitrate = bitrateKbpsOverride ?? _videoEncoder?.ActiveBitrateKbps;
            var carryOverWindow = captureWindowOverride ?? _videoEncoder?.CaptureWindow ?? IntPtr.Zero;
            _videoEncoder?.Stop();
            _videoEncoder = new VideoEncoder(_config);
            _videoEncoder.Start(bitrateKbpsOverride: carryOverBitrate, captureWindow: carryOverWindow, streamSize: CurrentStreamSize());
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"video encoder restart failed ({reason}): {ex}");
        }
    }

    /// <summary>AdaptiveBitrateController's apply callback - runs on its own Timer thread, not
    /// holding <see cref="_pipelineGate"/>, so this acquires it itself (mirrors
    /// ScheduleDelayedAudioRestart) and bails out quietly if the session ended in the meantime.</summary>
    private void ApplyAdaptiveBitrate(int bitrateKbps)
    {
        _ = Task.Run(async () =>
        {
            await _pipelineGate.WaitAsync();
            try
            {
                if (!_pipelineActive) return;
                RestartVideoEncoder($"adaptive bitrate -> {bitrateKbps} kbps", bitrateKbps);
            }
            finally
            {
                _pipelineGate.Release();
            }
        });
    }

    /// <summary>Runs RestartAudioEncoder a few seconds from now instead of immediately - see the
    /// "game launched" call site for why. Re-acquires <see cref="_pipelineGate"/> itself (the
    /// delay means this can't stay under the caller's own acquisition) and bails out quietly if
    /// the session already ended during the wait.</summary>
    private void ScheduleDelayedAudioRestart(string reason)
    {
        _ = Task.Run(async () =>
        {
            await Task.Delay(TimeSpan.FromSeconds(3));
            await _pipelineGate.WaitAsync();
            try
            {
                if (!_pipelineActive) return;
                RestartAudioEncoder(reason);
            }
            finally
            {
                _pipelineGate.Release();
            }
        });
    }

    /// <summary>
    /// Fully closes and reopens Big Picture after a game closes, instead of trying to coax Steam
    /// Input into handing the controller binding back to Big Picture's own context on its own.
    /// Confirmed live (via Steam's own C:\steam\logs\controller_ui.txt) that it doesn't: after a
    /// non-Steam-game shortcut (e.g. "Luna Abyss") closes, Steam keeps reloading that game's own
    /// controller config seconds after correctly switching to Big Picture's - neither a forced
    /// gamepad replug (removed, confirmed live to make no difference) nor a synthetic Escape
    /// keypress (SendBackNavigation, kept, also confirmed live not to fully fix it alone) stopped
    /// this. A fresh Big Picture launch is the one thing already known to produce a working,
    /// responsive Big Picture - every session's very first connect goes through exactly this path
    /// (LaunchSteamAsync/OpenBigPicture + WaitForWindowAsync) - so this trades a visible reload
    /// flicker for gamepad control that actually works, rather than a subtler in-place fix that
    /// repeatedly hasn't. Reopening is sequenced after the old window is actually gone (see the
    /// body) - reopening immediately was measured to hang steamwebhelper.
    ///
    /// Runs as a background task (mirrors ScheduleDelayedAudioRestart) since the content-window
    /// watcher tick that triggers this must not block on a multi-second close/reopen/wait sequence.
    /// Deliberately does not hold _pipelineGate for the whole operation - only _launcher state is
    /// touched, and OnSessionStopped's own ExitBigPicture/CloseActiveGame calls are idempotent-ish
    /// enough (matches WaitForWindowAsync's own periodic re-fire-OpenBigPicture pattern) that a
    /// benign race with a concurrent disconnect isn't worth serializing against.
    /// </summary>
    private void RestartBigPictureAfterGameClosed()
    {
        _restartingBigPicture = true;
        _ = Task.Run(async () =>
        {
            try
            {
                // Measured live (SolarNX.log, two game exits in a row, 13:09 and 13:14): Escape,
                // WM_CLOSE and steam://open/bigpicture all landing within the same ~2ms - i.e.
                // asking Steam to reopen Big Picture while its webview was still tearing down -
                // hung steamwebhelper for ~18s every time, until Steam gave up and showed its own
                // "steamwebhelper is not responding" dialog. So: let the Escape (sent by the
                // caller just before this) be handled first, close, wait until the old window is
                // genuinely gone, settle briefly, and only then reopen.
                await Task.Delay(TimeSpan.FromMilliseconds(500));

                var closingHwnd = _launcher.ExitBigPicture();
                var closed = await SteamLauncher.WaitForWindowClosedAsync(closingHwnd, TimeSpan.FromSeconds(15), CancellationToken.None);
                Logger.Log("AppOrchestrator", closed
                    ? "RestartBigPictureAfterGameClosed: old Big Picture window gone"
                    : "RestartBigPictureAfterGameClosed: old Big Picture window still present after 15s - reopening anyway");
                await Task.Delay(TimeSpan.FromSeconds(1));

                if (!_pipelineActive || _activeTarget != AppTarget.BigPicture) return;
                _launcher.OpenBigPicture();

                var hwnd = await _launcher.WaitForWindowAsync(TimeSpan.FromSeconds(30), CancellationToken.None);
                Logger.Log("AppOrchestrator", $"RestartBigPictureAfterGameClosed: reopened, hwnd={hwnd}");
            }
            catch (Exception ex)
            {
                Logger.Log("AppOrchestrator", $"RestartBigPictureAfterGameClosed failed: {ex}");
            }
            finally
            {
                _restartingBigPicture = false;
            }
        });
    }

    // Set for the duration of RestartBigPictureAfterGameClosed - tells the null-content-window
    // handling below that Big Picture's window disappearing right now is our own deliberate doing,
    // not the first user closing it, so it must not count towards ForceDisconnect's tolerance.
    private volatile bool _restartingBigPicture;

    /// <summary>
    /// Watcher tick (1s, Big Picture sessions only): video stall watchdog, game launch/exit
    /// detection, and the Big-Picture-window-gone check. Capture itself never needs to change here
    /// - ffmpeg's gfxcapture reads the whole primary monitor's output regardless of which window is
    /// currently showing there.
    ///
    /// No focus spoofing any more: focushook.dll (injected into steam.exe/steamwebhelper.exe to
    /// make Big Picture always believe it was the foreground window) was removed after a live A/B
    /// test - with it off and Steam restarted, gamepad control returned to Big Picture after a game
    /// exactly as when Big Picture is opened by hand, and Steam's Resume/Stop buttons stopped going
    /// grey. The spoof was what kept Steam from seeing the real focus changes it relies on.
    /// </summary>
    // Tracks whether the last watcher tick's active content window belonged to a game (true) or
    // Big Picture itself (false) - null means unknown/not yet observed. Used to detect the
    // game-just-closed transition below.
    private bool? _lastActiveWasGame;

    // The game process the Switch player is currently in, captured when its window first becomes
    // the active content window, cleared only once that process has actually exited - see the
    // "game is running" comment in OnContentWindowWatcherTick for why window visibility
    // alone isn't trusted. _gameWasRunning is the previous tick's verdict, for edge detection.
    private TrackedProcess? _trackedGame;
    private bool _gameWasRunning;

    // See the isGame-is-null handling below: debounces a transient non-Steam-family window (a
    // third-party game launcher) covering Big Picture against being mistaken for it actually
    // closing.
    private const int NullContentWindowTicksBeforeDisconnect = 5;
    private int _consecutiveNullContentWindowTicks;
    private bool _contentWindowWasKnownBeforeNullStreak;

    private void OnContentWindowWatcherTick()
    {
        if (!_pipelineGate.Wait(0)) return; // a start/stop is in progress this tick; try again next tick
        try
        {
            if (!_pipelineActive) return;

            // See VideoEncoder.IsStalled's doc comment - this is the only thing in the whole
            // pipeline that ever notices a hung capture and does something about it. 5s: long
            // enough that ffmpeg's own periodic stderr progress cadence never false-positives,
            // short enough to cut a real hang well before it's this noticeable again.
            if (_videoEncoder is { } video && video.IsStalled(TimeSpan.FromSeconds(5)))
            {
                RestartVideoEncoder("no encode progress for 5s");
            }

            var isGame = _launcher.FindActiveContentWindow();

            // "Is a game running" is decided by the game's *process*, not by which window is
            // currently the biggest visible one. Confirmed live (SolarNX.log, Windblown): clicking
            // into Explorer minimized the fullscreen game, its window dropped below the content
            // size floor, Big Picture's window won instead - and that was taken as "game closed":
            // Big Picture got closed and reopened on top of the still-running game (Steam's
            // Resume/Stop buttons then went grey, out of sync with the game), and ten seconds later
            // the same game window came back as "game launched", stealing real focus back. Three
            // false close/launch cycles in ninety seconds.
            if (isGame == true && _launcher.LastActiveContentProcessId is var pid and not 0 && _trackedGame?.Pid != pid)
            {
                _trackedGame = TrackedProcess.Capture(pid);
                Logger.Log("AppOrchestrator", $"tracking game process pid={pid}");
            }
            if (_trackedGame is { } tracked && !tracked.IsAlive())
            {
                Logger.Log("AppOrchestrator", $"game process pid={tracked.Pid} exited");
                _trackedGame = null;
            }
            var gameRunning = isGame == true || _trackedGame is not null;


            // After closing a game launched from Big Picture, gamepad control can stay completely
            // dead in Big Picture itself even though the focus hook keeps correctly targeting its
            // window every tick. FullReplug (a forced ViGEm disconnect/reconnect cycle) used to run
            // here as an attempted fix - removed after live testing via Steam's own
            // controller_ui.txt proved it had no effect: Steam Input re-warmed and correctly
            // switched the active binding back to Big Picture's own appid (769) with or without a
            // replug, then independently reloaded the just-closed game's own config seconds later
            // regardless. That reload is Steam's own behavior, not a device-presence/binding-handoff
            // problem a replug could ever address - see SendBackNavigation's doc comment for the
            // current best lead (the game's own Steam Input config having no navigation-compatible
            // action set) and CloseActiveGame in SteamLauncher.cs's comment history for other things
            // already ruled out.
            if (_gameWasRunning && !gameRunning)
            {
                RestartAudioEncoder("game closed");

                // Steam returns to Big Picture on its own after a game exits - by default that's
                // all that happens now. The Escape + full Big Picture close/reopen below is the old
                // workaround for a dead gamepad after a non-Steam-game shortcut closes, kept behind
                // Config.RestartBigPictureAfterGame (off) in case that problem resurfaces. Escape
                // (SendBackNavigation) backs Big Picture off the just-closed game's own page, where
                // Steam Input loads that game's controller config instead of Big Picture's
                // navigation config - confirmed live NOT to fully fix that on its own.
                if (_config.RestartBigPictureAfterGame)
                {
                    Logger.Log("AppOrchestrator", "game closed - backing off the game's own page and restarting Big Picture (RestartBigPictureAfterGame)");
                    SteamLauncher.SendBackNavigation();
                    RestartBigPictureAfterGameClosed();
                }
                else
                {
                    Logger.Log("AppOrchestrator", "game closed - leaving the return to Big Picture to Steam");
                }
            }

            // A game briefly grabbing/reconfiguring the shared audio endpoint right as it starts can
            // leave audio crackling for the rest of the session if the capture restart lands in the
            // middle of that disturbance - delaying the automatic restart lets it settle first.
            //
            // ForceForegroundWindow: real OS foreground doesn't necessarily move to a freshly
            // launched game window on its own (focus-stealing prevention), and Steam Input's
            // per-context controller binding keys off real focus. Never called per tick - that
            // would steal focus from the first user. Safe here specifically because it fires exactly once, at a confirmed
            // transition, targeting the exact just-detected game hwnd - once per game process now,
            // not every time a minimized game window comes back (see _trackedGame).
            if (!_gameWasRunning && gameRunning)
            {
                Logger.Log("AppOrchestrator", "game launched - forcing real foreground focus and scheduling audio capture restart after a settle delay");
                SteamLauncher.ForceForegroundWindow(_launcher.LastActiveContentWindow);
                ScheduleDelayedAudioRestart("game launched");
            }

            // isGame is null when FindActiveContentWindow found no Steam-family
            // content window at all - neither Big Picture nor a game. Since this watcher only ever
            // runs while _pipelineActive is true, which only becomes true after Big Picture's
            // window was already found once (see OnSessionStarted), a null result here always
            // means it disappeared *during* an active session - the first user closed it (or it
            // crashed) rather than us tearing it down ourselves (which only happens after a
            // disconnect, at which point this watcher has already been disposed).
            //
            // Not necessarily true, though: a game launched through a third-party launcher (Ubisoft
            // Connect, EA app, the game's own separate launcher window, etc.) shows a window that
            // isn't Steam-family at all (it isn't steam.exe/steamwebhelper.exe and typically isn't
            // installed under the Steam directory this check relies on - see IsSteamFamilyProcess),
            // so it can briefly cover/replace Big Picture's window while the launcher does its own
            // thing before the actual game starts. A single null tick here used to force-disconnect
            // the client immediately on exactly that transient case. Requiring several consecutive
            // null ticks before concluding Big Picture is *really* gone tolerates a launcher window
            // sitting there for a few seconds without kicking the client, while still catching a
            // genuine close/crash (which stays null) shortly after.
            if (isGame is null)
            {
                if (_restartingBigPicture)
                {
                    // We're the ones who made Big Picture's window disappear right now (see
                    // RestartBigPictureAfterGameClosed) - don't let the transient-launcher-window
                    // tolerance below mistake our own deliberate close/reopen for the first user
                    // having closed Big Picture themselves.
                    _consecutiveNullContentWindowTicks = 0;
                }
                else
                {
                    if (_consecutiveNullContentWindowTicks == 0)
                    {
                        // Capture this before _lastActiveWasGame gets overwritten to null below - by
                        // the time the tick threshold is reached a few seconds from now,
                        // _lastActiveWasGame would already read null regardless of what it was when
                        // this streak of null ticks actually started.
                        _contentWindowWasKnownBeforeNullStreak = _lastActiveWasGame is not null;
                    }
                    _consecutiveNullContentWindowTicks++;
                    if (_consecutiveNullContentWindowTicks == NullContentWindowTicksBeforeDisconnect && _contentWindowWasKnownBeforeNullStreak)
                    {
                        Logger.Log("AppOrchestrator", "Big Picture window gone for several seconds - forcing client disconnect");
                        _session.ForceDisconnect();
                    }
                }
            }
            else
            {
                _consecutiveNullContentWindowTicks = 0;
            }
            _lastActiveWasGame = isGame;
            _gameWasRunning = gameRunning;
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"content window focus-hook injection failed: {ex}");
        }
        finally
        {
            _pipelineGate.Release();
        }
    }

    // A launcher can exit a moment before the game it starts appears as its child, so a single
    // "nothing alive" tick isn't trusted.
    private const int LibraryDeadTicksBeforeExit = 3;

    /// <summary>
    /// Library sessions' 1s watcher: video stall watchdog (the Big Picture watcher's equivalent),
    /// one-shot real focus for the game's window once it exists, and session end once the game's
    /// process family is gone. Ending the session is how the Switch learns the game is over: the HID
    /// connection drops, and for a library target the client goes back to its menu by itself.
    /// </summary>
    private void OnLibraryWatcherTick()
    {
        if (!_pipelineGate.Wait(0)) return;
        try
        {
            if (!_pipelineActive || _libraryGame is not { } game) return;

            // A minimized captured window legitimately delivers no frames - not a hung encoder.
            if (_videoEncoder is { } video && video.IsStalled(TimeSpan.FromSeconds(5)) && !GameLauncher.IsMinimized(video.CaptureWindow))
                RestartVideoEncoder("no encode progress for 5s");

            if (!game.IsAlive())
            {
                if (++_libraryDeadTicks < LibraryDeadTicksBeforeExit) return;
                Logger.Log("AppOrchestrator", $"library game '{_libraryEntry?.Name}' exited - ending the session");
                _libraryGame = null;
                _session.ForceDisconnect();
                return;
            }
            _libraryDeadTicks = 0;

            // Follow the game's main window as it changes, not just the first one: confirmed live,
            // a launcher's splash (LunaAbyss.exe) got the one-shot focus, the real game window
            // (LunaAbyss-Win64-Shipping.exe) opened seconds later without it, and the gamepad went
            // nowhere. Each change of main window: real focus once (never per tick - that would steal
            // focus from the first user), and the capture is pointed at the new window.
            var hwnd = GameLauncher.FindMainWindow(game);

            // Same window, new size (a launcher-sized window growing into the game, a resolution
            // change): the streamed fit was computed for the old size - restart with a new one so the
            // picture stays centred and touches stay aligned. Minimized windows report a bogus size;
            // they're left alone.
            if (hwnd != IntPtr.Zero && hwnd == _libraryWindow && _libraryEntry?.CaptureGameWindow == true
                && _videoEncoder is { } enc && enc.CaptureWindow == hwnd && !GameLauncher.IsMinimized(hwnd)
                && WindowGeometry.TryGetClientSize(hwnd, out var w, out var h) && (w, h) != enc.CaptureSize)
            {
                RestartVideoEncoder($"game window resized to {w}x{h}", captureWindowOverride: hwnd);
            }

            if (hwnd != IntPtr.Zero && hwnd != _libraryWindow)
            {
                var first = _libraryWindow == IntPtr.Zero;
                _libraryWindow = hwnd;
                Logger.Log("AppOrchestrator", $"library game main window is now hwnd={hwnd} - giving it focus");
                SteamLauncher.ForceForegroundWindow(hwnd);

                if (_libraryEntry?.CaptureGameWindow == true)
                {
                    _touch.TargetWindow = hwnd;
                    RestartVideoEncoder("capturing the game window", captureWindowOverride: hwnd);
                }
                if (first) ScheduleDelayedAudioRestart("library game launched");
            }
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"library watcher tick failed: {ex}");
        }
        finally
        {
            _pipelineGate.Release();
        }
    }

    private void OnSessionStopped()
    {
        _pipelineGate.Wait();
        try
        {
            _contentWindowWatcher?.Dispose();
            _contentWindowWatcher = null;
            _libraryWatcher?.Dispose();
            _libraryWatcher = null;

            _adaptiveBitrate.Stop();
            _videoEncoder?.Stop();
            _audioEncoder?.Stop();
            _gamepads.DisconnectAll();
            TimerResolution.Release();
            try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.Normal; } catch { /* best-effort */ }

            _videoEncoder = null;
            _audioEncoder = null;
            _pipelineActive = false;
            _lastActiveWasGame = null;
            _trackedGame = null;
            _gameWasRunning = false;
            ConnectionChanged?.Invoke();

            // Nothing is hidden anymore - Big Picture/shadPS4 (or a game) sitting idle on the real
            // screen once nobody is connected would be a visible nuisance to the first user. Steam
            // itself stays running (see LaunchSteamAsync) so the next connect can jump straight
            // back in - shadPS4 has no equivalent persistent background process, so its window is
            // simply closed outright (see ShadPs4Launcher.Close).
            if (_activeTarget == AppTarget.BigPicture)
            {
                _launcher.CloseActiveGame();
                _launcher.ExitBigPicture();
            }
            else if (_activeTarget == AppTarget.Library)
            {
                // Same "the player left, close what they were playing" treatment as Big Picture games.
                // Steam games are closed through Steam's own process family as well, in case the
                // game relaunched itself outside the tracked tree.
                if (_libraryEntry?.Type == GameEntryType.Steam) _launcher.CloseActiveGame();
                _libraryGame?.CloseFamily(TimeSpan.FromSeconds(3));
                _libraryGame = null;
                _libraryEntry = null;
                _libraryWindow = IntPtr.Zero;
                _touch.TargetWindow = IntPtr.Zero;
                _activeTarget = null;
            }
            else if (_activeTarget == AppTarget.ShadPs4)
            {
                _shadLauncher.Close(_config.ShadPs4ExePath);
                // Unlike Big Picture (Steam itself keeps running, so _activeTarget staying
                // BigPicture correctly means "just OpenBigPicture(), don't relaunch Steam" on the
                // next connect), shadPS4 has no persistent background process - it's now fully
                // closed. Leaving _activeTarget at ShadPs4 here made the next OnSessionStarted take
                // the "already active" branch and just Focus() a window that no longer exists,
                // which silently does nothing and left WaitForWindowAsync timing out 30s later
                // without ever actually relaunching shadPS4 - confirmed live via SolarNX.log.
                _activeTarget = null;
            }
        }
        finally
        {
            _pipelineGate.Release();
        }
    }

    private void LogVideoLinkIfDue(HidPacket packet)
    {
        var now = DateTime.UtcNow;
        if (now - _lastVideoLinkLogUtc < VideoLinkLogInterval) return;
        var firstPacket = _lastVideoLinkLogUtc == DateTime.MinValue;
        _lastVideoLinkLogUtc = now;
        var recovered = packet.VideoRecoveredPacketCount - _loggedRecoveredPackets;
        var lost = packet.VideoLostFrameCount - _loggedLostFrames;
        _loggedRecoveredPackets = packet.VideoRecoveredPacketCount;
        _loggedLostFrames = packet.VideoLostFrameCount;
        if (firstPacket || (recovered == 0 && lost == 0)) return;
        Logger.Log("VideoLink", $"Switch, last {VideoLinkLogInterval.TotalSeconds:0}s: {recovered} datagrams rebuilt from parity, {lost} frames lost (held until a keyframe)");
    }

    private void OnPacketReceived(HidPacket packet)
    {
        try
        {
            _gamepads.Apply(packet);
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"gamepad apply failed: {ex}");
        }

        try
        {
            _touch.Update(packet.TouchX1, packet.TouchY1);
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"touch inject failed: {ex}");
        }

        LogVideoLinkIfDue(packet);
        FollowSwitchDisplay(packet);

        try
        {
            _adaptiveBitrate.OnPacketReceived(packet);
        }
        catch (Exception ex)
        {
            Logger.Log("AppOrchestrator", $"adaptive bitrate telemetry update failed: {ex}");
        }
    }

    public void Dispose()
    {
        IsArmed = false;
        _session.DisposeAsync().AsTask().GetAwaiter().GetResult();
        OnSessionStopped();
        _gamepads.Dispose();
        _touch.Dispose();
        _adaptiveBitrate.Dispose();
        _libraryPusher.Dispose();
        Updates.Dispose();
        _pipelineGate.Dispose();
    }
}
