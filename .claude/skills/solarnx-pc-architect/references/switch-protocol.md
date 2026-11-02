# Switch protocol (PC side)

Counterpart: `Client/` in this repo (skill `solarnx-switch-client`, file
`Client/network/network.h`). No shared build — every layout below is mirrored by hand in C. Change
both sides in the same commit.

## Ports (`Config.cs`) and roles

| Field | Default | Transport | Roles |
|---|---|---|---|
| `VideoPort` | 19000 | UDP | ffmpeg sends to `ClientIp:19000`; Switch listens |
| `HidPort` | 19001 | TCP | Switch binds/listens/accepts; `SessionController` dials out |
| `AudioPort` | 19002 | UDP | `AudioCaptureSender` sends; Switch listens |
| `LibraryPort` | 19003 | TCP | Switch listens from app start; `LibraryPusher` dials every 5s while armed |

## HID connection lifecycle (`Session/SessionController.cs`)

1. Connect to `ClientIp:HidPort` (retry every 1s while armed).
2. Immediately write `SessionConfig` (28 bytes, 7×u32 LE): video bitrate kbps, width, height, fps,
   audio bitrate kbps, sample rate, channels. The client blocks on it before opening video/audio.
3. Read loop: `HidFrameReader` (created **per connection** — a reader shared across connections
   glued a stale partial frame onto the new stream) resyncs on 8×`0xFF`, then takes exactly 224
   bytes.
4. `SessionStarted` fires on the **first parsed packet**, not on connect (a random device holding
   the IP could accept-then-close).
5. The session ends on remote close, any read error, or **3s without data**
   (`HidIdleTimeout`; the client sends every ~23ms, so silence means the Switch slept/dropped off
   Wi-Fi without a FIN).

## `HidPacket` (224 bytes, little-endian) — `Input/HidProtocol.cs`

| Offset | Field |
|---|---|
| 0 | 4 × controller, 44 bytes each: `heldKeys` u32 (host reads low 16 bits), `lJoyX/Y`, `rJoyX/Y` i32, `accelX/Y/Z` f32 (g), `gyroX/Y/Z` f32 (rad/s) |
| 176 | `touchX1`, `touchY1` u32 (1280×720 frame; 0,0 = no touch) |
| 184 | `touchX2`, `touchY2` u32 — on the wire, unused |
| 192 | `libraryEntryId` u32 — `GameEntry.Id` when `appTarget` is Library, else 0 (was the never-read `frameRate`) |
| 196 | `controllerCount` u32 |
| 200 | `appTarget` u64 (`AppTarget`: 0 Big Picture, 1 shadPS4, 2 Library; unknown → Big Picture) |
| 208 | telemetry u32 ×4: video reconnects, video bitrate kbps (currently echoes the configured value, not a measurement), audio reconnects, audio concealed (PLC) frames — free-running counters |

`heldKeys` bits = libnx `HidNpadButton` order: A=0 B=1 X=2 Y=3 LS=4 RS=5 L=6 R=7 ZL=8 ZR=9
Plus=10 Minus=11 Left=12 Up=13 Right=14 Down=15. Sticks are libnx's ±0x7FFF; triggers are digital.

## Virtual gamepads (`Input/VirtualGamepad.cs`)

- ViGEmBus (ScpVBus failed to load reliably). Xbox360 for Big Picture, DualShock4 for shadPS4
  (raw 63-byte report — the only path that carries gyro/accel).
- `SwapAbxy` (default true): Switch layout is diagonally mirrored vs Xbox/DS4.
- DS4 Y axes are inverted relative to Xbox; DS4 report timestamp is a fixed +188 per packet
  (calibration unverified).

### Switch buttons that never reach the client → host-side gestures

- **Minus+Plus → Guide / PS** (`Input/GuideButtonChord.cs`). A lone Minus/Plus is withheld up to
  100ms waiting for its partner (so Big Picture never sees a stray Back/Start before Guide); a
  tap released inside the window is still delivered; after the chord, both stay suppressed until
  both are released.
- **Hold Minus alone for 3s → back to the client's app menu** (any target). Detected on the **client**
  (`INPUT_MENU_HOLD_NS` in `Client/input/input.c`), which then closes the HID connection; the host
  just sees a normal session end and runs `OnSessionStopped` (closes the game, exits Big
  Picture). Nothing host-specific — but keep the gestures from colliding if you add new ones.

## Audio (PC → Switch)

WASAPI loopback (`NAudio`) → 20ms frames → Opus 128kbps (Concentus) → `RtpPacketizer`:
12-byte RTP header (V=2, PT 97, BE seq/timestamp/SSRC) + **2-byte BE length prefix** (not RTP) +
Opus payload, one unit per datagram. No PC-side A/V sync — **don't add any**; the streams are
independent by design.

## Library channel (PC → Switch, TCP 19003) — `Library/LibraryWire.cs` ↔ `Client/library/library.c`

**Protocol v2 (button layout sync):** the host header is 24 bytes - magic, version 2, u64 hash, u8 button
layout (1 = A/B X/Y by label), u8[3] zero, u32 layout revision - and the Switch replies 8 bytes - u8 need
payload, u8 its layout, u8[2] zero, u32 its revision (after adopting the host's when newer; a tie goes to
the host). The host adopts the Switch's value when its revision is higher (`AppOrchestrator
.AdoptSwitchButtonLayout`, raises `ButtonLayoutChanged` for the settings toggle); a PC-side change bumps
`Config.ButtonLayoutRevision` (`SetSwapAbxy`). This channel carries it because it is the only one open
while the Switch is in its menu, where its Y toggle lives (`Client/settings/settings.c`, stored in
`sdmc:/switch/SolarNX/settings.cfg`). The host still applies the swap itself; the HID packet is unchanged.

One exchange per connection, little-endian:
1. host → Switch: `u32 magic "ONXL" (0x4C584E4F)`, `u32 version (2)`, `u64 hash` (first 8 bytes of
   SHA-256 of the payload), `u8 button layout`, `u8[3]` zero, `u32 layout revision`.
2. Switch → host: `u8` 1 = send / 0 = already have this hash, `u8 button layout`, `u8[2]` zero,
   `u32 layout revision`.
3. host → Switch (if 1): `u32 count` (≤256), per entry `u32 id`, `u16 nameLen` (≤255) + UTF-8 name,
   `u32 coverLen` (≤512KB) + JPEG (0 = none). Host closes.

The Switch keeps the list in memory only (re-sent after every app start). Covers are normalized on
the host (`CoverImage`: 240×360 from the user-picked 2:3 region `GameEntry.CoverCrop` - set in
`UI/Library/CoverCropWindow` - or a centre crop, JPEG q85, ~20-40KB) so the client just decodes and
draws. The editor previews exactly that tile (`CoverLoader.LoadAsTile`). The payload is cached on the host until `GameLibrary.Changed`. Separate channel on purpose:
HID exists only during a session, the menu needs the list before one starts. Verified end-to-end
against a byte-level fake client (send on hash change, skip when unchanged).

## Video

Entirely owned by ffmpeg — `references/ffmpeg-pipeline.md`.
