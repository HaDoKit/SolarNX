# Steam Big Picture integration

Files: `BigPicture/SteamLauncher.cs`, `AppOrchestrator.cs` (session start/stop + the 1s
content-window watcher `OnContentWindowWatcherTick`).

No Steamworks SDK, no injection. All integration is external: registry, `steam://` URIs, window
enumeration, process tracking.

## Launch and close

- Steam path: `HKCU\Software\Valve\Steam\SteamExe`. Not running → `steam.exe -silent`, wait 4s.
- Open Big Picture: `steam://open/bigpicture`. This IPC to an already-running Steam can silently
  go nowhere, so `WaitForWindowAsync` re-fires it every ~10s while waiting (30s timeout).
- Close Big Picture: `steam://exitbigpicture` is a confirmed no-op on the current client. Instead
  `ExitBigPicture` posts `WM_CLOSE` to the window found by class (`SDL_app`, `vguiPopupWindow`;
  title-independent - it can read "Steam" instead of "Big Picture"). It returns the hwnd so a caller
  can wait for exactly that window to go (`WaitForWindowClosedAsync`).
- `FindBigPictureWindow` also matches unrelated `vguiPopupWindow` dialogs (e.g. Steam's own
  "steamwebhelper is not responding" box) - don't build "wait until no Big Picture window" logic
  on it; wait on a specific hwnd.
- Steam itself is left running between sessions; `OnSessionStopped` closes the game
  (`CloseActiveGame`) and exits Big Picture.

## Content-window watcher and game tracking (`AppOrchestrator`)

Runs every 1s, Big Picture sessions only (also hosts the video stall watchdog - see
`ffmpeg-pipeline.md`).

- `FindActiveContentWindow` picks the largest visible window (≥400px both ways) owned by a
  Steam-family process: steam.exe/steamwebhelper.exe, an exe under the Steam install dir, or a
  process-tree descendant of Steam (non-Steam shortcuts, secondary libraries). **Allow-list only** -
  a deny-list once grabbed the user's own VS Code window.
- `isGame` = that window's owner isn't steam.exe/steamwebhelper.exe.
- **Game running = the tracked game process is alive** (`_trackedGame`: pid + start time, captured
  when its window first wins). Confirmed live: clicking into Explorer minimized a fullscreen game,
  its window dropped below the size floor, Big Picture's window won, and the old window-based
  logic declared "game closed" - it then closed/reopened Big Picture on top of the still-running
  game (Steam's Resume/Stop buttons went grey) and re-declared "game launched" when the window came
  back. Three false cycles in 90s.
- On game launch (once per process): `ForceForegroundWindow` (one-shot
  `AttachThreadInput`+`SetForegroundWindow`) and a delayed (3s) audio capture restart.
- On game exit: audio capture restart. Steam returns to Big Picture by itself. Only with
  `Config.RestartBigPictureAfterGame` (off by default) does the host also send Escape
  (`SendBackNavigation`) and close/reopen Big Picture - an old workaround for a dead gamepad after
  non-Steam shortcuts that turned out to be caused by the focus hook (below).
- If no Steam-family content window exists for 5 consecutive ticks (and one existed before), the
  host force-disconnects the client - Big Picture was closed or crashed. The tolerance covers
  third-party launcher windows briefly covering Big Picture.

## Big Picture restart sequencing (when `RestartBigPictureAfterGame` is on)

Escape → 500ms → `WM_CLOSE` → wait until that hwnd is gone (≤15s) → 1s → `steam://open/bigpicture`.
Measured live: Escape, `WM_CLOSE` and reopen all within ~2ms - reopening while the webview was still
tearing down - hung steamwebhelper for ~18s every time until Steam showed its "not responding"
dialog.

## The removed focus hook - read before adding anything similar

History: `focushook.dll` was injected (CreateRemoteThread+LoadLibraryW) into
steam.exe/steamwebhelper.exe and IAT-patched `GetForegroundWindow` so Big Picture always believed
it was focused, plus `ReassertFocus` sent it `WM_ACTIVATE`-family messages every tick, and a named
event (`SolarNX_GameActive`) suspended the spoof while a game ran. Earlier it was also injected into
game processes.

Removed after a live A/B test (`UseFocusHook=false`, Steam restarted): Big Picture launched by
SolarNX then behaved exactly like one opened by hand - gamepad control returned to it after a game
and the Resume/Stop buttons stayed active. The spoof was what kept Steam from seeing the real focus
changes it relies on. Injection into games was dropped first, for anti-cheat reasons.

Consequences to keep in mind:
- The hook stayed loaded in Steam's processes until Steam restarted, even after SolarNX exited -
  any injected code outlives the host.
- There is no "keep Big Picture controllable while the first user works in another window"
  mechanism now. If that's needed again, it must not be injection/focus spoofing.

## Other quirks

- An earlier hidden-virtual-monitor architecture was abandoned (DPI virtualization mismatches,
  Windows reassigning the primary monitor). Big Picture and games run on the real desktop.
- `GetWindowDisplayAffinity` is logged for game windows: `WDA_EXCLUDEFROMCAPTURE` means no capture
  method can see that window.
- AppCompat "Disable fullscreen optimizations" flags must not be written for games (removed
  `FullscreenOptimizationFix`): it didn't fix Balatro and made Windblown invisible to capture.
