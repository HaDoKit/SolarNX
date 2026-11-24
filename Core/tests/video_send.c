// A tiny SolarNX video host for device tests: sends synthetic frames in the
// wire format (wire.h) with real Reed-Solomon parity, at a given fps, optionally
// dropping datagrams to exercise the recovery paths. The client's debug overlay
// shows the reassembled frame counters - connect and watch:
//   ./video_send <client-ip> [fps] [seconds] [loss-percent] [port]
// This file is part of SolarNX (GPL-3.0).

#include <arpa/inet.h>
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

static void sleepMs(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <client-ip> [fps=30] [seconds=10] [loss-percent=0] [port=%d]\n",
                argv[0], SNX_PORT_VIDEO);
        return 1;
    }
    const char *ip = argv[1];
    int fps = argc > 2 ? atoi(argv[2]) : 30;
    int seconds = argc > 3 ? atoi(argv[3]) : 10;
    int lossPercent = argc > 4 ? atoi(argv[4]) : 0;
    int port = argc > 5 ? atoi(argv[5]) : SNX_PORT_VIDEO;

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

    rsInit();
    uint32_t number = rand32();
    int parityCount = 3;    // per RS block - enough for the loss tests
    int blockCount = 2;
    long sent = 0, dropped = 0, frames = 0;

    printf("sending %d fps for %ds to %s:%d (loss %d%%)\n", fps, seconds, ip, port, lossPercent);
    for (int f = 0; f < fps * seconds; f++)
    {
        // A keyframe every second, "H.265", random-ish sizes.
        bool keyframe = f % fps == 0;
        uint32_t size = keyframe ? 40000 + rand32() % 20000 : 2000 + rand32() % 2000;
        int dataShards = (int)((size + SNX_VIDEO_SHARD_SIZE - 1) / SNX_VIDEO_SHARD_SIZE);

        uint8_t *frame = (uint8_t *)calloc(1, (size_t)dataShards * SNX_VIDEO_SHARD_SIZE);
        for (uint32_t i = 0; i < size; i++)
            frame[i] = (uint8_t)(i * 13 + number); // deterministic filler

        for (int b = 0, first = 0; b < blockCount; b++)
        {
            int count = dataShards / blockCount + (b < dataShards % blockCount ? 1 : 0);
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
                if ((int)(rand32() % 100) < lossPercent)
                {
                    dropped++;
                    continue; // simulate the loss
                }
                uint8_t dgram[SNX_VIDEO_DATAGRAM_SIZE];
                dgram[0] = SNX_VIDEO_MAGIC;
                dgram[1] = SNX_VIDEO_VERSION;
                dgram[2] = (uint8_t)((keyframe ? SNX_VIDEO_FLAG_KEYFRAME : 0) |
                                     SNX_VIDEO_FLAG_HEVC);
                dgram[3] = (uint8_t)b;
                dgram[4] = (uint8_t)blockCount;
                dgram[5] = (uint8_t)i;
                dgram[6] = (uint8_t)parityCount;
                dgram[7] = 0;
                memcpy(dgram + 8, &number, 4);
                memcpy(dgram + 12, &size, 4);
                memcpy(dgram + SNX_VIDEO_HEADER_SIZE,
                       i < count ? shards[i] : parityPtrs[i - count], SNX_VIDEO_SHARD_SIZE);
                sendto(sock, dgram, sizeof(dgram), 0, (struct sockaddr *)&dst, sizeof(dst));
                sent++;
            }
            free(parity);
            first += count;
        }
        free(frame);
        frames++;
        number++;
        sleepMs(1000 / fps);
    }
    printf("done: %ld frames, %ld datagrams sent, %ld dropped\n", frames, sent, dropped);
    close(sock);
    return 0;
}
