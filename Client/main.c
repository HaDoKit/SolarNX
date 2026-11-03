#include <string.h>
#include <stdatomic.h>

#include <switch.h>

#include "common/lang/lang.h"
#include "network/network.h"
#include "video/video.h"
#include "audio/audio.h"
#include "input/input.h"
#include "menu/menu.h"
#include "menu/renderer.h"
#include "menu/starsystem.h"
#include "common/logger/logger.h"
#include "common/appstate/appstate.h"
#include "library/library.h"
#include "settings/settings.h"
#include "transport/transport.h"
#include "control/control.h"

// HID (network.c) blocks forever on its own accept/send loop, and video (video.c) blocks forever
// pumping appMainLoop()/SDL - they can't both run on one thread, so HID gets its own. 0x4000
// stack: generous headroom for logWrite()'s printf/vprintf calls (varargs formatting isn't
// cheap), not an empirically-tuned minimum - revisit if this ever needs to shrink. Priority 0x2C
// matches the main thread's own default priority (see libnx's thread.h).
//
// cpuid is pinned (core 0), not -2 (no affinity) as originally: confirmed live that HID input and
// audio playback both progressively degrade over a long session (increasing perceived lag) while
// video stays fine, with no errors visible in the log - consistent with the network/audio threads
// intermittently losing CPU time to video's own thread (NVTEGRA's hwframe readback falls back to
// a slow CPU-copy path most frames, see logInstallFfmpegCallback's comment) whenever the scheduler
// happened to put them on the same core, since all three threads previously had equal priority and
// no fixed core assignment. main() explicitly pins the main/video thread to core 1 to guarantee
// separation instead of hoping the scheduler keeps them apart.
#define NETWORK_THREAD_STACK_SIZE 0x4000
#define NETWORK_THREAD_PRIORITY   0x2C
#define NETWORK_THREAD_CORE       0
static Thread s_networkThread;

// audio.c blocks forever too (its own reconnect loop, checking appIsRunning() itself), and
// video already owns the main thread - so audio gets its own thread as well. Bigger stack than
// the network thread (0x8000 vs 0x4000): audio.c keeps an 8KB receive buffer plus a ~4KB PCM
// frame buffer on its stack, both of which the HID thread has no equivalent of. Pinned to its own
// core (2) for the same reason as NETWORK_THREAD_CORE above.
#define AUDIO_THREAD_STACK_SIZE 0x8000
#define AUDIO_THREAD_PRIORITY   0x2C
#define AUDIO_THREAD_CORE       2
static Thread s_audioThread;

// The main/video thread's own core - see the pinning rationale on NETWORK_THREAD_CORE above.
// Deliberately not core 0 or 2 (network/audio's cores) and not core 3 (commonly left for the
// system on homebrew titles) - set via svcSetThreadCoreMask at the top of main().
#define MAIN_THREAD_CORE 1

// ~120Hz. A press waits for the next send, so this interval adds up to its own length to the input
// lag (half of it on average). It was ~43Hz (23.3ms, inherited from the original client) - up to
// 23ms of lag for no reason: the host applies every packet as it arrives and 232 bytes at 120Hz is
// ~28KB/s, nothing for Wi-Fi or the Internet transport.
#define HID_SEND_INTERVAL_NS 8333333ULL
#define HID_HEARTBEAT_EVERY  120u // ~once per second at the interval above

_Bool initAudio() {
    return audioInit();
}

_Bool initVideo() {
    return videoInit();
}

_Bool initInput() {
    return inputInit();
}

_Bool initNetwork() {
    if (!networkInit())
        return 0;

    // Debug build only (see Makefile's DEBUG=1 / `make debug`): connects back over the network
    // to whichever PC sent this .nro via `nxlink -a <ip> SolarNX-Switch-debug.nro -s` through
    // hbmenu's netloader, so logWrite() streams there from here on - no separate step needed on
    // the console. Best-effort: not fatal if it fails (e.g. launched without -s, or launched
    // directly from hbmenu without nxlink at all) - the app should still run without it.
    logInit();
    logWrite("network", "HID listen socket up on port %d", SOLARNX_HID_PORT);

    // Before the first menu, so the host's library can arrive while it's on screen. Not fatal:
    // without it the "My games" tile just stays empty.
    if (!libraryStart())
        logWrite("library", "library listener failed to start - \"My games\" will stay empty");

    // Experimental Internet mode: this side dials the PC over one UDP port - session, library and
    // the video/audio relay all go through transport.c instead of the listeners above.
    if (settingsGetConnectionMode() == SettingsConnection_Internet && !transportStart())
        logWrite("transport", "UDP transport failed to start - Internet mode won't connect");

    return 1;
}

