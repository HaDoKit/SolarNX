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

#include "crypto.h"
#include "../settings/settings.h"
#include "../library/library.h"
#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"

#define WIRE_MAGIC     0x55584E53u // "SNXU"
#define WIRE_VERSION   2
#define WIRE_HEADER    8
#define WIRE_DEVICE_ID 16
#define WIRE_HOST_ID   SETTINGS_HOST_ID_SIZE
#define WIRE_NAME      32

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
    TYPE_PAIR_START = 13,
    TYPE_PAIR_KEY = 14,
    TYPE_PAIR_NONCE_S = 15,
    TYPE_PAIR_NONCE_P = 16,
    TYPE_PAIR_RESULT = 17,
    TYPE_KEEPALIVE = 18,
    TYPE_KEEPALIVE_ACK = 19,
};

enum {
    REJECT_NOT_PAIRED = 1,
    REJECT_BUSY = 2,
    REJECT_WRONG_VERSION = 3,
    REJECT_PAIRING_CLOSED = 4,
};

// Rendezvous server protocol - mirrors Server/RendezvousWire.cs.
#define RV_MAGIC   0x52584E53u // "SNXR"
#define RV_VERSION 1
#define RV_KEY     32
enum {
    RV_LOOKUP = 3,
    RV_PEER = 4,
    RV_RELAY_UP = 6,
    RV_RELAY_FROM = 9,
};

#define TICKS_PER_SECOND 19200000ULL // svcGetSystemTick's frequency
#define SEND_INTERVAL    (TICKS_PER_SECOND)       // Hello / Keepalive / pairing retries
#define PC_SILENT_RESET  (3 * TICKS_PER_SECOND)   // connected, but nothing valid from the PC: start over
#define PC_SILENT_END    (5 * TICKS_PER_SECOND)   // a session ends
#define RESOLVE_RETRY    (10 * TICKS_PER_SECOND)
#define PAIRING_TIMEOUT  (150 * TICKS_PER_SECOND) // the PC keeps pairing open for 2 minutes
#define LOOKUP_REFRESH   (15 * TICKS_PER_SECOND)  // not connected: ask the server again (the PC's address may change)
#define DIRECT_GRACE     (5 * TICKS_PER_SECOND)   // no answer over the direct path this long: try the relay

// Same core/priority as the HID and library threads it replaces in this mode; idle between
// datagrams. Buffers are static, so a small stack is enough.
#define TRANSPORT_THREAD_STACK_SIZE 0x8000
#define TRANSPORT_THREAD_PRIORITY   0x2C
#define TRANSPORT_THREAD_CORE       0

static int s_sock = -1;
static Thread s_thread;

// Guards everything below that both the transport thread and the HID thread touch: the PC address,
// the session keys and the send counter.
static Mutex s_mutex;
static struct sockaddr_in s_pc;
static bool s_pcValid = false;
static bool s_connected = false;
static CryptoSessionKeys s_keys;
static uint64_t s_sendCounter = 0;
static CryptoReplayWindow s_replay;
static uint8_t s_switchNonce[CRYPTO_NONCE_SIZE];

// The rendezvous server, if one is set (also under s_mutex): its address, whether the PC behind the
// looked-up id is known, and whether we talk through the server's relay instead of directly.
static bool s_useServer = false;
static struct sockaddr_in s_server;
static bool s_serverValid = false;
static bool s_peerKnown = false;
static bool s_relayAllowed = false;
static bool s_relay = false;
static uint64_t s_peerAt = 0;
static uint64_t s_lastLookup = 0;
static int s_unansweredLookups = 0;
static char s_rvPcId[SETTINGS_HOST_ID_SIZE + 1] = "";
static uint8_t s_rvKey[RV_KEY];
static uint8_t s_relayBuffer[2048 + 8 + SETTINGS_HOST_ID_SIZE + RV_KEY];

static _Atomic int s_status = TransportStatus_NoAddress;
static atomic_bool s_ended = false;
static _Atomic uint64_t s_lastValidFromPc = 0;

