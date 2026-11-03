#include "input.h"

#include <string.h>

#include <switch.h>

#include "../common/logger/logger.h"
#include "../video/video.h"
#include "../audio/audio.h"
#include "../common/appstate/appstate.h"

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

    logWrite("input", "pad/touch/sixaxis ready for up to %d controllers", INPUT_MAX_CONTROLLERS);
    return true;
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
    for (int i = 0; i < INPUT_MAX_CONTROLLERS; i++)
    {
        padUpdate(&s_pads[i]);
        updateSixaxisSlot(i);

        if (!padIsConnected(&s_pads[i]))
            continue;

        connectedCount++;

        // libnx's HidNpadButton bit positions (A=0,B=1,X=2,Y=3,StickL=4,StickR=5,L=6,R=7,ZL=8,
        // ZR=9,Plus=10,Minus=11,Left=12,Up=13,Right=14,Down=15) match the wire format's heldKeys
        // bit-for-bit (see SolarNX/Input/HidProtocol.cs's ControllerState.IsSet) - no remapping,
        // just mask to the low 16 bits the host actually reads (HeldKeys is a C# ushort).
        u64 buttons = padGetButtons(&s_pads[i]);
        pkt->controllers[i].heldKeys = (uint32_t)(buttons & 0xFFFFu);
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
    updateMenuHold(minusHeldAlone);

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
