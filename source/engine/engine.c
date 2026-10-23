#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "torrentfs.h"
#include "torrent_meta.h"
#include "dhtclient.h"
#include "utp_nb.h"
#include "listen.h"
#include "upnp.h"
#include "engine_log.h"
#include "magnet.h"
#include "../utils/app_paths.h"

#define MAX_TORRENTS TSNX_MAX_TORRENTS

static char s_last_err[256] = {0};

struct tsnx_torrent {
    char            hash[TSNX_MAX_HASH_LEN + 1];
    char           *source;     /* magnet URI or .torrent path (owned) */
    torrentfs      *fs;
    torrent_meta    meta;
    int             file_index;
    bool            used;
    bool            paused;
    bool           *wanted_files;
    int             wanted_files_count;
    const volatile bool *cancel; /* polled by (re)opens of this torrent */
    uint64_t        bytes_recv_at_start;
    uint64_t        last_bytes_recv;
    uint64_t        last_speed_time_ms;
    float           download_kbps;

    /* Registry state -- all guarded by g_reg (see "Registry locking"). */
    bool            reserved;   /* taken by an add still fetching metadata (not findable) */
    bool            removing;   /* remove in progress: no new users */
    int             holders;    /* threads in a long call that need the slot to stay */
    int             fs_users;   /* threads using `fs` outside g_reg */
    Mutex           open_mtx;   /* serializes opening / replacing `fs` on this slot */
};

struct tsnx_engine {
    tsnx_torrent    torrents[MAX_TORRENTS];
    int             port;
    bool            running;
};

static tsnx_engine *g_engine = NULL;

static tsnx_engine *active_engine(tsnx_engine *eng) {
    return eng ? eng : g_engine;
}

//-----------------------------------------------------------------------------
// Registry locking
//
// The registry used to be completely unlocked: the UI, the download progress
// thread, the installer's reader and the listener all walked `torrents[]` and
// dereferenced `fs` while another thread removed the torrent (torrentfs_close
// frees it) or swapped `fs` for another file -- use-after-free.
//
//   g_reg       one mutex for slot state and the `fs` pointer. Short calls
//               (stats, zones, peers, pause, ...) run with it held, so the
//               torrentfs they touch cannot be closed under them. Lock order:
//               g_reg -> torrentfs-internal locks; nothing inside torrentfs
//               calls back into the registry.
//   fs_users    long calls on `fs` (the blocking read, select_file) run
//               without g_reg, counted here. Detaching `fs` (set NULL under
//               g_reg) stops new users; the closer cancels it, waits for
//               fs_users == 0, and only then closes it, outside g_reg.
//   holders     long calls that need the slot itself (prepare_stream, the
//               blocking read). remove waits for holders == 0 before it
//               recycles the slot.
//   reserved    an add fetching metadata owns a slot that no lookup can see
//               until it is published (used = true).
//-----------------------------------------------------------------------------
static Mutex g_reg;
static CondVar g_reg_cv;

#define REG_LOCK()   mutexLock(&g_reg)
#define REG_UNLOCK() mutexUnlock(&g_reg)

static void reg_wait(void) {   // caller holds g_reg
    condvarWaitTimeout(&g_reg_cv, &g_reg, 20000000ULL);   // 20 ms, re-check
}
static void reg_wake(void) { condvarWakeAll(&g_reg_cv); }

// Caller holds g_reg. Published, not being removed.
static tsnx_torrent *find_locked(tsnx_engine *eng, const char *hash) {
    if (!eng || !hash || !eng->running) return NULL;
    for (int i = 0; i < MAX_TORRENTS; i++) {
        tsnx_torrent *t = &eng->torrents[i];
        if (t->used && !t->removing && strcmp(t->hash, hash) == 0) return t;
    }
    return NULL;
}

// Pin a slot (and optionally its current fs) for a call that runs without
// g_reg. Returns NULL if there is no such torrent (or no fs when want_fs).
static tsnx_torrent *hold(tsnx_engine *eng, const char *hash, bool want_fs,
                          torrentfs **fs_out) {
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && want_fs) {
        if (!t->fs) t = NULL;
        else { t->fs_users++; *fs_out = t->fs; }
    }
    if (t) t->holders++;
    REG_UNLOCK();
    return t;
}

static void unhold(tsnx_torrent *t, bool used_fs) {
    REG_LOCK();
    if (used_fs && t->fs_users > 0) t->fs_users--;
    if (t->holders > 0) t->holders--;
    reg_wake();
    REG_UNLOCK();
}

// Caller holds g_reg. Takes `fs` out of the slot so nobody new can use it,
// unblocks a reader stuck on it, and waits until its current users are gone.
// The caller closes the returned torrentfs after dropping g_reg.
static torrentfs *detach_fs_locked(tsnx_torrent *t) {
    torrentfs *fs = t->fs;
    t->fs = NULL;
    if (fs) {
        torrentfs_cancel(fs);
        while (t->fs_users > 0) reg_wait();
    }
    return fs;
}

