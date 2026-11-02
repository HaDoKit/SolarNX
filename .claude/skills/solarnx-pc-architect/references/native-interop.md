# Native interop conventions

## CsWin32 vs manual `[DllImport]`

Two coexisting styles - use the right one:

- **CsWin32-generated** (`NativeMethods.txt` drives the `Microsoft.Windows.CsWin32` source
  generator): audio loopback/WASAPI, DXGI/D3D11, window enumeration, GDI helpers. **If an API is
  already listed in `NativeMethods.txt`, use the generated method** - don't duplicate it with a
  manual `[DllImport]`.
- **Hand-written `[DllImport]`**, grouped at the bottom of the owning class, for APIs not in that
  list: `Program.cs` (FindWindow/ShowWindow/SetForegroundWindow), `SteamLauncher.cs` (window
  messages, `IsWindow`, `AttachThreadInput`, `SendInput`), `Native/ProcessImagePath.cs`,
  `Native/ChildProcessJob.cs`, `Native/Mmcss.cs`, `Native/TimerResolution.cs`.

## Process access rights (anti-cheat)

- Opening any process that may be a game: `PROCESS_QUERY_LIMITED_INFORMATION` (+ `SYNCHRONIZE`)
  only. That's what `Process.HasExited`/`StartTime` need and what Task Manager uses.
- Exe path of another process: `ProcessImagePath.TryGet(pid)` (`QueryFullProcessImageNameW`).
  **Not** `Process.MainModule` - it enumerates modules through a `PROCESS_VM_READ` handle, the
  access pattern kernel anti-cheats watch for (and it also failed across bitness/integrity levels).
- No `VirtualAllocEx`/`WriteProcessMemory`/`CreateRemoteThread` anywhere. The former
  `DllInjector` and `focushook.dll` were deleted - see `steam-bigpicture.md`.

## Window messages to other processes

A plain cross-process `SendMessage` blocks until the target thread processes it. Steam's windows
do hang (steamwebhelper, ~18s observed), and the watcher holds `_pipelineGate` while it runs, so a
blocking send stalls the whole pipeline. Use `PostMessage` (e.g. `WM_CLOSE` in `ExitBigPicture`) or
`SendMessageTimeout` with `SMTO_ABORTIFHUNG`.

## Child processes

`ffmpeg.exe` and `touchhelper.exe` are assigned to `ChildProcessJob` (Job Object with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, handle deliberately never closed) right after `Start()`, so
the OS kills them however this process ends - debugger stop, crash, kill. User apps launched on the
player's behalf (Steam, shadPS4, games) must **not** go in the job.

## Handle lifetime

No `SafeHandle` wrappers - raw `IntPtr`, closed in `finally`. Consistent, deliberate practice;
don't introduce a `SafeHandle` in one isolated spot. A project-wide change would be its own
explicit refactor.

## `unsafe`

`AllowUnsafeBlocks=true` project-wide. Document memory ownership/lifetime in a comment for any
pointer code, and only use it where a safe alternative isn't adequate.
