#include "input.h"

#include <string.h>
#include <stdatomic.h>

#include <switch.h>

#include "../common/logger/logger.h"
#include "../video/video.h"
#include "../audio/audio.h"
#include "../common/appstate/appstate.h"
#include "../common/lang/lang.h"
#include "../control/control.h"
#include "../settings/settings.h"

#include <stdio.h>

#define INPUT_MAX_CONTROLLERS 4

static PadState s_pads[INPUT_MAX_CONTROLLERS];
static SolarNxAppTarget s_appTarget = SolarNxAppTarget_BigPicture;
static uint32_t s_libraryEntryId = 0;

void inputSetAppTarget(SolarNxAppTarget target, uint32_t libraryEntryId)
{
    s_appTarget = target;
    s_libraryEntryId = target == SolarNxAppTarget_Library ? libraryEntryId : 0;
}

SolarNxAppTarget inputGetAppTarget(void)
{
    return s_appTarget;
}

static const HidNpadIdType s_npadIds[INPUT_MAX_CONTROLLERS] = {
    HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
};

// Each controller carries its own motion reading (see network.h's SolarNxControllerState), but a
// given controller only ever reports ONE style at a time (FullKey xor JoyDual-Left xor
// JoyDual-Right) - starting all 3 possible handles per player regardless of which is actually
// current was confirmed live to cause growing CPU pressure with even 1-2 controllers connected
// (audio packet-loss concealment became near-continuous, then the app crashed). This tracks which
// single handle is the *current* one per player and keeps only that one running, switching if the
// controller's style changes at runtime (e.g. a Joy-Con pair splitting apart). Worst case with all
// 4 players connected is now at most 4 active non-handheld sensors (one per player) instead of 12.
static HidSixAxisSensorHandle s_sixaxisHandheld; // player 1 only, always running (a single handle)
static HidSixAxisSensorHandle s_sixaxisFullKey[INPUT_MAX_CONTROLLERS];
static HidSixAxisSensorHandle s_sixaxisJoyDualLeft[INPUT_MAX_CONTROLLERS];
static HidSixAxisSensorHandle s_sixaxisJoyDualRight[INPUT_MAX_CONTROLLERS];

typedef enum {
    SixaxisSlot_None,
    SixaxisSlot_FullKey,
    SixaxisSlot_JoyDualLeft,
    SixaxisSlot_JoyDualRight,
} SixaxisSlot;

// Which non-handheld handle is currently started for player i, if any (SixaxisSlot_None = none
// started right now).
static SixaxisSlot s_sixaxisSlot[INPUT_MAX_CONTROLLERS];

// Holding Minus (alone - Minus+Plus is the host's Guide/Steam-button chord) this long on any
// controller leaves the stream for the app-selection menu - see appRequestMenu. 3s for every target,
// per the user's explicit choice (it started at 15s for Steam/shadPS4; that was too slow to be
// usable). Trade-off: a game that wants Minus/Select held for 3s+ (map, scoreboard) will trip it.
#define INPUT_MENU_HOLD_NS 3000000000ULL
static u64 s_minusHeldSinceTick = 0; // 0 = Minus not currently held alone
static bool s_menuHoldNeedsRelease = false; // already fired - wait for a release before re-arming

// Rumble (HD rumble): two vibration devices per controller style - left and right Joy-Con, or the
// Pro Controller's two motors. Handles for every style are fetched once; inputRead() sends to the
// style the player has right now. The levels arrive from the PC on another thread, so they sit in
// atomics: (large << 8) | small per player.
typedef enum { RumbleStyle_None, RumbleStyle_Handheld, RumbleStyle_FullKey, RumbleStyle_JoyDual } RumbleStyle;
static HidVibrationDeviceHandle s_vibHandheld[2];
static HidVibrationDeviceHandle s_vibFullKey[INPUT_MAX_CONTROLLERS][2];
static HidVibrationDeviceHandle s_vibJoyDual[INPUT_MAX_CONTROLLERS][2];
static bool s_vibReady = false;
static atomic_uint s_rumbleTarget[INPUT_MAX_CONTROLLERS];
static uint32_t s_rumbleApplied[INPUT_MAX_CONTROLLERS];
static RumbleStyle s_rumbleStyle[INPUT_MAX_CONTROLLERS];

