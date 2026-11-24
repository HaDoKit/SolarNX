// Video channel reassembly: rebuilds whole H.264/H.265 frames from the PC's video
// datagrams (wire.h framing), recovering lost shards from the Reed-Solomon parity.
// Instance-based port of the Switch client's Client/video/reassembly.c - the logic
// (two-frame reorder window, keyframe gating, lost-frame history) is identical.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"
#include "rs.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_FRAME_BYTES (8u * 1024u * 1024u)
#define MAX_BLOCKS 255
#define PADDING_BYTES 64 // decoders may read that far past the end
// Frame numbers further than this from the current one (either way) mean the PC
// started a new stream (VideoSender starts each one at a random number).
#define STREAM_RESTART_DISTANCE 1000
#define LOST_HISTORY 16

typedef struct {
    int first;       // index of the block's first data shard in the frame
    int dataCount;
    int parityCount; // -1 until the first datagram of the block says
    int received;
    bool done;
    bool present[RS_MAX_SHARDS];
    uint8_t *parity;
    size_t parityCapacity;
} VideoBlock;

typedef struct {
    bool active;
    bool complete;
    bool delivered;
    bool keyframe;
    bool hevc;
    uint32_t number;
    uint32_t size;
    int blockCount;
    int blocksDone;
    uint8_t *data;
    size_t dataCapacity;
    uint64_t firstMs;
    VideoBlock blocks[MAX_BLOCKS];
} VideoFrame;

struct SnxVideo {
    VideoFrame slots[2];
    VideoFrame *a; // the next frame to hand out
    VideoFrame *b; // the one after it (Internet reordering window)
    bool haveExpected;
    uint32_t expected;
    int window; // 2: expected + next; 1: one frame at a time (LAN)
    bool needKeyframe;
    SnxVideoStats stats;
    uint32_t lostNumbers[LOST_HISTORY];
    bool lostValid[LOST_HISTORY];
    int lostNext;
};

static uint64_t videoNowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static inline uint32_t readU32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4); // little-endian on both ends - see wire.h
    return v;
}

SnxVideo *snx_video_create(void)
{
    rsInit(); // GF(2^8) tables - idempotent, cheap after the first call
    SnxVideo *v = (SnxVideo *)calloc(1, sizeof(*v));
    if (v == NULL)
        return NULL;
    v->a = &v->slots[0];
    v->b = &v->slots[1];
    v->window = 2;
    v->needKeyframe = true;
    for (int i = 0; i < 2; i++)
    {
        v->slots[i].dataCapacity = 512 * 1024;
        v->slots[i].data = (uint8_t *)malloc(v->slots[i].dataCapacity);
        if (v->slots[i].data == NULL)
        {
            snx_video_destroy(v);
            return NULL;
        }
    }
    return v;
}

void snx_video_destroy(SnxVideo *v)
{
    if (v == NULL)
        return;
    for (int i = 0; i < 2; i++)
    {
        free(v->slots[i].data);
        for (int b = 0; b < MAX_BLOCKS; b++)
            free(v->slots[i].blocks[b].parity);
    }
    free(v);
}

void snx_video_reset(SnxVideo *v, bool reorderWindow)
{
    v->window = reorderWindow ? 2 : 1;
    memset(v->lostValid, 0, sizeof(v->lostValid));
    v->a->active = false;
    v->b->active = false;
    v->haveExpected = false;
    v->needKeyframe = true;
}

void snx_video_request_keyframe(SnxVideo *v) { v->needKeyframe = true; }

const SnxVideoStats *snx_video_stats(const SnxVideo *v) { return &v->stats; }

