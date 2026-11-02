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
    fclose(f);
}

void settingsInit(void)
{
    mutexInit(&s_mutex);
    FILE *f = fopen(SETTINGS_PATH, "r");
    if (f == NULL)
        return;
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        unsigned value;
        if (sscanf(line, "button_layout_swap=%u", &value) == 1)
            s_swapAbxy = value != 0;
        else if (sscanf(line, "button_layout_revision=%u", &value) == 1)
            s_layoutRevision = value;
    }
    fclose(f);
    logWrite("settings", "button layout: %s (revision %u)", s_swapAbxy ? "by label" : "by position", (unsigned)s_layoutRevision);
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
