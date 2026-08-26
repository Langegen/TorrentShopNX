#include "dhtclient.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "engine_log.h"
#include "torrent_meta.h"   // torrent_announce_port(): our advertised listen port
#include "../utils/app_paths.h"

#include <arpa/inet.h>
#include <fcntl.h>
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
// Last values actually written to the log (change-detection for the
// 2s heartbeat): the periodic line is only printed when something changed,
// otherwise at most once per 60s -- cuts the "[dht] background" spam.
static int s_log_good = 0;
static int s_log_dubious = 0;
static int s_log_peers = 0;
static u64 s_last_change_log = 0;

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
    mbedtls_sha1_starts(&ctx);
    if (v1 && len1 > 0) mbedtls_sha1_update(&ctx, v1, len1);
    if (v2 && len2 > 0) mbedtls_sha1_update(&ctx, v2, len2);
    if (v3 && len3 > 0) mbedtls_sha1_update(&ctx, v3, len3);
    uint8_t digest[20];
    mbedtls_sha1_finish(&ctx, digest);
    mbedtls_sha1_free(&ctx);

    uint8_t *out = hash_return;
    for (int i = 0; i < hash_size; i++) out[i] = digest[i % 20];
}

int dht_random_bytes(void *buf, size_t size) {
    randomGet(buf, size);
    return (int)size;
}

//-----------------------------------------------------------------------------
// Node cache (fast warm start)
//-----------------------------------------------------------------------------

#define DHT_CACHE_MAGIC   "TDX1"
#define DHT_CACHE_MAX_NODES 256
#define DHT_CACHE_PATH    TSNX_DHT_CACHE_FILE

static char s_dht_cache_path[256] = DHT_CACHE_PATH;

void dht_set_cache_path(const char *path) {
    if (path && path[0])
        snprintf(s_dht_cache_path, sizeof(s_dht_cache_path), "%s", path);
}

static int dht_cache_read(const char *path, uint8_t node_id[20],
                          uint8_t (*nodes)[6], int max_nodes) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t magic[4];
    uint8_t cnt_le[2];
    int ok = fread(magic, 1, 4, f) == 4 &&
             memcmp(magic, DHT_CACHE_MAGIC, 4) == 0 &&
             fread(node_id, 1, 20, f) == 20 &&
             fread(cnt_le, 1, 2, f) == 2;
    int n = 0;
    if (ok) {
        n = cnt_le[0] | (cnt_le[1] << 8);
        if (n > DHT_CACHE_MAX_NODES) ok = 0;
    }
    if (ok) {
        if (n > max_nodes) n = max_nodes;
        ok = fread(nodes, 6, (size_t)n, f) == (size_t)n;
    }
    fclose(f);
    return ok ? n : -1;
}

static int dht_cache_write(const char *path, const uint8_t node_id[20],
                           const uint8_t (*nodes)[6], int count) {
    if (count <= 0) return 0;
    if (count > DHT_CACHE_MAX_NODES) count = DHT_CACHE_MAX_NODES;
    tsnx_ensure_parent_dirs(path);
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    uint8_t cnt_le[2] = { (uint8_t)(count & 0xff), (uint8_t)(count >> 8) };
    int ok = fwrite(DHT_CACHE_MAGIC, 1, 4, f) == 4 &&
             fwrite(node_id, 1, 20, f) == 20 &&
             fwrite(cnt_le, 1, 2, f) == 2 &&
             fwrite(nodes, 6, (size_t)count, f) == (size_t)count;
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        remove(tmp);
        return 0;
    }
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) {
            remove(tmp);
            return 0;
        }
    }
    return 1;
}

//-----------------------------------------------------------------------------
// Persistent background DHT
//
// ONE jech/DHT context (one socket, one routing table) owned by a background
// thread that lives for the whole engine session. Everything DHT-related --
// the metadata fetch (dht_find_peers) and the per-torrent peer discovery
// (dht_background_add) -- attaches to this shared instance instead of
// creating its own socket and cold table (two parallel dht_init() calls would
// also corrupt jech's global state).
//
// Besides searching the attached info-hashes, the thread runs periodic
// random-id walks so the routing table keeps growing (~200+ nodes after a few
// minutes), which is what makes later lookups fast and reliable.
//-----------------------------------------------------------------------------

