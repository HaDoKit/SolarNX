#pragma once

#include "context.h"

// Menu background: the pre-rendered eclipse scene (romfs:/branding/background.png, made by
// tools/branding/make_branding.py - corona, orbits, stars) with two planets moving along its
// orbits. The orbit geometry below must match SCENE in that script.
#define SS_PLANET_COUNT 2

typedef struct
{
    float angle;             // parameter on the orbit ellipse, radians
    float angularSpeed;      // radians per second
    int orbit;               // index into the scene's orbits
    float radius;            // pixels
    SDL_Color colour;
} SSPlanet;

typedef struct
{
    float centreX, centreY;
    SSPlanet planets[SS_PLANET_COUNT];
} StarSystem;

/* Places the planets at random points of their orbits around (centreX, centreY). Call once before
 * the first starsystem_update/draw. */
void starsystem_init(StarSystem *system, float centreX, float centreY);

/* Moves the planets along their orbits by dt seconds. */
void starsystem_update(StarSystem *system, float dt);

/* Draws the background image, then the planets; a planet on the far side of its orbit is hidden
 * while it passes behind the eclipse disc. */
void starsystem_draw(MenuContext *context, const StarSystem *system);
