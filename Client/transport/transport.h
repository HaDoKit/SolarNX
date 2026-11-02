#pragma once

#include <stdbool.h>

#include "../network/network.h"

// Experimental "Internet" mode (settings.h): the whole session over one UDP socket, and this side
// sends first - to the PC's address from the settings, port SOLARNX_INTERNET_PORT by default. Wire
// format mirrors SolarNX/Session/InternetWire.cs; change both sides together.
//
// One thread owns the socket. It says Hello (with the connection code) every second for as long as
// the app runs - the keepalive that tells the PC where to send - and sorts what comes back:
// Video/Audio datagrams are forwarded unchanged to 127.0.0.1 on the Wi-Fi ports, where video.c and
// audio.c already listen, so neither needs to know which mode is on; the library and the button
// layout go to library.c/settings.c; Welcome carries the session config (networkSetSessionConfig).
// network.c routes the HID calls here in this mode.

#define SOLARNX_INTERNET_PORT 19010 // == SolarNX InternetSession.Port

typedef enum {
    TransportStatus_Off = 0,       // Wi-Fi mode
    TransportStatus_NoAddress,     // no PC address in the settings yet
    TransportStatus_BadAddress,    // the address didn't resolve
    TransportStatus_Waiting,       // saying hello, no answer from the PC
    TransportStatus_Connected,     // the PC answers
    TransportStatus_WrongCode,
    TransportStatus_Busy,          // another Switch is in a session with that PC
    TransportStatus_WrongVersion,  // the PC runs a different SolarNX version
} TransportStatus;

// Opens the socket and starts the thread. Call once at startup, after networkInit(), Internet mode only.
bool transportStart(void);

TransportStatus transportGetStatus(void);
const char *transportStatusText(TransportStatus status);

// Session use from network.c (the HID thread):
void transportBeginSession(void);                     // forget the previous session's Welcome/End
bool transportWaitWelcome(void);                      // block until the PC welcomes us (false = app exiting)
bool transportSendInput(const SolarNxHidPacket *pkt); // false once the PC ended the session or went silent
void transportEndSession(void);                       // tell the PC we left (Bye)