static const char *BOOTSTRAP[] = {
    "router.bittorrent.com",
    "router.utorrent.com",
    "dht.transmissionbt.com",
    "bootstrap.dht.transmissionbt.com",
    "dht.libtorrent.org",
    "dht.aelitis.com",
    "dht2.opentracker.is",
};
#define BOOTSTRAP_PORT "6881"

#define DHT_MAX_TARGETS 4

typedef struct {
    bool active;
    uint8_t info_hash[20];
    dht_peer_cb cb;
    void *ctx;
    int delivered;          // peers delivered to this target
    u64 last_search;        // tick of the last dht_search issued
} dht_target;

static Mutex s_bg_mtx;
static int s_bg_init = 0;
static volatile bool s_bg_stop = true;
static Thread s_bg_thread;
static bool s_bg_thread_started = false;
static int s_bg_sock = -1;
static bool s_bg_bootstrapped = false;
static uint8_t s_bg_node_id[20];
static dht_target s_targets[DHT_MAX_TARGETS];
static u64 s_last_walk = 0;       // last random-id walk tick

// Table-refresh sweep state (moved to file scope so a socket restart can
// reset it together with the routing table).
static u64 s_sweep_start = 0;
static int s_sweep_pos = 0;

// Network-flap recovery: how long the routing table has been completely
// empty while bootstrap routers were pinged but never answered. A long
// streak means the persistent UDP socket went stale (the interface dropped
// and re-associated): TCP and fresh UDP sockets work, this one doesn't.
static u64 s_dead_since = 0;
static int s_restarts = 0;
// Result of the last bootstrap attempt (0 = DNS could not resolve the
// routers). Kept across iterations: the flap detector needs to know whether
// recent bootstrap pings actually went out, not just whether this
// particular iteration pinged anything.
static int s_last_boot_count = 0;
// Catastrophic path: dht_init failed during a restart. jech state is gone,
// so the thread must stop touching dht_* entirely for this session.
static bool s_dht_dead = false;

// Hunger signal from torrentfs (st_live < STARVED_LIVE): the torrent is
// starving for peers, so searches run on a short interval instead of the
// normal 15 s one, and the first hungry search fires immediately.
static volatile int s_bg_hungry = 0;

void dht_bg_set_hungry(int v) {
    v = v ? 1 : 0;
    if (v == s_bg_hungry) return;
    if (v) {
        // Hunger just started: don't wait out the next 15 s interval -- make
        // the first search happen on the very next loop iteration.
        mutexLock(&s_bg_mtx);
        for (int i = 0; i < DHT_MAX_TARGETS; i++)
            if (s_targets[i].active) s_targets[i].last_search = 0;
        mutexUnlock(&s_bg_mtx);
    }
    s_bg_hungry = v;
}

int dht_bg_hungry(void) {
    return s_bg_hungry;
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

// Re-ping the persisted node cache to warm a freshly initialised table.
// Returns the number of cached nodes pinged (-1 = no cache).
static int bg_ping_cache(void) {
    uint8_t nodes[DHT_CACHE_MAX_NODES][6];
    uint8_t id[20];
    int n = dht_cache_read(s_dht_cache_path, id, nodes, DHT_CACHE_MAX_NODES);
    for (int i = 0; i < n; i++) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr, nodes[i], 4);
        memcpy(&addr.sin_port, nodes[i] + 4, 2);
        dht_ping_node((struct sockaddr *)&addr, sizeof(addr));
    }
    return n;
}

