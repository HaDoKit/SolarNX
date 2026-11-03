#include "control.h"

#include <string.h>

#include <switch.h>

#include "../common/logger/logger.h"
#include "../input/input.h"

static Mutex s_mutex;
static PcVersion s_pc;          // what the PC last said about itself
static bool s_peerReady = false; // the PC said Hello in this session
static uint8_t s_options = 0;    // the PC's last Options flags
static ControlDebugInfo s_debugInfo;
static uint32_t s_debugSeq = 0;
static uint32_t s_rttUs = 0;
static uint32_t s_rumbleMessages = 0;
static uint8_t s_rumbleLevels[8];

const int kControlResolutions[CONTROL_RESOLUTION_COUNT][2] = {{640, 360}, {854, 480}, {960, 540}, {1280, 720}, {1920, 1080}};
const int kControlFps[CONTROL_FPS_COUNT] = {24, 30, 45, 60};
const char *const kControlTunes[CONTROL_TUNE_COUNT] = {"hq", "ll", "ull"};

// Outgoing messages, filled from any thread and drained by the HID thread (it alone writes the
// HID socket / sends Internet input, so frames never interleave). Small on purpose: these are rare
// (a menu change, a ping), not a stream.
#define QUEUE_SLOTS 16
#define QUEUE_SLOT_PAYLOAD 128
typedef struct {
    uint8_t type;
    uint16_t length;
    uint8_t payload[QUEUE_SLOT_PAYLOAD];
} QueuedMessage;
static QueuedMessage s_queue[QUEUE_SLOTS];
static int s_queueHead = 0; // next to take
static int s_queueCount = 0;

void controlInit(void)
{
    mutexInit(&s_mutex);
    memset(&s_pc, 0, sizeof(s_pc));
}

void controlWriteOwnVersion(uint8_t *out)
{
    memset(out, 0, CONTROL_VERSION_WIRE_SIZE);
    uint16_t major = SOLARNX_PROTOCOL_MAJOR, minor = SOLARNX_PROTOCOL_MINOR;
    memcpy(out, &major, 2);
    memcpy(out + 2, &minor, 2);
    strncpy((char *)out + 4, SOLARNX_VERSION, CONTROL_APP_VERSION_SIZE - 1);
}

void controlReadVersion(const uint8_t *wire, PcVersion *out)
{
    memset(out, 0, sizeof(*out));
    out->known = true;
    memcpy(&out->major, wire, 2);
    memcpy(&out->minor, wire + 2, 2);
    memcpy(out->appVersion, wire + 4, CONTROL_APP_VERSION_SIZE);
    out->appVersion[CONTROL_APP_VERSION_SIZE] = '\0';
    // Host text: keep it printable for the menu (it is drawn with "%s", never as a format).
    for (char *c = out->appVersion; *c; c++)
        if ((unsigned char)*c < 0x20)
            *c = '?';
    if (out->appVersion[0] == '\0')
        strcpy(out->appVersion, "?");
}

void controlSetPcVersion(const PcVersion *version)
{
    mutexLock(&s_mutex);
    bool changed = s_pc.known != version->known || s_pc.legacy != version->legacy ||
        s_pc.major != version->major || s_pc.minor != version->minor ||
        strcmp(s_pc.appVersion, version->appVersion) != 0;
    s_pc = *version;
    mutexUnlock(&s_mutex);
    if (changed)
        logWrite("control", "PC: %s (protocol %u.%u), this Switch: %s (protocol %u.%u)",
            version->legacy ? "older SolarNX" : version->appVersion, version->major, version->minor,
            SOLARNX_VERSION, SOLARNX_PROTOCOL_MAJOR, SOLARNX_PROTOCOL_MINOR);
}

bool controlGetPcVersion(PcVersion *out)
{
    mutexLock(&s_mutex);
    *out = s_pc;
    mutexUnlock(&s_mutex);
    return out->known;
}

