// A tiny SolarNX audio host for device tests: sends RTP-ish units (wire.h) at
// 50/s (20ms frames), optionally dropping datagrams. With a file argument it
// replays real Opus frames (2-byte BE length + payload each, see gen_tone.c);
// otherwise the payload is synthetic (client-side decode then fails, which is
// still useful for sequencer/loss-counter checks):
//   ./audio_send <client-ip> [seconds] [loss-percent] [port] [frames-file]
// This file is part of SolarNX (GPL-3.0).

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "solarnx/wire.h"

static uint32_t rand32(void)
{
    static uint32_t s = 0;
    if (s == 0)
        s = (uint32_t)time(NULL) ^ 0xA0D10;
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

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <client-ip> [seconds=10] [loss-percent=0] [port=%d]"
                        " [frames-file]\n", argv[0], SNX_PORT_AUDIO);
        return 1;
    }
    const char *ip = argv[1];
    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    int lossPercent = argc > 3 ? atoi(argv[3]) : 0;
    int port = argc > 4 ? atoi(argv[4]) : SNX_PORT_AUDIO;
    const char *framesFile = argc > 5 ? argv[5] : NULL;

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
            fprintf(stderr, "cannot open %s, falling back to synthetic payloads\n",
                    framesFile);
    }

    uint16_t seq = (uint16_t)(rand32() & 0xFFFF);
    uint32_t timestamp = 0;
    size_t payloadLen = 160 + rand32() % 80; // a plausible 20ms stereo Opus size
    uint8_t unit[SNX_AUDIO_UNIT_HEADER_SIZE + SNX_AUDIO_DATAGRAM_MAX];
    uint8_t payload[SNX_AUDIO_DATAGRAM_MAX];
    long sent = 0, dropped = 0;

    printf("sending 50 units/s for %ds to %s:%d (loss %d%%)\n", seconds, ip, port, lossPercent);
    int total = seconds * 50;
    long start = nowMs();
    for (int i = 0; i < total; i++)
    {
        sleepUntil(start + i * 20); // real-time pacing, catches up after overruns
        if (frames != NULL)
        {
            uint8_t hdr[2];
            if (fread(hdr, 1, 2, frames) == 2)
            {
                payloadLen = ((size_t)hdr[0] << 8) | hdr[1];
                if (payloadLen > sizeof(payload) ||
                    fread(payload, 1, payloadLen, frames) != payloadLen)
                {
                    fprintf(stderr, "bad frames file at unit %d\n", i);
                    break;
                }
            }
            else // loop the file
            {
                rewind(frames);
                i--;
                continue;
            }
        }
        else
            for (size_t b = 0; b < payloadLen; b++)
                payload[b] = (uint8_t)(seq + b);

        if ((int)(rand32() % 100) >= lossPercent)
        {
            memset(unit, 0, sizeof(unit));
            unit[0] = 0x80; // V=2
            unit[1] = 0x60;
            unit[2] = (uint8_t)(seq >> 8);
            unit[3] = (uint8_t)(seq & 0xFF);
            unit[4] = (uint8_t)(timestamp >> 24);
            unit[5] = (uint8_t)(timestamp >> 16);
            unit[6] = (uint8_t)(timestamp >> 8);
            unit[7] = (uint8_t)timestamp;
            unit[12] = (uint8_t)(payloadLen >> 8);
            unit[13] = (uint8_t)(payloadLen & 0xFF);
            memcpy(unit + SNX_AUDIO_UNIT_HEADER_SIZE, payload, payloadLen);
            sendto(sock, unit, SNX_AUDIO_UNIT_HEADER_SIZE + payloadLen, 0,
                   (struct sockaddr *)&dst, sizeof(dst));
            sent++;
        }
        else
            dropped++;
        seq++;
        timestamp += SNX_AUDIO_SAMPLES_PER_FRAME;
    }
    printf("done: %ld units sent, %ld dropped\n", sent, dropped);
    if (frames != NULL)
        fclose(frames);
    close(sock);
    return 0;
}
