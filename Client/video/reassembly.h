#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Rebuilds whole H.264/H.265 frames from the PC's video datagrams (videowire.h), recovering lost ones
// from the Reed-Solomon parity that comes with each frame (common/rs). Video decode thread only.
//
// One frame is assembled at a time: the PC paces a frame's datagrams over a few milliseconds and
// the next frame starts a whole frame interval later, so a datagram of a newer frame means the
// current one has got everything it is going to get - if it still can't be rebuilt, it is lost.
// After a lost frame every following frame is dropped until a keyframe completes: the decoder
// would only smear the damage across the picture until then (the "water on glass" artifact), so
// the last good picture simply stays on screen instead.

typedef struct {
    const uint8_t *data; // Annex B, followed by AV_INPUT_BUFFER_PADDING_SIZE zero bytes; valid until the next push
    size_t size;
    uint32_t number;
    bool keyframe;
    bool hevc; // H.265, else H.264
} ReassembledFrame;

typedef struct {
    uint32_t framesLost;        // could not be rebuilt, or never arrived at all
    uint32_t framesSkipped;     // complete, but dropped while waiting for a keyframe
    uint32_t shardsRecovered;   // data shards rebuilt from parity
    uint32_t datagramsRejected; // not our format (an older host sending MPEG-TS), or inconsistent
} ReassemblyStats;

bool reassemblyInit(void);
void reassemblyExit(void);

// Forgets the frame in progress and waits for a keyframe - a new stream.
void reassemblyReset(void);

// The decoder rejected a frame: drop everything up to the next keyframe.
void reassemblyRequestKeyframe(void);

// Takes one datagram. Returns true and fills *out when it completed a frame that should be decoded.
bool reassemblyPush(const uint8_t *datagram, size_t length, ReassembledFrame *out);

// Free-running counters (never reset) - read from other threads as plain words, telemetry only.
const ReassemblyStats *reassemblyStats(void);