// Pairing - transport thread only, except the request flags and the code.
typedef enum { PAIR_IDLE, PAIR_START, PAIR_NONCE, PAIR_CODE } PairState;
static PairState s_pairState = PAIR_IDLE;
static atomic_bool s_pairRequested = false;
static atomic_bool s_pairCancelRequested = false;
static _Atomic uint32_t s_pairCode = 0;
static uint8_t s_pairPrivate[32], s_pairPublic[32], s_pcPublic[32], s_pcCommit[32];
static uint8_t s_pairSwitchNonce[CRYPTO_NONCE_SIZE], s_pairKey[32];
static uint64_t s_pairDeadline = 0;

static uint8_t s_rxBuffer[2048];
static uint8_t s_plain[2048];

static void writeHeader(uint8_t *packet, uint8_t type)
{
    uint32_t magic = WIRE_MAGIC;
    memcpy(packet, &magic, 4);
    packet[4] = type;
    packet[5] = WIRE_VERSION;
    packet[6] = 0;
    packet[7] = 0;
}

static void writeRvHeader(uint8_t *packet, uint8_t type)
{
    uint32_t magic = RV_MAGIC;
    memcpy(packet, &magic, 4);
    packet[4] = type;
    packet[5] = RV_VERSION;
    packet[6] = 0;
    packet[7] = 0;
}

// Caller holds s_mutex. Directly to the PC, or wrapped in RelayUp to the server.
static void sendToPcLocked(const uint8_t *packet, size_t length)
{
    if (s_relay && s_serverValid)
    {
        if (length > 2048)
            return;
        writeRvHeader(s_relayBuffer, RV_RELAY_UP);
        memcpy(s_relayBuffer + 8, s_rvPcId, SETTINGS_HOST_ID_SIZE);
        memcpy(s_relayBuffer + 8 + SETTINGS_HOST_ID_SIZE, s_rvKey, RV_KEY);
        memcpy(s_relayBuffer + 8 + SETTINGS_HOST_ID_SIZE + RV_KEY, packet, length);
        sendto(s_sock, s_relayBuffer, 8 + SETTINGS_HOST_ID_SIZE + RV_KEY + length, 0, (struct sockaddr *)&s_server, sizeof(s_server));
    }
    else if (s_pcValid)
    {
        sendto(s_sock, packet, length, 0, (struct sockaddr *)&s_pc, sizeof(s_pc));
    }
}

static void sendPlain(uint8_t type, const void *payload, size_t length)
{
    uint8_t packet[WIRE_HEADER + 128];
    writeHeader(packet, type);
    memcpy(packet + WIRE_HEADER, payload, length);
    mutexLock(&s_mutex);
    sendToPcLocked(packet, WIRE_HEADER + length);
    mutexUnlock(&s_mutex);
}

// Sealed with this session's Switch->PC keys; false if there is no session.
static bool sendSealed(uint8_t type, const void *payload, size_t length)
{
    uint8_t header[WIRE_HEADER];
    uint8_t packet[WIRE_HEADER + sizeof(SolarNxHidPacket) + CRYPTO_OVERHEAD];
    if (length > sizeof(SolarNxHidPacket))
        return false;
    writeHeader(header, type);
    mutexLock(&s_mutex);
    bool ok = s_connected;
    if (ok)
    {
        size_t size = cryptoSeal(packet, s_keys.encSwitchToPc, s_keys.macSwitchToPc, ++s_sendCounter,
            header, sizeof(header), payload, length);
        sendToPcLocked(packet, size);
    }
    mutexUnlock(&s_mutex);
    return ok;
}

// "host" or "host:port" -> *out. getaddrinfo goes through the console's DNS, so a domain works too.
static bool resolveAddress(const char *address, int defaultPort, struct sockaddr_in *out)
{
    char host[SETTINGS_ADDRESS_MAX];
    snprintf(host, sizeof(host), "%s", address);
    int port = defaultPort;
    char *colon = strrchr(host, ':');
    if (colon != NULL)
    {
        *colon = '\0';
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535)
            port = defaultPort;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *result = NULL;
    bool ok = host[0] != '\0' && getaddrinfo(host, NULL, &hints, &result) == 0 && result != NULL;
    memset(out, 0, sizeof(*out));
    if (ok)
    {
        *out = *(struct sockaddr_in *)result->ai_addr;
        out->sin_port = htons((uint16_t)port);
    }
    if (result != NULL)
        freeaddrinfo(result);
    return ok;
}