PcCompat controlPcCompat(void)
{
    PcVersion pc;
    if (!controlGetPcVersion(&pc))
        return PcCompat_Unknown;
    if (pc.major != SOLARNX_PROTOCOL_MAJOR)
        return pc.major < SOLARNX_PROTOCOL_MAJOR ? PcCompat_UpdatePc : PcCompat_UpdateSwitch;
    if (pc.minor == SOLARNX_PROTOCOL_MINOR)
        return PcCompat_Same;
    return pc.minor < SOLARNX_PROTOCOL_MINOR ? PcCompat_PcOlder : PcCompat_PcNewer;
}

bool controlPcIncompatible(void)
{
    PcCompat compat = controlPcCompat();
    return compat == PcCompat_UpdatePc || compat == PcCompat_UpdateSwitch;
}

// Unlocked: caller holds s_mutex.
static void enqueueLocked(uint8_t type, const void *payload, size_t length)
{
    if (s_queueCount >= QUEUE_SLOTS || length > QUEUE_SLOT_PAYLOAD)
        return;
    QueuedMessage *m = &s_queue[(s_queueHead + s_queueCount) % QUEUE_SLOTS];
    m->type = type;
    m->length = (uint16_t)length;
    if (length > 0)
        memcpy(m->payload, payload, length);
    s_queueCount++;
}

void controlSessionReset(void)
{
    uint8_t hello[CONTROL_VERSION_WIRE_SIZE];
    controlWriteOwnVersion(hello);
    mutexLock(&s_mutex);
    s_peerReady = false;
    s_options = 0;
    s_debugInfo.valid = false;
    s_rttUs = 0;
    s_queueHead = 0;
    s_queueCount = 0;
    enqueueLocked(ClientMessage_Hello, hello, sizeof(hello)); // the opener - goes out regardless
    mutexUnlock(&s_mutex);
}

uint8_t controlOptions(void)
{
    mutexLock(&s_mutex);
    uint8_t options = s_options;
    mutexUnlock(&s_mutex);
    return options;
}

bool controlDebugEnabled(void)
{
    mutexLock(&s_mutex);
    bool enabled = s_peerReady && (s_options & CONTROL_OPTION_DEBUG);
    mutexUnlock(&s_mutex);
    return enabled;
}

bool controlGetDebugInfo(ControlDebugInfo *out)
{
    mutexLock(&s_mutex);
    *out = s_debugInfo;
    mutexUnlock(&s_mutex);
    return out->valid;
}

void controlSendPing(void)
{
    uint64_t now = armGetSystemTick();
    controlQueueClientMessage(ClientMessage_Ping, &now, sizeof(now));
}

uint32_t controlRttUs(void)
{
    mutexLock(&s_mutex);
    uint32_t rtt = s_rttUs;
    mutexUnlock(&s_mutex);
    return rtt;
}

uint32_t controlRumbleStats(uint8_t levels[8])
{
    mutexLock(&s_mutex);
    uint32_t count = s_rumbleMessages;
    memcpy(levels, s_rumbleLevels, 8);
    mutexUnlock(&s_mutex);
    return count;
}

void controlSendSetting(uint8_t setting, int32_t value)
{
    uint8_t payload[8] = {setting, 0, 0, 0};
    memcpy(payload + 4, &value, 4);
    controlQueueClientMessage(ClientMessage_SetSetting, payload, sizeof(payload));
}

bool controlPeerReady(void)
{
    mutexLock(&s_mutex);
    bool ready = s_peerReady;
    mutexUnlock(&s_mutex);
    return ready;
}

static bool supportsControl(const PcVersion *pc)
{
    return pc->known && !pc->legacy && pc->major == SOLARNX_PROTOCOL_MAJOR && pc->minor >= 1 && SOLARNX_PROTOCOL_MINOR >= 1;
}

void controlAssumePeerReady(void)
{
    mutexLock(&s_mutex);
    s_peerReady = supportsControl(&s_pc);
    mutexUnlock(&s_mutex);
}

