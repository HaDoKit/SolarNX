# Platform constraints (devkitPro / libnx / real Switch hardware)

## Applet message loop (`common/appstate/`)

- `appMainLoop()`: main thread only; wraps `appletMainLoop()` and **latches** a false result, so
  every later main-thread loop (menu, video, `networkWaitForSessionConfig`) also stops. Called off
  the main thread it only reads the flag (checked via `threadGetCurHandle()` vs
  `envGetMainThreadHandle()`), because popping the queue from two threads loses the exit request.
- `appIsRunning()`: any thread, never touches the queue.
- `appRequestMenu/appMenuRequested/appClearMenuRequest`: the back-to-menu request (atomics).
- Each `appMainLoop()` is an IPC round-trip — rate-limit callers that loop fast.

## Threads and cores

See the table in `SKILL.md`. Application homebrew gets cores 0-2; core 3 belongs to the system.
Main (render) and video decode share core 1 with decode one priority step lower, so the render thread
preempts the readback when a vblank arrives. HID on core 0 and audio on core 2 were pinned after both
degraded over long sessions while the scheduler let video share their core. Thread stacks are sized
per workload (FFmpeg decode needs a large stack; HID almost none) — reason about a new thread's needs
instead of copying a row.

libavformat's UDP fifo thread is deliberately **not** used (no `fifo_size`): it made blocked reads
ignore the interrupt callback.

## Socket config

`num_bsd_sessions = 12`, `udp_rx_buf_size = 0x40000` — details and history in
`network-protocol.md`. The BSD transfer memory grows with these (≈ buffer sizes × `sb_efficiency`);
current values cost a few MB.

## Memory

- FFmpeg objects are created/freed per stream session (`runStreamSession`) and the whole video stack
  per round (`videoInit`/`videoExit`). Any new retry path must free everything it allocated — a past
  reconnect leak exhausted memory and showed up as the *audio* thread failing.
- Audio `audout` buffers: `memalign(0x1000)` + `armDCacheFlush` before handing them to the service.
- No fixed per-resolution plane buffers any more; frames are `AVFrame`s moved between threads with
  `av_frame_move_ref`.

## No CPU overclock

The client does not touch `clkrst`; performance numbers assume stock clocks. NVTEGRA does the decode;
the remaining CPU cost is the readback copy and texture upload.

## Logging and clocks

- `logWrite(category, fmt, ...)`: `printf` under a mutex. DEBUG builds connect stdout to nxlink
  (`nxlinkStdio()`, needs the socket service, so `logInit` runs after `networkInit`). Release builds
  have no log sink at all.
- No reliable RTC: the client itself doesn't timestamp; the VS Code nxlink launch config stamps each
  line with the PC's wall clock (`Client/logs/`), matching the host's `SolarNX.log` format.

## Exit

`void main()` returns after video stops; the audio thread is joined, the HID thread isn't (process
exit tears it down). Exiting from the first menu tears down only the listen socket and romfs.