static void resolvePc(const char *address)
{
    int port = SOLARNX_INTERNET_PORT;
    struct sockaddr_in resolved;
    bool ok = resolveAddress(address, port, &resolved);
    port = ntohs(resolved.sin_port);
    const char *host = address;

    mutexLock(&s_mutex);
    s_pcValid = ok;
    s_pc = resolved;
    s_connected = false;
    mutexUnlock(&s_mutex);

    if (ok)
        logWrite("transport", "PC address %s -> %s:%d", address, inet_ntoa(resolved.sin_addr), port);
    else
        logWrite("transport", "cannot resolve PC address '%s'", address);
    atomic_store(&s_status, ok ? TransportStatus_Waiting : (host[0] == '\0' ? TransportStatus_NoAddress : TransportStatus_BadAddress));
}

static void disconnect(TransportStatus status)
{
    mutexLock(&s_mutex);
    bool was = s_connected;
    s_connected = false;
    if (s_useServer)
    {
        s_peerKnown = false; // ask the server again - the PC's public address may have changed
        s_relay = false;
    }
    mutexUnlock(&s_mutex);
    cryptoRandom(s_switchNonce, sizeof(s_switchNonce)); // the next Hello starts a new session
    if (was)
        logWrite("transport", "lost the PC - starting over");
    atomic_store(&s_status, status);
}

// --- Pairing ---

static void pairingFail(TransportStatus status, const char *why)
{
    logWrite("transport", "pairing: %s", why);
    s_pairState = PAIR_IDLE;
    atomic_store(&s_status, status);
}

static void sendPairStart(void)
{
    uint8_t payload[WIRE_DEVICE_ID + 32 + WIRE_NAME];
    memset(payload, 0, sizeof(payload));
    settingsGetDeviceId(payload);
    memcpy(payload + WIRE_DEVICE_ID, s_pairPublic, 32);

    // The console's own nickname, so the PC can show which Switch asks.
    SetSysDeviceNickName nickname;
    memset(&nickname, 0, sizeof(nickname));
    if (R_SUCCEEDED(setsysInitialize()))
    {
        setsysGetDeviceNickname(&nickname);
        setsysExit();
    }
    const char *name = nickname.nickname[0] != '\0' ? nickname.nickname : "Nintendo Switch";
    memcpy(payload + WIRE_DEVICE_ID + 32, name, strnlen(name, WIRE_NAME));
    sendPlain(TYPE_PAIR_START, payload, sizeof(payload));
}

static void sendPairNonce(void)
{
    uint8_t payload[WIRE_DEVICE_ID + CRYPTO_NONCE_SIZE];
    settingsGetDeviceId(payload);
    memcpy(payload + WIRE_DEVICE_ID, s_pairSwitchNonce, CRYPTO_NONCE_SIZE);
    sendPlain(TYPE_PAIR_NONCE_S, payload, sizeof(payload));
}

static void handlePairKey(const uint8_t *payload, size_t length)
{
    if (s_pairState != PAIR_START || length < 64)
        return;
    memcpy(s_pcPublic, payload, 32);
    memcpy(s_pcCommit, payload + 32, 32);
    cryptoRandom(s_pairSwitchNonce, sizeof(s_pairSwitchNonce)); // only now - after the PC committed
    s_pairState = PAIR_NONCE;
    atomic_store(&s_status, TransportStatus_Pairing);
    sendPairNonce();
}

static void handlePairNonceP(const uint8_t *payload, size_t length)
{
    if (s_pairState != PAIR_NONCE || length < CRYPTO_NONCE_SIZE)
        return;
    uint8_t commit[32];
    cryptoPairCommit(commit, s_pcPublic, s_pairPublic, payload);
    if (memcmp(commit, s_pcCommit, 32) != 0)
    {
        pairingFail(TransportStatus_PairingFailed, "the PC's nonce doesn't match its commitment");
        return;
    }
    uint8_t shared[32];
    cryptoScalarMult(shared, s_pairPrivate, s_pcPublic);
    cryptoPairKey(s_pairKey, shared, s_pairSwitchNonce, payload);
    atomic_store(&s_pairCode, cryptoPairCode(s_pairPublic, s_pcPublic, s_pairSwitchNonce, payload));
    s_pairState = PAIR_CODE;
    atomic_store(&s_status, TransportStatus_PairingCode);
    logWrite("transport", "pairing: code %06u - waiting for the PC to confirm", (unsigned)atomic_load(&s_pairCode));
}

