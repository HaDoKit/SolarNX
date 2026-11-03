#include "reassembly.h"

#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "videowire.h"
#include "../common/rs/rs.h"
#include "../common/logger/logger.h"

#define MAX_FRAME_BYTES (8u * 1024u * 1024u)
#define MAX_BLOCKS 255
#define PADDING_BYTES 64 // AV_INPUT_BUFFER_PADDING_SIZE - libavcodec may read that far past the end
// Frame numbers further than this from the current one (either way) mean the PC started a new
// stream (ffmpeg restarted - VideoSender starts each one at a random number), not lost frames.
#define STREAM_RESTART_DISTANCE 1000

typedef struct {
    int first;       // index of the block's first data shard in the frame
    int dataCount;
    int parityCount; // -1 until the first datagram of the block says
    int received;
    bool done;
    bool present[RS_MAX_SHARDS];
    uint8_t *parity;
    size_t parityCapacity;
} Block;

typedef struct {
    bool active;
    bool complete;
    bool delivered; // handed out (or dropped waiting for a keyframe) - done with
    bool keyframe;
    bool hevc;
    uint32_t number;
    uint32_t size;
    int blockCount;
    int blocksDone;
    uint8_t *data;
    size_t dataCapacity;
    uint64_t firstTick;
    Block blocks[MAX_BLOCKS];
} Frame;

// Two frames in flight. Over the internet (and through some routers) datagrams arrive out of order:
// the first datagrams of frame N+1 overtake the last ones of frame N. Assembling one frame at a time,
// N was given up the moment N+1 showed up - "1 frame lost per second, 0 datagrams rebuilt" with no
// datagram actually missing. Now the window is the next frame to hand out (s_expected, slot s_a) and
// the one after it (slot s_b) - either may still be waiting for its first datagram. s_expected is
// given up only when a datagram of s_expected + 2 arrives; a complete s_b waits for s_a, and frames
// always go out in order.
static Frame s_slots[2];
static Frame *s_a = &s_slots[0];
static Frame *s_b = &s_slots[1];
static bool s_haveExpected = false;
static uint32_t s_expected;

static bool s_needKeyframe = true;
static ReassemblyStats s_stats;

// Frames given up on recently - a datagram of one of them is "late" (reordering the window couldn't
// absorb), told apart from the harmless redundant parity of a frame already rebuilt.
#define LOST_HISTORY 16
static uint32_t s_lostNumbers[LOST_HISTORY];
static bool s_lostValid[LOST_HISTORY];
static int s_lostNext = 0;

static inline uint32_t readU32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4); // little-endian on both ends - see network.h
    return v;
}

bool reassemblyInit(void)
{
    rsInit();
    for (int i = 0; i < 2; i++)
    {
        s_slots[i].dataCapacity = 512 * 1024;
        s_slots[i].data = malloc(s_slots[i].dataCapacity);
        if (s_slots[i].data == NULL)
            return false;
    }
    return true;
}

void reassemblyExit(void)
{
    for (int i = 0; i < 2; i++)
    {
        Frame *f = &s_slots[i];
        free(f->data);
        f->data = NULL;
        f->dataCapacity = 0;
        f->active = false;
        for (int b = 0; b < MAX_BLOCKS; b++)
        {
            free(f->blocks[b].parity);
            f->blocks[b].parity = NULL;
            f->blocks[b].parityCapacity = 0;
        }
    }
}

void reassemblyReset(void)
{
    s_a->active = false;
    s_b->active = false;
    s_haveExpected = false;
    s_needKeyframe = true;
}

void reassemblyRequestKeyframe(void)
{
    s_needKeyframe = true;
}

const ReassemblyStats *reassemblyStats(void)
{
    return &s_stats;
}

