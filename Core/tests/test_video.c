// Video reassembly tests against crafted datagrams: clean delivery, parity
// recovery, unrecoverable loss, keyframe gating, and malformed input.
// This file is part of SolarNX (GPL-3.0).

#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "rs.h"

#include "test_harness.h"

static void sendFrame2(SnxVideo *v, uint32_t number, uint32_t size, int blockCount,
                       int parityCount, bool keyframe, bool hevc, int drop, int drop2,
                       SnxVideoFrame *out, bool *delivered);

// Builds and sends (via push) the datagrams of one frame. `size` payload bytes of
// recognizable filler, `blockCount` RS blocks, `parityCount` parity shards per
// block. `drop` selects one data shard to NOT send (-1 = send everything).
static void sendFrame(SnxVideo *v, uint32_t number, uint32_t size, int blockCount,
                      int parityCount, bool keyframe, bool hevc, int drop, SnxVideoFrame *out,
                      bool *delivered)
{
    sendFrame2(v, number, size, blockCount, parityCount, keyframe, hevc, drop, -1, out, delivered);
}

// drop2 = a second lost shard; two losses with one parity shard are unrecoverable.
static void sendFrame2(SnxVideo *v, uint32_t number, uint32_t size, int blockCount,
                       int parityCount, bool keyframe, bool hevc, int drop, int drop2,
                       SnxVideoFrame *out, bool *delivered)
{
    *delivered = false;
    int dataShards = (int)((size + SNX_VIDEO_SHARD_SIZE - 1) / SNX_VIDEO_SHARD_SIZE);
    uint8_t *frame = (uint8_t *)calloc(1, (size_t)dataShards * SNX_VIDEO_SHARD_SIZE);
    for (uint32_t i = 0; i < size; i++)
        frame[i] = (uint8_t)(i * 7 + number); // recognizable filler

    int first = 0;
    for (int b = 0; b < blockCount; b++)
    {
        int count = dataShards / blockCount + (b < dataShards % blockCount ? 1 : 0);

        // Parity shards for this block (Cauchy, same as the PC's ReedSolomon.cs).
        uint8_t *shards[RS_MAX_SHARDS];
        for (int i = 0; i < count; i++)
            shards[i] = frame + (size_t)(first + i) * SNX_VIDEO_SHARD_SIZE;
        uint8_t *parity = (uint8_t *)calloc((size_t)parityCount, SNX_VIDEO_SHARD_SIZE);
        uint8_t *parityPtrs[RS_MAX_PARITY];
        for (int p = 0; p < parityCount; p++)
            parityPtrs[p] = parity + (size_t)p * SNX_VIDEO_SHARD_SIZE;
        rsEncode((const uint8_t *const *)shards, count, parityPtrs, parityCount,
                 SNX_VIDEO_SHARD_SIZE);

        for (int i = 0; i < count + parityCount; i++)
        {
            int globalShard = first + i;
            if (i < count && (i == drop || i == drop2))
                continue; // simulate losing these data shards

            uint8_t dgram[SNX_VIDEO_DATAGRAM_SIZE];
            dgram[0] = SNX_VIDEO_MAGIC;
            dgram[1] = SNX_VIDEO_VERSION;
            dgram[2] = (uint8_t)((keyframe ? SNX_VIDEO_FLAG_KEYFRAME : 0) |
                                 (hevc ? SNX_VIDEO_FLAG_HEVC : 0));
            dgram[3] = (uint8_t)b;
            dgram[4] = (uint8_t)blockCount;
            dgram[5] = (uint8_t)i;
            dgram[6] = (uint8_t)parityCount;
            dgram[7] = 0;
            memcpy(dgram + 8, &number, 4);
            memcpy(dgram + 12, &size, 4);
            memcpy(dgram + SNX_VIDEO_HEADER_SIZE,
                   i < count ? shards[i] : parityPtrs[i - count], SNX_VIDEO_SHARD_SIZE);

            if (snx_video_push(v, dgram, sizeof(dgram), out))
                *delivered = true;
            while (snx_video_next(v, out))
                *delivered = true;
        }

        free(parity);
        first += count;
    }
    free(frame);
}

