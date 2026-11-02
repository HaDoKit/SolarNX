# ffmpeg video pipeline

File: `Capture/VideoEncoder.cs`. ffmpeg is used **only for video** — audio is a
fully separate, in-process pipeline (WASAPI → Opus → RTP → UDP) with no ffmpeg
involvement and no muxing/sync between the two streams; see
`references/switch-protocol.md`.

## Exact invocation

`ffmpeg.exe` (bundled at `AppContext.BaseDirectory`) launched via
`ProcessStartInfo` with `RedirectStandardError=true` (read via `BeginErrorReadLine` for the log and
the watchdog), `RedirectStandardOutput=true` (never read - ffmpeg sends over UDP itself),
`UseShellExecute=false`, `CreateNoWindow=true`. Right after `Start()` the process is put into
`ChildProcessJob` (Job Object, kill-on-close): confirmed live that a debugger stop/crash of the host
left ffmpeg orphaned and still streaming video to the Switch for over an hour, while audio and HID
(in-process) stopped - it looked like a client bug.

The `Exited` handler captures the local `Process`, never the `_ffmpeg` field: `Stop()` kills,
disposes and nulls the field, `Exited` then fires on the thread pool, and an exception there
terminates the app.

```
-fflags nobuffer -flags low_delay -probesize 32
-filter_complex "gfxcapture=monitor_idx=0:max_framerate={fps}:width={w}:height={h}:resize_mode=scale,hwdownload,format=bgra,fps={fps},format=yuv420p"
-f mpegts
-c:v h264_nvenc -gpu 0 -preset p4 -tune ll -rc cbr -zerolatency 1 -delay 0 -rc-lookahead 0 -bf 0 -profile:v high -g {fps/2}
  (fallback: -c:v libx264 -preset faster -tune zerolatency -profile:v high -crf 18 -g {fps/2})
-b:v {kbps}k -minrate {kbps}k -maxrate {kbps}k -bufsize {kbps}k
-flush_packets 1
udp://{clientIp}:{videoPort}?pkt_size=1316
```

- `-profile:v high` (was `baseline`): confirmed live the encoder ran at q=32-36 in games at 4000kbps;
  High (CABAC, 8x8 transform) gives ~15-25% better quality at the same bitrate. B-frames stay off
  (`-bf 0` / x264 zerolatency), so no added latency. NVTEGRA on the Switch decodes High.
- Library games (`GameEntry.CaptureGameWindow`): the source becomes
  `gfxcapture=hwnd=<game window>:capture_border=0:capture_cursor=0:resize_mode=scale_aspect:width=<fit W>:height=<fit H>`
  followed by `pad=<stream W>:<stream H>:<offX>:<offY>` after `hwdownload` - the client area scaled on
  the GPU to its aspect-preserving fit (`Native/WindowGeometry.Fit`) and centred. Letting gfxcapture fit
  into the full stream size itself pins the picture top-left (verified with test captures), which is
  why the fit is computed here. `VideoEncoder.CaptureWindow`/`CaptureSize` are carried over by
  restarts; AppOrchestrator restarts with a new fit when the window's client size changes.
- `pkt_size=1316` = 7×188-byte MPEG-TS packets — chosen to avoid IP fragmentation
  on a standard 1500-byte-MTU link.
- `-delay 0 -rc-lookahead 0 -bf 0` deliberately strip out NVENC's own internal
  frame buffering, even though a zerolatency/low-latency preset is already
  selected — that preset alone was not enough to eliminate buffering delay.
- NVENC availability is probed once per process lifetime (`_nvencAvailable`
  static cache) by encoding a single frame of `color=black:s=256x256:d=0.1` to
  `-f null -`. **Do not probe at a smaller resolution** — 64×64 was confirmed
  live to fail with "Frame Dimension less than the minimum supported value."
  Driver requirement: NVENC failures also surface as "Driver does not support the
  required nvenc API version" below driver 610 / Video Codec SDK 13.1.

## Capture method — rejected approaches, in order

1. **DXGI Desktop Duplication + stdin pipe** (C# captures, pipes raw frames to
   ffmpeg): topped out around ~33fps against a 60fps target — the C# capture loop
   itself was the bottleneck, not ffmpeg.
2. **`gdigrab`**: reliably reached full frame rate, but visibly flickered — it
   doesn't reliably see DirectComposition-rendered layers.
3. **`gfxcapture`** (current): Windows.Graphics.Capture-based ffmpeg filter,
   requires ffmpeg 8.x+ (the bundled `ffmpeg.exe` was updated from a 2022 build
   specifically to get this filter).

The old `gdigrab` references in `AppOrchestrator.cs` comments are gone; `VideoEncoder.cs`'s class
comment is the current description.

## Transport — rejected approaches, in order

1. **Plain TCP** for video: observed dying every ~7–20 seconds under sustained
   load.
2. **UDP** (current): no persistent connection to drop.
3. A brief revert back to TCP was tried after UDP packet loss appeared to smear
   video via motion-compensation error propagation — but the actual root cause of
   *that* symptom turned out to be something unrelated to the transport choice
   entirely. UDP was kept.
   - The "water running down glass for up to a second" artifact seen later was finally traced
     (client nxlink log: ~100 `Circular buffer overrun`/s, only while a game ran) to the **Switch
     client** dropping datagrams because its single decode+render thread was paced by VSYNC - fixed
     on the client by a decode/render thread split (see the `solarnx-switch-client` skill), not on
     the host.
4. GOP size (`-g`) was reduced to ~0.5s (fps/2), down from the original ~2s, as a
   deliberate hedge against ordinary UDP packet loss — a shorter GOP limits how
   much a single lost packet can corrupt.

**If you're considering changing the transport or GOP again, re-read this
history first** — both of these have already been tried, reverted, and the real
root causes chased down once.

## Encoder rate-control bug (already found and fixed once)

An earlier version used `-rc vbr -cq:v 19` (quality-target rate mode) combined
simultaneously with hard bitrate bounds (`-b:v`/`-minrate`/`-maxrate`). Live
testing showed these fighting each other, causing the encoder to fall behind real
time. Fix: switch to `-rc cbr` with an explicit `-preset p4`. **Don't reintroduce
a quality-target rc mode alongside hard bitrate bounds.**

## Watchdog

`IsStalled(threshold)` checks ffmpeg's stderr for `frame=` progress lines
(`BeginErrorReadLine`), tracking time since last progress. Two separate
thresholds: `StartupGrace` (15s, before the first frame ever arrives) and a
tighter steady-state threshold (5s, passed in by `AppOrchestrator`).
`RestartVideoEncoder` does `Stop()` (which `Kill()`s the process and
`WaitForExit(2000)`) then creates and starts a fresh `VideoEncoder`, carrying over the bitrate the
outgoing one ran at (so a stall restart doesn't undo an adaptive-bitrate change).

**Known gap**: the watchdog is called from the content-window watcher, which only runs for Big
Picture sessions - shadPS4 sessions have no stall detection.

**Known bug already fixed once**: an earlier version used the same 5s threshold
for both startup and steady-state. This caused the watchdog's own restart to
itself trigger exactly the freeze it was meant to fix — an infinite restart loop
that was originally misdiagnosed as "video became very slow" rather than "the
watchdog is restarting in a loop." Don't lower the startup grace period without
being aware of this failure mode.
