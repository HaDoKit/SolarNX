// Opus decoder + SDL audio output for the SolarNX audio channel. The core hands
// sequenced Opus payloads through snx_audio_player_push (from its own thread);
// an SDL audio callback drains a small ring buffer in real time. Packet gaps
// are concealed with Opus PLC so the playback timeline stays continuous.
// This file is part of SolarNX (GPL-3.0).

#pragma once

#include <stddef.h>
#include <stdint.h>

// Opens the SDL audio device (48 kHz stereo s16) and creates the Opus decoder.
// Safe to call before any packets exist - the callback outputs silence.
void snx_audio_player_start(void);
void snx_audio_player_stop(void);

// Drops buffered audio and returns to the prebuffering state (session restart).
void snx_audio_player_reset(void);

// From the core thread: one Opus frame (~20 ms). missingBefore packets were
// lost or arrived too late - conceal them before decoding this one.
void snx_audio_player_push(const uint8_t *opusData, size_t len, uint16_t missingBefore);
