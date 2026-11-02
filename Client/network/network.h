#pragma once

#include <switch.h>
#include <stdbool.h>
#include <stdint.h>

// Ports must exactly match SolarNX/Config.cs on the PC host - see
// .claude/skills/solarnx-pc-architect/references/switch-protocol.md.
#define SOLARNX_VIDEO_PORT 19000 // UDP, PC -> Switch, MPEG-TS/H264 (opened via libavformat, video.c)
#define SOLARNX_AUDIO_PORT 19002 // UDP, PC -> Switch, custom RTP-ish framing (opened via avio, audio.c)
#define SOLARNX_HID_PORT   19001 // TCP, Switch listens+accepts, PC dials in (this module)
#define SOLARNX_LIBRARY_PORT 19003 // TCP, Switch listens+accepts, PC dials in to push its game library (library/library.c)

// The host (SolarNX/Input/HidProtocol.cs, HidFrameReader) resyncs on this exact 8-byte marker and
// then reads exactly SOLARNX_HID_PAYLOAD_SIZE bytes as one packet. Both the marker bytes and the
// payload size must stay byte-for-byte identical to the host or its parser silently desyncs -
// there is no build-time check across the two repos.
#define SOLARNX_HID_MARKER_SIZE  8
#define SOLARNX_HID_PAYLOAD_SIZE 224

// Mirrors SolarNX.Input.AppTarget (HidProtocol.cs) - which app the host should launch/focus,
// picked in menu.c and sent unchanged on every packet of the session. Library = a game from the
// host's own library (library/library.c); which one is in SolarNxHidPacket.libraryEntryId.
typedef enum : uint64_t {
    SolarNxAppTarget_BigPicture = 0,
    SolarNxAppTarget_ShadPs4    = 1,
    SolarNxAppTarget_Library    = 2,
} SolarNxAppTarget;

// One controller's state, 44 bytes, little-endian (native on both ARM64 and x86-64, so no
// byteswap is done anywhere in this module). Four of these sit back to back in SolarNxHidPacket.
// Mirrors SolarNX.Input.ControllerState / the per-controller loop in HidPacket.Parse. Each
// controller carries its own motion reading (accelX/Y/Z, gyroX/Y/Z) - earlier versions of this
// protocol had a single shared motion block for "the primary player" only; the host was updated
// to read motion per-controller alongside this client (see VirtualGamepad.cs's ApplyDs4State).
typedef struct {
    uint32_t heldKeys; // host only reads the low 16 bits (HidPacket.HeldKeys is a C# ushort)
    int32_t  lJoyX;
    int32_t  lJoyY;
    int32_t  rJoyX;
    int32_t  rJoyY;
    float    accelX, accelY, accelZ; // g
    float    gyroX, gyroY, gyroZ;    // rad/s
} SolarNxControllerState;

// Mirrors SolarNX.Input.HidPacket.Parse field-for-field (host repo, Input/HidProtocol.cs).
// Must stay exactly SOLARNX_HID_PAYLOAD_SIZE (224) bytes - enforced by a _Static_assert in
// network.c. touchX2/Y2 are on the wire but ignored by the host parser today; they're kept here
// purely so every field after them lands at the offset the host expects.
typedef struct {
    SolarNxControllerState controllers[4]; // 176 bytes
    uint32_t touchX1;
    uint32_t touchY1;
    uint32_t touchX2; // unused by host, keep for offset alignment
    uint32_t touchY2; // unused by host, keep for offset alignment
    uint32_t libraryEntryId; // GameEntry.Id when appTarget is Library, else 0 (was the never-read frameRate slot)
    uint32_t controllerCount;
    SolarNxAppTarget appTarget; // u64 - see SolarNxAppTarget's doc comment above

    // Link-quality telemetry, read by the host's AdaptiveBitrateController (Session/
    // AdaptiveBitrateController.cs) to decide whether to lower/raise the encoder bitrate. Each is a
    // free-running counter (never reset while the app is alive) - the host tracks its own deltas
    // between packets rather than expecting these to reset per-session, so a value only ever going
    // up is fine and simpler than coordinating a reset across a reconnect. Zero cost to fill in
    // (see video.c/audio.c's getters) - plain word reads, no locking, matching this file's existing
    // s_lastKnownBitrateKbps pattern.
    uint32_t videoReconnectCount;     // videoGetReconnectCount() - runStreamSession exits/retries
    uint32_t videoMeasuredBitrateKbps; // videoGetMeasuredBitrateKbps() - last observed stream bit_rate
    uint32_t audioReconnectCount;     // audioGetReconnectCount() - UDP session re-opens
    uint32_t audioConcealedFrameCount; // audioGetConcealedFrameCount() - cumulative PLC frames
} SolarNxHidPacket;

