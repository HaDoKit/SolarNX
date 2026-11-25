#include "snxui.h"

#include <RmlUi/Core.h>
#include <SDL2/SDL.h>
#include <SDL_image.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <string>
#include <vector>

#include "file_interface.h"
#include "starfield.h"
#include "strings.h"

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_SDL.h"

// ---------------------------------------------------------------------------
// Bound data model "shell"

struct GameItem {
    Rml::String name;
    Rml::String cover; // "covers/<id>.jpg" or "" for the placeholder tile
    bool has_cover;
    Rml::String gid;
};

struct RowItem {
    Rml::String label;
    Rml::String value;
};

struct SnxUi {
    SDL_Window *window;
    SDL_Renderer *renderer;
    int width, height; // drawable pixels

    Rml::UniquePtr<SystemInterface_SDL> sys;
    Rml::UniquePtr<RenderInterface_SDL> rdr;
    Rml::UniquePtr<ShellFileInterface> files;
    Rml::Context *ctx = nullptr;
    Rml::ElementDocument *doc_menu = nullptr;
    Rml::ElementDocument *doc_lib = nullptr;
    Rml::ElementDocument *doc_set = nullptr;
    Rml::ElementDocument *doc_stream = nullptr;
    Rml::DataModelHandle model;

    Starfield stars = {};
    SDL_Texture *background = nullptr;

    SnxUiCallbacks cb = {};

    int screen = SNX_SCREEN_MENU;
    int menu_sel = 0;
    int lib_sel = 0;
    int set_sel = 0;
    int grid_cols = 6;
    bool touch_ui = false; // last input was finger/mouse -> show touch hints and buttons

    std::vector<GameItem> games;
    std::vector<uint32_t> game_ids;
    std::vector<RowItem> setrows;

    // Bound strings (re-rendered on language change / status update)
    Rml::String t_mygames, t_settings, t_hint, s_ip, s_layout, s_conn, s_pc, lib_l1, lib_l2, ver;
    Rml::String debug_text; // multi-line core telemetry overlay; empty = hidden
    Rml::String libtile_cover, note1, note2, note3, t_back;
    Rml::String cov_style; // adaptive cover limits ("max-width/max-height" - keeps image aspect)
    Rml::String ph_style;  // fixed box for the no-cover placeholder

    // Settings values (not all bound - values live in setrows[i].value)
    int language = 0;       // 0 auto, 1 en, 2 ru
    int host_type = 0;      // 0 SolarNX service, 1 Sunshine
    int rumble = 0;         // 0 off, 1 large-only, 2 all
    std::string address, server, server_key, pc_id;
    bool paired = false;
    std::string paired_host;

    // Settings edit mode (gamepad-friendly SDL text input)
    bool editing = false;
    int edit_row = -1;
    std::string edit_buf;

    bool quit = false;

    // Platform underlay (video plane) drawn beneath the documents.
    void (*underlay)(void *opaque, SDL_Renderer *) = nullptr;
    void *underlay_opaque = nullptr;
};

// ---------------------------------------------------------------------------
// Settings rows (same order as the Switch client)

enum { ROW_HOSTTYPE, ROW_ADDRESS, ROW_SERVER, ROW_SERVER_KEY, ROW_PC_ID, ROW_PAIR, ROW_LANGUAGE, ROW_RUMBLE };
static const int kRowCount = 8;

static const StrId kRowLabel[] = {
    Str_RowHostType, Str_RowAddress, Str_RowServer, Str_RowServerKey,
    Str_RowPcId, Str_RowPair, Str_RowLanguage, Str_RowRumble,
};

// Placeholder/hint for the platform text editor, only used by the text rows.
static const StrId kRowEditHint[] = {
    Str_EditAddress, Str_EditAddress, Str_EditServer, Str_EditServerKey,
    Str_EditPcId, Str_EditAddress, Str_EditAddress, Str_EditAddress,
};

