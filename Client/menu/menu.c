#include "menu.h"

#include <stdlib.h>
#include <string.h>

#include <switch.h>
#include <SDL2/SDL_image.h>
#include <unistd.h>

#include "renderer.h"
#include "../input/input.h"
#include "../library/library.h"
#include "../settings/settings.h"
#include "../transport/transport.h"
#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"
#include "../common/lang/lang.h"
#include "../control/control.h"

#define MENU_OPTION_COUNT 3
#define MENU_OPTION_LIBRARY 2 // index of the "My games" tile

// NULL = the translated "My games" (see drawMenuFrame).
static const char *kOptionLabels[MENU_OPTION_COUNT] = {
    "Steam",
    "shadPS4",
    NULL,
};

static const char *kIconPaths[MENU_OPTION_COUNT] = {
    "romfs:/icons/steam.png",
    "romfs:/icons/shadps4.png",
    NULL, // drawn from the library itself (first cover) or a placeholder - see drawLibraryIcon
};

#define MENU_FRAME_NS 14285714ULL // ~70fps

// Library grid geometry (covers arrive from the host as 240x360, drawn at 2/3 size).
#define GRID_COLUMNS      6
#define GRID_COVER_W      160
#define GRID_COVER_H      240
#define GRID_GAP_X        28
#define GRID_ROW_PITCH    296
#define GRID_TOP          96
#define GRID_VISIBLE_ROWS 2
#define GRID_LABEL_SCALE  0.5f

static const SDL_Color kWhite = {255, 255, 255, 255};
static const SDL_Color kDim = {170, 170, 170, 255};
static const SDL_Color kGold = {255, 200, 60, 255};

