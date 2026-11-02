# Audio pipeline

File: `audio/audio.c`. Runs on its own thread for the whole process (across menu round-trips).

## Transport: raw BSD UDP socket

`socket/bind` on `SOLARNX_AUDIO_PORT`, `SO_RCVTIMEO` 200ms (only so the loop re-checks
`appIsRunning()`), `SO_RCVBUF` 64KB. One `recv()` = one datagram = one unit, nothing to reassemble.
`EAGAIN` is the idle case; any other errno breaks out and reopens after 1s (counted in
`s_audioReconnectCount`).

History: this used `avio_open2` + `avio_read_partial` (mirroring video's libavformat use). That
crashed the whole process ~2s in, reproducibly, independent of bsd session count, poll interval or
the `av_log` mutex, and even with video disabled — while HID's raw socket was stable. Audio needs
no demuxing, so it moved to the same raw-socket pattern. Don't move it back.

## Wire format (host: `SolarNX/Audio/RtpPacketizer.cs`)

```
[12-byte RTP header: V=2 PT=97, BE seq u16, BE timestamp u32, BE SSRC u32][2-byte BE length][Opus frame]
```

The length prefix isn't RTP — kept from when units could arrive coalesced; with one datagram per
`recv` it's used as a bounds check (length disagreeing with the datagram → drop).

## Decode, PLC, playback

- libopus decoder, 48000Hz / 2ch, 20ms frames (960 samples) — matches `AudioCaptureSender.cs`.
  (The host can be configured to mono; an Opus stereo decoder handles that.)
- Sequence gap of 1..25 frames → `opus_decode(NULL)` per missing frame (PLC), counted in
  `s_audioConcealedFrameCount` (host adaptive-bitrate telemetry). Larger gaps are not concealed —
  just resync. Reordered/duplicate packets aren't special-cased (they look like a huge gap).
- `audout`: 4 buffers × one 20ms frame, `memalign(0x1000)`, `armDCacheFlush` before append.
  `submitPcm` reclaims released buffers and waits up to ~50ms for a free one, else drops the frame.

## Known open issue: latency drift

There's no clock-drift compensation between the PC's WASAPI capture clock and `audout`. Any
surplus accumulates in the 64KB socket buffer (~4s of audio at 128kbps), and a long PLC burst pushes
up to 500ms through a 4-buffer queue synchronously. Likely explanation for "audio lag grows over a
long session". Fix direction: drop when the socket backlog exceeds ~100ms, smaller `SO_RCVBUF`, cap
PLC at 2-3 frames.

## No A/V sync by design

Audio and video are independent streams with no shared clock on either side. Perceived drift is a
scheduling/buffering investigation in one pipeline, not a missing sync feature.
