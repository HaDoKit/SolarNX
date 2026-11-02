#include "starsystem.h"
#include "renderer.h"

#include <switch.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL2_gfxPrimitives.h>

#define SS_TWO_PI 6.28318530718f

// Scene geometry in fractions of the screen height - identical to SCENE in
// tools/branding/make_branding.py, which drew the orbits and the eclipse into the background image.
#define SS_SUN_RADIUS 0.13f
#define SS_TILT_DEGREES -12.0f
static const float kOrbits[][2] = {{0.46f, 0.105f}, {0.74f, 0.17f}}; // rx, ry

static const SDL_Color kRedColour = {255, 60, 40, 255};
static const SDL_Color kBlueColour = {10, 185, 230, 255};

static float ssRandRange(float minValue, float maxValue)
{
    float t = (float)rand() / (float)RAND_MAX;
    return minValue + t * (maxValue - minValue);
}

void starsystem_init(StarSystem *system, float centreX, float centreY)
{
    srand((unsigned int)svcGetSystemTick());
    system->centreX = centreX;
    system->centreY = centreY;

    system->planets[0] = (SSPlanet){ssRandRange(0.0f, SS_TWO_PI), 0.22f, 0, 7.0f, kRedColour};
    system->planets[1] = (SSPlanet){ssRandRange(0.0f, SS_TWO_PI), 0.13f, 1, 6.0f, kBlueColour};
}

void starsystem_update(StarSystem *system, float dt)
{
    for (int i = 0; i < SS_PLANET_COUNT; i++)
    {
        SSPlanet *p = &system->planets[i];
        p->angle = fmodf(p->angle + p->angularSpeed * dt, SS_TWO_PI);
    }
}

static void drawPlanet(MenuContext *context, float x, float y, const SSPlanet *p)
{
    // soft glow: a few translucent discs, widest first
    for (int k = 3; k >= 1; k--)
        filledCircleRGBA(context->renderer, (Sint16)x, (Sint16)y, (Sint16)(p->radius * (1.0f + 0.45f * k)),
            p->colour.r, p->colour.g, p->colour.b, 22);
    filledCircleRGBA(context->renderer, (Sint16)x, (Sint16)y, (Sint16)p->radius, p->colour.r, p->colour.g, p->colour.b, 255);
    aacircleRGBA(context->renderer, (Sint16)x, (Sint16)y, (Sint16)p->radius, p->colour.r, p->colour.g, p->colour.b, 255);
}

void starsystem_draw(MenuContext *context, const StarSystem *system)
{
    if (context->background != NULL)
        SDL_RenderCopy(context->renderer, context->background, NULL, NULL);

    const float height = (float)MENU_RESY;
    const float tilt = SS_TILT_DEGREES * (SS_TWO_PI / 360.0f);
    const float sunRadius = SS_SUN_RADIUS * height;

    for (int i = 0; i < SS_PLANET_COUNT; i++)
    {
        const SSPlanet *p = &system->planets[i];
        float ex = cosf(p->angle) * kOrbits[p->orbit][0] * height;
        float ey = sinf(p->angle) * kOrbits[p->orbit][1] * height;
        float x = system->centreX + ex * cosf(tilt) - ey * sinf(tilt);
        float y = system->centreY + ex * sinf(tilt) + ey * cosf(tilt);

        // The far half of the orbit (sin < 0) runs behind the eclipse disc.
        bool behind = sinf(p->angle) < 0.0f;
        if (behind && hypotf(x - system->centreX, y - system->centreY) < sunRadius + p->radius)
            continue;
        drawPlanet(context, x, y, p);
    }

    // SDL2_gfx leaves the blend mode at NONE after an opaque primitive - see SDL_ClearScreen.
    SDL_SetRenderDrawBlendMode(context->renderer, SDL_BLENDMODE_BLEND);
}
