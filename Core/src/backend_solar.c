// SolarNX backend: the built-in PC service protocol, LAN path.
//
// Current scope: the library channel (TCP 19003) - the PC dials in and pushes its
// game library. The HID channel (19001), video (UDP 19000) and audio (UDP 19002)
// land on the same accept/poll skeleton in the next milestones; the Internet path
// (encrypted UDP transport, Client/transport) after that.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    int libListenFd;
} SolarBackend;

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
    core->backendData = b;

    int port = core->cfg.libraryPort > 0 ? core->cfg.libraryPort : SNX_PORT_LIBRARY;
    b->libListenFd = snx__tcp_listen(port);
    if (b->libListenFd < 0)
    {
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
    free(b);
    core->backendData = NULL;
    core->libListenFd = -1;
}

static void deliverLibrary(SnxCore *core, const SnxLibEntryRaw *entries, size_t count)
{
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

static void solarPoll(SnxCore *core, int timeoutMs)
{
    SolarBackend *b = (SolarBackend *)core->backendData;
    if (b == NULL || b->libListenFd < 0)
        return;

    int fd = snx__accept_ms(b->libListenFd, timeoutMs);
    if (fd < 0)
        return; // -1 timeout (nothing to do), -2 transient accept error: keep listening

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

const SnxBackendOps snx_backend_solar = {
    .name  = "solar",
    .start = solarStart,
    .stop  = solarStop,
    .poll  = solarPoll,
};