// Set by the HID thread while it's parked for the app-selection menu (see pauseHidWhileMenu) -
// main() waits for it before the menu reconfigures the pads, so inputRead() and menu.c never touch
// HID at the same time.
static atomic_bool s_hidPaused = false;

// Parks the HID thread for as long as the "back to the menu" request is up. The host connection
// is already closed by then (see startNetwork's send loop), and nothing is accepted while parked,
// so the host's reconnect attempts can't start a session underneath the menu.
static void pauseHidWhileMenu(void) {
    if (!appMenuRequested())
        return;

    atomic_store(&s_hidPaused, true);
    logWrite("network", "HID paused while the app-selection menu is open");
    while (appMenuRequested() && appIsRunning())
        svcSleepThread(50000000ULL); // 50ms
    atomic_store(&s_hidPaused, false);
}

// Blocks forever: accepts one HID client at a time on SOLARNX_HID_PORT (19001, matching
// SolarNX/Config.cs's HidPort) and keeps sending framed SolarNxHidPacket payloads (real live
// input state, read fresh from input.c every packet) until the send fails (PC disconnected) or the
// player asks to go back to the menu, then goes back to waiting for a reconnect - mirrors the
// host's SessionController reconnect-on-drop loop.
static void startNetwork(void) {
    while (1) {
        pauseHidWhileMenu();

        logWrite("network", "waiting for PC to connect on HID port %d...", SOLARNX_HID_PORT);

        // Never give up on HID: this thread is the only thing that ever starts a session (and runs
        // the back-to-menu gesture). It used to return here on any failure and stay dead for the
        // rest of the app's life.
        if (!networkAcceptHid()) {
            logWrite("network", "HID accept failed - retrying");
            svcSleepThread(500000000ULL); // 500ms - don't spin if accept() keeps failing
            continue;
        }

        logWrite("network", "HID client connected");

        SolarNxHidPacket pkt;

        u64 totalSent = 0;
        u32 sinceHeartbeat = 0;

        while (1) {
            inputRead(&pkt);

            // Raised by inputRead() itself (hold-Minus gesture). Closing the connection is what
            // makes the host end the session: its SessionStopped closes the running game and
            // exits Big Picture on the PC (AppOrchestrator.OnSessionStopped).
            if (appMenuRequested()) {
                logWrite("network", "back to menu requested - closing the HID connection so the host ends the session");
                networkCloseHidClient();
                inputStopRumble();
                break;
            }

            if (!networkSendHid(&pkt) || !networkFlushControl()) {
                logWrite("network", "HID send failed after %lu packets - PC disconnected, waiting for reconnect", totalSent);
                networkCloseHidClient();
                inputStopRumble();
                // A library game's session is ended by the host when the game exits (or couldn't
                // start) - that's the signal to bring the player back to the menu to pick the next
                // one, instead of sitting on a frozen stream waiting for a reconnect.
                if (inputGetAppTarget() == SolarNxAppTarget_Library) {
                    logWrite("network", "library session ended by the host - back to the menu");
                    appRequestMenu();
                }
                break;
            }

            totalSent++;
            if (++sinceHeartbeat >= HID_HEARTBEAT_EVERY) {
                logWrite("network", "HID connection stable - %lu packets sent (%d bytes each)", totalSent, (int)sizeof(pkt));
                sinceHeartbeat = 0;
            }

            // Not a plain sleep: the PC's control messages (control/control.h) are read meanwhile, so
            // they're handled as they arrive rather than once per interval.
            if (!networkWaitForHost(HID_SEND_INTERVAL_NS)) {
                logWrite("network", "the PC closed the HID connection after %lu packets", totalSent);
                networkCloseHidClient();
                inputStopRumble();
                if (inputGetAppTarget() == SolarNxAppTarget_Library) {
                    logWrite("network", "library session ended by the host - back to the menu");
                    appRequestMenu();
                }
                break;
            }
        }
    }
}

static void audioThreadFunc(void *arg) {
    (void)arg;
    audioStart();
}

static void networkThreadFunc(void *arg) {
    (void)arg;
    startNetwork();
}

// Shows the app-selection menu with its own SDL2 window/renderer/font (MenuContext), fully torn
// down again (freeRenderer) before returning - video.c creates its own afterwards, the two are
// never on screen at the same time. The pick lands via inputSetAppTarget(). Returns false if the
// app was asked to exit from the menu (or the menu couldn't be shown at all).
static bool runMenuScreen(void) {
    MenuContext *menuCtx = makeRenderer();
    if (menuCtx == NULL) {
        logWrite("main", "menu renderer init failed");
        return false;
    }

    StarSystem starSystem;
    starsystem_init(&starSystem, MENU_RESX / 2.0f, MENU_RESY / 2.0f);

    runAppSelectMenu(menuCtx, &starSystem);

    freeRenderer(menuCtx);
    return appIsRunning();
}

