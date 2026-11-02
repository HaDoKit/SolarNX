# Video decode pipeline

File: `video/video.c` (FFmpeg portlib built with `--enable-nvtegra`; SDL2 output).

## Per stream round (`videoInit` → `videoStart` → `videoExit`, driven by `main.c`)

- `videoInit`: SDL window/renderer (`ACCELERATED | PRESENTVSYNC`, 1280×720),
  `avformat_network_init`, the decode→render handoff (mutex + `s_pendingFrame`), and
  `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_NVTEGRA)` — failure just means software decode.
- `videoExit`: tears all of it down (incl. `SDL_Quit`) so the menu can create its own SDL window
  when the player goes back to the menu; the next round re-inits.
- `videoStart`: loop { wait for the host's `SessionConfig` (first time only) → build the URL (no
  `fifo_size` - see network-protocol) → `runStreamSession` → 1s → retry } until exit or back-to-menu. A round
  interrupted on purpose doesn't count as a reconnect (`s_videoReconnectCount` is host telemetry).

## `runStreamSession`

1. `avformat_alloc_context` + `interrupt_callback` **before** `avformat_open_input` (the open/probe
   blocks until data arrives). The callback returns 1 on `s_decodeStop`, back-to-menu, or app exit;
   from the main thread it also pumps `appMainLoop()` (rate-limited to every 50ms — it's IPC).
2. `AVFMT_FLAG_NOBUFFER`, `probesize 65536`, `max_analyze_duration` 0.5s (single H.264 stream,
   keyframe every ~0.5s).
3. Decoder: `hw_device_ctx` + `get_format` preferring `AV_PIX_FMT_NVTEGRA`.
4. Spawn the **decode thread**; the main thread becomes the render loop; on exit set
   `s_decodeStop` (aborts a blocked read via the callback), join, free everything.

## Decode thread / render thread split

Confirmed live: with one thread doing read → decode → readback → upload → VSYNC present, games
(heavier frames than menus) dropped it to ~43fps — present waited an extra vblank whenever a frame's
work overran 16.7ms, nothing read the socket meanwhile, and libavformat logged ~100
`Circular buffer overrun`/s: dropped datagrams, seen as smearing ("water running down glass") until
the next keyframe. A bigger socket buffer only delayed it — the deficit was continuous.

- Decode thread (core 1, prio `0x2D`): `av_read_frame` → send/receive → for NVTEGRA frames
  `av_hwframe_transfer_data` into `swFrame` → move into `s_pendingFrame` under `s_frameMutex`,
  replacing any frame the display never got to. Every frame is decoded (references), not every
  frame is shown.
- Render (main thread): take the pending frame if any (else sleep 1ms), `SDL_Update{NV,YUV}Texture`,
  present with VSYNC. Heartbeat logs `N frames shown, M decoded` every 300 shown — `decoded` should
  track the host's fps; if it doesn't, decode+readback itself is the bottleneck.

## NVTEGRA readback

Frames live in Tegra memory as NV12 and must be copied to system memory for SDL. The readback stays
on the decode thread (the thread that decoded) — untested whether NVTEGRA tolerates another thread.
**Don't pre-allocate `swFrame` 256-byte aligned**: it removes the per-frame "falling back to cpu
transfer" path and is faster, but was confirmed live to cause video glitches followed by loss of HID
on a different core. The auto-allocated (unaligned) buffer is the proven-stable path. A zero-copy
deko3d renderer would remove the readback entirely — not attempted.

## Pixel formats / texture

NV12 (hw readback) → `SDL_PIXELFORMAT_NV12`; YUV420P (software decoder) → `IYUV`. The texture is
recreated only when width/height/format change — the decoded frame is the source of truth (the
host's resolution isn't otherwise transmitted to this module).
