#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../network/network.h"

// Experimental "Internet" mode (settings.h): the whole session over one UDP socket, and this side
// sends first - to the PC's address from the settings, port SOLARNX_INTERNET_PORT by default. Wire
// format mirrors SolarNX/Session/InternetWire.cs, cryptography crypto.c; change both sides together.
//
// With a rendezvous server in the settings (Server/RendezvousWire.cs) the PC is found by its id
// instead: Lookup gives the PC's public address while the server tells the PC to punch towards us,
// and the session goes straight there. If nothing valid comes back within a few seconds and the
// server allows relaying, every datagram goes wrapped through the server instead (still encrypted
// end to end).
//
// One thread owns the socket. Pairing (once, started from the menu) runs a numeric-comparison
// exchange with the PC and leaves a pairing key in the settings. With a key it says Hello every
// second until the PC's Welcome checks out, then sends an encrypted Keepalive every second; three
// seconds without a valid packet from the PC start over with a fresh Hello (new session keys).
// Everything after the handshake is sealed (AES-256-CTR + HMAC-SHA256, replay window). Video and
// audio are decrypted and forwarded unchanged to 127.0.0.1 on the Wi-Fi ports, where video.c and
// audio.c already listen; the library and the button layout go to library.c/settings.c; the Welcome
// carries the session config (networkSetSessionConfig). network.c routes the HID calls here.

#define SOLARNX_INTERNET_PORT 19010 // == SolarNX InternetSession.Port
#define SOLARNX_RENDEZVOUS_PORT 19020 // the server's default port

typedef enum {
    TransportStatus_Off = 0,        // Wi-Fi mode
    TransportStatus_NoAddress,      // no PC address in the settings yet
    TransportStatus_BadAddress,     // the address didn't resolve
    TransportStatus_NotPaired,      // pair first (menu: X > Pair)
    TransportStatus_Waiting,        // saying hello, no answer from the PC
    TransportStatus_Connected,      // the PC answers, session keys agreed
    TransportStatus_Busy,           // another Switch is in a session with that PC
    TransportStatus_WrongVersion,   // the PC runs a different SolarNX version
    TransportStatus_Pairing,        // pairing: waiting for the PC
    TransportStatus_PairingClosed,  // pairing: not open on the PC yet
    TransportStatus_PairingCode,    // pairing: compare transportPairingCode() with the PC
    TransportStatus_Paired,         // pairing: done
    TransportStatus_PairingRefused, // pairing: refused on the PC
    TransportStatus_PairingFailed,  // pairing: timed out or the PC's answer didn't check out
    TransportStatus_ServerNotFound, // the rendezvous server's address didn't resolve
    TransportStatus_ServerNoAnswer, // the rendezvous server doesn't answer
    TransportStatus_ServerBadKey,   // the server refused the access key
    TransportStatus_ServerNoPc,     // the server doesn't know that PC id (PC off, or another server)
    TransportStatus_NoPcId,         // a server is set but no PC id yet
} TransportStatus;

// Opens the socket and starts the thread. Call once at startup, after networkInit(), Internet mode only.
bool transportStart(void);

TransportStatus transportGetStatus(void);
const char *transportStatusText(TransportStatus status);

// Pairing from the menu. The code is valid while the status is TransportStatus_PairingCode.
void transportStartPairing(void);
void transportCancelPairing(void);
uint32_t transportPairingCode(void);

// Session use from network.c (the HID thread):
void transportBeginSession(void);                     // forget the previous session's End
bool transportWaitWelcome(void);                      // block until connected (false = app exiting)
bool transportSendInput(const SolarNxHidPacket *pkt); // false once the PC ended the session or went silent
void transportEndSession(void);                       // tell the PC we left (Bye)