// Recreate the persistent UDP socket and the jech state after a network flap.
// The new socket is created and bound first: socket()/bind failures leave the
// old state untouched. Only after that does the code tear down jech and swap
// in the new socket (a dht_init failure after uninit disables DHT entirely --
// see s_dht_dead).
static int bg_restart(void) {
    int ns = socket(AF_INET, SOCK_DGRAM, 0);
    if (ns < 0) {
        engine_log(ENGINE_LOG_WARN, "[dht] restart: socket() failed errno=%d",
                   errno);
        return -1;
    }

    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
#ifdef __SWITCH__
    me.sin_port = htons(51413);
#else
    me.sin_port = 0;
#endif
    if (bind(ns, (struct sockaddr *)&me, sizeof(me)) != 0) {
        me.sin_port = 0;
        if (bind(ns, (struct sockaddr *)&me, sizeof(me)) != 0) {
            engine_log(ENGINE_LOG_WARN, "[dht] restart: bind failed errno=%d",
                       errno);
            close(ns);
            return -1;
        }
    }

    struct timeval rcvto = { 1, 0 };
    setsockopt(ns, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    int old = s_bg_sock;
    dht_uninit();
    if (dht_init(ns, -1, s_bg_node_id, NULL) < 0) {
        // Extremely unlikely (calloc failure), but without jech state every
        // subsequent dht_* call is UB -- disable DHT for the session.
        close(ns);
        if (old >= 0) close(old);
        s_bg_sock = -1;
        s_dht_dead = true;
        engine_log(ENGINE_LOG_ERROR,
                   "[dht] restart: dht_init failed, DHT disabled for this session");
        return -1;
    }
    s_bg_sock = ns;
    if (old >= 0) close(old);

#ifndef __SWITCH__
    // Same as the startup path: jech marks the socket non-blocking; the loop
    // needs a BLOCKING recvfrom with SO_RCVTIMEO (Switch keeps jech's setting).
    {
        int fl = fcntl(s_bg_sock, F_GETFL, 0);
        if (fl >= 0) fcntl(s_bg_sock, F_SETFL, fl & ~O_NONBLOCK);
    }
#endif

    s_bg_bootstrapped = false;
    return 0;
}

// jech delivers DHT_EVENT_VALUES for the search registered with this closure;
// route the compact peer list to the matching target.
static void bg_on_event(void *closure, int event,
                        const unsigned char *info_hash,
                        const void *data, size_t data_len) {
    (void)closure;
    if (event != DHT_EVENT_VALUES || !info_hash || data_len < 6) return;

    dht_target *target = NULL;
    mutexLock(&s_bg_mtx);
    for (int i = 0; i < DHT_MAX_TARGETS; i++) {
        if (s_targets[i].active &&
            memcmp(s_targets[i].info_hash, info_hash, 20) == 0) {
            target = &s_targets[i];
            break;
        }
    }
    dht_peer_cb cb = target ? target->cb : NULL;
    void *ctx = target ? target->ctx : NULL;
    mutexUnlock(&s_bg_mtx);
    if (!cb) return;

    int n = (int)(data_len / 6);
    if (n <= 0) return;
    peer_addr peers[128];
    int np = 0;
    const uint8_t *b = data;
    for (int i = 0; i < n && np < 128; i++) {
        const uint8_t *e = b + i * 6;
        memcpy(&peers[np].ip, e, 4);                       // network byte order
        peers[np].port = (uint16_t)((e[4] << 8) | e[5]);   // host byte order
        if (peers[np].port) np++;
    }
    if (np > 0) {
        mutexLock(&s_bg_mtx);
        if (target->active) target->delivered += np;
        s_last_peers_found += np;
        mutexUnlock(&s_bg_mtx);
        cb(ctx, peers, np);
    }
}

static void dht_bg_main(void *arg) {
    (void)arg;
    u64 freq = armGetSystemTickFreq();

    s_bg_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_bg_sock < 0) {
        engine_log(ENGINE_LOG_ERROR, "[dht] background socket failed errno=%d", errno);
        return;
    }
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = INADDR_ANY;
#ifdef __SWITCH__
    me.sin_port = htons(51413);
#else
    // PC: many firewalls/ISPs silently drop inbound UDP on the well-known
    // DHT port 51413 (bind succeeds, responses never arrive). Outbound-
    // initiated lookups work just as well from an ephemeral port.
    me.sin_port = 0;
#endif
    if (bind(s_bg_sock, (struct sockaddr *)&me, sizeof(me)) != 0) {
        // The canonical port is taken (another DHT client on the console?);
        // an ephemeral port still works for outbound lookups.
        me.sin_port = 0;
        if (bind(s_bg_sock, (struct sockaddr *)&me, sizeof(me)) != 0) {
            engine_log(ENGINE_LOG_ERROR, "[dht] background bind failed errno=%d", errno);
            close(s_bg_sock);
            s_bg_sock = -1;
            return;
        }
    }

    // Switch's select() does not report UDP readability, so we use a blocking
    // recvfrom with a receive timeout instead (the pattern that works here).
    struct timeval rcvto = { 1, 0 };
    setsockopt(s_bg_sock, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));

    uint8_t node_id[20];
    randomGet(node_id, sizeof(node_id));

    uint8_t cached_nodes[DHT_CACHE_MAX_NODES][6];
    int cached_count = dht_cache_read(s_dht_cache_path, node_id, cached_nodes,
                                      DHT_CACHE_MAX_NODES);
    if (cached_count > 0)
        engine_log(ENGINE_LOG_INFO, "[dht] cache: loaded %d nodes", cached_count);

    if (dht_init(s_bg_sock, -1, node_id, NULL) < 0) {
        engine_log(ENGINE_LOG_ERROR, "[dht] background dht_init failed");
        close(s_bg_sock);
        s_bg_sock = -1;
        return;
    }
    memcpy(s_bg_node_id, node_id, 20);

