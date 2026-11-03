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
// Internet mode (this console dials the PC at pcAddress over UDP, see transport/transport.c). Internet
// mode needs pairing once: it leaves a 32-byte pairing key and the PC's host id here, next to this
// console's random device id. The mode is read at startup - a change applies after restarting the app.

#define SETTINGS_ADDRESS_MAX 128
#define SETTINGS_HOST_ID_SIZE 8

typedef enum {
    SettingsConnection_Wifi = 0,
    SettingsConnection_Internet = 1,
} SettingsConnectionMode;

// Interface language (see common/lang): Auto follows the console's language.
typedef enum {
    SettingsLanguage_Auto = 0,
    SettingsLanguage_Russian = 1,
    SettingsLanguage_English = 2,
} SettingsLanguage;

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

// This console's id for pairing - 16 random bytes, made on first use.
void settingsGetDeviceId(uint8_t out[16]);

// The pairing with the PC: false if not paired. hostId gets SETTINGS_HOST_ID_SIZE chars + NUL.
bool settingsGetPairing(uint8_t key[32], char hostId[SETTINGS_HOST_ID_SIZE + 1]);
void settingsSetPairing(const uint8_t key[32], const char *hostId);

// Optional rendezvous server ("host" or "host:port", default port 19020) and its access key: with a
// server set, the PC is found by its id (settingsGetPcId) instead of pcAddress.
void settingsGetServer(char *out, size_t size);
void settingsSetServer(const char *server);
void settingsGetServerKey(char *out, size_t size);
void settingsSetServerKey(const char *key);
// The PC's id on the server: typed in before the first pairing, the paired PC's host id afterwards.
void settingsGetPcId(char out[SETTINGS_HOST_ID_SIZE + 1]);
void settingsSetPcId(const char *id);

// Rumble from the PC's games on this console's controllers - X > Settings (default All). Strong only
// drops a player's rumble below a threshold - meant for weak background rumble, but in at least one
// game (Phantom Blade Zero) walking is the strong one and hits the weak one, so it's not the default.
typedef enum {
    SettingsRumble_Off = 0,
    SettingsRumble_All = 1,
    SettingsRumble_Strong = 2,
} SettingsRumble;

SettingsRumble settingsGetRumble(void);
// All -> Strong only -> Off -> All, saved.
void settingsCycleRumble(void);

SettingsLanguage settingsGetLanguage(void);
// Auto -> Russian -> English -> Auto, saved; applies at once.
void settingsCycleLanguage(void);
