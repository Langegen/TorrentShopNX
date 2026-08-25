// PC-only shim: POSIX socket API on top of Winsock2, so the torrent engine
// (written against libnx/newlib sockets) compiles with mingw64.
// Include path: -Ipctest\compat comes first, so <sys/socket.h>,
// <arpa/inet.h>, <netinet/in.h> resolve here; <unistd.h> and <fcntl.h>
// come from mingw64 itself (we only override fcntl() via a macro).

#ifndef PC_POSIX_SOCKET_COMPAT_H
#define PC_POSIX_SOCKET_COMPAT_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <errno.h>
#include <fcntl.h>

//-----------------------------------------------------------------------------
// Types/constants missing on Winsock
//-----------------------------------------------------------------------------
#ifndef socklen_t
typedef int socklen_t;
#endif

typedef unsigned int nfds_t;

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
// winsock2.h forward-declares `struct pollfd` for WSAPoll; complete it.
struct pollfd { SOCKET fd; short events; short revents; };
#endif

#ifndef POLLIN
#define POLLIN POLLRDNORM
#endif
#ifndef POLLOUT
#define POLLOUT POLLWRNORM
#endif

#ifndef TCP_NODELAY
#define TCP_NODELAY 0x0001
#endif

// mingw64's fcntl.h has no POSIX command flags (and no fcntl() at all).
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif

// windows.h (via ddeml.h) defines ST_CONNECTED which collides with the
// engine's utp_nb session state enum. These are Windows-only symbols that
// nothing in the engine uses.
#ifdef ST_CONNECTED
#undef ST_CONNECTED
#endif
#ifdef ST_CONNECTING
#undef ST_CONNECTING
#endif
#ifdef ST_EOF
#undef ST_EOF
#endif
#ifdef ST_ERROR
#undef ST_ERROR
#endif

// mingw64's CRT has rand() but not random() (jech's DHT uses the latter).
#include <stdlib.h>
static inline long pc_random(void) { return rand(); }
#define random pc_random

//-----------------------------------------------------------------------------
// errno translation
//-----------------------------------------------------------------------------
static inline int tsnx_ws_errno(int w) {
    switch (w) {
        case WSAEINTR:        return EINTR;
        case WSAEWOULDBLOCK:  return EWOULDBLOCK;
        case WSAEINPROGRESS:  return EWOULDBLOCK; /* non-blocking connect in progress */
        case WSAEALREADY:     return EWOULDBLOCK;
        case WSAETIMEDOUT:    return ETIMEDOUT;
        case WSAECONNRESET:   return ECONNRESET;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAENOTCONN:     return ENOTCONN;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAENETUNREACH:  return EHOSTUNREACH;
        case WSAEADDRINUSE:   return EADDRINUSE;
        case WSAEINVAL:       return EINVAL;
        case WSAEMSGSIZE:     return EMSGSIZE;
        case WSAEBADF:        return EBADF;
        case WSA_NOT_ENOUGH_MEMORY: return ENOMEM;
        case WSAEACCES:       return EPERM;
        default:              return EIO;
    }
}

