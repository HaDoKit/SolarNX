#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "../common/logger/logger.h"
#include "../transport/crypto.h"

#define SETTINGS_DIR  "sdmc:/switch/SolarNX"
#define SETTINGS_PATH SETTINGS_DIR "/settings.cfg"

static Mutex s_mutex;
static bool s_swapAbxy = true; // the PC's default too
static uint32_t s_layoutRevision = 0;
static SettingsConnectionMode s_connectionMode = SettingsConnection_Wifi;
static char s_pcAddress[SETTINGS_ADDRESS_MAX] = "";
static uint8_t s_deviceId[16];
static bool s_hasDeviceId = false;
static uint8_t s_pairKey[32];
static bool s_paired = false;
static char s_hostId[SETTINGS_HOST_ID_SIZE + 1] = "";
static char s_server[SETTINGS_ADDRESS_MAX] = "";
static char s_serverKey[33] = "";
static char s_pcId[SETTINGS_HOST_ID_SIZE + 1] = "";
static SettingsLanguage s_language = SettingsLanguage_Auto;
static SettingsRumble s_rumble = SettingsRumble_All;

static const char *const kLanguageNames[] = {"auto", "ru", "en"};

static void toHex(char *out, const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; i++)
        sprintf(out + 2 * i, "%02x", data[i]);
    out[2 * size] = '\0';
}

static bool fromHex(uint8_t *out, const char *hex, size_t size)
{
    if (strlen(hex) < 2 * size)
        return false;
    for (size_t i = 0; i < size; i++)
    {
        unsigned value;
        if (sscanf(hex + 2 * i, "%2x", &value) != 1)
            return false;
        out[i] = (uint8_t)value;
    }
    return true;
}

// Caller holds s_mutex. Plain "key=value" lines, so the file stays readable and editable by hand.
static void save(void)
{
    mkdir(SETTINGS_DIR, 0777); // exists when installed normally, not when sent over nxlink
    FILE *f = fopen(SETTINGS_PATH, "w");
    if (f == NULL)
    {
        logWrite("settings", "cannot write %s", SETTINGS_PATH);
        return;
    }
    fprintf(f, "button_layout_swap=%d\nbutton_layout_revision=%u\n", s_swapAbxy ? 1 : 0, (unsigned)s_layoutRevision);
    fprintf(f, "connection_mode=%s\npc_address=%s\n",
        s_connectionMode == SettingsConnection_Internet ? "internet" : "wifi", s_pcAddress);
    char hex[65];
    if (s_hasDeviceId)
    {
        toHex(hex, s_deviceId, sizeof(s_deviceId));
        fprintf(f, "device_id=%s\n", hex);
    }
    if (s_paired)
    {
        toHex(hex, s_pairKey, sizeof(s_pairKey));
        fprintf(f, "pair_key=%s\nhost_id=%s\n", hex, s_hostId);
    }
    fprintf(f, "server=%s\nserver_key=%s\npc_id=%s\n", s_server, s_serverKey, s_pcId);
    fprintf(f, "language=%s\n", kLanguageNames[s_language]);
    fprintf(f, "rumble=%d\n", (int)s_rumble);
    fclose(f);
}

// Copies the value after "key=" up to the end of the line, without the newline.
static bool readValue(const char *line, const char *key, char *out, size_t size)
{
    size_t keyLength = strlen(key);
    if (strncmp(line, key, keyLength) != 0 || line[keyLength] != '=')
        return false;
    snprintf(out, size, "%s", line + keyLength + 1);
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

void settingsInit(void)
{
    mutexInit(&s_mutex);
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f == NULL)
        return;
    char line[256];
    char mode[16];
    char hex[80];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        unsigned value;
        if (sscanf(line, "button_layout_swap=%u", &value) == 1)
            s_swapAbxy = value != 0;
        else if (sscanf(line, "button_layout_revision=%u", &value) == 1)
            s_layoutRevision = value;
        else if (sscanf(line, "rumble=%u", &value) == 1)
            s_rumble = value <= SettingsRumble_Strong ? (SettingsRumble)value : SettingsRumble_Strong;
        else if (readValue(line, "connection_mode", mode, sizeof(mode)))
            s_connectionMode = strcmp(mode, "internet") == 0 ? SettingsConnection_Internet : SettingsConnection_Wifi;
        else if (readValue(line, "pc_address", s_pcAddress, sizeof(s_pcAddress))) {}
        else if (readValue(line, "device_id", hex, sizeof(hex)))
            s_hasDeviceId = fromHex(s_deviceId, hex, sizeof(s_deviceId));
        else if (readValue(line, "pair_key", hex, sizeof(hex)))
            s_paired = fromHex(s_pairKey, hex, sizeof(s_pairKey));
        else if (readValue(line, "host_id", s_hostId, sizeof(s_hostId))) {}
        else if (readValue(line, "server", s_server, sizeof(s_server))) {}
        else if (readValue(line, "server_key", s_serverKey, sizeof(s_serverKey))) {}
        else if (readValue(line, "pc_id", s_pcId, sizeof(s_pcId))) {}
        else if (readValue(line, "language", mode, sizeof(mode)))
            s_language = strcmp(mode, "ru") == 0 ? SettingsLanguage_Russian
                       : strcmp(mode, "en") == 0 ? SettingsLanguage_English : SettingsLanguage_Auto;
    }
    fclose(f);
    logWrite("settings", "button layout: %s (revision %u), connection: %s",
        s_swapAbxy ? "by label" : "by position", (unsigned)s_layoutRevision,
        s_connectionMode == SettingsConnection_Internet ? "internet" : "wifi");
}

