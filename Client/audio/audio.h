#pragma once

#include <stdbool.h>
#include <stdint.h>

// Brings up audout (48kHz/stereo - matches the host exactly, see Config.cs's AudioSampleRate
// comment, so no resampling is needed anywhere in this path) and the Opus decoder. Call once at
// startup, before audioStart().
bool audioInit(void);

// Blocks: opens the Opus/RTP-like UDP stream from the PC on SOLARNX_AUDIO_PORT (network/network.h),
// decodes frames and plays them via audout, concealing lost frames via Opus's own PLC. Reconnects
// automatically if the stream drops. Returns once appIsRunning() goes false (common/appstate), so
// it's safe to run this on its own thread - see main.c.
// Returns false only if audioInit() wasn't called/didn't succeed first.
bool audioStart(void);

// Tears down the Opus decoder, audout buffers, and the audout service. Call once at shutdown.
void audioExit(void);

// Link-quality telemetry for input.c to stamp onto the outgoing SolarNxHidPacket (see network.h) -
// consumed by the host's AdaptiveBitrateController. Free-running counters, no locking (plain word
// reads/writes - see network.h's telemetry field doc comment).
uint32_t audioGetReconnectCount(void);
uint32_t audioGetConcealedFrameCount(void);
