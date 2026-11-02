#include "audio.h"

#include <stdio.h>
#include <string.h>
#include <malloc.h>

#include <switch.h>
#include <opus/opus.h>

#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "../network/network.h"
#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"

// Wire format (see SolarNX/Audio/RtpPacketizer.cs, RtpPacketizer.Wrap): a 12-byte RTP-like header
// (V=2,P=0,X=0,CC=0; PT; u16 BE seq; u32 BE timestamp; u32 BE SSRC), followed by a 2-byte
// big-endian length prefix - NOT part of real RTP, a deliberate deviation the host's own comment
// explains (it exists for a receiver that might see several units coalesced into one read) - then
// the raw Opus frame. On this side, a plain UDP socket (not libavformat/avio - see below) means
// each recv() already returns exactly one datagram = exactly one such unit, so there's nothing to
// walk or reassemble.
#define RTP_HEADER_SIZE        12
#define RTP_LENGTH_PREFIX_SIZE 2
#define RTP_UNIT_HEADER_SIZE   (RTP_HEADER_SIZE + RTP_LENGTH_PREFIX_SIZE)
#define RTP_SEQ_OFFSET         2

// Matches AudioCaptureSender.cs: AudioSampleRate=48000, AudioChannels=2, FrameDurationMs=20.
#define AUDIO_SAMPLE_RATE       48000
#define AUDIO_CHANNELS          2
#define AUDIO_FRAME_DURATION_MS 20
#define AUDIO_SAMPLES_PER_FRAME (AUDIO_SAMPLE_RATE * AUDIO_FRAME_DURATION_MS / 1000) // 960

// One datagram = one RTP-ish unit; a 20ms Opus stereo frame at this app's 128kbps target is a few
// hundred bytes at most (see AudioCaptureSender.cs's OpusBitrateBps) - generous headroom, not a
// tight worst case.
#define AUDIO_DATAGRAM_BUFFER_SIZE 2048

#define AUDIO_RECV_TIMEOUT_US         200000 // 200ms - just for periodic appIsRunning() checks
#define AUDIO_MAX_PLC_FRAMES_PER_GAP  25       // ~0.5s cap - never try to conceal a long real drop

#define AUDIO_RECONNECT_DELAY_NS     1000000000ULL // 1s
#define AUDIO_HEARTBEAT_EVERY_FRAMES 250u           // ~5s at 50 frames/s (20ms frames)

#define AUDIO_OUT_BUFFER_COUNT      4
#define AUDIO_OUT_BUFFER_DATA_SIZE  (AUDIO_SAMPLES_PER_FRAME * AUDIO_CHANNELS * (int)sizeof(s16))
#define AUDIO_OUT_BUFFER_SIZE       ((AUDIO_OUT_BUFFER_DATA_SIZE + 0xFFF) & ~0xFFF)

static OpusDecoder *s_opusDecoder = NULL;
static bool s_audoutReady = false;

// See audio.h's doc comment on the getters below - fed to the host as link-quality telemetry.
static uint32_t s_audioReconnectCount = 0;
static uint32_t s_audioConcealedFrameCount = 0;

uint32_t audioGetReconnectCount(void)
{
    return s_audioReconnectCount;
}

uint32_t audioGetConcealedFrameCount(void)
{
    return s_audioConcealedFrameCount;
}

static AudioOutBuffer s_outBuffers[AUDIO_OUT_BUFFER_COUNT];
static u8 *s_outBufferData[AUDIO_OUT_BUFFER_COUNT];
static bool s_outBufferInFlight[AUDIO_OUT_BUFFER_COUNT];

bool audioInit(void)
{
    // See logger.h's doc comment on logInstallFfmpegCallback - unifies FFmpeg's own av_log output
    // with logWrite()'s mutex instead of racing it. Kept even though audio.c no longer uses
    // libavformat itself (see runStreamSession) - video.c still does.
    logInstallFfmpegCallback();

    if (R_FAILED(audoutInitialize()))
    {
        logWrite("audio", "audoutInitialize failed");
        return false;
    }

    if (R_FAILED(audoutStartAudioOut()))
    {
        logWrite("audio", "audoutStartAudioOut failed");
        return false;
    }
    s_audoutReady = true;

    for (int i = 0; i < AUDIO_OUT_BUFFER_COUNT; i++)
    {
        s_outBufferData[i] = (u8 *)memalign(0x1000, AUDIO_OUT_BUFFER_SIZE);
        if (s_outBufferData[i] == NULL)
        {
            logWrite("audio", "failed to allocate audout buffer %d", i);
            return false;
        }
        memset(s_outBufferData[i], 0, AUDIO_OUT_BUFFER_SIZE);
        s_outBufferInFlight[i] = false;
    }

    int opusError = OPUS_OK;
    s_opusDecoder = opus_decoder_create(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, &opusError);
    if (s_opusDecoder == NULL || opusError != OPUS_OK)
    {
        logWrite("audio", "opus_decoder_create failed: %s", opus_strerror(opusError));
        return false;
    }

    logWrite("audio", "audout ready (%uHz, %u ch), opus decoder ready",
        audoutGetSampleRate(), audoutGetChannelCount());
    return true;
}

