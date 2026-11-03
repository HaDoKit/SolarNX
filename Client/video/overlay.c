#include "overlay.h"

#include <stdio.h>
#include <string.h>

#include <switch.h>
#include <SDL_ttf.h>

#include "../common/fontcache/SDL_FontCache.h"
#include "../common/lang/lang.h"
#include "../common/logger/logger.h"
#include "../control/control.h"
#include "../input/input.h"
#include "video.h"

// The overlay's own font, made for the video renderer the first time it is needed (the menu's
// font belongs to the menu's renderer, which is gone while streaming). Loading it preloads the
// glyph set, which takes a moment - so only in debug mode.
static FC_Font *s_font = NULL;
static bool s_fontFailed = false;
static bool s_plOpen = false;
static SDL_Renderer *s_fontRenderer = NULL;

static bool s_statsVisible = true; // the menu's first item - this session only

// Once-a-second figures.
static u64 s_windowStart = 0;
static uint32_t s_lastLost = 0, s_lastRecovered = 0;
static char s_lines[6][200];
static int s_lineCount = 0;

// Debug menu.
enum { ITEM_STATS, ITEM_BITRATE, ITEM_ADAPTIVE, ITEM_CODEC, ITEM_RESOLUTION, ITEM_FPS, ITEM_PRESET, ITEM_TUNE, ITEM_PRIORITY, ITEM_COUNT };
// The PC setting behind each item (ITEM_STATS is this side's own).
static const uint8_t kItemSetting[ITEM_COUNT] = {0, DebugSetting_BitrateKbps, DebugSetting_AdaptiveBitrate, DebugSetting_Codec,
    DebugSetting_Resolution, DebugSetting_Fps, DebugSetting_NvencPreset, DebugSetting_NvencTune, DebugSetting_TrafficPriority};
static int s_selected = 0;
static bool s_wasOpen = false;
// A change waits this long for the next one before going to the PC: each one restarts the encoder.
#define SEND_DELAY_NS 700000000ULL
static int s_pendingItem = -1;
static int32_t s_pendingValue = 0;
static u64 s_pendingTick = 0;
static bool s_pendingSent = false;
static uint32_t s_pendingSeq = 0; // the DebugInfo seq when it was sent - a newer one means applied

static const SDL_Color kWhite = {255, 255, 255, 255};
static const SDL_Color kDim = {170, 170, 170, 255};
static const SDL_Color kGold = {232, 184, 74, 255};

static bool ensureFont(SDL_Renderer *renderer)
{
    if (s_font != NULL && s_fontRenderer == renderer)
        return true;
    if (s_fontFailed)
        return false;
    overlayExit();

    TTF_Init();
    if (R_FAILED(plInitialize(PlServiceType_User)))
    {
        logWrite("overlay", "plInitialize failed - no debug overlay");
        s_fontFailed = true;
        return false;
    }
    s_plOpen = true;
    PlFontData fontData, fontExtData;
    plGetSharedFontByType(&fontData, PlSharedFontType_Standard);
    plGetSharedFontByType(&fontExtData, PlSharedFontType_NintendoExt);
    s_font = FC_CreateFont();
    if (s_font == NULL || !FC_LoadFont_RW(s_font, renderer,
            SDL_RWFromMem((void *)fontData.address, fontData.size),
            SDL_RWFromMem((void *)fontExtData.address, fontExtData.size),
            1, 40, FC_MakeColor(0, 0, 0, 255), TTF_STYLE_NORMAL))
    {
        logWrite("overlay", "font load failed - no debug overlay");
        s_fontFailed = true;
        overlayExit();
        return false;
    }
    s_fontRenderer = renderer;
    logWrite("overlay", "debug overlay font ready");
    return true;
}

void overlayExit(void)
{
    if (s_font != NULL)
        FC_FreeFont(s_font);
    s_font = NULL;
    s_fontRenderer = NULL;
    if (s_plOpen)
    {
        plExit();
        TTF_Quit();
        s_plOpen = false;
    }
}

bool overlayActive(void)
{
    return controlDebugEnabled();
}

