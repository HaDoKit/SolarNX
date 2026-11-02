#include "library.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <switch.h>

#include "../network/network.h"
#include "../common/logger/logger.h"
#include "../common/appstate/appstate.h"
#include "../settings/settings.h"

// Wire format - mirrors SolarNX/Library/LibraryWire.cs, little-endian, one exchange per connection
// (the host dials in, same direction as HID):
//   host -> here   u32 magic "ONXL", u32 version (2), u64 content hash,
//                  u8 button layout (1 = A/B X/Y by label), u8[3] zero, u32 layout revision
//   here -> host   u8 1 = send it / 0 = already have this hash,
//                  u8 button layout, u8[2] zero, u32 layout revision - this side's, after syncing
//                  with the host's (higher revision wins, a tie goes to the host; see settings.h)
//   host -> here   (only if 1) u32 count, then per entry:
//                  u32 id, u16 nameLength, name (UTF-8), u32 coverLength, cover (JPEG)
// A malformed or oversized message is dropped whole; the previous library stays in place.
#define LIBRARY_MAGIC   0x4C584E4Fu
#define LIBRARY_VERSION 2u

// Receiver thread: idle almost all the time (blocked in accept), short bursts while a library
// arrives. Covers go to the heap, so a small stack is plenty. Shares core 0 with the HID thread,
// which only wakes every 23ms.
#define LIBRARY_THREAD_STACK_SIZE 0x8000
#define LIBRARY_THREAD_PRIORITY   0x2C
#define LIBRARY_THREAD_CORE       0

static int s_listenSock = -1;
static Thread s_thread;

static Mutex s_mutex;
static LibraryEntry *s_entries = NULL;
static int s_count = 0;
static uint64_t s_hash = 0; // 0 = nothing received yet (a real payload hash being 0 is irrelevant)
static uint32_t s_version = 0;

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

static void freeEntries(LibraryEntry *entries, int count)
{
    if (entries == NULL)
        return;
    for (int i = 0; i < count; i++)
        free(entries[i].cover);
    free(entries);
}

// Reads the payload part. Returns the new array (count in *outCount), or NULL on any error.
static LibraryEntry *receivePayload(int sock, int *outCount)
{
    uint32_t count;
    if (!recvAll(sock, &count, sizeof(count)) || count > LIBRARY_MAX_ENTRIES)
        return NULL;

    LibraryEntry *entries = (LibraryEntry *)calloc(count > 0 ? count : 1, sizeof(LibraryEntry));
    if (entries == NULL)
        return NULL;

    for (uint32_t i = 0; i < count; i++)
    {
        LibraryEntry *e = &entries[i];
        uint16_t nameLength;
        uint32_t coverLength;

        if (!recvAll(sock, &e->id, sizeof(e->id)) ||
            !recvAll(sock, &nameLength, sizeof(nameLength)) || nameLength > LIBRARY_MAX_NAME_BYTES ||
            !recvAll(sock, e->name, nameLength) ||
            !recvAll(sock, &coverLength, sizeof(coverLength)) || coverLength > LIBRARY_MAX_COVER_BYTES)
        {
            freeEntries(entries, (int)count);
            return NULL;
        }
        e->name[nameLength] = '\0';

        if (coverLength > 0)
        {
            e->cover = (uint8_t *)malloc(coverLength);
            if (e->cover == NULL || !recvAll(sock, e->cover, coverLength))
            {
                freeEntries(entries, (int)count);
                return NULL;
            }
            e->coverSize = coverLength;
        }
    }

    *outCount = (int)count;
    return entries;
}

static void handleConnection(int sock)
{
    // Bounded: a host that stalls mid-transfer must not park this thread forever.
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    uint32_t header[6]; // magic, version, hash (2 words), layout flags, layout revision
    if (!recvAll(sock, header, sizeof(header)))
        return;
    if (header[0] != LIBRARY_MAGIC || header[1] != LIBRARY_VERSION)
    {
        logWrite("library", "unexpected header (magic %08x, version %u) - ignoring", header[0], header[1]);
        return;
    }
    uint64_t hash = (uint64_t)header[2] | ((uint64_t)header[3] << 32);

    settingsSyncButtonLayout((header[4] & 0xFF) != 0, header[5]);
    bool swapAbxy;
    uint32_t layoutRevision;
    settingsGetButtonLayout(&swapAbxy, &layoutRevision);

    mutexLock(&s_mutex);
    bool need = hash != s_hash;
    mutexUnlock(&s_mutex);

    uint8_t reply[8] = {need ? 1 : 0, swapAbxy ? 1 : 0, 0, 0};
    memcpy(&reply[4], &layoutRevision, sizeof(layoutRevision));
    if (send(sock, reply, sizeof(reply), 0) != (ssize_t)sizeof(reply) || !need)
        return;

    int count = 0;
    LibraryEntry *entries = receivePayload(sock, &count);
    if (entries == NULL)
    {
        logWrite("library", "library transfer failed or malformed - keeping the previous one");
        return;
    }

    mutexLock(&s_mutex);
    LibraryEntry *old = s_entries;
    int oldCount = s_count;
    s_entries = entries;
    s_count = count;
    s_hash = hash;
    s_version++;
    mutexUnlock(&s_mutex);

    freeEntries(old, oldCount);
    logWrite("library", "received library: %d game(s)", count);
}

static void libraryThreadFunc(void *arg)
{
    (void)arg;
    // Never joined - like the HID thread, it ends with the process (blocked in accept()).
    while (appIsRunning())
    {
        int sock = accept(s_listenSock, NULL, NULL);
        if (sock < 0)
        {
            svcSleepThread(100000000ULL); // 100ms - don't spin if accept keeps failing
            continue;
        }
        handleConnection(sock);
        close(sock);
    }
}

bool libraryStart(void)
{
    mutexInit(&s_mutex);

    s_listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listenSock < 0)
        return false;

    int reuse = 1;
    setsockopt(s_listenSock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(SOLARNX_LIBRARY_PORT);

    if (bind(s_listenSock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s_listenSock, 1) != 0)
    {
        close(s_listenSock);
        s_listenSock = -1;
        return false;
    }

    if (R_FAILED(threadCreate(&s_thread, libraryThreadFunc, NULL, NULL,
            LIBRARY_THREAD_STACK_SIZE, LIBRARY_THREAD_PRIORITY, LIBRARY_THREAD_CORE)) ||
        R_FAILED(threadStart(&s_thread)))
    {
        close(s_listenSock);
        s_listenSock = -1;
        return false;
    }

    logWrite("library", "library listener up on port %d", SOLARNX_LIBRARY_PORT);
    return true;
}

uint32_t libraryVersion(void)
{
    mutexLock(&s_mutex);
    uint32_t version = s_version;
    mutexUnlock(&s_mutex);
    return version;
}

void libraryLock(void)
{
    mutexLock(&s_mutex);
}

const LibraryEntry *libraryEntries(int *count)
{
    *count = s_count;
    return s_entries;
}

void libraryUnlock(void)
{
    mutexUnlock(&s_mutex);
}