// Caller holds g_reg; the slot is published. Removes it, dropping g_reg while
// the torrent is torn down (joins its threads: can take a while).
static void remove_locked(tsnx_torrent *t) {
    t->removing = true;
    torrentfs *fs = detach_fs_locked(t);
    while (t->holders > 0 || t->fs_users > 0) reg_wait();

    // The slot is ours alone now: take what must be freed, then recycle it.
    torrent_meta meta = t->meta;
    char *source = t->source;
    bool *wanted = t->wanted_files;
    t->source = NULL;
    t->wanted_files = NULL;
    t->wanted_files_count = 0;
    memset(&t->meta, 0, sizeof(t->meta));
    t->hash[0] = '\0';
    t->cancel = NULL;
    t->paused = false;
    t->file_index = -1;
    t->bytes_recv_at_start = t->last_bytes_recv = t->last_speed_time_ms = 0;
    t->download_kbps = 0.0f;
    t->used = false;
    t->removing = false;
    reg_wake();
    REG_UNLOCK();

    torrentfs_close(fs);
    torrent_unload(&meta);
    free(source);
    free(wanted);
    REG_LOCK();
}

// Reserve a free slot for an add (caller holds g_reg).
static tsnx_torrent *reserve_slot_locked(tsnx_engine *eng) {
    if (!eng || !eng->running) return NULL;
    for (int i = 0; i < MAX_TORRENTS; i++) {
        tsnx_torrent *t = &eng->torrents[i];
        if (!t->used && !t->reserved && t->holders == 0 && t->fs_users == 0) {
            t->reserved = true;
            return t;
        }
    }
    return NULL;
}

// Give back a reservation whose add failed (meta already unloaded or empty).
static void unreserve(tsnx_torrent *t) {
    REG_LOCK();
    memset(&t->meta, 0, sizeof(t->meta));
    t->reserved = false;
    reg_wake();
    REG_UNLOCK();
}

static void init_wanted(tsnx_torrent *t) {
    free(t->wanted_files);
    t->wanted_files_count = t->meta.file_count;
    t->wanted_files = (bool *)malloc(t->wanted_files_count > 0
        ? (size_t)t->wanted_files_count * sizeof(bool) : sizeof(bool));
    if (t->wanted_files)
        for (int i = 0; i < t->wanted_files_count; i++) t->wanted_files[i] = true;
}

// Publish a reserved slot (caller holds g_reg): from here lookups find it.
static void publish_locked(tsnx_torrent *t, const char *source, torrentfs *fs,
                           int file_index, const volatile bool *cancel) {
    free(t->source);
    t->source = source ? strdup(source) : NULL;
    t->fs = fs;
    t->file_index = file_index;
    t->cancel = cancel;
    t->paused = false;
    init_wanted(t);
    t->bytes_recv_at_start = 0;
    t->last_bytes_recv = 0;
    t->last_speed_time_ms = 0;
    t->download_kbps = 0.0f;
    t->used = true;
    t->reserved = false;
}

// Open a torrentfs for a published slot the caller holds (holders > 0) and
// whose open_mtx it owns; installs it unless the slot is being removed.
static bool slot_open_fs(tsnx_torrent *t, const char *source, int file_index,
                         const volatile bool *cancel, char *err, size_t errlen) {
    torrentfs *nfs = torrentfs_open_file_cancel(source, TSNX_TORRENTFS_CACHE,
                                                file_index, cancel, err, errlen);
    if (!nfs) return false;
    REG_LOCK();
    if (t->removing || t->fs) {
        // Removed meanwhile (or someone else opened it, which open_mtx should
        // make impossible): never leak or double-install a torrentfs.
        REG_UNLOCK();
        torrentfs_cancel(nfs);
        torrentfs_close(nfs);
        return false;
    }
    t->fs = nfs;
    t->file_index = file_index;
    if (t->paused) torrentfs_pause(nfs, 1);
    REG_UNLOCK();
    return true;
}

static void dht_log_cb(const char *msg) {
    engine_log(ENGINE_LOG_INFO, "[dht] %s", msg);
}

static void torrent_log_cb(const char *msg) {
    engine_log(ENGINE_LOG_INFO, "[meta] %s", msg);
}

static uint64_t now_ms(void) {
#ifdef __SWITCH__
    return armGetSystemTick() / (armGetSystemTickFreq() / 1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void bin_to_hex(const uint8_t *bin, char *hex, size_t hex_len) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < hex_len / 2; i++) {
        hex[i * 2]     = digits[bin[i] >> 4];
        hex[i * 2 + 1] = digits[bin[i] & 0x0f];
    }
    hex[i * 2] = '\0';
}