// HD rumble at full amplitude is harsh on a Joy-Con; XInput's 255 is "full", so it is scaled to this.
#define RUMBLE_MAX_AMPLITUDE 0.8f
// An Xbox pad's small motor is a tiny one - a faint buzz even at 255. Games lean on that: footsteps in
// Phantom Blade Zero are the small motor alone, 0..134 every 20ms, which at full HD-rumble strength
// on the high band felt like a constant tremor. Scaled down to match how it feels on the original.
#define RUMBLE_SMALL_MOTOR_SCALE 0.35f
// Hz. The Switch's own defaults for its two bands are 160/320 - at those the Joy-Con's actuator is
// audible: with a game changing the level every 20ms (Phantom Blade Zero's footsteps) it whined.
// An Xbox pad's motors spin far slower; down here it feels like them and stays quiet.
#define RUMBLE_FREQ_LOW  80.0f
#define RUMBLE_FREQ_HIGH 160.0f


// Debug menu (debug mode only): the chord, the presses handed to the menu, and per player the
// style for the overlay. The menu itself lives in video/overlay.c on the main thread.
#define DEBUG_CHORD (HidNpadButton_Minus | HidNpadButton_Plus | HidNpadButton_ZL | HidNpadButton_ZR)
static atomic_bool s_debugMenuOpen = false;
static atomic_ullong s_debugMenuDown = 0;
static u64 s_debugPrevButtons = 0;
static bool s_debugChordLatched = false;
// After the menu closes, whatever is still held (B, the chord) must not reach the game.
static bool s_debugMenuWasOpen = false;
static bool s_holdUntilReleased = false;
static atomic_ullong s_playerStyle[INPUT_MAX_CONTROLLERS]; // padGetStyleSet, 0 = not connected
static atomic_uint s_playerAttributes[INPUT_MAX_CONTROLLERS];

// The host turns Minus + Plus into Guide/PS (its GuideButtonChord) the moment it sees both. In debug
// mode they are held back here for a moment, and not sent at all while ZL + ZR are held, so the
// debug chord never reaches the host as a Guide press. A quick tap still goes through, a little late.
#define DEBUG_MINUS_PLUS_HOLD_NS 100000000ULL // 100ms
#define DEBUG_TAP_PACKETS 3
static u64 s_mpSinceTick[INPUT_MAX_CONTROLLERS];
static u64 s_mpLastBits[INPUT_MAX_CONTROLLERS];
static int s_mpTapPackets[INPUT_MAX_CONTROLLERS];

static u64 filterMinusPlus(int i, u64 buttons, u64 now)
{
    const u64 mp = HidNpadButton_Minus | HidNpadButton_Plus;
    u64 held = buttons & mp;
    bool triggers = (buttons & HidNpadButton_ZL) && (buttons & HidNpadButton_ZR);
    u64 out = buttons & ~mp;

    if (s_mpTapPackets[i] > 0)
    {
        s_mpTapPackets[i]--;
        out |= s_mpLastBits[i];
    }
    if (held == 0)
    {
        // Released before the hold time was up and not part of the chord: deliver it as a tap now.
        if (s_mpSinceTick[i] != 0 && s_mpLastBits[i] != 0 && armTicksToNs(now - s_mpSinceTick[i]) < DEBUG_MINUS_PLUS_HOLD_NS)
            s_mpTapPackets[i] = DEBUG_TAP_PACKETS;
        s_mpSinceTick[i] = 0;
        return out;
    }
    if (triggers)
    {
        s_mpSinceTick[i] = 0; // part of the debug chord - never a tap
        s_mpLastBits[i] = 0;
        return out;
    }
    if (s_mpSinceTick[i] == 0)
        s_mpSinceTick[i] = now;
    s_mpLastBits[i] = held;
    if (armTicksToNs(now - s_mpSinceTick[i]) >= DEBUG_MINUS_PLUS_HOLD_NS)
        out |= held;
    return out;
}

bool inputDebugMenuOpen(void)
{
    return atomic_load(&s_debugMenuOpen);
}

void inputCloseDebugMenu(void)
{
    atomic_store(&s_debugMenuOpen, false);
}

uint64_t inputTakeDebugMenuButtons(void)
{
    return atomic_exchange(&s_debugMenuDown, 0);
}