int test_video(void)
{
    SnxVideoFrame f;
    bool delivered;

    // --- clean delivery: one block, parity present but not needed ---
    SnxVideo *v = snx_video_create();
    tcheck(v != NULL, "video create");
    sendFrame(v, 1000, 3000, 1, 2, true, false, -1, &f, &delivered);
    tcheck(delivered, "clean: frame delivered");
    tcheck(delivered && f.number == 1000 && f.size == 3000 && f.keyframe && !f.hevc,
           "clean: metadata");
    tcheck(delivered && f.data[0] == (uint8_t)(0 * 7 + 1000) &&
               f.data[2999] == (uint8_t)(2999 * 7 + 1000),
           "clean: payload bytes");
    tcheck(snx_video_stats(v)->shardsRecovered == 0, "clean: nothing to recover");
    snx_video_destroy(v);

    // --- one data shard lost, one parity: rebuilt byte-exact ---
    v = snx_video_create();
    sendFrame(v, 2000, 5000, 1, 1, true, false, 2, &f, &delivered); // drop shard 2
    tcheck(delivered, "parity: frame delivered despite the loss");
    tcheck(delivered && f.size == 5000 && f.data[2 * 1280] == (uint8_t)(2 * 1280 * 7 + 2000) &&
               f.data[2 * 1280 + 1279] == (uint8_t)((2 * 1280 + 1279) * 7 + 2000),
           "parity: lost shard rebuilt byte-exact");
    tcheck(snx_video_stats(v)->shardsRecovered == 1, "parity: one shard recovered");
    snx_video_destroy(v);

    // --- two data shards lost, one parity: frame is lost, keyframe gating ---
    v = snx_video_create();
    sendFrame(v, 3000, 3000, 1, 1, true, false, -1, &f, &delivered); // keyframe first
    tcheck(delivered, "gate: keyframe arrives first");
    // Two data shards lost with one parity shard: unrecoverable, the frame is lost.
    sendFrame2(v, 3001, 3000, 1, 1, false, false, 0, 1, &f, &delivered);
    tcheck(!delivered, "gate: unrecoverable frame not delivered");
    sendFrame(v, 3002, 3000, 1, 1, false, false, -1, &f, &delivered); // complete, no keyframe
    tcheck(!delivered, "gate: complete frame skipped while waiting for a keyframe");
    sendFrame(v, 3003, 3000, 1, 1, true, false, -1, &f, &delivered); // keyframe
    tcheck(delivered && f.number == 3003, "gate: keyframe resumes delivery");
    // The skip itself happens once 3003's arrival gives up on the lost 3001 and
    // flushes the queued 3002 past the keyframe gate.
    tcheck(snx_video_stats(v)->framesSkipped == 1, "gate: skipped counted");
    tcheck(snx_video_stats(v)->framesLost >= 1, "gate: loss counted");
    snx_video_destroy(v);

    // --- multi-block split: blocks carry their data and parity independently ---
    v = snx_video_create();
    sendFrame(v, 4000, 10000, 3, 1, true, true, -1, &f, &delivered); // 8 shards -> 3+3+2
    tcheck(delivered && f.hevc, "blocks: multi-block frame, hevc flag");
    tcheck(delivered && f.data[9999] == (uint8_t)(9999 * 7 + 4000), "blocks: payload intact");
    snx_video_destroy(v);

    // --- not our framing: rejected, no crash ---
    v = snx_video_create();
    uint8_t garbage[SNX_VIDEO_DATAGRAM_SIZE];
    memset(garbage, 0xEE, sizeof(garbage));
    tcheck(!snx_video_push(v, garbage, sizeof(garbage), &f), "garbage: rejected");
    tcheck(!snx_video_push(v, garbage, 100, &f), "short: rejected");
    tcheck(snx_video_stats(v)->datagramsRejected == 2, "garbage: counted");
    snx_video_destroy(v);

    return 0;
}
