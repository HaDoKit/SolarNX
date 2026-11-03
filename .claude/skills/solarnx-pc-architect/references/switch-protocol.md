# Switch protocol (PC side)

Counterpart: `Client/` in this repo (skill `solarnx-switch-client`, file
`Client/network/network.h`). No shared build — every layout below is mirrored by hand in C. Change
both sides in the same commit.

## Ports (`Config.cs`) and roles

| Field | Default | Transport | Roles |
|---|---|---|---|
| `VideoPort` | 19000 | UDP | VideoSender sends to `ClientIp:19000`; Switch listens |
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
| 184 | `videoLostFrameCount`, `videoRecoveredPacketCount` u32 (were touchX2/Y2) — Switch video FEC counters, logged as "VideoLink", lost frames feed adaptive bitrate |
| 192 | `libraryEntryId` u32 — `GameEntry.Id` when `appTarget` is Library, else 0 (was the never-read `frameRate`) |
| 196 | `controllerCount` u32 |
| 200 | `appTarget` u32 (`AppTarget`: 0 Big Picture, 1 shadPS4, 2 Library; unknown → Big Picture) |
| 204 | `displayHeight` u32 — Switch screen: 720 handheld, 1080 docked (0 = older client). With `Config.AutoResolution` the stream is 1280x720 / 1920x1080; `AppOrchestrator.FollowSwitchDisplay` restarts the encoder once a change has held 1s. Was the upper half of a u64 appTarget |
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

**Version 3 (protocol 1.1):** header 56 bytes = magic, u32 3, version[28], u64 hash, u8 layout, u8[3],
u32 revision, u32 extra length; reply 40 bytes = the v2 reply's 8, version[28], u32 extra length. On
another major the Switch replies need = 0 and takes neither the library nor the layout. v2 below is
still spoken to a 0.2.4 Switch.

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

## Protocol version and control messages (protocol 1.1, 0.2.5)

`PROTOCOL` at the repo root holds the wire protocol as `major.minor` - separate from `VERSION`, read by
`SolarNX.csproj` (assembly metadata -> `Session/ProtocolVersion.cs`), `Client/Makefile`
(`-DSOLARNX_PROTOCOL_MAJOR/MINOR` -> `Client/control/control.h`) and the release workflow (`release.json`
asset: version + protocol, read by `Update/UpdateChecker.cs`). **Bump the major** for anything an older
peer can't live with (video framing, HID packet, SessionConfig); **bump the minor** for additions an
older peer can ignore (a new control message, a field appended to a handshake) and gate their use on
`ProtocolVersion.Supports(peer, feature)`. Different majors refuse to stream (`SwitchCompatibility.Blocks`
on the PC, `controlPcIncompatible()` + the menu on the Switch); both say which side to update. 1.0 = 0.2.4,
which announces nothing.

Version exchange - the 28-byte `PeerVersion` wire form (u16 major, u16 minor, char[24] app version),
frozen across majors so an old side can always read a newer one's version:
- library header **v3** (56 bytes) / reply (40 bytes), see the library section; a 0.2.4 Switch drops
  the v3 header unanswered (RST - it leaves 32 bytes unread), and `LibraryPusher` retries with v2,
  re-trying v3 every 30s;
- Internet: Hello + version, Welcome's sealed payload = SessionConfig + version, Reject WrongVersion +
  the PC's version;
- the Hello control message at the start of every HID session.

Control messages (`Session/ControlWire.cs` <-> `Client/control/control.h`), sent only after the peer's
Hello in this session:
- Wi-Fi, PC -> Switch: after the SessionConfig on the HID TCP connection, frames `u16 length, u8 type,
  payload` (one writer task per connection, bounded queue, drop-oldest). The Switch reads them in its
  HID loop (`networkWaitForHost`: `poll` instead of the plain send-interval sleep).
- Wi-Fi, Switch -> PC: `8x0xFE marker, u16 length, u8 type, payload` between HID packets
  (`HidFrameReader` yields `HidFrame`s); a 0.2.4 host skips them as noise.
- Internet: sealed types 21 (Control, PC -> Switch) / 22 (ClientControl), payload `u8 type + payload`.

| PC -> Switch | Payload |
|---|---|
| 1 Hello | version[28] |
| 2 Rumble | 4 x (u8 large, u8 small) - XInput motors; Switch drives HD rumble (160/320 Hz, amplitude x0.8) |
| 3 Options | u8 flags (1 traffic priority -> Switch sets IP_TOS 0xB8 on its HID socket, 2 debug mode), u8[3] |
| 4 DebugInfo | debug mode: bitrate setting/running, size, fps, codec, encoder, resolution index, NVENC preset/tune index, flags, encoder name |
| 5 Pong | the Ping's 8 bytes |

| Switch -> PC | Payload |
|---|---|
| 1 Hello | version[28] |
| 2 Ping | u64 tick (debug mode, every 0.5s) |
| 3 SetSetting | u8 DebugSetting, u8[3], i32 value - the debug menu; `AppOrchestrator.ApplyRemoteSetting` saves it and restarts the encoder |

