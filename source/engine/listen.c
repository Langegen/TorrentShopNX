#include "listen.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <switch.h>

#include "torrentfs.h"
#include "engine_log.h"

#define INCOMING_HS_TIMEOUT_SECS 5   // patience for the peer's handshake bytes

static Thread         s_thread;
static bool           s_thread_started = false;
static volatile bool  s_stop = false;
static int            s_listen_fd = -1;
static int            s_port = 0;
static listen_find_fn s_find = NULL;

// Read exactly 68 bytes of the peer's handshake, with a deadline. Returns 0 on
// success, -1 on error/timeout. The peer's first byte must be 19 (the BT
// preamble length) -- anything else is MSE/PE (unsupported on the responder
// side for now) or garbage, and the connection is dropped.
static int read_handshake(int fd, uint8_t hs[68]) {
    size_t got = 0;
    uint64_t deadline = armGetSystemTick() +
                        (uint64_t)INCOMING_HS_TIMEOUT_SECS *
                            armGetSystemTickFreq();

    while (got < 68) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = poll(&pfd, 1, 250);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (rc == 0) {
            if (armGetSystemTick() > deadline) return -1;
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;

        size_t want = 68 - got;
        ssize_t n = recv(fd, hs + got, want, 0);
        if (n <= 0) return -1;
        if (got == 0 && hs[0] != 19) {
            engine_log(ENGINE_LOG_DEBUG,
                       "[listen] drop conn: first byte %d (MSE or garbage)", hs[0]);
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

static void listener_main(void *arg) {
    (void)arg;
    engine_log(ENGINE_LOG_INFO, "[listen] accept thread start fd=%d", s_listen_fd);

    while (!s_stop) {
        tsnx_engine_wd_tick(2);
        struct pollfd pfd;
        pfd.fd = s_listen_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = poll(&pfd, 1, 250);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        int fd = accept(s_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

        uint8_t hs[68];
        if (read_handshake(fd, hs) != 0) {
            engine_log(ENGINE_LOG_DEBUG, "[listen] handshake read failed");
            close(fd);
            continue;
        }

        const uint8_t *info_hash = hs + 28;
        torrentfs *t = s_find ? s_find(info_hash) : NULL;
        if (!t) {
            engine_log(ENGINE_LOG_DEBUG, "[listen] no torrent for this info_hash");
            close(fd);
            continue;
        }

        torrentfs_incoming_push(t, fd, hs);
    }

    engine_log(ENGINE_LOG_INFO, "[listen] accept thread stop");
}

int torrent_listener_start(int port, listen_find_fn find) {
    if (s_thread_started) return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        engine_log(ENGINE_LOG_WARN, "[listen] socket() failed errno=%d", errno);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        engine_log(ENGINE_LOG_WARN, "[listen] bind(%d) failed errno=%d -- retrying on an ephemeral port", port, errno);
        sa.sin_port = 0;
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            engine_log(ENGINE_LOG_WARN, "[listen] bind(0) failed errno=%d", errno);
            close(fd);
            return -1;
        }
    }
    if (listen(fd, 8) != 0) {
        engine_log(ENGINE_LOG_WARN, "[listen] listen() failed errno=%d", errno);
        close(fd);
        return -1;
    }

    socklen_t sl = sizeof(sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
        s_port = ntohs(sa.sin_port);

    s_find = find;
    s_stop = false;
    s_listen_fd = fd;

    if (threadCreate(&s_thread, listener_main, NULL, NULL, 0x20000, 0x2C,
                     -2) == 0) {
        threadStart(&s_thread);
        s_thread_started = true;
    } else {
        engine_log(ENGINE_LOG_WARN, "[listen] accept thread create failed");
        close(fd);
        s_listen_fd = -1;
        s_port = 0;
        return -1;
    }

    engine_log(ENGINE_LOG_INFO, "[listen] listening on port %d", s_port);
    return 0;
}

void torrent_listener_stop(void) {
    if (!s_thread_started) return;
    s_stop = true;
    if (s_listen_fd >= 0) {
        close(s_listen_fd);   // wakes the poll() with POLLNVAL
        s_listen_fd = -1;
    }
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
    s_thread_started = false;
    s_port = 0;
    s_find = NULL;
}

int torrent_listener_port(void) {
    return s_port;
}

void torrentfs_incoming_push(torrentfs *t, int fd, const uint8_t hs68[68]) {
    if (!t) {
        close(fd);
        return;
    }
    struct torrentfs_incoming_q *iq = torrentfs_incoming_queue(t);
    int queued = -1;
    mutexLock(&iq->lock);
    if (iq->n < TSNX_INCOMING_QUEUE_MAX) {
        iq->q[iq->n].fd = fd;
        memcpy(iq->q[iq->n].hs, hs68, 68);
        iq->n++;
        queued = iq->n;
    }
    mutexUnlock(&iq->lock);
    if (queued >= 0) {
        engine_log(ENGINE_LOG_DEBUG, "[listen] queued incoming conn to %s (q=%d)",
                   torrentfs_name(t), queued);
    } else {
        engine_log(ENGINE_LOG_DEBUG, "[listen] incoming queue full, dropping");
        close(fd);
    }
}