static void rebuild_rows(SnxUi *ui)
{
    const char *lang_name = ui->language == 2 ? "Русский" : ui->language == 1 ? "English" : tr(Str_LanguageAuto);
    const char *rumble_txt = tr(ui->rumble == 0 ? Str_Off : ui->rumble == 1 ? Str_RumbleLargeOnly : Str_RumbleAll);

    const char *labels[kRowCount];
    for (int i = 0; i < kRowCount; i++)
        labels[i] = tr(kRowLabel[i]);

    char pairing[128];
    snprintf(pairing, sizeof(pairing), "%s%s", tr(Str_PairedPrefix), ui->paired_host.c_str());
    const char *values[kRowCount] = {
        tr(ui->host_type == 1 ? Str_HostSunshine : Str_HostSolar),
        !ui->address.empty() ? ui->address.c_str() : tr(Str_NotSet),
        !ui->server.empty() ? ui->server.c_str() : tr(Str_NoServer),
        tr(!ui->server_key.empty() ? Str_KeySet : Str_KeyNone),
        !ui->pc_id.empty() ? ui->pc_id.c_str() : tr(Str_NotSet),
        ui->paired ? pairing : tr(Str_NotPaired),
        lang_name,
        rumble_txt,
    };

    static char edit_shown[300];
    for (int i = 0; i < kRowCount; i++)
    {
        if (ui->editing && i == ui->edit_row)
        {
            // Show the typed text next to the label on the left - the soft keyboard
            // often overlays the right half of the screen where the value would be.
            snprintf(edit_shown, sizeof(edit_shown), "%s:  %s|", labels[i], ui->edit_buf.c_str());
            ui->setrows[i].label = edit_shown;
            ui->setrows[i].value = "";
        }
        else
        {
            ui->setrows[i].label = labels[i];
            ui->setrows[i].value = values[i];
        }
    }
}

// ---------------------------------------------------------------------------
// Localized bound strings

static void refresh_strings(SnxUi *ui)
{
    ui->t_mygames = tr(Str_MyGames);
    ui->t_settings = tr(Str_SettingsTitle);
    ui->lib_l1 = tr(ui->games.empty() && ui->game_ids.empty() ? Str_LibWaiting : Str_LibEmpty);
    ui->lib_l2 = tr(Str_LibAddHint);
    ui->note1 = tr(Str_Note1);
    ui->note2 = tr(Str_Note2);
    ui->note3 = tr(Str_Note3);
    ui->t_back = tr(Str_Back);
    StrId hint = ui->screen == SNX_SCREEN_SETTINGS ? (ui->touch_ui ? Str_HintChangeTouch : Str_HintChange)
                 : ui->screen == SNX_SCREEN_LIBRARY && !ui->games.empty()
                     ? (ui->touch_ui ? Str_HintPlayTouch : Str_HintPlay)
                     : Str_HintBack;
    ui->t_hint = tr(hint);
    rebuild_rows(ui);
    ui->model.DirtyAllVariables();
}

// ---------------------------------------------------------------------------
// Screens

static void update_hint(SnxUi *ui)
{
    refresh_strings(ui);
}

static void set_touch_ui(SnxUi *ui, bool on)
{
    if (ui->touch_ui == on)
        return;
    ui->touch_ui = on;
    ui->model.DirtyVariable("touch_ui");
    update_hint(ui);
}

static void show_screen(SnxUi *ui, int screen)
{
    ui->screen = screen;
    Rml::ElementDocument *docs[] = {ui->doc_menu, ui->doc_lib, ui->doc_set,
                                   ui->doc_stream};
    for (int i = 0; i < 4; i++)
    {
        if (docs[i] == nullptr)
            continue;
        if (i == screen) docs[i]->Show();
        else docs[i]->Hide();
    }
    update_hint(ui);
}

