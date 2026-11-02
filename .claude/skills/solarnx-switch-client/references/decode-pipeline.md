# Video decode pipeline

Files: `video/video.c` (FFmpeg portlib built with `--enable-nvtegra`; SDL2 output),
`video/reassembly.c` (frames from datagrams + Reed-Solomon recovery, `common/rs/rs.c`),
`video/videowire.h` (the format - mirrors the PC's `Capture/VideoSender.cs`).

No libavformat for video since 0.2.3: the PC sends its own framing with FEC instead of MPEG-TS
(lost MPEG-TS datagrams smeared the picture until the next keyframe).

## Per stream round (`videoInit` → `videoStart` → `videoExit`, driven by `main.c`)

- `videoInit`: SDL window/renderer (`ACCELERATED | PRESENTVSYNC`, 1280×720),
  `reassemblyInit` (rs tables + frame buffer), the decode→render handoff (mutex + `s_pendingFrame`), and
  `av_hwdevice_ctx_create(AV_HWDEVICE_TYPE_NVTEGRA)` — failure just means software decode.
- `videoExit`: tears all of it down (incl. `SDL_Quit`) so the menu can create its own SDL window
  when the player goes back to the menu; the next round re-inits.
- `videoStart`: loop { wait for the host's `SessionConfig` (first time only) → `runStreamSession`
  → 1s → retry } until exit or back-to-menu. `runStreamSession` only returns early when setup or
  the socket fails - that counts as a reconnect (`s_videoReconnectCount`, host telemetry); a round
  interrupted on purpose doesn't.

## `runStreamSession`

1. Plain BSD UDP socket on 19000: `SO_RCVBUF` 256KB (= `udp_rx_buf_size`), `SO_RCVTIMEO` 50ms -
   the decode thread checks `s_decodeStop`/`appIsRunning()` between timeouts, so nothing blocks
   without an exit path. In Internet mode `transport.c` delivers the unsealed datagrams here on loopback.
2. `reassemblyReset` (wait for a keyframe), spawn the **decode thread**; the main thread becomes the
   render loop (pumps `appMainLoop()` every 50ms — it's IPC); on exit set `s_decodeStop`, join, close.

Decoder (opened by the decode thread for the codec of the first frame, reopened when the codec
flag changes - the PC switched H.265 ↔ H.264): `avcodec_find_decoder(AV_CODEC_ID_HEVC / H264)`,
`AV_CODEC_FLAG_LOW_DELAY`, `hw_device_ctx` + `get_format` preferring `AV_PIX_FMT_NVTEGRA`
(the portlib has both `ff_h264_nvtegra_hwaccel` and `ff_hevc_nvtegra_hwaccel`). No extradata:
VPS/SPS/PPS come in front of every keyframe (PC: `dump_extra`).

## Reassembly (`reassembly.c`)

- One frame at a time: a datagram of a newer frame means the current one got all it will get (the
  PC paces a frame over ≤4ms, the next starts a frame interval later). Late datagrams of an older
  frame are dropped. A frame-number jump of ≥1000 either way = new stream (VideoSender starts at a
  random number), not loss.
- Block done once K of its K+M shards are in; missing data shards rebuilt with `rsReconstruct`
  (static scratch - decode thread only). Frame complete when all blocks are done; 64 zero bytes
  follow it (`AV_INPUT_BUFFER_PADDING_SIZE`).
- A lost frame (unrebuildable or never arrived) sets "need keyframe": complete frames are dropped
  until a keyframe completes, so the decoder never sees a broken reference and the last good
  picture stays on screen instead of smearing. `avcodec_send_packet` failing does the same.
- Counters (`ReassemblyStats`, free-running): `framesLost` and `shardsRecovered` go to the host in
  the HID packet (where touchX2/Y2 were); the heartbeat log also shows `framesSkipped`.
- Rejected datagrams (wrong size/magic/version - e.g. an older PC app sending MPEG-TS) are logged
  once.

## Decode thread / render thread split

Confirmed live: with one thread doing read → decode → readback → upload → VSYNC present, games
(heavier frames than menus) dropped it to ~43fps — present waited an extra vblank whenever a frame's
work overran 16.7ms, nothing read the socket meanwhile, and (then) libavformat logged ~100
`Circular buffer overrun`/s: dropped datagrams, seen as smearing ("water running down glass") until
the next keyframe. A bigger socket buffer only delayed it — the deficit was continuous.

- Decode thread (core 1, prio `0x2D`): `recv` → `reassemblyPush` → when a frame completes, send/receive → for NVTEGRA frames
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