// Reclaims any audout buffers that finished playing, then submits pcm for playback in a free
// slot. AUDIO_OUT_BUFFER_COUNT * 20ms = 80ms of buffering headroom means a free slot should
// normally appear within a millisecond or two; if audout is genuinely falling behind, waits
// briefly rather than dropping this frame immediately.
static bool submitPcm(const s16 *pcm, u32 sampleCount)
{
    for (int attempt = 0; attempt < 50; attempt++)
    {
        AudioOutBuffer *released = NULL;
        u32 releasedCount = 0;
        audoutGetReleasedAudioOutBuffer(&released, &releasedCount);
        for (AudioOutBuffer *b = released; b != NULL; b = b->next)
        {
            for (int i = 0; i < AUDIO_OUT_BUFFER_COUNT; i++)
                if (&s_outBuffers[i] == b)
                    s_outBufferInFlight[i] = false;
        }

        int slot = -1;
        for (int i = 0; i < AUDIO_OUT_BUFFER_COUNT; i++)
        {
            if (!s_outBufferInFlight[i]) { slot = i; break; }
        }

        if (slot >= 0)
        {
            size_t dataSize = (size_t)sampleCount * AUDIO_CHANNELS * sizeof(s16);
            memcpy(s_outBufferData[slot], pcm, dataSize);
            // CPU writes to this buffer aren't guaranteed visible to the audio service until
            // flushed - matches the devkitPro hwopus-decoder example's use of the same call
            // before handing a written buffer off.
            armDCacheFlush(s_outBufferData[slot], dataSize);

            s_outBuffers[slot].next = NULL;
            s_outBuffers[slot].buffer = s_outBufferData[slot];
            s_outBuffers[slot].buffer_size = AUDIO_OUT_BUFFER_SIZE;
            s_outBuffers[slot].data_size = dataSize;
            s_outBuffers[slot].data_offset = 0;

            if (R_FAILED(audoutAppendAudioOutBuffer(&s_outBuffers[slot])))
                return false;

            s_outBufferInFlight[slot] = true;
            return true;
        }

        svcSleepThread(1000000ULL); // 1ms
    }

    return false; // every buffer still in flight after ~50ms - drop this frame
}

// Decodes and plays one already-received RTP-ish unit (header+prefix+Opus payload), handling PLC
// for any sequence-number gap since the last one.
static void handleUnit(const uint8_t *unit, int len, u32 *framesDecoded, bool *haveLastSeq, u16 *lastSeq, s16 *pcm)
{
    if (len < RTP_UNIT_HEADER_SIZE)
        return; // too short to even hold the header - drop

    u16 payloadLength = ((u16)unit[12] << 8) | unit[13];
    if (RTP_UNIT_HEADER_SIZE + payloadLength > len)
        return; // length prefix disagrees with what actually arrived - drop rather than read OOB

    u16 seq = ((u16)unit[RTP_SEQ_OFFSET] << 8) | unit[RTP_SEQ_OFFSET + 1];
    const uint8_t *opusPayload = unit + RTP_UNIT_HEADER_SIZE;

    // Opus's own packet-loss concealment synthesizes plausible audio for a missing frame instead
    // of a gap or a naive repeat - AudioCaptureSender.cs's doc comment explicitly says the
    // client's RTP jitter buffer/Opus PLC is what's meant to absorb gaps, not a host-side retry.
    // A sequence-number gap here is exactly that signal.
    if (*haveLastSeq)
    {
        u16 missing = (u16)(seq - *lastSeq - 1);
        if (missing > 0 && missing <= AUDIO_MAX_PLC_FRAMES_PER_GAP)
        {
            for (u16 i = 0; i < missing; i++)
            {
                int plcSamples = opus_decode(s_opusDecoder, NULL, 0, pcm, AUDIO_SAMPLES_PER_FRAME, 0);
                if (plcSamples > 0)
                {
                    submitPcm(pcm, (u32)plcSamples);
                    s_audioConcealedFrameCount++;
                }
            }
            logWrite("audio", "concealed %u missing frame(s) (seq %u -> %u)", missing, *lastSeq, seq);
        }
        else if (missing > AUDIO_MAX_PLC_FRAMES_PER_GAP)
        {
            logWrite("audio", "large sequence gap (%u frames, seq %u -> %u) - not concealing, resyncing", missing, *lastSeq, seq);
        }
    }
    *lastSeq = seq;
    *haveLastSeq = true;

    int decodedSamples = opus_decode(s_opusDecoder, opusPayload, payloadLength, pcm, AUDIO_SAMPLES_PER_FRAME, 0);
    if (decodedSamples > 0)
    {
        submitPcm(pcm, (u32)decodedSamples);
        (*framesDecoded)++;
        if (*framesDecoded % AUDIO_HEARTBEAT_EVERY_FRAMES == 0)
            logWrite("audio", "connection stable - %u frames decoded", *framesDecoded);
    }
    else
    {
        logWrite("audio", "opus_decode failed: %s", opus_strerror(decodedSamples));
    }
}

