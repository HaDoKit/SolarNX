// The library channel protocol against in-memory host simulations: v3 handshake
// with payload, the legacy v2 host, and the "already have this hash" short-circuit.
// This file is part of SolarNX (GPL-3.0).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal.h"

#include "test_harness.h"

// --- in-memory I/O ---
typedef struct {
    const uint8_t *in;
    size_t inSize, inPos;
    uint8_t out[256];
    size_t outSize;
} MemIo;

static bool memRead(void *io, void *buf, size_t len)
{
    MemIo *m = (MemIo *)io;
    if (len > m->inSize - m->inPos)
        return false;
    memcpy(buf, m->in + m->inPos, len);
    m->inPos += len;
    return true;
}

static bool memWrite(void *io, const void *buf, size_t len)
{
    MemIo *m = (MemIo *)io;
    if (m->outSize + len > sizeof(m->out))
        return false;
    memcpy(m->out + m->outSize, buf, len);
    m->outSize += len;
    return true;
}

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

// Builds a v3 host stream: 8-byte header, 28-byte version, hash/layout/revision/
// extra, then count + entries (only when withPayload).
static uint8_t *buildV3(size_t *size, uint64_t hash, int withPayload)
{
    size_t cap = 4096;
    uint8_t *b = (uint8_t *)calloc(1, cap); // zero-padded version block
    size_t n = 0;
    put32(b + n, SNX_LIBRARY_MAGIC); n += 4;
    put32(b + n, SNX_LIBRARY_VERSION); n += 4;
    put16(b + n, SNX_PROTOCOL_MAJOR); n += 2;
    put16(b + n, SNX_PROTOCOL_MINOR); n += 2;
    memcpy(b + n, "0.2.5-alpha", 11); n += 24; // app version block, zero-padded
    put64(b + n, hash); n += 8;
    b[n++] = 1; // host's layout: swapped
    b[n++] = 0; b[n++] = 0; b[n++] = 0;
    put32(b + n, 7); n += 4; // layout revision
    put32(b + n, 0); n += 4; // extra length

    if (withPayload)
    {
        put32(b + n, 2); n += 4;
        put32(b + n, 100); n += 4; // id
        put16(b + n, 5); n += 2;
        memcpy(b + n, "Witch", 5); n += 5;
        put32(b + n, 4); n += 4;
        memcpy(b + n, "\xFF\xD8\xFF\xE0", 4); n += 4; // fake JPEG bytes
        put32(b + n, 101); n += 4;
        put16(b + n, 6); n += 2;
        memcpy(b + n, "Hades!", 6); n += 6;
        put32(b + n, 0); n += 4; // no cover
    }
    *size = n;
    return b;
}

int test_library(void)
{
    // --- v3: full handshake, we ask for the payload and parse it ---
    size_t streamSize = 0;
    uint8_t *stream = buildV3(&streamSize, UINT64_C(0xABCD1234), 1);
    MemIo io = {.in = stream, .inSize = streamSize};

    SnxLibraryInfo info;
    size_t count = 0;
    SnxLibEntryRaw *entries =
        snx__library_session(memRead, memWrite, &io, "core-test", false, 0, &info, &count);

    tcheck(entries != NULL, "v3: payload received");
    tcheck(count == 2, "v3: two entries");
    tcheck(entries != NULL && entries[0].id == 100 && strcmp(entries[0].name, "Witch") == 0,
           "v3: first entry");
    tcheck(entries != NULL && entries[0].coverSize == 4 &&
               memcmp(entries[0].cover, "\xFF\xD8\xFF\xE0", 4) == 0,
           "v3: cover bytes");
    tcheck(entries != NULL && entries[1].id == 101 && strcmp(entries[1].name, "Hades!") == 0,
           "v3: second entry");
    tcheck(entries != NULL && entries[1].cover == NULL && entries[1].coverSize == 0,
           "v3: coverless entry");

    tcheck(info.pcAnnounced && info.pc.major == 1 && info.pc.minor == 1 &&
               strcmp(info.pc.app, "0.2.5-alpha") == 0,
           "v3: pc version parsed");
    tcheck(info.compatible, "v3: same major -> compatible");
    tcheck(info.requested, "v3: requested the payload");
    tcheck(info.buttonLayout, "v3: host layout flag");
    tcheck(info.layoutRevision == 7, "v3: layout revision");

    // Reply: 40 bytes, [need=1][layout=1][0][0] + revision + own version + extra=0.
    tcheck(io.outSize == 40, "v3: 40-byte reply");
    tcheck(io.out[0] == 1 && io.out[1] == 1, "v3: reply asks + echoes layout");
    uint32_t rev;
    memcpy(&rev, io.out + 4, 4);
    tcheck(rev == 7, "v3: reply revision");
    uint16_t major, minor;
    memcpy(&major, io.out + 8, 2);
    memcpy(&minor, io.out + 10, 2);
    tcheck(major == SNX_PROTOCOL_MAJOR && minor == SNX_PROTOCOL_MINOR, "v3: own version sent");
    tcheck(io.out[12] == 'c' && io.out[13] == 'o', "v3: own app version sent");

    snx__library_free(entries, count);
    free(stream);

    // --- same hash: answer "already have it", no payload follows ---
    stream = buildV3(&streamSize, UINT64_C(0xABCD1234), 0);
    MemIo io2 = {.in = stream, .inSize = streamSize};
    entries = snx__library_session(memRead, memWrite, &io2, "core-test", true,
                                   UINT64_C(0xABCD1234), &info, &count);
    tcheck(entries == NULL, "hash: no payload when unchanged");
    tcheck(!info.requested, "hash: not requested");
    tcheck(io2.outSize == 40 && io2.out[0] == 0, "hash: reply says 'have it'");
    free(stream);

    // --- legacy v2 host: 24-byte header, 8-byte reply, still answered ---
    uint8_t legacy[24];
    put32(legacy, SNX_LIBRARY_MAGIC);
    put32(legacy + 4, SNX_LIBRARY_LEGACY_VERSION);
    put64(legacy + 8, UINT64_C(0x11));
    legacy[16] = 0; // layout
    legacy[17] = legacy[18] = legacy[19] = 0;
    put32(legacy + 20, 3); // revision
    MemIo io3 = {.in = legacy, .inSize = sizeof(legacy)};

    entries = snx__library_session(memRead, memWrite, &io3, "core-test", false, 0, &info, &count);
    tcheck(entries == NULL, "legacy: nothing to take (no payload in the stream)");
    tcheck(!info.pcAnnounced && info.pc.major == 1 && info.pc.minor == 0,
           "legacy: 1.0 assumed");
    tcheck(info.compatible, "legacy: same major");
    tcheck(io3.outSize == 8 && io3.out[0] == 1, "legacy: 8-byte reply asks for the payload");
    uint32_t lrev;
    memcpy(&lrev, io3.out + 4, 4);
    tcheck(lrev == 3, "legacy: reply revision");

    // --- bad magic: ignored whole ---
    uint8_t garbage[56];
    memset(garbage, 0xEE, sizeof(garbage));
    MemIo io4 = {.in = garbage, .inSize = sizeof(garbage)};
    entries = snx__library_session(memRead, memWrite, &io4, "core-test", false, 0, &info, &count);
    tcheck(entries == NULL && io4.outSize == 0, "bad magic: ignored, no reply");

    return 0;
}