// Routes an incoming connection (handshake already read) to the matching open
// torrent. Called from the listener thread. The lookup and the hand-over run
// under g_reg, so the torrentfs cannot be closed in between (the listener used
// to get a bare pointer back and push to it later). False = drop the socket.
static bool engine_route_incoming(const uint8_t info_hash[20], int fd,
                                  const uint8_t hs68[68]) {
    char hex[41];
    bin_to_hex(info_hash, hex, 40);
    bool ok = false;
    REG_LOCK();
    tsnx_torrent *t = find_locked(g_engine, hex);
    if (t && t->fs) {
        torrentfs_incoming_push(t->fs, fd, hs68);   // takes ownership of fd
        ok = true;
    }
    REG_UNLOCK();
    return ok;
}

//-----------------------------------------------------------------------------
// Watchdog (PC builds only): samples every engine thread's heartbeat and
// writes the ages to watchdog.log, so a hung process can be diagnosed without
// a debugger. Lock-free, standalone file -- immune to the log mutex and to a
// blocked stdio pipe.
//-----------------------------------------------------------------------------
#ifndef __SWITCH__
static volatile bool g_wd_stop = false;
static Thread g_wd_thread;

static void watchdog_main(void *arg) {
    (void)arg;
    FILE *f = fopen("watchdog.log", "w");
    if (!f) return;
    static const char *names[8] = {
        "dht", "disc", "listen", "upnp", "net", "writer", "reader", "wd"
    };
    u64 freq = armGetSystemTickFreq();
    while (!g_wd_stop) {
        svcSleepThread(5000000000ULL);  // 5 s
        tsnx_engine_wd_tick(7);
        u64 now = armGetSystemTick();
        fprintf(f, "t=%llu", (unsigned long long)(now / freq));
        for (int i = 0; i < 8; i++) {
            u64 tick = tsnx_engine_wd_last(i);
            unsigned long long elapsed = 99999ULL;
            if (tick) {
                elapsed = (now >= tick) ? (unsigned long long)((now - tick) / freq) : 0ULL;
            }
            fprintf(f, " %s=%llu", names[i], elapsed);
        }
        fprintf(f, "\n");
        fflush(f);
    }
    fclose(f);
}
#endif

tsnx_engine *tsnx_engine_start(int listen_port) {
    tsnx_engine *eng = calloc(1, sizeof(tsnx_engine));
    if (!eng) return NULL;

    // Never die on a write to a socket a peer just closed. POSIX send() raises
    // SIGPIPE before returning the error and the default action kills the
    // process; the bsd service on Switch does not deliver it, but any PC build
    // of the engine would be taken down by the first reset peer.
    signal(SIGPIPE, SIG_IGN);

    mutexInit(&g_reg);
    condvarInit(&g_reg_cv);
    for (int i = 0; i < MAX_TORRENTS; i++) {
        mutexInit(&eng->torrents[i].open_mtx);
        eng->torrents[i].file_index = -1;
    }

    eng->port = listen_port;
    eng->running = true;
    engine_log_init("sdmc:/switch/TorrentShopNX/engine.log");
    engine_log(ENGINE_LOG_INFO, "engine start port=%d", listen_port);
    dht_set_log(dht_log_cb);
    torrent_set_log(torrent_log_cb);
    // Make the lazily-initialised engine mutexes ready single-threaded, before
    // the first announce/DHT thread can touch them (PC compat shim lazy-init
    // would otherwise race on first use).
    torrent_announce_mutex_init();
    dht_bg_init_early();
    torrentfs_set_ram_stream(1);
    utp_nb_init();
    g_engine = eng;

    // Incoming connections: bind the port and advertise it (trackers/DHT).
    // UPnP then tries to make the port reachable from the internet; its
    // success overrides the advertised port with the mapped external one.
    if (torrent_listener_start(listen_port, engine_route_incoming) == 0) {
        torrent_set_announce_port(torrent_listener_port());
        upnp_start(torrent_listener_port());
    } else {
        engine_log(ENGINE_LOG_WARN,
                   "[engine] listener failed: incoming connections disabled");
    }
#ifndef __SWITCH__
    g_wd_stop = false;
    if (threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x20000, 0x2C,
                     -2) == 0)
        threadStart(&g_wd_thread);
#endif
    return eng;
}

