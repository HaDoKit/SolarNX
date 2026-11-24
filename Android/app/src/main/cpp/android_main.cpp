// Android entry point (SDL_main via libmain.so): the shared shell renders the UI,
// libsolarnx-core owns the session (route detection, library channel, later HID/
// video/audio) on its own thread and reports back through the SDL event queue.

#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <SDL_image.h>
#include <android/log.h>
#include <jni.h>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "snxui.h"
#include "solarnx/core.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SolarNX", __VA_ARGS__)

static bool running = true;
static Uint32 g_text_result_ev = 0; // user event carrying a native text dialog result
static Uint32 g_lib_ev = 0;         // user event carrying a core library update
static Uint32 g_state_ev = 0;       // user event carrying a core state change

struct TextResult { std::string key, text; bool ok; };

// Deep copy of a core library update - the core reuses its storage right after
// the callback returns, and the shell consumes this on the main thread.
struct LibUpdate
{
    std::vector<SnxUiGame> games;
    std::vector<std::string> names;
    std::vector<std::vector<uint8_t>> covers;
};
struct StateUpdate { std::string line; };

// --- the core and its thread ---
static SnxCore *g_core = NULL;
static std::thread g_core_thread;
static std::atomic<bool> g_core_run{false};
static std::string g_host_address; // last applied host
static int g_host_backend = SNX_BACKEND_SOLAR;

// --- input collection (main thread): SDL gamepads + touch -> SnxInputState ---
static SnxInputState g_input_state = {};
static SDL_FingerID g_touch_finger = -1; // primary touch being tracked
struct PadSlot
{
    SDL_JoystickID id;
    SDL_GameController *ctrl;
};
static PadSlot g_pads[4];

// SDL controller button -> SolarNX wire bit (A=0..Down=15, see wire.h). -1 = unmapped.
static int wireButtonBit(int b)
{
    switch (b)
    {
    case SDL_CONTROLLER_BUTTON_A: return 0;
    case SDL_CONTROLLER_BUTTON_B: return 1;
    case SDL_CONTROLLER_BUTTON_X: return 2;
    case SDL_CONTROLLER_BUTTON_Y: return 3;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 4;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return 5;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 6;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 7;
    case SDL_CONTROLLER_BUTTON_START: return 10;  // Plus
    case SDL_CONTROLLER_BUTTON_BACK: return 11;   // Minus
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 12;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 13;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 14;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 15;
    default: return -1; // ZL/ZR come from the trigger axes below
    }
}

static int padSlotOf(SDL_JoystickID id)
{
    for (int i = 0; i < 4; i++)
        if (g_pads[i].ctrl != NULL && g_pads[i].id == id)
            return i;
    return -1;
}

static int clampAxis(Sint16 v) { return v < -32767 ? -32767 : v; }

static void handleInputEvent(const SDL_Event &ev, int windowW, int windowH)
{
    switch (ev.type)
    {
    case SDL_CONTROLLERDEVICEADDED: {
        int slot = -1;
        for (int i = 0; i < 4; i++)
            if (g_pads[i].ctrl == NULL) { slot = i; break; }
        if (slot < 0)
            break; // the protocol carries four pads; extras are ignored
        SDL_GameController *c = SDL_GameControllerOpen(ev.cdevice.which);
        if (c == NULL)
            break;
        g_pads[slot].ctrl = c;
        g_pads[slot].id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
        g_input_state.pads[slot] = {};
        break;
    }
    case SDL_CONTROLLERDEVICEREMOVED: {
        int slot = padSlotOf(ev.cdevice.which);
        if (slot < 0)
            break;
        SDL_GameControllerClose(g_pads[slot].ctrl);
        g_pads[slot].ctrl = NULL;
        g_input_state.pads[slot] = {};
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        int slot = padSlotOf(ev.cbutton.which);
        int bit = wireButtonBit(ev.cbutton.button);
        if (slot < 0 || bit < 0)
            break;
        if (ev.type == SDL_CONTROLLERBUTTONDOWN)
            g_input_state.pads[slot].buttons |= (1u << bit);
        else
            g_input_state.pads[slot].buttons &= ~(1u << bit);
        break;
    }
    case SDL_CONTROLLERAXISMOTION: {
        int slot = padSlotOf(ev.caxis.which);
        if (slot < 0)
            break;
        SnxInputPad *p = &g_input_state.pads[slot];
        switch (ev.caxis.axis)
        {
        case SDL_CONTROLLER_AXIS_LEFTX: p->lx = clampAxis(ev.caxis.value); break;
        case SDL_CONTROLLER_AXIS_LEFTY: p->ly = clampAxis(ev.caxis.value); break;
        case SDL_CONTROLLER_AXIS_RIGHTX: p->rx = clampAxis(ev.caxis.value); break;
        case SDL_CONTROLLER_AXIS_RIGHTY: p->ry = clampAxis(ev.caxis.value); break;
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: // analog trigger -> the wire's digital ZL bit
            if (ev.caxis.value > 8192) p->buttons |= (1u << 8);
            else p->buttons &= ~(1u << 8);
            break;
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
            if (ev.caxis.value > 8192) p->buttons |= (1u << 9);
            else p->buttons &= ~(1u << 9);
            break;
        default: break;
        }
        break;
    }
    case SDL_FINGERDOWN:
    case SDL_FINGERMOTION:
    case SDL_FINGERUP: {
        // One primary touch point, window pixels; released = (0,0), like the Switch.
        if (ev.type == SDL_FINGERDOWN && g_touch_finger == -1)
            g_touch_finger = ev.tfinger.fingerId;
        if (ev.tfinger.fingerId == g_touch_finger)
        {
            if (ev.type == SDL_FINGERUP)
            {
                g_input_state.touchX1 = g_input_state.touchY1 = 0;
                g_touch_finger = -1;
            }
            else
            {
                g_input_state.touchX1 = (uint32_t)(ev.tfinger.x * windowW);
                g_input_state.touchY1 = (uint32_t)(ev.tfinger.y * windowH);
            }
        }
        break;
    }
    default: break;
    }
}

