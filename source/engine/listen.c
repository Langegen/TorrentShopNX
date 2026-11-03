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
#include "thread_cfg.h"

#define INCOMING_HS_TIMEOUT_SECS 5   // patience for the peer's handshake bytes

static Thread         s_thread;
static bool           s_thread_started = false;
static volatile bool  s_stop = false;
static int            s_listen_fd = -1;
static int            s_port = 0;      // last successfully bound port (0 = none)
static int            s_req_port = 0;  // port requested at torrent_listener_start
static listen_route_fn s_find = NULL;

// Incoming connections whose 68-byte handshake is still arriving. They are
// read side by side in the accept loop: a single blocking read used to let one
// silent connection hold up every other incoming peer for its whole timeout.
#define MAX_PENDING_HS 16

typedef struct {
    int fd;
    size_t got;
    uint64_t deadline;
    uint8_t hs[68];
} pending_hs;

// Sleep n seconds in 1 s chunks: shutdown stays fast (s_stop checked each
// second) and the engine watchdog keeps seeing this thread as alive.
static void listener_backoff(int secs) {
    for (int i = 0; i < secs && !s_stop; i++) {
        svcSleepThread(1000000000ULL);
        tsnx_engine_wd_tick(2);
    }
}

// Create + bind + listen on the announced port. On a re-open after a network
// flap the previously bound port is kept stable: silently switching to an
// ephemeral port would make tracker/DHT announcements point at a dead port
// for the rest of the session.
static int listener_open(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        engine_log(ENGINE_LOG_WARN, "[listen] socket() failed errno=%d", errno);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    int want = s_port ? s_port : s_req_port;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = INADDR_ANY;
    sa.sin_port = htons((uint16_t)want);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        engine_log(ENGINE_LOG_WARN,
                   "[listen] bind(%d) failed errno=%d", want, errno);
        if (s_port != 0) {
            // Re-open case: never silently move to another port.
            close(fd);
            return -1;
        }
        engine_log(ENGINE_LOG_WARN,
                   "[listen] retrying on an ephemeral port");
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

    s_listen_fd = fd;
    return 0;
}

static void listener_main(void *arg) {
    (void)arg;

    while (!s_stop) {
        tsnx_engine_wd_tick(2);
        if (s_listen_fd < 0) {
            // (Re)open after a flap or an initial failure: retry the same
            // port with a backoff until the interface is back.
            if (listener_open() != 0) {
                engine_log(ENGINE_LOG_WARN,
                           "[listen] open failed, retrying in 5s");
                listener_backoff(5);
                continue;
            }
            engine_log(ENGINE_LOG_INFO, "[listen] re-listening on port %d",
                       s_port);
        }

        engine_log(ENGINE_LOG_INFO, "[listen] accept thread start fd=%d",
                   s_listen_fd);

        pending_hs pend[MAX_PENDING_HS];
        int npend = 0;
        const uint64_t hs_ticks = (uint64_t)INCOMING_HS_TIMEOUT_SECS * armGetSystemTickFreq();
        bool listen_failed = false;

        while (!s_stop && !listen_failed) {
            tsnx_engine_wd_tick(2);
            struct pollfd pfds[1 + MAX_PENDING_HS];
            pfds[0].fd = s_listen_fd;
            // With every slot busy, leave new connections in the backlog.
            pfds[0].events = npend < MAX_PENDING_HS ? POLLIN : 0;
            pfds[0].revents = 0;
            for (int i = 0; i < npend; i++) {
                pfds[1 + i].fd = pend[i].fd;
                pfds[1 + i].events = POLLIN;
                pfds[1 + i].revents = 0;
            }
            int rc = poll(pfds, (nfds_t)(1 + npend), 250);
            if (rc < 0) {
                if (errno == EINTR) continue;
                break;
            }
            const uint64_t now = armGetSystemTick();

            // Pending handshakes, backwards so a finished slot can be filled
            // with the last one (already serviced in this pass).
            for (int i = npend - 1; i >= 0; i--) {
                pending_hs *ph = &pend[i];
                short re = pfds[1 + i].revents;
                bool drop = false, done = false;
                if (re & (POLLERR | POLLHUP | POLLNVAL)) {
                    drop = true;
                } else if (re & POLLIN) {
                    ssize_t n = recv(ph->fd, ph->hs + ph->got, 68 - ph->got, 0);
                    if (n <= 0) {
                        if (!(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)))
                            drop = true;
                    } else if (ph->got == 0 && ph->hs[0] != 19) {
                        engine_log(ENGINE_LOG_DEBUG,
                                   "[listen] drop conn: first byte %d (MSE or garbage)", ph->hs[0]);
                        drop = true;
                    } else {
                        ph->got += (size_t)n;
                        done = ph->got == 68;
                    }
                }
                if (!drop && !done && now > ph->deadline) {
                    engine_log(ENGINE_LOG_DEBUG, "[listen] handshake read failed");
                    drop = true;
                }
                if (done) {
                    const uint8_t *info_hash = ph->hs + 28;
                    if (!s_find || !s_find(info_hash, ph->fd, ph->hs)) {
                        engine_log(ENGINE_LOG_DEBUG,
                                   "[listen] no torrent for this info_hash");
                        close(ph->fd);
                    }
                } else if (drop) {
                    close(ph->fd);
                }
                if (done || drop) pend[i] = pend[--npend];
            }

            if (rc == 0) continue;
            if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                listen_failed = true;
                break;
            }
            if (!(pfds[0].revents & POLLIN)) continue;

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

            pend[npend].fd = fd;
            pend[npend].got = 0;
            pend[npend].deadline = now + hs_ticks;
            npend++;
        }

        for (int i = 0; i < npend; i++) close(pend[i].fd);

        close(s_listen_fd);
        s_listen_fd = -1;
        engine_log(ENGINE_LOG_INFO, "[listen] accept thread stop");
        if (s_stop) break;

        // The listen socket died (interface flap / sleep-resume): restart
        // instead of silently losing incoming connections for the whole
        // session (the port stays the same, announcements remain valid).
        engine_log(ENGINE_LOG_WARN, "[listen] listener lost, restarting in 5s");
        listener_backoff(5);
    }
}

int torrent_listener_start(int port, listen_route_fn find) {
    if (s_thread_started) return 0;

    s_req_port = port;
    s_find = find;
    s_stop = false;
    s_port = 0;
    s_listen_fd = -1;

    if (listener_open() != 0) {
        engine_log(ENGINE_LOG_WARN, "[listen] initial open failed");
        s_find = NULL;
        return -1;
    }

    if (threadCreate(&s_thread, listener_main, NULL, NULL, 0x20000,
                     TSNX_THREAD_ARGS(TSNX_CORE_NET, TSNX_PRIO_NET_BG)) == 0) {
        threadStart(&s_thread);
        s_thread_started = true;
    } else {
        engine_log(ENGINE_LOG_WARN, "[listen] accept thread create failed");
        close(s_listen_fd);
        s_listen_fd = -1;
        s_port = 0;
        s_find = NULL;
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
