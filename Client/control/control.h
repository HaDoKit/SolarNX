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
} HostMessage;

typedef enum {
    ClientMessage_Hello = 1, // CONTROL_VERSION_WIRE_SIZE bytes: this side's protocol and app version
} ClientMessage;

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
// A message from the PC (network.c for Wi-Fi, transport.c for Internet). Any thread.
void controlHandleHostMessage(uint8_t type, const uint8_t *payload, size_t length);

// Queues a message for the PC - sent by the HID thread after its next packet, only once the PC
// said Hello (Hello itself goes out regardless). Any thread; drops the message if the queue is full.
void controlQueueClientMessage(uint8_t type, const void *payload, size_t length);
// Takes the next queued message (HID thread). Returns false when there is none (or the PC hasn't
// said Hello yet, except for this side's own Hello).
bool controlTakeClientMessage(uint8_t *type, uint8_t *payload, size_t *length);
