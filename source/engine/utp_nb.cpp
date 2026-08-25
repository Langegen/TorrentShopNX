#include "utp_nb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "utp.h"

#define SESS_MAX 128
#define RBUF_SIZE (1024 * 1024)
#define WBUF_SIZE (256 * 1024)

enum { ST_CONNECTING = 0, ST_CONNECTED = 1, ST_EOF = 2, ST_ERROR = 3 };

struct utp_nb_sess {
    UTPSocket *sock;
    int state;
    bool in_use;
    bool closing;
    int refs;

    uint8_t *rbuf; size_t rhead, rcount;
    uint8_t *wbuf; size_t whead, wcount;
};

static int    g_udp = -1;
static int    g_inited = 0;
static utp_nb_sess g_sess[SESS_MAX];

static size_t rb_free(utp_nb_sess *c) { return RBUF_SIZE - c->rcount; }
static size_t wb_free(utp_nb_sess *c) { return WBUF_SIZE - c->wcount; }

static void rb_push(utp_nb_sess *c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        size_t tail = (c->rhead + c->rcount) % RBUF_SIZE;
        c->rbuf[tail] = p[i];
        c->rcount++;
    }
}

static size_t rb_pop(utp_nb_sess *c, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n && c->rcount > 0) {
        p[got++] = c->rbuf[c->rhead];
        c->rhead = (c->rhead + 1) % RBUF_SIZE;
        c->rcount--;
    }
    return got;
}

static void wb_push(utp_nb_sess *c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        size_t tail = (c->whead + c->wcount) % WBUF_SIZE;
        c->wbuf[tail] = p[i];
        c->wcount++;
    }
}

static size_t wb_pop(utp_nb_sess *c, uint8_t *p, size_t n) {
    size_t got = 0;
    while (got < n && c->wcount > 0) {
        p[got++] = c->wbuf[c->whead];
        c->whead = (c->whead + 1) % WBUF_SIZE;
        c->wcount--;
    }
    return got;
}

static void free_sess(utp_nb_sess *c) {
    if (!c) return;
    free(c->rbuf);
    free(c->wbuf);
    memset(c, 0, sizeof(*c));
}

static void decref(utp_nb_sess *c) {
    if (--c->refs <= 0) free_sess(c);
}

static utp_nb_sess *find_free_sess(void) {
    for (int i = 0; i < SESS_MAX; i++) {
        if (!g_sess[i].in_use && g_sess[i].refs == 0) return &g_sess[i];
    }
    return NULL;
}

static utp_nb_sess *sess_from_userdata(void *u) {
    utp_nb_sess *c = (utp_nb_sess *)u;
    if (c < g_sess || c >= g_sess + SESS_MAX) return NULL;
    return c;
}

static void cb_on_read(void *u, const byte *bytes, size_t count) {
    utp_nb_sess *c = sess_from_userdata(u);
    if (!c) return;
    size_t n = count < rb_free(c) ? count : rb_free(c);
    rb_push(c, bytes, n);
}

static void cb_on_write(void *u, byte *bytes, size_t count) {
    utp_nb_sess *c = sess_from_userdata(u);
    if (!c) return;
    wb_pop(c, bytes, count);
}

static size_t cb_get_rb_size(void *u) {
    utp_nb_sess *c = sess_from_userdata(u);
    return c ? c->rcount : 0;
}

static void cb_on_state(void *u, int state) {
    utp_nb_sess *c = sess_from_userdata(u);
    if (!c) return;
    if (state == UTP_STATE_CONNECT || state == UTP_STATE_WRITABLE) {
        c->state = ST_CONNECTED;
        // Anything queued before the connect completed (our handshake) must
        // be handed to libutp now -- UTP_Write is a no-op on a connecting
        // socket, so the peer would otherwise wait on us forever.
        if (c->sock && c->wcount > 0) UTP_Write(c->sock, c->wcount);
    } else if (state == UTP_STATE_EOF) {
        c->state = ST_EOF;
    } else if (state == UTP_STATE_DESTROYING) {
        c->sock = NULL;
        decref(c);
    }
}

static void cb_on_error(void *u, int err) {
    (void)err;
    utp_nb_sess *c = sess_from_userdata(u);
    if (!c) return;
    if (c->state != ST_EOF) c->state = ST_ERROR;
}

static void cb_on_overhead(void *u, bool send, size_t count, int type) {
    (void)u; (void)send; (void)count; (void)type;
}

static UTPFunctionTable g_functable = {
    cb_on_read, cb_on_write, cb_get_rb_size, cb_on_state, cb_on_error, cb_on_overhead
};

static void cb_send_to(void *u, const byte *p, size_t len,
                       const struct sockaddr *to, socklen_t tolen) {
    (void)u;
    if (g_udp >= 0) sendto(g_udp, p, len, 0, to, tolen);
}

static void cb_on_incoming(void *u, UTPSocket *s) {
    (void)u;
    // We do not accept inbound µTP in the streaming path yet.
    UTP_Close(s);
}

