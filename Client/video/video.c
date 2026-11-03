#include "video.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <switch.h>
#include <SDL.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>

#include "../network/network.h"
#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"
#include "reassembly.h"
#include "videowire.h"

// Hardware H264 decode via NVTEGRA (the Tegra X1's NVDEC engine), through libavcodec's standard
// hwaccel API - devkitPro's ffmpeg portlib is built with --enable-nvtegra (confirmed directly in
// the installed libavutil.a: av_nvtegra_channel_open, ff_hwcontext_type_nvtegra, etc. are present
// as real exported symbols), so this needs no custom FFmpeg build.
//
// Decoded NVTEGRA frames live in Tegra-local hardware memory in NV12 layout and can't be handed
// to SDL2 directly - av_hwframe_transfer_data() reads them back into a normal system-memory NV12
// AVFrame first, which SDL2 renders via SDL_UpdateNVTexture(). This isn't zero-copy (a true
// zero-copy path would render the hardware surface directly via deko3d instead of SDL2, skipping
// this readback) - that's a possible later optimization once this path is proven; the readback
// still moves the actual decode work off the CPU, which is the expensive part.
//
// hw_device_ctx creation is attempted once in videoInit() and reused across reconnects (opening
// an NVDEC hardware channel isn't something to redo on every network hiccup). If it fails for any
// reason, get_format() below just never gets offered AV_PIX_FMT_NVTEGRA and libavcodec falls back
// to its normal software H264 decoder. The encoder sends High profile without B-frames (see
// SolarNX/Capture/VideoEncoder.cs) - CABAC makes that fallback somewhat more CPU-hungry than the
// baseline profile used previously, but the NVTEGRA path is the one that normally runs.

// The stream: SolarNX's own framing (videowire.h) - each H.265 or H.264 frame cut into datagrams with
// Reed-Solomon parity, rebuilt by reassembly.c and handed to libavcodec as one packet. No
// libavformat: there is no container to probe, so the first keyframe decodes the moment it lands.
// In Internet mode transport.c unseals the same datagrams and delivers them to this port on loopback.

#define VIDEO_RECONNECT_DELAY_NS 1000000000ULL // 1s between attempts after a failed setup
#define VIDEO_HEARTBEAT_EVERY_FRAMES 300u       // ~once per 5s at 60fps

// The decode thread reads continuously (see the decode/render split below), so the socket buffer
// only has to absorb a frame's worth of datagrams while one frame decodes - 256KB is several
// keyframes' worth at the PC's pacing.
#define VIDEO_SOCKET_RCVBUF_BYTES 0x40000 // == network.c's cfg.udp_rx_buf_size - see its comment
// recv() timeout - how often the decode thread looks at the stop flag while no video arrives.
#define VIDEO_RECV_TIMEOUT_US 50000
#define VIDEO_BITRATE_WINDOW_NS 2000000000ULL

// The host's configured bitrate (from its SessionConfig - see videoStart) until frames arrive, then
// the measured one; only reported back as telemetry (videoGetMeasuredBitrateKbps) and logged.
#define VIDEO_DEFAULT_BITRATE_KBPS 4000
static int s_lastKnownBitrateKbps = VIDEO_DEFAULT_BITRATE_KBPS;

// Bumped once per runStreamSession() that ended without being asked to (setup failed, socket
// error) - see videoStart()'s loop. Read by input.c via videoGetReconnectCount() to stamp onto the outgoing HID packet; the
// host's AdaptiveBitrateController (SolarNX/Session/AdaptiveBitrateController.cs) diffs this
// against its own last-seen value to detect video instability. Plain word, no locking - see
// network.h's telemetry field doc comment.
static uint32_t s_videoReconnectCount = 0;

uint32_t videoGetReconnectCount(void)
{
    return (uint32_t)s_videoReconnectCount;
}

uint32_t videoGetMeasuredBitrateKbps(void)
{
    return (uint32_t)s_lastKnownBitrateKbps;
}