static void text(SDL_Renderer *renderer, float x, float y, float scale, SDL_Color colour, const char *s)
{
    FC_DrawEffect(s_font, renderer, x, y, FC_MakeEffect(FC_ALIGN_LEFT, FC_MakeScale(scale, scale), colour), "%s", s);
}

static void box(SDL_Renderer *renderer, int x, int y, int w, int h)
{
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

static const char *codecName(int codec)
{
    return codec == 1 ? "H.264" : "H.265";
}

// Recomputes the statistics lines once a second.
static void updateStats(void)
{
    u64 now = armGetSystemTick();
    if (s_windowStart != 0 && armTicksToNs(now - s_windowStart) < 1000000000ULL)
        return;
    double seconds = s_windowStart == 0 ? 1.0 : (double)armTicksToNs(now - s_windowStart) / 1e9;
    s_windowStart = now;

    VideoDebugCounters c;
    videoTakeDebugCounters(&c);
    uint32_t lost = videoGetLostFrameCount(), recovered = videoGetRecoveredPacketCount();
    uint32_t lostDelta = lost - s_lastLost, recoveredDelta = recovered - s_lastRecovered;
    s_lastLost = lost;
    s_lastRecovered = recovered;

    ControlDebugInfo info;
    s_lineCount = 0;
    if (controlGetDebugInfo(&info))
    {
        char extra[48] = "";
        if (info.flags & DEBUG_INFO_NVENC)
            snprintf(extra, sizeof(extra), " · p%d/%s", info.preset + 1, kControlTunes[info.tune % CONTROL_TUNE_COUNT]);
        snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s · %s · %ux%u · %u fps%s",
            info.encoderName, codecName(info.codec), info.width, info.height, info.fps, extra);
    }
    else
    {
        snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s", tr(Str_DbgWaiting));
    }

    // "Frames/s: 60, lost: 0/s, packets rebuilt: 3/s"
    snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s%u%s%u%s%s%u%s",
        tr(Str_DbgFrames), (unsigned)(c.presents / seconds + 0.5),
        tr(Str_DbgLost), (unsigned)(lostDelta / seconds + 0.5), tr(Str_DbgPerSec),
        tr(Str_DbgRecovered), (unsigned)(recoveredDelta / seconds + 0.5), tr(Str_DbgPerSec));

    // "Bitrate: 12000 kbps   Decode: 4.2 ms"
    snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s%u%s   %s%.1f%s",
        tr(Str_DbgBitrate), videoGetMeasuredBitrateKbps(), tr(Str_DbgKbps),
        tr(Str_DbgDecode), c.decodeCount > 0 ? (double)c.decodeNsSum / c.decodeCount / 1e6 : 0.0, tr(Str_DbgMs));

    uint32_t rttUs = controlRttUs();
    double localMs = c.latencyCount > 0 ? (double)c.latencyNsSum / c.latencyCount / 1e6 : 0.0;
    if (rttUs > 0)
        snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s%.0f%s%s%.1f%s",
            tr(Str_DbgLatency), localMs + rttUs / 2000.0, tr(Str_DbgMs), tr(Str_DbgRtt), rttUs / 1000.0, tr(Str_DbgMs));
    else
        snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s%.0f%s", tr(Str_DbgLatency), localMs, tr(Str_DbgMs));
    snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s", tr(Str_DbgLatencyNote));

    char pads[160];
    inputDescribeControllers(pads, sizeof(pads));
    snprintf(s_lines[s_lineCount++], sizeof(s_lines[0]), "%s%s", tr(Str_DbgControllers), pads[0] ? pads : "-");
}

// --- Menu ---

static int32_t currentValue(int item, const ControlDebugInfo *info)
{
    if (item == s_pendingItem)
        return s_pendingValue;
    switch (item)
    {
    case ITEM_STATS:      return s_statsVisible;
    case ITEM_BITRATE:    return (int32_t)info->bitrateKbps;
    case ITEM_ADAPTIVE:   return (info->flags & DEBUG_INFO_ADAPTIVE) != 0;
    case ITEM_CODEC:      return info->codecSetting;
    case ITEM_RESOLUTION: return info->resolution;
    case ITEM_FPS:        return info->fps;
    case ITEM_PRESET:     return info->preset;
    case ITEM_TUNE:       return info->tune;
    case ITEM_PRIORITY:   return (info->flags & DEBUG_INFO_PRIORITY) != 0;
    default:              return 0;
    }
}

