#include "dhtclient.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "engine_log.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <switch.h>
#include <mbedtls/sha1.h>

#include "dht.h"  // jech/dht (no include guard; needs the headers above first)

//-----------------------------------------------------------------------------
// Logging
//-----------------------------------------------------------------------------

static void (*s_log_fn)(const char *) = NULL;

void dht_set_log(void (*fn)(const char *)) { s_log_fn = fn; }

static int s_last_good = 0;
static int s_last_dubious = 0;
static int s_last_peers_found = 0;

void dhtclient_get_nodes(int *good, int *dubious) {
    if (good) *good = s_last_good;
    if (dubious) *dubious = s_last_dubious;
}

void dhtclient_get_last_lookup(int *peers_found, int *good_nodes, int *dubious_nodes) {
    if (peers_found) *peers_found = s_last_peers_found;
    if (good_nodes) *good_nodes = s_last_good;
    if (dubious_nodes) *dubious_nodes = s_last_dubious;
}

static void dlog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log_fn) {
        s_log_fn(buf);
    } else {
        engine_log(ENGINE_LOG_INFO, "[dht] %s", buf);
    }
}

//-----------------------------------------------------------------------------
// Callbacks required by jech/dht
//-----------------------------------------------------------------------------

int dht_blacklisted(const struct sockaddr *sa, int salen) {
    (void)sa; (void)salen;
    return 0;
}

// Deterministic hash over up to three inputs, used by the DHT for tokens and
// node ids. SHA-1 gives 20 bytes; copy/repeat to fill hash_size.
void dht_hash(void *hash_return, int hash_size,
              const void *v1, int len1,
              const void *v2, int len2,
              const void *v3, int len3) {
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts_ret(&ctx);
    if (v1 && len1 > 0) mbedtls_sha1_update_ret(&ctx, v1, len1);
    if (v2 && len2 > 0) mbedtls_sha1_update_ret(&ctx, v2, len2);
    if (v3 && len3 > 0) mbedtls_sha1_update_ret(&ctx, v3, len3);
    uint8_t digest[20];
    mbedtls_sha1_finish_ret(&ctx, digest);
    mbedtls_sha1_free(&ctx);

    uint8_t *out = hash_return;
    for (int i = 0; i < hash_size; i++) out[i] = digest[i % 20];
}

int dht_random_bytes(void *buf, size_t size) {
    randomGet(buf, size);
    return (int)size;
}

//-----------------------------------------------------------------------------
// Lookup
//-----------------------------------------------------------------------------

static const char *BOOTSTRAP[] = {
    "router.bittorrent.com",
    "router.utorrent.com",
    "dht.transmissionbt.com",
    "dht.libtorrent.org",
    "dht.aelitis.com",
    "bootstrap.dht.transmissionbt.com",
};
#define BOOTSTRAP_PORT "6881"

typedef struct {
    dht_peer_cb cb;
    void *ctx;
    int delivered;
} dht_ctx;

// jech/dht calls this when a search yields results or finishes.
static void on_dht_event(void *closure, int event,
                         const unsigned char *info_hash,
                         const void *data, size_t data_len) {
    (void)info_hash;
    dht_ctx *dc = closure;
    if (event != DHT_EVENT_VALUES) return;  // ignore IPv6 + search-done here

    // data is a packed array of 6-byte compact peers (4B IP + 2B port).
    int n = (int)(data_len / 6);
    if (n <= 0) return;
    const uint8_t *b = data;

    peer_addr peers[128];
    int np = 0;
    for (int i = 0; i < n && np < 128; i++) {
        const uint8_t *e = b + i * 6;
        memcpy(&peers[np].ip, e, 4);                       // network byte order
        peers[np].port = (uint16_t)((e[4] << 8) | e[5]);   // host byte order
        if (peers[np].port) np++;
    }
    if (np > 0 && dc->cb) {
        dc->cb(dc->ctx, peers, np);
        dc->delivered += np;
    }
}

