#pragma once

// libsolarnx-ui — shared graphical shell for all SolarNX clients (Switch, Android, ...).
// RmlUi documents on top of an SDL2 window/renderer supplied by the platform. The platform owns
// the main loop and pushes SDL events in; the library draws screens and reports the player's
// choices through callbacks. Everything on the wire/protocol side lives in libsolarnx-core —
// this library knows nothing about networking.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnxUi SnxUi;

// Forward declarations so hosts only need SDL.h where they create the window.
struct SDL_Window;
struct SDL_Renderer;
union SDL_Event;

// Menu targets, same order as the Switch client's SolarNxAppTarget.
enum {
    SNX_TARGET_BIGPICTURE = 0,
    SNX_TARGET_SHADPS4 = 1,
    SNX_TARGET_LIBRARY = 2, // a specific library game - see library_id
};

enum {
    SNX_SCREEN_MENU = 0,
    SNX_SCREEN_LIBRARY = 1,
    SNX_SCREEN_SETTINGS = 2,
};

// One entry of the host's game library. `cover` is the encoded image bytes (JPEG/PNG) exactly as
// they arrive from the PC - the shell decodes them itself; NULL draws the placeholder tile.
typedef struct {
    uint32_t id;
    const char *name;      // UTF-8
    const uint8_t *cover;  // encoded image bytes, may be NULL
    uint32_t cover_size;
} SnxUiGame;

typedef struct {
    // A tile or a library game was confirmed (A / tap). For SNX_TARGET_LIBRARY, library_id is the
    // game's id; for the other targets it is 0.
    void (*on_pick)(void *opaque, int target, uint32_t library_id);
    // The player asked to leave the app (quit gesture on the menu screen).
    void (*on_quit)(void *opaque);
    // A settings row changed (key: "language", "connection_mode", "pc_address", ...).
    void (*on_settings_changed)(void *opaque, const char *key, const char *value);
    // If set, the platform provides its own text editor (native dialog on Android/iOS)
    // instead of the shell's built-in SDL text input. Deliver the outcome through
    // snxui_text_result(); row_key is the same key family as on_settings_changed.
    void (*on_text_request)(void *opaque, const char *row_key, const char *label,
                            const char *current, const char *hint);
    void *opaque;
} SnxUiCallbacks;

// asset_root: where ui.rcml/img/... live - a real directory on desktop, "" on Android (files are
// then read from the APK assets via SDL_RWops).
SnxUi *snxui_create(SDL_Window *window, SDL_Renderer *renderer, const char *asset_root,
                    const SnxUiCallbacks *callbacks);
void snxui_destroy(SnxUi *ui);

// Feeds one SDL event. Gamepad buttons are mapped to navigation internally; the rest is handed to
// RmlUi (touch, mouse, keyboard, text input). Returns true when the shell consumed the event
// (e.g. navigation keys), false when the event went to the document or was ignored.
bool snxui_process_event(SnxUi *ui, const SDL_Event *event);

// Advances animations; call once per frame before snxui_render.
void snxui_update(SnxUi *ui, float dt);

// Draws the current screen into the renderer and presents it.
void snxui_render(SnxUi *ui);

// Replaces the library contents; the grid and the "My games" tile update on the next render.
void snxui_set_library(SnxUi *ui, const SnxUiGame *games, int count);

// Bottom-of-menu status lines; pass NULL to leave a line unchanged.
void snxui_set_status(SnxUi *ui, const char *ip_line, const char *layout_line,
                    const char *connection_line, const char *pc_line);

void snxui_show(SnxUi *ui, int screen);
int snxui_screen(const SnxUi *ui);

// Restores persisted values (platform storage) into the settings rows:
// the PC address and the host backend (0 SolarNX service, 1 Sunshine).
void snxui_set_host(SnxUi *ui, const char *address, int host_type);

// Multi-line core telemetry overlay (NULL or "" hides it). The platform feeds it
// from snx_core_stats() while developing - the debug block.
void snxui_set_debug(SnxUi *ui, const char *text);

// Result of a platform text editor started by on_text_request. NULL `text` = cancelled.
void snxui_text_result(SnxUi *ui, const char *row_key, const char *text);

#ifdef __cplusplus
} // extern "C"
#endif
