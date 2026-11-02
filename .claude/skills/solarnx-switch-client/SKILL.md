---
name: solarnx-switch-client
description: This skill should be used when writing, reviewing, or reasoning about code in Client/**/*.c or Client/**/*.h (and Client/Makefile) in this repo — the Nintendo Switch homebrew client (devkitPro/libnx, C) of the SolarNX remote-play system, whose PC host lives in SolarNX/ in the same repo. Trigger on requests like "review this C code", "how does video decode work on the Switch side", "why is video split into decode and render threads", "add a new thread", "change the HID packet", "why is NVTEGRA readback slow", "how does the back-to-menu gesture work", or any work touching main.c, network/, video/, audio/, input/, menu/ or common/. This is Switch/libnx-specific systems C, not generic C advice.
---

# SolarNX (Nintendo Switch client) — architecture and platform guide

## What this app is

A homebrew app (devkitPro/libnx, builds `SolarNX-Switch.nro`/`.nsp`) that runs **on the Switch** as
the client of a remote-play system. The PC host is `SolarNX/` in this same repo (C#/.NET, skill
`solarnx-pc-architect`). There is **no shared build**: every wire format is mirrored by hand —
change both sides in the same commit (see `references/network-protocol.md`).

The player picks a target in the client's menu (Steam Big Picture, shadPS4, or "My games" — a cover
grid of the host's own library); the host launches it on the PC and streams the primary monitor
back.

| Stream | Transport | Port | This side |
|---|---|---|---|
| Video | UDP, MPEG-TS/H.264 High | 19000 | passive listener via libavformat (`video.c`) |
| HID | TCP | 19001 | **listens + accepts** (`network.c`); PC dials in, sends a 28-byte `SessionConfig`, then this side sends marker + 224-byte packet at ~43Hz |
| Audio | UDP, RTP-like + Opus | 19002 | raw BSD socket (`audio.c`) |
| Library | TCP | 19003 | **listens** from app start (`library/library.c`); host pushes its game list + covers when it changes |

## Layout

| Path | Responsibility |
|---|---|
| `main.c` | Startup, thread creation, stream ↔ menu loop, HID send loop (`startNetwork`) |
| `common/appstate/` | Single owner of `appletMainLoop()`; exit flag; back-to-menu request flag |
| `network/` | Socket service init, HID listen/accept/send, `SessionConfig` receive, wire structs + ports |
| `video/` | libavformat input, NVTEGRA hw decode, decode thread → render thread handoff, SDL2 output |
| `audio/` | UDP receive, RTP-ish parse, Opus decode + PLC, `audout` playback |
| `input/` | 4 pads, touch, per-player sixaxis → `SolarNxHidPacket`; hold-Minus back-to-menu gesture |
| `menu/` | App-selection screen (own SDL window/renderer, shared system font, star animation) + library cover grid |
| `library/` | Library receiver thread (port 19003), in-memory entries (id, name, JPEG cover) under a mutex |
| `settings/` | Settings on the SD card (`sdmc:/switch/SolarNX/settings.cfg`): the A/B X/Y layout + its revision (Y in the menu, synced with the PC over the library channel, higher revision wins); connection mode, PC address and connection code (X → Connection screen, swkbd) |
| `transport/` | Experimental Internet mode: one UDP socket to the PC (port 19010), Hello/Welcome with the connection code, Input/Bye, library chunks; forwards Video/Audio datagrams to 127.0.0.1:19000/19002 so `video.c`/`audio.c` are unchanged. `network.c` routes HID calls here when `settings` say Internet (read at startup). Mirrors `SolarNX/Session/InternetWire.cs` |
| `common/logger/` | `logWrite()` (stdout under a mutex; nxlink in DEBUG builds), FFmpeg `av_log` bridge |
| `common/fontcache/` | Third-party SDL_FontCache |
| `romfs/icons/` | Menu tile icons |

## Threads

| Thread | Stack | Priority | Core | Job |
|---|---|---|---|---|
| main | default | 0x2C | 1 (pinned) | menu, video render loop (SDL + VSYNC present), **only** caller of `appletMainLoop()` |
| video decode (per stream session) | `0x100000` | `0x2D` | 1 | `av_read_frame` → decode → NVTEGRA readback → newest-frame slot |
| HID | `0x4000` | 0x2C | 0 | accept, `inputRead()` + send every 23.3ms |
| audio | `0x8000` | 0x2C | 2 | recv, Opus decode, `audout` |
| library | `0x8000` | 0x2C | 0 | accept, receive library pushes (idle otherwise) |

