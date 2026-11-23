// Internal glue shared by the core sources - not installed, not public.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solarnx/core.h"
#include "solarnx/input.h"
#include "solarnx/route.h"
#include "solarnx/wire.h"

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
    // Input pending shipment by whichever backend implements it.
    SnxInputState input;
    bool          inputQueued;
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