uint32_t videoGetLostFrameCount(void)
{
    return reassemblyStats()->framesLost;
}

uint32_t videoGetRecoveredPacketCount(void)
{
    return reassemblyStats()->shardsRecovered;
}

static SDL_Window *s_window = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture *s_texture = NULL;
static int s_textureWidth = 0;
static int s_textureHeight = 0;
static Uint32 s_textureFormat = 0;

static AVBufferRef *s_hwDeviceCtx = NULL;

// Decode-thread -> render-thread handoff of the newest finished frame - see decodeThreadFunc.
static Mutex s_frameMutex;             // guards s_pendingFrame/s_pendingValid
static AVFrame *s_pendingFrame = NULL; // newest decoded system-memory frame not yet shown
static bool s_pendingValid = false;

bool videoInit(void)
{
    // See logger.h's doc comment on logInstallFfmpegCallback - unifies FFmpeg's own av_log output
    // with logWrite()'s mutex instead of racing it. Also called from audio.c's audioInit(); safe
    // to call from both, idempotent (av_log_set_callback just overwrites the same pointer).
    logInstallFfmpegCallback();

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        logWrite("video", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    // The screen's own resolution - 1920x1080 docked, 1280x720 handheld - so a 1080p stream (the
    // host's automatic resolution when docked) isn't squeezed into 720p first. RESIZABLE: SDL's Switch
    // backend then resizes the window itself when the console goes in or out of the dock (it does
    // only for resizable windows); renderFrame stretches the picture over whatever size it is.
    bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    s_window = SDL_CreateWindow("SolarNX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        docked ? 1920 : 1280, docked ? 1080 : 720, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (s_window == NULL)
    {
        logWrite("video", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (s_renderer == NULL)
    {
        logWrite("video", "SDL_CreateRenderer failed: %s", SDL_GetError());
        return false;
    }

    if (!reassemblyInit())
    {
        logWrite("video", "reassembly buffers could not be allocated");
        return false;
    }

    mutexInit(&s_frameMutex);
    s_pendingFrame = av_frame_alloc();
    if (s_pendingFrame == NULL)
    {
        logWrite("video", "av_frame_alloc for the decode->render handoff failed");
        return false;
    }

    int hwResult = av_hwdevice_ctx_create(&s_hwDeviceCtx, AV_HWDEVICE_TYPE_NVTEGRA, NULL, NULL, 0);
    if (hwResult < 0)
    {
        logWrite("video", "NVTEGRA hwaccel unavailable (%d) - falling back to software decode", hwResult);
        s_hwDeviceCtx = NULL;
    }
    else
    {
        logWrite("video", "NVTEGRA hwaccel ready");
    }

    logWrite("video", "SDL2 video ready");
    return true;
}

// (Re)creates the streaming texture only when the decoded frame size or pixel format actually
// changes, instead of trusting a hardcoded resolution/format - the host's Config.cs
// TargetWidth/Height is never actually transmitted on the wire, and whether NVTEGRA hwaccel is
// active can in principle differ session to session, so the decoded AVFrame is the only source of
// truth for both.
static bool ensureTexture(int width, int height, Uint32 sdlFormat)
{
    if (s_texture != NULL && width == s_textureWidth && height == s_textureHeight && sdlFormat == s_textureFormat)
        return true;

    if (s_texture != NULL)
    {
        SDL_DestroyTexture(s_texture);
        s_texture = NULL;
    }

    s_texture = SDL_CreateTexture(s_renderer, sdlFormat, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (s_texture == NULL)
    {
        logWrite("video", "SDL_CreateTexture failed at %dx%d (format %u): %s", width, height, sdlFormat, SDL_GetError());
        return false;
    }

    s_textureWidth = width;
    s_textureHeight = height;
    s_textureFormat = sdlFormat;
    logWrite("video", "texture (re)created at %dx%d, format %u", width, height, sdlFormat);
    return true;
}

// Renders one decoded software-memory frame (already read back from hardware if it came from
// NVTEGRA - see decodeThreadFunc). Handles the two layouts this app can actually produce: NV12
// from the hwaccel readback, or plain YUV420P from the software decoder fallback. Main thread only
// (SDL). Returns whether the frame was actually presented.
static bool renderFrame(const AVFrame *swFrame)
{
    if (swFrame->format == AV_PIX_FMT_NV12 && ensureTexture(swFrame->width, swFrame->height, SDL_PIXELFORMAT_NV12))
    {
        SDL_UpdateNVTexture(s_texture, NULL,
            swFrame->data[0], swFrame->linesize[0],
            swFrame->data[1], swFrame->linesize[1]);
    }
    else if (swFrame->format == AV_PIX_FMT_YUV420P && ensureTexture(swFrame->width, swFrame->height, SDL_PIXELFORMAT_IYUV))
    {
        SDL_UpdateYUVTexture(s_texture, NULL,
            swFrame->data[0], swFrame->linesize[0],
            swFrame->data[1], swFrame->linesize[1],
            swFrame->data[2], swFrame->linesize[2]);
    }
    else
    {
        logWrite("video", "unexpected pixel format %d (expected NV12 or YUV420P) - dropping frame", swFrame->format);
        return false;
    }

    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer); // blocks until vblank (PRESENTVSYNC) - fine here, see decodeThreadFunc
    return true;
}

static enum AVPixelFormat negotiatePixelFormat(AVCodecContext *ctx, const enum AVPixelFormat *pixFmts)
{
    (void)ctx;
    for (const enum AVPixelFormat *p = pixFmts; *p != AV_PIX_FMT_NONE; p++)
    {
        if (*p == AV_PIX_FMT_NVTEGRA)
            return *p;
    }
    return pixFmts[0]; // no NVTEGRA offered (hwaccel unavailable) - take the software format
}

// The main loop pumps applet messages (HOME, close requests) while it waits for frames - at most
// every 50ms, since appMainLoop() is an IPC round-trip.
#define VIDEO_INTERRUPT_POLL_NS 50000000ULL
static u64 s_lastAppletPollTick = 0;

// Rate-limited appMainLoop() - main thread only (appMainLoop() refuses to touch the applet queue
// off it, see common/appstate).
static bool pumpAppletRateLimited(void)
{
    u64 now = armGetSystemTick();
    if (armTicksToNs(now - s_lastAppletPollTick) < VIDEO_INTERRUPT_POLL_NS)
        return appIsRunning();

    s_lastAppletPollTick = now;
    return appMainLoop();
}

// Set by the main thread to make the decode thread give up - checked between recv() timeouts.
static atomic_bool s_decodeStop = false;

// Decode thread vs. render (main) thread split.
//
// Confirmed live (nxlink log, 13:37): in Big Picture the client decoded and showed a steady 60fps
// with zero UDP overruns, but as soon as a game was running it fell to ~43fps and libavformat
// logged ~100 "Circular buffer overrun" per second - video datagrams dropped on the Switch itself,
// which is the "water running down glass, then clean again" artifact. One thread used to do it
// all in sequence - read, decode, NVTEGRA readback, texture upload and SDL_RenderPresent with
// VSYNC. Present blocks until the next vblank, so whenever a (heavier, in-game) frame's work
// didn't fit in one 16.7ms refresh it waited for the one after, the effective rate dropped
// towards 30fps, and nothing read the socket in the meantime. Enlarging the receive buffer could
// only delay that overflow: the deficit was continuous, not a burst.
//
// Now the decode thread never waits on the display: it reads/decodes/reads back continuously and
// keeps only the newest finished frame in s_pendingFrame; the main thread presents whatever is
// newest at each vsync and frames it never got to are simply superseded (every frame is still
// decoded - H.264 needs them as references - just not every frame is shown).
#define VIDEO_DECODE_THREAD_STACK_SIZE 0x100000
// One step below the main thread's 0x2C, on the same core (MAIN_THREAD_CORE in main.c): the main
// thread spends nearly all its time blocked in Present/sleep, and when a vblank arrives it should
// preempt the decode thread's CPU-side readback rather than queue behind it.
#define VIDEO_DECODE_THREAD_PRIORITY   0x2D
#define VIDEO_DECODE_THREAD_CORE       1
#define VIDEO_RENDER_IDLE_SLEEP_NS     1000000ULL // 1ms - main thread poll while no new frame is ready

typedef struct
{
    int socket;
} DecodeJob;

static atomic_uint s_framesDecoded = 0;
static atomic_bool s_decodeEnded = false; // decode thread -> main: socket failed, set up again

// Decodes one reassembled frame and hands the newest picture to the render thread.
static void decodeFrame(AVCodecContext *codecCtx, AVPacket *packet, AVFrame *frame, AVFrame *swFrame,
    const ReassembledFrame *assembled)
{
    packet->data = (uint8_t *)assembled->data; // not reference-counted: libavcodec copies it
    packet->size = (int)assembled->size;
    packet->flags = assembled->keyframe ? AV_PKT_FLAG_KEY : 0;
    if (avcodec_send_packet(codecCtx, packet) < 0)
    {
        // Damaged beyond what the decoder accepts - whatever it would make of the next frames
        // until a keyframe would be garbage.
        reassemblyRequestKeyframe();
        return;
    }

    while (avcodec_receive_frame(codecCtx, frame) == 0)
    {
        AVFrame *displayable = frame;
        if (frame->format == AV_PIX_FMT_NVTEGRA)
        {
            // Deliberately NOT pre-allocating swFrame with 256-byte alignment (tried live:
            // it does make av_hwframe_transfer_data take a faster path - the "Frame
            // address/pitch not aligned to 256, falling back to cpu transfer" log line
            // disappeared entirely - but that faster path was confirmed live to bring real
            // instability with it: video glitches, then total loss of HID input on a
            // completely separate, dedicated core, which points at broader system
            // instability rather than a video-only rendering glitch. This experimental
            // hwaccel's internals aren't documented or debuggable from here (mailing-list
            // patches, no source access to the actual transfer implementation) - reverted
            // to the slower but proven-stable CPU-copy fallback rather than chase
            // instability inside code neither of us can step through. The auto-allocated
            // (unaligned) buffer below is what reliably takes that slow-but-stable path.
            // Kept on this thread (the one that decoded the frame), not moved to the
            // render thread, for the same reason: don't find out the hard way whether
            // NVTEGRA's transfer tolerates a different thread.
            if (av_hwframe_transfer_data(swFrame, frame, 0) < 0)
            {
                logWrite("video", "av_hwframe_transfer_data failed - dropping frame");
                av_frame_unref(frame);
                continue;
            }
            displayable = swFrame;
        }

        atomic_fetch_add(&s_framesDecoded, 1);

        mutexLock(&s_frameMutex);
        av_frame_unref(s_pendingFrame); // an older frame the display never got to is superseded
        av_frame_move_ref(s_pendingFrame, displayable);
        s_pendingValid = true;
        mutexUnlock(&s_frameMutex);

        av_frame_unref(frame);
        av_frame_unref(swFrame);
    }
}

static AVCodecContext *openDecoder(enum AVCodecID codecId);

static void decodeThreadFunc(void *arg)
{
    const DecodeJob *job = (const DecodeJob *)arg;
    // Opened for the codec of the first frame, and again whenever it changes (the PC switched
    // between H.265 and H.264 - a restart with another encoder): the flag in every datagram says.
    AVCodecContext *codecCtx = NULL;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *swFrame = av_frame_alloc(); // readback target for NVTEGRA frames only
    // Room for more than a valid datagram, so an oversized one shows up as the wrong length.
    static uint8_t datagram[VIDEO_WIRE_DATAGRAM_SIZE + 64];

    u64 windowStart = armGetSystemTick();
    u64 windowBytes = 0;

    if (packet == NULL || frame == NULL || swFrame == NULL)
    {
        logWrite("video", "decode thread: frame/packet allocation failed");
    }
    else
    {
        while (!atomic_load(&s_decodeStop) && appIsRunning())
        {
            ssize_t length = recv(job->socket, datagram, sizeof(datagram), 0);
            if (length < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue; // the timeout - just a chance to look at the stop flag
                logWrite("video", "recv failed (errno %d) after %u frames decoded", errno, atomic_load(&s_framesDecoded));
                break;
            }

            ReassembledFrame assembled;
            if (!reassemblyPush(datagram, (size_t)length, &assembled))
                continue;

            windowBytes += assembled.size;
            u64 now = armGetSystemTick();
            u64 elapsedNs = armTicksToNs(now - windowStart);
            if (elapsedNs >= VIDEO_BITRATE_WINDOW_NS)
            {
                s_lastKnownBitrateKbps = (int)(windowBytes * 8ULL * 1000000ULL / elapsedNs);
                windowStart = now;
                windowBytes = 0;
            }

            enum AVCodecID codecId = assembled.hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
            if (codecCtx == NULL || codecCtx->codec_id != codecId)
            {
                avcodec_free_context(&codecCtx);
                codecCtx = openDecoder(codecId);
                if (codecCtx == NULL)
                    break; // nothing to decode with - runStreamSession sets up again
            }

            decodeFrame(codecCtx, packet, frame, swFrame, &assembled);
        }
    }

    avcodec_free_context(&codecCtx);
    av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    atomic_store(&s_decodeEnded, true);
}

static int openVideoSocket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        logWrite("video", "socket failed (errno %d)", errno);
        return -1;
    }

    int rcvbuf = VIDEO_SOCKET_RCVBUF_BYTES;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct timeval timeout = { .tv_sec = 0, .tv_usec = VIDEO_RECV_TIMEOUT_US };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(SOLARNX_VIDEO_PORT);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0)
    {
        logWrite("video", "bind to port %d failed (errno %d)", SOLARNX_VIDEO_PORT, errno);
        close(sock);
        return -1;
    }
    return sock;
}

static AVCodecContext *openDecoder(enum AVCodecID codecId)
{
    const AVCodec *decoder = avcodec_find_decoder(codecId);
    if (decoder == NULL)
    {
        logWrite("video", "no decoder for codec id %d", (int)codecId);
        return NULL;
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(decoder);
    if (codecCtx == NULL)
    {
        logWrite("video", "decoder context allocation failed");
        return NULL;
    }
    // Every frame goes out as soon as it is decoded - the stream has no B-frames to reorder.
    // Decoded on NVTEGRA either way (H.264 High and H.265 Main, both 8-bit NV12).
    codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (s_hwDeviceCtx != NULL)
    {
        codecCtx->hw_device_ctx = av_buffer_ref(s_hwDeviceCtx);
        codecCtx->get_format = negotiatePixelFormat;
    }

    if (avcodec_open2(codecCtx, decoder, NULL) < 0)
    {
        logWrite("video", "avcodec_open2 failed");
        avcodec_free_context(&codecCtx);
        return NULL;
    }

    logWrite("video", "decoder ready - %s (%s)", decoder->name, s_hwDeviceCtx != NULL ? "NVTEGRA hwaccel" : "software");
    return codecCtx;
}

// One receive-decode-render cycle; returns when the player goes back to the menu, the app is
// exiting, or the socket failed - the caller (videoStart) decides whether to set up again.
static void runStreamSession(void)
{
    atomic_store(&s_decodeStop, false);
    atomic_store(&s_decodeEnded, false);
    atomic_store(&s_framesDecoded, 0);

    int sock = openVideoSocket();
    if (sock < 0)
        return;

    reassemblyReset();
    DecodeJob job = { sock };
    Thread decodeThread;
    if (R_FAILED(threadCreate(&decodeThread, decodeThreadFunc, &job, NULL,
            VIDEO_DECODE_THREAD_STACK_SIZE, VIDEO_DECODE_THREAD_PRIORITY, VIDEO_DECODE_THREAD_CORE)))
    {
        logWrite("video", "failed to create decode thread");
        close(sock);
        return;
    }
    if (R_FAILED(threadStart(&decodeThread)))
    {
        logWrite("video", "failed to start decode thread");
        threadClose(&decodeThread);
        close(sock);
        return;
    }

    AVFrame *displayFrame = av_frame_alloc();
    u32 framesShown = 0;

    while (displayFrame != NULL && !atomic_load(&s_decodeEnded) && !appMenuRequested() && pumpAppletRateLimited())
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) { /* no input handling yet - see input.c */ }

        bool haveFrame = false;
        mutexLock(&s_frameMutex);
        if (s_pendingValid)
        {
            av_frame_move_ref(displayFrame, s_pendingFrame);
            s_pendingValid = false;
            haveFrame = true;
        }
        mutexUnlock(&s_frameMutex);

        if (!haveFrame)
        {
            svcSleepThread(VIDEO_RENDER_IDLE_SLEEP_NS);
            continue;
        }

        if (renderFrame(displayFrame) && ++framesShown % VIDEO_HEARTBEAT_EVERY_FRAMES == 0)
        {
            const ReassemblyStats *stats = reassemblyStats();
            logWrite("video", "%u frames shown, %u decoded, %d kbps; since start: %u packets rebuilt from parity, %u frames lost, %u skipped waiting for a keyframe",
                framesShown, atomic_load(&s_framesDecoded), s_lastKnownBitrateKbps,
                stats->shardsRecovered, stats->framesLost, stats->framesSkipped);
        }
        av_frame_unref(displayFrame);
    }

    // The decode thread notices within one recv() timeout.
    atomic_store(&s_decodeStop, true);
    threadWaitForExit(&decodeThread);
    threadClose(&decodeThread);
    close(sock);

    mutexLock(&s_frameMutex);
    av_frame_unref(s_pendingFrame);
    s_pendingValid = false;
    mutexUnlock(&s_frameMutex);
    av_frame_free(&displayFrame);
}

