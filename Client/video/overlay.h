#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

// Debug mode (the PC turns it on - control/control.h): stream statistics over the video, and the
// debug menu (input.c opens it on Minus + Plus + ZL + ZR) that changes the PC's video settings while
// playing. Main thread only, drawn by video.c right before each present.

// Counters video.c keeps while the overlay is active, handed over (and reset) once a second.
typedef struct {
    uint64_t decodeNsSum;   // decode + NVTEGRA readback per frame
    uint32_t decodeCount;
    uint64_t latencyNsSum;  // first datagram of a frame arrived -> that frame presented
    uint32_t latencyCount;
    uint32_t presents;
} VideoDebugCounters;

// Implemented in video.c.
void videoTakeDebugCounters(VideoDebugCounters *out);

// True while video.c should collect the counters above.
bool overlayActive(void);

// Draws the statistics and/or the menu (and handles the menu's buttons). No-op outside debug mode.
void overlayDraw(SDL_Renderer *renderer);

// Frees the overlay's font - with videoExit (the renderer it was made for goes away).
void overlayExit(void);