void tsnx_engine_stop(tsnx_engine *eng) {
    eng = active_engine(eng);
    if (!eng) return;
    // Stop listener first so incoming connections cannot race with torrentfs_close.
    torrent_listener_stop();

    REG_LOCK();
    eng->running = false;   // lookups and new adds fail from here
    for (int i = 0; i < MAX_TORRENTS; i++) {
        tsnx_torrent *t = &eng->torrents[i];
        if (t->used && !t->removing) remove_locked(t);   // drops/retakes g_reg
    }
    // An add still fetching metadata owns a reserved slot inside `eng`; it
    // fails as soon as it sees !running, but it must be gone before `eng` is
    // freed. Bounded: if one is stuck, leak the struct rather than free it.
    bool busy = true;
    for (int waited = 0; waited < 500 && busy; waited++) {   // ~10 s
        busy = false;
        for (int i = 0; i < MAX_TORRENTS; i++) {
            tsnx_torrent *t = &eng->torrents[i];
            if (t->reserved || t->holders > 0 || t->fs_users > 0) busy = true;
        }
        if (busy) reg_wait();
    }
    if (g_engine == eng) g_engine = NULL;
    REG_UNLOCK();

    engine_log(ENGINE_LOG_INFO, "engine stop");
#ifndef __SWITCH__
    g_wd_stop = true;
    threadWaitForExit(&g_wd_thread);
    threadClose(&g_wd_thread);
#endif
    upnp_stop();               // no more mapping announcements
    dht_stop();  // persistent DHT: save the routing table for the next start
    utp_nb_exit();
    if (busy) {
        engine_log(ENGINE_LOG_WARN, "[engine] stop: an add is still running, engine state leaked");
        engine_log_close();
        return;
    }
    engine_log_close();
    free(eng);
}

bool tsnx_engine_running(const tsnx_engine *eng) {
    eng = active_engine((tsnx_engine *)eng);
    return eng && eng->running;
}

bool tsnx_engine_add_torrent_file(tsnx_engine *eng, const char *path,
                                  char *out_hash, size_t out_hash_len) {
    char err[256] = {0};
    eng = active_engine(eng);
    if (!eng || !path || !out_hash || out_hash_len < TSNX_MAX_HASH_LEN + 1)
        return false;
    REG_LOCK();
    tsnx_torrent *slot = reserve_slot_locked(eng);
    REG_UNLOCK();
    if (!slot) return false;
    if (torrent_load(&slot->meta, path, err, sizeof(err)) != 0) {
        unreserve(slot);
        return false;
    }
    torrentfs *fs = torrentfs_open_file(path, TSNX_TORRENTFS_CACHE,
                                        -1, err, sizeof(err));
    if (!fs) {
        torrent_unload(&slot->meta);
        unreserve(slot);
        return false;
    }
    REG_LOCK();
    if (!eng->running) {
        REG_UNLOCK();
        torrentfs_cancel(fs);
        torrentfs_close(fs);
        torrent_unload(&slot->meta);
        unreserve(slot);
        return false;
    }
    bin_to_hex(slot->meta.info_hash, slot->hash, 40);
    publish_locked(slot, path, fs, -1, NULL);
    snprintf(out_hash, out_hash_len, "%s", slot->hash);
    REG_UNLOCK();
    fprintf(stderr, "[engine] add_torrent_file ok hash=%s\n", out_hash);
    return true;
}

bool tsnx_engine_add_magnet(tsnx_engine *eng, const char *magnet_uri,
                            char *out_hash, size_t out_hash_len) {
    return tsnx_engine_add_magnet_ex(eng, magnet_uri, -1, false, NULL,
                                     out_hash, out_hash_len);
}

// Already registered (probe kept it): open the download on it if asked.
static bool add_to_existing(tsnx_engine *eng, const char *hex,
                            const char *magnet_uri, int file_index,
                            bool meta_only, const volatile bool *cancel,
                            char *out_hash, size_t out_hash_len, bool *found) {
    char err[256] = {0};
    torrentfs *unused = NULL;
    tsnx_torrent *t = hold(eng, hex, false, &unused);
    *found = t != NULL;
    if (!t) return false;
    snprintf(out_hash, out_hash_len, "%s", hex);
    bool ok = true;
    if (!meta_only) {
        mutexLock(&t->open_mtx);
        REG_LOCK();
        bool need_open = t->fs == NULL;
        if (need_open) {
            t->cancel = cancel;
            t->file_index = file_index;
        }
        REG_UNLOCK();
        if (need_open)
            ok = slot_open_fs(t, magnet_uri, file_index, cancel, err, sizeof(err));
        mutexUnlock(&t->open_mtx);
    }
    unhold(t, false);
    return ok;
}

/*
 * Magnet add with the full set of options:
 *   file_index  - stream target for the initial torrentfs open (-1 = largest)
 *   meta_only   - fetch metadata into the slot but do NOT open torrentfs
 *                 (used by the file-list probe: no download threads, no RAM)
 *   cancel      - polled while metadata is fetched (may be NULL)
 *
 * With meta_only the slot stays registered: get_files() and a later
 * prepare_stream() (which opens torrentfs on demand) both work on it.
 */
