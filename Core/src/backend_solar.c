// SolarNX backend: the built-in PC service protocol, LAN path.
//
// Channels (all live in one poll loop):
//   - library TCP 19003: the PC dials in and pushes its game library;
//   - HID TCP 19001: the PC dials in, sends the 28-byte SessionConfig, then the
//     core streams the platform's input state at ~120 Hz (8x0xFF marker + the
//     224-byte packet) and reads the PC's control messages in between.
// The PC closing the HID connection just goes back to accept - same as the
// Switch client's reconnect behavior.
// Video (UDP 19000) and audio (UDP 19002) land on this skeleton next.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int libListenFd;
    int hidListenFd;
    int hidFd;              // -1 while waiting for the PC to dial in
    uint64_t lastHidSendMs; // pacing for the ~120 Hz input cadence
    // Partial control message from the PC: u16 length + u8 type + payload.
    uint8_t ctlBuf[2 + 1 + 1024];
    size_t ctlFill;
    // Video channel: UDP receiver + the frame reassembly.
    int videoFd;
    SnxVideo *video;
    // Audio channel: UDP receiver + the RTP-ish sequencing.
    int audioFd;
    SnxAudio *audio;
    uint32_t sessionBitrateKbps; // from the host's SessionConfig - echoed back in telemetry
} SolarBackend;

// ~120 Hz: a press waits for the next send, so the interval adds its own length
// to the input lag. 232 bytes at 120 Hz is ~28 KB/s - nothing for Wi-Fi.
#define HID_SEND_INTERVAL_MS 8

static uint64_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static bool fdRead(void *io, void *buf, size_t len)
{
    return snx__read_all(*(int *)io, buf, len);
}

static bool fdWrite(void *io, const void *buf, size_t len)
{
    return snx__write_all(*(int *)io, buf, len);
}

static bool solarStart(SnxCore *core, SnxPath path)
{
    if (path != SNX_PATH_LAN)
        return false; // the Internet path needs the encrypted transport (later milestone)

    SolarBackend *b = (SolarBackend *)calloc(1, sizeof(*b));
    if (b == NULL)
        return false;
    b->libListenFd = -1;
    b->hidListenFd = -1;
    b->hidFd = -1;
    b->videoFd = -1;
    core->backendData = b;

    b->libListenFd = snx__tcp_listen(core->cfg.libraryPort > 0 ? core->cfg.libraryPort
                                                               : SNX_PORT_LIBRARY);
    int hidPort = core->cfg.hidPort > 0 ? core->cfg.hidPort : SNX_PORT_HID;
    b->hidListenFd = snx__tcp_listen(hidPort);
    int videoPort = core->cfg.videoPort > 0 ? core->cfg.videoPort : SNX_PORT_VIDEO;
    b->videoFd = snx__udp_listen(videoPort);
    b->video = snx_video_create();
    int audioPort = core->cfg.audioPort > 0 ? core->cfg.audioPort : SNX_PORT_AUDIO;
    b->audioFd = snx__udp_listen(audioPort);
    b->audio = snx_audio_create();
    if (b->libListenFd < 0 || b->hidListenFd < 0 || b->videoFd < 0 || b->video == NULL ||
        b->audioFd < 0 || b->audio == NULL)
    {
        snx__log(core, 2, "listen failed: library=%d hid=%d video=%d audio=%d", b->libListenFd,
                 b->hidListenFd, b->videoFd, b->audioFd);
        if (b->libListenFd >= 0) close(b->libListenFd);
        if (b->hidListenFd >= 0) close(b->hidListenFd);
        if (b->videoFd >= 0) close(b->videoFd);
        if (b->audioFd >= 0) close(b->audioFd);
        snx_video_destroy(b->video);
        snx_audio_destroy(b->audio);
        free(b);
        core->backendData = NULL;
        return false;
    }
    core->libListenFd = b->libListenFd;
    return true;
}