void settingsGetButtonLayout(bool *swapAbxy, uint32_t *revision)
{
    mutexLock(&s_mutex);
    *swapAbxy = s_swapAbxy;
    *revision = s_layoutRevision;
    mutexUnlock(&s_mutex);
}

void settingsToggleButtonLayout(void)
{
    mutexLock(&s_mutex);
    s_swapAbxy = !s_swapAbxy;
    s_layoutRevision++;
    save();
    mutexUnlock(&s_mutex);
}

void settingsSyncButtonLayout(bool hostSwapAbxy, uint32_t hostRevision)
{
    mutexLock(&s_mutex);
    if (hostRevision > s_layoutRevision || (hostRevision == s_layoutRevision && hostSwapAbxy != s_swapAbxy))
    {
        s_swapAbxy = hostSwapAbxy;
        s_layoutRevision = hostRevision;
        save();
        logWrite("settings", "button layout from the PC: %s (revision %u)", s_swapAbxy ? "by label" : "by position", (unsigned)s_layoutRevision);
    }
    mutexUnlock(&s_mutex);
}

SettingsConnectionMode settingsGetConnectionMode(void)
{
    mutexLock(&s_mutex);
    SettingsConnectionMode mode = s_connectionMode;
    mutexUnlock(&s_mutex);
    return mode;
}

void settingsSetConnectionMode(SettingsConnectionMode mode)
{
    mutexLock(&s_mutex);
    s_connectionMode = mode;
    save();
    mutexUnlock(&s_mutex);
}

void settingsGetPcAddress(char *out, size_t size)
{
    mutexLock(&s_mutex);
    snprintf(out, size, "%s", s_pcAddress);
    mutexUnlock(&s_mutex);
}

void settingsSetPcAddress(const char *address)
{
    mutexLock(&s_mutex);
    snprintf(s_pcAddress, sizeof(s_pcAddress), "%s", address);
    save();
    mutexUnlock(&s_mutex);
}

void settingsGetDeviceId(uint8_t out[16])
{
    mutexLock(&s_mutex);
    if (!s_hasDeviceId)
    {
        cryptoRandom(s_deviceId, sizeof(s_deviceId));
        s_hasDeviceId = true;
        save();
    }
    memcpy(out, s_deviceId, sizeof(s_deviceId));
    mutexUnlock(&s_mutex);
}

bool settingsGetPairing(uint8_t key[32], char hostId[SETTINGS_HOST_ID_SIZE + 1])
{
    mutexLock(&s_mutex);
    bool paired = s_paired;
    if (paired)
    {
        memcpy(key, s_pairKey, sizeof(s_pairKey));
        snprintf(hostId, SETTINGS_HOST_ID_SIZE + 1, "%s", s_hostId);
    }
    mutexUnlock(&s_mutex);
    return paired;
}

void settingsSetPairing(const uint8_t key[32], const char *hostId)
{
    mutexLock(&s_mutex);
    memcpy(s_pairKey, key, sizeof(s_pairKey));
    snprintf(s_hostId, sizeof(s_hostId), "%s", hostId);
    snprintf(s_pcId, sizeof(s_pcId), "%s", hostId); // the server finds the paired PC by this id
    s_paired = true;
    save();
    mutexUnlock(&s_mutex);
}

void settingsGetServer(char *out, size_t size)
{
    mutexLock(&s_mutex);
    snprintf(out, size, "%s", s_server);
    mutexUnlock(&s_mutex);
}

void settingsSetServer(const char *server)
{
    mutexLock(&s_mutex);
    snprintf(s_server, sizeof(s_server), "%s", server);
    save();
    mutexUnlock(&s_mutex);
}

void settingsGetServerKey(char *out, size_t size)
{
    mutexLock(&s_mutex);
    snprintf(out, size, "%s", s_serverKey);
    mutexUnlock(&s_mutex);
}

void settingsSetServerKey(const char *key)
{
    mutexLock(&s_mutex);
    snprintf(s_serverKey, sizeof(s_serverKey), "%s", key);
    save();
    mutexUnlock(&s_mutex);
}

void settingsGetPcId(char out[SETTINGS_HOST_ID_SIZE + 1])
{
    mutexLock(&s_mutex);
    snprintf(out, SETTINGS_HOST_ID_SIZE + 1, "%s", s_pcId);
    mutexUnlock(&s_mutex);
}

void settingsSetPcId(const char *id)
{
    mutexLock(&s_mutex);
    snprintf(s_pcId, sizeof(s_pcId), "%s", id);
    for (char *c = s_pcId; *c; c++)
        if (*c >= 'a' && *c <= 'z')
            *c = (char)(*c - 32);
    save();
    mutexUnlock(&s_mutex);
}

SettingsRumble settingsGetRumble(void)
{
    mutexLock(&s_mutex);
    SettingsRumble rumble = s_rumble;
    mutexUnlock(&s_mutex);
    return rumble;
}

void settingsCycleRumble(void)
{
    mutexLock(&s_mutex);
    s_rumble = s_rumble == SettingsRumble_All ? SettingsRumble_Strong
             : s_rumble == SettingsRumble_Strong ? SettingsRumble_Off : SettingsRumble_All;
    save();
    mutexUnlock(&s_mutex);
}

SettingsLanguage settingsGetLanguage(void)
{
    mutexLock(&s_mutex);
    SettingsLanguage language = s_language;
    mutexUnlock(&s_mutex);
    return language;
}

void settingsCycleLanguage(void)
{
    mutexLock(&s_mutex);
    s_language = (SettingsLanguage)((s_language + 1) % 3);
    save();
    mutexUnlock(&s_mutex);
}