bool tsnx_engine_add_magnet_ex(tsnx_engine *eng, const char *magnet_uri,
                               int file_index, bool meta_only,
                               const volatile bool *cancel,
                               char *out_hash, size_t out_hash_len) {
    char err[256] = {0};
    eng = active_engine(eng);
    if (!eng || !magnet_uri || !out_hash || out_hash_len < TSNX_MAX_HASH_LEN + 1)
        return false;

    // Fast-path: check if this magnet hash is already registered in the engine
    magnet_info minfo;
    if (magnet_parse(magnet_uri, &minfo, err, sizeof(err)) == 0) {
        char parsed_hex[TSNX_MAX_HASH_LEN + 1];
        bin_to_hex(minfo.info_hash, parsed_hex, 40);
        magnet_free(&minfo);
        bool found = false;
        bool ok = add_to_existing(eng, parsed_hex, magnet_uri, file_index, meta_only,
                                  cancel, out_hash, out_hash_len, &found);
        if (found) return ok;
    }

    REG_LOCK();
    tsnx_torrent *slot = reserve_slot_locked(eng);
    if (slot) {
        // Briefly pause new peer dials on active downloads to give network &
        // socket headroom for the new torrent's announces and metadata fetch.
        for (int i = 0; i < MAX_TORRENTS; i++) {
            tsnx_torrent *t = &eng->torrents[i];
            if (t->used && !t->removing && t->fs) torrentfs_pause_dials(t->fs, 5);
        }
    }
    REG_UNLOCK();
    if (!slot) {
        snprintf(s_last_err, sizeof(s_last_err), "no free torrent slots");
        return false;
    }

    // The slot is reserved: invisible to lookups while the metadata arrives.
    if (torrent_load_magnet_cancel(&slot->meta, magnet_uri, cancel,
                                   err, sizeof(err)) != 0) {
        snprintf(s_last_err, sizeof(s_last_err), "%s", err[0] ? err : "metadata fetch failed");
        unreserve(slot);
        return false;
    }
    char hex[TSNX_MAX_HASH_LEN + 1];
    bin_to_hex(slot->meta.info_hash, hex, 40);

    // Someone registered the same torrent while we fetched: use theirs.
    REG_LOCK();
    bool dup = find_locked(eng, hex) != NULL;
    REG_UNLOCK();
    if (dup) {
        torrent_unload(&slot->meta);
        unreserve(slot);
        bool found = false;
        bool ok = add_to_existing(eng, hex, magnet_uri, file_index, meta_only,
                                  cancel, out_hash, out_hash_len, &found);
        return found && ok;
    }

    torrentfs *fs = NULL;
    if (!meta_only) {
        fs = torrentfs_open_file_cancel(magnet_uri, TSNX_TORRENTFS_CACHE,
                                        file_index, cancel, err, sizeof(err));
        if (!fs) {
            snprintf(s_last_err, sizeof(s_last_err), "%s", err[0] ? err : "torrentfs_open failed");
            torrent_unload(&slot->meta);
            unreserve(slot);
            return false;
        }
    }

    REG_LOCK();
    if (!eng->running) {
        REG_UNLOCK();
        if (fs) { torrentfs_cancel(fs); torrentfs_close(fs); }
        torrent_unload(&slot->meta);
        unreserve(slot);
        return false;
    }
    memcpy(slot->hash, hex, sizeof(hex));
    publish_locked(slot, magnet_uri, fs, file_index, cancel);
    snprintf(out_hash, out_hash_len, "%s", slot->hash);
    REG_UNLOCK();
    return true;
}

bool tsnx_engine_has_torrent(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    bool found = find_locked(eng, hash) != NULL;
    REG_UNLOCK();
    return found;
}

const char *tsnx_engine_get_last_error(tsnx_engine *eng) {
    (void)eng;
    return s_last_err;
}

/* Update the cancel flag a torrent's (re)opens poll. The download backend
 * takes ownership of the slot the probe kept, so the slot must stop polling
 * the probe's flag and follow the download's instead. */
void tsnx_engine_set_cancel(tsnx_engine *eng, const char *hash,
                            const volatile bool *flag) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t) t->cancel = flag;
    REG_UNLOCK();
}

bool tsnx_engine_remove_torrent(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t) remove_locked(t);
    REG_UNLOCK();
    return t != NULL;
}

bool tsnx_engine_pause_torrent(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t) {
        t->paused = true;
        if (t->fs) torrentfs_pause(t->fs, 1);
    }
    REG_UNLOCK();
    if (!t) return false;
    engine_log(ENGINE_LOG_INFO, "[engine] pause torrent %s", hash);
    return true;
}

bool tsnx_engine_resume_torrent(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t) {
        t->paused = false;
        if (t->fs) torrentfs_pause(t->fs, 0);
    }
    REG_UNLOCK();
    if (!t) return false;
    engine_log(ENGINE_LOG_INFO, "[engine] resume torrent %s", hash);
    return true;
}