static int fpsIndex(int fps)
{
    for (int i = 0; i < CONTROL_FPS_COUNT; i++)
        if (kControlFps[i] == fps)
            return i;
    return CONTROL_FPS_COUNT - 1;
}

// -1 / +1 on the selected item.
static void change(int item, int direction, const ControlDebugInfo *info)
{
    int32_t value = currentValue(item, info);
    switch (item)
    {
    case ITEM_STATS:
        s_statsVisible = !s_statsVisible;
        return; // local - nothing to send
    case ITEM_BITRATE:
        value += direction * 1000;
        if (value < 1000) value = 1000;
        if (value > 50000) value = 50000;
        break;
    case ITEM_ADAPTIVE:
    case ITEM_CODEC:
    case ITEM_PRIORITY:
        value = !value;
        break;
    case ITEM_RESOLUTION:
        value = (value + direction + CONTROL_RESOLUTION_COUNT + 1) % (CONTROL_RESOLUTION_COUNT + 1);
        break;
    case ITEM_FPS:
        value = kControlFps[(fpsIndex(value) + direction + CONTROL_FPS_COUNT) % CONTROL_FPS_COUNT];
        break;
    case ITEM_PRESET:
        if (!(info->flags & DEBUG_INFO_NVENC)) return;
        value += direction;
        if (value < 0) value = 0;
        if (value >= CONTROL_PRESET_COUNT) value = CONTROL_PRESET_COUNT - 1;
        break;
    case ITEM_TUNE:
        if (!(info->flags & DEBUG_INFO_NVENC)) return;
        value = (value + direction + CONTROL_TUNE_COUNT) % CONTROL_TUNE_COUNT;
        break;
    default:
        return;
    }
    if (s_pendingItem >= 0 && s_pendingItem != item && !s_pendingSent)
        controlSendSetting(kItemSetting[s_pendingItem], s_pendingValue); // another item: send the waiting change now
    s_pendingItem = item;
    s_pendingValue = value;
    s_pendingTick = armGetSystemTick();
    s_pendingSent = false;
}

static void flushPending(const ControlDebugInfo *info)
{
    if (s_pendingItem < 0)
        return;
    if (!s_pendingSent && armTicksToNs(armGetSystemTick() - s_pendingTick) >= SEND_DELAY_NS)
    {
        controlSendSetting(kItemSetting[s_pendingItem], s_pendingValue);
        s_pendingSent = true;
        s_pendingSeq = info->seq;
        s_pendingTick = armGetSystemTick();
    }
    // Applied once the PC's answer (a newer DebugInfo) arrived - or given up on after a while.
    if (s_pendingSent && (info->seq != s_pendingSeq || armTicksToNs(armGetSystemTick() - s_pendingTick) > 5000000000ULL))
        s_pendingItem = -1;
}

static void valueText(int item, const ControlDebugInfo *info, char *out, size_t size)
{
    int32_t v = currentValue(item, info);
    switch (item)
    {
    case ITEM_STATS:
    case ITEM_ADAPTIVE:
    case ITEM_PRIORITY:
        snprintf(out, size, "%s", tr(v ? Str_On : Str_Off));
        break;
    case ITEM_BITRATE:
        snprintf(out, size, "%d%s", (int)v, tr(Str_DbgKbps));
        break;
    case ITEM_CODEC:
        snprintf(out, size, "%s", codecName(v));
        break;
    case ITEM_RESOLUTION:
        if (v <= 0 || v > CONTROL_RESOLUTION_COUNT)
            snprintf(out, size, "%s", tr(Str_DbgAuto));
        else
            snprintf(out, size, "%dx%d", kControlResolutions[v - 1][0], kControlResolutions[v - 1][1]);
        break;
    case ITEM_FPS:
        snprintf(out, size, "%d", (int)v);
        break;
    case ITEM_PRESET:
        snprintf(out, size, "p%d%s", (int)v + 1, (info->flags & DEBUG_INFO_NVENC) ? "" : tr(Str_DbgNvencOnly));
        break;
    case ITEM_TUNE:
        snprintf(out, size, "%s%s", kControlTunes[(v % CONTROL_TUNE_COUNT + CONTROL_TUNE_COUNT) % CONTROL_TUNE_COUNT],
            (info->flags & DEBUG_INFO_NVENC) ? "" : tr(Str_DbgNvencOnly));
        break;
    default:
        out[0] = '\0';
    }
    if (item == s_pendingItem && item != ITEM_STATS)
    {
        size_t used = strlen(out);
        snprintf(out + used, size - used, "%s", tr(Str_DbgApplying));
    }
}

