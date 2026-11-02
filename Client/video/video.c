#include "video.h"

#include <stdio.h>
#include <stdatomic.h>

#include <switch.h>
#include <SDL.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>

#include "../network/network.h"
#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"

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

#define VIDEO_RECONNECT_DELAY_NS 1000000000ULL // 1s between reconnect attempts
#define VIDEO_HEARTBEAT_EVERY_FRAMES 300u       // ~once per 5s at 60fps

// libavformat's UDP fifo is explicitly disabled (fifo_size=0) - only the kernel socket buffer.
// Explicitly: simply omitting fifo_size does NOT disable it - its default is 7*4096 packets (~5MB),
// so the fifo thread stayed and the hang below came back (confirmed live: the hold-Minus gesture
// fired, but the main thread never got back to the menu).
// The fifo runs its own receive thread, and while it waits for data the reader blocks on that
// thread's condition variable instead of polling the socket, so the interrupt callback is never
// consulted: confirmed live, a "back to menu" request raised while video was still waiting for the
// stream left the main thread stuck there and the screen black. Without the fifo, udp_read polls in
// 100ms steps and checks the callback each time. The fifo's other job - absorbing bursts while the
// decoder was busy - is covered by the decode/render thread split (the decode thread reads
// continuously) plus the 256KB socket buffer below (~0.3s at 6Mbps). Historical note: a 100000-packet
// fifo (~37s) once just accumulated an ever-growing backlog; 300ms was the size that worked.
#define VIDEO_SOCKET_RCVBUF_BYTES 0x40000 // == network.c's cfg.udp_rx_buf_size - see its comment

// The host's configured bitrate (from its SessionConfig - see videoStart) or the stream's measured
// one; only reported back as telemetry (videoGetMeasuredBitrateKbps) and logged.
#define VIDEO_DEFAULT_BITRATE_KBPS 4000
static int s_lastKnownBitrateKbps = VIDEO_DEFAULT_BITRATE_KBPS;

// Bumped once per runStreamSession() return (stream dropped/failed to open) - see videoStart()'s
// loop. Read by input.c via videoGetReconnectCount() to stamp onto the outgoing HID packet; the
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

    s_window = SDL_CreateWindow("SolarNX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, SDL_WINDOW_SHOWN);
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

    avformat_network_init();

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

// libavformat's UDP reads block for as long as no datagram arrives - avformat_open_input()'s probe
// and av_read_frame() both, whenever the PC isn't streaming (not connected yet, between sessions,
// ffmpeg restarting). This thread is also the one that has to pump applet messages, so while it
// sat in there HOME/close requests went unanswered until the system force-killed the app. Not
// solved with the udp "timeout" option instead: that turns every brief host-side gap (the host
// restarts ffmpeg to change bitrate - see AdaptiveBitrateController.cs) into a counted video
// reconnect, which that same controller reads as link trouble and lowers the bitrate again - a
// feedback loop. The interrupt callback only aborts the read when the app is actually exiting.
#define VIDEO_INTERRUPT_POLL_NS 50000000ULL // 50ms - appMainLoop() is an IPC round-trip, callers poll this far more often
static u64 s_lastAppletPollTick = 0;

// Rate-limited appMainLoop(). From the decode thread (which also runs the interrupt callback) this
// only reads the latched exit flag - appMainLoop() itself refuses to touch the applet queue off
// the main thread, see common/appstate.
static bool pumpAppletRateLimited(void)
{
    u64 now = armGetSystemTick();
    if (armTicksToNs(now - s_lastAppletPollTick) < VIDEO_INTERRUPT_POLL_NS)
        return appIsRunning();

    s_lastAppletPollTick = now;
    return appMainLoop();
}

// Set by the main thread to make the decode thread give up now - checked by the interrupt callback,
// so it also aborts an av_read_frame() that is blocked waiting for data.
static atomic_bool s_decodeStop = false;

static int interruptCallback(void *opaque)
{
    (void)opaque;
    // appMenuRequested(): the player held Minus to go back to the menu - abort a blocked read
    // (e.g. waiting for a stream that will never come, now that the host session is ending).
    return atomic_load(&s_decodeStop) || appMenuRequested() || !pumpAppletRateLimited();
}

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
    AVFormatContext *fmtCtx;
    AVCodecContext *codecCtx;
    int streamIndex;
} DecodeJob;

static atomic_uint s_framesDecoded = 0;
static atomic_bool s_decodeEnded = false; // decode thread -> main: stream dropped, reconnect

static void decodeThreadFunc(void *arg)
{
    const DecodeJob *job = (const DecodeJob *)arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *swFrame = av_frame_alloc(); // readback target for NVTEGRA frames only

    if (packet == NULL || frame == NULL || swFrame == NULL)
    {
        logWrite("video", "decode thread: frame/packet allocation failed");
    }
    else
    {
        while (!atomic_load(&s_decodeStop) && appIsRunning())
        {
            int readResult = av_read_frame(job->fmtCtx, packet);
            if (readResult < 0)
            {
                if (!atomic_load(&s_decodeStop) && appIsRunning())
                    logWrite("video", "av_read_frame failed (%d) after %u frames decoded - reconnecting",
                        readResult, atomic_load(&s_framesDecoded));
                break;
            }

            if (packet->stream_index == job->streamIndex && avcodec_send_packet(job->codecCtx, packet) == 0)
            {
                while (avcodec_receive_frame(job->codecCtx, frame) == 0)
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

            av_packet_unref(packet);
        }
    }

    av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&packet);
    atomic_store(&s_decodeEnded, true);
}

