// Path (channel) detection: given the PC's address, decide whether it is reachable
// directly on the LAN or needs the encrypted Internet transport. The whole point:
// the caller never picks a channel manually and never restarts - this is re-run
// whenever the host address changes or a fresh connect cycle starts.
//
// This file is part of SolarNX (GPL-3.0).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNX_PATH_UNKNOWN = 0,
    SNX_PATH_LAN,      // same subnet / loopback: direct sockets, PC dials in
    SNX_PATH_INTERNET, // routed: the whole session over the encrypted UDP transport
} SnxPath;

const char *snx_path_name(SnxPath path);

// Resolves `host` (IP or name) and classifies it against the system's interfaces.
// On success returns the path and, if `resolved` is non-NULL, copies the resolved
// textual address. SNX_PATH_UNKNOWN means the address did not resolve.
SnxPath snx_route_detect(const char *host, char *resolved, size_t resolved_size);

#ifdef __cplusplus
}
#endif
