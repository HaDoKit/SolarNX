#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Settings kept on the SD card (sdmc:/switch/SolarNX/settings.cfg). Thread-safe: the menu changes
// them, the library thread syncs them with the PC.
//
// Button layout (A/B and X/Y): true = by label (pressing A sends the game A), false = by position
// (Xbox layout). It can be changed both here and on the PC; the revision counter decides which
// change is newer - every change bumps it, and the side with the higher revision wins when the two
// sync over the library channel (see library.c). The host applies the swap itself; this side only
// stores and shows the setting.
//
// Connection: Wi-Fi (default - the PC finds this console on the local network) or the experimental
// Internet mode (this console dials the PC at pcAddress over UDP, see transport/transport.c, and
// presents connectionCode, shown in the PC's settings). The mode is read at startup - a change
// applies after restarting the app.

#define SETTINGS_ADDRESS_MAX 128
#define SETTINGS_CODE_MAX    16

typedef enum {
    SettingsConnection_Wifi = 0,
    SettingsConnection_Internet = 1,
} SettingsConnectionMode;

// Loads the file (defaults if missing). Call once at startup, before the library thread starts.
void settingsInit(void);

void settingsGetButtonLayout(bool *swapAbxy, uint32_t *revision);

// Flips the layout from the menu and saves it.
void settingsToggleButtonLayout(void);

// Adopts the PC's value when it is newer (higher revision; on a tie the PC wins), and saves it.
void settingsSyncButtonLayout(bool hostSwapAbxy, uint32_t hostRevision);

SettingsConnectionMode settingsGetConnectionMode(void);
void settingsSetConnectionMode(SettingsConnectionMode mode);

// "host" or "host:port" (port defaults to the PC's Internet-mode port).
void settingsGetPcAddress(char *out, size_t size);
void settingsSetPcAddress(const char *address);

void settingsGetConnectionCode(char *out, size_t size);
void settingsSetConnectionCode(const char *code);