void inputDescribeControllers(char *out, size_t size)
{
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < INPUT_MAX_CONTROLLERS && used + 1 < size; i++)
    {
        u64 style = atomic_load(&s_playerStyle[i]);
        if (style == 0)
            continue;
        u32 attributes = atomic_load(&s_playerAttributes[i]);
        const char *name =
            (i == 0 && (style & HidNpadStyleTag_NpadHandheld)) ? tr(Str_PadHandheld)
            : (style & HidNpadStyleTag_NpadFullKey) ? "Pro Controller"
            : (style & HidNpadStyleTag_NpadGc) ? "GameCube"
            : (style & HidNpadStyleTag_NpadJoyDual) ? ((attributes & HidNpadAttribute_IsLeftConnected) && (attributes & HidNpadAttribute_IsRightConnected)
                ? "Joy-Con (L+R)" : (attributes & HidNpadAttribute_IsLeftConnected) ? "Joy-Con (L)" : "Joy-Con (R)")
            : (style & HidNpadStyleTag_NpadJoyLeft) ? "Joy-Con (L)"
            : (style & HidNpadStyleTag_NpadJoyRight) ? "Joy-Con (R)"
            : tr(Str_PadOther);
        int n = snprintf(out + used, size - used, "%s%d: %s", used == 0 ? "" : ", ", i + 1, name);
        if (n < 0)
            break;
        used += (size_t)n < size - used ? (size_t)n : size - used - 1;
    }
}