static void solarStop(SnxCore *core)
{
    SolarBackend *b = (SolarBackend *)core->backendData;
    if (b == NULL)
        return;
    if (b->libListenFd >= 0)
        close(b->libListenFd);
    if (b->hidListenFd >= 0)
        close(b->hidListenFd);
    if (b->hidFd >= 0)
        close(b->hidFd);
    if (b->videoFd >= 0)
        close(b->videoFd);
    if (b->audioFd >= 0)
        close(b->audioFd);
    snx_video_destroy(b->video);
    snx_audio_destroy(b->audio);
    free(b);
    core->backendData = NULL;
    core->libListenFd = -1;
}

static void deliverLibrary(SnxCore *core, const SnxLibEntryRaw *entries, size_t count)
{
    core->statLibGames = (uint32_t)count;
    if (core->cb.on_library == NULL)
        return;
    SnxLibraryGame *games = (SnxLibraryGame *)calloc(count > 0 ? count : 1, sizeof(*games));
    if (games == NULL)
        return;
    for (size_t i = 0; i < count; i++)
    {
        games[i].id = entries[i].id;
        games[i].name = entries[i].name;
        games[i].cover = entries[i].cover;
        games[i].coverSize = entries[i].coverSize;
    }
    core->cb.on_library(core->cbCtx, games, count);
    free(games);
}

static void handleLibraryConnection(SnxCore *core, SolarBackend *b)
{
    int fd = snx__accept_ms(b->libListenFd, 0);
    if (fd < 0)
        return; // -1 nothing waiting, -2 transient accept error: keep listening

    // A host that stalls mid-transfer must not park the core forever. The handshake
    // bursts are tiny, so a short bound is plenty.
    snx__io_timeout(fd, 5);

    SnxLibraryInfo info;
    size_t count = 0;
    SnxLibEntryRaw *entries = snx__library_session(
        fdRead, fdWrite, &fd, core->cfg.appVersion, core->libHashValid, core->libHash,
        &info, &count);

    if (entries != NULL)
    {
        core->libHash = info.hash;
        core->libHashValid = true;
        deliverLibrary(core, entries, count);
        snx__library_free(entries, count);
        snx__set_state(core, SNX_STATE_LIBRARY, "library received");
    }
    else if (info.pcAnnounced)
    {
        // Connected and answered (even if "nothing new" or an incompatible major):
        // the PC is attached - report what it said about itself.
        char detail[96];
        snprintf(detail, sizeof(detail), "host: %s (protocol %u.%u)", info.pc.app,
                 info.pc.major, info.pc.minor);
        snx__set_state(core, SNX_STATE_LIBRARY, detail);
    }
    close(fd);
}