static void handlePairResult(const uint8_t *payload, size_t length)
{
    if (s_pairState != PAIR_CODE || length < 1)
        return;
    if (payload[0] != 1)
    {
        pairingFail(TransportStatus_PairingRefused, "refused on the PC");
        return;
    }
    if (length < 1 + WIRE_HOST_ID + 32)
        return;
    uint8_t deviceId[WIRE_DEVICE_ID], proof[32];
    settingsGetDeviceId(deviceId);
    cryptoPairProof(proof, s_pairKey, deviceId, payload + 1);
    if (memcmp(proof, payload + 1 + WIRE_HOST_ID, 32) != 0)
    {
        pairingFail(TransportStatus_PairingFailed, "the PC's confirmation doesn't check out");
        return;
    }
    char hostId[WIRE_HOST_ID + 1];
    memcpy(hostId, payload + 1, WIRE_HOST_ID);
    hostId[WIRE_HOST_ID] = '\0';
    settingsSetPairing(s_pairKey, hostId);
    s_pairState = PAIR_IDLE;
    disconnect(TransportStatus_Paired);
    logWrite("transport", "paired with PC %s", hostId);
}

// --- Session ---

static void sendHello(void)
{
    uint8_t payload[WIRE_DEVICE_ID + CRYPTO_NONCE_SIZE];
    settingsGetDeviceId(payload);
    memcpy(payload + WIRE_DEVICE_ID, s_switchNonce, CRYPTO_NONCE_SIZE);
    sendPlain(TYPE_HELLO, payload, sizeof(payload));
}

static void handleWelcome(const uint8_t *packet, size_t length)
{
    const size_t aadSize = WIRE_HEADER + CRYPTO_NONCE_SIZE + WIRE_HOST_ID;
    uint8_t pairKey[32];
    char hostId[WIRE_HOST_ID + 1];
    if (length < aadSize + CRYPTO_OVERHEAD || !settingsGetPairing(pairKey, hostId))
        return;

    mutexLock(&s_mutex);
    bool already = s_connected;
    mutexUnlock(&s_mutex);
    if (already)
        return; // a Welcome for a Hello we repeated - this session is up

    CryptoSessionKeys keys;
    cryptoSessionKeys(&keys, pairKey, s_switchNonce, packet + WIRE_HEADER);
    uint64_t counter;
    long size = cryptoOpen(s_plain, keys.encPcToSwitch, keys.macPcToSwitch, packet, length, aadSize, &counter);
    if (size < (long)sizeof(SolarNxSessionConfig))
        return; // not from our PC (or for an older Hello) - keep saying hello

    SolarNxSessionConfig config;
    memcpy(&config, s_plain, sizeof(config));
    networkSetSessionConfig(&config);

    mutexLock(&s_mutex);
    s_keys = keys;
    s_sendCounter = 0;
    memset(&s_replay, 0, sizeof(s_replay));
    cryptoReplayAccept(&s_replay, counter);
    s_connected = true;
    mutexUnlock(&s_mutex);
    atomic_store(&s_lastValidFromPc, svcGetSystemTick());
    atomic_store(&s_status, TransportStatus_Connected);
    logWrite("transport", "connected to the PC (encrypted session)");
}