static void on_pick(void *, int target, uint32_t id)
{
    LOGI("pick: target=%d library_id=%u", target, id);
}

static void on_quit(void *)
{
    running = false;
}

static void save_settings();
static void load_settings();

static void on_setting(void *, const char *key, const char *value)
{
    LOGI("setting: %s=%s", key, value);
    if (strcmp(key, "pc_address") == 0)
        g_host_address = value != NULL ? value : "";
    else if (strcmp(key, "host_type") == 0 && value != NULL)
        g_host_backend = strcmp(value, "sunshine") == 0 ? SNX_BACKEND_SUNSHINE : SNX_BACKEND_SOLAR;
    save_settings();
    // The core thread owns the SnxCore: apply the change by restarting it from the
    // main loop (snx_core_set_host is same-thread-only by contract).
}

// --- settings persistence (simple key=value file in the app's data dir) ---

static std::string settings_path()
{
    const char *pref = SDL_GetPrefPath("SolarNX", "");
    if (pref == NULL)
        return "";
    return std::string(pref) + "settings.txt";
}

static void save_settings()
{
    std::string path = settings_path();
    if (path.empty())
        return;
    SDL_RWops *rw = SDL_RWFromFile(path.c_str(), "w");
    if (rw == NULL)
        return;
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "host=%s\nbackend=%s\n",
                     g_host_address.c_str(),
                     g_host_backend == SNX_BACKEND_SUNSHINE ? "sunshine" : "solar");
    SDL_RWwrite(rw, buf, 1, (size_t)n);
    SDL_RWclose(rw);
}

