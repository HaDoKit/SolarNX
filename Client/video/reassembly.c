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

static struct {
    bool active;
    bool complete;
    bool keyframe;
    bool hevc;
    uint32_t number;
    uint32_t size;
    int blockCount;
    int blocksDone;
    uint8_t *data;
    size_t dataCapacity;
    uint64_t firstTick;
} s_frame;

static Block s_blocks[MAX_BLOCKS];
static bool s_needKeyframe = true;
static ReassemblyStats s_stats;

static inline uint32_t readU32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4); // little-endian on both ends - see network.h
    return v;
}

bool reassemblyInit(void)
{
    rsInit();
    s_frame.dataCapacity = 512 * 1024;
    s_frame.data = malloc(s_frame.dataCapacity);
    return s_frame.data != NULL;
}

void reassemblyExit(void)
{
    free(s_frame.data);
    s_frame.data = NULL;
    s_frame.dataCapacity = 0;
    for (int b = 0; b < MAX_BLOCKS; b++)
    {
        free(s_blocks[b].parity);
        s_blocks[b].parity = NULL;
        s_blocks[b].parityCapacity = 0;
    }
}

void reassemblyReset(void)
{
    s_frame.active = false;
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

static bool startFrame(uint32_t number, uint32_t size, int blockCount, bool keyframe, bool hevc)
{
    s_frame.active = false;
    if (size == 0 || size > MAX_FRAME_BYTES || blockCount == 0)
        return false;

    int dataShards = (int)((size + VIDEO_WIRE_SHARD_SIZE - 1) / VIDEO_WIRE_SHARD_SIZE);
    int perBlock = (dataShards + blockCount - 1) / blockCount;
    if (dataShards < blockCount || perBlock >= RS_MAX_SHARDS)
        return false;

    size_t needed = (size_t)dataShards * VIDEO_WIRE_SHARD_SIZE + PADDING_BYTES;
    if (needed > s_frame.dataCapacity)
    {
        uint8_t *grown = realloc(s_frame.data, needed);
        if (grown == NULL)
            return false;
        s_frame.data = grown;
        s_frame.dataCapacity = needed;
    }

    for (int b = 0, first = 0; b < blockCount; b++)
    {
        Block *block = &s_blocks[b];
        block->first = first;
        block->dataCount = dataShards / blockCount + (b < dataShards % blockCount ? 1 : 0);
        block->parityCount = -1;
        block->received = 0;
        block->done = false;
        memset(block->present, 0, sizeof(block->present));
        first += block->dataCount;
    }

    s_frame.active = true;
    s_frame.complete = false;
    s_frame.keyframe = keyframe;
    s_frame.hevc = hevc;
    s_frame.number = number;
    s_frame.size = size;
    s_frame.blockCount = blockCount;
    s_frame.blocksDone = 0;
    s_frame.firstTick = armGetSystemTick();
    return true;
}

// The current frame is being left behind - lost if it never completed.
static void abandonFrame(void)
{
    if (s_frame.active && !s_frame.complete)
    {
        s_stats.framesLost++;
        s_needKeyframe = true;
    }
    s_frame.active = false;
}

static void finishBlock(Block *block)
{
    int missing = 0;
    for (int i = 0; i < block->dataCount; i++)
        missing += block->present[i] ? 0 : 1;

    if (missing > 0)
    {
        uint8_t *shards[RS_MAX_SHARDS];
        for (int i = 0; i < block->dataCount; i++)
            shards[i] = s_frame.data + (size_t)(block->first + i) * VIDEO_WIRE_SHARD_SIZE;
        for (int p = 0; p < block->parityCount; p++)
            shards[block->dataCount + p] = block->parity + (size_t)p * VIDEO_WIRE_SHARD_SIZE;
        if (!rsReconstruct(shards, block->present, block->dataCount, block->parityCount, VIDEO_WIRE_SHARD_SIZE))
            return; // cannot happen with dataCount shards present; the frame then counts as lost when abandoned
        s_stats.shardsRecovered += (uint32_t)missing;
    }

    block->done = true;
    s_frame.blocksDone++;
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

    if (!s_frame.active || number != s_frame.number)
    {
        int32_t distance = s_frame.active ? (int32_t)(number - s_frame.number) : STREAM_RESTART_DISTANCE;
        if (distance < 0 && distance > -STREAM_RESTART_DISTANCE)
            return false; // a late datagram of a frame already left behind

        abandonFrame();
        if (distance >= STREAM_RESTART_DISTANCE || distance <= -STREAM_RESTART_DISTANCE)
        {
            s_needKeyframe = true; // a new stream starts mid-GOP as far as this side knows
        }
        else if (distance > 1)
        {
            s_stats.framesLost += (uint32_t)(distance - 1); // not a single datagram of these arrived
            s_needKeyframe = true;
        }

        if (!startFrame(number, size, blockCount, keyframe, hevc))
        {
            s_stats.datagramsRejected++;
            return false;
        }
    }

    if (s_frame.complete)
        return false; // a redundant parity datagram of a frame already rebuilt
    if (size != s_frame.size || blockCount != s_frame.blockCount || blockIndex >= blockCount)
    {
        s_stats.datagramsRejected++;
        return false;
    }

    Block *block = &s_blocks[blockIndex];
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
        ? s_frame.data + (size_t)(block->first + shardIndex) * VIDEO_WIRE_SHARD_SIZE
        : block->parity + (size_t)(shardIndex - block->dataCount) * VIDEO_WIRE_SHARD_SIZE;
    memcpy(target, payload, VIDEO_WIRE_SHARD_SIZE);
    block->present[shardIndex] = true;
    if (++block->received >= block->dataCount)
        finishBlock(block);

    if (s_frame.blocksDone < s_frame.blockCount)
        return false;

    s_frame.complete = true;
    if (s_needKeyframe && !s_frame.keyframe)
    {
        s_stats.framesSkipped++;
        return false;
    }
    s_needKeyframe = false;

    memset(s_frame.data + s_frame.size, 0, PADDING_BYTES);
    out->data = s_frame.data;
    out->size = s_frame.size;
    out->number = s_frame.number;
    out->keyframe = s_frame.keyframe;
    out->hevc = s_frame.hevc;
    out->firstTick = s_frame.firstTick;
    return true;
}