static void update_layout(SnxUi *ui)
{
    // Columns in dp space: 720dp-tall canvas, dp width follows the real aspect ratio.
    // At most 4 tiles per row; covers keep their own aspect inside a 2:3 slot.
    const float dp_w = ui->width * 720.0f / ui->height;
    int cols = (int)(dp_w - 120) / 188;
    if (cols > 4) cols = 4;
    if (cols < 1) cols = 1;
    ui->grid_cols = cols;

    const int pitch = (int)(dp_w - 120) / cols; // tile + border + margins
    // Grow covers with the screen (capped) so the wrap still yields `cols` per row;
    // the -4 leaves rounding slack so the last tile never wraps to the next line.
    const int cw = pitch - 32 < 340 ? pitch - 32 : 340;
    char buf[96];
    // Fill the slot width, cap height: keeps aspect for any cover art.
    snprintf(buf, sizeof(buf), "width: 100%%; height: auto; max-width: %ddp; max-height: %ddp;",
             cw, cw * 3 / 2);
    ui->cov_style = buf;
    snprintf(buf, sizeof(buf), "width: %ddp; height: %ddp; line-height: %ddp;",
             cw, cw * 3 / 2, cw * 3 / 2);
    ui->ph_style = buf;
}

// ---------------------------------------------------------------------------
// Actions (also exposed to RCML via data-event-click)

static void act_pick_menu(SnxUi *ui, int idx)
{
    ui->menu_sel = idx;
    ui->model.DirtyVariable("menusel");
    if (idx == SNX_TARGET_LIBRARY)
    {
        ui->lib_sel = 0;
        ui->model.DirtyVariable("libsel");
        show_screen(ui, SNX_SCREEN_LIBRARY);
        return;
    }
    if (ui->cb.on_pick)
        ui->cb.on_pick(ui->cb.opaque, idx, 0);
}

static void act_pick_game(SnxUi *ui, int idx)
{
    if (idx < 0 || idx >= (int)ui->game_ids.size())
        return;
    ui->lib_sel = idx;
    ui->model.DirtyVariable("libsel");
    if (ui->cb.on_pick)
        ui->cb.on_pick(ui->cb.opaque, SNX_TARGET_LIBRARY, ui->game_ids[idx]);
}

static const char *kRowKey[] = {
    "host_type", "pc_address", "server", "server_key", "pc_id", "pair", "language", "rumble",
};

static void notify_row(SnxUi *ui, int row, const char *value)
{
    if (ui->cb.on_settings_changed)
        ui->cb.on_settings_changed(ui->cb.opaque, kRowKey[row], value);
}

static void edit_commit(SnxUi *ui);

static void act_row(SnxUi *ui, int row)
{
    switch (row)
    {
    case ROW_HOSTTYPE:
        // The channel itself is never picked by hand: the core detects the route
        // (LAN vs Internet) from the PC address. This row only selects the protocol.
        ui->host_type ^= 1;
        notify_row(ui, row, ui->host_type ? "sunshine" : "solar");
        break;
    case ROW_LANGUAGE:
        ui->language = (ui->language + 1) % 3;
        stringsSetLanguage(ui->language);
        notify_row(ui, row, ui->language == 2 ? "ru" : ui->language == 1 ? "en" : "auto");
        break;
    case ROW_RUMBLE:
        ui->rumble = (ui->rumble + 1) % 3;
        notify_row(ui, row, ui->rumble == 2 ? "all" : ui->rumble == 1 ? "large_only" : "off");
        break;
    case ROW_ADDRESS:
    case ROW_SERVER:
    case ROW_SERVER_KEY:
    case ROW_PC_ID:
    {
        const std::string &cur = row == ROW_ADDRESS ? ui->address
                               : row == ROW_SERVER  ? ui->server
                               : row == ROW_SERVER_KEY ? ui->server_key
                                                    : ui->pc_id;
        if (ui->editing && ui->edit_row != row)
            edit_commit(ui); // switching rows while editing keeps the typed value
        if (ui->cb.on_text_request)
        {
            // The platform shows its own editor (native dialog) and calls snxui_text_result.
            ui->cb.on_text_request(ui->cb.opaque, kRowKey[row], tr(kRowLabel[row]), cur.c_str(),
                                   tr(kRowEditHint[row]));
            break;
        }
        ui->editing = true;
        ui->edit_row = row;
        ui->edit_buf = cur;
        SDL_StartTextInput();
        break;
    }
    case ROW_PAIR:
        notify_row(ui, row, "request");
        break;
    }
    rebuild_rows(ui);
    ui->model.DirtyVariable("setrows");
    if (row == ROW_LANGUAGE)
        refresh_strings(ui); // everything relocalizes
}

