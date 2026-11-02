#include "network.h"

#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"
#include "../settings/settings.h"
#include "../transport/transport.h"

_Static_assert(sizeof(SolarNxHidPacket) == SOLARNX_HID_PAYLOAD_SIZE,
    "SolarNxHidPacket must stay exactly SOLARNX_HID_PAYLOAD_SIZE bytes - see network.h");
_Static_assert(sizeof(SolarNxSessionConfig) == SOLARNX_SESSION_CONFIG_SIZE,
    "SolarNxSessionConfig must stay exactly SOLARNX_SESSION_CONFIG_SIZE bytes - see network.h");

static const uint8_t s_hidMarker[SOLARNX_HID_MARKER_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

static int s_listenSock = -1;
static int s_clientSock = -1;

// Experimental Internet mode (settings.h, read once at startup): the HID calls below go to
// transport.c's UDP session instead of the TCP socket.
static bool s_internet = false;

// Guards the two fields below - written by networkAcceptHid() (network thread), read by
// networkWaitForSessionConfig() (called from video.c on the main thread, and potentially audio.c
// on its own thread).
static Mutex s_sessionConfigMutex;
static SolarNxSessionConfig s_sessionConfig;
static bool s_sessionConfigReady = false;

static bool sendAll(int sock, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(sock, p + sent, len - sent, 0);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recvAll(int sock, void *data, size_t len)
{
    uint8_t *p = (uint8_t *)data;
    size_t received = 0;
    while (received < len)
    {
        ssize_t n = recv(sock, p + received, len - received, 0);
        if (n <= 0)
            return false;
        received += (size_t)n;
    }
    return true;
}

bool networkInit(void)
{
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    // Concurrently open once HID/video/audio are all running: HID's listen socket + its accepted
    // client socket (2), video's UDP socket (1), audio's UDP socket (1), and - in a DEBUG build -
    // logger.c's nxlink debug connection (1) = 5 persistent sockets across 3 threads, previously
    // undercounted as "one per stream" (6 covered only 3). Confirmed live: running HID+video+audio
    // together crashed after ~2s under real concurrent load (not immediately at socket-open time),
    // consistent with transient bsd:u session pressure from overlapping blocking calls across
    // threads, not just the static socket count - raised well past the known persistent-socket
    // count to leave headroom for that concurrency, not just the minimum that fits today.
    cfg.num_bsd_sessions = 12;

    // libnx's default UDP receive buffer is 0xA500 (~42KB, ~32 of the host's 1316-byte datagrams).
    // The host sends an H.264 keyframe every ~0.5s (VideoEncoder.cs's -g) and at this bitrate a
    // keyframe is often 60-150KB, pushed out as one burst at wired-LAN speed. Whatever doesn't fit
    // in the socket buffer before libavformat's UDP thread drains it is silently dropped - and a
    // keyframe missing its tail corrupts every frame predicted from it until the next keyframe,
    // which is exactly the reported "water running down glass for up to a second, then clean"
    // artifact. 256KB holds a worst-case keyframe burst with room to spare (~0.5s of stream at
    // 4Mbps). Only memory cost: the BSD transfer memory libnx allocates grows by about 0.85MB
    // (udp_rx_buf_size x sb_efficiency). video.c's URL also asks for a matching SO_RCVBUF
    // (buffer_size) - libavformat otherwise sets its own, smaller, default on the socket.
    cfg.udp_rx_buf_size = 0x40000;

    if (R_FAILED(socketInitialize(&cfg)))
        return false;

    mutexInit(&s_sessionConfigMutex);
    s_internet = settingsGetConnectionMode() == SettingsConnection_Internet;

    s_listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listenSock < 0)
        return false;

    int reuse = 1;
    setsockopt(s_listenSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SOLARNX_HID_PORT);

    if (bind(s_listenSock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return false;

    if (listen(s_listenSock, 1) != 0)
        return false;

    return true;
}

// True if the peer already closed/reset this connection. The host never sends anything after
// SessionConfig, so a non-blocking peek that reports EOF (0) or a hard error means it's gone, while
// EAGAIN means "open, nothing to read" - a live session.
static bool connectionAlreadyClosed(int sock)
{
    uint8_t probe;
    ssize_t n = recv(sock, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0)
        return true;
    return n < 0 && errno != EAGAIN && errno != EWOULDBLOCK;
}

void networkSetSessionConfig(const SolarNxSessionConfig *config)
{
    mutexLock(&s_sessionConfigMutex);
    s_sessionConfig = *config;
    s_sessionConfigReady = true;
    mutexUnlock(&s_sessionConfigMutex);
}

bool networkAcceptHid(void)
{
    if (s_internet)
    {
        transportBeginSession();
        return transportWaitWelcome();
    }

    if (s_listenSock < 0)
        return false;

    // Loops only to skip stale connections (see connectionAlreadyClosed below).
    while (1)
    {
        struct sockaddr_in clientAddr;
        socklen_t addrLen = sizeof(clientAddr);
        memset(&clientAddr, 0, sizeof(clientAddr));

        int sock = accept(s_listenSock, (struct sockaddr *)&clientAddr, &addrLen);
        if (sock < 0)
        {
            logWrite("network", "accept() failed (errno %d)", errno);
            return false;
        }

        // Without this, Nagle's algorithm can batch/delay these small (208-byte), frequent (~43Hz)
        // sends waiting for an ACK - a well-known source of exactly the kind of several-hundred-ms
        // input lag confirmed live here, even though the host already sets NoDelay on its end
        // (SessionController.cs) - that only affects the host's own socket, not this one, and this
        // side is the one actually producing the small frequent sends.
        int noDelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

        // sendAll()'s send() calls are otherwise unbounded blocking - if the host ever stops draining
        // this socket for any reason (confirmed live: HID silently stopped responding after several
        // minutes, no error logged anywhere, consistent with the send thread being stuck inside a
        // blocking send() forever once the TCP send buffer filled), the thread would hang forever
        // with nothing to log, since it never gets back out of the syscall to log anything. This turns
        // that into a bounded wait: send() starts returning -1/EAGAIN-ish behavior once nothing drains
        // for this long, which sendAll() (via networkSendHid's caller) already treats as a failure -
        // triggering the normal reconnect path instead of hanging indefinitely.
        struct timeval sendTimeout;
        sendTimeout.tv_sec = 3;
        sendTimeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

        // The host (SessionController.cs) writes this immediately after connecting, before any
        // SolarNxHidPacket traffic starts the other way - block reading it now, before this accept is
        // considered successful. A failure here (connection dropped mid-handshake, etc.) is treated
        // exactly like a failed accept - the caller goes back to waiting for a fresh connection.
        // A connection that drops mid-handshake is skipped, not fatal: this used to return false,
        // and the caller then ended the HID thread for good - confirmed live: one failed handshake
        // right after a library game was picked left no HID session at all (black screen, and the
        // hold-Minus menu gesture - detected in the HID loop - dead too).
        SolarNxSessionConfig config;
        if (!recvAll(sock, &config, sizeof(config)))
        {
            logWrite("network", "HID handshake failed (errno %d) - waiting for the next connection", errno);
            close(sock);
            continue;
        }

        // While the app-selection menu is up nothing calls accept(), but the kernel still completes
        // the host's connects into the listen backlog; the host gives up on each after 3s of no HID data
        // and closes it. Confirmed live: right after a library game was picked, the first accept()
        // returned such a dead connection, its SessionConfig was still buffered, the first send failed -
        // and main.c read that as "the host ended the library session" and went straight back to the
        // menu, so the pick never reached the host at all. Skip anything already closed and wait for a
        // live connect (the host retries every ~1s).
        if (connectionAlreadyClosed(sock))
        {
            logWrite("network", "skipping a stale HID connection (host already closed it while we weren't accepting)");
            close(sock);
            continue;
        }

        mutexLock(&s_sessionConfigMutex);
        s_sessionConfig = config;
        s_sessionConfigReady = true;
        mutexUnlock(&s_sessionConfigMutex);

        s_clientSock = sock;
        return true;
    }
}

bool networkWaitForSessionConfig(SolarNxSessionConfig *out)
{
    while (appMainLoop()) // main thread only - see network.h
    {
        mutexLock(&s_sessionConfigMutex);
        bool ready = s_sessionConfigReady;
        if (ready)
            *out = s_sessionConfig;
        mutexUnlock(&s_sessionConfigMutex);

        if (ready)
            return true;

        svcSleepThread(50000000ULL); // 50ms - only actually waited on before the first session ever connects
    }

    return false;
}

bool networkSendHid(const SolarNxHidPacket *pkt)
{
    if (s_internet)
        return transportSendInput(pkt);

    if (s_clientSock < 0)
        return false;

    if (!sendAll(s_clientSock, s_hidMarker, sizeof(s_hidMarker)))
        return false;

    return sendAll(s_clientSock, pkt, sizeof(*pkt));
}

void networkCloseHidClient(void)
{
    if (s_internet)
    {
        transportEndSession();
        return;
    }

    if (s_clientSock >= 0)
    {
        close(s_clientSock);
        s_clientSock = -1;
    }
}

void networkExit(void)
{
    networkCloseHidClient();

    if (s_listenSock >= 0)
    {
        close(s_listenSock);
        s_listenSock = -1;
    }

    socketExit();
}
