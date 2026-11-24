// Audio sequencing tests against crafted RTP-ish units: clean order, gaps
// (PLC accounting), late/duplicate dropping, resync on large jumps, bad input.
// This file is part of SolarNX (GPL-3.0).

#include <string.h>

#include "internal.h"

#include "test_harness.h"

// Builds one unit: V=2 header, BE seq, BE timestamp, BE length, filler payload.
static void makeUnit(uint8_t *out, uint16_t seq, uint32_t timestamp, size_t payloadLen)
{
    memset(out, 0, SNX_AUDIO_UNIT_HEADER_SIZE + payloadLen);
    out[0] = 0x80; // V=2, P=X=CC=0
    out[1] = 0x60; // PT - unused by the receiver
    out[2] = (uint8_t)(seq >> 8);
    out[3] = (uint8_t)(seq & 0xFF);
    out[4] = (uint8_t)(timestamp >> 24);
    out[5] = (uint8_t)(timestamp >> 16);
    out[6] = (uint8_t)(timestamp >> 8);
    out[7] = (uint8_t)timestamp;
    out[8] = 0x11; out[9] = 0x22; out[10] = 0x33; out[11] = 0x44; // SSRC
    out[12] = (uint8_t)(payloadLen >> 8);
    out[13] = (uint8_t)(payloadLen & 0xFF);
    for (size_t i = 0; i < payloadLen; i++)
        out[SNX_AUDIO_UNIT_HEADER_SIZE + i] = (uint8_t)(seq + i);
}

int test_audio(void)
{
    SnxAudio *a = snx_audio_create();
    SnxAudioPacket p;
    uint8_t unit[SNX_AUDIO_UNIT_HEADER_SIZE + 200];

    // --- clean in-order delivery ---
    makeUnit(unit, 100, 48000, 200);
    tcheck(snx_audio_push(a, unit, SNX_AUDIO_UNIT_HEADER_SIZE + 200, &p), "clean: first");
    tcheck(p.seq == 100 && p.len == 200 && p.missingBefore == 0, "clean: metadata");
    tcheck(p.opus[0] == 100 && p.opus[199] == (uint8_t)(100 + 199), "clean: payload pointer");

    makeUnit(unit, 101, 49200, 150);
    tcheck(snx_audio_push(a, unit, SNX_AUDIO_UNIT_HEADER_SIZE + 150, &p), "clean: second");
    tcheck(p.seq == 101 && p.missingBefore == 0, "clean: no gap");

    // --- gap of 2: the decoder is asked to conceal ---
    makeUnit(unit, 104, 54000, 100);
    tcheck(snx_audio_push(a, unit, SNX_AUDIO_UNIT_HEADER_SIZE + 100, &p), "gap: delivered");
    tcheck(p.seq == 104 && p.missingBefore == 2, "gap: two frames to conceal");
    tcheck(snx_audio_stats(a)->framesConcealed == 2, "gap: concealed counted");

    // --- late and duplicate: dropped ---
    makeUnit(unit, 102, 50400, 100); // the concealed one arriving late
    tcheck(!snx_audio_push(a, unit, SNX_AUDIO_UNIT_HEADER_SIZE + 100, &p), "late: dropped");
    makeUnit(unit, 104, 54000, 100); // exact duplicate
    tcheck(!snx_audio_push(a, unit, SNX_AUDIO_UNIT_HEADER_SIZE + 100, &p), "dup: dropped");
    tcheck(snx_audio_stats(a)->lateDropped == 1 && snx_audio_stats(a)->duplicates == 1,
           "late/dup: counted");

    // --- large jump: resync, no absurd concealment ---
    makeUnit(unit, 300, 90000, 100);
    tcheck(snx_audio_push(a, unit, SNX_AUDIO_UNIT_HEADER_SIZE + 100, &p), "resync: delivered");
    tcheck(p.missingBefore == 0, "resync: not asked to conceal ~200 frames");
    tcheck(snx_audio_stats(a)->resyncs == 1, "resync: counted");

    // --- malformed: rejected ---
    uint8_t bad[64];
    memset(bad, 0, sizeof(bad));
    bad[0] = 0x40; // wrong RTP version
    tcheck(!snx_audio_push(a, bad, sizeof(bad), &p), "bad: version rejected");
    makeUnit(unit, 301, 91000, 200);
    unit[12] = 0xFF; // length prefix beyond the datagram
    tcheck(!snx_audio_push(a, unit, SNX_AUDIO_UNIT_HEADER_SIZE + 50, &p),
           "bad: length prefix rejected");
    tcheck(snx_audio_stats(a)->datagramsRejected == 2, "bad: counted");
    tcheck(!snx_audio_push(a, unit, 5, &p), "bad: too short");

    // --- stats sanity ---
    tcheck(snx_audio_stats(a)->packetsDelivered == 4, "stats: four delivered");
    snx_audio_destroy(a);
    return 0;
}