static SDL_Texture *loadIcon(MenuContext *context, const char *path)
{
    if (path == NULL)
        return NULL;

    SDL_Surface *surface = IMG_Load(path);
    if (surface == NULL)
    {
        logWrite("menu", "loadIcon: failed to load %s: %s", path, IMG_GetError());
        return NULL;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(context->renderer, surface);
    if (texture == NULL)
    {
        logWrite("menu", "loadIcon: failed to create texture from %s: %s", path, SDL_GetError());
    }
    SDL_FreeSurface(surface);
    return texture;
}

static SDL_Texture *textureFromJpeg(MenuContext *context, const uint8_t *data, uint32_t size)
{
    if (data == NULL || size == 0)
        return NULL;
    SDL_Surface *surface = IMG_Load_RW(SDL_RWFromConstMem(data, (int)size), 1);
    if (surface == NULL)
        return NULL;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(context->renderer, surface);
    SDL_FreeSurface(surface);
    return texture;
}

static void drawScaledText(MenuContext *context, int x, int y, float scale, SDL_Color colour, const char *text)
{
    FC_DrawEffect(context->font, context->renderer, x, y,
        FC_MakeEffect(FC_ALIGN_LEFT, FC_MakeScale(scale, scale), colour), "%s", text);
}

static int scaledWidth(MenuContext *context, float scale, const char *text)
{
    return (int)(FC_GetWidth(context->font, "%s", text) * scale);
}

// Copies src into dst, cut on a UTF-8 character boundary with "..." appended if it doesn't fit in
// maxWidth pixels at the given scale.
static void fitText(MenuContext *context, char *dst, size_t dstSize, const char *src, int maxWidth, float scale)
{
    snprintf(dst, dstSize, "%s", src);
    if (scaledWidth(context, scale, dst) <= maxWidth)
        return;

    size_t len = strlen(dst);
    while (len > 0)
    {
        len--;
        while (len > 0 && ((unsigned char)dst[len] & 0xC0) == 0x80)
            len--; // step back over continuation bytes to a character start
        if (len + 4 > dstSize)
            continue;
        memcpy(dst + len, "...", 4);
        if (scaledWidth(context, scale, dst) <= maxWidth)
            return;
    }
}

// --- Library state for the menu: textures/names copied out of library.c under its lock ---

typedef struct {
    uint32_t id;
    char name[LIBRARY_MAX_NAME_BYTES + 1];
    SDL_Texture *cover;
} MenuGame;

typedef struct {
    MenuGame *games;
    int count;
    uint32_t version;
} MenuLibrary;

static void freeMenuLibrary(MenuLibrary *lib)
{
    for (int i = 0; i < lib->count; i++)
        if (lib->games[i].cover != NULL)
            SDL_DestroyTexture(lib->games[i].cover);
    free(lib->games);
    lib->games = NULL;
    lib->count = 0;
}

// Rebuilds the menu's copy if library.c holds a newer version. Cheap no-op otherwise.
static void refreshMenuLibrary(MenuContext *context, MenuLibrary *lib)
{
    // Read before taking the lock (libraryVersion() locks the same non-recursive mutex). If a newer
    // library lands in between, the copy below is newer than `version` and the next frame simply
    // rebuilds once more - harmless. version 0 = nothing received yet (lib starts zeroed).
    uint32_t version = libraryVersion();
    if (version == lib->version)
        return;

    freeMenuLibrary(lib);
    lib->version = version;

    libraryLock();
    int count = 0;
    const LibraryEntry *entries = libraryEntries(&count);
    if (count > 0)
    {
        lib->games = (MenuGame *)calloc((size_t)count, sizeof(MenuGame));
        if (lib->games != NULL)
        {
            for (int i = 0; i < count; i++)
            {
                lib->games[i].id = entries[i].id;
                snprintf(lib->games[i].name, sizeof(lib->games[i].name), "%s", entries[i].name);
                lib->games[i].cover = textureFromJpeg(context, entries[i].cover, entries[i].coverSize);
            }
            lib->count = count;
        }
    }
    libraryUnlock();
}

// --- App-selection tiles ---

static void drawLibraryIcon(MenuContext *context, const MenuLibrary *lib, SDL_Rect area)
{
    // First game with a cover, drawn as a 2:3 capsule centred in the icon area.
    for (int i = 0; i < lib->count; i++)
    {
        if (lib->games[i].cover == NULL)
            continue;
        int h = area.h;
        int w = h * 2 / 3;
        SDL_Rect dst = {area.x + (area.w - w) / 2, area.y, w, h};
        SDL_RenderCopy(context->renderer, lib->games[i].cover, NULL, &dst);
        return;
    }

    // Placeholder: a 2x2 grid of tiles.
    SDL_Color tile = {255, 255, 255, 60};
    int cell = area.h / 2 - 12;
    int startX = area.x + (area.w - (cell * 2 + 16)) / 2;
    int startY = area.y + (area.h - (cell * 2 + 16)) / 2;
    for (int r = 0; r < 2; r++)
        for (int c = 0; c < 2; c++)
            fillRoundedRect(context, startX + c * (cell + 16), startY + r * (cell + 16), cell, cell, 12, tile);
}

// Portrait rounded rect, thin border that turns gold when selected, dark translucent fill, icon
// in the upper area, label under it.
static void drawCard(MenuContext *context, int blockX, int blockY, int blockW, int blockH,
                      bool isSelected, SDL_Texture *icon, const MenuLibrary *lib, bool isLibraryTile, const char *label)
{
    const int cornerRadius = 20;
    const int borderThickness = 3;

    SDL_Color borderColour = isSelected ? kGold : (SDL_Color){255, 255, 255, 200};
    SDL_Color fillColour = {0, 0, 0, 130};

    fillRoundedRect(context, blockX, blockY, blockW, blockH, cornerRadius, fillColour);
    for (int i = 0; i < borderThickness; i++)
    {
        drawRoundedRect(context, blockX + i, blockY + i, blockW - i * 2, blockH - i * 2, cornerRadius, borderColour);
    }

    const int labelHeight = 70;
    const int iconPadding = 10;
    int iconAreaW = blockW - iconPadding * 2;
    int iconAreaH = blockH - labelHeight - iconPadding * 2;
    int iconSize = iconAreaW < iconAreaH ? iconAreaW : iconAreaH;
    SDL_Rect iconDest = {
        blockX + (blockW - iconSize) / 2,
        blockY + iconPadding + (iconAreaH - iconSize) / 2,
        iconSize, iconSize};

    if (isLibraryTile)
        drawLibraryIcon(context, lib, iconDest);
    else if (icon != NULL)
        SDL_RenderCopy(context->renderer, icon, NULL, &iconDest);

    SDL_Color labelColour = isSelected ? kWhite : (SDL_Color){210, 210, 210, 255};
    int labelWidth = FC_GetWidth(context->font, "%s", label);
    SDL_DrawText(context, blockX + blockW / 2 - labelWidth / 2, blockY + blockH - labelHeight + 14, labelColour, label);
}

// Bottom-right, opposite the version: the current A/B X/Y layout and the button that flips it.
// Synced with the PC's setting (see settings.h).
static void drawButtonLayoutHint(MenuContext *context)
{
    bool swapAbxy;
    uint32_t revision;
    settingsGetButtonLayout(&swapAbxy, &revision);
    const char *text = tr(swapAbxy ? Str_LayoutSwitch : Str_LayoutXbox);
    drawScaledText(context, MENU_RESX - 24 - scaledWidth(context, 0.5f, text), MENU_RESY - 34, 0.5f, kDim, text);

    // Above it: the connection mode (X opens the connection screen), with the Internet status.
    char connection[160];
    TransportStatus status = transportGetStatus();
    if (status == TransportStatus_Off)
        snprintf(connection, sizeof(connection), "%s", tr(Str_MenuSettingsWifi));
    else
        snprintf(connection, sizeof(connection), "%s%s", tr(Str_MenuInternetPrefix), transportStatusText(status));
    drawScaledText(context, MENU_RESX - 24 - scaledWidth(context, 0.5f, connection), MENU_RESY - 62, 0.5f,
        status == TransportStatus_Off || status == TransportStatus_Connected ? kDim : kGold, connection);

    // Above that: the PC's SolarNX, once it said (library channel / Internet Welcome) - gold when the
    // protocols differ, with which side to update.
    PcVersion pc;
    if (!controlGetPcVersion(&pc))
        return;
    char line[200];
    PcCompat compat = controlPcCompat();
    if (compat == PcCompat_UpdatePc || compat == PcCompat_UpdateSwitch)
        snprintf(line, sizeof(line), "%s", tr(compat == PcCompat_UpdatePc ? Str_UpdatePc : Str_UpdateSwitch));
    else if (pc.legacy)
        snprintf(line, sizeof(line), "%s", tr(Str_PcLegacy));
    else
        snprintf(line, sizeof(line), "%s%s%s", tr(Str_PcVersionPrefix), pc.appVersion,
            compat == PcCompat_PcOlder ? tr(Str_PcOlderSuffix) : compat == PcCompat_PcNewer ? tr(Str_PcNewerSuffix) : "");
    drawScaledText(context, MENU_RESX - 24 - scaledWidth(context, 0.5f, line), MENU_RESY - 90, 0.5f,
        compat == PcCompat_Same ? kDim : kGold, line);
}

// --- Settings screen (X): Wi-Fi / experimental Internet mode, PC address, pairing, language ---

#define CONNECTION_ROWS 8
enum { ROW_MODE, ROW_ADDRESS, ROW_SERVER, ROW_SERVER_KEY, ROW_PC_ID, ROW_PAIR, ROW_LANGUAGE, ROW_RUMBLE };

// The system keyboard applet; false if it couldn't be shown or the player cancelled.
static bool editText(const char *header, const char *current, char *out, size_t size)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetInitialText(&kbd, current);
    swkbdConfigSetStringLenMax(&kbd, (u32)(size - 1));
    Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc);
}