int dht_find_peers(const uint8_t info_hash[20], int target_peers, int budget_ms,
                   dht_peer_cb cb, void *ctx, const volatile bool *cancel,
                   char *err, size_t errlen) {
    dlog("DHT demarre (jech)");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        if (err) snprintf(err, errlen, "DHT socket failed");
        return -1;
    }
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
    me.sin_port = 0;  // ephemeral; DHT replies come back to the source port
    bind(s, (struct sockaddr *)&me, sizeof(me));

    // Switch's select() does not report UDP readability, so we use a blocking
    // recvfrom with a receive timeout instead (the pattern that works here).
    struct timeval rcvto = { 1, 0 };  // 1s
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    uint8_t node_id[20];
    randomGet(node_id, sizeof(node_id));

    if (dht_init(s, -1, node_id, NULL) < 0) {
        if (err) snprintf(err, errlen, "dht_init failed");
        close(s);
        return -1;
    }

    // Bootstrap: ping the well-known routers so the routing table fills up.
    int booted = 0;
    for (size_t i = 0; i < sizeof(BOOTSTRAP) / sizeof(*BOOTSTRAP); i++) {
        struct addrinfo hints = {0}, *res = NULL, *r;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(BOOTSTRAP[i], BOOTSTRAP_PORT, &hints, &res) != 0 || !res)
            continue;
        for (r = res; r; r = r->ai_next) {
            dht_ping_node(r->ai_addr, r->ai_addrlen);
            booted++;
        }
        freeaddrinfo(res);
    }
    dlog("DHT bootstrap: %d routeurs pingues", booted);
    if (booted == 0) {
        if (err) snprintf(err, errlen, "bootstrap DHT injoignable");
        dht_uninit();
        close(s);
        return -1;
    }

    dht_ctx dc = { cb, ctx, 0 };

    u64 freq = armGetSystemTickFreq();
    u64 start = armGetSystemTick();
    u64 last_log = start;
    bool searching = false;
    time_t tosleep = 0;

    while (1) {
        if (cancel && *cancel) break;
        double elapsed_ms = (double)(armGetSystemTick() - start) / freq * 1000.0;
        if (elapsed_ms >= budget_ms) break;
        if (dc.delivered >= target_peers) break;

        // Blocking recvfrom (up to the 1s SO_RCVTIMEO). On a packet, feed it to
        // the DHT; on timeout, tick the DHT with no packet.
        uint8_t buf[3072];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            buf[n] = '\0';  // jech expects a NUL-terminated buffer
            dht_periodic(buf, n, (struct sockaddr *)&from, fromlen,
                         &tosleep, on_dht_event, &dc);
        } else {
            dht_periodic(NULL, 0, NULL, 0, &tosleep, on_dht_event, &dc);
            // Socket is non-blocking: avoid busy-spinning while waiting for
            // bootstrap replies or search results.
            usleep(20000);
        }

        // Once the table has a few nodes, start (and keep) searching.
        int good = 0, dubious = 0;
        dht_nodes(AF_INET, &good, &dubious, NULL, NULL);
        s_last_good = good;
        s_last_dubious = dubious;
        if (!searching && (good + dubious) >= 2) {
            dht_search(info_hash, 0, AF_INET, on_dht_event, &dc);  // port 0 = no announce
            searching = true;
            dlog("DHT: recherche lancee (%d noeuds)", good + dubious);
        }

        if ((double)(armGetSystemTick() - last_log) / freq >= 2.0) {
            dlog("DHT: %d noeuds, %d peers", good + dubious, dc.delivered);
            last_log = armGetSystemTick();
        }
    }

    dlog("DHT fin: %d peers", dc.delivered);
    s_last_peers_found = dc.delivered;
    dht_uninit();
    close(s);
    return dc.delivered;
}

//-----------------------------------------------------------------------------
// Persistent background DHT
//-----------------------------------------------------------------------------

static Mutex s_bg_mtx;
static int s_bg_init = 0;
static volatile bool s_bg_stop = true;
static Thread s_bg_thread;
static bool s_bg_thread_started = false;
static int s_bg_sock = -1;
static bool s_bg_bootstrapped = false;

typedef struct {
    bool active;
    uint8_t info_hash[20];
    dht_peer_cb cb;
    void *ctx;
    u64 last_search;
} bg_target;

static bg_target s_bg_target = {0};

static void bg_on_event(void *closure, int event,
                        const unsigned char *info_hash,
                        const void *data, size_t data_len) {
    (void)info_hash;
    bg_target *target = closure;
    if (event != DHT_EVENT_VALUES) return;

    mutexLock(&s_bg_mtx);
    bool active = target && target->active;
    dht_peer_cb cb = active ? target->cb : NULL;
    void *ctx = active ? target->ctx : NULL;
    mutexUnlock(&s_bg_mtx);
    if (!cb) return;

    int n = (int)(data_len / 6);
    if (n <= 0) return;
    peer_addr peers[128];
    int np = 0;
    const uint8_t *b = data;
    for (int i = 0; i < n && np < 128; i++) {
        const uint8_t *e = b + i * 6;
        memcpy(&peers[np].ip, e, 4);
        peers[np].port = (uint16_t)((e[4] << 8) | e[5]);
        if (peers[np].port) np++;
    }
    if (np > 0) {
        cb(ctx, peers, np);
        s_last_peers_found += np;
    }
}

static int bg_bootstrap(int s) {
    int booted = 0;
    for (size_t i = 0; i < sizeof(BOOTSTRAP) / sizeof(*BOOTSTRAP); i++) {
        struct addrinfo hints = {0}, *res = NULL, *r;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(BOOTSTRAP[i], BOOTSTRAP_PORT, &hints, &res) != 0 || !res)
            continue;
        for (r = res; r; r = r->ai_next) {
            dht_ping_node(r->ai_addr, r->ai_addrlen);
            booted++;
        }
        freeaddrinfo(res);
    }
    return booted;
}