#ifndef __SWITCH__
    // jech's dht_init marks the socket non-blocking; this loop relies on a
    // BLOCKING recvfrom with SO_RCVTIMEO (the Switch pattern above). Restore
    // blocking or every empty recv returns EAGAIN and the loop busy-spins.
    {
        int fl = fcntl(s_bg_sock, F_GETFL, 0);
        if (fl >= 0) fcntl(s_bg_sock, F_SETFL, fl & ~O_NONBLOCK);
    }
#endif

    for (int i = 0; i < cached_count; i++) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        memcpy(&addr.sin_addr, cached_nodes[i], 4);
        memcpy(&addr.sin_port, cached_nodes[i] + 4, 2);
        dht_ping_node((struct sockaddr *)&addr, sizeof(addr));
    }
    if (cached_count > 0)
        engine_log(ENGINE_LOG_INFO, "[dht] cache: pinged %d nodes", cached_count);

    s_bg_bootstrapped = false;
    u64 last_bootstrap = 0;
    u64 last_log = 0;
    time_t tosleep = 0;

    while (!s_bg_stop) {
        tsnx_engine_wd_tick(0);
        if (s_dht_dead) {
            // dht_init failed during a restart: jech state is gone and must
            // not be touched. Keep the thread (and watchdog) alive.
            svcSleepThread(1000000000ULL);
            continue;
        }
        uint8_t buf[3072];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(s_bg_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&from, &fromlen);
        u64 now = armGetSystemTick();
        if (n > 0) {
            buf[n] = '\0';
            dht_periodic(buf, n, (struct sockaddr *)&from, fromlen,
                         &tosleep, bg_on_event, NULL);
        } else {
            dht_periodic(NULL, 0, NULL, 0, &tosleep, bg_on_event, NULL);
        }

        int good = 0, dubious = 0;
        dht_nodes(AF_INET, &good, &dubious, NULL, NULL);
        s_last_good = good;
        s_last_dubious = dubious;

        int boot_count = 0;
        if (!s_bg_bootstrapped) {
            boot_count = bg_bootstrap(s_bg_sock);
            if (boot_count > 0) {
                s_bg_bootstrapped = true;
                last_bootstrap = now;
                engine_log(ENGINE_LOG_INFO,
                           "[dht] background bootstrap: %d routers", boot_count);
            }
        } else if (good < 32 &&
                   now - last_bootstrap > (u64)(good == 0 ? 15 : 60) * freq) {
            // anacrolix re-bootstraps whenever the routing table is thin;
            // same idea here: the table can decay below usefulness (field:
            // 163 good nodes -> 12 in ~10 min) and then every search walks
            // dead addresses. Refill from the router set + cache.
            boot_count = bg_bootstrap(s_bg_sock);
            last_bootstrap = now;
            engine_log(ENGINE_LOG_INFO,
                       "[dht] background re-bootstrap (good=%d): %d routers",
                       good, boot_count);
        }
        if (boot_count > 0) s_last_boot_count = boot_count;

        // Network-flap recovery: the table is empty AND the bootstrap
        // routers were resolved and pinged recently (so DNS is fine) but
        // never answered. That points at a stale UDP socket, not a dead
        // network (TCP trackers and fresh UDP sockets work in that state) --
        // pinging the same socket forever is pointless, so recreate it
        // wholesale. First 3 attempts are 2 min apart, then the interval
        // backs off to 10 min so a permanently blocked UDP path cannot
        // churn the socket.
        if (s_bg_bootstrapped && good == 0) {
            if (s_dead_since == 0) s_dead_since = now;
            u64 dead_iv = (u64)(s_restarts >= 3 ? 600 : 120) * freq;
            if (now - s_dead_since > dead_iv && s_last_boot_count > 0) {
                s_dead_since = 0;
                s_restarts++;
                engine_log(ENGINE_LOG_WARN,
                           "[dht] no replies for %ds (%d routers pinged), "
                           "restarting socket (attempt %d)",
                           (int)(dead_iv / freq), s_last_boot_count, s_restarts);
                if (bg_restart() == 0) {
                    last_bootstrap = now;
                    s_last_boot_count = 0;
                    s_sweep_pos = 0;
                    s_sweep_start = now;
                    int cached = bg_ping_cache();
                    if (cached > 0)
                        engine_log(ENGINE_LOG_INFO,
                                   "[dht] restart: re-pinged %d cached nodes",
                                   cached);
                }
                continue;   // one fresh iteration on the new socket
            }
        } else {
            s_dead_since = 0;
        }
        if (good > 0) s_restarts = 0;   // network recovered: reset the backoff

        if (s_bg_bootstrapped && (good + dubious) >= 1) {
            // Search every active target every ~15 s (5 s while the torrent is
            // starving for peers). The port is ours, so the nodes we query
            // store OUR address as a peer of the infohash and other clients
            // dial us back (needs the listener/UPnP forwarding).
            int my_port = torrent_announce_port();
            u64 search_iv = (u64)(s_bg_hungry ? 5 : 15) * freq;
            mutexLock(&s_bg_mtx);
            for (int i = 0; i < DHT_MAX_TARGETS; i++) {
                dht_target *tg = &s_targets[i];
                if (!tg->active) continue;
                if (now - tg->last_search > search_iv) {
                    tg->last_search = now;
                    dht_search(tg->info_hash, my_port, AF_INET, bg_on_event, tg);
                }
            }
            mutexUnlock(&s_bg_mtx);

            // Random-id walk: fills buckets across the whole id space.
            // anacrolix refreshes buckets until they are full; a sparse
            // table must walk aggressively or it decays (measured in the
            // field: 163 good nodes -> 12 in ~10 min with a 60 s walk,
            // because jech never re-pings nodes off the search path).
            u64 walk_iv = (u64)(good < 150 ? 15 : 60) * freq;
            if (now - s_last_walk > walk_iv) {
                uint8_t rid[20];
                randomGet(rid, sizeof(rid));
                dht_search(rid, 0, AF_INET, NULL, NULL);
                s_last_walk = now;
            }
        }

        // Table refresh sweep (anacrolix TableMaintainer equivalent): jech
        // marks a node dubious 15 min after its last reply and never pings
        // it again unless it sits on a search path, so the table decays
        // passively. A rolling sweep pings every known node every 10 min:
        // live nodes refresh their reply time and stay good, dead ones stay
        // dubious and get replaced by the walk above. Rate-limited to a
        // handful of pings per loop iteration (~1/s) to avoid a UDP burst.
        {
            u64 sweep_iv = (u64)600 * freq;
            if (s_sweep_start == 0) s_sweep_start = now;
            if (now - s_sweep_start > sweep_iv) {
                struct sockaddr_in sins_p[DHT_CACHE_MAX_NODES];
                int num_p = DHT_CACHE_MAX_NODES, num6_p = 0;
                dht_get_nodes(sins_p, &num_p, NULL, &num6_p);
                int sent = 0;
                while (sent < 8 && s_sweep_pos < num_p) {
                    dht_ping_node((struct sockaddr *)&sins_p[s_sweep_pos],
                                  sizeof(sins_p[s_sweep_pos]));
                    s_sweep_pos++;
                    sent++;
                }
                if (s_sweep_pos >= num_p) {
                    engine_log(ENGINE_LOG_INFO,
                               "[dht] table refresh sweep done (%d nodes)", num_p);
                    s_sweep_pos = 0;
                    s_sweep_start = now;
                }
            }
        }

        if (now - last_log > (u64)2 * freq) {
            bool changed = (good != s_log_good || dubious != s_log_dubious ||
                            s_last_peers_found != s_log_peers);
            if (changed || now - s_last_change_log > (u64)60 * freq) {
                engine_log(ENGINE_LOG_INFO,
                           "[dht] background nodes=%d/%d peers_found=%d",
                           good, dubious, s_last_peers_found);
                s_log_good = good;
                s_log_dubious = dubious;
                s_log_peers = s_last_peers_found;
                s_last_change_log = now;
            }
            last_log = now;

            // Periodically persist known good nodes every 3 minutes so a crash/shutdown
            // does not lose warm DHT routing table.
            static u64 last_cache_save = 0;
            if (good >= 30 && (last_cache_save == 0 || now - last_cache_save > (u64)180 * freq)) {
                last_cache_save = now;
                struct sockaddr_in sins_p[DHT_CACHE_MAX_NODES];
                int num_p = DHT_CACHE_MAX_NODES, num6_p = 0;
                dht_get_nodes(sins_p, &num_p, NULL, &num6_p);
                if (num_p > 0) {
                    uint8_t nodes_p[DHT_CACHE_MAX_NODES][6];
                    for (int i = 0; i < num_p; i++) {
                        memcpy(nodes_p[i], &sins_p[i].sin_addr, 4);
                        memcpy(nodes_p[i] + 4, &sins_p[i].sin_port, 2);
                    }
                    dht_cache_write(s_dht_cache_path, s_bg_node_id, nodes_p, num_p);
                }
            }
        }
    }

    /* Persist good nodes for the next warm start. */
    struct sockaddr_in sins[DHT_CACHE_MAX_NODES];
    int num = DHT_CACHE_MAX_NODES, num6 = 0;
    dht_get_nodes(sins, &num, NULL, &num6);
    if (num > 0) {
        uint8_t nodes[DHT_CACHE_MAX_NODES][6];
        for (int i = 0; i < num; i++) {
            memcpy(nodes[i], &sins[i].sin_addr, 4);
            memcpy(nodes[i] + 4, &sins[i].sin_port, 2);
        }
        if (dht_cache_write(s_dht_cache_path, s_bg_node_id, nodes, num))
            engine_log(ENGINE_LOG_INFO, "[dht] saved %d nodes to cache", num);
    }

    dht_uninit();
    if (s_bg_sock >= 0) close(s_bg_sock);
    s_bg_sock = -1;
    s_bg_bootstrapped = false;
}