static void edit_commit(SnxUi *ui)
{
    switch (ui->edit_row)
    {
    case ROW_ADDRESS: ui->address = ui->edit_buf; break;
    case ROW_SERVER: ui->server = ui->edit_buf; break;
    case ROW_SERVER_KEY: ui->server_key = ui->edit_buf; break;
    case ROW_PC_ID: ui->pc_id = ui->edit_buf; break;
    }
    if (ui->edit_row >= 0)
        notify_row(ui, ui->edit_row, ui->edit_buf.c_str());
    ui->editing = false;
    ui->edit_row = -1;
    SDL_StopTextInput();
    rebuild_rows(ui);
    ui->model.DirtyVariable("setrows");
}

static void edit_cancel(SnxUi *ui)
{
    ui->editing = false;
    ui->edit_row = -1;
    SDL_StopTextInput();
    rebuild_rows(ui);
    ui->model.DirtyVariable("setrows");
}

// ---------------------------------------------------------------------------
// Gamepad / keyboard navigation (applied before RmlUi sees the event)

enum NavAction { NAV_NONE, NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_OK, NAV_BACK, NAV_X, NAV_Y };

static NavAction nav_from_key(SDL_Keycode k)
{
    switch (k)
    {
    case SDLK_UP: return NAV_UP;
    case SDLK_DOWN: return NAV_DOWN;
    case SDLK_LEFT: return NAV_LEFT;
    case SDLK_RIGHT: return NAV_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: return NAV_OK;
    case SDLK_ESCAPE: case SDLK_AC_BACK: return NAV_BACK;
    case SDLK_x: return NAV_X;
    case SDLK_y: return NAV_Y;
    default: return NAV_NONE;
    }
}

static NavAction nav_from_button(Uint8 b)
{
    switch (b)
    {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return NAV_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return NAV_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return NAV_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return NAV_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return NAV_OK;
    case SDL_CONTROLLER_BUTTON_B: return NAV_BACK;
    case SDL_CONTROLLER_BUTTON_X: return NAV_X;
    case SDL_CONTROLLER_BUTTON_Y: return NAV_Y;
    default: return NAV_NONE;
    }
}

static void nav_apply(SnxUi *ui, NavAction a)
{
    if (a == NAV_NONE)
        return;

    if (ui->screen == SNX_SCREEN_MENU)
    {
        if (a == NAV_LEFT)
            ui->menu_sel = (ui->menu_sel + 2) % 3;
        else if (a == NAV_RIGHT)
            ui->menu_sel = (ui->menu_sel + 1) % 3;
        else if (a == NAV_OK)
            act_pick_menu(ui, ui->menu_sel);
        else if (a == NAV_X)
            show_screen(ui, SNX_SCREEN_SETTINGS);
        else if (a == NAV_BACK)
        {
            if (ui->cb.on_quit)
                ui->cb.on_quit(ui->cb.opaque);
            ui->quit = true;
        }
        ui->model.DirtyVariable("menusel");
    }
    else if (ui->screen == SNX_SCREEN_LIBRARY)
    {
        int count = (int)ui->games.size();
        int cols = ui->grid_cols;
        if (a == NAV_BACK)
            show_screen(ui, SNX_SCREEN_MENU);
        else if (a == NAV_OK && count > 0)
            act_pick_game(ui, ui->lib_sel);
        else if (count > 0)
        {
            if (a == NAV_LEFT && ui->lib_sel > 0) ui->lib_sel--;
            else if (a == NAV_RIGHT && ui->lib_sel + 1 < count) ui->lib_sel++;
            else if (a == NAV_UP && ui->lib_sel >= cols) ui->lib_sel -= cols;
            else if (a == NAV_DOWN) ui->lib_sel = ui->lib_sel + cols < count ? ui->lib_sel + cols : count - 1;
        }
        ui->model.DirtyVariable("libsel");
    }
    else if (ui->screen == SNX_SCREEN_SETTINGS)
    {
        if (a == NAV_BACK)
            show_screen(ui, SNX_SCREEN_MENU);
        else if (a == NAV_UP && ui->set_sel > 0) ui->set_sel--;
        else if (a == NAV_DOWN && ui->set_sel + 1 < kRowCount) ui->set_sel++;
        else if (a == NAV_OK) act_row(ui, ui->set_sel);
        ui->model.DirtyVariable("setsel");
    }
    else if (ui->screen == SNX_SCREEN_STREAM)
    {
        if (a == NAV_BACK)
            show_screen(ui, SNX_SCREEN_MENU);
    }
}

