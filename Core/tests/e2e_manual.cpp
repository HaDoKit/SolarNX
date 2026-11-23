// Local end-to-end check: runs the core against a host simulator on the loopback
// (route: LAN, library channel live). Not part of the unit suite - run manually.
#include <stdio.h>
#include <thread>
#include <unistd.h>

#include "solarnx/core.h"

static void on_state(void *, SnxState state, const char *detail)
{
    printf("state: %s (%s)\n", snx_state_name(state), detail ? detail : "");
}
static void on_library(void *, const SnxLibraryGame *games, size_t count)
{
    printf("library: %zu game(s)\n", count);
    for (size_t i = 0; i < count; i++)
        printf("  [%u] %s (cover %zu bytes)\n", games[i].id, games[i].name, games[i].coverSize);
}
static void on_log(void *, int level, const char *msg) { printf("log: %s\n", msg); }

int main(void)
{
    SnxConfig cfg = {};
    cfg.backend = SNX_BACKEND_SOLAR;
    cfg.host = "127.0.0.1";
    cfg.appVersion = "core-e2e";
    SnxCallbacks cb = {};
    cb.on_state = on_state;
    cb.on_library = on_library;
    cb.on_log = on_log;

    SnxCore *core = snx_core_create(&cfg, &cb, NULL);
    if (core == NULL)
        return 1;

    for (int i = 0; i < 20; i++) // ~4s: detect + accept + handshake
    {
        snx_core_poll(core, 200);
        if (snx_core_state(core) == SNX_STATE_LIBRARY)
            break;
    }
    int ok = snx_core_state(core) == SNX_STATE_LIBRARY;
    snx_core_destroy(core);
    printf(ok ? "E2E OK\n" : "E2E FAILED (state=%s)\n", snx_state_name(snx_core_state(core)));
    return ok ? 0 : 1;
}