static void handleSealed(uint8_t type, const uint8_t *packet, size_t length)
{
    mutexLock(&s_mutex);
    bool connected = s_connected;
    CryptoSessionKeys keys = s_keys;
    mutexUnlock(&s_mutex);
    if (!connected)
        return;

    uint64_t counter;
    long size = cryptoOpen(s_plain, keys.encPcToSwitch, keys.macPcToSwitch, packet, length, WIRE_HEADER, &counter);
    if (size < 0)
        return; // forged or damaged
    mutexLock(&s_mutex);
    bool fresh = cryptoReplayAccept(&s_replay, counter);
    mutexUnlock(&s_mutex);
    if (!fresh)
        return;
    atomic_store(&s_lastValidFromPc, svcGetSystemTick());

    const uint8_t *payload = s_plain;
    switch (type)
    {
    case TYPE_END:
        atomic_store(&s_ended, true);
        break;
    case TYPE_VIDEO:
    case TYPE_AUDIO:
    {
        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = htons(type == TYPE_VIDEO ? SOLARNX_VIDEO_PORT : SOLARNX_AUDIO_PORT);
        sendto(s_sock, payload, (size_t)size, 0, (struct sockaddr *)&local, sizeof(local));
        break;
    }
    case TYPE_LIB_OFFER:
        if (size >= 20)
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

            uint8_t reply[12] = {0};
            memcpy(reply, &offerId, 4);
            reply[4] = libraryUdpOffer(hash) ? 1 : 0;
            reply[5] = swapAbxy ? 1 : 0;
            memcpy(reply + 8, &revision, 4);
            sendSealed(TYPE_LIB_REPLY, reply, sizeof(reply));
        }
        break;
    case TYPE_LIB_CHUNK:
        if (size >= 16)
        {
            uint64_t hash;
            uint32_t total, offset;
            memcpy(&hash, payload, 8);
            memcpy(&total, payload + 8, 4);
            memcpy(&offset, payload + 12, 4);
            uint32_t received = libraryUdpChunk(hash, total, offset, payload + 16, (uint32_t)(size - 16));

            uint8_t ack[12];
            memcpy(ack, &hash, 8);
            memcpy(ack + 8, &received, 4);
            sendSealed(TYPE_LIB_ACK, ack, sizeof(ack));
        }
        break;
    default: // KeepaliveAck - its arrival is all that matters
        break;
    }
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

    // Pairing replies and Reject come in clear but show the direct path works - otherwise serverStep
    // would switch to the relay in the middle of pairing.
    if (type == TYPE_REJECT || (type >= TYPE_PAIR_KEY && type <= TYPE_PAIR_RESULT))
        atomic_store(&s_lastValidFromPc, svcGetSystemTick());

    switch (type)
    {
    case TYPE_REJECT:
        if (payloadLength >= 1)
        {
            int status = payload[0] == REJECT_NOT_PAIRED ? TransportStatus_NotPaired
                : payload[0] == REJECT_BUSY ? TransportStatus_Busy
                : payload[0] == REJECT_PAIRING_CLOSED ? TransportStatus_PairingClosed
                : TransportStatus_WrongVersion;
            if (atomic_load(&s_status) != status)
                logWrite("transport", "the PC refused: %s", transportStatusText(status));
            atomic_store(&s_status, status);
        }
        break;
    case TYPE_PAIR_KEY:
        handlePairKey(payload, payloadLength);
        break;
    case TYPE_PAIR_NONCE_P:
        handlePairNonceP(payload, payloadLength);
        break;
    case TYPE_PAIR_RESULT:
        handlePairResult(payload, payloadLength);
        break;
    case TYPE_WELCOME:
        handleWelcome(data, length);
        break;
    default:
        handleSealed(type, data, length);
        break;
    }
}

// --- Rendezvous server ---

static void resolveServer(const char *address)
{
    struct sockaddr_in resolved;
    bool ok = address[0] != '\0' && resolveAddress(address, SOLARNX_RENDEZVOUS_PORT, &resolved);
    mutexLock(&s_mutex);
    s_useServer = address[0] != '\0';
    s_serverValid = ok;
    if (ok)
        s_server = resolved;
    s_peerKnown = false;
    s_relay = false;
    s_pcValid = false;
    s_connected = false;
    mutexUnlock(&s_mutex);
    s_unansweredLookups = 0;
    if (address[0] != '\0')
    {
        if (ok)
            logWrite("transport", "rendezvous server %s -> %s:%d", address, inet_ntoa(resolved.sin_addr), ntohs(resolved.sin_port));
        else
            logWrite("transport", "cannot resolve the rendezvous server '%s'", address);
        atomic_store(&s_status, ok ? TransportStatus_Waiting : TransportStatus_ServerNotFound);
    }
}

