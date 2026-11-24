// Internal glue shared by the core sources - not installed, not public.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "solarnx/core.h"
#include "solarnx/input.h"
#include "solarnx/route.h"
#include "solarnx/wire.h"

// video.c - the frame reassembly on top of the wire framing (see wire.h).
typedef struct SnxVideo SnxVideo;

typedef struct {
    const uint8_t *data; // Annex B + SNX_VIDEO padding zeros; valid until the next push
    size_t size;
    uint32_t number;
    bool keyframe;
    bool hevc;   // H.265, else H.264
    uint64_t firstMs; // when its first datagram arrived (the debug overlay's latency)
} SnxVideoFrame;

typedef struct {
    uint32_t framesHanded;      // complete frames handed to the decoder
    uint32_t framesLost;        // could not be rebuilt, or never arrived at all
    uint32_t framesSkipped;     // complete, but dropped while waiting for a keyframe
    uint32_t shardsRecovered;   // data shards rebuilt from parity
    uint32_t datagramsRejected; // not our format, or inconsistent
    uint32_t datagramsLate;     // for a frame already given up on
} SnxVideoStats;

SnxVideo *snx_video_create(void);
void snx_video_destroy(SnxVideo *v);
void snx_video_reset(SnxVideo *v, bool reorderWindow); // true: two-frame window (Internet)
void snx_video_request_keyframe(SnxVideo *v);          // decoder rejected a frame
bool snx_video_push(SnxVideo *v, const uint8_t *datagram, size_t length, SnxVideoFrame *out);
bool snx_video_next(SnxVideo *v, SnxVideoFrame *out);
const SnxVideoStats *snx_video_stats(const SnxVideo *v);

// audio.c - the RTP-ish unit sequencing on top of the wire framing (see wire.h).
typedef struct SnxAudio SnxAudio;

typedef struct {
    const uint8_t *opus; // raw Opus frame; valid only during the callback
    size_t len;
    uint16_t seq;
    uint32_t timestamp;
    uint16_t missingBefore; // frames missing since the previous packet: conceal them
} SnxAudioPacket;

typedef struct {
    uint32_t packetsDelivered;
    uint32_t framesConcealed; // gaps the decoder is asked to fill (PLC)
    uint32_t lateDropped;     // arrived behind the last delivered one
    uint32_t duplicates;
    uint32_t resyncs;         // gaps too large to conceal - decoder should flush
    uint32_t datagramsRejected;
} SnxAudioStats;

SnxAudio *snx_audio_create(void);
void snx_audio_destroy(SnxAudio *a);
void snx_audio_reset(SnxAudio *a);
bool snx_audio_push(SnxAudio *a, const uint8_t *datagram, size_t len, SnxAudioPacket *out);
const SnxAudioStats *snx_audio_stats(const SnxAudio *a);

struct SnxCore {
    SnxConfig   cfg;
    SnxCallbacks cb;
    void        *cbCtx;
    SnxBackend  backend;
    SnxState    state;
    SnxPath      path;
    char        *host;        // current host address (owned)
    bool         backendUp;   // backend started
    bool        *stopFlag;    // NULL for the sync core (threaded wrapper later)
    // Backend instance data.
    void        *backendData;
    const struct SnxBackendOps *ops;
    // SolarNX LAN library channel.
    int          libListenFd;
    // Last library hash (the "already have it" short-circuit).
    uint64_t     libHash;
    bool         libHashValid;
    // Input pending shipment. Guarded by inputMutex: the platform pushes from its
    // own thread, the core thread snapshots it on its send cadence.
    SnxInputState input;
    pthread_mutex_t inputMutex;
    // Telemetry (plain reads from other threads are fine - the debug overlay polls).
    uint32_t statLibGames;
    uint32_t statHidFrames;
    SnxVideoStats statVideo;
    SnxAudioStats statAudio;
    // Resolve retry pacing.
    uint64_t     lastAttemptMs;
};

// --- backend vtable (one per host protocol) ---
typedef struct SnxBackendOps {
    const char *name;
    // Bring the backend up for the detected path. Returns false on a fatal error.
    bool (*start)(SnxCore *core, SnxPath path);
    void (*stop)(SnxCore *core);
    // Non-blocking work: accept, handshake, parse, ship input. Must not exceed timeout_ms.
    void (*poll)(SnxCore *core, int timeout_ms);
} SnxBackendOps;

extern const SnxBackendOps snx_backend_solar;
extern const SnxBackendOps snx_backend_sunshine;

// --- helpers ---
void snx__log(SnxCore *core, int level, const char *fmt, ...);
void snx__set_state(SnxCore *core, SnxState state, const char *detail);
// Thread-safe snapshot of the platform's latest input state.
void snx__input_snapshot(SnxCore *core, SnxInputState *out);

// Route classification (testable core of route.c).
typedef struct SnxRouteIface {
    uint32_t addr;   // network byte order
    uint32_t mask;   // network byte order
    int      loopback;
} SnxRouteIface;
SnxPath snx__route_classify(uint32_t host, const SnxRouteIface *ifaces, size_t count);

// net.c - POSIX socket helpers (no Windows yet; add an equivalent when needed).
int  snx__tcp_listen(int port);          // listening fd on INADDR_ANY, or -1
int  snx__accept_ms(int listenFd, int timeout_ms); // accepted fd, -1 on timeout, -2 on error
void snx__io_timeout(int fd, int seconds); // SO_RCVTIMEO/SO_SNDTIMEO
bool snx__read_all(int fd, void *buf, size_t len);
bool snx__write_all(int fd, const void *buf, size_t len);
// UDP receiver bound to INADDR_ANY:port with a 256KB kernel buffer (keyframe bursts
// at LAN speed - the Switch client's sizing, see its network.c) and a drain helper.
int  snx__udp_listen(int port);
int  snx__udp_recv(int fd, void *buf, size_t len); // n, 0 on nothing, -1 on error

// library.c - the library channel protocol, on I/O callbacks (unit-testable).
typedef bool (*SnxIoRead)(void *io, void *buf, size_t len);
typedef bool (*SnxIoWrite)(void *io, const void *buf, size_t len);

typedef struct {
    uint64_t hash;         // content hash the host announced
    uint8_t  buttonLayout; // 1 = host says its layout is swapped (A/B X/Y by label)
    uint32_t layoutRevision;
    SnxPeerVersion pc;     // what the PC announced (major/minor/app)
    bool     pcAnnounced;  // false for a legacy 0.2.4-era host
    bool     compatible;   // same protocol major
    bool     requested;     // we asked the host to (re)send the payload
} SnxLibraryInfo;

// Runs one full library connection on the given I/O. `haveHash` suppresses the
// transfer when the content is unchanged. Returns a malloc'd array of `count`
// games (free with snx__library_free), or NULL when nothing was received.
typedef struct {
    uint32_t id;
    char     name[SNX_LIBRARY_MAX_NAME_BYTES + 1];
    uint8_t *cover;
    size_t   coverSize;
} SnxLibEntryRaw;

SnxLibEntryRaw *snx__library_session(SnxIoRead rd, SnxIoWrite wr, void *io,
                                     const char *ownAppVersion,
                                     bool haveHash, uint64_t lastHash,
                                     SnxLibraryInfo *info, size_t *count);
void snx__library_free(SnxLibEntryRaw *entries, size_t count);
