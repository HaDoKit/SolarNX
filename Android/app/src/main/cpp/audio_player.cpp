// This file is part of SolarNX (GPL-3.0).

#include "audio_player.h"

#include <SDL.h>
#include <opus.h>
#include <string.h>

#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SolarNX", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "SolarNX", __VA_ARGS__)

// 48 kHz stereo s16: one 20 ms Opus frame is 960 samples * 2 ch * 2 B = 3840 B.
#define SNX_FRAME_SAMPLES 960
#define SNX_FRAME_BYTES (SNX_FRAME_SAMPLES * 2 * 2)

// ~0.68 s of audio. Power of two - the ring indexes wrap with a mask.
// The producer never blocks: overflow drops the oldest audio.
#define SNX_RING_BYTES (128 * 1024)

// Play silence until this much is buffered - absorbs LAN jitter without
// underflowing the callback. ~40 ms of lead at 192 KB/s.
#define SNX_PRIME_BYTES (SNX_FRAME_BYTES * 2)

// After this much backlog, drop to the watermark instead of letting latency grow.
#define SNX_HIGH_WATER_BYTES (SNX_FRAME_BYTES * 12) // ~240 ms

static OpusDecoder *s_decoder;
static SDL_AudioDeviceID s_dev;
static SDL_mutex *s_lock;
static uint8_t s_ring[SNX_RING_BYTES];
static size_t s_read, s_write; // ring offsets; fill = write - read (mod)
static bool s_primed;
static uint32_t s_underruns;

static size_t ring_fill(void)
{
    return (s_write - s_read) & (SNX_RING_BYTES - 1);
}

static void ring_write(const uint8_t *data, size_t len)
{
    size_t fill = ring_fill();
    if (len > SNX_RING_BYTES - fill)
    {
        // Overflow: skip the read head forward so the newest audio wins.
        size_t drop = len - (SNX_RING_BYTES - fill);
        s_read = (s_read + drop) & (SNX_RING_BYTES - 1);
    }
    size_t first = SNX_RING_BYTES - s_write;
    if (first > len)
        first = len;
    memcpy(s_ring + s_write, data, first);
    memcpy(s_ring, data + first, len - first);
    s_write = (s_write + len) & (SNX_RING_BYTES - 1);
}

static size_t ring_read(uint8_t *out, size_t len)
{
    size_t fill = ring_fill();
    if (len > fill)
        len = fill;
    size_t first = SNX_RING_BYTES - s_read;
    if (first > len)
        first = len;
    memcpy(out, s_ring + s_read, first);
    memcpy(out + first, s_ring, len - first);
    s_read = (s_read + len) & (SNX_RING_BYTES - 1);
    return len;
}

static void audio_cb(void *, Uint8 *stream, int len)
{
    SDL_LockMutex(s_lock);
    size_t n = 0;
    if (s_primed)
        n = ring_read(stream, (size_t)len);
    else if (ring_fill() >= SNX_PRIME_BYTES)
    {
        s_primed = true;
        n = ring_read(stream, (size_t)len);
    }
    if (n < (size_t)len)
    {
        memset(stream + n, 0, (size_t)len - n);
        if (s_primed)
        {
            s_primed = false;
            s_underruns++;
        }
    }
    SDL_UnlockMutex(s_lock);
}

void snx_audio_player_start(void)
{
    if (s_lock == NULL)
        s_lock = SDL_CreateMutex();
    if (s_dev != 0)
        return;
    int err = 0;
    s_decoder = opus_decoder_create(48000, 2, &err);
    if (err != OPUS_OK || s_decoder == NULL)
    {
        LOGW("opus_decoder_create failed: %d", err);
        return;
    }
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 480; // 10 ms
    want.callback = audio_cb;
    s_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (s_dev == 0)
    {
        LOGW("SDL_OpenAudioDevice failed: %s", SDL_GetError());
        opus_decoder_destroy(s_decoder);
        s_decoder = NULL;
        return;
    }
    SDL_PauseAudioDevice(s_dev, 0);
    LOGI("audio player started");
}

void snx_audio_player_stop(void)
{
    if (s_dev != 0)
    {
        SDL_CloseAudioDevice(s_dev);
        s_dev = 0;
    }
    if (s_decoder != NULL)
    {
        opus_decoder_destroy(s_decoder);
        s_decoder = NULL;
    }
    if (s_underruns != 0)
        LOGI("audio player stopped (%u underruns)", s_underruns);
    s_underruns = 0;
}

void snx_audio_player_reset(void)
{
    SDL_LockMutex(s_lock);
    s_read = s_write = 0;
    s_primed = false;
    SDL_UnlockMutex(s_lock);
}

void snx_audio_player_push(const uint8_t *opusData, size_t len, uint16_t missingBefore)
{
    if (s_decoder == NULL)
        return;
    int16_t pcm[SNX_FRAME_SAMPLES * 2];
    SDL_LockMutex(s_lock);

    // Conceal the gap, then decode the real packet. Cap the PLC run: past ~8
    // concealed frames the backlog cost outweighs smoothness - jump forward.
    uint16_t conceal = missingBefore > 8 ? 8 : missingBefore;
    for (uint16_t i = 0; i < conceal; i++)
    {
        int plc = opus_decode(s_decoder, NULL, 0, pcm, SNX_FRAME_SAMPLES, 0);
        ring_write((const uint8_t *)pcm,
                   plc > 0 ? (size_t)plc * 2 * 2 : SNX_FRAME_BYTES);
    }
    if (missingBefore > conceal)
    {
        // Resync: skip the remainder of the gap as silence so playback does not
        // fall behind the stream by the full loss burst.
        int16_t zeros[SNX_FRAME_SAMPLES * 2] = {0};
        for (uint16_t i = conceal; i < missingBefore; i++)
            ring_write((const uint8_t *)zeros, SNX_FRAME_BYTES);
    }

    int samples = opus_decode(s_decoder, opusData, (opus_int32)len, pcm,
                              SNX_FRAME_SAMPLES, 0);
    if (samples > 0)
    {
        ring_write((const uint8_t *)pcm, (size_t)samples * 2 * 2);
        static uint32_t s_decoded;
        if (++s_decoded % 250 == 0) // ~5 s of audio
            LOGI("audio: %u frames decoded, %zu ms buffered, %u underruns",
                 s_decoded, ring_fill() * 1000 / 192000, s_underruns);
    }
    else
    {
        static uint32_t s_decodeErrs;
        if (++s_decodeErrs < 10 || s_decodeErrs % 100 == 0)
            LOGW("opus_decode failed: %d (total %u)", samples, s_decodeErrs);
    }

    if (ring_fill() > SNX_HIGH_WATER_BYTES)
    {
        size_t drop = ring_fill() - SNX_PRIME_BYTES;
        s_read = (s_read + drop) & (SNX_RING_BYTES - 1);
    }
    SDL_UnlockMutex(s_lock);
}
