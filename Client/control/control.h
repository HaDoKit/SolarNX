#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The PC <-> Switch wire protocol as "major.minor" (../PROTOCOL via the Makefile) and the control
// messages of a session. Mirrors SolarNX/Session/ProtocolVersion.cs and ControlWire.cs - change both
// sides together.
//
// - Major: bumped by a change the other side can't live with. Different majors don't stream; both
//   sides say which one to update.
// - Minor: bumped by an addition an older peer can ignore; the newer side then doesn't use it.
// 1.0 is what 0.2.4 speaks without saying so; 1.1 added the version exchange and control messages.
//
// Control messages ride the session's own channel:
//   Wi-Fi, PC -> here:   after the 28-byte SessionConfig on the HID TCP connection, frames of
//                        u16 length (type + payload), u8 type, payload (network.c reads them).
//   Wi-Fi, here -> PC:   8x0xFE marker, u16 length, u8 type, payload - between HID packets.
//   Internet:            sealed types 21 (PC -> here) / 22 (here -> PC), u8 type + payload.
// Each side opens with Hello; nothing else is sent before the peer's Hello arrived.

#ifndef SOLARNX_PROTOCOL_MAJOR
#error "SOLARNX_PROTOCOL_MAJOR/MINOR come from ../PROTOCOL via the Makefile"
#endif

#define CONTROL_MAX_PAYLOAD 1024
#define CONTROL_VERSION_WIRE_SIZE 28 // u16 major, u16 minor, char[24] app version (zero-padded)
#define CONTROL_APP_VERSION_SIZE 24

typedef enum {
    HostMessage_Hello = 1, // CONTROL_VERSION_WIRE_SIZE bytes: the PC's protocol and app version
    HostMessage_Rumble = 2, // 8 bytes: per player 1-4, u8 large motor, u8 small motor (input.c)
    HostMessage_Options = 3, // 4 bytes: u8 flags (CONTROL_OPTION_*), u8[3] zero
    HostMessage_DebugInfo = 4, // debug mode: what the overlay and the debug menu show (ControlDebugInfo)
    HostMessage_Pong = 5,      // debug mode: the 8 bytes of our Ping, echoed
} HostMessage;

#define CONTROL_OPTION_TRAFFIC_PRIORITY 0x01 // mark this side's input packets DSCP EF (network.c)
#define CONTROL_OPTION_DEBUG            0x02 // debug mode: overlay, debug menu, pings

typedef enum {
    ClientMessage_Hello = 1, // CONTROL_VERSION_WIRE_SIZE bytes: this side's protocol and app version
    ClientMessage_Ping = 2,  // debug mode: u64 armGetSystemTick() - the PC echoes it in a Pong
    ClientMessage_SetSetting = 3, // debug menu: u8 DebugSetting, u8[3] zero, i32 value
} ClientMessage;

// What the debug menu can change on the PC - mirrors SolarNX ControlWire.DebugSetting.
typedef enum {
    DebugSetting_BitrateKbps = 1,     // kbps, 1000..50000
    DebugSetting_AdaptiveBitrate = 2, // 0/1
    DebugSetting_Codec = 3,           // 0 H.265, 1 H.264
    DebugSetting_Resolution = 4,      // 0 auto, 1.. = CONTROL_RESOLUTIONS + 1
    DebugSetting_Fps = 5,             // one of CONTROL_FPS
    DebugSetting_NvencPreset = 6,     // 0..6 = p1..p7
    DebugSetting_NvencTune = 7,       // 0 hq, 1 ll, 2 ull
    DebugSetting_TrafficPriority = 8, // 0/1
    DebugSetting_AdaptivePreset = 9,  // 0/1 - a manual preset/tuning turns it off on the PC
} DebugSetting;

// The PC settings' fixed lists, in order (ControlWire.DebugResolutions / DebugFps,
// EncoderSelection.NvencPresets / NvencTunes on the PC).
#define CONTROL_RESOLUTION_COUNT 5
extern const int kControlResolutions[CONTROL_RESOLUTION_COUNT][2];
#define CONTROL_FPS_COUNT 4
extern const int kControlFps[CONTROL_FPS_COUNT];
#define CONTROL_PRESET_COUNT 7
#define CONTROL_TUNE_COUNT 3
extern const char *const kControlTunes[CONTROL_TUNE_COUNT];