// One list entry for a published slot (caller holds g_reg). Also advances the
// slot's speed EWMA -- under g_reg now, so concurrent callers no longer race
// on its sample state.
static void fill_item_locked(tsnx_torrent *t, tsnx_torrent_item *it) {
    int64_t done, total, ph;
    uint64_t now;
    uint64_t bytes_recv;
    snprintf(it->hash, sizeof(it->hash), "%s", t->hash);
    // Metadata-only slots (probe result) have no torrentfs yet.
    torrentfs *fs = t->fs;
    if (fs) {
        snprintf(it->name, sizeof(it->name), "%s", torrentfs_name(fs));
        torrentfs_stats(fs, &done, &total, &ph);
        it->progress = total > 0 ? (float)done / (float)total : 0.0f;
        it->loaded_size = done;
        it->total_size = total;

        int live = 0, peak = 0, connecting = 0;
        torrentfs_live_peers(fs, &live, &peak, &connecting);
        it->seeds       = torrentfs_seed_count(fs); // peers holding the whole file
        it->peers       = live;                        // currently connected sessions
        it->known_peers = torrentfs_peer_count(fs); // discovered pool

        bytes_recv = (uint64_t)torrentfs_bytes_recv(fs);
    } else {
        snprintf(it->name, sizeof(it->name), "%s",
                 t->meta.name[0] ? t->meta.name : "torrent");
        it->progress = 0.0f;
        it->loaded_size = 0;
        it->total_size = t->meta.total_len;
        it->seeds = 0;
        it->peers = 0;
        it->known_peers = 0;
        bytes_recv = 0;
    }
    int good = 0, dubious = 0;
    dhtclient_get_nodes(&good, &dubious);
    it->dht_nodes = good + dubious;
    // Useful payload = received bytes minus duplicate re-deliveries
    // (late answers to expired/re-requested blocks). The UI shows the
    // real download rate, not wasted traffic.
    //
    // bytes_recv and dup_bytes are separate monotonic counters read a few
    // instructions apart, so a block landing between the reads can make
    // dup momentarily exceed bytes_recv. Done in uint64 that underflows
    // into ~2^64 and the EWMA reports gigabytes per second while the real
    // rate is a few KB/s. Signed arithmetic + clamp keeps that benign.
    now = now_ms();
    int64_t useful_now = (int64_t)bytes_recv -
                         (fs ? (int64_t)torrentfs_dup_bytes(fs) : 0);
    if (useful_now < 0) useful_now = 0;
    if (t->last_speed_time_ms == 0) {
        t->last_speed_time_ms = now;
        t->last_bytes_recv = (uint64_t)useful_now;
        t->download_kbps = 0.0f;
    } else {
        uint64_t dt = now - t->last_speed_time_ms;
        if (dt >= 500) {
            int64_t db = useful_now - (int64_t)t->last_bytes_recv;
            if (db < 0) db = 0;
            // dt is in milliseconds; convert to KB/s.
            float inst = (float)((double)db * 1000.0 / 1024.0 / (double)dt);
            if (inst < 0.0f) inst = 0.0f;
            // ~10 s EWMA: pieces arrive in bursts (several 8 MB pieces can
            // verify in the same second, then nothing for a while), so a
            // per-second sample bounces between line rate and idle and the
            // UI showed 8+ MB/s while the real sustained rate was ~1.5.
            if (t->download_kbps <= 0.0f) {
                t->download_kbps = inst;
            } else {
                float alpha = (float)dt / 10000.0f;
                if (alpha > 0.35f) alpha = 0.35f;
                t->download_kbps += (inst - t->download_kbps) * alpha;
            }
            t->last_speed_time_ms = now;
            t->last_bytes_recv = (uint64_t)useful_now;
        }
    }
    it->download_kbps = t->download_kbps;
}

int tsnx_engine_get_torrents(tsnx_engine *eng, tsnx_torrent_item *out,
                             int max_items) {
    int count = 0;
    eng = active_engine(eng);
    if (!eng || !out || max_items <= 0) return 0;
    REG_LOCK();
    for (int i = 0; i < MAX_TORRENTS && count < max_items; i++) {
        tsnx_torrent *t = &eng->torrents[i];
        if (!t->used || t->removing) continue;
        fill_item_locked(t, &out[count++]);
    }
    REG_UNLOCK();
    return count;
}

bool tsnx_engine_get_torrent(tsnx_engine *eng, const char *hash,
                             tsnx_torrent_item *out) {
    eng = active_engine(eng);
    if (!eng || !hash || !out) return false;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t) fill_item_locked(t, out);
    REG_UNLOCK();
    return t != NULL;
}