static bool startFrame(Frame *f, uint32_t number, uint32_t size, int blockCount, bool keyframe, bool hevc)
{
    f->active = false;
    if (size == 0 || size > MAX_FRAME_BYTES || blockCount == 0)
        return false;

    int dataShards = (int)((size + VIDEO_WIRE_SHARD_SIZE - 1) / VIDEO_WIRE_SHARD_SIZE);
    int perBlock = (dataShards + blockCount - 1) / blockCount;
    if (dataShards < blockCount || perBlock >= RS_MAX_SHARDS)
        return false;

    size_t needed = (size_t)dataShards * VIDEO_WIRE_SHARD_SIZE + PADDING_BYTES;
    if (needed > f->dataCapacity)
    {
        uint8_t *grown = realloc(f->data, needed);
        if (grown == NULL)
            return false;
        f->data = grown;
        f->dataCapacity = needed;
    }

    for (int b = 0, first = 0; b < blockCount; b++)
    {
        Block *block = &f->blocks[b];
        block->first = first;
        block->dataCount = dataShards / blockCount + (b < dataShards % blockCount ? 1 : 0);
        block->parityCount = -1;
        block->received = 0;
        block->done = false;
        memset(block->present, 0, sizeof(block->present));
        first += block->dataCount;
    }

    f->active = true;
    f->complete = false;
    f->delivered = false;
    f->keyframe = keyframe;
    f->hevc = hevc;
    f->number = number;
    f->size = size;
    f->blockCount = blockCount;
    f->blocksDone = 0;
    f->firstTick = armGetSystemTick();
    return true;
}

// The expected frame isn't coming (complete) any more - lost unless it was handed out already.
static void giveUpExpected(void)
{
    if (!(s_a->active && s_a->delivered))
    {
        s_stats.framesLost++;
        s_needKeyframe = true;
        s_lostNumbers[s_lostNext] = s_expected;
        s_lostValid[s_lostNext] = true;
        s_lostNext = (s_lostNext + 1) % LOST_HISTORY;
    }
    s_a->active = false;
    Frame *t = s_a;
    s_a = s_b;
    s_b = t;
    s_b->active = false;
    s_expected++;
}

static bool wasLost(uint32_t number)
{
    for (int i = 0; i < LOST_HISTORY; i++)
        if (s_lostValid[i] && s_lostNumbers[i] == number)
            return true;
    return false;
}

static void finishBlock(Frame *f, Block *block)
{
    int missing = 0;
    for (int i = 0; i < block->dataCount; i++)
        missing += block->present[i] ? 0 : 1;

    if (missing > 0)
    {
        uint8_t *shards[RS_MAX_SHARDS];
        for (int i = 0; i < block->dataCount; i++)
            shards[i] = f->data + (size_t)(block->first + i) * VIDEO_WIRE_SHARD_SIZE;
        for (int p = 0; p < block->parityCount; p++)
            shards[block->dataCount + p] = block->parity + (size_t)p * VIDEO_WIRE_SHARD_SIZE;
        if (!rsReconstruct(shards, block->present, block->dataCount, block->parityCount, VIDEO_WIRE_SHARD_SIZE))
            return; // cannot happen with dataCount shards present; the frame then counts as lost when abandoned
        s_stats.shardsRecovered += (uint32_t)missing;
    }

    block->done = true;
    f->blocksDone++;
}

// Which slot a datagram of frame `number` goes to - starting it if needed and moving the window
// along. NULL: nothing to do with it (late or redundant, or a frame that couldn't be started).
static Frame *slotFor(uint32_t number, uint32_t size, int blockCount, bool keyframe, bool hevc)
{
    if (!s_haveExpected)
    {
        s_haveExpected = true;
        s_expected = number;
        s_a->active = s_b->active = false;
    }

    int32_t distance = (int32_t)(number - s_expected);
    if (distance >= STREAM_RESTART_DISTANCE || distance <= -STREAM_RESTART_DISTANCE)
    {
        s_a->active = s_b->active = false;
        s_expected = number;
        s_needKeyframe = true; // a new stream starts mid-GOP as far as this side knows
        distance = 0;
    }
    if (distance < 0)
    {
        if (wasLost(number))
            s_stats.datagramsLate++; // a frame given up on - the reordering outran the window
        return NULL; // otherwise: redundant parity of a frame already handed out
    }
    while (distance >= 2)
    {
        giveUpExpected(); // a frame two ahead is here: the expected one won't complete any more
        distance--;
    }

    Frame *f = distance == 0 ? s_a : s_b;
    if (f->active)
        return f->number == number ? f : NULL;
    return startFrame(f, number, size, blockCount, keyframe, hevc) ? f : NULL;
}