Traffic priority (`Native/QosMarking.cs`): DSCP via qWAVE (`QOSAddSocketToFlow` + `QOSSetFlow
(OutgoingDSCPValue)` - needs admin, SolarNX runs elevated; Windows ignores IP_TOS). Video AF41 (34),
audio + HID EF (46); loopback sockets (Internet relays) unmarked. Verified with pktmon on the NIC.

## Video

ffmpeg encodes (RTP on loopback); `Capture/VideoSender` sends SolarNX framing with Reed-Solomon parity to `ClientIp:19000` (Internet: relay 19011) — format and FEC rules in `references/ffmpeg-pipeline.md` ("Video transport with FEC"), mirrored by `Client/video/videowire.h`.

## Internet mode (experimental) - `Session/InternetSession.cs` ↔ `Client/transport/transport.c`

`Config.ConnectionMode` = Wifi (everything above) | Internet. Internet: the PC listens on **UDP 19010**,
the Switch sends first, so only that port needs forwarding; replies go to the address the Switch's
packets came from. Wire format v2 in `Session/InternetWire.cs` (8-byte header "SNXU", type, version 2).

**Crypto** (`Session/InternetCrypto.cs` ↔ `Client/transport/crypto.c`, verified byte-for-byte against
each other and Python `cryptography`): X25519 = TweetNaCl `crypto_scalarmult` ported to both sides
(.NET has no X25519; checked with the RFC 7748 vector), HKDF-SHA256, packets = header (AAD) + counter
u64 LE + AES-256-CTR (IV = counter BE in the high 8 bytes, block index low) + HMAC-SHA256 truncated to
16 bytes; 64-packet replay window per direction. libnx provides AES-CTR/HMAC/SHA-256 on the Switch.

**Pairing** (numeric comparison, only while the PC's PairingWindow is open, 2 min): PairStart (device
id, X25519 pub, nickname) → PairKey (PC pub + commit = SHA256(pcPub||swPub||nP)) → PairNonceS (nS, sent
only after the commit) → PairNonceP (nP) → both show code = SHA256("SolarNX pair code"||swPub||pcPub||nS
||nP)[0..4] LE % 1e6 → player confirms on the PC → PairResult (host id + HMAC proof). Pairing key =
HKDF(shared, nS||nP, "SolarNX pair key"). PC stores it DPAPI-encrypted in `paired-devices.json`
(`Session/PairedDevices`), the Switch in `settings.cfg` (device_id, pair_key, host_id).

**Session**: Hello (device id, nS) → Welcome (nP, host id, SessionConfig sealed at counter 1 with
header+nP+hostId as AAD); keys = HKDF(pairKey, nS||nP, "SolarNX session") → enc/mac S→P, enc/mac P→S.
Unknown device → Reject NotPaired. A repeated Hello with the same nS gets the cached Welcome; a new nS
means new keys (and ends an active session). Then everything is sealed: Input, Bye/End, Video/Audio
(the PC relays 127.0.0.1:19011/19012 → sealed; the Switch decrypts and forwards to 127.0.0.1:19000/
19002, so the media code is unchanged), LibOffer/LibReply + LibChunk/LibAck (go-back-N, 1100-byte
chunks, window 48), Keepalive/KeepaliveAck once a second; 3s without a valid packet → the Switch
starts over with a new Hello. A session starts on the first Input, ends on Bye/End or 3s without Input.
The mode is chosen when streaming is armed (PC) and at app start (Switch).

## Rendezvous server (experimental) - `Server/` (`solarnx-rendezvous`), wire format `Server/RendezvousWire.cs`

A separate .NET console app the user self-hosts on a VPS (UDP 19020; Linux/Windows single-file, Docker,
systemd unit; settings file/env/CLI - `docs/server.md`). `RendezvousWire.cs` is compiled into the PC app
too (linked in `SolarNX.csproj`). Header "SNXR", version 1. The PC (`InternetSession.RendezvousLoopAsync`)
sends Register (host id + access key) every 15s **from the session socket 19010**, so the server learns
the public mapping of exactly that socket. The Switch (`transport.c`) sends Lookup (PC id + key) → Peer
(status, PC public endpoint, relay allowed); at the same time the server sends the PC Introduce (Switch
endpoint) and the PC fires 3 `InternetWire.Type.Punch` packets towards it (hole punching). The session
then runs directly. If the Switch hears nothing valid for 5s after Peer and relaying is allowed, it wraps
everything in RelayUp → server → RelayDown to the PC (with the Switch endpoint); the PC answers with
RelayBack → RelayFrom (the PC keeps a set of relayed endpoints in `InternetSession.Send`). Relay is off by
default, rate-limited per Switch (token bucket); RelayBack is accepted only from a registered PC towards a
Switch that relayed to it recently. The server never sees plaintext - the Internet-mode crypto is end to
end. Verified locally: lookup with wrong/right key, punch, pairing + encrypted session + library entirely
through the relay.