static void drawConnectionFrame(MenuContext *context, StarSystem *starSystem, int selected)
{
    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);
    starsystem_draw(context, starSystem);
    drawSolarNxTopBar(context);

    const char *title = tr(Str_SettingsTitle);
    SDL_DrawText(context, MENU_RESX - 24 - FC_GetWidth(context->font, "%s", title), 14, kWhite, title);

    SettingsConnectionMode mode = settingsGetConnectionMode();
    char address[SETTINGS_ADDRESS_MAX];
    settingsGetPcAddress(address, sizeof(address));
    uint8_t key[32];
    char hostId[SETTINGS_HOST_ID_SIZE + 1];
    char pairing[64];
    if (settingsGetPairing(key, hostId))
        snprintf(pairing, sizeof(pairing), "%s%s", tr(Str_PairedWithPrefix), hostId);
    else
        snprintf(pairing, sizeof(pairing), "%s", tr(Str_NotPaired));

    char server[SETTINGS_ADDRESS_MAX], serverKey[40], pcId[SETTINGS_HOST_ID_SIZE + 1];
    settingsGetServer(server, sizeof(server));
    settingsGetServerKey(serverKey, sizeof(serverKey));
    settingsGetPcId(pcId);

    // Each language by its own name, so it can be found whatever the current one is.
    SettingsLanguage language = settingsGetLanguage();
    const char *languageName = language == SettingsLanguage_Russian ? "Русский"
                             : language == SettingsLanguage_English ? "English" : tr(Str_LanguageAuto);

    const char *labels[CONNECTION_ROWS] = {
        tr(Str_RowMode), tr(Str_RowAddress), tr(Str_RowServer), tr(Str_RowServerKey), tr(Str_RowPcId), tr(Str_RowPair),
        tr(Str_RowLanguage), tr(Str_RowRumble),
    };
    const char *values[CONNECTION_ROWS] = {
        tr(mode == SettingsConnection_Internet ? Str_ModeInternet : Str_ModeWifi),
        address[0] != '\0' ? address : tr(Str_NotSet),
        server[0] != '\0' ? server : tr(Str_NoServer),
        tr(serverKey[0] != '\0' ? Str_KeySet : Str_KeyNone),
        pcId[0] != '\0' ? pcId : tr(Str_NotSet),
        pairing,
        languageName,
        tr(settingsGetRumble() == SettingsRumble_Strong ? Str_RumbleStrong
            : settingsGetRumble() == SettingsRumble_All ? Str_RumbleAll : Str_Off),
    };

    const int rowX = 160, rowW = MENU_RESX - 320, rowH = 46, rowTop = 84, rowPitch = 52;
    for (int i = 0; i < CONNECTION_ROWS; i++)
    {
        int y = rowTop + i * rowPitch;
        fillRoundedRect(context, rowX, y, rowW, rowH, 14, (SDL_Color){20, 21, 28, 220});
        if (i == selected)
            for (int t = 0; t < 3; t++)
                drawRoundedRect(context, rowX + t, y + t, rowW - t * 2, rowH - t * 2, 14, kGold);
        drawScaledText(context, rowX + 24, y + 9, 0.6f, kDim, labels[i]);
        drawScaledText(context, rowX + rowW - 24 - scaledWidth(context, 0.6f, values[i]), y + 9, 0.6f, kWhite, values[i]);
    }

    int y = rowTop + CONNECTION_ROWS * rowPitch + 10;
    const char *notes[] = {tr(Str_Note1), tr(Str_Note2), tr(Str_Note3)};
    for (int i = 0; i < 3; i++)
        drawScaledText(context, rowX, y + i * 28, 0.48f, kDim, notes[i]);
    y += 3 * 28 + 12;

    // The mode in use was fixed at startup (transport running or not); a changed setting needs a restart.
    bool internetRunning = transportGetStatus() != TransportStatus_Off;
    if ((mode == SettingsConnection_Internet) != internetRunning)
        drawScaledText(context, rowX, y, 0.55f, kGold, tr(Str_RestartNote));
    else if (internetRunning)
    {
        TransportStatus status = transportGetStatus();
        char line[160];
        snprintf(line, sizeof(line), "%s%s", tr(Str_StatusPrefix), transportStatusText(status));
        drawScaledText(context, rowX, y, 0.55f, status == TransportStatus_Connected || status == TransportStatus_Paired ? kWhite : kGold, line);
        if (status == TransportStatus_PairingCode)
        {
            // The numeric comparison: the same code must show on the PC before it's accepted there.
            uint32_t code = transportPairingCode();
            char text[32];
            snprintf(text, sizeof(text), "%03u %03u", (unsigned)(code / 1000), (unsigned)(code % 1000));
            int width = scaledWidth(context, 1.6f, text);
            drawScaledText(context, MENU_RESX - rowX - width, y - 20, 1.6f, kGold, text);
        }
    }

    const char *hint = tr(selected == ROW_PAIR ? Str_HintPair : Str_HintChange);
    drawScaledText(context, MENU_RESX / 2 - scaledWidth(context, 0.55f, hint) / 2, MENU_RESY - 34, 0.55f, kDim, hint);
    SDL_RenderPresent(context->renderer);
}

