#include "renderer.h"

#include <math.h>
#include <stdlib.h>
#include <unistd.h>

#include <SDL2/SDL_image.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#include "../common/logger/logger.h"

// Frame-timing state for rendererTick()'s dt, in Switch system ticks (svcGetSystemTick's units,
// not seconds - divided down below). Separate from any timing video.c/main.c already do: this
// only needs to run while a MenuContext-based screen (menu, splash) is actually on screen.
static u64 s_lastTick = 0;

float rendererTick(void)
{
    u64 now = svcGetSystemTick();
    if (s_lastTick == 0)
    {
        s_lastTick = now;
        return 0.0f;
    }

    float dt = (float)(now - s_lastTick) / 19200000.0f; // matches svcGetSystemTick's known frequency
    s_lastTick = now;

    // Guards against a huge dt after a stall (e.g. a debugger breakpoint, or the app losing focus
    // and coming back) feeding starsystem_update a jump large enough to fling a planet's position.
    if (dt > 1.0f)
        dt = 1.0f;

    return dt;
}

static SDL_Texture *loadTexture(MenuContext *context, const char *path)
{
    SDL_Surface *surface = IMG_Load(path);
    if (surface == NULL)
    {
        logWrite("menu", "loadTexture: failed to load %s: %s", path, IMG_GetError());
        return NULL;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(context->renderer, surface);
    SDL_FreeSurface(surface);
    if (texture == NULL)
        logWrite("menu", "loadTexture: failed to create texture from %s: %s", path, SDL_GetError());
    return texture;
}

MenuContext *makeRenderer(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        logWrite("menu", "makeRenderer: SDL_Init failed: %s", SDL_GetError());
        return NULL;
    }
    IMG_Init(IMG_INIT_PNG);
    TTF_Init();
    chdir("romfs:/"); // romfs itself must already be mounted - see main.c's romfsInit()

    if (R_FAILED(plInitialize(PlServiceType_User)))
    {
        logWrite("menu", "makeRenderer: plInitialize failed");
        SDL_Quit();
        return NULL;
    }

    MenuContext *context = malloc(sizeof(MenuContext));

    context->window = SDL_CreateWindow("SolarNX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        MENU_RESX, MENU_RESY, SDL_WINDOW_SHOWN);
    if (context->window == NULL)
    {
        logWrite("menu", "makeRenderer: SDL_CreateWindow failed: %s", SDL_GetError());
        free(context);
        plExit();
        SDL_Quit();
        return NULL;
    }

    context->renderer = SDL_CreateRenderer(context->window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (context->renderer == NULL)
    {
        logWrite("menu", "makeRenderer: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(context->window);
        free(context);
        plExit();
        SDL_Quit();
        return NULL;
    }
    SDL_SetRenderDrawBlendMode(context->renderer, SDL_BLENDMODE_BLEND);

    PlFontData fontData, fontExtData;
    plGetSharedFontByType(&fontData, PlSharedFontType_Standard);
    plGetSharedFontByType(&fontExtData, PlSharedFontType_NintendoExt);
    context->font = FC_CreateFont();
    FC_LoadFont_RW(context->font, context->renderer,
        SDL_RWFromMem((void *)fontData.address, fontData.size),
        SDL_RWFromMem((void *)fontExtData.address, fontExtData.size),
        1, 40, FC_MakeColor(0, 0, 0, 255), TTF_STYLE_NORMAL);

    context->background = loadTexture(context, "romfs:/branding/background.png");
    context->wordmark = loadTexture(context, "romfs:/branding/wordmark.png");

    s_lastTick = 0; // reset so the first rendererTick() after this doesn't see a stale tick
    return context;
}

void freeRenderer(MenuContext *context)
{
    if (context == NULL)
        return;
    if (context->background != NULL) SDL_DestroyTexture(context->background);
    if (context->wordmark != NULL) SDL_DestroyTexture(context->wordmark);
    if (context->font != NULL) FC_FreeFont(context->font);
    if (context->renderer != NULL) SDL_DestroyRenderer(context->renderer);
    if (context->window != NULL) SDL_DestroyWindow(context->window);
    free(context);
    plExit();
    IMG_Quit();
    TTF_Quit();
    SDL_Quit();
}

void SDL_ClearScreen(MenuContext *context, SDL_Color colour)
{
    // SDL2_gfx's *RGBA primitives (roundedRectangleRGBA/roundedBoxRGBA) set the renderer's blend
    // mode themselves based on the alpha of whatever they last drew, forcing it to NONE for a
    // fully-opaque (alpha 255) call - and that mode leaks into every draw call afterwards,
    // including the next frame's, until something sets it back. Since every frame starts here,
    // this is the one place that reliably restores it before anything else this frame relies on
    // alpha blending (translucent bars, trails, ...).
    SDL_SetRenderDrawBlendMode(context->renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(context->renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_RenderClear(context->renderer);
}

void SDL_DrawText(MenuContext *context, int x, int y, SDL_Color colour, const char *text)
{
    // "%s", not text itself: text can be a game name from the host's library - a '%' in it would
    // otherwise be parsed as a format specifier (undefined behaviour, likely a crash).
    FC_DrawColor(context->font, context->renderer, x, y, colour, "%s", text);
}

void draw_rect(MenuContext *context, int x, int y, int w, int h, SDL_Color colour)
{
    SDL_SetRenderDrawColor(context->renderer, colour.r, colour.g, colour.b, colour.a);
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(context->renderer, &r);
}

void fillCircle(MenuContext *context, float centreX, float centreY, float radius, SDL_Color colour)
{
    for (double dy = 1; dy <= radius; dy += 1.0)
    {
        double dx = floor(sqrt((2.0 * radius * dy) - (dy * dy)));
        SDL_SetRenderDrawColor(context->renderer, colour.r, colour.g, colour.b, colour.a);
        SDL_RenderDrawLine(context->renderer, centreX - dx, centreY + dy - radius, centreX + dx, centreY + dy - radius);
        SDL_RenderDrawLine(context->renderer, centreX - dx, (centreY + 1) - dy + radius, centreX + dx, (centreY + 1) - dy + radius);
    }
}

void drawRoundedRect(MenuContext *context, int x, int y, int w, int h, int radius, SDL_Color colour)
{
    roundedRectangleRGBA(context->renderer, x, y, x + w, y + h, radius, colour.r, colour.g, colour.b, colour.a);
}

void fillRoundedRect(MenuContext *context, int x, int y, int w, int h, int radius, SDL_Color colour)
{
    roundedBoxRGBA(context->renderer, x, y, x + w, y + h, radius, colour.r, colour.g, colour.b, colour.a);
}

void drawSolarNxTopBar(MenuContext *context)
{
    SDL_Color barColour = {150, 150, 150, 26}; // ~0.1 alpha
    draw_rect(context, 0, 0, MENU_RESX, 70, barColour);

    int y = 14;
    if (context->wordmark != NULL)
    {
        // The image carries transparent padding around the letters for their glow - offset so the
        // letters themselves sit where the text logo used to start (x 24), centred in the bar.
        int w, h;
        SDL_QueryTexture(context->wordmark, NULL, NULL, &w, &h);
        SDL_Rect dest = {24 - h / 4, 35 - h / 2, w, h};
        SDL_RenderCopy(context->renderer, context->wordmark, NULL, &dest);
    }
    else
    {
        SDL_Color gold = {255, 214, 140, 255};
        SDL_Color red = {255, 60, 40, 255};
        SDL_Color blue = {10, 185, 230, 255};
        int x = 24;
        SDL_DrawText(context, x, y, gold, "SOLAR");
        x += FC_GetWidth(context->font, "%s", "SOLAR");
        SDL_DrawText(context, x, y, red, "N");
        x += FC_GetWidth(context->font, "%s", "N");
        SDL_DrawText(context, x, y, blue, "X");
    }

    // Small, bottom-left: the top-right corner belongs to screen titles ("My games").
    SDL_Color grey = {150, 150, 160, 200};
    FC_DrawEffect(context->font, context->renderer, 24, MENU_RESY - 30,
        FC_MakeEffect(FC_ALIGN_LEFT, FC_MakeScale(0.45f, 0.45f), grey), "%s", "v" SOLARNX_VERSION);
}

void drawIpAddressLine(MenuContext *context)
{
    u32 ip = gethostid();
    char ipBuf[64];
    snprintf(ipBuf, sizeof(ipBuf), "IP Address: %u.%u.%u.%u",
             ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);

    SDL_Color white = {255, 255, 255, 255};
    int textWidth = FC_GetWidth(context->font, "%s", ipBuf);
    SDL_DrawText(context, MENU_RESX / 2 - textWidth / 2, MENU_RESY - 46, white, ipBuf);
}

void drawSplash(MenuContext *context, StarSystem *starSystem)
{
    float dt = rendererTick();

    SDL_Color bg = {5, 5, 8, 255};
    SDL_ClearScreen(context, bg);

    starsystem_update(starSystem, dt);
    starsystem_draw(context, starSystem);

    drawSolarNxTopBar(context);
    drawIpAddressLine(context);

    SDL_RenderPresent(context->renderer);
}