#define DEBUG_INFO_ADAPTIVE 0x01
#define DEBUG_INFO_PRIORITY 0x02
#define DEBUG_INFO_NVENC    0x04
#define DEBUG_INFO_ADAPTIVE_PRESET 0x08

// HostMessage_DebugInfo, decoded. seq goes up with every one received (the menu uses it to know its
// change has been applied).
typedef struct {
    bool valid;
    uint32_t seq;
    uint32_t bitrateKbps;    // the setting (the ceiling with adaptive bitrate)
    uint32_t runningKbps;    // what the encoder runs at now (0 = not streaming)
    uint16_t width, height;  // running
    uint8_t fps;
    uint8_t codec;           // running: 0 H.265, 1 H.264
    uint8_t codecSetting;
    uint8_t encoder;         // SolarNX EncoderKind
    uint8_t resolution;      // 0 auto, 1.. = kControlResolutions + 1
    uint8_t preset;          // 0..6
    uint8_t tune;            // 0..2
    uint8_t flags;           // DEBUG_INFO_*
    char encoderName[61];
} ControlDebugInfo;

// What the PC said about itself. legacy = it didn't say (0.2.4 or earlier: protocol 1.0 assumed).
typedef struct {
    bool known;
    bool legacy;
    uint16_t major;
    uint16_t minor;
    char appVersion[CONTROL_APP_VERSION_SIZE + 1];
} PcVersion;

typedef enum {
    PcCompat_Unknown = 0,
    PcCompat_Same,       // same protocol
    PcCompat_PcOlder,    // same major, the PC's minor is older - works without what it lacks
    PcCompat_PcNewer,    // same major, the PC's minor is newer - works without what this side lacks
    PcCompat_UpdatePc,   // another major, the PC's is older - update SolarNX on the PC
    PcCompat_UpdateSwitch, // another major, the PC's is newer - update this app
} PcCompat;

void controlInit(void);

// This side's version in the shared wire form (CONTROL_VERSION_WIRE_SIZE bytes).
void controlWriteOwnVersion(uint8_t *out);
// Parses the wire form into *out (known = true, legacy = false).
void controlReadVersion(const uint8_t *wire, PcVersion *out);

// What the PC last said about itself (library channel, Internet Welcome/Reject, session Hello).
// Any thread.
void controlSetPcVersion(const PcVersion *version);
bool controlGetPcVersion(PcVersion *out);
PcCompat controlPcCompat(void);
bool controlPcIncompatible(void); // another major - don't start a session

// --- Session ---

// A new HID connection/session: forget the previous one's Hello and anything still queued.
void controlSessionReset(void);
// True once the PC said Hello in this session (and supports control messages at all).
bool controlPeerReady(void);
// Internet mode: the Welcome carried the PC's version instead of a Hello - same effect.
void controlAssumePeerReady(void);
// The last Options flags from the PC (0 before any). Any thread.
uint8_t controlOptions(void);
// Debug mode is on (the PC said so in this session). Any thread.
bool controlDebugEnabled(void);
bool controlGetDebugInfo(ControlDebugInfo *out);
// Debug mode: queues a Ping (HID thread, a couple of times a second); the Pong updates the round trip.
void controlSendPing(void);
uint32_t controlRttUs(void); // smoothed round trip to the PC and back, 0 = not measured yet
// Rumble messages received so far (free-running) and the last levels (4 x large, small) - debug overlay.
uint32_t controlRumbleStats(uint8_t levels[8]);
// Debug menu: asks the PC to change a setting.
void controlSendSetting(uint8_t setting, int32_t value);
// A message from the PC (network.c for Wi-Fi, transport.c for Internet). Any thread.
void controlHandleHostMessage(uint8_t type, const uint8_t *payload, size_t length);

// Queues a message for the PC - sent by the HID thread after its next packet, only once the PC
// said Hello (Hello itself goes out regardless). Any thread; drops the message if the queue is full.
void controlQueueClientMessage(uint8_t type, const void *payload, size_t length);
// Takes the next queued message (HID thread). Returns false when there is none (or the PC hasn't
// said Hello yet, except for this side's own Hello).
bool controlTakeClientMessage(uint8_t *type, uint8_t *payload, size_t *length);
