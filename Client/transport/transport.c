#include "transport.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#include <switch.h>

#include "../settings/settings.h"
#include "../library/library.h"
#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"

#define WIRE_MAGIC   0x55584E53u // "SNXU"
#define WIRE_VERSION 1
#define WIRE_HEADER  8
#define WIRE_CODE    16

enum {
    TYPE_HELLO = 1,
    TYPE_WELCOME = 2,
    TYPE_REJECT = 3,
    TYPE_INPUT = 4,
    TYPE_BYE = 5,
    TYPE_END = 6,
    TYPE_VIDEO = 7,
    TYPE_AUDIO = 8,
    TYPE_LIB_OFFER = 9,
    TYPE_LIB_REPLY = 10,
    TYPE_LIB_CHUNK = 11,
    TYPE_LIB_ACK = 12,
};

#define TICKS_PER_SECOND 19200000ULL // svcGetSystemTick's frequency
#define HELLO_INTERVAL   (TICKS_PER_SECOND)
#define PC_SILENT_WAIT   (3 * TICKS_PER_SECOND)  // status back to "waiting"
#define PC_SILENT_END    (5 * TICKS_PER_SECOND)  // a session ends - the PC answers every Hello, so this is real silence
#define RESOLVE_RETRY    (10 * TICKS_PER_SECOND)

// Same core/priority as the HID and library threads it replaces in this mode; idle between
// datagrams. The receive buffer is on the heap-free static below, so a small stack is enough.
#define TRANSPORT_THREAD_STACK_SIZE 0x8000
#define TRANSPORT_THREAD_PRIORITY   0x2C
#define TRANSPORT_THREAD_CORE       0

static int s_sock = -1;
static Thread s_thread;
static uint32_t s_nonce;

static Mutex s_mutex; // guards s_pc / s_pcValid
static struct sockaddr_in s_pc;
static bool s_pcValid = false;

static _Atomic int s_status = TransportStatus_NoAddress;
static atomic_bool s_welcomed = false;
static atomic_bool s_ended = false;
static _Atomic uint64_t s_lastFromPc = 0;

static uint8_t s_rxBuffer[2048];

static void writeHeader(uint8_t *packet, uint8_t type)
{
    uint32_t magic = WIRE_MAGIC;
    memcpy(packet, &magic, 4);
    packet[4] = type;
    packet[5] = WIRE_VERSION;
    packet[6] = 0;
    packet[7] = 0;
}

static void sendToPc(const uint8_t *packet, size_t length)
{
    struct sockaddr_in pc;
    mutexLock(&s_mutex);
    bool valid = s_pcValid;
    pc = s_pc;
    mutexUnlock(&s_mutex);
    if (valid)
        sendto(s_sock, packet, length, 0, (struct sockaddr *)&pc, sizeof(pc));
}

// "host" or "host:port" -> s_pc. getaddrinfo goes through the console's DNS, so a domain works too.
static void resolvePc(const char *address)
{
    char host[SETTINGS_ADDRESS_MAX];
    snprintf(host, sizeof(host), "%s", address);
    int port = SOLARNX_INTERNET_PORT;
    char *colon = strrchr(host, ':');
    if (colon != NULL)
    {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535)
            port = SOLARNX_INTERNET_PORT;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *result = NULL;
    bool ok = host[0] != '\0' && getaddrinfo(host, NULL, &hints, &result) == 0 && result != NULL;

    mutexLock(&s_mutex);
    s_pcValid = ok;
    if (ok)
    {
        s_pc = *(struct sockaddr_in *)result->ai_addr;
        s_pc.sin_port = htons((uint16_t)port);
    }
    mutexUnlock(&s_mutex);
    if (result != NULL)
        freeaddrinfo(result);

    if (ok)
        logWrite("transport", "PC address %s -> %s:%d", address, inet_ntoa(s_pc.sin_addr), port);
    else
        logWrite("transport", "cannot resolve PC address '%s'", address);
    atomic_store(&s_status, ok ? TransportStatus_Waiting : (host[0] == '\0' ? TransportStatus_NoAddress : TransportStatus_BadAddress));
}

static void sendHello(void)
{
    char code[SETTINGS_CODE_MAX + 1];
    settingsGetConnectionCode(code, sizeof(code));
    uint8_t packet[WIRE_HEADER + 4 + WIRE_CODE];
    memset(packet, 0, sizeof(packet));
    writeHeader(packet, TYPE_HELLO);
    memcpy(packet + WIRE_HEADER, &s_nonce, 4);
    memcpy(packet + WIRE_HEADER + 4, code, strnlen(code, WIRE_CODE));
    sendToPc(packet, sizeof(packet));
}

