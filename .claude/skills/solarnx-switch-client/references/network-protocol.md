# Network protocol (Switch client side)

Counterpart: `SolarNX/` in this repo (skill `solarnx-pc-architect`, file
`SolarNX/Input/HidProtocol.cs`, `references/switch-protocol.md`). No shared build — mirrored by hand;
`network.c` `_Static_assert`s the C struct sizes, the C# side checks the payload length. Change both
sides in the same commit.

## Ports (`network/network.h`)

`SOLARNX_VIDEO_PORT 19000` (UDP in), `SOLARNX_HID_PORT 19001` (TCP, this side listens),
`SOLARNX_AUDIO_PORT 19002` (UDP in), `SOLARNX_LIBRARY_PORT 19003` (TCP, this side listens). Must
match `SolarNX/Config.cs`.

## Socket service (`networkInit`)

`socketInitialize` with the default config plus:
- `num_bsd_sessions = 12` — HID listen + client, video UDP, audio UDP, nxlink in DEBUG, plus
  headroom: concurrent HID+video+audio crashed ~2s in at the previous value.
- `udp_rx_buf_size = 0x40000` (libnx default `0xA500` ≈ 42KB ≈ 32 datagrams). Keyframes arrive as
  60-150KB bursts every ~0.5s. `video.c` requests the matching `SO_RCVBUF` on its socket.

## HID (TCP, PC dials in)

1. `networkAcceptHid`: accept (backlog 1), `TCP_NODELAY` (small frequent sends - Nagle caused
   several hundred ms of input lag), `SO_SNDTIMEO` 3s (a stalled host used to hang the send forever
   with nothing logged).
2. Read `SolarNxSessionConfig` (28 bytes, 7×u32 LE: video kbps, width, height, fps, audio kbps,
   sample rate, channels) — the host always sends it first. Stored under a mutex;
   `networkWaitForSessionConfig` (main thread only — pumps `appMainLoop`) gives `video.c` the host's
   real settings (bitrate is reported back as telemetry). *Open issue: no receive timeout on this read — a client that connects
   and sends nothing blocks the HID thread.* After the config, a non-blocking `MSG_PEEK` checks the
   connection is still open: connects that queued in the listen backlog while the menu was up may
   already be closed by the host, and taking one as live used to fake "host ended the library
   session" and bounce the player back to the menu.
3. `startNetwork` (`main.c`) sends `8×0xFF` + `SolarNxHidPacket` (224 bytes) every 8.3ms (~120Hz) until a send
   fails or the back-to-menu request is raised (then it closes the connection and parks).

### `SolarNxHidPacket` (224 bytes, little-endian, no byte swapping)

| Offset | Field |
|---|---|
| 0 | `controllers[4]`, 44 bytes each: `heldKeys` u32 (low 16 bits used), `lJoyX/Y`, `rJoyX/Y` i32 (±0x7FFF), `accelX/Y/Z` f32 g, `gyroX/Y/Z` f32 rad/s |
| 176 | `touchX1/Y1` u32 (1280×720; 0,0 = none) |
| 184 | `videoLostFrameCount`, `videoRecoveredPacketCount` u32 (were touchX2/Y2) — from `reassembly.c` |
| 192 | `libraryEntryId` u32 — picked `LibraryEntry.id` when `appTarget` is Library, else 0 (was `frameRate`) |
| 196 | `controllerCount` u32 |
| 200 | `appTarget` u64 (`SolarNxAppTarget`: 0 Big Picture, 1 shadPS4, 2 Library) |
| 208 | telemetry u32: `videoReconnectCount`, `videoMeasuredBitrateKbps` (currently the configured value, not a measurement), `audioReconnectCount`, `audioConcealedFrameCount` — free-running |

`heldKeys` uses libnx `HidNpadButton` bit positions directly (A=0 … Down=15); the host's
`ControllerState` reads the same bits. The Home and Capture buttons never reach the app — the host
maps Minus+Plus to Guide/PS, and holding Minus for 3s is the client's back-to-menu gesture.

## Video (UDP in)

Plain BSD socket on `0.0.0.0:19000` (`SO_RCVBUF` 256KB, `SO_RCVTIMEO` 50ms), SolarNX framing
(`video/videowire.h`): every datagram is 16 + 1280 bytes - magic 0x53, version 2, flags (bit 0
keyframe, bit 1 H.265),
block index/count, shard index, block parity count, u32 frame number, u32 frame size. Frames are
split into ≤64-data-shard Reed-Solomon blocks with ~20% parity (min 2); see `decode-pipeline.md`
("Reassembly"). Bursts are absorbed by the socket buffer and the always-reading decode thread.
UDP, not TCP: TCP video died every ~7-20s under load.

History: until 0.2.3 this was MPEG-TS via libavformat (`udp://...?fifo_size=0` - its fifo thread
ignored the interrupt callback and hung back-to-menu). A lost datagram there smeared the picture
until the next keyframe; that is what the FEC framing replaced.

## Library (TCP in, `library/library.c`)

Listen socket opened by `libraryStart()` right after `networkInit()` (before the first menu). Per
connection (5s send/recv timeouts): read 16-byte header (`"ONXL"`, version 1, u64 hash) → reply one
byte (1 = need, 0 = have) → if need, read `u32 count` (≤`LIBRARY_MAX_ENTRIES` 256), then per entry
`u32 id`, `u16 nameLen` (≤255) + UTF-8 name, `u32 coverLen` (≤512KB) + JPEG. Any violation drops the
whole message and keeps the previous library. A successful receive swaps the entry array under the
mutex and bumps `libraryVersion()`; the menu rebuilds its textures when that changes. Host side:
`SolarNX/Library/LibraryWire.cs`.

## Audio (UDP in)

Raw socket on 19002 — framing in `audio-pipeline.md`.
