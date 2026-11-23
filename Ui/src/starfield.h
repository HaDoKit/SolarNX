#pragma once

#include <SDL2/SDL.h>

// Port of Client/menu/starsystem.c: the pre-rendered eclipse background plus two planets moving
// along its orbits. Planets are drawn from generated glow sprites instead of SDL2_gfx primitives.

typedef struct {
    float angle;
    float angular_speed;
    int orbit;
    float radius;
    SDL_Color colour;
    SDL_Texture *sprite;
} SsfPlanet;

typedef struct {
    int win_w, win_h;      // window size in pixels
    int draw_w, draw_h;    // letterboxed ("contain" fit) background rect
    float centre_x, centre_y;
    SsfPlanet planets[2];
    SDL_Texture *background;
} Starfield;

void starfield_init(Starfield *sf, SDL_Renderer *renderer, SDL_Texture *background,
                    int win_w, int win_h);
void starfield_update(Starfield *sf, float dt);
void starfield_draw(Starfield *sf, SDL_Renderer *renderer);
void starfield_free(Starfield *sf);