// Sent once by the host (SolarNX/Input/HidProtocol.cs's SessionConfig), immediately after the HID
// TCP connection is accepted and before any SolarNxHidPacket traffic starts flowing the other way.
// Mirrors SessionConfig field-for-field: 7x uint32 little-endian, 28 bytes, no framing/marker
// needed since it's always the first 28 bytes read from a freshly-accepted connection.
#define SOLARNX_SESSION_CONFIG_SIZE 28
typedef struct {
    uint32_t videoBitrateKbps;
    uint32_t videoWidth;
    uint32_t videoHeight;
    uint32_t videoFps;
    uint32_t audioBitrateKbps;
    uint32_t audioSampleRate;
    uint32_t audioChannels;
} SolarNxSessionConfig;

// Brings up the BSD socket service and opens+binds+listens the HID TCP socket on
// 0.0.0.0:SOLARNX_HID_PORT. Call once at startup, before networkAcceptHid().
bool networkInit(void);

// Blocks until the PC dials in (mirrors the host's SessionController, which connects out to
// ClientIp:HidPort and treats the connection as the whole session's lifetime), then blocks again
// reading the SOLARNX_SESSION_CONFIG_SIZE-byte SessionConfig the host sends immediately after
// connecting (see networkWaitForSessionConfig). Only one HID client is handled at a time; a second
// inbound attempt just waits in the listen backlog until the current client is closed. Returns
// false if the listen socket isn't up, or if the connection dropped before the full config
// arrived (the caller should treat this exactly like any other failed accept).
bool networkAcceptHid(void);

// Blocks until a session config has been read by networkAcceptHid() at least once (from any
// connection, past or present) and copies the most recently received one into *out. Call this
// before opening the first video/audio stream so their buffer sizing can use the host's real
// configured values instead of a guessed default - see video.c's use of this. Returns quickly
// (no real wait) once at least one config has ever arrived, including for reconnects long after
// the first. Returns false only if the app is exiting first. MAIN THREAD ONLY: it pumps appMainLoop()
// (common/appstate) while it waits.
bool networkWaitForSessionConfig(SolarNxSessionConfig *out);

// Stores the session config the PC sent - networkAcceptHid() does it for the TCP handshake,
// transport.c for the Internet mode's Welcome. Any thread.
void networkSetSessionConfig(const SolarNxSessionConfig *config);

// Sends the 8x0xFF marker followed by sizeof(SolarNxHidPacket) bytes of *pkt over the accepted
// HID socket, as one framed unit. Returns false if the send failed (e.g. the PC closed the
// connection) - the caller should then call networkCloseHidClient() and go back to
// networkAcceptHid(), mirroring the host's reconnect-on-drop behavior.
bool networkSendHid(const SolarNxHidPacket *pkt);

// Closes the current HID client socket (if any) without tearing down the listen socket, so the
// caller can go back to networkAcceptHid() for the next PC connection.
void networkCloseHidClient(void);

// Tears down the listen socket, any accepted client socket, and the BSD socket service. Call
// once at shutdown.
void networkExit(void);