static void sendLookup(void)
{
    char key[RV_KEY + 1];
    settingsGetPcId(s_rvPcId);
    settingsGetServerKey(key, sizeof(key));
    memset(s_rvKey, 0, sizeof(s_rvKey));
    memcpy(s_rvKey, key, strnlen(key, RV_KEY));

    uint8_t packet[8 + SETTINGS_HOST_ID_SIZE + RV_KEY];
    writeRvHeader(packet, RV_LOOKUP);
    memset(packet + 8, 0, SETTINGS_HOST_ID_SIZE);
    memcpy(packet + 8, s_rvPcId, strnlen(s_rvPcId, SETTINGS_HOST_ID_SIZE));
    memcpy(packet + 8 + SETTINGS_HOST_ID_SIZE, s_rvKey, RV_KEY);
    mutexLock(&s_mutex);
    if (s_serverValid)
        sendto(s_sock, packet, sizeof(packet), 0, (struct sockaddr *)&s_server, sizeof(s_server));
    mutexUnlock(&s_mutex);
    if (++s_unansweredLookups > 3)
        atomic_store(&s_status, TransportStatus_ServerNoAnswer);
}

// Peer: u8 status, the PC's IPv4[4], u16 port (LE), u8 relay allowed.
static void handlePeer(const uint8_t *payload, size_t length)
{
    if (length < 8)
        return;
    s_unansweredLookups = 0;
    if (payload[0] != 0)
    {
        atomic_store(&s_status, payload[0] == 2 ? TransportStatus_ServerBadKey : TransportStatus_ServerNoPc);
        return;
    }
    uint16_t port;
    memcpy(&port, payload + 5, 2);
    mutexLock(&s_mutex);
    bool changed = !s_peerKnown || memcmp(&s_pc.sin_addr, payload + 1, 4) != 0 || s_pc.sin_port != htons(port);
    memset(&s_pc, 0, sizeof(s_pc));
    s_pc.sin_family = AF_INET;
    memcpy(&s_pc.sin_addr, payload + 1, 4);
    s_pc.sin_port = htons(port);
    s_pcValid = true;
    s_relayAllowed = payload[7] != 0;
    s_peerKnown = true;
    s_peerAt = svcGetSystemTick();
    mutexUnlock(&s_mutex);
    if (changed)
        logWrite("transport", "server: PC %s is at %s:%u (relay %s)", s_rvPcId, inet_ntoa(s_pc.sin_addr), port, payload[7] ? "allowed" : "off");
    int status = atomic_load(&s_status);
    bool pairing = status >= TransportStatus_Pairing && status <= TransportStatus_PairingFailed;
    if (status != TransportStatus_Connected && !pairing)
        atomic_store(&s_status, TransportStatus_Waiting);
}

static bool fromServer(const struct sockaddr_in *from)
{
    mutexLock(&s_mutex);
    bool ok = s_serverValid && from->sin_addr.s_addr == s_server.sin_addr.s_addr && from->sin_port == s_server.sin_port;
    mutexUnlock(&s_mutex);
    return ok;
}

static void handleServerDatagram(const uint8_t *data, size_t length)
{
    uint32_t magic;
    if (length < 8)
        return;
    memcpy(&magic, data, 4);
    if (magic != RV_MAGIC || data[5] != RV_VERSION)
        return;
    if (data[4] == RV_PEER)
        handlePeer(data + 8, length - 8);
    else if (data[4] == RV_RELAY_FROM)
        handleDatagram(data + 8, length - 8);
}