static void dht_bg_ensure_init(void);

static int dht_bg_start(void) {
    dht_bg_ensure_init();
    mutexLock(&s_bg_mtx);
    if (s_bg_thread_started) {
        mutexUnlock(&s_bg_mtx);
        return 1;
    }
    s_bg_stop = false;
    if (threadCreate(&s_bg_thread, dht_bg_main, NULL, NULL,
                     0x20000, 0x2C, -2) == 0) {
        threadStart(&s_bg_thread);
        s_bg_thread_started = true;
        mutexUnlock(&s_bg_mtx);
        return 1;
    }
    s_bg_stop = true;
    mutexUnlock(&s_bg_mtx);
    engine_log(ENGINE_LOG_ERROR, "[dht] background thread create failed");
    return 0;
}

static void dht_bg_ensure_init(void) {
    if (s_bg_init) return;
    mutexInit(&s_bg_mtx);
    s_bg_init = 1;
}

// Engine-startup hook: makes the lazily-initialised background mutex ready
// before any thread can reach it (see dhtclient.h).
void dht_bg_init_early(void) {
    dht_bg_ensure_init();
}
// Stop the persistent DHT thread (engine shutdown). Saves the node cache.
void dht_stop(void) {
    if (!s_bg_init) return;
    mutexLock(&s_bg_mtx);
    s_bg_stop = true;
    bool started = s_bg_thread_started;
    s_bg_thread_started = false;
    mutexUnlock(&s_bg_mtx);
    if (started) {
        threadWaitForExit(&s_bg_thread);
        threadClose(&s_bg_thread);
    }
}

