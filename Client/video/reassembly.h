#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Rebuilds whole H.264/H.265 frames from the PC's video datagrams (videowire.h), recovering lost ones
// from the Reed-Solomon parity that comes with each frame (common/rs). Video decode thread only.
//
// Internet mode assembles two frames at a time: datagrams arrive out of order there, so the first
// ones of frame N+1 may overtake the last ones of frame N. N is lost only once N+1 is complete or a
// datagram of N+2 arrives - see reassembly.c. Wi-Fi keeps one frame at a time (the 0.2.4 behaviour,
// which the player found the steadier one there): N is given up as soon as N+1 shows up.
// After a lost frame every following frame is dropped until a keyframe completes: the decoder
// would only smear the damage across the picture until then (the "water on glass" artifact), so
// the last good picture simply stays on screen instead.

typedef struct {
    const uint8_t *data; // Annex B, followed by AV_INPUT_BUFFER_PADDING_SIZE zero bytes; valid until the next push
    size_t size;
    uint32_t number;
    bool keyframe;
    bool hevc; // H.265, else H.264
    uint64_t firstTick; // armGetSystemTick() when its first datagram arrived - the debug overlay's latency
} ReassembledFrame;

typedef struct {
    uint32_t framesLost;        // could not be rebuilt, or never arrived at all
    uint32_t framesSkipped;     // complete, but dropped while waiting for a keyframe
    uint32_t shardsRecovered;   // data shards rebuilt from parity
    uint32_t datagramsRejected; // not our format (an older host sending MPEG-TS), or inconsistent
    uint32_t datagramsLate;     // arrived for a frame already given up on - reordering beyond the two-frame window
} ReassemblyStats;

bool reassemblyInit(void);
void reassemblyExit(void);

// Forgets the frame in progress and waits for a keyframe - a new stream.
// reorderWindow: the two-frame window (Internet mode) - see above.
void reassemblyReset(bool reorderWindow);

// The decoder rejected a frame: drop everything up to the next keyframe.
void reassemblyRequestKeyframe(void);

// Takes one datagram. Returns true and fills *out when it completed a frame that should be decoded.
// One datagram can complete two (the older frame, with a newer one already waiting): call
// reassemblyNext after decoding until it returns false.
bool reassemblyPush(const uint8_t *datagram, size_t length, ReassembledFrame *out);

bool reassemblyNext(ReassembledFrame *out);

// Free-running counters (never reset) - read from other threads as plain words, telemetry only.
const ReassemblyStats *reassemblyStats(void);
