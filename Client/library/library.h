#pragma once

#include <stdbool.h>
#include <stdint.h>

// The host's game library as last pushed over SOLARNX_LIBRARY_PORT (see library.c for the wire
// format, mirrored from SolarNX/Library/LibraryWire.cs). Kept in memory only - the host re-sends it
// whenever it changes, and on every app start (the hash starts empty).

#define LIBRARY_MAX_ENTRIES     256
#define LIBRARY_MAX_NAME_BYTES  255
#define LIBRARY_MAX_COVER_BYTES (512 * 1024) // == SolarNX CoverImage.MaxBytes

typedef struct {
    uint32_t id;                              // GameEntry.Id - sent back in SolarNxHidPacket.libraryEntryId
    char name[LIBRARY_MAX_NAME_BYTES + 1];    // UTF-8, NUL-terminated
    uint8_t *cover;                           // JPEG (240x360), NULL if none
    uint32_t coverSize;
} LibraryEntry;

// Opens the listen socket and starts the receiver thread. Requires the socket service (call after
// networkInit()). Start it before the first menu so the list can arrive while the menu is up.
bool libraryStart(void);

// Bumped every time a new library is stored - lets the menu notice an update and rebuild.
uint32_t libraryVersion(void);

// The current entries, valid only between libraryLock() and libraryUnlock() (the receiver swaps
// the whole set under the same lock). Keep the critical section short - e.g. just long enough to
// turn the covers into textures.
void libraryLock(void);
const LibraryEntry *libraryEntries(int *count);
void libraryUnlock(void);