static void runConnectionScreen(MenuContext *context, StarSystem *starSystem, PadState *pad)
{
    int selected = 0;
    while (appMainLoop())
    {
        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);
        if (down & HidNpadButton_B)
            return;
        if ((down & HidNpadButton_AnyUp) && selected > 0)
            selected--;
        else if ((down & HidNpadButton_AnyDown) && selected + 1 < CONNECTION_ROWS)
            selected++;
        else if (down & HidNpadButton_A)
        {
            char current[SETTINGS_ADDRESS_MAX];
            char edited[SETTINGS_ADDRESS_MAX];
            if (selected == ROW_MODE)
            {
                settingsSetConnectionMode(settingsGetConnectionMode() == SettingsConnection_Internet
                    ? SettingsConnection_Wifi : SettingsConnection_Internet);
            }
            else if (selected == ROW_ADDRESS)
            {
                settingsGetPcAddress(current, sizeof(current));
                if (editText(tr(Str_KbdAddress), current, edited, sizeof(edited)))
                    settingsSetPcAddress(edited);
            }
            else if (selected == ROW_SERVER)
            {
                settingsGetServer(current, sizeof(current));
                if (editText(tr(Str_KbdServer), current, edited, sizeof(edited)))
                    settingsSetServer(edited);
            }
            else if (selected == ROW_SERVER_KEY)
            {
                settingsGetServerKey(current, sizeof(current));
                if (editText(tr(Str_KbdServerKey), current, edited, 33))
                    settingsSetServerKey(edited);
            }
            else if (selected == ROW_PC_ID)
            {
                settingsGetPcId(current);
                if (editText(tr(Str_KbdPcId), current, edited, SETTINGS_HOST_ID_SIZE + 1))
                    settingsSetPcId(edited);
            }
            else if (selected == ROW_LANGUAGE)
            {
                settingsCycleLanguage();
            }
            else if (selected == ROW_RUMBLE)
            {
                settingsCycleRumble();
            }
            else if (transportGetStatus() != TransportStatus_Off)
            {
                transportStartPairing(); // needs the Internet mode running - see the restart note otherwise
            }
        }

        float dt = rendererTick();
        starsystem_update(starSystem, dt);
        drawConnectionFrame(context, starSystem, selected);
        svcSleepThread(MENU_FRAME_NS);
    }
}