static void load_settings()
{
    std::string path = settings_path();
    if (path.empty())
        return;
    SDL_RWops *rw = SDL_RWFromFile(path.c_str(), "r");
    if (rw == NULL)
        return;
    char buf[1024];
    size_t n = (size_t)SDL_RWread(rw, buf, 1, sizeof(buf) - 1);
    SDL_RWclose(rw);
    buf[n] = '\0';
    for (char *line = strtok(buf, "\n"); line != NULL; line = strtok(NULL, "\n"))
    {
        char *eq = strchr(line, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        if (strcmp(line, "host") == 0)
            g_host_address = eq + 1;
        else if (strcmp(line, "backend") == 0)
            g_host_backend = strcmp(eq + 1, "sunshine") == 0 ? SNX_BACKEND_SUNSHINE
                                                             : SNX_BACKEND_SOLAR;
    }
    LOGI("settings loaded: host=%s backend=%d", g_host_address.c_str(), g_host_backend);
}

// --- core callbacks (core thread; everything is copied and posted to the main thread) ---

static void core_on_state(void *, SnxState state, const char *detail)
{
    LOGI("core state: %s (%s)", snx_state_name(state), detail != NULL ? detail : "");
    if (g_state_ev == 0)
        return;
    auto *u = new StateUpdate;
    u->line = detail != NULL && *detail != '\0'
                  ? std::string(snx_state_name(state)) + ": " + detail
                  : snx_state_name(state);
    SDL_Event ev = {};
    ev.type = g_state_ev;
    ev.user.data1 = u;
    SDL_PushEvent(&ev);
}

static void core_on_library(void *, const SnxLibraryGame *games, size_t count)
{
    if (g_lib_ev == 0 || games == NULL || count == 0)
        return;
    auto *u = new LibUpdate;
    u->names.resize(count);
    u->covers.resize(count);
    u->games.resize(count);
    for (size_t i = 0; i < count; i++)
    {
        u->names[i] = games[i].name != NULL ? games[i].name : "";
        if (games[i].cover != NULL && games[i].coverSize > 0)
            u->covers[i].assign(games[i].cover, games[i].cover + games[i].coverSize);
        u->games[i].id = games[i].id;
        u->games[i].name = u->names[i].c_str();
        u->games[i].cover = u->covers[i].empty() ? NULL : u->covers[i].data();
        u->games[i].cover_size = (uint32_t)u->covers[i].size();
    }
    SDL_Event ev = {};
    ev.type = g_lib_ev;
    ev.user.data1 = u;
    SDL_PushEvent(&ev);
}

static void core_on_log(void *, int level, const char *msg)
{
    LOGI("core: %s", msg != NULL ? msg : "");
}

static void core_thread_func()
{
    while (g_core_run.load())
        snx_core_poll(g_core, 200);
}

static void stop_core()
{
    if (g_core_thread.joinable())
    {
        g_core_run.store(false);
        g_core_thread.join();
    }
    if (g_core != NULL)
    {
        snx_core_destroy(g_core);
        g_core = NULL;
    }
}

static void start_core()
{
    stop_core();
    SnxConfig cfg = {};
    cfg.backend = g_host_backend;
    cfg.host = g_host_address.empty() ? NULL : g_host_address.c_str();
    cfg.appVersion = "SolarNX Android 0.2.5-alpha";
    SnxCallbacks cb = {};
    cb.on_state = core_on_state;
    cb.on_library = core_on_library;
    cb.on_log = core_on_log;
    g_core = snx_core_create(&cfg, &cb, NULL);
    if (g_core == NULL)
        return;
    g_core_run.store(true);
    g_core_thread = std::thread(core_thread_func);
}

static void on_text_request(void *, const char *key, const char *label, const char *current,
                            const char *hint)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    jclass cls = env->GetObjectClass(act);
    jmethodID mid = env->GetMethodID(cls, "showTextDialog",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    if (mid != nullptr)
    {
        jstring jkey = env->NewStringUTF(key ? key : "");
        jstring jlabel = env->NewStringUTF(label ? label : "");
        jstring jcur = env->NewStringUTF(current ? current : "");
        jstring jhint = env->NewStringUTF(hint ? hint : "");
        env->CallVoidMethod(act, mid, jkey, jlabel, jcur, jhint);
        env->DeleteLocalRef(jkey);
        env->DeleteLocalRef(jlabel);
        env->DeleteLocalRef(jcur);
        env->DeleteLocalRef(jhint);
    }
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck())
    {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
}

// Called on the UI thread from SnxActivity's AlertDialog; forwards through the SDL event queue
// so the shell stays single-threaded.
extern "C" JNIEXPORT void JNICALL
Java_org_solarnx_client_SnxActivity_nativeTextResult(JNIEnv *env, jobject, jstring key, jstring text)
{
    auto *r = new TextResult;
    const char *k = env->GetStringUTFChars(key, nullptr);
    r->key = k != nullptr ? k : "";
    if (k != nullptr) env->ReleaseStringUTFChars(key, k);
    r->ok = text != nullptr;
    if (text != nullptr)
    {
        const char *t = env->GetStringUTFChars(text, nullptr);
        r->text = t != nullptr ? t : "";
        if (t != nullptr) env->ReleaseStringUTFChars(text, t);
    }
    if (g_text_result_ev != 0)
    {
        SDL_Event ev = {};
        ev.type = g_text_result_ev;
        ev.user.data1 = r;
        SDL_PushEvent(&ev);
    }
    else
        delete r;
}

// Called on the UI thread from SnxActivity's OnBackInvokedCallback (API 33+ predictive
// back): re-injects the gesture as the classic SDLK_AC_BACK key pair so the shell's
// normal handling decides (menu: ignore, sub-screens: navigate back).
extern "C" JNIEXPORT void JNICALL
Java_org_solarnx_client_SnxActivity_nativeBackEvent(JNIEnv *, jobject)
{
    SDL_Event ev = {};
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.sym = SDLK_AC_BACK;
    SDL_PushEvent(&ev);
    ev.type = SDL_KEYUP;
    SDL_PushEvent(&ev);
}

extern "C" int SDL_main(int argc, char *argv[])
{
    running = true; // the process may be reused after the previous SDL_main quit

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "Landscape");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0"); // RmlUi handles touch natively
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER |
                 SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC | SDL_INIT_SENSOR) != 0)
    {
        LOGI("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    // Keep Wi-Fi awake while the app runs (a streaming client never wants doze throttling).
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    SDL_Window *window = SDL_CreateWindow("SolarNX", 0, 0, 0, 0,
                                        SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == NULL)
    {
        LOGI("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
                                              SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL)
    {
        LOGI("SDL_CreateRenderer failed: %s", SDL_GetError());
        return 1;
    }

    g_text_result_ev = SDL_RegisterEvents(1);
    g_lib_ev = SDL_RegisterEvents(1);
    g_state_ev = SDL_RegisterEvents(1);

    SnxUiCallbacks cb = {on_pick, on_quit, on_setting, on_text_request, NULL};
    SnxUi *ui = snxui_create(window, renderer, "", &cb);
    if (ui == NULL)
    {
        LOGI("snxui_create failed");
        return 1;
    }

    // The real session core: route detection (LAN/internet from the PC address, no
    // manual channel pick) + the library channel. The grid stays empty until the
    // host pushes its library.
    load_settings();
    snxui_set_host(ui, g_host_address.c_str(), g_host_backend);
    std::string cur_host;
    int cur_backend = -1;
    bool core_started = false;
    auto sync_core = [&]()
    {
        int want = g_host_backend;
        if (core_started && cur_host == g_host_address && cur_backend == want)
            return;
        stop_core();
        start_core();
        core_started = g_core != NULL;
        cur_host = g_host_address;
        cur_backend = want;
    };
    sync_core();

    Uint64 last = SDL_GetTicks64();
    int windowW = 0, windowH = 0;
    SDL_GetWindowSize(window, &windowW, &windowH);
    while (running)
    {
        // Host address / backend changed in settings: re-cycle the core.
        if (cur_host != g_host_address || cur_backend != g_host_backend)
            sync_core();

        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            // Raw input for the stream (gamepads + touch) - collected before the shell
            // sees the same events, they serve UI navigation too.
            handleInputEvent(ev, windowW, windowH);
            // Android BACK on the menu screen must not kill the app (the shell maps
            // it to B, which exits - correct for a console, wrong for Android).
            // Swallow it there; on sub-screens it still navigates back.
            if ((ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) &&
                ev.key.keysym.sym == SDLK_AC_BACK &&
                snxui_screen(ui) == SNX_SCREEN_MENU)
            {
                // keep running; ignore
            }
            else if (ev.type == SDL_QUIT || ev.type == SDL_APP_TERMINATING)
                running = false;
            else if (ev.type == g_text_result_ev)
            {
                auto *r = (TextResult *)ev.user.data1;
                snxui_text_result(ui, r->key.c_str(), r->ok ? r->text.c_str() : nullptr);
                delete r;
            }
            else if (ev.type == g_lib_ev)
            {
                auto *u = (LibUpdate *)ev.user.data1;
                snxui_set_library(ui, u->games.data(), (int)u->games.size());
                delete u;
            }
            else if (ev.type == g_state_ev)
            {
                auto *u = (StateUpdate *)ev.user.data1;
                snxui_set_status(ui, u->line.c_str(), NULL, NULL, NULL);
                delete u;
            }
            else
                snxui_process_event(ui, &ev);
        }
        Uint64 now = SDL_GetTicks64();
        float dt = (float)(now - last) / 1000.0f;
        if (dt > 1.0f) dt = 1.0f;
        last = now;

        // Hand the current input state to the core; the backend serializes and
        // ships it on its own cadence (~120 Hz while the HID channel is up).
        g_input_state.padCount = 0;
        for (int i = 0; i < 4; i++)
            if (g_pads[i].ctrl != NULL)
                g_input_state.padCount++;
        g_input_state.appTarget = SNX_APP_BIG_PICTURE;
        g_input_state.displayHeight = (uint32_t)windowH;
        if (g_core != NULL)
            snx_core_send_input(g_core, &g_input_state);

        snxui_update(ui, dt);
        snxui_render(ui);
    }

    snxui_destroy(ui);
    stop_core();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
