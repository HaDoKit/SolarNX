// Generates an Opus frames file for audio_send: a stereo sine sweep encoded at
// 48 kHz/20 ms frames, written as (2-byte BE length + payload) units.
//   ./gen_tone <seconds> <out-file> [freq-hz]
// Build on the host against the vendored opus submodule. SolarNX (GPL-3.0).

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <opus.h>

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <seconds> <out-file> [freq-hz=440]\n", argv[0]);
        return 1;
    }
    int seconds = atoi(argv[1]);
    const char *outPath = argv[2];
    double freq = argc > 3 ? atof(argv[3]) : 440.0;

    int err = 0;
    OpusEncoder *enc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK)
    {
        fprintf(stderr, "opus_encoder_create: %d\n", err);
        return 1;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(96000));

    FILE *out = fopen(outPath, "wb");
    if (out == NULL)
    {
        perror("fopen");
        return 1;
    }

    int16_t pcm[960 * 2];
    uint8_t packet[1500];
    long frames = 0;
    for (int f = 0; f < seconds * 50; f++)
    {
        for (int i = 0; i < 960; i++)
        {
            double t = (f * 960 + i) / 48000.0;
            int16_t s = (int16_t)(sin(2.0 * M_PI * freq * t) * 12000);
            pcm[i * 2] = s;
            pcm[i * 2 + 1] = s;
        }
        opus_int32 n = opus_encode(enc, pcm, 960, packet, sizeof(packet));
        if (n < 0)
        {
            fprintf(stderr, "opus_encode: %d\n", n);
            return 1;
        }
        uint8_t hdr[2] = {(uint8_t)(n >> 8), (uint8_t)(n & 0xFF)};
        fwrite(hdr, 1, 2, out);
        fwrite(packet, 1, (size_t)n, out);
        frames++;
    }
    fclose(out);
    opus_encoder_destroy(enc);
    printf("wrote %ld frames (%ds) to %s\n", frames, seconds, outPath);
    return 0;
}
