// Route detection: LAN vs Internet. Pure classification is kept separate from the
// system-interface walk so it can be unit-tested by including this file.
// This file is part of SolarNX (GPL-3.0).

#include "solarnx/route.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ifaddrs.h>

const char *snx_path_name(SnxPath path)
{
    switch (path)
    {
    case SNX_PATH_LAN:      return "LAN";
    case SNX_PATH_INTERNET: return "Internet";
    default:                return "unknown";
    }
}

// A local interface as the classifier sees it; every field is in network byte
// order, straight from sockaddr_in.
typedef struct {
    uint32_t addr;
    uint32_t mask;
    int      loopback;
} SnxRouteIface;

// The testable core (declared in internal.h): would traffic to `host` (network
// byte order) stay on one of the given local subnets? Loopback counts as LAN;
// anything else is Internet.
SnxPath snx__route_classify(uint32_t host, const SnxRouteIface *ifaces, size_t count)
{
    if (host == htonl(INADDR_LOOPBACK))
        return SNX_PATH_LAN;

    for (size_t i = 0; i < count; i++)
    {
        if (ifaces[i].loopback)
            continue;
        if ((host & ifaces[i].mask) == (ifaces[i].addr & ifaces[i].mask))
            return SNX_PATH_LAN;
    }
    return SNX_PATH_INTERNET;
}

// Public entry: resolve, then walk the system interfaces.
SnxPath snx_route_detect(const char *host, char *resolved, size_t resolved_size)
{
    if (host == NULL || *host == '\0')
        return SNX_PATH_UNKNOWN;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // the SolarNX host stack is IPv4 today
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL)
        return SNX_PATH_UNKNOWN;

    struct sockaddr_in *ai = (struct sockaddr_in *)(void *)res->ai_addr;
    uint32_t hostAddr = ai->sin_addr.s_addr; // network byte order

    if (resolved != NULL && resolved_size > 0)
    {
        if (inet_ntop(AF_INET, &ai->sin_addr, resolved, (socklen_t)resolved_size) == NULL)
            resolved[0] = '\0';
    }

    SnxRouteIface ifaces[16];
    size_t n = 0;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0)
    {
        for (struct ifaddrs *it = ifa; it != NULL && n < 16; it = it->ifa_next)
        {
            if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET)
                continue;
            struct sockaddr_in *sa = (struct sockaddr_in *)(void *)it->ifa_addr;
            ifaces[n].addr = sa->sin_addr.s_addr;
            ifaces[n].loopback = (it->ifa_flags & IFF_LOOPBACK) != 0;
            ifaces[n].mask = 0xFFFFFFFF;
            if (it->ifa_netmask != NULL && it->ifa_netmask->sa_family == AF_INET)
            {
                struct sockaddr_in *sm = (struct sockaddr_in *)(void *)it->ifa_netmask;
                ifaces[n].mask = sm->sin_addr.s_addr;
            }
            n++;
        }
        freeifaddrs(ifa);
    }

    SnxPath path = snx__route_classify(hostAddr, ifaces, n);
    freeaddrinfo(res);
    return path;
}
