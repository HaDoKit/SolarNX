// A tiny SolarNX video host for device tests: sends frames in the wire format
// (wire.h) with real Reed-Solomon parity, at a given fps, optionally dropping
// datagrams. With a file argument it replays length-prefixed Annex-B access
// units (u32 BE size + AU bytes each - see the ffmpeg/au-split recipe in the
// repo history); otherwise the payload is deterministic filler (only the
// reassembly counters are meaningful then):
//   ./video_send <client-ip> [fps] [seconds] [loss-percent] [port] [frames-file] [hevc]
// This file is part of SolarNX (GPL-3.0).

#include <arpa/inet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "rs.h"
#include "solarnx/wire.h"

static uint32_t rand32(void)
{
    static uint32_t s = 0;
    if (s == 0)
        s = (uint32_t)time(NULL) ^ 0x5A17;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}

static long nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void sleepUntil(long deadlineMs)
{
    long left = deadlineMs - nowMs();
    if (left <= 0)
        return;
    struct timespec ts = {.tv_sec = left / 1000, .tv_nsec = left % 1000 * 1000000};
    nanosleep(&ts, NULL);
}

// True when the AU carries an IDR (H.264 type 5; HEVC type 19/20).
static bool au_is_keyframe(const uint8_t *au, size_t len, bool hevc)
{
    for (size_t i = 0; i + 4 < len; i++)
    {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)
        {
            uint8_t nal = au[i + 3];
            int type = hevc ? (nal >> 1) & 0x3F : nal & 0x1F;
            if ((!hevc && type == 5) || (hevc && (type == 19 || type == 20)))
                return true;
            if ((!hevc && type == 1) || (hevc && type == 1))
                return false; // a non-IDR slice first - delta frame
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <client-ip> [fps=30] [seconds=10] [loss-percent=0]"
                        " [port=%d] [frames-file] [hevc]\n",
                argv[0], SNX_PORT_VIDEO);
        return 1;
    }
    const char *ip = argv[1];
    int fps = argc > 2 ? atoi(argv[2]) : 30;
    int seconds = argc > 3 ? atoi(argv[3]) : 10;
    int lossPercent = argc > 4 ? atoi(argv[4]) : 0;
    int port = argc > 5 ? atoi(argv[5]) : SNX_PORT_VIDEO;
    const char *framesFile = argc > 6 ? argv[6] : NULL;
    bool hevc = argc > 7 ? atoi(argv[7]) != 0 : true;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return 1;
    }
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1)
    {
        fprintf(stderr, "bad address: %s\n", ip);
        return 1;
    }

    FILE *frames = NULL;
    if (framesFile != NULL)
    {
        frames = fopen(framesFile, "rb");
        if (frames == NULL)
            fprintf(stderr, "cannot open %s, falling back to synthetic frames\n",
                    framesFile);
    }

    rsInit();
    uint32_t number = rand32();
    long sent = 0, dropped = 0, frameCount = 0;
    static uint8_t auBuf[2 * 1024 * 1024];
    static uint8_t parityBuf[64 * SNX_VIDEO_SHARD_SIZE];

    printf("sending %d fps for %ds to %s:%d (loss %d%%, %s)\n", fps, seconds, ip, port,
           lossPercent, frames != NULL ? "file" : "synthetic");
    long start = nowMs();
    for (int f = 0; f < fps * seconds; f++)
    {
        sleepUntil(start + (long)f * 1000 / fps);
        uint32_t size;
        bool keyframe;
        if (frames != NULL)
        {
            uint8_t hdr[4];
            if (fread(hdr, 1, 4, frames) != 4) // loop the file
            {
                rewind(frames);
                if (fread(hdr, 1, 4, frames) != 4)
                    break;
            }
            size = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                   ((uint32_t)hdr[2] << 8) | hdr[3];
            if (size > sizeof(auBuf) || fread(auBuf, 1, size, frames) != size)
            {
                fprintf(stderr, "bad frames file at frame %d\n", f);
                break;
            }
            keyframe = au_is_keyframe(auBuf, size, hevc);
        }
        else
        {
            keyframe = f % fps == 0;
            size = keyframe ? 40000 + rand32() % 20000 : 2000 + rand32() % 2000;
            for (uint32_t i = 0; i < size; i++)
                auBuf[i] = (uint8_t)(i * 13 + number); // deterministic filler
        }

        int dataShards = (int)((size + SNX_VIDEO_SHARD_SIZE - 1) / SNX_VIDEO_SHARD_SIZE);
        uint8_t *frame = (uint8_t *)calloc(1, (size_t)dataShards * SNX_VIDEO_SHARD_SIZE);
        memcpy(frame, auBuf, size);

        // Same framing as the PC host (VideoSender.cs): blocks of at most 64 data
        // shards each with ~20% parity (min 2), datagrams sent interleaved -
        // shard 0 of every block, then shard 1 of every block, ...
        int blockCount = (dataShards + 63) / 64;
        int firstShard[256], blockData[256], blockParity[256], parityOffset[256];
        int parityTotal = 0, maxShardsInBlock = 0;
        for (int b = 0, first = 0; b < blockCount; b++)
        {
            blockData[b] = dataShards / blockCount + (b < dataShards % blockCount ? 1 : 0);
            int ratio = (blockData[b] * 20 + 99) / 100; // ceil(20%)
            blockParity[b] = ratio < 2 ? 2 : ratio;
            firstShard[b] = first;
            parityOffset[b] = parityTotal;
            first += blockData[b];
            parityTotal += blockParity[b];
            if (blockData[b] + blockParity[b] > maxShardsInBlock)
                maxShardsInBlock = blockData[b] + blockParity[b];
        }
        for (int b = 0; b < blockCount; b++)
        {
            uint8_t *shards[RS_MAX_SHARDS];
            for (int i = 0; i < blockData[b]; i++)
                shards[i] = frame + (size_t)(firstShard[b] + i) * SNX_VIDEO_SHARD_SIZE;
            uint8_t *parityPtrs[RS_MAX_PARITY];
            for (int p = 0; p < blockParity[b]; p++)
                parityPtrs[p] = parityBuf + (size_t)(parityOffset[b] + p) * SNX_VIDEO_SHARD_SIZE;
            rsEncode((const uint8_t *const *)shards, blockData[b], parityPtrs,
                     blockParity[b], SNX_VIDEO_SHARD_SIZE);
        }

        for (int shard = 0; shard < maxShardsInBlock; shard++)
        {
            for (int b = 0; b < blockCount; b++)
            {
                if (shard >= blockData[b] + blockParity[b])
                    continue;
                if ((int)(rand32() % 100) < lossPercent)
                {
                    dropped++;
                    continue; // simulate the loss
                }
                uint8_t dgram[SNX_VIDEO_DATAGRAM_SIZE];
                dgram[0] = SNX_VIDEO_MAGIC;
                dgram[1] = SNX_VIDEO_VERSION;
                dgram[2] = (uint8_t)((keyframe ? SNX_VIDEO_FLAG_KEYFRAME : 0) |
                                     (hevc ? SNX_VIDEO_FLAG_HEVC : 0));
                dgram[3] = (uint8_t)b;
                dgram[4] = (uint8_t)blockCount;
                dgram[5] = (uint8_t)shard;
                dgram[6] = (uint8_t)blockParity[b];
                dgram[7] = 0;
                memcpy(dgram + 8, &number, 4);
                memcpy(dgram + 12, &size, 4);
                memcpy(dgram + SNX_VIDEO_HEADER_SIZE,
                       shard < blockData[b]
                           ? frame + (size_t)(firstShard[b] + shard) * SNX_VIDEO_SHARD_SIZE
                           : parityBuf + (size_t)(parityOffset[b] + shard - blockData[b]) *
                                           SNX_VIDEO_SHARD_SIZE,
                       SNX_VIDEO_SHARD_SIZE);
                sendto(sock, dgram, sizeof(dgram), 0, (struct sockaddr *)&dst, sizeof(dst));
                sent++;
            }
        }
        free(frame);
        frameCount++;
        number++;
    }
    printf("done: %ld frames, %ld datagrams sent, %ld dropped\n", frameCount, sent, dropped);
    if (frames != NULL)
        fclose(frames);
    close(sock);
    return 0;
}