int utp_nb_init(void) {
    if (g_inited) return 0;

    g_udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp < 0) {
        fprintf(stderr, "[utp_nb] socket() failed errno=%d\n", errno);
        return -1;
    }

    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
    me.sin_port = 0;
    if (bind(g_udp, (struct sockaddr *)&me, sizeof(me)) < 0) {
        close(g_udp);
        g_udp = -1;
        return -1;
    }

    int fl = fcntl(g_udp, F_GETFL, 0);
    if (fl >= 0) fcntl(g_udp, F_SETFL, fl | O_NONBLOCK);

    memset(g_sess, 0, sizeof(g_sess));
    g_inited = 1;
    return 0;
}

void utp_nb_exit(void) {
    if (!g_inited) return;
    for (int i = 0; i < SESS_MAX; i++) {
        if (g_sess[i].sock) UTP_Close(g_sess[i].sock);
    }
    // Give libutp a moment to destroy sockets; then free remaining sessions.
    for (int k = 0; k < 10; k++) UTP_CheckTimeouts();
    for (int i = 0; i < SESS_MAX; i++) {
        if (g_sess[i].refs > 0) free_sess(&g_sess[i]);
    }
    if (g_udp >= 0) close(g_udp);
    g_udp = -1;
    g_inited = 0;
}

int utp_nb_fd(void) { return g_inited ? g_udp : -1; }

void utp_nb_service(void) {
    if (!g_inited || g_udp < 0) return;

    uint8_t buf[4096];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(g_udp, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (getenv("UTP_DBG")) {
            fprintf(stderr, "[utp_nb] rx %u bytes from %u.%u.%u.%u:%u\n", (unsigned)n,
                    (unsigned)(from.sin_addr.s_addr & 0xff), (unsigned)((from.sin_addr.s_addr >> 8) & 0xff),
                    (unsigned)((from.sin_addr.s_addr >> 16) & 0xff), (unsigned)((from.sin_addr.s_addr >> 24) & 0xff),
                    (unsigned)ntohs(from.sin_port));
        }
        UTP_IsIncomingUTP(cb_on_incoming, cb_send_to, NULL,
                          buf, (size_t)n, (struct sockaddr *)&from, fromlen);
    }
    UTP_CheckTimeouts();
}

utp_nb_sess *utp_nb_connect(uint32_t ip_net, uint16_t port_host) {
    if (!g_inited) { fprintf(stderr, "[utp_nb] connect: not inited\n"); return NULL; }
    utp_nb_sess *c = find_free_sess();
    if (!c) {
        int inuse = 0, zombies = 0;
        for (int i = 0; i < SESS_MAX; i++) {
            if (g_sess[i].in_use) inuse++;
            else if (g_sess[i].refs > 0) zombies++;
        }
        fprintf(stderr, "[utp_nb] connect: no free sess (in_use=%d zombies=%d)\n",
                inuse, zombies);
        return NULL;
    }

    c->rbuf = (uint8_t *)malloc(RBUF_SIZE);
    c->wbuf = (uint8_t *)malloc(WBUF_SIZE);
    if (!c->rbuf || !c->wbuf) {
        fprintf(stderr, "[utp_nb] connect: malloc failed\n");
        free(c->rbuf); free(c->wbuf);
        c->rbuf = NULL; c->wbuf = NULL;
        return NULL;
    }

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = ip_net;
    sa.sin_port = htons(port_host);

    UTPSocket *sock = UTP_Create(cb_send_to, NULL,
                                 (struct sockaddr *)&sa, sizeof(sa));
    if (!sock) {
        fprintf(stderr, "[utp_nb] connect: UTP_Create failed\n");
        free(c->rbuf); free(c->wbuf);
        c->rbuf = NULL; c->wbuf = NULL;
        return NULL;
    }

    UTP_SetCallbacks(sock, &g_functable, c);
    c->sock = sock;
    c->state = ST_CONNECTING;
    c->in_use = true;
    c->closing = false;
    c->refs = 2;

    UTP_Connect(sock);
    return c;
}

void utp_nb_close(utp_nb_sess *c) {
    if (!c || !c->in_use) return;
    c->in_use = false;
    c->closing = true;
    if (c->sock) {
        UTP_Close(c->sock);
        c->sock = NULL;
    }
    decref(c);
}

int utp_nb_read(utp_nb_sess *c, void *buf, int len) {
    if (!c || !c->in_use) return -1;
    if (c->state == ST_ERROR) {
        fprintf(stderr, "[utp_nb] read: ST_ERROR\n");
        return -1;
    }
    if (c->rcount == 0) {
        if (c->state == ST_EOF) { fprintf(stderr, "[utp_nb] read: ST_EOF\n"); return -1; }
        return 0;
    }
    return (int)rb_pop(c, (uint8_t *)buf, (size_t)len);
}

int utp_nb_write(utp_nb_sess *c, const void *buf, int len) {
    if (!c || !c->in_use) return -1;
    if (c->state == ST_ERROR || c->state == ST_EOF) return -1;
    if ((size_t)len > wb_free(c)) return -1;
    wb_push(c, (const uint8_t *)buf, (size_t)len);
    if (c->sock && c->state == ST_CONNECTED) UTP_Write(c->sock, (size_t)len);
    return len;
}

int utp_nb_state(const utp_nb_sess *c) {
    if (!c || !c->in_use) return -1;
    if (c->state == ST_ERROR || c->state == ST_EOF) return -1;
    return c->state == ST_CONNECTED ? 1 : 0;
}