static void drawMenuFrame(MenuContext *context, StarSystem *starSystem, int selectedIndex,
                          SDL_Texture *icons[MENU_OPTION_COUNT], const MenuLibrary *lib)
{
    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);
    starsystem_draw(context, starSystem);

    drawSolarNxTopBar(context);

    const int blockW = 320;
    const int blockH = 420;
    const int blockGap = 50;
    const int blockY = (MENU_RESY - blockH) / 2;
    const int totalWidth = blockW * MENU_OPTION_COUNT + blockGap * (MENU_OPTION_COUNT - 1);
    const int firstBlockX = (MENU_RESX - totalWidth) / 2;

    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        int blockX = firstBlockX + i * (blockW + blockGap);
        drawCard(context, blockX, blockY, blockW, blockH, i == selectedIndex, icons[i], lib,
            i == MENU_OPTION_LIBRARY, kOptionLabels[i] != NULL ? kOptionLabels[i] : tr(Str_MyGames));
    }

    drawIpAddressLine(context);
    drawButtonLayoutHint(context);

    SDL_RenderPresent(context->renderer);
}

// --- Library grid screen ---

static void drawLibraryFrame(MenuContext *context, StarSystem *starSystem, const MenuLibrary *lib, int selected, int firstRow)
{
    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);
    starsystem_draw(context, starSystem);
    drawSolarNxTopBar(context);

    const char *title = tr(Str_MyGames);
    SDL_DrawText(context, MENU_RESX - 24 - FC_GetWidth(context->font, "%s", title), 14, kWhite, title);

    if (lib->count == 0)
    {
        const char *line1 = tr(lib->version == 0 ? Str_LibWaiting : Str_LibEmpty);
        const char *line2 = tr(Str_LibAddHint);
        SDL_DrawText(context, MENU_RESX / 2 - FC_GetWidth(context->font, "%s", line1) / 2, 300, kWhite, line1);
        drawScaledText(context, MENU_RESX / 2 - scaledWidth(context, 0.55f, line2) / 2, 360, 0.55f, kDim, line2);
    }
    else
    {
        const int gridWidth = GRID_COLUMNS * GRID_COVER_W + (GRID_COLUMNS - 1) * GRID_GAP_X;
        const int startX = (MENU_RESX - gridWidth) / 2;
        SDL_Color placeholder = {40, 40, 54, 255};

        for (int row = firstRow; row < firstRow + GRID_VISIBLE_ROWS; row++)
        {
            for (int col = 0; col < GRID_COLUMNS; col++)
            {
                int i = row * GRID_COLUMNS + col;
                if (i >= lib->count)
                    break;

                int x = startX + col * (GRID_COVER_W + GRID_GAP_X);
                int y = GRID_TOP + (row - firstRow) * GRID_ROW_PITCH;
                const MenuGame *g = &lib->games[i];

                if (g->cover != NULL)
                {
                    SDL_Rect dst = {x, y, GRID_COVER_W, GRID_COVER_H};
                    SDL_RenderCopy(context->renderer, g->cover, NULL, &dst);
                }
                else
                {
                    fillRoundedRect(context, x, y, GRID_COVER_W, GRID_COVER_H, 10, placeholder);
                }

                if (i == selected)
                    for (int t = 0; t < 4; t++)
                        drawRoundedRect(context, x - 4 + t, y - 4 + t, GRID_COVER_W + 8 - t * 2, GRID_COVER_H + 8 - t * 2, 12, kGold);

                char label[LIBRARY_MAX_NAME_BYTES + 4];
                fitText(context, label, sizeof(label), g->name, GRID_COVER_W, GRID_LABEL_SCALE);
                drawScaledText(context, x + (GRID_COVER_W - scaledWidth(context, GRID_LABEL_SCALE, label)) / 2,
                    y + GRID_COVER_H + 10, GRID_LABEL_SCALE, i == selected ? kWhite : kDim, label);
            }
        }
    }

    const char *hint = tr(lib->count > 0 ? Str_HintPlay : Str_HintBack);
    drawScaledText(context, MENU_RESX / 2 - scaledWidth(context, 0.55f, hint) / 2, MENU_RESY - 34, 0.55f, kDim, hint);

    SDL_RenderPresent(context->renderer);
}