// With a server: no PC id / no answer / not found yet -> false (nothing else to send this round).
// Also switches to the relay when the direct path stays silent.
static bool serverStep(uint64_t now)
{
    mutexLock(&s_mutex);
    bool useServer = s_useServer, serverValid = s_serverValid, peerKnown = s_peerKnown;
    bool connected = s_connected, relay = s_relay, relayAllowed = s_relayAllowed;
    uint64_t peerAt = s_peerAt;
    mutexUnlock(&s_mutex);
    if (!useServer)
        return true;
    if (!serverValid)
        return false;

    char pcId[SETTINGS_HOST_ID_SIZE + 1];
    settingsGetPcId(pcId);
    if (pcId[0] == '\0')
    {
        atomic_store(&s_status, TransportStatus_NoPcId);
        return false;
    }
    if (!peerKnown || (!connected && now - s_lastLookup > LOOKUP_REFRESH))
    {
        sendLookup();
        s_lastLookup = now;
        if (!peerKnown)
            return false;
    }
    bool silent = now - atomic_load(&s_lastValidFromPc) > DIRECT_GRACE;
    if (!connected && !relay && relayAllowed && silent && now - peerAt > DIRECT_GRACE)
    {
        mutexLock(&s_mutex);
        s_relay = true;
        mutexUnlock(&s_mutex);
        logWrite("transport", "no direct path to the PC - using the server's relay");
    }
    return true;
}

// Only what comes from the PC's address is ours - anything else on this port is dropped.
static bool fromPc(const struct sockaddr_in *from)
{
    mutexLock(&s_mutex);
    bool ok = s_pcValid && from->sin_addr.s_addr == s_pc.sin_addr.s_addr && from->sin_port == s_pc.sin_port;
    mutexUnlock(&s_mutex);
    return ok;
}

// Once a second: pairing retries, or Hello until connected, then Keepalive.
static void periodic(uint64_t now)
{
    if (atomic_exchange(&s_pairCancelRequested, false) && s_pairState != PAIR_IDLE)
    {
        s_pairState = PAIR_IDLE;
        disconnect(TransportStatus_Waiting);
    }
    if (atomic_exchange(&s_pairRequested, false))
    {
        disconnect(TransportStatus_Pairing);
        cryptoKeyPair(s_pairPrivate, s_pairPublic);
        s_pairState = PAIR_START;
        s_pairDeadline = now + PAIRING_TIMEOUT;
        logWrite("transport", "pairing started");
    }

    if (!serverStep(now))
        return;

    if (s_pairState != PAIR_IDLE)
    {
        if (now > s_pairDeadline)
            pairingFail(TransportStatus_PairingFailed, "timed out");
        else if (s_pairState == PAIR_START)
            sendPairStart();
        else
            sendPairNonce(); // PAIR_NONCE: fetch the PC's nonce; PAIR_CODE: fetch the result
        return;
    }

    uint8_t key[32];
    char hostId[WIRE_HOST_ID + 1];
    if (!settingsGetPairing(key, hostId))
    {
        int status = atomic_load(&s_status);
        if (status != TransportStatus_NoAddress && status != TransportStatus_BadAddress && status != TransportStatus_PairingRefused && status != TransportStatus_PairingFailed && status < TransportStatus_ServerNotFound)
            atomic_store(&s_status, TransportStatus_NotPaired);
        return;
    }

    mutexLock(&s_mutex);
    bool connected = s_connected;
    mutexUnlock(&s_mutex);
    if (connected && now - atomic_load(&s_lastValidFromPc) > PC_SILENT_RESET)
    {
        disconnect(TransportStatus_Waiting);
        connected = false;
    }
    if (connected)
        sendSealed(TYPE_KEEPALIVE, "", 0);
    else
        sendHello();
}