static void drawMenu(SDL_Renderer *renderer, float s, const ControlDebugInfo *info)
{
    static const StrId labels[ITEM_COUNT] = {Str_DbgShowStats, Str_DbgBitrateItem, Str_DbgAdaptive, Str_DbgCodec,
        Str_DbgResolution, Str_DbgFps, Str_DbgPreset, Str_DbgTune, Str_DbgPriority};
    float x = 380 * s, y = 150 * s, lineH = 34 * s;
    box(renderer, (int)(x - 24 * s), (int)(y - 20 * s), (int)(560 * s), (int)(lineH * (ITEM_COUNT + 3) + 30 * s));
    text(renderer, x, y, 0.75f * s, kWhite, tr(Str_DbgMenuTitle));
    y += lineH * 1.4f;
    for (int i = 0; i < ITEM_COUNT; i++)
    {
        char value[96], line[200];
        valueText(i, info, value, sizeof(value));
        snprintf(line, sizeof(line), "%s: %s", tr(labels[i]), value);
        text(renderer, x, y, 0.6f * s, i == s_selected ? kGold : kWhite, line);
        y += lineH;
    }
    y += lineH * 0.4f;
    text(renderer, x, y, 0.5f * s, kDim, tr(Str_DbgMenuHelp));
}

static void handleMenu(const ControlDebugInfo *info)
{
    uint64_t down = inputTakeDebugMenuButtons();
    if (down & (HidNpadButton_B))
    {
        inputCloseDebugMenu();
        return;
    }
    if (down & HidNpadButton_AnyUp)
        s_selected = (s_selected + ITEM_COUNT - 1) % ITEM_COUNT;
    if (down & HidNpadButton_AnyDown)
        s_selected = (s_selected + 1) % ITEM_COUNT;
    if (!info->valid && s_selected != ITEM_STATS)
        return; // nothing known about the PC's settings yet
    if (down & (HidNpadButton_AnyRight | HidNpadButton_A))
        change(s_selected, +1, info);
    else if (down & HidNpadButton_AnyLeft)
        change(s_selected, -1, info);
}

void overlayDraw(SDL_Renderer *renderer)
{
    if (!controlDebugEnabled())
    {
        s_windowStart = 0;
        return;
    }
    bool menuOpen = inputDebugMenuOpen();
    if (!s_statsVisible && !menuOpen && s_pendingItem < 0)
        return;
    if (!ensureFont(renderer))
        return;

    int w = 1280, h = 720;
    SDL_GetRendererOutputSize(renderer, &w, &h);
    float s = h / 720.0f;

    ControlDebugInfo info;
    controlGetDebugInfo(&info);
    if (menuOpen)
    {
        if (!s_wasOpen)
            inputTakeDebugMenuButtons(); // whatever was pressed with the chord
        handleMenu(&info);
    }
    s_wasOpen = menuOpen;
    flushPending(&info);

    if (s_statsVisible)
    {
        updateStats();
        float lineH = 26 * s;
        box(renderer, (int)(10 * s), (int)(10 * s), (int)(900 * s), (int)(lineH * (s_lineCount + 1) + 12 * s));
        float y = 16 * s;
        for (int i = 0; i < s_lineCount; i++)
        {
            text(renderer, 20 * s, y, 0.45f * s, i == 4 ? kDim : kWhite, s_lines[i]); // 4: the latency note
            y += lineH;
        }
        text(renderer, 20 * s, y, 0.4f * s, kDim, tr(Str_DbgMenuHint));
    }
    if (menuOpen)
        drawMenu(renderer, s, &info);
}