static void configurePads(void)
{
    padConfigureInput(INPUT_MAX_CONTROLLERS, HidNpadStyleSet_NpadStandard);

    // Player 1 combines No1 (a physical/paired controller) and Handheld (built-in Joy-Cons) into
    // one PadState, matching padInitializeDefault's own convention - whichever is actually active
    // is what padGetButtons/padGetStickPos read from. Players 2-4 have no handheld equivalent.
    padInitialize(&s_pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
    padInitialize(&s_pads[1], HidNpadIdType_No2);
    padInitialize(&s_pads[2], HidNpadIdType_No3);
    padInitialize(&s_pads[3], HidNpadIdType_No4);
}

void inputReconfigure(void)
{
    configurePads();
    s_minusHeldSinceTick = 0;
    s_menuHoldNeedsRelease = true; // whatever is still held from the menu must be let go first
}

static void updateMenuHold(bool minusHeldAlone)
{
    if (!minusHeldAlone)
    {
        s_minusHeldSinceTick = 0;
        s_menuHoldNeedsRelease = false;
        return;
    }
    if (s_menuHoldNeedsRelease)
        return;

    u64 now = armGetSystemTick();
    if (s_minusHeldSinceTick == 0)
    {
        s_minusHeldSinceTick = now;
        return;
    }
    if (armTicksToNs(now - s_minusHeldSinceTick) >= INPUT_MENU_HOLD_NS)
    {
        logWrite("input", "Minus held for %llus - returning to the app-selection menu", INPUT_MENU_HOLD_NS / 1000000000ULL);
        appRequestMenu();
        s_menuHoldNeedsRelease = true;
    }
}

bool inputInit(void)
{
    configurePads();

    hidInitializeTouchScreen();

    hidGetSixAxisSensorHandles(&s_sixaxisHandheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidStartSixAxisSensor(s_sixaxisHandheld);

    for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++)
    {
        // Handles are fetched up front (cheap, no ongoing cost) - only hidStartSixAxisSensor
        // (below, lazily, one at a time per player) has an ongoing resource cost.
        hidGetSixAxisSensorHandles(&s_sixaxisFullKey[i], 1, s_npadIds[i], HidNpadStyleTag_NpadFullKey);

        // JoyDual style yields two handles (left + right Joy-Con) from one call, in that order -
        // matches the devkitPro sixaxis example.
        HidSixAxisSensorHandle joyDualHandles[2];
        hidGetSixAxisSensorHandles(joyDualHandles, 2, s_npadIds[i], HidNpadStyleTag_NpadJoyDual);
        s_sixaxisJoyDualLeft[i] = joyDualHandles[0];
        s_sixaxisJoyDualRight[i] = joyDualHandles[1];
    }

    Result rc = hidInitializeVibrationDevices(s_vibHandheld, 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    for (int i = 0; i < INPUT_MAX_CONTROLLERS && R_SUCCEEDED(rc); i++)
    {
        rc = hidInitializeVibrationDevices(s_vibFullKey[i], 2, s_npadIds[i], HidNpadStyleTag_NpadFullKey);
        if (R_SUCCEEDED(rc))
            rc = hidInitializeVibrationDevices(s_vibJoyDual[i], 2, s_npadIds[i], HidNpadStyleTag_NpadJoyDual);
    }
    s_vibReady = R_SUCCEEDED(rc);
    if (!s_vibReady)
        logWrite("input", "vibration devices unavailable (0x%x) - no rumble", rc);

    logWrite("input", "pad/touch/sixaxis ready for up to %d controllers", INPUT_MAX_CONTROLLERS);
    return true;
}

void inputSetRumble(int player, uint8_t large, uint8_t small)
{
    if (player < 0 || player >= INPUT_MAX_CONTROLLERS)
        return;
    atomic_store(&s_rumbleTarget[player], ((unsigned)large << 8) | small);
}

static RumbleStyle currentRumbleStyle(int i)
{
    if (!padIsConnected(&s_pads[i]))
        return RumbleStyle_None;
    u64 styleSet = padGetStyleSet(&s_pads[i]);
    if (i == 0 && (styleSet & HidNpadStyleTag_NpadHandheld))
        return RumbleStyle_Handheld;
    if (styleSet & HidNpadStyleTag_NpadFullKey)
        return RumbleStyle_FullKey;
    if (styleSet & HidNpadStyleTag_NpadJoyDual)
        return RumbleStyle_JoyDual;
    return RumbleStyle_None;
}

static const HidVibrationDeviceHandle *rumbleHandles(int i, RumbleStyle style)
{
    switch (style)
    {
    case RumbleStyle_Handheld: return s_vibHandheld;
    case RumbleStyle_FullKey:  return s_vibFullKey[i];
    case RumbleStyle_JoyDual:  return s_vibJoyDual[i];
    default:                   return NULL;
    }
}

static void sendRumble(int i, RumbleStyle style, uint32_t levels)
{
    const HidVibrationDeviceHandle *handles = rumbleHandles(i, style);
    if (handles == NULL)
        return;
    HidVibrationValue value = {
        .amp_low = (float)((levels >> 8) & 0xFF) / 255.0f * RUMBLE_MAX_AMPLITUDE,
        .freq_low = RUMBLE_FREQ_LOW,
        .amp_high = (float)(levels & 0xFF) / 255.0f * RUMBLE_MAX_AMPLITUDE * RUMBLE_SMALL_MOTOR_SCALE,
        .freq_high = RUMBLE_FREQ_HIGH,
    };
    HidVibrationValue values[2] = {value, value};
    hidSendVibrationValues(handles, values, 2);
}

// Sends what changed: new levels from the PC, or a player whose controller changed style (e.g. the
// Joy-Cons went into the console) - the old devices are stopped, the new ones get the current level.
static void applyRumble(void)
{
    if (!s_vibReady)
        return;
    SettingsRumble mode = settingsGetRumble(); // off on this console: everything stays (or goes) still
    for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++)
    {
        RumbleStyle style = currentRumbleStyle(i);
        uint32_t target = mode == SettingsRumble_Off ? 0 : atomic_load(&s_rumbleTarget[i]);
        if (mode == SettingsRumble_LargeOnly)
            target &= 0xFF00u; // the small motor's byte gone
        if (style != s_rumbleStyle[i])
        {
            if (s_rumbleApplied[i] != 0)
                sendRumble(i, s_rumbleStyle[i], 0);
            s_rumbleStyle[i] = style;
            s_rumbleApplied[i] = 0;
        }
        if (target != s_rumbleApplied[i])
        {
            sendRumble(i, style, target);
            s_rumbleApplied[i] = target;
        }
    }
}

void inputStopRumble(void)
{
    for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++)
        atomic_store(&s_rumbleTarget[i], 0);
    applyRumble();
}

static HidSixAxisSensorHandle sixaxisHandleForSlot(int i, SixaxisSlot slot)
{
    switch (slot)
    {
        case SixaxisSlot_JoyDualLeft:  return s_sixaxisJoyDualLeft[i];
        case SixaxisSlot_JoyDualRight: return s_sixaxisJoyDualRight[i];
        case SixaxisSlot_FullKey:
        case SixaxisSlot_None:
        default:                       return s_sixaxisFullKey[i];
    }
}

