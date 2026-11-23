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

static void on_pick(void *, int target, uint32_t id)
{
    LOGI("pick: target=%d library_id=%u", target, id);
}

static void on_quit(void *)
{
    running = false;
}

static void on_setting(void *, const char *key, const char *value)
{
    LOGI("setting: %s=%s", key, value);
    if (strcmp(key, "pc_address") == 0)
        g_host_address = value != NULL ? value : "";
    else if (strcmp(key, "host_type") == 0 && value != NULL)
        g_host_backend = strcmp(value, "sunshine") == 0 ? SNX_BACKEND_SUNSHINE : SNX_BACKEND_SOLAR;
    // The core thread owns the SnxCore: apply the change by restarting it from the
    // main loop (snx_core_set_host is same-thread-only by contract).
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
    while (running)
    {
        // Host address / backend changed in settings: re-cycle the core.
        if (cur_host != g_host_address || cur_backend != g_host_backend)
            sync_core();

        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT || ev.type == SDL_APP_TERMINATING)
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