static void dht_bg_main(void *arg) {
    (void)arg;
    u64 freq = armGetSystemTickFreq();

    s_bg_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_bg_sock < 0) {
        engine_log(ENGINE_LOG_ERROR, "[dht] background socket failed");
        return;
    }
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
    me.sin_port = htons(51413);
    bind(s_bg_sock, (struct sockaddr *)&me, sizeof(me));

    struct timeval rcvto = {1, 0};
    setsockopt(s_bg_sock, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    uint8_t node_id[20];
    randomGet(node_id, sizeof(node_id));
    if (dht_init(s_bg_sock, -1, node_id, NULL) < 0) {
        engine_log(ENGINE_LOG_ERROR, "[dht] background dht_init failed");
        close(s_bg_sock);
        s_bg_sock = -1;
        return;
    }

    s_bg_bootstrapped = false;
    int boot_count = 0;
    u64 last_bootstrap = 0;
    u64 last_log = 0;
    time_t tosleep = 0;

    while (!s_bg_stop) {
        uint8_t buf[3072];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(s_bg_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
        u64 now = armGetSystemTick();
        if (n > 0) {
            buf[n] = '\0';
            dht_periodic(buf, n, (struct sockaddr *)&from, fromlen,
                         &tosleep, bg_on_event, &s_bg_target);
        } else {
            dht_periodic(NULL, 0, NULL, 0, &tosleep, bg_on_event, &s_bg_target);
        }

        int good = 0, dubious = 0;
        dht_nodes(AF_INET, &good, &dubious, NULL, NULL);
        s_last_good = good;
        s_last_dubious = dubious;

        if (!s_bg_bootstrapped) {
            boot_count = bg_bootstrap(s_bg_sock);
            if (boot_count > 0) {
                s_bg_bootstrapped = true;
                last_bootstrap = now;
                engine_log(ENGINE_LOG_INFO,
                           "[dht] background bootstrap: %d routers", boot_count);
            }
        } else if (good == 0 && now - last_bootstrap > (u64)30 * freq) {
            boot_count = bg_bootstrap(s_bg_sock);
            last_bootstrap = now;
            engine_log(ENGINE_LOG_INFO,
                       "[dht] background re-bootstrap: %d routers", boot_count);
        }

        if (s_bg_bootstrapped && (good + dubious) >= 2) {
            mutexLock(&s_bg_mtx);
            bool active = s_bg_target.active;
            bool need_search = active &&
                (s_bg_target.last_search == 0 ||
                 now - s_bg_target.last_search > (u64)30 * freq);
            mutexUnlock(&s_bg_mtx);
            if (need_search) {
                dht_search(s_bg_target.info_hash, 0, AF_INET,
                           bg_on_event, &s_bg_target);
                mutexLock(&s_bg_mtx);
                s_bg_target.last_search = now;
                mutexUnlock(&s_bg_mtx);
                engine_log(ENGINE_LOG_INFO, "[dht] background search issued");
            }
        }

        if (now - last_log > (u64)2 * freq) {
            engine_log(ENGINE_LOG_INFO,
                       "[dht] background nodes=%d/%d peers_found=%d",
                       good, dubious, s_last_peers_found);
            last_log = now;
        }
    }

    dht_uninit();
    if (s_bg_sock >= 0) close(s_bg_sock);
    s_bg_sock = -1;
    s_bg_bootstrapped = false;
}

void dht_background_add(const uint8_t info_hash[20],
                        dht_peer_cb cb, void *ctx) {
    if (!s_bg_init) {
        mutexInit(&s_bg_mtx);
        s_bg_init = 1;
    }
    mutexLock(&s_bg_mtx);
    memcpy(s_bg_target.info_hash, info_hash, 20);
    s_bg_target.cb = cb;
    s_bg_target.ctx = ctx;
    s_bg_target.active = true;
    s_bg_target.last_search = 0;
    s_last_peers_found = 0;
    if (!s_bg_thread_started) {
        s_bg_stop = false;
        if (threadCreate(&s_bg_thread, dht_bg_main, NULL, NULL,
                         0x20000, 0x2C, -2) == 0) {
            threadStart(&s_bg_thread);
            s_bg_thread_started = true;
        } else {
            s_bg_stop = true;
            engine_log(ENGINE_LOG_ERROR, "[dht] background thread create failed");
        }
    }
    mutexUnlock(&s_bg_mtx);
}

void dht_background_remove(const uint8_t info_hash[20]) {
    (void)info_hash;
    if (!s_bg_init) return;
    mutexLock(&s_bg_mtx);
    s_bg_target.active = false;
    s_bg_target.cb = NULL;
    s_bg_target.ctx = NULL;
    s_bg_stop = true;
    bool started = s_bg_thread_started;
    s_bg_thread_started = false;
    mutexUnlock(&s_bg_mtx);
    if (started) {
        threadWaitForExit(&s_bg_thread);
        threadClose(&s_bg_thread);
    }
}
