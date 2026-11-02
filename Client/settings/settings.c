#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include "../common/logger/logger.h"

#define SETTINGS_DIR  "sdmc:/switch/SolarNX"
#define SETTINGS_PATH SETTINGS_DIR "/settings.cfg"

static Mutex s_mutex;
static bool s_swapAbxy = true; // the PC's default too
static uint32_t s_layoutRevision = 0;
static SettingsConnectionMode s_connectionMode = SettingsConnection_Wifi;
static char s_pcAddress[SETTINGS_ADDRESS_MAX] = "";
static char s_connectionCode[SETTINGS_CODE_MAX + 1] = "";

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
    fprintf(f, "connection_mode=%s\npc_address=%s\nconnection_code=%s\n",
        s_connectionMode == SettingsConnection_Internet ? "internet" : "wifi", s_pcAddress, s_connectionCode);
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
    while (fgets(line, sizeof(line), f) != NULL)
    {
        unsigned value;
        if (sscanf(line, "button_layout_swap=%u", &value) == 1)
            s_swapAbxy = value != 0;
        else if (sscanf(line, "button_layout_revision=%u", &value) == 1)
            s_layoutRevision = value;
        else if (readValue(line, "connection_mode", mode, sizeof(mode)))
            s_connectionMode = strcmp(mode, "internet") == 0 ? SettingsConnection_Internet : SettingsConnection_Wifi;
        else if (readValue(line, "pc_address", s_pcAddress, sizeof(s_pcAddress))) {}
        else if (readValue(line, "connection_code", s_connectionCode, sizeof(s_connectionCode))) {}
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

void settingsGetConnectionCode(char *out, size_t size)
{
    mutexLock(&s_mutex);
    snprintf(out, size, "%s", s_connectionCode);
    mutexUnlock(&s_mutex);
}

void settingsSetConnectionCode(const char *code)
{
    mutexLock(&s_mutex);
    snprintf(s_connectionCode, sizeof(s_connectionCode), "%s", code);
    save();
    mutexUnlock(&s_mutex);
}