int tsnx_engine_get_files(tsnx_engine *eng, const char *hash,
                          tsnx_file_info *out, int max_files) {
    int count = 0;
    eng = active_engine(eng);
    if (!eng || !out || max_files <= 0) return 0;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t) {
        for (int i = 0; i < t->meta.file_count && count < max_files; i++) {
            tsnx_file_info *f = &out[count++];
            f->index = i;
            snprintf(f->path, sizeof(f->path), "%s", t->meta.files[i].path);
            f->size = t->meta.files[i].length;
            f->offset = t->meta.files[i].offset;
            f->wanted = true;
        }
    }
    REG_UNLOCK();
    return count;
}

int tsnx_engine_get_file_count(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    if (!eng || !hash) return 0;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    int n = t ? t->meta.file_count : 0;
    REG_UNLOCK();
    return n;
}

bool tsnx_engine_set_file_wanted(tsnx_engine *eng, const char *hash,
                                 int file_index, bool wanted) {
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    bool ok = false;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && file_index >= 0 && file_index < t->wanted_files_count && t->wanted_files) {
        t->wanted_files[file_index] = wanted;
        ok = true;
    }
    REG_UNLOCK();
    return ok;
}

bool tsnx_engine_prepare_stream(tsnx_engine *eng, const char *hash,
                                int file_index) {
    char err[256] = {0};
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    torrentfs *unused = NULL;
    tsnx_torrent *t = hold(eng, hash, false, &unused);
    if (!t) return false;

    bool ok = false;
    mutexLock(&t->open_mtx);   // one open/replace at a time on this slot
    REG_LOCK();
    if (file_index < 0 || file_index >= t->meta.file_count || t->removing) {
        REG_UNLOCK();
        goto out;
    }
    torrentfs *fs = t->fs;
    if (fs && t->file_index == file_index) {
        torrentfs_resume_reader(fs);
        REG_UNLOCK();
        ok = true;
        goto out;
    }
    if (fs) {
        // Switch files on the open stream without dropping peers. select_file
        // waits for the netloop (up to ~1 s): run it as an fs user, not under g_reg.
        t->fs_users++;
        REG_UNLOCK();
        bool switched = torrentfs_select_file(fs, file_index);
        REG_LOCK();
        t->fs_users--;
        reg_wake();
        if (switched) {
            t->file_index = file_index;
            if (t->paused) torrentfs_pause(fs, 1);
            REG_UNLOCK();
            ok = true;
            goto out;
        }
        // Could not switch: close the stream and reopen on the requested file.
        fs = detach_fs_locked(t);
    }
    char *source = strdup(t->source ? t->source : t->hash);
    const volatile bool *cancel = t->cancel;
    REG_UNLOCK();
    if (fs) torrentfs_close(fs);
    if (source) {
        ok = slot_open_fs(t, source, file_index, cancel, err, sizeof(err));
        free(source);
    }
out:
    mutexUnlock(&t->open_mtx);
    unhold(t, false);
    return ok;
}

int64_t tsnx_engine_read(tsnx_engine *eng, const char *hash,
                         int64_t offset, void *buf, int64_t size) {
    eng = active_engine(eng);
    if (!eng || !hash || !buf || size <= 0) return -1;
    torrentfs *fs = NULL;
    tsnx_torrent *t = hold(eng, hash, true, &fs);
    if (!t) return -1;
    // Blocking read without g_reg; a remove/reopen cancels fs and waits for us.
    int64_t r = torrentfs_read(fs, offset, (char *)buf, size);
    unhold(t, true);
    return r;
}

void tsnx_engine_cancel_read(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs) torrentfs_cancel_reader(t->fs);
    REG_UNLOCK();
}

void tsnx_engine_resume_reader(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs) torrentfs_resume_reader(t->fs);
    REG_UNLOCK();
}

void tsnx_engine_set_min_keep_offset(tsnx_engine *eng, const char *hash,
                                     int64_t offset) {
    eng = active_engine(eng);
    if (!eng || !hash) return;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    /* Advance the playhead so the RAM window follows the installer. */
    if (t && t->fs) torrentfs_set_playhead(t->fs, offset);
    REG_UNLOCK();
}

int tsnx_engine_piece_size(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    int n = (t && t->fs) ? (int)torrentfs_piece_len(t->fs) : 0;
    REG_UNLOCK();
    return n;
}

int64_t tsnx_engine_file_offset(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    int64_t off = (t && t->file_index >= 0 && t->file_index < t->meta.file_count)
                      ? t->meta.files[t->file_index].offset : 0;
    REG_UNLOCK();
    return off;
}