// Figures out which single non-handheld handle should be active for player i right now (or None),
// and starts/stops sensors so exactly that one (and no other) is running for this player. Player
// 1's handheld style is handled separately (see s_sixaxisHandheld) and never touches this slot.
static void updateSixaxisSlot(int i)
{
    SixaxisSlot desired = SixaxisSlot_None;

    if (padIsConnected(&s_pads[i]))
    {
        u64 styleSet = padGetStyleSet(&s_pads[i]);
        bool isHandheld = (i == 0) && (styleSet & HidNpadStyleTag_NpadHandheld);

        if (!isHandheld)
        {
            if (styleSet & HidNpadStyleTag_NpadFullKey)
            {
                desired = SixaxisSlot_FullKey;
            }
            else if (styleSet & HidNpadStyleTag_NpadJoyDual)
            {
                u64 attrib = padGetAttributes(&s_pads[i]);
                if (attrib & HidNpadAttribute_IsLeftConnected)
                    desired = SixaxisSlot_JoyDualLeft;
                else if (attrib & HidNpadAttribute_IsRightConnected)
                    desired = SixaxisSlot_JoyDualRight;
            }
        }
    }

    if (desired == s_sixaxisSlot[i])
        return;

    if (s_sixaxisSlot[i] != SixaxisSlot_None)
        hidStopSixAxisSensor(sixaxisHandleForSlot(i, s_sixaxisSlot[i]));

    if (desired != SixaxisSlot_None)
        hidStartSixAxisSensor(sixaxisHandleForSlot(i, desired));

    logWrite("input", "player %d sixaxis slot %d -> %d", i + 1, s_sixaxisSlot[i], desired);
    s_sixaxisSlot[i] = desired;
}

// Reads player i's motion sensor into *pkt's per-controller accel/gyro fields, from whichever
// handle updateSixaxisSlot (called just before this, in inputRead) determined is current. No-op
// (leaves them zeroed) if nothing is connected/readable.
static void readSixaxis(int i, SolarNxControllerState *dst)
{
    HidSixAxisSensorState sixaxis = {0};
    bool got = false;

    if (i == 0 && (padGetStyleSet(&s_pads[0]) & HidNpadStyleTag_NpadHandheld))
        got = hidGetSixAxisSensorStates(s_sixaxisHandheld, &sixaxis, 1) > 0;
    else if (s_sixaxisSlot[i] != SixaxisSlot_None)
        got = hidGetSixAxisSensorStates(sixaxisHandleForSlot(i, s_sixaxisSlot[i]), &sixaxis, 1) > 0;

    if (!got)
        return;

    // libnx reports acceleration in g and angular_velocity in rad/s. The host's DS4 output path
    // (VirtualGamepad.cs's ApplyDs4State) multiplies gyro by 180/pi to get deg/s and accel by a
    // raw-units-per-g constant - both imply the wire values are expected in rad/s and g
    // respectively, so these are sent as-is with no conversion.
    dst->accelX = sixaxis.acceleration.x;
    dst->accelY = sixaxis.acceleration.y;
    dst->accelZ = sixaxis.acceleration.z;
    dst->gyroX = sixaxis.angular_velocity.x;
    dst->gyroY = sixaxis.angular_velocity.y;
    dst->gyroZ = sixaxis.angular_velocity.z;
}