// One connect-decode-play cycle; returns when the socket errors or the app is exiting, so the
// caller (audioStart) decides whether to reconnect.
//
// Plain BSD UDP socket, not libavformat/avio - this used to go through avio_open2/avio_read_partial
// (matching what video.c then did for its MPEG-TS input), but that combination was confirmed live to
// crash the whole process after ~2s of running alongside the HID thread, consistently, regardless
// of bsd session headroom or poll interval (both changed independently with zero effect on the
// crash timing) or of unifying FFmpeg's av_log with logWrite()'s mutex (also no effect) - and the
// crash reproduced with video entirely disabled, while HID alone (network.c, this same raw-socket
// approach) ran stable well past that point. Audio doesn't need libavformat's demuxing at all (no
// container, just this app's own length-prefixed framing), so this switches to the same
// already-proven-stable raw-socket pattern network.c uses instead of chasing a bug inside a
// library layer neither of us can step through on-device.
static void runStreamSession(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        logWrite("audio", "socket() failed");
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SOLARNX_AUDIO_PORT);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        logWrite("audio", "bind() failed on port %d", SOLARNX_AUDIO_PORT);
        close(sock);
        return;
    }

    // Just so recv() returns periodically to let this loop re-check appIsRunning(), not a
    // real per-packet deadline.
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = AUDIO_RECV_TIMEOUT_US;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // This socket has no userspace buffering of its own (as video.c's socket now too) - it relies
    // entirely on the OS's own receive buffer to absorb this thread being briefly scheduled late
    // (e.g. while video.c's decode/transfer is CPU-heavy on another thread). 64KB is ~4s of this
    // stream's audio (OpusBitrateBps=128000) - generous headroom against a transient stall without
    // the runaway-latency problem a multi-second buffer caused on the video side.
    int rcvBufSize = 65536;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvBufSize, sizeof(rcvBufSize));

    uint8_t datagram[AUDIO_DATAGRAM_BUFFER_SIZE];
    s16 pcm[AUDIO_SAMPLES_PER_FRAME * AUDIO_CHANNELS];
    u32 framesDecoded = 0;
    bool haveLastSeq = false;
    u16 lastSeq = 0;

    // appIsRunning(), never appletMainLoop(): this runs on the audio thread, and appletMainLoop()
    // here used to race the main thread for the applet's exit request - see common/appstate.
    while (appIsRunning())
    {
        ssize_t received = recv(sock, datagram, sizeof(datagram), 0);
        if (received < 0)
        {
            // SO_RCVTIMEO expiring with nothing received also comes back as -1 with errno
            // EAGAIN/EWOULDBLOCK - that's the common idle case while waiting for a stream, not a
            // real error, so just keep going. Any other errno is a genuine socket failure - previously
            // treated identically to a timeout, which meant a real error would spin this loop
            // calling recv() as fast as possible forever instead of ever breaking out to reconnect.
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;

            logWrite("audio", "recv() failed (errno %d) after %u frames - reconnecting", errno, framesDecoded);
            break;
        }

        handleUnit(datagram, (int)received, &framesDecoded, &haveLastSeq, &lastSeq, pcm);
    }

    close(sock);
}

bool audioStart(void)
{
    if (!s_audoutReady || s_opusDecoder == NULL)
    {
        logWrite("audio", "audioStart() called before audioInit() succeeded");
        return false;
    }

    while (appIsRunning())
    {
        logWrite("audio", "waiting for audio stream on port %d...", SOLARNX_AUDIO_PORT);
        runStreamSession();
        s_audioReconnectCount++;
        svcSleepThread(AUDIO_RECONNECT_DELAY_NS);
    }

    return true;
}

void audioExit(void)
{
    if (s_opusDecoder != NULL)
    {
        opus_decoder_destroy(s_opusDecoder);
        s_opusDecoder = NULL;
    }

    for (int i = 0; i < AUDIO_OUT_BUFFER_COUNT; i++)
    {
        if (s_outBufferData[i] != NULL)
        {
            free(s_outBufferData[i]);
            s_outBufferData[i] = NULL;
        }
    }

    if (s_audoutReady)
    {
        audoutStopAudioOut();
        audoutExit();
        s_audoutReady = false;
    }
}
