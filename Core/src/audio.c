// Audio channel sequencing: validates the RTP-ish units (wire.h framing),
// tracks the sequence number, and hands packets out in order with the size of
// any gap before them - the platform's Opus decoder is the one that conceals
// missing frames (opus_decode(NULL)), so it needs to know how many to conceal.
// The Switch client's handleUnit logic (late/duplicate dropping, the ~0.5s
// concealment cap, resync on large jumps) ported instance-based.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"

#include <stdlib.h>
#include <string.h>

// ~0.5s cap - never try to conceal a long real drop, resync instead.
#define AUDIO_MAX_PLC_FRAMES_PER_GAP 25

struct SnxAudio {
    bool haveLastSeq;
    uint16_t lastSeq;
    SnxAudioStats stats;
};

SnxAudio *snx_audio_create(void)
{
    return (SnxAudio *)calloc(1, sizeof(SnxAudio));
}

void snx_audio_destroy(SnxAudio *a) { free(a); }

void snx_audio_reset(SnxAudio *a) { a->haveLastSeq = false; }

const SnxAudioStats *snx_audio_stats(const SnxAudio *a) { return &a->stats; }

bool snx_audio_push(SnxAudio *a, const uint8_t *datagram, size_t len, SnxAudioPacket *out)
{
    if (len < SNX_AUDIO_UNIT_HEADER_SIZE || len > SNX_AUDIO_DATAGRAM_MAX)
    {
        a->stats.datagramsRejected++;
        return false;
    }

    // V=2, P=0, X=0, CC=0 - the host always sends exactly this byte.
    if ((datagram[0] & 0xC0) != 0x80)
    {
        a->stats.datagramsRejected++;
        return false;
    }

    uint16_t payloadLength = (uint16_t)((datagram[12] << 8) | datagram[13]);
    if ((size_t)SNX_AUDIO_UNIT_HEADER_SIZE + payloadLength > len)
    {
        // length prefix disagrees with what actually arrived - drop rather than read OOB
        a->stats.datagramsRejected++;
        return false;
    }

    uint16_t seq = (uint16_t)((datagram[SNX_AUDIO_SEQ_OFFSET] << 8) |
                              datagram[SNX_AUDIO_SEQ_OFFSET + 1]);
    uint32_t timestamp;
    memcpy(&timestamp, datagram + 4, 4);
    timestamp = ((timestamp & 0xFF000000u) >> 24) | ((timestamp & 0x00FF0000u) >> 8) |
                ((timestamp & 0x0000FF00u) << 8) | ((timestamp & 0x000000FFu) << 24); // BE

    // A datagram behind the last one delivered was already concealed (or is a
    // duplicate): playing it now only adds delay. Not counted as a loss - the
    // host's adaptive bitrate reads concealment as link trouble.
    if (a->haveLastSeq)
    {
        int16_t ahead = (int16_t)(uint16_t)(seq - a->lastSeq);
        if (ahead <= 0 && ahead > -AUDIO_MAX_PLC_FRAMES_PER_GAP)
        {
            if (ahead == 0)
                a->stats.duplicates++;
            else
                a->stats.lateDropped++;
            return false;
        }
    }

    out->opus = datagram + SNX_AUDIO_UNIT_HEADER_SIZE;
    out->len = payloadLength;
    out->seq = seq;
    out->timestamp = timestamp;
    out->missingBefore = 0;

    if (a->haveLastSeq)
    {
        uint16_t missing = (uint16_t)(seq - a->lastSeq - 1);
        if (missing > 0 && missing <= AUDIO_MAX_PLC_FRAMES_PER_GAP)
        {
            out->missingBefore = missing;
            a->stats.framesConcealed += missing;
        }
        else if (missing > AUDIO_MAX_PLC_FRAMES_PER_GAP)
        {
            // A long real drop (or a new stream): don't conceal half a second,
            // resync here - the platform's decoder should flush.
            a->stats.resyncs++;
        }
    }

    a->lastSeq = seq;
    a->haveLastSeq = true;
    a->stats.packetsDelivered++;
    return true;
}