void inputRead(SolarNxHidPacket *pkt)
{
    memset(pkt, 0, sizeof(*pkt));

    u32 connectedCount = 0;
    bool minusHeldAlone = false;
    bool debug = controlDebugEnabled();
    if (!debug)
        atomic_store(&s_debugMenuOpen, false);
    u64 now = armGetSystemTick();
    u64 allButtons = 0;
    bool chordHeld = false;
    for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++)
    {
        padUpdate(&s_pads[i]);
        updateSixaxisSlot(i);

        if (!padIsConnected(&s_pads[i]))
        {
            atomic_store(&s_playerStyle[i], 0);
            continue;
        }
        atomic_store(&s_playerStyle[i], padGetStyleSet(&s_pads[i]));
        atomic_store(&s_playerAttributes[i], (u32)padGetAttributes(&s_pads[i]));

        connectedCount++;

        // libnx's HidNpadButton bit positions (A=0,B=1,X=2,Y=3,StickL=4,StickR=5,L=6,R=7,ZL=8,
        // ZR=9,Plus=10,Minus=11,Left=12,Up=13,Right=14,Down=15) match the wire format's heldKeys
        // bit-for-bit (see SolarNX/Input/HidProtocol.cs's ControllerState.IsSet) - no remapping,
        // just mask to the low 16 bits the host actually reads (HeldKeys is a C# ushort).
        u64 buttons = padGetButtons(&s_pads[i]);
        allButtons |= buttons;
        if ((buttons & DEBUG_CHORD) == DEBUG_CHORD)
            chordHeld = true;
        u64 sent = debug ? filterMinusPlus(i, buttons, now) : buttons;
        pkt->controllers[i].heldKeys = (uint32_t)(sent & 0xFFFFu);
        if ((buttons & HidNpadButton_Minus) && !(buttons & HidNpadButton_Plus))
            minusHeldAlone = true;

        HidAnalogStickState left = padGetStickPos(&s_pads[i], 0);
        HidAnalogStickState right = padGetStickPos(&s_pads[i], 1);
        // libnx's stick range (JOYSTICK_MIN/MAX = -0x7FFF..0x7FFF) matches what the host's
        // ConvertAnalog (VirtualGamepad.cs: "axis / 32767.5") expects - sent as-is, no rescale.
        pkt->controllers[i].lJoyX = left.x;
        pkt->controllers[i].lJoyY = left.y;
        pkt->controllers[i].rJoyX = right.x;
        pkt->controllers[i].rJoyY = right.y;

        readSixaxis(i, &pkt->controllers[i]);
    }
    pkt->controllerCount = connectedCount;

    // The debug chord toggles the menu once per press (all four have to be let go to toggle again).
    if (debug && chordHeld && !s_debugChordLatched)
    {
        bool open = !atomic_load(&s_debugMenuOpen);
        atomic_store(&s_debugMenuOpen, open);
        atomic_store(&s_debugMenuDown, 0);
        logWrite("input", "debug menu %s", open ? "opened" : "closed");
    }
    s_debugChordLatched = chordHeld;

    bool menuOpen = atomic_load(&s_debugMenuOpen);
    if (s_debugMenuWasOpen && !menuOpen)
        s_holdUntilReleased = true;
    s_debugMenuWasOpen = menuOpen;
    if (s_holdUntilReleased && allButtons == 0)
        s_holdUntilReleased = false;

    if (menuOpen || s_holdUntilReleased)
    {
        // The menu has the controller: presses go to it, the PC gets a controller at rest.
        if (menuOpen)
            atomic_fetch_or(&s_debugMenuDown, allButtons & ~s_debugPrevButtons & ~(u64)DEBUG_CHORD);
        for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++)
        {
            pkt->controllers[i].heldKeys = 0;
            pkt->controllers[i].lJoyX = pkt->controllers[i].lJoyY = 0;
            pkt->controllers[i].rJoyX = pkt->controllers[i].rJoyY = 0;
        }
        minusHeldAlone = false;
    }
    s_debugPrevButtons = allButtons;

    updateMenuHold(minusHeldAlone);
    applyRumble();

    HidTouchScreenState touch = {0};
    if (hidGetTouchScreenStates(&touch, 1) && touch.count > 0)
    {
        // The Switch's touch digitizer is natively 1280x720 (matches the console's screen
        // resolution), which already matches what the host expects - see
        // .claude/skills/solarnx-pc-architect/references/switch-protocol.md: "Touch coordinates
        // arrive pre-normalized to a 1280x720 reference frame". No rescale needed.
        pkt->touchX1 = touch.touches[0].x;
        pkt->touchY1 = touch.touches[0].y;
    }

    // Chosen once via menu.c's runAppSelectMenu(), before the HID send loop ever starts - see
    // inputSetAppTarget().
    pkt->appTarget = s_appTarget;
    // appletGetOperationMode() is the value libnx caches from the applet messages the main thread
    // pumps - a plain read, fine from this thread.
    pkt->displayHeight = appletGetOperationMode() == AppletOperationMode_Console ? 1080 : 720;
    pkt->libraryEntryId = s_libraryEntryId;

    // Link-quality telemetry for the host's AdaptiveBitrateController - see network.h's doc
    // comment on these fields.
    pkt->videoReconnectCount = videoGetReconnectCount();
    pkt->videoMeasuredBitrateKbps = videoGetMeasuredBitrateKbps();
    pkt->videoLostFrameCount = videoGetLostFrameCount();
    pkt->videoRecoveredPacketCount = videoGetRecoveredPacketCount();
    pkt->audioReconnectCount = audioGetReconnectCount();
    pkt->audioConcealedFrameCount = audioGetConcealedFrameCount();
}

void inputExit(void)
{
    hidStopSixAxisSensor(s_sixaxisHandheld);
    for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++)
    {
        if (s_sixaxisSlot[i] != SixaxisSlot_None)
            hidStopSixAxisSensor(sixaxisHandleForSlot(i, s_sixaxisSlot[i]));
    }
}
