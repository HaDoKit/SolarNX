// Route classification: LAN vs Internet from the local interface list.
// This file is part of SolarNX (GPL-3.0).

#include <arpa/inet.h>

#include "internal.h"
#include "solarnx/route.h"

#include "test_harness.h"

static SnxRouteIface mk(const char *addr, const char *mask, int loopback)
{
    SnxRouteIface f;
    f.addr = inet_addr(addr);
    f.mask = inet_addr(mask);
    f.loopback = loopback;
    return f;
}

int test_route(void)
{
    SnxRouteIface wifi = mk("192.168.1.5", "255.255.255.0", 0);
    SnxRouteIface lo = mk("127.0.0.1", "255.0.0.0", 1);
    SnxRouteIface ifaces[2] = {wifi, lo};

    // Same subnet -> direct LAN path.
    tcheck(snx__route_classify(inet_addr("192.168.1.10"), ifaces, 2) == SNX_PATH_LAN,
           "same /24 subnet is LAN");

    // A private address on another subnet is still routed, not direct.
    tcheck(snx__route_classify(inet_addr("192.168.2.10"), ifaces, 2) == SNX_PATH_INTERNET,
           "neighbour subnet is Internet");
    tcheck(snx__route_classify(inet_addr("10.0.0.5"), ifaces, 2) == SNX_PATH_INTERNET,
           "unrouted private range is Internet");

    // Public address -> Internet.
    tcheck(snx__route_classify(inet_addr("8.8.8.8"), ifaces, 2) == SNX_PATH_INTERNET,
           "public address is Internet");

    // Loopback counts as LAN even though its interface is skipped for subnet math.
    tcheck(snx__route_classify(inet_addr("127.0.0.1"), ifaces, 2) == SNX_PATH_LAN,
           "loopback is LAN");

    // A loopback interface must not drag its whole (fake) subnet in as LAN.
    tcheck(snx__route_classify(inet_addr("127.9.9.9"), ifaces, 2) == SNX_PATH_INTERNET,
           "loopback interface subnet ignored");

    // No interfaces at all: nothing is direct.
    tcheck(snx__route_classify(inet_addr("192.168.1.10"), ifaces, 0) == SNX_PATH_INTERNET,
           "no interfaces -> Internet");

    return 0;
}