// Blocking grid of the host's library. Returns true with *outId set when a game is picked with A,
// false on B or when the app is asked to exit.
static bool runLibraryScreen(MenuContext *context, StarSystem *starSystem, PadState *pad, MenuLibrary *lib, uint32_t *outId)
{
    int selected = 0;
    int firstRow = 0;

    while (appMainLoop())
    {
        refreshMenuLibrary(context, lib);
        if (selected >= lib->count)
            selected = lib->count > 0 ? lib->count - 1 : 0;

        padUpdate(pad);
        u64 down = padGetButtonsDown(pad);

        if (down & HidNpadButton_B)
            return false;
        if ((down & HidNpadButton_A) && lib->count > 0)
        {
            *outId = lib->games[selected].id;
            logWrite("menu", "library game picked: id %u (%s)", lib->games[selected].id, lib->games[selected].name);
            return true;
        }
        if (lib->count > 0)
        {
            if ((down & HidNpadButton_AnyLeft) && selected > 0)
                selected--;
            else if ((down & HidNpadButton_AnyRight) && selected + 1 < lib->count)
                selected++;
            else if ((down & HidNpadButton_AnyUp) && selected >= GRID_COLUMNS)
                selected -= GRID_COLUMNS;
            else if (down & HidNpadButton_AnyDown)
                selected = selected + GRID_COLUMNS < lib->count ? selected + GRID_COLUMNS : lib->count - 1;
        }

        int row = selected / GRID_COLUMNS;
        if (row < firstRow)
            firstRow = row;
        else if (row >= firstRow + GRID_VISIBLE_ROWS)
            firstRow = row - GRID_VISIBLE_ROWS + 1;

        float dt = rendererTick();
        starsystem_update(starSystem, dt);
        drawLibraryFrame(context, starSystem, lib, selected, firstRow);
        svcSleepThread(MENU_FRAME_NS);
    }
    return false;
}

