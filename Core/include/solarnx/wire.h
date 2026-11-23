// SolarNX wire protocol - the single source of truth shared by every client built on
// libsolarnx-core. Must stay byte-for-byte identical to the PC host (SolarNX/, C#) and
// the Switch client (Client/network/network.h). Protocol majors refuse to stream; see
// PROTOCOL (currently 1.1) and SolarNX/Session/ProtocolVersion.cs.
//
// This file is part of SolarNX (GPL-3.0).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- ports (== SolarNX/Config.cs; the PC dials in on the TCP ones) ---
#define SNX_PORT_VIDEO    19000 // UDP, PC -> client, H.264/H.265 in SolarNX framing + RS parity
#define SNX_PORT_HID      19001 // TCP, client listens+accepts, PC dials in
#define SNX_PORT_AUDIO    19002 // UDP, PC -> client, RTP-ish framing
#define SNX_PORT_LIBRARY  19003 // TCP, client listens+accepts, PC pushes its game library
#define SNX_PORT_INTERNET 19010 // UDP, the whole encrypted session (Internet path)
#define SNX_PORT_RENDEZVOUS 19020 // UDP, rendezvous server (Internet path, optional)

// --- protocol / app version announced in handshakes ---
#define SNX_PROTOCOL_MAJOR 1
#define SNX_PROTOCOL_MINOR 1
#define SNX_VERSION_WIRE_SIZE  28 // u16 major, u16 minor, char[24] app version (zero-padded)
#define SNX_APP_VERSION_SIZE   24

// --- HID framing ---
// The host resyncs on this exact 8-byte marker, then reads exactly
// SNX_HID_PAYLOAD_SIZE bytes as one packet. Control frames use the other marker.
#define SNX_HID_MARKER_SIZE   8
#define SNX_HID_PAYLOAD_SIZE  224
#define SNX_HID_MARKER_BYTE     0xFF // 8x, before every HID payload
#define SNX_CONTROL_MARKER_BYTE 0xFE // 8x, before every control frame

// --- shared version block (every handshake: library header/reply, Hello control) ---
typedef struct {
    uint16_t major;
    uint16_t minor;
    char app[SNX_APP_VERSION_SIZE + 1];
} SnxPeerVersion;

// Which app the host should launch/focus (== SolarNX.Input.AppTarget).
typedef enum {
    SNX_APP_BIG_PICTURE = 0,
    SNX_APP_SHADPS4     = 1,
    SNX_APP_LIBRARY     = 2,
} SnxAppTarget;

// One controller's state, 44 bytes, little-endian (native on ARM64 and x86-64).
// Mirrors SolarNX.Input.ControllerState; motion is per-controller.
typedef struct {
    uint32_t heldKeys; // host reads the low 16 bits
    int32_t  lJoyX;
    int32_t  lJoyY;
    int32_t  rJoyX;
    int32_t  rJoyY;
    float    accelX, accelY, accelZ; // g
    float    gyroX, gyroY, gyroZ;    // rad/s
} SnxControllerState;

// Mirrors SolarNX.Input.HidPacket.Parse field-for-field. Exactly 224 bytes.
typedef struct {
    SnxControllerState controllers[4]; // 176 bytes
    uint32_t touchX1;
    uint32_t touchY1;
    uint32_t videoLostFrameCount;       // telemetry: free-running counters, host tracks deltas
    uint32_t videoRecoveredPacketCount;
    uint32_t libraryEntryId;            // when appTarget is SNX_APP_LIBRARY, else 0
    uint32_t controllerCount;
    uint32_t appTarget;                 // SnxAppTarget
    uint32_t displayHeight;             // 720 handheld / 1080 docked; host streams at that size
    uint32_t videoReconnectCount;
    uint32_t videoMeasuredBitrateKbps;
    uint32_t audioReconnectCount;
    uint32_t audioConcealedFrameCount;
} SnxHidPacket;

// Sent once by the host right after the HID TCP connection is accepted, before any
// SnxHidPacket traffic: 7x uint32 little-endian, 28 bytes, no framing.
#define SNX_SESSION_CONFIG_SIZE 28
typedef struct {
    uint32_t videoBitrateKbps;
    uint32_t videoWidth;
    uint32_t videoHeight;
    uint32_t videoFps;
    uint32_t audioBitrateKbps;
    uint32_t audioSampleRate;
    uint32_t audioChannels;
} SnxSessionConfig;

// --- library channel (TCP SNX_PORT_LIBRARY) ---
// host -> client  56 bytes: u32 magic "ONXL", u32 version (3), version[28], u64 content hash,
//                 u8 button layout, u8[3] zero, u32 layout revision, u32 extra length (skipped)
// client -> host  40 bytes: u8 send-it / u8 layout / u8[2] zero / u32 revision / version[28] /
//                 u32 extra length (0)
// host -> client  (only if send-it) u32 count, then per entry:
//                 u32 id, u16 nameLength, name (UTF-8), u32 coverLength, cover (JPEG)
// Version 2 (0.2.4-era host): 24-byte header, 8-byte reply, still answered.
#define SNX_LIBRARY_MAGIC         UINT32_C(0x4C584E4F) // "ONXL"
#define SNX_LIBRARY_VERSION       3u
#define SNX_LIBRARY_LEGACY_VERSION 2u
#define SNX_LIBRARY_MAX_ENTRIES   256
#define SNX_LIBRARY_MAX_NAME_BYTES   255
#define SNX_LIBRARY_MAX_COVER_BYTES (512u * 1024u)

#ifdef __cplusplus
}
#endif
