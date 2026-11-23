// Sunshine backend: a Sunshine host speaking the Moonlight protocol.
//
// Stub for now - the slot, the switch and the state reporting exist so the rest of
// the stack (settings UI, core dispatch) is built against the final shape. The
// implementation vendors moonlight-common-c (GPL-3.0, same license) and maps:
//   APP_LIST      -> on_library
//   DECODER/AUDIO -> on_video_frame / on_audio_packet
//   LiSend*       -> snx_core_send_input
// It needs a TLS provider (mbedTLS) and the RTSP handshake - a milestone of its own.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"

static bool sunshineStart(SnxCore *core, SnxPath path)
{
    (void)path;
    snx__set_state(core, SNX_STATE_BACKEND_UNSUPPORTED,
                   "Sunshine backend is not wired yet (planned via moonlight-common-c)");
    return false;
}

static void sunshineStop(SnxCore *core) { (void)core; }
static void sunshinePoll(SnxCore *core, int timeoutMs)
{
    (void)core;
    (void)timeoutMs;
}

const SnxBackendOps snx_backend_sunshine = {
    .name  = "sunshine",
    .start = sunshineStart,
    .stop  = sunshineStop,
    .poll  = sunshinePoll,
};