// ---------------------------------------------------------------------------
// Public API

extern "C" {

SnxUi *snxui_create(SDL_Window *window, SDL_Renderer *renderer, const char *asset_root,
                    const SnxUiCallbacks *callbacks)
{
    SnxUi *ui = new SnxUi();
    ui->window = window;
    ui->renderer = renderer;
    if (callbacks)
        ui->cb = *callbacks;
    {
        const char *plat = SDL_GetPlatform();
        ui->touch_ui = plat != NULL && (strcmp(plat, "Android") == 0 || strcmp(plat, "iOS") == 0);
    }

    SDL_GetRendererOutputSize(renderer, &ui->width, &ui->height);
    update_layout(ui);

    ui->sys = Rml::MakeUnique<SystemInterface_SDL>(window);
    ui->rdr = Rml::MakeUnique<RenderInterface_SDL>(renderer);
    ui->files = Rml::MakeUnique<ShellFileInterface>(asset_root);

    Rml::SetSystemInterface(ui->sys.get());
    Rml::SetRenderInterface(ui->rdr.get());
    Rml::SetFileInterface(ui->files.get());
    if (!Rml::Initialise())
    {
        SDL_Log("snxui: Rml::Initialise failed");
        delete ui;
        return NULL;
    }

    Rml::LoadFontFace("fonts/Roboto-Regular.ttf", "Roboto", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Normal);
    Rml::LoadFontFace("fonts/Roboto-Medium.ttf", "Roboto", Rml::Style::FontStyle::Normal,
                      Rml::Style::FontWeight::Bold);

    ui->ctx = Rml::CreateContext("main", Rml::Vector2i(ui->width, ui->height));
    if (ui->ctx == nullptr)
    {
        SDL_Log("snxui: CreateContext failed");
        Rml::Shutdown();
        delete ui;
        return NULL;
    }
    // The RCSS is authored in dp where 1dp = 1px at 720p - scale to the actual screen height.
    ui->ctx->SetDensityIndependentPixelRatio(ui->height / 720.0f);

    // --- data model ---
    {
        Rml::DataModelConstructor c = ui->ctx->CreateDataModel("shell");

        if (auto h = c.RegisterStruct<GameItem>())
        {
            h.RegisterMember("name", &GameItem::name);
            h.RegisterMember("cover", &GameItem::cover);
            h.RegisterMember("has_cover", &GameItem::has_cover);
            h.RegisterMember("gid", &GameItem::gid);
        }
        c.RegisterArray<std::vector<GameItem>>();
        c.Bind("games", &ui->games);

        if (auto h = c.RegisterStruct<RowItem>())
        {
            h.RegisterMember("label", &RowItem::label);
            h.RegisterMember("value", &RowItem::value);
        }
        c.RegisterArray<std::vector<RowItem>>();
        ui->setrows.resize(kRowCount);
        c.Bind("setrows", &ui->setrows);

        c.Bind("menusel", &ui->menu_sel);
        c.Bind("libsel", &ui->lib_sel);
        c.Bind("setsel", &ui->set_sel);

        c.Bind("t_mygames", &ui->t_mygames);
        c.Bind("t_settings", &ui->t_settings);
        c.Bind("t_hint", &ui->t_hint);
        c.Bind("s_ip", &ui->s_ip);
        c.Bind("s_layout", &ui->s_layout);
        c.Bind("s_conn", &ui->s_conn);
        c.Bind("s_pc", &ui->s_pc);
        c.Bind("debug_text", &ui->debug_text);
        c.Bind("lib_l1", &ui->lib_l1);
        c.Bind("lib_l2", &ui->lib_l2);
        c.Bind("libtile_cover", &ui->libtile_cover);
        c.Bind("note1", &ui->note1);
        c.Bind("note2", &ui->note2);
        c.Bind("note3", &ui->note3);
        c.Bind("ver", &ui->ver);
        c.Bind("t_back", &ui->t_back);
        c.Bind("cov_style", &ui->cov_style);
        c.Bind("ph_style", &ui->ph_style);
        c.Bind("touch_ui", &ui->touch_ui);

        c.BindEventCallback("pick_menu", [ui](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) act_pick_menu(ui, (int)args[0].Get<double>());
        });
        c.BindEventCallback("pick_game", [ui](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) act_pick_game(ui, (int)args[0].Get<double>());
        });
        c.BindEventCallback("row_activate", [ui](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &args) {
            if (!args.empty()) act_row(ui, (int)args[0].Get<double>());
        });
        c.BindEventCallback("nav_back", [ui](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
            nav_apply(ui, NAV_BACK);
        });
        c.BindEventCallback("open_settings", [ui](Rml::DataModelHandle, Rml::Event &, const Rml::VariantList &) {
            show_screen(ui, SNX_SCREEN_SETTINGS);
        });

        ui->model = c.GetModelHandle();
    }

    ui->ver = "v" SOLARNX_UI_VERSION;
    ui->s_ip = tr(Str_IpAddressPrefix);
    ui->s_layout = tr(Str_LayoutSwitch);
    ui->s_conn = tr(Str_MenuSettingsWifi);
    refresh_strings(ui);

    // --- documents ---
    ui->doc_menu = ui->ctx->LoadDocument("menu.rcml");
    ui->doc_lib = ui->ctx->LoadDocument("library.rcml");
    ui->doc_set = ui->ctx->LoadDocument("settings.rcml");
    ui->doc_stream = ui->ctx->LoadDocument("stream.rcml");
    if (!ui->doc_menu || !ui->doc_lib || !ui->doc_set)
        SDL_Log("snxui: a document failed to load (menu=%p lib=%p set=%p)",
                (void *)ui->doc_menu, (void *)ui->doc_lib, (void *)ui->doc_set);

    // --- background + starfield ---
    std::string bg_path = std::string(asset_root ? asset_root : "") +
                          (asset_root && asset_root[0] ? "/" : "") + "img/background.png";
    SDL_Surface *bg = IMG_Load(bg_path.c_str());
    if (bg == NULL)
        bg = IMG_Load_RW(SDL_RWFromFile("img/background.png", "rb"), 1);
    if (bg != NULL)
    {
        ui->background = SDL_CreateTextureFromSurface(renderer, bg);
        SDL_FreeSurface(bg);
    }
    SDL_Log("snxui: background texture %s (%s)", ui->background ? "ok" : "FAILED", bg_path.c_str());
    starfield_init(&ui->stars, renderer, ui->background, ui->width, ui->height);

    show_screen(ui, SNX_SCREEN_MENU);
    return ui;
}

