#pragma once

#include <stdbool.h>

#include "../network/network.h"

// Configures pad/touch/sixaxis input for up to 4 controllers. Call once at startup.
bool inputInit(void);

// Sets the AppTarget (and, for SolarNxAppTarget_Library, the picked GameEntry id) every subsequent
// inputRead() will stamp onto the outgoing packet (see network.h) - chosen via menu.c's
// runAppSelectMenu() before the HID send loop (re)starts. Defaults to Big Picture if never called.
void inputSetAppTarget(SolarNxAppTarget target, uint32_t libraryEntryId);
SolarNxAppTarget inputGetAppTarget(void);

// Fills *pkt with the current live input state (buttons, sticks, touch, motion) for the HID send
// loop to transmit - call once per outgoing packet (see main.c's startNetwork()). Safe to call
// repeatedly; each call re-scans all input sources. Also watches for the hold-Minus "back to the
// menu" gesture and raises appRequestMenu() when it completes (see INPUT_MENU_HOLD_NS in input.c).
void inputRead(SolarNxHidPacket *pkt);

// Re-applies the 4-player pad configuration after menu.c's runAppSelectMenu() switched HID to its
// own single-player setup (when returning to the menu mid-session - see main.c). Only call while
// the HID thread is paused (not calling inputRead).
void inputReconfigure(void);

// The game's rumble from the PC (control message Rumble): motor levels 0-255 for player 0-3, XInput
// meaning (large = low frequency, small = high frequency). Any thread - inputRead() applies it to
// whatever controller the player has right now (Joy-Con pair, handheld, Pro Controller).
void inputSetRumble(int player, uint8_t large, uint8_t small);
// Stops every controller's rumble at once (the session ended). HID thread.
void inputStopRumble(void);

// Stops the sixaxis sensors started by inputInit(). Call once at shutdown.
void inputExit(void);
