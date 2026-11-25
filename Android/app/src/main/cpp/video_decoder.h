// MediaCodec (NDK) decoder for the SolarNX video channel. Annex-B frames from
// the core are queued on the caller thread; decoded YUV is copied into the
// newest-frame slot and drawn from the render thread as an SDL texture via the
// shell's underlay hook. This file is part of SolarNX (GPL-3.0).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SDL_Renderer;

// Lazily creates the codec on the first fed frame (the HEVC flag picks the
// decoder). Safe to call before/without any frames.
void snx_video_decoder_start(void);
void snx_video_decoder_stop(void);
void snx_video_decoder_reset(void); // codec survives, pending frames dropped

// Core thread: one reassembled Annex-B access unit (SPS/PPS ride on keyframes).
void snx_video_decoder_feed(const uint8_t *data, size_t len, bool keyframe, bool hevc);

// Render thread (underlay): upload the newest decoded frame to the texture and
// draw it aspect-fit. No-op until the first frame lands.
void snx_video_decoder_draw(void *opaque, SDL_Renderer *renderer);

// True once at least one frame has been decoded (for status/debug).
bool snx_video_decoder_active(void);
