// POSIX socket helpers for the SolarNX LAN channels.
// This file is part of SolarNX (GPL-3.0).

#include "internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int snx__tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int snx__accept_ms(int listenFd, int timeout_ms)
{
    struct pollfd pfd = {.fd = listenFd, .events = POLLIN, .revents = 0};
    int n = poll(&pfd, 1, timeout_ms);
    if (n == 0)
        return -1;
    if (n < 0)
        return errno == EINTR ? -1 : -2;
    int fd = accept(listenFd, NULL, NULL);
    return fd >= 0 ? fd : -2;
}

void snx__io_timeout(int fd, int seconds)
{
    struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool snx__read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len)
    {
        ssize_t n = recv(fd, p + got, len - got, 0);
        if (n <= 0)
        {
            if (n < 0 && (errno == EINTR))
                continue;
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

bool snx__write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}
