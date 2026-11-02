#pragma once

#include <stdbool.h>
#include <stdint.h>

// Brings up SDL2 (video) and creates the render window/renderer. Call once at startup, before
// videoStart().
bool videoInit(void);

// Blocks: receives the PC's video on SOLARNX_VIDEO_PORT (network/network.h) in SolarNX's own framing
// (videowire.h), rebuilds lost datagrams from its Reed-Solomon parity, decodes with NVTEGRA (or in
// software) and renders via SDL2 - see the comment at the top of video.c. Also pumps
// appMainLoop() (common/appstate) and the SDL event queue, so this is meant to own the main thread - see main.c.
// Returns false only if videoInit() wasn't called/didn't succeed first.
bool videoStart(void);

// Tears down the decoder (if a stream is open), the SDL2 window/renderer, and SDL2 itself. Call
// once at shutdown.
void videoExit(void);

// Link-quality telemetry for input.c to stamp onto the outgoing SolarNxHidPacket (see network.h) -
// consumed by the host's AdaptiveBitrateController. Free-running counters/last-known-values, no
// locking (plain word reads/writes - see network.h's telemetry field doc comment).
uint32_t videoGetReconnectCount(void);
uint32_t videoGetMeasuredBitrateKbps(void);
uint32_t videoGetLostFrameCount(void);       // frames the parity could not save (reassembly.h)
uint32_t videoGetRecoveredPacketCount(void); // datagrams rebuilt from parity