//-----------------------------------------------------------------------------
// Function wrappers. Defined before the name macros below so the macros do
// not rewrite these definitions themselves.
//-----------------------------------------------------------------------------
// Winsock needs WSAStartup before the first socket call; on the Switch this
// is done by socketInit() in main(). Lazy-init on the first socket().
static inline void tsnx_wsa_init(void) {
    static int done = 0;
    if (!done) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) { /* fall back to unusable */ }
        done = 1;
    }
}
static inline int tsnx_socket(int af, int type, int proto) {
    tsnx_wsa_init();
    SOCKET s = socket(af, type, proto);
    if (s == INVALID_SOCKET) { errno = tsnx_ws_errno(WSAGetLastError()); return -1; }
    return (int)s;
}
static inline int tsnx_bind(int fd, const struct sockaddr *addr, socklen_t len) {
    if (bind((SOCKET)fd, addr, (int)len) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline int tsnx_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (connect((SOCKET)fd, addr, (int)len) != 0) {
        int w = WSAGetLastError();
        if (w != WSAEWOULDBLOCK && w != WSAEINPROGRESS && w != WSAEALREADY) {
            errno = tsnx_ws_errno(w); return -1;
        }
        errno = EINPROGRESS; return -1;
    }
    return 0;
}
static inline int tsnx_listen(int fd, int backlog) {
    if (listen((SOCKET)fd, backlog) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline int tsnx_accept(int fd, struct sockaddr *addr, socklen_t *len) {
    SOCKET s = accept((SOCKET)fd, addr, (int *)len);
    if (s == INVALID_SOCKET) { errno = tsnx_ws_errno(WSAGetLastError()); return -1; }
    return (int)s;
}
static inline int tsnx_close(int fd) {
    if (fd < 0) { errno = EBADF; return -1; }
    if (closesocket((SOCKET)fd) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline int tsnx_shutdown(int fd, int how) {
    if (shutdown((SOCKET)fd, how) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline ssize_t tsnx_recv(int fd, void *buf, size_t len, int flags) {
    int n = recv((SOCKET)fd, (char *)buf, (int)len, flags);
    if (n == SOCKET_ERROR) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return (ssize_t)n;
}
static inline ssize_t tsnx_send(int fd, const void *buf, size_t len, int flags) {
    int n = send((SOCKET)fd, (const char *)buf, (int)len, flags);
    if (n == SOCKET_ERROR) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return (ssize_t)n;
}
static inline ssize_t tsnx_recvfrom(int fd, void *buf, size_t len, int flags,
                                    struct sockaddr *from, socklen_t *fromlen) {
    int n = recvfrom((SOCKET)fd, (char *)buf, (int)len, flags, from, (int *)fromlen);
    if (n == SOCKET_ERROR) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return (ssize_t)n;
}
static inline ssize_t tsnx_sendto(int fd, const void *buf, size_t len, int flags,
                                  const struct sockaddr *to, socklen_t tolen) {
    int n = sendto((SOCKET)fd, (const char *)buf, (int)len, flags, to, (int)tolen);
    if (n == SOCKET_ERROR) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return (ssize_t)n;
}
static inline int tsnx_getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) {
    if (getsockopt((SOCKET)fd, level, optname, (char *)optval, (int *)optlen) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline int tsnx_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
    // POSIX passes struct timeval for SO_RCVTIMEO/SO_SNDTIMEO; Winsock wants
    // DWORD milliseconds.
    if ((optname == SO_RCVTIMEO || optname == SO_SNDTIMEO) &&
        optval && optlen == (socklen_t)sizeof(struct timeval)) {
        const struct timeval *tv = (const struct timeval *)optval;
        DWORD ms = (DWORD)(tv->tv_sec * 1000 + (tv->tv_usec / 1000));
        if (setsockopt((SOCKET)fd, level, optname, (const char *)&ms, (int)sizeof(ms)) != 0) {
            errno = tsnx_ws_errno(WSAGetLastError()); return -1;
        }
        return 0;
    }
    if (setsockopt((SOCKET)fd, level, optname, (const char *)optval, (int)optlen) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline int tsnx_getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    if (getsockname((SOCKET)fd, addr, (int *)len) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline int tsnx_getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    if (getpeername((SOCKET)fd, addr, (int *)len) != 0) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return 0;
}
static inline int tsnx_select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) {
    int n = select(nfds, r, w, e, tv);
    if (n == SOCKET_ERROR) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return n;
}
static inline int tsnx_poll(struct pollfd *fds, nfds_t n, int timeout_ms) {
    int rc = WSAPoll((WSAPOLLFD *)fds, (ULONG)n, timeout_ms);
    if (rc == SOCKET_ERROR) {
        errno = tsnx_ws_errno(WSAGetLastError()); return -1;
    }
    return rc;
}
// The engine only ever uses fcntl() to flip O_NONBLOCK on sockets, which is
// ioctlsocket(FIONBIO) on Windows.
static inline int tsnx_fcntl(int fd, int cmd, unsigned long arg) {
    if (cmd == F_SETFL) {
        u_long nb = (arg & O_NONBLOCK) ? 1 : 0;
        if (ioctlsocket((SOCKET)fd, FIONBIO, &nb) != 0) {
            errno = tsnx_ws_errno(WSAGetLastError()); return -1;
        }
        return 0;
    }
    if (cmd == F_GETFL) {
        u_long nb = 0;
        if (ioctlsocket((SOCKET)fd, FIONBIO, &nb) != 0) {
            errno = tsnx_ws_errno(WSAGetLastError()); return -1;
        }
        return nb ? O_NONBLOCK : 0;
    }
    errno = EINVAL;
    return -1;
}

//-----------------------------------------------------------------------------
// Redirect POSIX names onto the wrappers above. Must come last so it does
// not rewrite the wrapper definitions themselves.
//-----------------------------------------------------------------------------
#define socket      tsnx_socket
#define bind        tsnx_bind
#define connect     tsnx_connect
#define listen      tsnx_listen
#define accept      tsnx_accept
#define close       tsnx_close
#define shutdown    tsnx_shutdown
#define recv        tsnx_recv
#define send        tsnx_send
#define recvfrom    tsnx_recvfrom
#define sendto      tsnx_sendto
#define getsockopt  tsnx_getsockopt
#define setsockopt  tsnx_setsockopt
#define getsockname tsnx_getsockname
#define getpeername tsnx_getpeername
#define select      tsnx_select
#define poll        tsnx_poll
#define fcntl       tsnx_fcntl

#endif // PC_POSIX_SOCKET_COMPAT_H
