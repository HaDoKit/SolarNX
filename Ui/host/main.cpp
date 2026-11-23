// Desktop preview host for libsolarnx-ui: an SDL window running the same shell as the device
// clients, with a mock game library.
// Usage: snxui-host [--assets DIR] [--empty] [--shot FILE [FRAMES]] [--screen N]

#include <SDL2/SDL.h>
#include <SDL_image.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "snxui.h"

static std::vector<uint8_t> read_file(const std::string &path)
{
    SDL_RWops *rw = SDL_RWFromFile(path.c_str(), "rb");
    if (rw == NULL)
        return {};
    std::vector<uint8_t> buf((size_t)SDL_RWsize(rw));
    SDL_RWread(rw, buf.data(), 1, buf.size());
    SDL_RWclose(rw);
    return buf;
}

static bool running = true;

static void on_pick(void *, int target, uint32_t id)
{
    const char *names[] = {"Steam Big Picture", "shadPS4", "library game"};
    SDL_Log("pick: %s%s", names[target], target == SNX_TARGET_LIBRARY ? "" : "");
    if (target == SNX_TARGET_LIBRARY)
        SDL_Log("  library id: %u", id);
}

static void on_quit(void *)
{
    running = false;
}

static void on_setting(void *, const char *key, const char *value)
{
    SDL_Log("setting: %s = %s", key, value);
}

int main(int argc, char **argv)
{
    std::string assets = "Ui/assets";
    std::string shot;
    int shot_frames = 30;
    int start_screen = -1;
    bool demo_library = true;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--assets" && i + 1 < argc)
            assets = argv[++i];
        else if (a == "--empty")
            demo_library = false;
        else if (a == "--shot" && i + 1 < argc)
        {
            shot = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                shot_frames = atoi(argv[++i]);
        }
        else if (a == "--screen" && i + 1 < argc)
            start_screen = atoi(argv[++i]);
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        SDL_Log("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);

    SDL_Window *window = SDL_CreateWindow("SolarNX (ui host)", SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, 1280, 720,
                                        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (window == NULL || renderer == NULL)
    {
        SDL_Log("window/renderer: %s", SDL_GetError());
        return 1;
    }

    SnxUiCallbacks cb = {on_pick, on_quit, on_setting, NULL};
    SnxUi *ui = snxui_create(window, renderer, assets.c_str(), &cb);
    if (ui == NULL)
    {
        SDL_Log("snxui_create failed");
        return 1;
    }

    if (demo_library)
    {
        // Mock library: names + real cover art, like a real host push.
        const char *names[] = {
            "Cyberpunk 2077", "Elden Ring", "Hollow Knight", "The Witcher 3",
            "Bloodborne (shadPS4)", "Baldur's Gate 3", "Hades", "Stardew Valley",
            "Factorio", "A Very Long Game Title To Check Clipping",
        };
        const char *covers[] = {
            "cyberpunk2077.jpg", "eldenring.jpg", "hollowknight.jpg", "witcher3.jpg",
            NULL, "baldursgate3.jpg", "hades.jpg", "stardewvalley.jpg",
            "factorio.jpg", NULL,
        };
        std::vector<SnxUiGame> games(10);
        std::vector<std::vector<uint8_t>> bytes(10);
        for (int i = 0; i < 10; i++)
        {
            games[i].id = 100 + i;
            games[i].name = names[i];
            if (covers[i] != NULL) // Bloodborne + long-title tiles stay coverless on purpose
                bytes[i] = read_file(assets + "/img/covers/" + covers[i]);
            games[i].cover = bytes[i].empty() ? NULL : bytes[i].data();
            games[i].cover_size = (uint32_t)bytes[i].size();
        }
        snxui_set_library(ui, games.data(), (int)games.size());
    }

    snxui_set_status(ui, "IP Address: 192.168.1.42", NULL, NULL, "PC: SolarNX 0.2.5-alpha");
    if (start_screen >= 0)
        snxui_show(ui, start_screen);

    Uint64 last = SDL_GetTicks64();
    int frames = 0;
    while (running)
    {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
        {
            if (ev.type == SDL_QUIT)
                running = false;
            else
                snxui_process_event(ui, &ev);
        }
        Uint64 now = SDL_GetTicks64();
        float dt = (float)(now - last) / 1000.0f;
        if (dt > 1.0f) dt = 1.0f;
        last = now;
        snxui_update(ui, dt);
        snxui_render(ui);

        if (!shot.empty() && ++frames >= shot_frames)
        {
            int w, h;
            SDL_GetRendererOutputSize(renderer, &w, &h);
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
            if (s != NULL && SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
                                                  s->pixels, s->pitch) == 0)
            {
                SDL_SaveBMP(s, shot.c_str());
                SDL_Log("screenshot: %s (%dx%d)", shot.c_str(), w, h);
            }
            SDL_FreeSurface(s);
            break;
        }
    }

    snxui_destroy(ui);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
