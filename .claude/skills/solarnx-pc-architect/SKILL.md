---
name: solarnx-pc-architect
description: This skill should be used when writing, reviewing, or reasoning about code in SolarNX/**/*.cs (and SolarNX/installer.iss) — the .NET 10 WPF PC host app in this repo. Trigger on requests like "review this C# code in SolarNX", "add a service to AppOrchestrator", "why does the video stream use UDP", "how does the host detect that a game closed", "add a new virtual gamepad button", "change the ffmpeg encoder args", "add a DllImport", or any work touching Capture/, Audio/, Input/, Session/, BigPicture/, ShadPs4/, Native/, UI/ or Config.cs. This is NOT a generic C# guide and NOT a web-framework skill — it documents this specific desktop/streaming app.
---

# SolarNX (PC host) — architecture guide

## What this app is

SolarNX is the **PC-side host** of a remote-play system whose client is a homebrew app
running **on a Nintendo Switch**. Both live in this one repository:

- `SolarNX/` — this host (C#, .NET 10, WPF UI + a WinForms tray icon).
- `Client/` — the Switch client (C, devkitPro/libnx), covered by the `solarnx-switch-client`
  skill.

There is still **no shared build** between them: the wire formats are mirrored by hand on both
sides (see `references/switch-protocol.md`), so a mismatch compiles fine and fails at runtime.

Three independent, unsynchronized streams to the Switch's IP:

| Stream | Transport | Port | Direction | Payload |
|---|---|---|---|---|
| Video | UDP (own framing + Reed-Solomon FEC) | 19000 | PC → Switch | H.265 Main (default) or H.264 High from a child `ffmpeg.exe` (RTP on loopback → `Capture/VideoSender`) |
| HID | TCP | 19001 | PC dials the Switch, Switch listens | 28-byte `SessionConfig` PC→Switch once, then 8×`0xFF` marker + 224-byte `HidPacket` Switch→PC at ~120Hz |
| Audio | UDP (RTP-like) | 19002 | PC → Switch | Opus (Concentus), hand-rolled RTP header + 2-byte length prefix |
| Library | TCP | 19003 | PC dials the Switch every 5s while armed | game list (id, name, 240×360 JPEG cover), sent only when the content hash changed |

The Switch player picks a target in the client's menu (`AppTarget`: Big Picture, shadPS4, or a
game from the host's own **library**); the host launches/focuses it on the PC's normal desktop and
captures the primary monitor.

## Module map

| Folder / file | Responsibility |
|---|---|
| `AppOrchestrator.cs` | Composition root: wires session start/stop to Steam/shadPS4 launch, capture, audio, gamepads; runs the 1s content-window watcher (game launch/exit detection, video stall watchdog) |
| `Session/SessionController.cs` | Dials `ClientIp:HidPort`, sends `SessionConfig`, reads HID frames; session starts on the first parsed packet, ends on close or 3s of HID silence |
| `Session/AdaptiveBitrateController.cs` | Optional (`Config.AdaptiveBitrate`): lowers/raises bitrate from client telemetry by restarting ffmpeg |
| `Capture/VideoEncoder.cs` | Owns the `ffmpeg.exe` process (gfxcapture → NVENC/x264 → UDP) |
| `Audio/` | WASAPI loopback → Opus → `RtpPacketizer` → UDP |
| `Input/HidProtocol.cs` | `HidPacket`/`SessionConfig`/`HidFrameReader` wire formats |
| `Input/VirtualGamepad.cs` | `VirtualGamepadHub`: up to 4 ViGEm pads, Xbox360 (Big Picture) or DualShock4 raw reports (shadPS4) |
| `Input/GuideButtonChord.cs` | Minus+Plus on the Switch → Guide (Xbox) / PS (DS4) — the Switch Home button never reaches the client |
| `Input/TouchInjector.cs` | Single touch → `touchhelper.exe` child process (InjectTouchInput) |
| `Input/GameBarSuppressor.cs` | Stubs `ms-gamebar:` so a new virtual Xbox pad doesn't pop a Game Bar dialog |
| `BigPicture/SteamLauncher.cs` | Steam detect/launch, Big Picture open/close, content-window detection, closing the game on disconnect |
| `Library/` | User game library: `GameLibrary` (library.json + covers/), `GameLauncher` (Native exe / Steam `rungameid` / Emulator template / cmd Script), `LibraryPusher` + `LibraryWire` (port 19003 protocol), `CoverImage` (→ 240×360 JPEG), `SteamLibraryScanner` (installed games + capsule art) |
| `ShadPs4/` | shadPS4 launcher (`--big-picture`). The old Switch-side shadPS4 game picker and its TCP channel (`ShadPs4LibraryChannel`, `LibraryProtocol`, `ParamSfoReader`, scanner) were deleted - don't revive them |
| `Native/` | `ChildProcessJob` (kill children with this process), `ProcessImagePath` (limited-rights exe path), `TrackedProcess` + `ProcessTree` (follow a game by pid/start time, optionally through descendants, via Toolhelp - no handles), `Mmcss`, `TimerResolution`, `TouchHelper/` (native helper) |
| `UI/` | WPF `MainWindow`/`HomeView`/`SettingsView`/`Library/LibraryView` + `GameEditWindow` — UI only |
| `Config.cs` | `config.json` next to the exe; see the field list in `references/architecture-conventions.md` |
| `Program.cs` | Bootstrap: single-instance mutex, tray icon, WPF `App`, `--protocol-stub`/`--dev` args |

**New functionality goes in a service wired by `AppOrchestrator`, never directly in the UI.**

## Hard rules learned the hard way

**Anti-cheat (VAC/EAC/BattlEye):**
- Never inject anything into a game process, and never open a game process with more than
  `PROCESS_QUERY_LIMITED_INFORMATION` (+ `SYNCHRONIZE`). Use `ProcessImagePath.TryGet`, not
  `Process.MainModule` (that needs a `PROCESS_VM_READ` handle — the external-cheat pattern).
- There is no DLL injection anywhere in the app any more. The old Steam focus hook was removed
  after a live A/B test — see `references/steam-bigpicture.md` before reintroducing anything
  similar.

**Steam / Big Picture:**
- Don't spoof or force Big Picture's focus. With the hook gone, Steam's own focus tracking returns
  gamepad control to Big Picture after a game and keeps its Resume/Stop buttons working.
- "Game closed" = the tracked game **process exited**, never "its window disappeared" (a
  minimized fullscreen game drops out of the window heuristic).
- Don't close and reopen Big Picture around a game exit by default (`RestartBigPictureAfterGame`
  is off), and never reopen it while the old window is still closing — that hung steamwebhelper.