void controlHandleHostMessage(uint8_t type, const uint8_t *payload, size_t length)
{
    switch (type)
    {
    case HostMessage_Hello:
        if (length >= CONTROL_VERSION_WIRE_SIZE)
        {
            PcVersion pc;
            controlReadVersion(payload, &pc);
            controlSetPcVersion(&pc);
            mutexLock(&s_mutex);
            s_peerReady = supportsControl(&pc);
            mutexUnlock(&s_mutex);
            logWrite("control", "the PC says hello: SolarNX %s", pc.appVersion);
        }
        break;
    case HostMessage_Options:
        if (length >= 1)
        {
            mutexLock(&s_mutex);
            s_options = payload[0];
            mutexUnlock(&s_mutex);
        }
        break;
    case HostMessage_DebugInfo:
        if (length >= 33 && length >= 33u + payload[32])
        {
            ControlDebugInfo info = {.valid = true};
            memcpy(&info.bitrateKbps, payload, 4);
            memcpy(&info.runningKbps, payload + 4, 4);
            memcpy(&info.width, payload + 8, 2);
            memcpy(&info.height, payload + 10, 2);
            info.fps = payload[12];
            info.codec = payload[13];
            info.codecSetting = payload[14];
            info.encoder = payload[15];
            info.resolution = payload[16];
            info.preset = payload[17];
            info.tune = payload[18];
            info.flags = payload[19];
            size_t nameLength = payload[32] < sizeof(info.encoderName) - 1 ? payload[32] : sizeof(info.encoderName) - 1;
            memcpy(info.encoderName, payload + 33, nameLength);
            info.encoderName[nameLength] = '\0';
            for (char *c = info.encoderName; *c; c++) // host text: drawn with "%s" only, but keep it printable
                if ((unsigned char)*c < 0x20)
                    *c = '?';
            mutexLock(&s_mutex);
            info.seq = ++s_debugSeq;
            s_debugInfo = info;
            mutexUnlock(&s_mutex);
        }
        break;
    case HostMessage_Pong:
        if (length >= 8)
        {
            uint64_t sent;
            memcpy(&sent, payload, 8);
            uint64_t us = armTicksToNs(armGetSystemTick() - sent) / 1000;
            uint32_t sample = us > 2000000 ? 2000000 : (uint32_t)us;
            mutexLock(&s_mutex);
            s_rttUs = s_rttUs == 0 ? sample : (s_rttUs * 7 + sample) / 8;
            mutexUnlock(&s_mutex);
        }
        break;
    case HostMessage_Rumble:
        if (length >= 8)
        {
            for (int i = 0; i < 4; i++)
                inputSetRumble(i, payload[i * 2], payload[i * 2 + 1]);
            mutexLock(&s_mutex);
            s_rumbleMessages++;
            memcpy(s_rumbleLevels, payload, 8);
            mutexUnlock(&s_mutex);
        }
        break;
    default: // a newer PC's message this build doesn't know - ignored by design (protocol minor)
        break;
    }
}

void controlQueueClientMessage(uint8_t type, const void *payload, size_t length)
{
    mutexLock(&s_mutex);
    enqueueLocked(type, payload, length);
    mutexUnlock(&s_mutex);
}

bool controlTakeClientMessage(uint8_t *type, uint8_t *payload, size_t *length)
{
    mutexLock(&s_mutex);
    bool have = s_queueCount > 0;
    if (have)
    {
        QueuedMessage *m = &s_queue[s_queueHead];
        // Only Hello may go out before the PC's own Hello; anything else waits for it.
        have = s_peerReady || m->type == ClientMessage_Hello;
        if (have)
        {
            *type = m->type;
            *length = m->length;
            memcpy(payload, m->payload, m->length);
            s_queueHead = (s_queueHead + 1) % QUEUE_SLOTS;
            s_queueCount--;
        }
    }
    mutexUnlock(&s_mutex);
    return have;
}
