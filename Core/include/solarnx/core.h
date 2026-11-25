// libsolarnx-core public API: a single-threaded, poll-driven session core shared by
// every SolarNX client. The platform owns the thread, the decoders, the UI and the
// OS integration; the core owns the protocol.
//
// Usage: create with a host address (changeable at runtime - no restart, no manual
// channel pick: the path is detected from the route, see route.h), then pump
// snx_core_poll() from your own thread. Everything comes back through callbacks.
//
// This file is part of SolarNX (GPL-3.0).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "input.h"
#include "route.h"
#include "wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNX_BACKEND_SOLAR = 0,    // the SolarNX PC service (built-in protocol)
    SNX_BACKEND_SUNSHINE,    // Sunshine host via the Moonlight protocol (moonlight-common-c)
} SnxBackend;

typedef enum {
    SNX_STATE_IDLE = 0,        // created / no host address configured
    SNX_STATE_RESOLVING,       // detecting the route to the host
    SNX_STATE_WAIT_HOST,       // LAN: listening, waiting for the PC to dial in
    SNX_STATE_LIBRARY,         // a library arrived (the PC is attached)
    SNX_STATE_SESSION,         // full streaming session established
    SNX_STATE_PATH_UNSUPPORTED,// internet path: the encrypted transport is not wired yet
    SNX_STATE_BACKEND_UNSUPPORTED, // backend selected but not implemented yet
    SNX_STATE_ERROR,           // resolve failed / socket error - will retry
} SnxState;

const char *snx_state_name(SnxState state);

// One game record from the host's library. Valid only inside the on_library callback
// (the core reuses its storage on the next update).
typedef struct {
    uint32_t    id;
    const char *name;        // UTF-8
    const uint8_t *cover;   // JPEG bytes, NULL if none
    size_t      coverSize;
} SnxLibraryGame;

typedef struct {
    // State transitions, with a human-readable detail for the UI (may be NULL).
    void (*on_state)(void *ctx, SnxState state, const char *detail);
    // The host's library, arriving on the library channel. `games` is valid only
    // during the call - copy what you need.
    void (*on_library)(void *ctx, const SnxLibraryGame *games, size_t count);
    // The PC connected on the HID channel and sent its session config: the encoder
    // parameters the streams will run at. NULL `config` never happens.
    void (*on_session)(void *ctx, const SnxSessionConfig *config);
    // A control message from the PC (type + payload), e.g. rumble and debug
    // requests. Payload is valid only during the call.
    void (*on_control)(void *ctx, uint8_t type, const void *payload, size_t len);
    // A reassembled video frame: Annex B bytes (H.264 or H.265 - see `hevc`),
    // zero-padded past the end for decoder overreads. `data` is valid only
    // during the call (the reassembly reuses its buffers).
    void (*on_video_frame)(void *ctx, const uint8_t *data, size_t size, uint32_t number,
                           bool keyframe, bool hevc);
    // One Opus frame from the audio channel, in order, with `missingBefore`
    // frames lost since the previous one: conceal them first (the platform owns
    // the Opus decoder - opus_decode(NULL) does the PLC). `data` is valid only
    // during the call.
    void (*on_audio_packet)(void *ctx, const uint8_t *data, size_t len, uint16_t seq,
                            uint32_t timestamp, uint16_t missingBefore);
    // Diagnostic log, one line, no trailing newline.
    void (*on_log)(void *ctx, int level, const char *msg);
} SnxCallbacks;

typedef struct {
    int         backend;       // SnxBackend; SolarNX service by default
    const char *host;          // PC address (IP or name); NULL or "" keeps the core idle
    const char *appVersion;    // announced in handshakes; NULL = "solarnx-core"
    // Ports; 0 falls back to the wire.h defaults.
    int libraryPort, hidPort, videoPort, audioPort;
} SnxConfig;

typedef struct SnxCore SnxCore;

SnxCore *snx_core_create(const SnxConfig *cfg, const SnxCallbacks *cb, void *ctx);
void     snx_core_destroy(SnxCore *core);

// Pumps every channel once: accepts the PC's connections, parses the library,
// flushes queued input. Never blocks longer than timeout_ms. Thread of the
// platform's choosing; callbacks fire from inside this call.
void snx_core_poll(SnxCore *core, int timeout_ms);

// Runtime host change (settings screen): the route is re-detected and the session
// restarts on the next poll - no app restart, no manual channel pick. Same thread
// as snx_core_poll (or while poll is not running): the core is deliberately
// single-owner; other threads should stop/recreate the core instead.
void snx_core_set_host(SnxCore *core, const char *host);

// Queues the input state; the active backend serializes and ships it on its own
// cadence (SolarNX HID at ~120 Hz, Moonlight input for Sunshine). Thread-safe:
// call from the platform's input/UI thread while the core polls on its own.
void snx_core_send_input(SnxCore *core, const SnxInputState *input);

// Drops the live session: the backend closes the HID connection on its next
// poll. The host sees a normal disconnect, tears the running game down, and
// dials back in - the "hold Minus to exit" path of the Switch client.
void snx_core_disconnect(SnxCore *core);

SnxState snx_core_state(const SnxCore *core);
SnxBackend snx_core_backend(const SnxCore *core);
SnxPath snx_core_path(const SnxCore *core);

// Live telemetry for the debug overlay: everything the core knows about the
// session right now. Safe to poll from another thread (plain reads).
typedef struct {
    SnxState state;
    SnxPath path;
    SnxBackend backend;
    const char *host;        // the configured address (owned by the core)
    uint32_t libraryGames;   // entries in the last received library
    uint32_t hidFramesSent;  // ~120 Hz while the HID channel is up
    uint32_t videoFrames;    // reassembled frames handed out
    uint32_t videoFramesLost;
    uint32_t videoFramesSkipped; // dropped while waiting for a keyframe
    uint32_t videoShardsRecovered; // rebuilt from Reed-Solomon parity
    uint32_t videoDatagramsRejected;
    uint32_t audioPackets;    // Opus frames delivered in order
    uint32_t audioConcealed; // gap frames the decoder was asked to synthesize (PLC)
    uint32_t audioLateDropped;
} SnxStats;

SnxStats snx_core_stats(const SnxCore *core);

#ifdef __cplusplus
}
#endif