void snxui_destroy(SnxUi *ui)
{
    if (ui == NULL)
        return;
    starfield_free(&ui->stars);
    if (ui->background)
        SDL_DestroyTexture(ui->background);
    if (ui->ctx)
        Rml::RemoveContext(ui->ctx->GetName());
    Rml::Shutdown();
    delete ui;
}

bool snxui_process_event(SnxUi *ui, const SDL_Event *ev)
{
    // Text editing owns all key/text events while active.
    if (ui->editing)
    {
        if (ev->type == SDL_TEXTINPUT)
        {
            ui->edit_buf += ev->text.text;
            rebuild_rows(ui);
            ui->model.DirtyVariable("setrows");
            return true;
        }
        if (ev->type == SDL_KEYDOWN)
        {
            if (ev->key.keysym.sym == SDLK_RETURN || ev->key.keysym.sym == SDLK_KP_ENTER)
            {
                edit_commit(ui);
                return true;
            }
            if (ev->key.keysym.sym == SDLK_ESCAPE || ev->key.keysym.sym == SDLK_AC_BACK)
            {
                edit_cancel(ui);
                return true;
            }
            if (ev->key.keysym.sym == SDLK_BACKSPACE && !ui->edit_buf.empty())
            {
                // step back one UTF-8 character
                size_t n = ui->edit_buf.size();
                do { n--; } while (n > 0 && ((uint8_t)ui->edit_buf[n] & 0xC0) == 0x80);
                ui->edit_buf.resize(n);
                rebuild_rows(ui);
                ui->model.DirtyVariable("setrows");
                return true;
            }
            return true;
        }
        if (ev->type == SDL_CONTROLLERBUTTONDOWN)
        {
            if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_A) edit_commit(ui);
            else if (ev->cbutton.button == SDL_CONTROLLER_BUTTON_B) edit_cancel(ui);
            return true;
        }
        if (ev->type == SDL_FINGERDOWN || ev->type == SDL_MOUSEBUTTONDOWN)
            edit_commit(ui); // the tap still reaches the element under it
        return false;
    }

    if (ev->type == SDL_KEYDOWN)
    {
        NavAction a = nav_from_key(ev->key.keysym.sym);
        if (a != NAV_NONE)
        {
            nav_apply(ui, a);
            return true;
        }
        return false;
    }
    // Switch between touch hints/buttons and gamepad hints based on the last input method.
    if (ev->type == SDL_FINGERDOWN || ev->type == SDL_MOUSEBUTTONDOWN)
        set_touch_ui(ui, true);
    else if (ev->type == SDL_CONTROLLERBUTTONDOWN ||
             (ev->type == SDL_KEYDOWN && nav_from_key(ev->key.keysym.sym) != NAV_NONE))
        set_touch_ui(ui, false);

    if (ev->type == SDL_CONTROLLERBUTTONDOWN)
    {
        NavAction a = nav_from_button(ev->cbutton.button);
        if (a != NAV_NONE)
        {
            nav_apply(ui, a);
            return true;
        }
        return false;
    }

    if (ev->type == SDL_WINDOWEVENT && ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
    {
        SDL_GetRendererOutputSize(ui->renderer, &ui->width, &ui->height);
        ui->ctx->SetDimensions(Rml::Vector2i(ui->width, ui->height));
        ui->ctx->SetDensityIndependentPixelRatio(ui->height / 720.0f);
        update_layout(ui);
        ui->model.DirtyVariable("cov_style");
        ui->model.DirtyVariable("ph_style");
        starfield_free(&ui->stars);
        starfield_init(&ui->stars, ui->renderer, ui->background, ui->width, ui->height);
        return true;
    }

    SDL_Event copy = *ev;
    RmlSDL::InputEventHandler(ui->ctx, ui->window, copy);
    return false;
}

