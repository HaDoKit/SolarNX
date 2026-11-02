# Architecture conventions

## Composition root: `AppOrchestrator`

`Program.cs` is bootstrap only: `--protocol-stub` early exit (Game Bar stub, see
`Input/GameBarSuppressor.cs`), single-instance mutex, `Config.Load()`, `AppOrchestrator`, WinForms
`NotifyIcon` tray, WPF `App` + `MainWindow` (`ShutdownMode=OnExplicitShutdown`; closing the window
hides it to tray unless `AllowClose`). `app.Exit` disposes the orchestrator.

`AppOrchestrator` constructs and owns the services and wires them:

- `Arm()`/`Disarm()` (main window toggle) start/stop `SessionController`.
- `OnSessionStarted` (`async void`, serialized by `_pipelineGate`): picks controller mode from
  `AppTarget` (Xbox360 for Big Picture, DS4 for shadPS4), launches/opens the target, waits for its
  window, then starts `VideoEncoder`, `AdaptiveBitrateController`, `AudioCaptureSender`, raises
  process priority/timer resolution, and (Big Picture only) the 1s content-window watcher.
- `OnSessionStopped`: stops all of that, disconnects pads, closes the game and exits Big Picture
  (or closes shadPS4).
- `ConnectionChanged`/`ShadPs4NotConfigured` fire on the thread pool - the UI marshals via
  `Dispatcher.BeginInvoke` (never a synchronous `Invoke`: `Dispose` runs on the UI thread and
  waits for the session loop).

UI (`UI/main/MainWindow`, `HomeView`, `UI/Settings/SettingsView`) only calls into the orchestrator.
**New functionality = a service wired in `AppOrchestrator`**, not logic in a view.

## `Config.cs` (`config.json` next to the exe)

`ClientIp`, `VideoPort` 19000, `HidPort` 19001, `AudioPort` 19002, `LibraryPort` 19003 (unused),
`TargetWidth`/`TargetHeight` 1280×720, `Fps` 60, `BitrateKbps` 4000 (higher saturated the real
Wi-Fi link - see its doc comment), `AdaptiveBitrate`, `AudioSampleRate` (forced to 48000 in
`Program.cs`), `AudioChannels`, `AudioGain`, `SwitchButtonLayout`, `RestartBigPictureAfterGame`
(off), `ShadPs4ExePath`, `AudioDebugRecordToFile` (`[JsonIgnore]`, never persisted). Unknown JSON
keys are ignored on load, so removing a field is safe.

## `IDisposable` / teardown

Owned-resource services are `sealed class ... : IDisposable`. `Dispose()` is best-effort: never
throws, logs failures, safe to call twice or after a partial init. Bounded waits only during
teardown (`WaitForExit(500..2000)`), never indefinite.

## Logging

`Logger.Log("Component", message)` → `SolarNX.log` in `AppContext.BaseDirectory`, truncated once per
process start, `[HH:mm:ss.fff]` timestamps, all I/O swallowed. Never `Console`/`Debug.WriteLine`.
The Switch client's nxlink log (`Client/logs/`, via the VS Code launch config) is stamped with the
PC clock in the same format, so the two can be read side by side.

## Fire-and-forget

Background work is `_ = Task.Run(async () => { ... })` with its own try/catch (see
`ScheduleDelayedAudioRestart`, `ApplyAdaptiveBitrate`) or `Task.Delay(...).ContinueWith(...)`.
Anything that touches pipeline state re-acquires `_pipelineGate` itself and bails if
`_pipelineActive` went false meanwhile. `OnSessionStarted` is the one `async void` (event handler) and
catches everything.

## Comment style - "engineering diary"

Long-form comments explaining *why*: what was tried, what was observed live, the root cause.
Preserve them when editing nearby code; update them when behavior changes (a stale diary entry is
worse than none - several were found describing removed mechanisms).

## Language

Russian for user-facing UI strings, English for code, identifiers, comments and logs.