static void forwardToLoopback(const uint8_t *data, size_t length, uint16_t port)
{
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(port);
    sendto(s_sock, data, length, 0, (struct sockaddr *)&local, sizeof(local));
}

static void handleDatagram(const uint8_t *data, size_t length)
{
    uint32_t magic;
    if (length < WIRE_HEADER)
        return;
    memcpy(&magic, data, 4);
    if (magic != WIRE_MAGIC)
        return;
    uint8_t type = data[4];
    const uint8_t *payload = data + WIRE_HEADER;
    size_t payloadLength = length - WIRE_HEADER;

    atomic_store(&s_lastFromPc, svcGetSystemTick());

    switch (type)
    {
    case TYPE_WELCOME:
        if (payloadLength >= 4 + sizeof(SolarNxSessionConfig))
        {
            SolarNxSessionConfig config;
            memcpy(&config, payload + 4, sizeof(config));
            networkSetSessionConfig(&config);
            if (atomic_load(&s_status) != TransportStatus_Connected)
                logWrite("transport", "connected to the PC");
            atomic_store(&s_status, TransportStatus_Connected);
            atomic_store(&s_welcomed, true);
        }
        break;
    case TYPE_REJECT:
        if (payloadLength >= 5)
        {
            int status = payload[4] == 1 ? TransportStatus_WrongCode
                : payload[4] == 2 ? TransportStatus_Busy
                : TransportStatus_WrongVersion;
            if (atomic_load(&s_status) != status)
                logWrite("transport", "the PC refused us: %s", transportStatusText(status));
            atomic_store(&s_status, status);
        }
        break;
    case TYPE_END:
        atomic_store(&s_ended, true);
        break;
    case TYPE_VIDEO:
        forwardToLoopback(payload, payloadLength, SOLARNX_VIDEO_PORT);
        break;
    case TYPE_AUDIO:
        forwardToLoopback(payload, payloadLength, SOLARNX_AUDIO_PORT);
        break;
    case TYPE_LIB_OFFER:
        if (payloadLength >= 20)
        {
            uint32_t offerId, hostRevision;
            uint64_t hash;
            memcpy(&offerId, payload, 4);
            memcpy(&hash, payload + 4, 8);
            memcpy(&hostRevision, payload + 16, 4);
            settingsSyncButtonLayout(payload[12] != 0, hostRevision);
            bool swapAbxy;
            uint32_t revision;
            settingsGetButtonLayout(&swapAbxy, &revision);

            uint8_t reply[WIRE_HEADER + 12];
            memset(reply, 0, sizeof(reply));
            writeHeader(reply, TYPE_LIB_REPLY);
            memcpy(reply + WIRE_HEADER, &offerId, 4);
            reply[WIRE_HEADER + 4] = libraryUdpOffer(hash) ? 1 : 0;
            reply[WIRE_HEADER + 5] = swapAbxy ? 1 : 0;
            memcpy(reply + WIRE_HEADER + 8, &revision, 4);
            sendToPc(reply, sizeof(reply));
        }
        break;
    case TYPE_LIB_CHUNK:
        if (payloadLength >= 16)
        {
            uint64_t hash;
            uint32_t total, offset;
            memcpy(&hash, payload, 8);
            memcpy(&total, payload + 8, 4);
            memcpy(&offset, payload + 12, 4);
            uint32_t received = libraryUdpChunk(hash, total, offset, payload + 16, (uint32_t)(payloadLength - 16));

            uint8_t ack[WIRE_HEADER + 12];
            writeHeader(ack, TYPE_LIB_ACK);
            memcpy(ack + WIRE_HEADER, &hash, 8);
            memcpy(ack + WIRE_HEADER + 8, &received, 4);
            sendToPc(ack, sizeof(ack));
        }
        break;
    default:
        break;
    }
}

// Only what comes from the PC's address is ours - anything else on this port is dropped.
static bool fromPc(const struct sockaddr_in *from)
{
    mutexLock(&s_mutex);
    bool ok = s_pcValid && from->sin_addr.s_addr == s_pc.sin_addr.s_addr && from->sin_port == s_pc.sin_port;
    mutexUnlock(&s_mutex);
    return ok;
}