// Hands a complete frame out (unless a keyframe is awaited). True when *out was filled.
static bool deliver(Frame *f, ReassembledFrame *out)
{
    f->delivered = true;
    if (s_needKeyframe && !f->keyframe)
    {
        s_stats.framesSkipped++;
        return false;
    }
    s_needKeyframe = false;

    memset(f->data + f->size, 0, PADDING_BYTES);
    out->data = f->data;
    out->size = f->size;
    out->number = f->number;
    out->keyframe = f->keyframe;
    out->hevc = f->hevc;
    out->firstTick = f->firstTick;
    return true;
}

bool reassemblyPush(const uint8_t *datagram, size_t length, ReassembledFrame *out)
{
    if (length != VIDEO_WIRE_DATAGRAM_SIZE || datagram[0] != VIDEO_WIRE_MAGIC || datagram[1] != VIDEO_WIRE_VERSION)
    {
        if (s_stats.datagramsRejected++ == 0)
            logWrite("video", "rejected a %u-byte datagram (first byte 0x%02X) - not SolarNX video framing; is the PC app the same version?",
                (unsigned)length, length > 0 ? datagram[0] : 0);
        return false;
    }

    bool keyframe = (datagram[2] & VIDEO_WIRE_FLAG_KEYFRAME) != 0;
    bool hevc = (datagram[2] & VIDEO_WIRE_FLAG_HEVC) != 0;
    int blockIndex = datagram[3];
    int blockCount = datagram[4];
    int shardIndex = datagram[5];
    int parityCount = datagram[6];
    uint32_t number = readU32(datagram + 8);
    uint32_t size = readU32(datagram + 12);
    const uint8_t *payload = datagram + VIDEO_WIRE_HEADER_SIZE;

    Frame *f = slotFor(number, size, blockCount, keyframe, hevc);
    if (f == NULL)
        return false;

    if (f->complete)
        return false; // a redundant parity datagram of a frame already rebuilt
    if (size != f->size || blockCount != f->blockCount || blockIndex >= blockCount)
    {
        s_stats.datagramsRejected++;
        return false;
    }

    Block *block = &f->blocks[blockIndex];
    if (block->parityCount < 0)
    {
        if (parityCount > RS_MAX_PARITY || block->dataCount + parityCount > RS_MAX_SHARDS)
        {
            s_stats.datagramsRejected++;
            return false;
        }
        size_t needed = (size_t)parityCount * VIDEO_WIRE_SHARD_SIZE;
        if (needed > block->parityCapacity)
        {
            uint8_t *grown = realloc(block->parity, needed);
            if (grown == NULL)
                return false;
            block->parity = grown;
            block->parityCapacity = needed;
        }
        block->parityCount = parityCount;
    }
    if (parityCount != block->parityCount || shardIndex >= block->dataCount + block->parityCount)
    {
        s_stats.datagramsRejected++;
        return false;
    }
    if (block->done || block->present[shardIndex])
        return false;

    uint8_t *target = shardIndex < block->dataCount
        ? f->data + (size_t)(block->first + shardIndex) * VIDEO_WIRE_SHARD_SIZE
        : block->parity + (size_t)(shardIndex - block->dataCount) * VIDEO_WIRE_SHARD_SIZE;
    memcpy(target, payload, VIDEO_WIRE_SHARD_SIZE);
    block->present[shardIndex] = true;
    if (++block->received >= block->dataCount)
        finishBlock(f, block);

    if (f->blocksDone == f->blockCount)
        f->complete = true; // the newer one may finish first - it then waits for the older one
    return reassemblyNext(out);
}

bool reassemblyNext(ReassembledFrame *out)
{
    // In order: the expected frame first; once it is out, the one after it becomes the expected one.
    // The handed-out frame's slot is only reused by a later push, so its data stays valid until then.
    while (s_haveExpected && s_a->active && s_a->complete)
    {
        bool handed = deliver(s_a, out);
        Frame *t = s_a;
        s_a = s_b;
        s_b = t;
        s_b->active = false;
        s_expected++;
        if (handed)
            return true;
    }
    return false;
}