static void transportThreadFunc(void *arg)
{
    (void)arg;
    char address[SETTINGS_ADDRESS_MAX] = "";
    char resolvedFor[SETTINGS_ADDRESS_MAX] = "\x01"; // never a real address: forces the first resolve
    char server[SETTINGS_ADDRESS_MAX] = "";
    char serverResolvedFor[SETTINGS_ADDRESS_MAX] = "\x01";
    uint64_t lastResolve = 0, lastServerResolve = 0, lastSend = 0;

    while (appIsRunning())
    {
        uint64_t now = svcGetSystemTick();

        settingsGetServer(server, sizeof(server));
        mutexLock(&s_mutex);
        bool serverValid = s_serverValid;
        mutexUnlock(&s_mutex);
        if (strcmp(server, serverResolvedFor) != 0 || (!serverValid && server[0] != '\0' && now - lastServerResolve > RESOLVE_RETRY))
        {
            snprintf(serverResolvedFor, sizeof(serverResolvedFor), "%s", server);
            resolveServer(server);
            lastServerResolve = now;
            resolvedFor[0] = '\x01'; // back to the plain address if the server was removed
        }

        if (server[0] == '\0')
        {
            settingsGetPcAddress(address, sizeof(address));
            mutexLock(&s_mutex);
            bool pcValid = s_pcValid;
            mutexUnlock(&s_mutex);
            if (strcmp(address, resolvedFor) != 0 || (!pcValid && address[0] != '\0' && now - lastResolve > RESOLVE_RETRY))
            {
                snprintf(resolvedFor, sizeof(resolvedFor), "%s", address);
                resolvePc(address);
                lastResolve = now;
            }
        }

        bool pairingNow = atomic_load(&s_pairRequested) || atomic_load(&s_pairCancelRequested);
        if (now - lastSend >= SEND_INTERVAL || pairingNow)
        {
            periodic(now);
            lastSend = now;
        }

        struct sockaddr_in from;
        socklen_t fromLength = sizeof(from);
        ssize_t n = recvfrom(s_sock, s_rxBuffer, sizeof(s_rxBuffer), 0, (struct sockaddr *)&from, &fromLength);
        if (n > 0 && fromServer(&from))
            handleServerDatagram(s_rxBuffer, (size_t)n);
        else if (n > 0 && fromPc(&from))
            handleDatagram(s_rxBuffer, (size_t)n);
    }
}

bool transportStart(void)
{
    mutexInit(&s_mutex);
    cryptoRandom(s_switchNonce, sizeof(s_switchNonce));

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
    case TransportStatus_NotPaired: return "not paired - X > Pair";
    case TransportStatus_Waiting: return "waiting for the PC";
    case TransportStatus_Connected: return "connected (encrypted)";
    case TransportStatus_Busy: return "PC is busy with another Switch";
    case TransportStatus_WrongVersion: return "PC runs another SolarNX version";
    case TransportStatus_Pairing: return "pairing - waiting for the PC";
    case TransportStatus_PairingClosed: return "pairing - open it on the PC first";
    case TransportStatus_PairingCode: return "pairing - compare the code on the PC";
    case TransportStatus_Paired: return "paired";
    case TransportStatus_PairingRefused: return "pairing refused on the PC";
    case TransportStatus_PairingFailed: return "pairing failed";
    case TransportStatus_ServerNotFound: return "server address not found";
    case TransportStatus_ServerNoAnswer: return "the server doesn't answer";
    case TransportStatus_ServerBadKey: return "the server refused the key";
    case TransportStatus_ServerNoPc: return "PC not on the server (is streaming on?)";
    case TransportStatus_NoPcId: return "set the PC ID (shown on the PC)";
    }
    return "";
}

void transportStartPairing(void)
{
    atomic_store(&s_pairRequested, true);
}

void transportCancelPairing(void)
{
    atomic_store(&s_pairCancelRequested, true);
}

uint32_t transportPairingCode(void)
{
    return atomic_load(&s_pairCode);
}

void transportBeginSession(void)
{
    atomic_store(&s_ended, false);
}

bool transportWaitWelcome(void)
{
    while (appIsRunning())
    {
        mutexLock(&s_mutex);
        bool connected = s_connected;
        mutexUnlock(&s_mutex);
        if (connected)
            return true;
        svcSleepThread(50000000ULL); // 50ms - the next Hello (<=1s) brings a Welcome
    }
    return false;
}

bool transportSendInput(const SolarNxHidPacket *pkt)
{
    if (atomic_load(&s_ended))
        return false;
    if (svcGetSystemTick() - atomic_load(&s_lastValidFromPc) > PC_SILENT_END)
        return false;
    return sendSealed(TYPE_INPUT, pkt, sizeof(*pkt));
}

void transportEndSession(void)
{
    for (int i = 0; i < 3; i++) // a lost Bye would leave the PC waiting for its input timeout
        sendSealed(TYPE_BYE, "", 0);
}