static void transportThreadFunc(void *arg)
{
    (void)arg;
    char address[SETTINGS_ADDRESS_MAX] = "";
    char resolvedFor[SETTINGS_ADDRESS_MAX] = "\x01"; // never a real address: forces the first resolve
    uint64_t lastResolve = 0, lastHello = 0;

    while (appIsRunning())
    {
        uint64_t now = svcGetSystemTick();

        settingsGetPcAddress(address, sizeof(address));
        bool pcValid;
        mutexLock(&s_mutex);
        pcValid = s_pcValid;
        mutexUnlock(&s_mutex);
        if (strcmp(address, resolvedFor) != 0 || (!pcValid && address[0] != '\0' && now - lastResolve > RESOLVE_RETRY))
        {
            snprintf(resolvedFor, sizeof(resolvedFor), "%s", address);
            resolvePc(address);
            lastResolve = now;
        }

        if (now - lastHello >= HELLO_INTERVAL)
        {
            sendHello();
            lastHello = now;
            int status = atomic_load(&s_status);
            if (status == TransportStatus_Connected && now - atomic_load(&s_lastFromPc) > PC_SILENT_WAIT)
            {
                logWrite("transport", "the PC stopped answering");
                atomic_store(&s_status, TransportStatus_Waiting);
            }
        }

        struct sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        ssize_t n = recvfrom(s_sock, s_rxBuffer, sizeof(s_rxBuffer), 0, (struct sockaddr *)&from, &fromLength);
        if (n > 0 && fromPc(&from))
            handleDatagram(s_rxBuffer, (size_t)n);
    }
}

bool transportStart(void)
{
    mutexInit(&s_mutex);
    s_nonce = (uint32_t)svcGetSystemTick();

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0)
        return false;

    // The receive timeout only lets the loop come round for Hello/resolve/exit checks.
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    // Video keyframes arrive as bursts here first now - same headroom as video.c's own socket.
    int rcvBuf = 0x40000;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVBUF, &rcvBuf, sizeof(rcvBuf));

    if (R_FAILED(threadCreate(&s_thread, transportThreadFunc, NULL, NULL,
            TRANSPORT_THREAD_STACK_SIZE, TRANSPORT_THREAD_PRIORITY, TRANSPORT_THREAD_CORE)) ||
        R_FAILED(threadStart(&s_thread)))
    {
        close(s_sock);
        s_sock = -1;
        return false;
    }
    logWrite("transport", "Internet mode (experimental) - UDP transport started");
    return true;
}

TransportStatus transportGetStatus(void)
{
    return s_sock < 0 ? TransportStatus_Off : (TransportStatus)atomic_load(&s_status);
}

const char *transportStatusText(TransportStatus status)
{
    switch (status)
    {
    case TransportStatus_Off: return "off";
    case TransportStatus_NoAddress: return "no PC address set";
    case TransportStatus_BadAddress: return "PC address not found";
    case TransportStatus_Waiting: return "waiting for the PC";
    case TransportStatus_Connected: return "connected";
    case TransportStatus_WrongCode: return "wrong connection code";
    case TransportStatus_Busy: return "PC is busy with another Switch";
    case TransportStatus_WrongVersion: return "PC runs another SolarNX version";
    }
    return "";
}

void transportBeginSession(void)
{
    atomic_store(&s_welcomed, false);
    atomic_store(&s_ended, false);
}

bool transportWaitWelcome(void)
{
    while (appIsRunning())
    {
        if (atomic_load(&s_welcomed))
            return true;
        svcSleepThread(50000000ULL); // 50ms - the next Hello (<=1s) brings a Welcome
    }
    return false;
}

bool transportSendInput(const SolarNxHidPacket *pkt)
{
    if (atomic_load(&s_ended))
        return false;
    if (svcGetSystemTick() - atomic_load(&s_lastFromPc) > PC_SILENT_END)
        return false;

    uint8_t packet[WIRE_HEADER + sizeof(SolarNxHidPacket)];
    writeHeader(packet, TYPE_INPUT);
    memcpy(packet + WIRE_HEADER, pkt, sizeof(*pkt));
    sendToPc(packet, sizeof(packet));
    return true;
}

void transportEndSession(void)
{
    uint8_t packet[WIRE_HEADER];
    writeHeader(packet, TYPE_BYE);
    for (int i = 0; i < 3; i++) // a lost Bye would leave the PC waiting for its input timeout
        sendToPc(packet, sizeof(packet));
}