void snxui_update(SnxUi *ui, float dt)
{
    starfield_update(&ui->stars, dt);
}

void snxui_render(SnxUi *ui)
{
    // RenderInterface_SDL::BeginFrame() clears the frame to black, so the
    // animated background must be drawn after it, before the RmlUi documents.
    ui->rdr->BeginFrame();
    starfield_draw(&ui->stars, ui->renderer);
    if (ui->underlay != nullptr)
        ui->underlay(ui->underlay_opaque, ui->renderer);

    ui->ctx->Update();
    ui->ctx->Render();
    ui->rdr->EndFrame();

    SDL_RenderPresent(ui->renderer);
}

void snxui_set_library(SnxUi *ui, const SnxUiGame *games, int count)
{
    ui->games.clear();
    ui->game_ids.clear();
    ui->files->ClearCovers();

    for (int i = 0; i < count; i++)
    {
        GameItem g;
        g.name = games[i].name ? games[i].name : "";
        g.has_cover = games[i].cover != NULL && games[i].cover_size > 0;
        g.gid = Rml::CreateString("%u", games[i].id);
        if (g.has_cover)
        {
            ui->files->SetCover(games[i].id, games[i].cover, games[i].cover_size);
            g.cover = Rml::CreateString("covers/%u.jpg", games[i].id);
        }
        ui->games.push_back(std::move(g));
        ui->game_ids.push_back(games[i].id);
    }
    if (ui->lib_sel >= count)
        ui->lib_sel = count > 0 ? count - 1 : 0;

    // "My games" tile shows the first cover, like the Switch client's drawLibraryIcon
    ui->libtile_cover = "";
    for (const auto &g : ui->games)
        if (g.has_cover) { ui->libtile_cover = g.cover; break; }

    refresh_strings(ui);
    ui->model.DirtyVariable("games");
    ui->model.DirtyVariable("libsel");
    ui->model.DirtyVariable("libtile_cover");
}