Core 3 is left to the system. Cores were pinned after HID/audio degraded over long sessions while
sharing cores with video. `num_bsd_sessions = 12` (raised from 6 after a crash ~2s into concurrent
HID+video+audio load), `udp_rx_buf_size = 0x40000` (see network-protocol).

## Lifecycle (`main.c`)

romfs → `networkInit` (listen socket) → **menu** (exit here = clean shutdown) → `inputInit` → HID
thread → audio init + thread → loop { `videoInit` → `videoStart` (blocks) → `videoExit`; if the
player held Minus: wait for HID thread to park → menu → `inputReconfigure` → clear request }.
Holding **Minus alone for 3s** (`INPUT_MENU_HOLD_NS`, `input.c`) raises the request; the HID
thread then closes the host connection, which the host treats as a session end (closes the game,
exits Big Picture). For a **library** target the request is also raised when the *host* drops the
HID connection - that's how the host says "the game exited". The audio thread lives across menu round-trips; the HID thread is never joined
(process exit tears it down).

## Hard rules learned the hard way

- **`appletMainLoop()` only via `appMainLoop()`, only on the main thread.** It pops a shared queue
  and the exit request isn't latched by libnx: when the audio thread also called it, it could eat
  the exit request and the app never closed. Worker threads use `appIsRunning()`; hot callers
  rate-limit (`pumpAppletRateLimited` in `video.c`).
- **Never block the main thread on the network without an interrupt path.** libavformat UDP reads
  block indefinitely when no stream comes; the interrupt callback aborts them on exit/menu. Not the
  udp `timeout` option: that turns every host-side ffmpeg restart into a counted reconnect, which
  the host's adaptive bitrate reads as link trouble (feedback loop).
- **Never let VSYNC pace network reads.** One thread doing read→decode→readback→present dropped to
  ~43fps in games and overflowed the UDP fifo (~100 overruns/s → "water running down glass"
  artifact). Decode and render are separate threads; only the newest frame is shown.
- Don't pre-align the NVTEGRA readback target to 256 bytes: faster transfer, but it brought video
  glitches and then loss of HID on another core. The unaligned CPU-copy path is the stable one.
- Never pass host-supplied text (library game names) as a format string - `SDL_DrawText` uses
  `"%s"`; keep it that way in any new drawing helper.
- Don't log from hot paths unthrottled; FFmpeg's `av_log` is capped at ERROR (NVTEGRA logged a
  fallback line every frame) except UDP `overrun` warnings, which are the on-device proof of drops.
- Audio uses a raw socket, not `avio`: `avio_open2`/`avio_read_partial` crashed the process ~2s in,
  reproducibly, even with video disabled.
- On every reconnect path free everything (`AVFormatContext`, `AVCodecContext`, frames, threads):
  a past leak starved the *audio* thread of memory.
- New threads: choose stack/priority/core deliberately; don't copy a row from the table above.

## Build / debug

- `make` → `SolarNX-Switch.nro/.nsp` (`-O2`); `make DEBUG=1` → `SolarNX-Switch-debug.nro`
  (`-Og -DDEBUG`, `build_debug/`), which streams `logWrite` over nxlink.
- devkitPro make breaks on a path containing spaces (the repo lives under "visual code project");
  CI converts to a `/c/...` MSYS path, local builds may need a space-free copy.
- VS Code "Switch (Debug, logs via nxlink)" runs `Client/tools/nxlink-log.ps1`: it sends the .nro
  with plain `nxlink -a` and itself accepts the app's nxlinkStdio connection (TCP 28771), printing
  lines live and appending them to `Client/logs/nxlink-<time>.log`. Don't go back to piping
  `nxlink -s` into a file: nxlink then block-buffers and the log stalls right after startup. Each
  line is stamped with the PC's `HH:mm:ss.fff` — same format as
  the host's `SolarNX.log`.
- Release builds have **no log output at all** (stdout isn't redirected anywhere).
- Only pre-existing warnings are expected: `void main` and SDL_FontCache.

## Reference files

- **`references/network-protocol.md`** — ports, `SessionConfig`, `SolarNxHidPacket` layout,
  telemetry, socket options, buffer sizing
- **`references/decode-pipeline.md`** — libavformat input, NVTEGRA, decode/render split,
  reconnects, interrupt callback
- **`references/audio-pipeline.md`** — raw socket, RTP-ish framing, Opus PLC, `audout` buffering,
  known latency drift
- **`references/rendering-input.md`** — SDL2 output, menu screen, pads/touch/sixaxis, gestures
- **`references/platform-constraints.md`** — appstate, socket config, threads/cores, memory and
  alignment, no RTC