SolarNxAppTarget runAppSelectMenu(MenuContext *context, StarSystem *starSystem)
{
    // padConfigureInput must run before any padInitialize/padUpdate call. On first start this menu
    // runs before input.c's inputInit(); mid-session main.c calls inputReconfigure() afterwards to
    // restore input.c's 4-player setup.
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);

    PadState menuPad;
    padInitializeDefault(&menuPad);

    SDL_Texture *icons[MENU_OPTION_COUNT];
    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        icons[i] = loadIcon(context, kIconPaths[i]);
    }

    MenuLibrary lib = {0};
    int selectedIndex = 0;
    SolarNxAppTarget chosen = SolarNxAppTarget_BigPicture;
    uint32_t chosenLibraryId = 0;
    bool picked = false;

    while (!picked && appMainLoop())
    {
        refreshMenuLibrary(context, &lib);

        padUpdate(&menuPad);
        u64 down = padGetButtonsDown(&menuPad);

        if (down & HidNpadButton_AnyLeft)
        {
            selectedIndex = (selectedIndex - 1 + MENU_OPTION_COUNT) % MENU_OPTION_COUNT;
        }
        else if (down & HidNpadButton_AnyRight)
        {
            selectedIndex = (selectedIndex + 1) % MENU_OPTION_COUNT;
        }
        else if (down & HidNpadButton_Y)
        {
            settingsToggleButtonLayout();
        }
        else if (down & HidNpadButton_X)
        {
            runConnectionScreen(context, starSystem, &menuPad);
            continue;
        }
        else if ((down & HidNpadButton_A) && controlPcIncompatible())
        {
            // Another major protocol: a session would show nothing. The line above the connection
            // mode says which side to update; A does nothing until then.
            logWrite("menu", "pick ignored - the PC's SolarNX is incompatible");
        }
        else if (down & HidNpadButton_A)
        {
            if (selectedIndex == MENU_OPTION_LIBRARY)
            {
                if (runLibraryScreen(context, starSystem, &menuPad, &lib, &chosenLibraryId))
                {
                    chosen = SolarNxAppTarget_Library;
                    picked = true;
                }
                continue; // B on the library screen comes back to the tiles
            }
            chosen = selectedIndex == 0 ? SolarNxAppTarget_BigPicture : SolarNxAppTarget_ShadPs4;
            picked = true;
            continue;
        }

        float dt = rendererTick();
        starsystem_update(starSystem, dt);
        drawMenuFrame(context, starSystem, selectedIndex, icons, &lib);
        svcSleepThread(MENU_FRAME_NS);
    }

    for (int i = 0; i < MENU_OPTION_COUNT; i++)
    {
        if (icons[i] != NULL) SDL_DestroyTexture(icons[i]);
    }
    freeMenuLibrary(&lib);

    logWrite("menu", "app target selected: %d", (int)chosen);
    inputSetAppTarget(chosen, chosenLibraryId);
    return chosen;
}