int dht_find_peers(const uint8_t info_hash[20], int target_peers, int budget_ms,
                   dht_peer_cb cb, void *ctx, const volatile bool *cancel,
                   char *err, size_t errlen) {
    if (!dht_bg_start()) {
        if (err) snprintf(err, errlen, "DHT thread failed to start");
        return -1;
    }

    dht_target *tg = NULL;
    mutexLock(&s_bg_mtx);
    for (int i = 0; i < DHT_MAX_TARGETS; i++) {
        if (!s_targets[i].active) {
            tg = &s_targets[i];
            break;
        }
    }
    if (!tg) {
        mutexUnlock(&s_bg_mtx);
        if (err) snprintf(err, errlen, "no free DHT target slot");
        return -1;
    }
    tg->active = true;
    memcpy(tg->info_hash, info_hash, 20);
    tg->cb = cb;
    tg->ctx = ctx;
    tg->delivered = 0;
    tg->last_search = 0;
    mutexUnlock(&s_bg_mtx);

    dlog("DHT lookup attached (%d nodes)", s_last_good + s_last_dubious);

    u64 freq = armGetSystemTickFreq();
    u64 start = armGetSystemTick();
    u64 last_log = start;
    for (;;) {
        if (cancel && *cancel) break;
        double elapsed_ms = (double)(armGetSystemTick() - start) / freq * 1000.0;
        if (elapsed_ms >= (double)budget_ms) break;

        mutexLock(&s_bg_mtx);
        int d = tg->delivered;
        mutexUnlock(&s_bg_mtx);
        if (d >= target_peers) break;

        if (armGetSystemTick() - last_log > (u64)2 * freq) {
            dlog("DHT: %d noeuds, %d peers",
                 s_last_good + s_last_dubious, d);
            last_log = armGetSystemTick();
        }
        svcSleepThread(50000000ULL);  // 50 ms
    }

    mutexLock(&s_bg_mtx);
    int delivered = tg->delivered;
    tg->active = false;
    tg->cb = NULL;
    tg->ctx = NULL;
    mutexUnlock(&s_bg_mtx);

    dlog("DHT fin: %d peers", delivered);
    // NB: s_last_peers_found is deliberately NOT reset here. It is a
    // session-cumulative diagnostic: a value that stops growing means the
    // DHT stopped delivering peers entirely (usually a decayed routing
    // table), which is exactly what the log should show.
    return delivered;
}