- Never write AppCompat "Disable fullscreen optimizations" flags for games — it made a working game
  (Windblown) invisible to capture and didn't fix the one it was added for.

**Library games:**
- Launched games are never held by a handle: `GameLauncher` disposes the `Process` from `Start()`
  right away and follows the game via `TrackedProcess` (descendants included - launchers and
  `start` in scripts exit immediately). Never inject anything to "connect the gamepad": the ViGEm
  pad is system-wide and every game sees it.
- A library session ends when the game's process family is gone (3 consecutive dead ticks) - the
  host force-disconnects HID, and the client goes back to its menu on that disconnect.
- Library entry ids are stable and never reused (`GameLibrary.NextId`) - the Switch sends them back.
- The library watcher follows the game's **main window as it changes** (launcher splash → real game
  window, fullscreen toggles): each change gets one `ForceForegroundWindow` and, with
  `GameEntry.CaptureGameWindow` (default on), the encoder is restarted capturing that window
  (exact fit + centring via `Native/WindowGeometry`, see ffmpeg-pipeline) and
  `TouchInjector.TargetWindow` maps touches back through the same fit. A client-size change of the same
  window restarts the capture with a new fit. A minimized captured window yields no frames, so the stall watchdog
  skips it. WGC delivers frames only when the window changes - a static/minimized window stalls.

**Process/thread hygiene:**
- Every long-running child process goes into `ChildProcessJob` — an orphaned ffmpeg kept streaming
  after the host died.
- Event handlers of child `Process` objects must not touch fields that `Stop()` nulls/disposes
  (`Exited` runs on the thread pool; an exception there kills the app).
- Cross-process window messages: `SendMessageTimeout(SMTO_ABORTIFHUNG)` or `PostMessage`, never a
  plain `SendMessage` from code that holds `_pipelineGate`.

**Protocol:** any change to ports, `HidPacket`, `SessionConfig`, the marker, the RTP framing or the
Guide/menu gestures must be made in `Client/` in the same change — see
`references/switch-protocol.md`.

## Project-wide conventions

- `sealed class ... : IDisposable` for owned-resource services; `Dispose()` is best-effort and never
  throws.
- Logging only via `Logger.Log(category, message)` → `SolarNX.log` (truncated per run,
  `HH:mm:ss.fff` timestamps — the client's nxlink log uses the same format for side-by-side reads).
- Comments are long-form "engineering diary" entries: what was tried, what failed, why the current
  approach won. **Preserve them** when editing nearby code.
- Russian for user-facing UI strings, English for code/comments/logs.

## Reference files

- **`references/switch-protocol.md`** — ports, `SessionConfig`/`HidPacket` layouts, telemetry,
  ViGEm mapping, Guide chord, hold-Minus back-to-menu, library channel
- **`references/steam-bigpicture.md`** — Steam launch/close, content-window watcher, game process
  tracking, the removed focus hook and why
- **`references/ffmpeg-pipeline.md`** — exact ffmpeg invocation, capture/transport/rc history,
  watchdog, child-process lifetime
- **`references/native-interop.md`** — CsWin32 vs manual P/Invoke, handle rules, access-rights
  rules
- **`references/architecture-conventions.md`** — composition root, session lifecycle, Config
  fields, Dispose/Logger/fire-and-forget patterns
