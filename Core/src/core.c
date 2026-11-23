// libsolarnx-core: state machine, route re-detection and backend dispatch.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t nowMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

const char *snx_state_name(SnxState state)
{
    switch (state)
    {
    case SNX_STATE_IDLE:               return "idle";
    case SNX_STATE_RESOLVING:          return "resolving";
    case SNX_STATE_WAIT_HOST:          return "waiting for host";
    case SNX_STATE_LIBRARY:            return "library received";
    case SNX_STATE_SESSION:            return "session";
    case SNX_STATE_PATH_UNSUPPORTED:   return "internet path not supported yet";
    case SNX_STATE_BACKEND_UNSUPPORTED: return "backend not supported yet";
    case SNX_STATE_ERROR:              return "error";
    default:                           return "?";
    }
}

void snx__log(SnxCore *core, int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (core != NULL && core->cb.on_log != NULL)
        core->cb.on_log(core->cbCtx, level, buf);
}

void snx__set_state(SnxCore *core, SnxState state, const char *detail)
{
    if (core->state == state && detail == NULL)
        return;
    core->state = state;
    if (core->cb.on_state != NULL)
        core->cb.on_state(core->cbCtx, state, detail);
}

SnxCore *snx_core_create(const SnxConfig *cfg, const SnxCallbacks *cb, void *ctx)
{
    if (cfg == NULL)
        return NULL;
    SnxCore *core = (SnxCore *)calloc(1, sizeof(*core));
    if (core == NULL)
        return NULL;

    core->cfg = *cfg;
    if (core->cfg.appVersion == NULL)
        core->cfg.appVersion = "solarnx-core";
    if (cb != NULL)
        core->cb = *cb;
    core->cbCtx = ctx;
    core->backend = (SnxBackend)cfg->backend;
    core->ops = core->backend == SNX_BACKEND_SUNSHINE ? &snx_backend_sunshine : &snx_backend_solar;
    core->libListenFd = -1;
    core->state = SNX_STATE_IDLE;
    pthread_mutex_init(&core->inputMutex, NULL);

    if (cfg->host != NULL && *cfg->host != '\0')
    {
        core->host = strdup(cfg->host);
        core->state = SNX_STATE_RESOLVING;
    }
    return core;
}

void snx_core_destroy(SnxCore *core)
{
    if (core == NULL)
        return;
    if (core->backendUp && core->ops != NULL)
        core->ops->stop(core);
    pthread_mutex_destroy(&core->inputMutex);
    free(core->host);
    free(core);
}

void snx_core_set_host(SnxCore *core, const char *host)
{
    if (core == NULL)
        return;
    free(core->host);
    core->host = (host != NULL && *host != '\0') ? strdup(host) : NULL;
    if (core->backendUp && core->ops != NULL)
    {
        core->ops->stop(core);
        core->backendUp = false;
    }
    core->libHashValid = false;
    core->state = core->host != NULL ? SNX_STATE_RESOLVING : SNX_STATE_IDLE;
}

// One connect cycle: detect the route from the address (no manual channel choice,
// no restart), then bring the right path's backend up.
static void startCycle(SnxCore *core)
{
    char resolved[64] = "";
    SnxPath path = snx_route_detect(core->host, resolved, sizeof(resolved));
    core->path = path;

    if (path == SNX_PATH_UNKNOWN)
    {
        snx__set_state(core, SNX_STATE_ERROR, "address did not resolve - retrying");
        return;
    }

    snx__log(core, 1, "host %s -> %s (%s)", resolved, snx_path_name(path), core->ops->name);

    if (path == SNX_PATH_INTERNET)
    {
        // The encrypted single-socket transport (Client/transport.c port) is a later
        // milestone; be explicit instead of silently falling back to LAN sockets.
        snx__set_state(core, SNX_STATE_PATH_UNSUPPORTED,
                       "internet path: the encrypted transport is not wired yet");
        return;
    }

    if (!core->ops->start(core, path))
    {
        if (core->state != SNX_STATE_BACKEND_UNSUPPORTED)
            snx__set_state(core, SNX_STATE_ERROR, "backend failed to start - retrying");
        return;
    }
    core->backendUp = true;
    snx__set_state(core, SNX_STATE_WAIT_HOST, resolved);
}

void snx_core_poll(SnxCore *core, int timeoutMs)
{
    if (core == NULL)
        return;

    if (core->host == NULL)
    {
        snx__set_state(core, SNX_STATE_IDLE, NULL);
        return;
    }

    if (!core->backendUp)
    {
        // Retry pacing: a host that is off or unreachable gets one attempt per cycle.
        uint64_t now = nowMs();
        if (core->state == SNX_STATE_RESOLVING || core->state == SNX_STATE_ERROR ||
            now - core->lastAttemptMs >= 5000)
        {
            core->lastAttemptMs = now;
            startCycle(core);
        }
        return;
    }

    core->ops->poll(core, timeoutMs);
}

void snx_core_send_input(SnxCore *core, const SnxInputState *input)
{
    if (core == NULL || input == NULL)
        return;
    pthread_mutex_lock(&core->inputMutex);
    core->input = *input;
    pthread_mutex_unlock(&core->inputMutex);
}

void snx__input_snapshot(SnxCore *core, SnxInputState *out)
{
    pthread_mutex_lock(&core->inputMutex);
    *out = core->input;
    pthread_mutex_unlock(&core->inputMutex);
}

SnxState snx_core_state(const SnxCore *core) { return core != NULL ? core->state : SNX_STATE_IDLE; }
SnxBackend snx_core_backend(const SnxCore *core)
{
    return core != NULL ? core->backend : SNX_BACKEND_SOLAR;
}
SnxPath snx_core_path(const SnxCore *core) { return core != NULL ? core->path : SNX_PATH_UNKNOWN; }
