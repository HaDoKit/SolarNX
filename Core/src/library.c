// The library channel protocol, lifted from Client/library/library.c onto I/O
// callbacks so the same code serves TCP sockets (LAN path) and, later, the
// reassembled Internet-transport frames. Wire format: see wire.h.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"

#include <stdlib.h>
#include <string.h>

// controlReadVersion's twin: u16 major, u16 minor, char[24] (zero-padded) -> printable.
static void readPeerVersion(const uint8_t *wire, SnxPeerVersion *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(&out->major, wire, 2);
    memcpy(&out->minor, wire + 2, 2);
    memcpy(out->app, wire + 4, SNX_APP_VERSION_SIZE);
    out->app[SNX_APP_VERSION_SIZE] = '\0';
    for (char *c = out->app; *c; c++)
        if ((unsigned char)*c < 0x20)
            *c = '?';
    if (out->app[0] == '\0')
        strcpy(out->app, "?");
}

static void writeOwnVersion(uint8_t *out, const char *appVersion)
{
    memset(out, 0, SNX_VERSION_WIRE_SIZE);
    uint16_t major = SNX_PROTOCOL_MAJOR, minor = SNX_PROTOCOL_MINOR;
    memcpy(out, &major, 2);
    memcpy(out + 2, &minor, 2);
    strncpy((char *)out + 4, appVersion != NULL ? appVersion : "solarnx-core",
            SNX_APP_VERSION_SIZE - 1);
}

static SnxLibEntryRaw *receivePayload(SnxIoRead rd, void *io, size_t *outCount)
{
    uint32_t count;
    if (!rd(io, &count, sizeof(count)) || count > SNX_LIBRARY_MAX_ENTRIES)
        return NULL;

    SnxLibEntryRaw *entries = (SnxLibEntryRaw *)calloc(count > 0 ? count : 1, sizeof(*entries));
    if (entries == NULL)
        return NULL;

    for (uint32_t i = 0; i < count; i++)
    {
        SnxLibEntryRaw *e = &entries[i];
        uint16_t nameLength;
        uint32_t coverLength;

        if (!rd(io, &e->id, sizeof(e->id)) ||
            !rd(io, &nameLength, sizeof(nameLength)) || nameLength > SNX_LIBRARY_MAX_NAME_BYTES ||
            !rd(io, e->name, nameLength) ||
            !rd(io, &coverLength, sizeof(coverLength)) || coverLength > SNX_LIBRARY_MAX_COVER_BYTES)
        {
            snx__library_free(entries, count);
            return NULL;
        }
        e->name[nameLength] = '\0';

        if (coverLength > 0)
        {
            e->cover = (uint8_t *)malloc(coverLength);
            if (e->cover == NULL || !rd(io, e->cover, coverLength))
            {
                snx__library_free(entries, count);
                return NULL;
            }
            e->coverSize = coverLength;
        }
    }

    *outCount = count;
    return entries;
}

void snx__library_free(SnxLibEntryRaw *entries, size_t count)
{
    if (entries == NULL)
        return;
    for (size_t i = 0; i < count; i++)
        free(entries[i].cover);
    free(entries);
}

SnxLibEntryRaw *snx__library_session(SnxIoRead rd, SnxIoWrite wr, void *io,
                                     const char *ownAppVersion,
                                     bool haveHash, uint64_t lastHash,
                                     SnxLibraryInfo *info, size_t *count)
{
    *count = 0;
    memset(info, 0, sizeof(*info));

    uint32_t head[2]; // magic, version
    if (!rd(io, head, sizeof(head)))
        return NULL;
    if (head[0] != SNX_LIBRARY_MAGIC ||
        (head[1] != SNX_LIBRARY_LEGACY_VERSION && head[1] < SNX_LIBRARY_VERSION))
        return NULL;
    bool legacy = head[1] == SNX_LIBRARY_LEGACY_VERSION;
    info->pcAnnounced = !legacy;

    // Version 2 = hash, layout, revision (16 bytes); 3 = version[28] first, then the
    // same plus the extra length (48 bytes).
    uint8_t rest[SNX_VERSION_WIRE_SIZE + 20];
    size_t restSize = legacy ? 16 : sizeof(rest);
    if (!rd(io, rest, restSize))
        return NULL;

    if (!legacy)
    {
        readPeerVersion(rest, &info->pc);
        uint32_t extra;
        memcpy(&extra, rest + SNX_VERSION_WIRE_SIZE + 16, 4);
        uint8_t skip[64];
        while (extra > 0) // a newer minor's appended fields - not ours to read
        {
            size_t chunk = extra < sizeof(skip) ? extra : sizeof(skip);
            if (!rd(io, skip, chunk))
                return NULL;
            extra -= (uint32_t)chunk;
        }
    }
    else
    {
        info->pc.major = 1;
        info->pc.minor = 0;
        strcpy(info->pc.app, "?");
    }
    info->compatible = info->pc.major == SNX_PROTOCOL_MAJOR;

    const uint8_t *fields = rest + (legacy ? 0 : SNX_VERSION_WIRE_SIZE);
    memcpy(&info->hash, fields, 8);
    info->buttonLayout = fields[8] != 0;
    memcpy(&info->layoutRevision, fields + 12, 4);

    // Reply: our (echoed) layout + revision + own version + zero extra. Another major
    // is still answered - the PC learns our version and can say "update" - but we take
    // no payload from it.
    bool need = info->compatible && !(haveHash && info->hash == lastHash);
    info->requested = need;

    uint8_t reply[8 + SNX_VERSION_WIRE_SIZE + 4] = {
        (uint8_t)(need ? 1 : 0), (uint8_t)(info->buttonLayout ? 1 : 0), 0, 0};
    memcpy(reply + 4, &info->layoutRevision, 4);
    if (!legacy)
        writeOwnVersion(reply + 8, ownAppVersion);
    size_t replySize = legacy ? 8 : sizeof(reply);
    if (!wr(io, reply, replySize) || !need)
        return NULL;

    return receivePayload(rd, io, count);
}