// The PC's control stream after the SessionConfig: u16 length (type + payload) +
// u8 type + payload, back to back. Returns false when the connection is gone.
static bool pumpControlMessages(SnxCore *core, SolarBackend *b)
{
    for (;;)
    {
        if (b->ctlFill >= 3)
        {
            uint16_t len;
            memcpy(&len, b->ctlBuf, 2);
            if (len < 1 || len > 1 + 1024)
                return false; // nonsense length: treat the connection as broken
            if (b->ctlFill >= (size_t)2 + len)
            {
                uint8_t type = b->ctlBuf[2];
                const uint8_t *payload = b->ctlBuf + 3;
                size_t payloadLen = (size_t)len - 1;
                // Options (u8 flags + u8[3] zero): TrafficPriority asks us to
                // mark our own HID packets with DSCP EF.
                if (type == 3 && payloadLen >= 1)
                {
                    int tos = (payload[0] & 0x01) ? 0xB8 : 0; // EF << 2
                    setsockopt(b->hidFd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
                }
                if (core->cb.on_control != NULL)
                    core->cb.on_control(core->cbCtx, type, payload, payloadLen);
                memmove(b->ctlBuf, b->ctlBuf + 2 + len, b->ctlFill - (size_t)2 - len);
                b->ctlFill -= (size_t)2 + len;
                continue;
            }
        }

        // Top up from the socket: non-blocking-ish read with the poll budget.
        size_t want = sizeof(b->ctlBuf) - b->ctlFill;
        if (want == 0)
            return false;
        ssize_t n = recv(b->hidFd, b->ctlBuf + b->ctlFill, want, MSG_DONTWAIT);
        if (n > 0)
        {
            b->ctlFill += (size_t)n;
            continue;
        }
        if (n == 0)
            return false;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return true; // nothing more for now
        return false;
    }
}

static void acceptHid(SnxCore *core, SolarBackend *b)
{
    int fd = snx__accept_ms(b->hidListenFd, 0);
    if (fd < 0)
        return;

    // The host sends the 28-byte SessionConfig immediately after connecting.
    snx__io_timeout(fd, 5);
    SnxSessionConfig cfg;
    if (!snx__read_all(fd, &cfg, sizeof(cfg)))
    {
        close(fd);
        return;
    }
    b->hidFd = fd;
    b->ctlFill = 0;
    b->sessionBitrateKbps = cfg.videoBitrateKbps;
    b->lastHidSendMs = 0; // send the first packet on the next poll tick

    // Protocol 1.1: the client announces itself first - one control frame
    // (8x0xFE marker, u16 length = type+payload, u8 Hello, version block)
    // ahead of the HID stream; the PC gates its control messages on it.
    uint8_t hello[8 + 2 + 1 + SNX_VERSION_WIRE_SIZE];
    memset(hello, 0xFE, 8);
    uint16_t helloLen = 1 + SNX_VERSION_WIRE_SIZE;
    memcpy(hello + 8, &helloLen, 2);
    hello[10] = 1;
    snx__write_own_version(hello + 11, core->cfg.appVersion);
    if (!snx__write_all(fd, hello, sizeof(hello)))
    {
        close(fd);
        b->hidFd = -1;
        return;
    }

    if (core->cb.on_session != NULL)
        core->cb.on_session(core->cbCtx, &cfg);

    char detail[96];
    snprintf(detail, sizeof(detail), "%ux%u@%u fps, %u kbps video", cfg.videoWidth,
             cfg.videoHeight, cfg.videoFps, cfg.videoBitrateKbps);
    snx__set_state(core, SNX_STATE_SESSION, detail);
}

static void dropHid(SnxCore *core, SolarBackend *b)
{
    close(b->hidFd);
    b->hidFd = -1;
    b->ctlFill = 0;
    snx__set_state(core, SNX_STATE_WAIT_HOST, "host disconnected - waiting");
}

static void sendHidFrame(SnxCore *core, SolarBackend *b)
{
    SnxInputState in;
    snx__input_snapshot(core, &in);

    SnxHidPacket pkt;
    snx_input_to_hid(&in, &pkt);
    // Telemetry: free-running counters; the host reads deltas and feeds
    // videoLostFrameCount into the adaptive bitrate controller.
    pkt.videoLostFrameCount = core->statVideo.framesLost;
    pkt.videoRecoveredPacketCount = core->statVideo.shardsRecovered;
    pkt.audioConcealedFrameCount = core->statAudio.framesConcealed;
    pkt.videoMeasuredBitrateKbps = b->sessionBitrateKbps; // echo, like the Switch

    uint8_t marker[SNX_HID_MARKER_SIZE];
    memset(marker, SNX_HID_MARKER_BYTE, sizeof(marker));
    if (!snx__write_all(b->hidFd, marker, sizeof(marker)) ||
        !snx__write_all(b->hidFd, &pkt, sizeof(pkt)))
    {
        dropHid(core, b);
        return;
    }
    core->statHidFrames++;
}

// Drains the video UDP socket through the reassembly, handing frames to the
// platform. Bounded per poll so one burst can't starve the other channels.
static void pumpVideo(SnxCore *core, SolarBackend *b)
{
    uint8_t buf[SNX_VIDEO_DATAGRAM_SIZE];
    SnxVideoFrame frame;
    for (int i = 0; i < 128; i++)
    {
        int n = snx__udp_recv(b->videoFd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (snx_video_push(b->video, buf, (size_t)n, &frame) && core->cb.on_video_frame != NULL)
            core->cb.on_video_frame(core->cbCtx, frame.data, frame.size, frame.number,
                                    frame.keyframe, frame.hevc);
        // snx_video_next covers the two-frames-per-push case.
        while (snx_video_next(b->video, &frame))
        {
            if (core->cb.on_video_frame != NULL)
                core->cb.on_video_frame(core->cbCtx, frame.data, frame.size, frame.number,
                                        frame.keyframe, frame.hevc);
        }
    }
    core->statVideo = *snx_video_stats(b->video); // telemetry snapshot for snx_core_stats
}

// Drains the audio UDP socket through the sequencer, handing Opus packets (and
// their gap sizes) to the platform - the platform's decoder does the PLC.
static void pumpAudio(SnxCore *core, SolarBackend *b)
{
    uint8_t buf[SNX_AUDIO_DATAGRAM_MAX];
    SnxAudioPacket pkt;
    for (int i = 0; i < 64; i++)
    {
        int n = snx__udp_recv(b->audioFd, buf, sizeof(buf));
        if (n <= 0)
            break;
        if (snx_audio_push(b->audio, buf, (size_t)n, &pkt) && core->cb.on_audio_packet != NULL)
            core->cb.on_audio_packet(core->cbCtx, pkt.opus, pkt.len, pkt.seq, pkt.timestamp,
                                     pkt.missingBefore);
    }
    core->statAudio = *snx_audio_stats(b->audio); // telemetry snapshot
}

static void solarPoll(SnxCore *core, int timeoutMs)
{
    SolarBackend *b = (SolarBackend *)core->backendData;
    if (b == NULL)
        return;

    // Wait on whichever sockets are live, then service what became ready.
    struct pollfd pfds[2];
    int n = 0;
    int libIx = -1, hidIx = -1;
    if (b->libListenFd >= 0)
    {
        libIx = n;
        pfds[n].fd = b->libListenFd;
        pfds[n].events = POLLIN;
        n++;
    }
    bool hidConnected = b->hidFd >= 0;
    if (!hidConnected && b->hidListenFd >= 0)
    {
        hidIx = n;
        pfds[n].fd = b->hidListenFd;
        pfds[n].events = POLLIN;
        n++;
    }

    int waitMs = timeoutMs;
    if (hidConnected)
    {
        // The input cadence must survive even when nothing else happens.
        uint64_t elapsed = nowMs() - b->lastHidSendMs;
        if (elapsed >= HID_SEND_INTERVAL_MS)
            waitMs = 0;
        else
        {
            int until = HID_SEND_INTERVAL_MS - (int)elapsed;
            waitMs = timeoutMs < until ? timeoutMs : until;
        }
    }
    if (n > 0)
    {
        int ready = poll(pfds, (nfds_t)n, waitMs);
        (void)ready; // readiness is re-checked per socket below anyway
    }
    else if (waitMs > 0)
    {
        struct timespec ts = {.tv_sec = waitMs / 1000, .tv_nsec = (long)(waitMs % 1000) * 1000000};
        nanosleep(&ts, NULL);
    }

    // A platform-requested disconnect (the Switch's "hold Minus to exit"
    // gesture): the host closes the game and redials - a normal session end.
    if (core->dropSession)
    {
        core->dropSession = false;
        if (b->hidFd >= 0)
        {
            dropHid(core, b);
            return;
        }
    }

    handleLibraryConnection(core, b);

    // Video arrives whenever the PC sends: drain it every cycle, even before the
    // HID session is up (the host may start the stream first).
    if (b->videoFd >= 0)
        pumpVideo(core, b);
    if (b->audioFd >= 0)
        pumpAudio(core, b);

    if (!hidConnected && hidIx >= 0 && (pfds[hidIx].revents & POLLIN))
        acceptHid(core, b);

    if (b->hidFd >= 0)
    {
        if (!pumpControlMessages(core, b))
        {
            dropHid(core, b);
        }
        else
        {
            uint64_t now = nowMs();
            if (now - b->lastHidSendMs >= HID_SEND_INTERVAL_MS)
            {
                b->lastHidSendMs = now;
                sendHidFrame(core, b);
            }
        }
    }
}

const SnxBackendOps snx_backend_solar = {
    .name  = "solar",
    .start = solarStart,
    .stop  = solarStop,
    .poll  = solarPoll,
};