int tsnx_engine_get_peers(tsnx_engine *eng, const char *hash,
                          tsnx_peer_info *out, int max_peers) {
    if (!out || max_peers <= 0) return 0;
    eng = active_engine(eng);
    if (!eng || !hash) return 0;
    torrentfs_peer_info pinfos[64];
    int n = 0;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs)
        n = torrentfs_get_peers(t->fs, pinfos, max_peers < 64 ? max_peers : 64);
    REG_UNLOCK();
    for (int i = 0; i < n; i++) {
        out[i].ip         = pinfos[i].ip;
        out[i].port       = pinfos[i].port;
        out[i].bytes_recv = pinfos[i].bytes_recv;
        out[i].rate_bps   = pinfos[i].rate_bps;
        out[i].rtt_ms     = pinfos[i].rtt_ms;
        out[i].connecting = pinfos[i].connecting;
        out[i].handshaked = pinfos[i].handshaked;
        out[i].choked     = pinfos[i].choked;
        out[i].claim_piece= pinfos[i].claim_piece;
    }
    return n;
}

bool tsnx_engine_set_piece_zone(tsnx_engine *eng, const char *hash,
                                int first_piece, int piece_count,
                                tsnx_piece_zone zone) {
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    bool ok = false;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs)
        ok = torrentfs_set_piece_zone(t->fs, first_piece, piece_count, (int)zone);
    REG_UNLOCK();
    return ok;
}

bool tsnx_engine_clear_piece_zones(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    bool ok = false;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs) ok = torrentfs_clear_piece_zones(t->fs);
    REG_UNLOCK();
    return ok;
}

void tsnx_engine_set_backlog_ms(tsnx_engine *eng, const char *hash, int ms) {
    eng = active_engine(eng);
    if (!eng || !hash) return;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs) torrentfs_set_backlog(t->fs, ms);
    REG_UNLOCK();
}

void tsnx_engine_set_governor(tsnx_engine *eng, int on) {
    (void)eng;
    torrentfs_set_governor(on);
}

void tsnx_engine_set_ram_stream(tsnx_engine *eng, int on) {
    (void)eng;
    torrentfs_set_ram_stream(on);
}

void tsnx_engine_set_strict_verify(tsnx_engine *eng, const char *hash, int on) {
    eng = active_engine(eng);
    if (!eng || !hash) return;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs) torrentfs_set_strict_verify(t->fs, on);
    REG_UNLOCK();
}

bool tsnx_engine_announce_now(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    bool ok = false;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs) {
        torrentfs_announce_now(t->fs);
        ok = true;
    }
    REG_UNLOCK();
    return ok;
}

bool tsnx_engine_add_peers(tsnx_engine *eng, const char *hash,
                           const uint32_t *ips, const uint16_t *ports, int n) {
    if (!ips || !ports || n <= 0 || n > 256) return false;
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    peer_addr pa[256];
    for (int i = 0; i < n; i++) {
        pa[i].ip   = ips[i];
        pa[i].port = ports[i];
    }
    bool ok = false;
    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (t && t->fs) {
        torrentfs_add_peers(t->fs, pa, n);
        ok = true;
    }
    REG_UNLOCK();
    return ok;
}

bool tsnx_engine_get_diag(tsnx_engine *eng, const char *hash, tsnx_engine_diag *out) {
    eng = active_engine(eng);
    if (!eng || !hash || !out) return false;

    REG_LOCK();
    tsnx_torrent *t = find_locked(eng, hash);
    if (!t || !t->fs) {
        REG_UNLOCK();
        return false;
    }
    torrentfs *fs = t->fs;

    memset(out, 0, sizeof(*out));

    tsnx_torrent_item item;
    fill_item_locked(t, &item);
    out->peers = item.known_peers;
    out->download_kbps = item.download_kbps;

    int live = 0, peak = 0, connecting = 0;
    torrentfs_live_peers(fs, &live, &peak, &connecting);
    out->live = live;
    out->peak = peak;
    out->connecting = connecting;

    int claiming = 0, idle = 0;
    torrentfs_claim_stats(fs, &claiming, &idle);
    out->claiming = claiming;
    out->idle = idle;

    int empty = 0, ok = 0, bad = 0;
    torrentfs_bitfield_stats(fs, &empty, &ok, &bad);
    out->empty_bitfield = empty;
    out->good_bitfield = ok;
    out->bad_bitfield = bad;

    torrentfs_fail_kinds(fs, &out->sock_fail, &out->timeouts);
    out->hs_fail = torrentfs_hs_fail(fs);
    out->calm = torrentfs_calm(fs);
    out->bytes_recv = torrentfs_bytes_recv(fs);
    out->dup_bytes  = torrentfs_dup_bytes(fs);

    int64_t done = 0, total = 0, ph = 0;
    torrentfs_stats(fs, &done, &total, &ph);
    out->pieces_done = done;
    out->pieces_total = total;
    out->playhead_piece = ph;

    int status = 0, have = 0, req = 0, tot = 0;
    torrentfs_piece_debug(fs, ph, &status, &have, &req, &tot);
    out->piece_status = status;
    out->piece_have = have;
    out->piece_req = req;
    out->piece_total = tot;
    REG_UNLOCK();

    dhtclient_get_last_lookup(&out->dht_peers, &out->dht_good, &out->dht_dubious);
    return true;
}