// Bounded: normally the HID thread parks within one send interval (~23ms) of the request, since
// the request is raised from inside its own send loop. The cap only guards against it being stuck
// elsewhere (e.g. inside accept()), where it isn't touching the pads anyway.
static void waitForHidPaused(void) {
    for (int i = 0; i < 200 && !atomic_load(&s_hidPaused); i++)
        svcSleepThread(10000000ULL); // 10ms, up to 2s total
}

// startNetwork() blocks forever with no exit condition of its own (see its doc comment) - this
// thread runs for the process's whole lifetime and is deliberately never joined. When main()
// returns, process exit tears the thread and its open socket down along with everything else -
// there's nothing to signal it to stop cleanly first, and nothing that needs there to be, yet.
void main() {
    // Pin this thread (which video.c's render loop runs on) to its own core - see
    // MAIN_THREAD_CORE's doc comment. Best-effort: a failure here just leaves this thread on
    // whatever core the system assigned it, not fatal to the rest of startup.
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, MAIN_THREAD_CORE, BIT(MAIN_THREAD_CORE));

    // Mounts the embedded romfs (icons/etc for the menu below) - must happen before makeRenderer()
    // tries to load anything from "romfs:/...".
    romfsInit();
    settingsInit(); // before initNetwork() starts the library thread, which syncs these
    controlInit();  // before the library thread and transport, which report the PC's version
    langInit();

    if (!initNetwork())
        return;

    // Runs before initInput(): menu.c's runAppSelectMenu() configures/reads a single dedicated
    // PadState of its own (padConfigureInput(1, ...)) rather than touching input.c's 4-player
    // setup. The choice made here (see inputSetAppTarget) must land before the HID send loop's
    // very first packet, so this also has to finish before the network thread is spawned below.
    //
    // The menu loop also ends when the user closes the app from it (appMainLoop() went false) -
    // that must not fall through into starting HID/audio/video. Nothing but the HID listen socket
    // (networkInit) and romfs are up yet, so that's all there is to tear down.
    if (!runMenuScreen()) {
        logWrite("main", "exit requested from the menu - shutting down");
        networkExit();
        romfsExit();
        return;
    }

    // Before spawning the HID thread: it calls inputRead() as soon as a PC connects, which needs
    // pad/touch/sixaxis already configured.
    if (!initInput()) {
        logWrite("main", "input init failed");
        return;
    }

    if (R_FAILED(threadCreate(&s_networkThread, networkThreadFunc, NULL, NULL,
            NETWORK_THREAD_STACK_SIZE, NETWORK_THREAD_PRIORITY, NETWORK_THREAD_CORE)) ||
        R_FAILED(threadStart(&s_networkThread)))
    {
        logWrite("main", "failed to start HID network thread");
        return;
    }

    if (!initAudio()) {
        logWrite("main", "audio init failed");
        return;
    }

    // Audio runs on its own thread for the whole process lifetime - across menu round-trips too
    // (it just receives nothing while no session is up).
    if (R_FAILED(threadCreate(&s_audioThread, audioThreadFunc, NULL, NULL,
            AUDIO_THREAD_STACK_SIZE, AUDIO_THREAD_PRIORITY, AUDIO_THREAD_CORE)) ||
        R_FAILED(threadStart(&s_audioThread)))
    {
        logWrite("main", "failed to start audio thread");
        return;
    }

    // Stream, and whenever the player holds Minus long enough (input.c), go back to the menu:
    // video.c's loop returns, its SDL window is torn down so the menu can create its own, the HID
    // thread has closed the host connection (the host closes the game and Big Picture), the menu
    // runs, and streaming restarts with the newly picked target.
    while (appIsRunning()) {
        if (!initVideo()) {
            logWrite("main", "video init failed");
            return;
        }

        videoStart(); // main thread - owns appMainLoop()/the SDL event pump
        videoExit();

        if (!appIsRunning() || !appMenuRequested())
            break;

        logWrite("main", "returning to the app-selection menu");
        waitForHidPaused();
        if (!runMenuScreen())
            break;
        inputReconfigure();
        appClearMenuRequest(); // releases the HID thread - the host reconnects within ~1s
    }

    // Unlike startNetwork(), audioStart()'s loop checks appIsRunning() itself and actually
    // returns once the app is exiting - safe to wait for it here instead of leaving it to process
    // teardown the way the HID thread is.
    threadWaitForExit(&s_audioThread);
    threadClose(&s_audioThread);
    audioExit();

    inputExit();

    romfsExit();
}
