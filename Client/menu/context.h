#pragma once

#include <switch.h>
#include <SDL.h>

#include "../common/fontcache/SDL_FontCache.h"

#define MENU_RESX 1280
#define MENU_RESY 720

// Rendering state for the pre-session app-select menu (menu.c) and its shared background
// (starsystem.c) - a self-contained SDL2 window/renderer/font, created and torn down before
// video.c creates its own for actual gameplay (see main.c). Deliberately NOT shared with video.c:
// this app only ever needs one of the two on screen at a time (menu, then gameplay), so keeping
// them fully independent avoids coupling this screen's lifecycle to video.c's NVTEGRA/SDL2 state.
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    FC_Font *font;
    // Branding art from romfs:/branding/ (tools/branding/make_branding.py); NULL if loading failed -
    // the screens then fall back to the plain clear colour / text logo.
    SDL_Texture *background;
    SDL_Texture *wordmark;
} MenuContext;
