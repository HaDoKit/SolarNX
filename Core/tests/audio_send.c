// A tiny SolarNX audio host for device tests: sends RTP-ish units (wire.h) with
// synthetic Opus payloads at 50/s (20ms frames), optionally dropping datagrams.
// The client's debug overlay shows the sequencer's counters:
//   ./audio_send <client-ip> [seconds] [loss-percent] [port]
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

static void sleepMs(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <client-ip> [seconds=10] [loss-percent=0] [port=%d]\n",
                argv[0], SNX_PORT_AUDIO);
        return 1;
    }
    const char *ip = argv[1];
    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    int lossPercent = argc > 3 ? atoi(argv[3]) : 0;
    int port = argc > 4 ? atoi(argv[4]) : SNX_PORT_AUDIO;

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

    uint16_t seq = (uint16_t)(rand32() & 0xFFFF);
    uint32_t timestamp = 0;
    size_t payloadLen = 160 + rand32() % 80; // a plausible 20ms stereo Opus size
    uint8_t unit[SNX_AUDIO_UNIT_HEADER_SIZE + 256];
    long sent = 0, dropped = 0;

    printf("sending 50 units/s for %ds to %s:%d (loss %d%%)\n", seconds, ip, port, lossPercent);
    int total = seconds * 50;
    for (int i = 0; i < total; i++)
    {
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
            for (size_t b = 0; b < payloadLen; b++)
                unit[SNX_AUDIO_UNIT_HEADER_SIZE + b] = (uint8_t)(seq + b);
            sendto(sock, unit, SNX_AUDIO_UNIT_HEADER_SIZE + payloadLen, 0,
                   (struct sockaddr *)&dst, sizeof(dst));
            sent++;
        }
        else
            dropped++;
        seq++;
        timestamp += SNX_AUDIO_SAMPLES_PER_FRAME;
        sleepMs(20);
    }
    printf("done: %ld units sent, %ld dropped\n", sent, dropped);
    close(sock);
    return 0;
}