void snxui_set_status(SnxUi *ui, const char *ip_line, const char *layout_line,
                    const char *connection_line, const char *pc_line)
{
    if (ip_line) ui->s_ip = ip_line;
    if (layout_line) ui->s_layout = layout_line;
    if (connection_line) ui->s_conn = connection_line;
    if (pc_line) ui->s_pc = pc_line;
    ui->model.DirtyVariable("s_ip");
    ui->model.DirtyVariable("s_layout");
    ui->model.DirtyVariable("s_conn");
    ui->model.DirtyVariable("s_pc");
}

void snxui_show(SnxUi *ui, int screen)
{
    show_screen(ui, screen);
}

// Restores persisted values (platform storage) into the settings screen.
void snxui_set_host(SnxUi *ui, const char *address, int host_type)
{
    if (address != NULL)
        ui->address = address;
    if (host_type >= 0)
        ui->host_type = host_type ? 1 : 0;
    rebuild_rows(ui);
    ui->model.DirtyVariable("setrows");
}

// Multi-line core telemetry, shown as an overlay while non-empty (NULL hides it).
// The platform refreshes it from snx_core_stats() every few hundred milliseconds.
void snxui_set_debug(SnxUi *ui, const char *text)
{
    ui->debug_text = text != NULL ? text : "";
    ui->model.DirtyVariable("debug_text");
}

int snxui_screen(const SnxUi *ui)
{
    return ui->screen;
}

void snxui_set_underlay(SnxUi *ui, void (*draw)(void *opaque, SDL_Renderer *),
                        void *opaque)
{
    ui->underlay = draw;
    ui->underlay_opaque = opaque;
}

void snxui_text_result(SnxUi *ui, const char *row_key, const char *text)
{
    if (!row_key || !text)
        return; // NULL text = cancelled
    for (int row = 0; row < kRowCount; row++)
    {
        if (strcmp(kRowKey[row], row_key) != 0)
            continue;
        switch (row)
        {
        case ROW_ADDRESS:    ui->address = text; break;
        case ROW_SERVER:     ui->server = text; break;
        case ROW_SERVER_KEY: ui->server_key = text; break;
        case ROW_PC_ID:      ui->pc_id = text; break;
        default: return;
        }
        notify_row(ui, row, text);
        rebuild_rows(ui);
        ui->model.DirtyVariable("setrows");
        return;
    }
}

} // extern "C"
