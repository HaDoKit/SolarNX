// Local end-to-end check: runs the core against a host simulator on the loopback
// (route: LAN, library channel live). Not part of the unit suite - run manually.
#include <stdio.h>
#include <thread>
#include <time.h>
#include <unistd.h>

#include "solarnx/core.h"

static uint64_t timeMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

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

    // Push an input state like a platform would (button A + stick on pad 0).
    SnxInputState in = {};
    in.padCount = 1;
    in.pads[0].buttons = 1u << 0;      // A
    in.pads[0].lx = -1234;
    in.displayHeight = 1080;
    in.appTarget = SNX_APP_BIG_PICTURE;
    snx_core_send_input(core, &in);

    for (int i = 0; i < 30; i++) // ~6s: detect + accepts + handshake + HID frames
    {
        snx_core_poll(core, 200);
        if (snx_core_state(core) == SNX_STATE_SESSION)
            break;
    }
    int ok = snx_core_state(core) == SNX_STATE_SESSION;
    if (ok)
    {
        // Keep the session up for a bit so the host simulator can read input frames.
        // Note each poll returns within ~8ms while HID is connected (the input cadence).
        uint64_t until = timeMs() + 1500;
        while (timeMs() < until)
            snx_core_poll(core, 100);
    }
    snx_core_destroy(core);
    printf(ok ? "E2E OK\n" : "E2E FAILED (state=%s)\n", snx_state_name(snx_core_state(core)));
    return ok ? 0 : 1;
}