// One connect-decode-render cycle; returns when the stream drops or the app is exiting, so the
// caller (videoStart) decides whether to reconnect.
static void runStreamSession(const char *url)
{
    atomic_store(&s_decodeStop, false); // checked by interruptCallback during the open/probe below too
    atomic_store(&s_decodeEnded, false);
    atomic_store(&s_framesDecoded, 0);

    AVFormatContext *fmtCtx = avformat_alloc_context();
    if (fmtCtx == NULL)
    {
        logWrite("video", "avformat_alloc_context failed");
        return;
    }
    fmtCtx->interrupt_callback.callback = interruptCallback;
    fmtCtx->interrupt_callback.opaque = NULL;

    // On failure avformat_open_input() frees the caller-allocated context itself.
    if (avformat_open_input(&fmtCtx, url, NULL, NULL) < 0)
    {
        logWrite("video", "avformat_open_input failed for %s", url);
        return;
    }

    // This stream carries H264 video only (audio is a separate UDP port with its own framing) and
    // the encoder sends a keyframe at least every ~0.5s (VideoEncoder.cs's -g setting) - keep
    // analysis short, there's only ever one stream to find.
    fmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    fmtCtx->probesize = 65536;
    fmtCtx->max_analyze_duration = 500000; // microseconds

    if (avformat_find_stream_info(fmtCtx, NULL) < 0)
    {
        logWrite("video", "avformat_find_stream_info failed");
        avformat_close_input(&fmtCtx);
        return;
    }

    int streamIndex = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (streamIndex < 0)
    {
        logWrite("video", "no video stream found in input");
        avformat_close_input(&fmtCtx);
        return;
    }

    AVCodecParameters *codecPar = fmtCtx->streams[streamIndex]->codecpar;

    // Records the stream's own bit_rate if avformat managed to estimate one during the probe (it
    // doesn't always) - telemetry/log only.
    if (codecPar->bit_rate > 0)
    {
        int measuredKbps = (int)(codecPar->bit_rate / 1000);
        if (measuredKbps != s_lastKnownBitrateKbps)
        {
            logWrite("video", "measured stream bitrate %d kbps (host configured %d kbps)",
                measuredKbps, s_lastKnownBitrateKbps);
            s_lastKnownBitrateKbps = measuredKbps;
        }
    }
    const AVCodec *decoder = avcodec_find_decoder(codecPar->codec_id);
    if (decoder == NULL)
    {
        logWrite("video", "no decoder for codec id %d", (int)codecPar->codec_id);
        avformat_close_input(&fmtCtx);
        return;
    }

    AVCodecContext *codecCtx = avcodec_alloc_context3(decoder);
    if (codecCtx == NULL || avcodec_parameters_to_context(codecCtx, codecPar) < 0)
    {
        logWrite("video", "decoder context setup failed");
        if (codecCtx != NULL)
            avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    if (s_hwDeviceCtx != NULL)
    {
        codecCtx->hw_device_ctx = av_buffer_ref(s_hwDeviceCtx);
        codecCtx->get_format = negotiatePixelFormat;
    }

    if (avcodec_open2(codecCtx, decoder, NULL) < 0)
    {
        logWrite("video", "avcodec_open2 failed");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }

    logWrite("video", "stream connected - decoding with %s (%s)", decoder->name,
        s_hwDeviceCtx != NULL ? "NVTEGRA hwaccel" : "software");

    DecodeJob job = { fmtCtx, codecCtx, streamIndex };
    Thread decodeThread;
    if (R_FAILED(threadCreate(&decodeThread, decodeThreadFunc, &job, NULL,
            VIDEO_DECODE_THREAD_STACK_SIZE, VIDEO_DECODE_THREAD_PRIORITY, VIDEO_DECODE_THREAD_CORE)))
    {
        logWrite("video", "failed to create decode thread");
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return;
    }
    if (R_FAILED(threadStart(&decodeThread)))
    {
        logWrite("video", "failed to start decode thread");
        threadClose(&decodeThread);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
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
            logWrite("video", "connection stable - %u frames shown, %u decoded", framesShown, atomic_load(&s_framesDecoded));
        av_frame_unref(displayFrame);
    }

    // Makes the interrupt callback abort whatever av_read_frame() the decode thread is blocked in,
    // so this join can't hang on a stream that simply went quiet.
    atomic_store(&s_decodeStop, true);
    threadWaitForExit(&decodeThread);
    threadClose(&decodeThread);

    mutexLock(&s_frameMutex);
    av_frame_unref(s_pendingFrame);
    s_pendingValid = false;
    mutexUnlock(&s_frameMutex);
    av_frame_free(&displayFrame);

    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);
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
        // BitrateKbps is known - see s_lastKnownBitrateKbps's doc comment. A false return only
        // happens if the app is exiting before any session ever connected, in which case the
        // loop condition below will also be false and this iteration's URL is never used.
        SolarNxSessionConfig sessionConfig;
        if (networkWaitForSessionConfig(&sessionConfig) && sessionConfig.videoBitrateKbps > 0)
            s_lastKnownBitrateKbps = (int)sessionConfig.videoBitrateKbps;

        char url[160];
        // buffer_size: SO_RCVBUF for the socket - must match network.c's udp_rx_buf_size, or
        // libavformat applies its own smaller default and the bigger BSD buffer goes unused.
        // fifo_size=0 turns libavformat's fifo thread off - see the comment at VIDEO_SOCKET_RCVBUF_BYTES.
        snprintf(url, sizeof(url), "udp://0.0.0.0:%d?fifo_size=0&buffer_size=%d", SOLARNX_VIDEO_PORT, VIDEO_SOCKET_RCVBUF_BYTES);

        logWrite("video", "waiting for video stream on port %d (host bitrate %d kbps)...",
            SOLARNX_VIDEO_PORT, s_lastKnownBitrateKbps);
        runStreamSession(url);
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
    avformat_network_deinit();
    SDL_Quit();
}
