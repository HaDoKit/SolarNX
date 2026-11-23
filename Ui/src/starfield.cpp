#include "starfield.h"

#include <math.h>
#include <stdlib.h>

// Scene geometry in fractions of the screen height - identical to Client/menu/starsystem.c
// (and to SCENE in tools/branding/make_branding.py, which drew the orbits and eclipse).
#define SSF_TWO_PI 6.28318530718f
#define SSF_TILT_DEGREES -12.0f
static const float kOrbits[][2] = {{0.46f, 0.105f}, {0.74f, 0.17f}}; // rx, ry
#define SSF_SUN_RADIUS 0.13f

static const SDL_Color kRedColour = {255, 60, 40, 255};
static const SDL_Color kBlueColour = {10, 185, 230, 255};

static float ssf_rand(float lo, float hi)
{
    return lo + (hi - lo) * ((float)rand() / (float)RAND_MAX);
}

// A soft radial sprite: opaque core of `core` px, alpha glow fading to 0 at the edge.
static SDL_Texture *make_planet_sprite(SDL_Renderer *renderer, SDL_Color colour, float core)
{
    const int size = (int)(core * 4.5f) | 1;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface == NULL)
        return NULL;
    SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
    uint32_t *pixels = (uint32_t *)surface->pixels;
    const float centre = size / 2.0f;
    const float edge = size / 2.0f;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            float d = hypotf((float)x - centre, (float)y - centre);
            int a;
            if (d <= core)
                a = 255;
            else
            {
                float t = (d - core) / (edge - core);
                a = (int)(66.0f * (1.0f - t) * (1.0f - t));
            }
            pixels[y * (surface->pitch / 4) + x] =
                ((uint32_t)a << 24) | ((uint32_t)colour.b << 16) | ((uint32_t)colour.g << 8) | colour.r;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
    if (tex != NULL)
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

void starfield_init(Starfield *sf, SDL_Renderer *renderer, SDL_Texture *background,
                    int win_w, int win_h)
{
    srand((unsigned int)SDL_GetTicks64());
    sf->win_w = win_w;
    sf->win_h = win_h;
    // "Cover" fit of the 1280x720 scene: fills the window, edges may crop.
    float scale = fmaxf((float)win_w / 1280.0f, (float)win_h / 720.0f);
    sf->draw_w = (int)(1280.0f * scale);
    sf->draw_h = (int)(720.0f * scale);
    sf->centre_x = win_w * 0.5f;
    sf->centre_y = win_h * 0.5f;
    sf->background = background;

    const int scene_h = sf->draw_h;
    sf->planets[0] = (SsfPlanet){ssf_rand(0, SSF_TWO_PI), 0.22f, 0, 7.0f * scene_h / 720.0f, kRedColour, NULL};
    sf->planets[1] = (SsfPlanet){ssf_rand(0, SSF_TWO_PI), 0.13f, 1, 6.0f * scene_h / 720.0f, kBlueColour, NULL};
    for (int i = 0; i < 2; i++)
        sf->planets[i].sprite = make_planet_sprite(renderer, sf->planets[i].colour, sf->planets[i].radius);
}

void starfield_update(Starfield *sf, float dt)
{
    for (int i = 0; i < 2; i++)
    {
        SsfPlanet *p = &sf->planets[i];
        p->angle = fmodf(p->angle + p->angular_speed * dt, SSF_TWO_PI);
    }
}

void starfield_draw(Starfield *sf, SDL_Renderer *renderer)
{
    const int ox = (sf->win_w - sf->draw_w) / 2;
    const int oy = (sf->win_h - sf->draw_h) / 2;

    SDL_SetRenderDrawColor(renderer, 8, 9, 13, 255);
    SDL_RenderFillRect(renderer, NULL);
    if (sf->background != NULL)
    {
        SDL_Rect dst = {ox, oy, sf->draw_w, sf->draw_h};
        SDL_RenderCopy(renderer, sf->background, NULL, &dst);
    }

    const float height = (float)sf->draw_h;
    const float tilt = SSF_TILT_DEGREES * (SSF_TWO_PI / 360.0f);
    const float sun_radius = SSF_SUN_RADIUS * height;

    for (int i = 0; i < 2; i++)
    {
        const SsfPlanet *p = &sf->planets[i];
        float ex = cosf(p->angle) * kOrbits[p->orbit][0] * height;
        float ey = sinf(p->angle) * kOrbits[p->orbit][1] * height;
        float x = sf->centre_x + ex * cosf(tilt) - ey * sinf(tilt);
        float y = sf->centre_y + ex * sinf(tilt) + ey * cosf(tilt);

        // The far half of the orbit (sin < 0) runs behind the eclipse disc.
        bool behind = sinf(p->angle) < 0.0f;
        if (behind && hypotf(x - sf->centre_x, y - sf->centre_y) < sun_radius + p->radius)
            continue;

        if (p->sprite != NULL)
        {
            int sw, sh;
            SDL_QueryTexture(p->sprite, NULL, NULL, &sw, &sh);
            SDL_Rect dst = {(int)(x - sw / 2), (int)(y - sh / 2), sw, sh};
            SDL_RenderCopy(renderer, p->sprite, NULL, &dst);
        }
    }
}

void starfield_free(Starfield *sf)
{
    for (int i = 0; i < 2; i++)
        if (sf->planets[i].sprite != NULL)
            SDL_DestroyTexture(sf->planets[i].sprite);
}