bool videoStart(void)
{
    if (s_renderer == NULL)
    {
        logWrite("video", "videoStart() called before videoInit() succeeded");
        return false;
    }

    // Also returns (true) when the player asked to go back to the menu - main.c takes it from there.
    while (!appMenuRequested() && appMainLoop())
    {
        // Blocks (briefly, only before the very first session ever) until the host's real
        // BitrateKbps is known - see s_lastKnownBitrateKbps's doc comment.
        SolarNxSessionConfig sessionConfig;
        if (networkWaitForSessionConfig(&sessionConfig) && sessionConfig.videoBitrateKbps > 0)
            s_lastKnownBitrateKbps = (int)sessionConfig.videoBitrateKbps;

        logWrite("video", "waiting for video on port %d (host bitrate %d kbps)...", SOLARNX_VIDEO_PORT, s_lastKnownBitrateKbps);
        runStreamSession();
        if (!appIsRunning() || appMenuRequested())
            break; // interrupted on purpose (exit / back to menu) - not a reconnect, and no point waiting 1s first
        s_videoReconnectCount++;
        svcSleepThread(VIDEO_RECONNECT_DELAY_NS);
    }

    return true;
}

void videoExit(void)
{
    if (s_texture != NULL) { SDL_DestroyTexture(s_texture); s_texture = NULL; }
    if (s_renderer != NULL) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (s_window != NULL) { SDL_DestroyWindow(s_window); s_window = NULL; }
    if (s_hwDeviceCtx != NULL) { av_buffer_unref(&s_hwDeviceCtx); }
    av_frame_free(&s_pendingFrame);
    reassemblyExit();
    SDL_Quit();
}