static bool startFrame(VideoFrame *f, uint32_t number, uint32_t size, int blockCount,
                       bool keyframe, bool hevc)
{
    f->active = false;
    if (size == 0 || size > MAX_FRAME_BYTES || blockCount == 0)
        return false;

    int dataShards = (int)((size + SNX_VIDEO_SHARD_SIZE - 1) / SNX_VIDEO_SHARD_SIZE);
    int perBlock = (dataShards + blockCount - 1) / blockCount;
    if (dataShards < blockCount || perBlock >= RS_MAX_SHARDS)
        return false;

    size_t needed = (size_t)dataShards * SNX_VIDEO_SHARD_SIZE + PADDING_BYTES;
    if (needed > f->dataCapacity)
    {
        uint8_t *grown = (uint8_t *)realloc(f->data, needed);
        if (grown == NULL)
            return false;
        f->data = grown;
        f->dataCapacity = needed;
    }

    for (int b = 0, first = 0; b < blockCount; b++)
    {
        VideoBlock *block = &f->blocks[b];
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
    f->firstMs = videoNowMs();
    return true;
}

// The expected frame isn't coming (complete) any more - lost unless it was handed out already.
static void giveUpExpected(SnxVideo *v)
{
    if (!(v->a->active && v->a->delivered))
    {
        v->stats.framesLost++;
        v->needKeyframe = true;
        v->lostNumbers[v->lostNext] = v->expected;
        v->lostValid[v->lostNext] = true;
        v->lostNext = (v->lostNext + 1) % LOST_HISTORY;
    }
    v->a->active = false;
    VideoFrame *t = v->a;
    v->a = v->b;
    v->b = t;
    v->b->active = false;
    v->expected++;
}

static bool wasLost(const SnxVideo *v, uint32_t number)
{
    for (int i = 0; i < LOST_HISTORY; i++)
        if (v->lostValid[i] && v->lostNumbers[i] == number)
            return true;
    return false;
}

static void finishBlock(VideoFrame *f, VideoBlock *block, SnxVideo *v)
{
    int missing = 0;
    for (int i = 0; i < block->dataCount; i++)
        missing += block->present[i] ? 0 : 1;

    if (missing > 0)
    {
        uint8_t *shards[RS_MAX_SHARDS];
        for (int i = 0; i < block->dataCount; i++)
            shards[i] = f->data + (size_t)(block->first + i) * SNX_VIDEO_SHARD_SIZE;
        for (int p = 0; p < block->parityCount; p++)
            shards[block->dataCount + p] = block->parity + (size_t)p * SNX_VIDEO_SHARD_SIZE;
        if (!rsReconstruct(shards, block->present, block->dataCount, block->parityCount,
                           SNX_VIDEO_SHARD_SIZE))
            return; // cannot happen with dataCount shards present; the frame counts as lost then
        v->stats.shardsRecovered += (uint32_t)missing;
    }

    block->done = true;
    f->blocksDone++;
}

// Which slot a datagram of frame `number` goes to - starting it if needed and moving
// the window along. NULL: nothing to do with it (late or redundant).
static VideoFrame *slotFor(SnxVideo *v, uint32_t number, uint32_t size, int blockCount,
                           bool keyframe, bool hevc)
{
    if (!v->haveExpected)
    {
        v->haveExpected = true;
        v->expected = number;
        v->a->active = v->b->active = false;
    }

    int32_t distance = (int32_t)(number - v->expected);
    if (distance >= STREAM_RESTART_DISTANCE || distance <= -STREAM_RESTART_DISTANCE)
    {
        v->a->active = v->b->active = false;
        v->expected = number;
        v->needKeyframe = true; // a new stream starts mid-GOP as far as this side knows
        distance = 0;
    }
    if (distance < 0)
    {
        if (wasLost(v, number))
            v->stats.datagramsLate++; // a frame given up on - reordering outran the window
        return NULL; // otherwise: redundant parity of a frame already handed out
    }
    while (distance >= v->window)
    {
        giveUpExpected(v); // a frame past the window is here: the expected one won't complete
        distance--;
    }

    VideoFrame *f = distance == 0 ? v->a : v->b;
    if (f->active)
        return f->number == number ? f : NULL;
    return startFrame(f, number, size, blockCount, keyframe, hevc) ? f : NULL;
}

// Hands a complete frame out (unless a keyframe is awaited). True when *out was filled.
static bool deliver(SnxVideo *v, VideoFrame *f, SnxVideoFrame *out)
{
    f->delivered = true;
    if (v->needKeyframe && !f->keyframe)
    {
        v->stats.framesSkipped++;
        return false;
    }
    v->needKeyframe = false;

    memset(f->data + f->size, 0, PADDING_BYTES);
    out->data = f->data;
    out->size = f->size;
    out->number = f->number;
    out->keyframe = f->keyframe;
    out->hevc = f->hevc;
    out->firstMs = f->firstMs;
    v->stats.framesHanded++;
    return true;
}

bool snx_video_push(SnxVideo *v, const uint8_t *datagram, size_t length, SnxVideoFrame *out)
{
    if (length != SNX_VIDEO_DATAGRAM_SIZE || datagram[0] != SNX_VIDEO_MAGIC ||
        datagram[1] != SNX_VIDEO_VERSION)
    {
        v->stats.datagramsRejected++;
        return false;
    }

    bool keyframe = (datagram[2] & SNX_VIDEO_FLAG_KEYFRAME) != 0;
    bool hevc = (datagram[2] & SNX_VIDEO_FLAG_HEVC) != 0;
    int blockIndex = datagram[3];
    int blockCount = datagram[4];
    int shardIndex = datagram[5];
    int parityCount = datagram[6];
    uint32_t number = readU32(datagram + 8);
    uint32_t size = readU32(datagram + 12);
    const uint8_t *payload = datagram + SNX_VIDEO_HEADER_SIZE;

    VideoFrame *f = slotFor(v, number, size, blockCount, keyframe, hevc);
    if (f == NULL)
        return false;

    if (f->complete)
        return false; // a redundant parity datagram of a frame already rebuilt
    if (size != f->size || blockCount != f->blockCount || blockIndex >= blockCount)
    {
        v->stats.datagramsRejected++;
        return false;
    }

    VideoBlock *block = &f->blocks[blockIndex];
    if (block->parityCount < 0)
    {
        if (parityCount > RS_MAX_PARITY || block->dataCount + parityCount > RS_MAX_SHARDS)
        {
            v->stats.datagramsRejected++;
            return false;
        }
        size_t needed = (size_t)parityCount * SNX_VIDEO_SHARD_SIZE;
        if (needed > block->parityCapacity)
        {
            uint8_t *grown = (uint8_t *)realloc(block->parity, needed);
            if (grown == NULL)
                return false;
            block->parity = grown;
            block->parityCapacity = needed;
        }
        block->parityCount = parityCount;
    }
    if (parityCount != block->parityCount || shardIndex >= block->dataCount + block->parityCount)
    {
        v->stats.datagramsRejected++;
        return false;
    }
    if (block->done || block->present[shardIndex])
        return false;

    uint8_t *target = shardIndex < block->dataCount
        ? f->data + (size_t)(block->first + shardIndex) * SNX_VIDEO_SHARD_SIZE
        : block->parity + (size_t)(shardIndex - block->dataCount) * SNX_VIDEO_SHARD_SIZE;
    memcpy(target, payload, SNX_VIDEO_SHARD_SIZE);
    block->present[shardIndex] = true;
    if (++block->received >= block->dataCount)
        finishBlock(f, block, v);

    if (f->blocksDone == f->blockCount)
        f->complete = true; // the newer one may finish first - it then waits for the older one
    return snx_video_next(v, out);
}

bool snx_video_next(SnxVideo *v, SnxVideoFrame *out)
{
    // In order: the expected frame first; once it is out, the one after it becomes expected.
    // The handed-out frame's slot is only reused by a later push, so its data stays valid then.
    while (v->haveExpected && v->a->active && v->a->complete)
    {
        bool handed = deliver(v, v->a, out);
        VideoFrame *t = v->a;
        v->a = v->b;
        v->b = t;
        v->b->active = false;
        v->expected++;
        if (handed)
            return true;
    }
    return false;
}
