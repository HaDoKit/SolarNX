#pragma once

#include "context.h"
#include "starsystem.h"

// Allocates and initializes a MenuContext: SDL2 (video/timer), SDL2_image, SDL2_ttf, mounts
// romfs (must already be initialized via romfsInit() - see main.c), and loads the Switch's own
// shared system font via the pl service (no bundled font file needed). Returns NULL on failure.
MenuContext *makeRenderer(void);

// Deallocates a MenuContext and tears down SDL2/SDL2_image/SDL2_ttf/pl. Call once, after the menu
// (and any other screen using this context) is done - before video.c creates its own SDL2 state.
void freeRenderer(MenuContext *context);

// Advances the frame-timing state used by starsystem_update's dt and returns the elapsed seconds
// since the previous call (first call after initDelta() returns ~0). Call once per frame.
float rendererTick(void);

// Draws the post-selection waiting screen (same cosmos background/top bar/IP line as the menu,
// minus the Steam/shadPS4 cards). starSystem must be the same already-initialised instance passed
// to runAppSelectMenu, so the system carries on animating instead of resetting.
void drawSplash(MenuContext *context, StarSystem *starSystem);

// Drawing primitives shared by menu.c's app-selection cards and starsystem.c.
void SDL_ClearScreen(MenuContext *context, SDL_Color colour);
void SDL_DrawText(MenuContext *context, int x, int y, SDL_Color colour, const char *text);
void draw_rect(MenuContext *context, int x, int y, int w, int h, SDL_Color colour);
void fillCircle(MenuContext *context, float centreX, float centreY, float radius, SDL_Color colour);

// Rounded-rectangle border/fill via SDL2_gfx (already linked in the Makefile) - used by menu.c's
// app-selection cards.
void drawRoundedRect(MenuContext *context, int x, int y, int w, int h, int radius, SDL_Color colour);
void fillRoundedRect(MenuContext *context, int x, int y, int w, int h, int radius, SDL_Color colour);

// Shared between the app-selection menu and the post-selection waiting screen, so both look like
// the same app instead of the top bar/IP line being drawn differently per screen.
void drawSolarNxTopBar(MenuContext *context);
void drawIpAddressLine(MenuContext *context);
