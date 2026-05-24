#pragma once

#ifdef __SWITCH__

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#ifndef ESHUTDOWN
#define ESHUTDOWN EPIPE
#endif

// libnx networking headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/select.h>
#include <arpa/inet.h> // libnx uses this path now for inet_ntop etc.
#include <poll.h>

struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int    ipv6mr_interface;
};

// Bypassing accept4 in httplib.h (causes build errors on Switch)
#ifdef SOCK_CLOEXEC
#undef SOCK_CLOEXEC
#endif

#ifdef __cplusplus
extern "C" {
#endif

inline time_t timegm(struct tm* tm) {
    time_t ret;
    char* tz;
    tz = getenv("TZ");
    setenv("TZ", "", 1);
    tzset();
    ret = mktime(tm);
    if (tz)
        setenv("TZ", tz, 1);
    else
        unsetenv("TZ");
    tzset();
    return ret;
}

inline int posix_fallocate(int fd, off_t offset, off_t len) {
    (void)fd; (void)offset; (void)len;
    return 0;
}

char* if_indextoname(unsigned int ifindex, char* ifname);
unsigned int if_nametoindex(const char* ifname);
int pipe(int pipefd[2]);
int pause(void);

#ifdef __cplusplus
}
#endif

#endif // __SWITCH__