// Register a persistent target: the background thread keeps searching it and
// delivers peers through cb until removed. Used by torrentfs for continuous
// peer discovery while a torrent is open.
void dht_background_add(const uint8_t info_hash[20],
                        dht_peer_cb cb, void *ctx) {
    dht_bg_start();

    mutexLock(&s_bg_mtx);
    for (int i = 0; i < DHT_MAX_TARGETS; i++) {
        if (s_targets[i].active &&
            memcmp(s_targets[i].info_hash, info_hash, 20) == 0) {
            s_targets[i].cb = cb;
            s_targets[i].ctx = ctx;
            mutexUnlock(&s_bg_mtx);
            return;
        }
    }
    for (int i = 0; i < DHT_MAX_TARGETS; i++) {
        if (!s_targets[i].active) {
            dht_target *tg = &s_targets[i];
            memcpy(tg->info_hash, info_hash, 20);
            tg->cb = cb;
            tg->ctx = ctx;
            tg->active = true;
            tg->delivered = 0;
            tg->last_search = 0;
            break;
        }
    }
    mutexUnlock(&s_bg_mtx);
}

void dht_background_remove(const uint8_t info_hash[20]) {
    if (!s_bg_init) return;
    mutexLock(&s_bg_mtx);
    for (int i = 0; i < DHT_MAX_TARGETS; i++) {
        if (s_targets[i].active &&
            memcmp(s_targets[i].info_hash, info_hash, 20) == 0) {
            s_targets[i].active = false;
            s_targets[i].cb = NULL;
            s_targets[i].ctx = NULL;
            break;
        }
    }
    mutexUnlock(&s_bg_mtx);
    // The thread itself keeps running: the routing table stays warm for the
    // next lookup and is saved on engine shutdown (dht_stop).
}
