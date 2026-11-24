#include "torrent_meta.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <switch.h>
#include <curl/curl.h>
#include <mbedtls/sha1.h>

#include "udp_tracker.h"
#include "magnet.h"
#include "bt_peer.h"
#include "dhtclient.h"
#include "meta_fetch.h"
#include "thread_cfg.h"
#include "../utils/app_paths.h"

static void set_err(char *err, size_t errlen, const char *msg) {
    if (err && errlen) snprintf(err, errlen, "%s", msg);
}

// Process-wide peer id, generated on first use. The lazy init races benignly
// (both threads produce a valid id, one wins) and it never changes afterwards,
// so every announce/DHT/torrentfs user presents the same identity to trackers.
static uint8_t s_peer_id[20];
static volatile int s_peer_id_ready = 0;

void torrent_peer_id(uint8_t out[20]) {
    if (!s_peer_id_ready) {
        memcpy(s_peer_id, "-SW0004-", 8);
        srand((unsigned)time(NULL));
        for (int i = 8; i < 20; i++) s_peer_id[i] = (uint8_t)(rand() % 256);
        s_peer_id_ready = 1;
    }
    memcpy(out, s_peer_id, 20);
}

uint32_t torrent_announce_key(void) {
    uint8_t id[20];
    torrent_peer_id(id);
    uint32_t k;
    memcpy(&k, id + 12, 4);   // random bytes of the id: fixed for the process
    return k;
}

// The port we tell trackers/DHT about. 6881 until a listener binds (and UPnP
// possibly maps) a real one.
static int s_announce_port = 6881;

void torrent_set_announce_port(int port) {
    if (port > 0 && port < 65536) s_announce_port = port;
}

int torrent_announce_port(void) {
    return s_announce_port;
}

static void (*s_log_fn)(const char *) = NULL;

void torrent_set_log(void (*fn)(const char *)) { s_log_fn = fn; }

static void tlog(const char *fmt, ...) {
    if (!s_log_fn) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_log_fn(buf);
}

// Well-known public trackers, appended to every torrent so we discover far
// more peers than the torrent's own (often stale) tracker list provides.
// Mixed HTTP(S) and UDP: HTTP trackers are reached via curl, UDP via BEP 15.
// add_tracker dedups.
//
// Полевой прогон 2026-08-18 (Switch, t-ru swarm): из ~25 дефолтных трекеров
// живых оказалось четыре — остальные стабильно давали DNS-fail / timeout /
// no connect reply (0/10 в обоих прогонах) и только жгли сокеты и шумели в
// лог. Список почищен до подтверждённо живых. t-ru.org добавляется из
// магнита раздачи, в дефолт не входит.
static const char *DEFAULT_TRACKERS[] = {
    "udp://open.stealth.si:80/announce",
    "udp://exodus.desync.com:6969/announce",
    "udp://tracker.dler.org:6969/announce",
    "udp://tracker.torrent.eu.org:451/announce",
};

static void add_tracker(torrent_meta *t, const char *url, size_t len);

static void add_default_trackers(torrent_meta *t) {
    for (size_t i = 0; i < sizeof(DEFAULT_TRACKERS) / sizeof(*DEFAULT_TRACKERS); i++)
        add_tracker(t, DEFAULT_TRACKERS[i], strlen(DEFAULT_TRACKERS[i]));
}

static void add_tracker_single(torrent_meta *t, const char *url, size_t len) {
    if (t->tracker_count >= MAX_TRACKERS) return;
    int is_http = len > 4 && strncmp(url, "http", 4) == 0;
    int is_udp = len > 6 && strncmp(url, "udp://", 6) == 0;
    if (!is_http && !is_udp) return;
    for (int i = 0; i < t->tracker_count; i++)
        if (strlen(t->trackers[i]) == len && strncmp(t->trackers[i], url, len) == 0)
            return;
    char *copy = malloc(len + 1);
    if (!copy) return;
    memcpy(copy, url, len);
    copy[len] = '\0';
    t->trackers[t->tracker_count++] = copy;
}

// url points into the raw .torrent buffer, which is not NUL-terminated.
static int url_contains(const char *url, size_t len, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(url + i, needle, n) == 0) return 1;
    return 0;
}

static void add_tracker(torrent_meta *t, const char *url, size_t len) {
    add_tracker_single(t, url, len);
    if (url_contains(url, len, "t-ru.org/ann") || url_contains(url, len, "bt.t-ru.org") ||
        url_contains(url, len, "bt2.t-ru.org") || url_contains(url, len, "bt3.t-ru.org") ||
        url_contains(url, len, "bt4.t-ru.org")) {
        static const char *RU_MIRRORS[] = {
            "http://bt.t-ru.org/ann?magnet",
            "http://bt2.t-ru.org/ann?magnet",
            "http://bt3.t-ru.org/ann?magnet",
            "http://bt4.t-ru.org/ann?magnet"
        };
        for (size_t m = 0; m < sizeof(RU_MIRRORS)/sizeof(RU_MIRRORS[0]); m++) {
            add_tracker_single(t, RU_MIRRORS[m], strlen(RU_MIRRORS[m]));
        }
    }
}

//-----------------------------------------------------------------------------
// Persistent metadata cache, keyed by info-hash.
//
// Fetching metadata from the swarm is the slow, flaky part of opening a magnet
// (BEP 9 over mostly-dead peers). Once fetched, the raw info dict is stored on
// disk so every later open of the same magnet (probe -> download, or a future
// session) loads it locally instead of re-running the network fetch.
//-----------------------------------------------------------------------------

#define META_CACHE_DIR_DEFAULT TSNX_CACHE_META

#define META_CACHE_MAX_SIZE (8 * 1024 * 1024)

static char s_meta_cache_dir[256] = META_CACHE_DIR_DEFAULT;

void torrent_meta_cache_set_dir(const char *dir) {
    if (dir && dir[0])
        snprintf(s_meta_cache_dir, sizeof(s_meta_cache_dir), "%s", dir);
}

static void info_hash_hex(const uint8_t info_hash[20], char out[41]) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2]     = digits[info_hash[i] >> 4];
        out[i * 2 + 1] = digits[info_hash[i] & 0x0f];
    }
    out[40] = '\0';
}

bool torrent_meta_cache_has(const uint8_t info_hash[20]) {
    if (!info_hash) return false;
    char hex[41], path[320];
    info_hash_hex(info_hash, hex);
    snprintf(path, sizeof(path), "%s/%s.meta", s_meta_cache_dir, hex);
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return st.st_size > 0 && st.st_size <= META_CACHE_MAX_SIZE;
}

#define PEER_CACHE_SLOTS 16
#define PEER_CACHE_MAX   64

typedef struct {
    bool      used;
    uint8_t   info_hash[20];
    peer_addr peers[PEER_CACHE_MAX];
    int       count;
    u64       updated_tick;
} meta_peer_cache_entry;

static Mutex s_peer_cache_mtx;
static meta_peer_cache_entry s_peer_cache[PEER_CACHE_SLOTS];

void torrent_meta_cache_peers(const uint8_t info_hash[20], const peer_addr *peers, int n) {
    if (!info_hash || !peers || n <= 0) return;
    u64 now = armGetSystemTick();
    mutexLock(&s_peer_cache_mtx);
    int slot = -1;
    u64 oldest = UINT64_MAX;
    int oldest_idx = 0;
    for (int i = 0; i < PEER_CACHE_SLOTS; i++) {
        if (s_peer_cache[i].used &&
            memcmp(s_peer_cache[i].info_hash, info_hash, 20) == 0) {
            slot = i;
            break;
        }
        if (!s_peer_cache[i].used && slot < 0) slot = i;
        if (s_peer_cache[i].used && s_peer_cache[i].updated_tick < oldest) {
            oldest = s_peer_cache[i].updated_tick;
            oldest_idx = i;
        }
    }
    if (slot < 0) slot = oldest_idx;
    meta_peer_cache_entry *e = &s_peer_cache[slot];
    e->used = true;
    memcpy(e->info_hash, info_hash, 20);
    int c = n < PEER_CACHE_MAX ? n : PEER_CACHE_MAX;
    memcpy(e->peers, peers, (size_t)c * sizeof(peer_addr));
    e->count = c;
    e->updated_tick = now;
    mutexUnlock(&s_peer_cache_mtx);
}

int torrent_meta_get_cached_peers(const uint8_t info_hash[20], peer_addr *out, int max) {
    if (!info_hash || !out || max <= 0) return 0;
    int copied = 0;
    mutexLock(&s_peer_cache_mtx);
    for (int i = 0; i < PEER_CACHE_SLOTS; i++) {
        if (s_peer_cache[i].used &&
            memcmp(s_peer_cache[i].info_hash, info_hash, 20) == 0) {
            copied = s_peer_cache[i].count < max ? s_peer_cache[i].count : max;
            if (copied > 0) memcpy(out, s_peer_cache[i].peers, (size_t)copied * sizeof(peer_addr));
            break;
        }
    }
    mutexUnlock(&s_peer_cache_mtx);
    return copied;
}

static int meta_cache_save(const uint8_t info_hash[20],
                           const uint8_t *info, size_t len) {
    if (!info || len == 0 || len > META_CACHE_MAX_SIZE) return -1;
    tsnx_ensure_parent_dirs(s_meta_cache_dir);
    char hex[41], path[320], tmp[324];
    info_hash_hex(info_hash, hex);
    snprintf(path, sizeof(path), "%s/%s.meta", s_meta_cache_dir, hex);
    snprintf(tmp,  sizeof(tmp),  "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    int ok = fwrite(info, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return -1; }
    if (rename(tmp, path) != 0) {
        remove(path);
        if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    }
    tlog("metadata cached (%u bytes)", (unsigned)len);
    return 0;
}

static int meta_cache_load(const uint8_t info_hash[20],
                           uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    char hex[41], path[320];
    info_hash_hex(info_hash, hex);
    snprintf(path, sizeof(path), "%s/%s.meta", s_meta_cache_dir, hex);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > META_CACHE_MAX_SIZE) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)fsize);
    if (!buf || fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    // The cache is trusted storage-adjacent, but an info dict fetched from a
    // random peer is by definition unauthenticated: verify the SHA-1 against
    // the requested info hash and drop a mismatch instead of using it.
    uint8_t digest[20];
    mbedtls_sha1(buf, (size_t)fsize, digest);
    if (memcmp(digest, info_hash, 20) != 0) {
        free(buf);
        remove(path);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)fsize;
    return 0;
}

// Fills name/piece_len/piece_count/piece_hashes/files/total_len from an info
// dict. Shared by .torrent loading and magnet metadata. piece_hashes points
// into the node's backing buffer, which must stay alive (t->buf / t->root).
static int parse_info_fields(torrent_meta *t, be_node *info, char *err, size_t errlen) {
    be_node *name = be_dict_get(info, "name");
    if (name && name->type == BE_STR) {
        size_t n = name->str.len < sizeof(t->name) - 1 ? name->str.len : sizeof(t->name) - 1;
        memcpy(t->name, name->str.ptr, n);
        t->name[n] = '\0';
    }

    be_node *plen = be_dict_get(info, "piece length");
    if (!plen || plen->type != BE_INT || plen->i <= 0) {
        set_err(err, errlen, "missing 'piece length'");
        return -1;
    }
    // Real torrents use 16 KB..32 MB pieces. A huge value would make the
    // engine allocate absurd piece buffers.
    if (plen->i > TORRENT_MAX_PIECE_LEN) {
        set_err(err, errlen, "'piece length' too large");
        return -1;
    }
    t->piece_len = plen->i;

    be_node *pieces = be_dict_get(info, "pieces");
    if (!pieces || pieces->type != BE_STR || pieces->str.len % 20 != 0) {
        set_err(err, errlen, "invalid 'pieces'");
        return -1;
    }
    t->piece_count = pieces->str.len / 20;
    t->piece_hashes = (const uint8_t *)pieces->str.ptr;

    if (t->files) {
        free(t->files);
        t->files = NULL;
        t->file_count = 0;
        t->file_capacity = 0;
    }

    t->total_len = 0;
    int truncated = 0;
    be_node *length = be_dict_get(info, "length");
    if (length && length->type == BE_INT) {
        if (length->i <= 0) {
            set_err(err, errlen, "invalid 'length'");
            return -1;
        }
        // Single-file torrent: synthesize one file entry so callers can treat
        // single- and multi-file torrents uniformly.
        t->total_len = length->i;
        t->file_count = 1;
        t->file_capacity = 1;
        t->files = (torrent_file *)calloc(1, sizeof(torrent_file));
        if (!t->files) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
        t->files[0].length = length->i;
        t->files[0].offset = 0;
        snprintf(t->files[0].path, sizeof(t->files[0].path), "%s", t->name);
    } else {
        be_node *files = be_dict_get(info, "files");
        if (!files || files->type != BE_LIST) {
            set_err(err, errlen, "ni 'length' ni 'files'");
            return -1;
        }
        size_t initial_cap = files->list.count > 0 ? files->list.count : 16;
        if (initial_cap > MAX_TORRENT_FILES) initial_cap = MAX_TORRENT_FILES;
        t->files = (torrent_file *)calloc(initial_cap, sizeof(torrent_file));
        if (!t->files) {
            set_err(err, errlen, "out of memory");
            return -1;
        }
        t->file_capacity = (int)initial_cap;
        t->file_count = 0;

        int64_t off = 0;
        for (size_t i = 0; i < files->list.count && t->file_count < MAX_TORRENT_FILES; i++) {
            be_node *flen = be_dict_get(files->list.items[i], "length");
            if (!flen || flen->type != BE_INT) continue;
            if (flen->i < 0 || flen->i > INT64_MAX - off) {
                set_err(err, errlen, "invalid file 'length'");
                return -1;
            }

            if (t->file_count >= t->file_capacity) {
                int new_cap = t->file_capacity * 2;
                if (new_cap > MAX_TORRENT_FILES) new_cap = MAX_TORRENT_FILES;
                if (new_cap <= t->file_capacity) { truncated = 1; break; }
                torrent_file *new_files = (torrent_file *)realloc(t->files, (size_t)new_cap * sizeof(torrent_file));
                if (!new_files) { truncated = 1; break; }
                t->files = new_files;
                t->file_capacity = new_cap;
            }

            torrent_file *tf = &t->files[t->file_count++];
            tf->length = flen->i;
            tf->offset = off;
            off += flen->i;
            t->total_len += flen->i;

            // Join the path components with '/'.
            be_node *plist = be_dict_get(files->list.items[i], "path");
            size_t used = 0;
            tf->path[0] = '\0';
            if (plist && plist->type == BE_LIST) {
                for (size_t j = 0; j < plist->list.count; j++) {
                    be_node *comp = plist->list.items[j];
                    if (comp->type != BE_STR) continue;
                    int n = snprintf(tf->path + used, sizeof(tf->path) - used,
                                     "%s%.*s", j ? "/" : "",
                                     (int)comp->str.len, comp->str.ptr);
                    if (n < 0) break;
                    used += (size_t)n;
                    if (used >= sizeof(tf->path)) break;
                }
            }
        }
        if (t->file_count < (int)files->list.count && t->file_count >= MAX_TORRENT_FILES)
            truncated = 1;
    }

    // The piece table must cover exactly the payload. Otherwise piece/offset
    // math downstream (file_last_piece, last piece length) walks past the
    // per-piece arrays. A list cut at MAX_TORRENT_FILES only has to fit.
    if (t->total_len <= 0 || t->piece_count <= 0) {
        set_err(err, errlen, "empty torrent");
        return -1;
    }
    int64_t want_pieces = (t->total_len + t->piece_len - 1) / t->piece_len;
    if (truncated ? want_pieces > t->piece_count : want_pieces != t->piece_count) {
        set_err(err, errlen, "'pieces' does not match total length");
        return -1;
    }
    return 0;
}

// Build a torrent_meta from raw metadata bytes (the info dict, as fetched from
// peers for a magnet link) plus a known info hash and tracker list. Takes
// ownership of `metadata` (stored as t->buf, freed by torrent_unload).
int torrent_load_from_metadata(torrent_meta *t, uint8_t *metadata, size_t len,
                               const uint8_t info_hash[20],
                               char *const *trackers, int tracker_count,
                               char *err, size_t errlen) {
    memset(t, 0, sizeof(*t));
    t->buf = (char *)metadata;
    t->root = be_parse(t->buf, len);
    if (!t->root || t->root->type != BE_DICT) {
        torrent_unload(t);
        set_err(err, errlen, "invalid bencode metadata");
        return -1;
    }

    memcpy(t->info_hash, info_hash, 20);

    // The metadata IS the info dict, so parse fields directly from the root.
    if (parse_info_fields(t, t->root, err, errlen) != 0) {
        torrent_unload(t);
        return -1;
    }

    for (int i = 0; i < tracker_count; i++)
        add_tracker(t, trackers[i], strlen(trackers[i]));
    add_default_trackers(t);

    if (t->tracker_count == 0) {
        torrent_unload(t);
        set_err(err, errlen, "magnet without a usable tracker");
        return -1;
    }
    return 0;
}

// Progress of the magnet metadata fetch, for the UI to poll. Plain ints written
// from the loader thread and read from the UI thread: a torn read only shows a
// stale count for one frame, which is not worth a lock.
volatile int torrent_meta_peers_tried = 0;
volatile int torrent_meta_peers_total = 0;
volatile int torrent_meta_state       = META_IDLE;
volatile int torrent_meta_trackers    = 0;
volatile int torrent_meta_connected   = 0;
char torrent_meta_last_err[128]       = {0};

const char *torrent_meta_state_str(int state) {
    switch (state) {
        case META_PARSE:    return "parsing magnet";
        case META_ANNOUNCE: return "announcing to trackers";
        case META_FETCH:    return "fetching metadata (BEP 9)";
        case META_BUILD:    return "building torrent";
        case META_DONE:     return "done";
        case META_FAIL:     return "failed";
        default:            return "idle";
    }
}

int torrent_load_magnet(torrent_meta *t, const char *magnet_uri,
                        char *err, size_t errlen) {
    return torrent_load_magnet_cancel(t, magnet_uri, NULL, err, errlen);
}

int torrent_load_magnet_cancel(torrent_meta *t, const char *magnet_uri,
                               const volatile bool *cancel,
                               char *err, size_t errlen) {
    return torrent_load_magnet_peers_cancel(t, magnet_uri, NULL, 0, NULL,
                                            cancel, err, errlen);
}

int torrent_load_magnet_peers(torrent_meta *t, const char *magnet_uri,
                              peer_addr *out, int max, int *out_n,
                              char *err, size_t errlen) {
    return torrent_load_magnet_peers_cancel(t, magnet_uri, out, max, out_n,
                                            NULL, err, errlen);
}

int torrent_load_magnet_peers_cancel(torrent_meta *t, const char *magnet_uri,
                                     peer_addr *out, int max, int *out_n,
                                     const volatile bool *cancel,
                                     char *err, size_t errlen) {
    if (out_n) *out_n = 0;
    memset(t, 0, sizeof(*t));

    torrent_meta_state     = META_PARSE;
    torrent_meta_trackers  = 0;
    torrent_meta_connected = 0;
    torrent_meta_peers_tried = torrent_meta_peers_total = 0;
    torrent_meta_last_err[0] = 0;

    magnet_info m;
    if (magnet_parse(magnet_uri, &m, err, errlen) != 0) {
        torrent_meta_state = META_FAIL;
        return -1;
    }
    if (m.tracker_count == 0) {
        tlog("magnet has no tracker; will use DHT + defaults");
    }
    torrent_meta_trackers = m.tracker_count;

    // Expand t-ru.org mirror family cleanly
    {
        bool is_tru = false;
        for (int i = 0; i < m.tracker_count && !is_tru; i++)
            is_tru = strstr(m.trackers[i], "t-ru.org") != NULL;
        if (is_tru) {
            static const char *tru_mirrors[] = {
                "http://bt.t-ru.org/ann?magnet",
                "http://bt2.t-ru.org/ann?magnet",
                "http://bt3.t-ru.org/ann?magnet",
                "http://bt4.t-ru.org/ann?magnet",
            };
            for (size_t i = 0;
                 i < sizeof(tru_mirrors) / sizeof(*tru_mirrors);
                 i++) {
                bool dup = false;
                for (int j = 0; j < m.tracker_count; j++)
                    if (strcmp(m.trackers[j], tru_mirrors[i]) == 0) {
                        dup = true;
                        break;
                    }
                if (!dup && m.tracker_count < MAX_TRACKERS)
                    m.trackers[m.tracker_count++] = strdup(tru_mirrors[i]);
            }
        }
    }

    // Persistent cache: if this magnet's metadata was already fetched, load it
    // from disk instead of running a network fetch.
    {
        uint8_t *cached = NULL;
        size_t cached_len = 0;
        if (meta_cache_load(m.info_hash, &cached, &cached_len) == 0) {
            tlog("metadata loaded from cache (%u bytes)", (unsigned)cached_len);
            torrent_meta_state = META_BUILD;
            if (out && max > 0 && out_n) {
                *out_n = torrent_meta_get_cached_peers(m.info_hash, out, max);
            }
            int rc = torrent_load_from_metadata(t, cached, cached_len,
                                                m.info_hash, m.trackers,
                                                m.tracker_count, err, errlen);
            magnet_free(&m);
            torrent_meta_state = rc == 0 ? META_DONE : META_FAIL;
            return rc;
        }
    }

    // Announce stub (magnet trackers + defaults) paired with background DHT
    // inside meta_fetch_run(): peers from trackers, DHT, incoming TCP/uTP,
    // and ut_pex stream concurrently into one non-blocking poll() loop.
    torrent_meta stub;
    memset(&stub, 0, sizeof(stub));
    memcpy(stub.info_hash, m.info_hash, 20);
    for (int i = 0; i < m.tracker_count; i++) stub.trackers[i] = m.trackers[i];
    stub.tracker_count = m.tracker_count;
    int stub_owned_start = stub.tracker_count;
    add_default_trackers(&stub);

    uint8_t peer_id[20];
    torrent_peer_id(peer_id);

    peer_addr cached_peers[64];
    int cached_peer_n = torrent_meta_get_cached_peers(m.info_hash, cached_peers, 64);

    uint8_t *metadata = NULL;
    size_t meta_len = 0;
    peer_addr peers[80];
    int n = 0;

    meta_fetch_opts opts = {0};
    opts.info_hash     = m.info_hash;
    opts.peer_id       = peer_id;
    opts.announce_stub = &stub;
    opts.use_dht       = true;
    opts.seed_peers    = cached_peer_n > 0 ? cached_peers : NULL;
    opts.seed_count    = cached_peer_n;
    opts.budget_ms     = 50000;
    opts.cancel        = cancel;

    torrent_meta_state = META_FETCH;
    int mrc = meta_fetch_run(&opts, &metadata, &meta_len, peers, 80, &n, err, errlen);

    for (int i = stub_owned_start; i < stub.tracker_count; i++)
        free(stub.trackers[i]);

    if (mrc != 0 || !metadata) {
        magnet_free(&m);
        torrent_meta_state = META_FAIL;
        return -1;
    }
    torrent_meta_state = META_BUILD;

    meta_cache_save(m.info_hash, metadata, meta_len);
    if (n > 0) torrent_meta_cache_peers(m.info_hash, peers, n);

    if (out && max > 0 && out_n) {
        int c = n < max ? n : max;
        memcpy(out, peers, (size_t)c * sizeof(peers[0]));
        *out_n = c;
    }

    int rc = torrent_load_from_metadata(t, metadata, meta_len, m.info_hash,
                                        m.trackers, m.tracker_count, err, errlen);
    magnet_free(&m);
    torrent_meta_state = rc == 0 ? META_DONE : META_FAIL;
    return rc;
}

int torrent_load(torrent_meta *t, const char *path, char *err, size_t errlen) {
    memset(t, 0, sizeof(*t));

    FILE *f = fopen(path, "rb");
    if (!f) { set_err(err, errlen, "file not found"); return -1; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 16 * 1024 * 1024) {
        fclose(f);
        set_err(err, errlen, "invalid file size");
        return -1;
    }
    t->buf = malloc(fsize);
    if (!t->buf || fread(t->buf, 1, fsize, f) != (size_t)fsize) {
        fclose(f);
        torrent_unload(t);
        set_err(err, errlen, "read error");
        return -1;
    }
    fclose(f);

    t->root = be_parse(t->buf, fsize);
    if (!t->root) {
        torrent_unload(t);
        set_err(err, errlen, "invalid bencode");
        return -1;
    }

    be_node *info = be_dict_get(t->root, "info");
    if (!info || info->type != BE_DICT) {
        torrent_unload(t);
        set_err(err, errlen, "missing 'info' dict");
        return -1;
    }

    // info_hash = SHA-1 of the info dict exactly as it appears in the file
    mbedtls_sha1((const unsigned char *)info->raw, info->rawlen, t->info_hash);

    if (parse_info_fields(t, info, err, errlen) != 0) {
        torrent_unload(t);
        return -1;
    }

    // announce-list (tiers of trackers), then plain announce as fallback
    be_node *alist = be_dict_get(t->root, "announce-list");
    if (alist && alist->type == BE_LIST) {
        for (size_t i = 0; i < alist->list.count; i++) {
            be_node *tier = alist->list.items[i];
            if (tier->type != BE_LIST) continue;
            for (size_t j = 0; j < tier->list.count; j++) {
                be_node *url = tier->list.items[j];
                if (url->type == BE_STR)
                    add_tracker(t, url->str.ptr, url->str.len);
            }
        }
    }
    be_node *announce = be_dict_get(t->root, "announce");
    if (announce && announce->type == BE_STR)
        add_tracker(t, announce->str.ptr, announce->str.len);

    add_default_trackers(t);

    if (t->tracker_count == 0) {
        torrent_unload(t);
        set_err(err, errlen, "no usable tracker");
        return -1;
    }
    return 0;
}

int torrent_largest_file(const torrent_meta *t) {
    int best = -1;
    int64_t best_len = -1;
    for (int i = 0; i < t->file_count; i++) {
        if (t->files[i].length > best_len) {
            best_len = t->files[i].length;
            best = i;
        }
    }
    return best;
}

int64_t torrent_piece_len(const torrent_meta *t, int64_t index) {
    if (index == t->piece_count - 1) {
        int64_t rem = t->total_len % t->piece_len;
        if (rem) return rem;
    }
    return t->piece_len;
}

void torrent_unload(torrent_meta *t) {
    for (int i = 0; i < t->tracker_count; i++)
        free(t->trackers[i]);
    be_free(t->root);
    free(t->buf);
    if (t->files) free(t->files);
    memset(t, 0, sizeof(*t));
}

//---------------------------------------------------------------------------
// Tracker announce
//---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t len;
} membuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    membuf *m = userdata;
    size_t add = size * nmemb;
    if (m->len + add > 4 * 1024 * 1024) return 0;  // sanity cap
    char *grown = realloc(m->data, m->len + add + 1);
    if (!grown) return 0;
    m->data = grown;
    memcpy(m->data + m->len, ptr, add);
    m->len += add;
    m->data[m->len] = '\0';
    return add;
}

static void urlencode_bytes(const uint8_t *in, size_t len, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *out++ = c;
        } else {
            *out++ = '%';
            *out++ = hex[c >> 4];
            *out++ = hex[c & 0xF];
        }
    }
    *out = '\0';
}

static int parse_peers(const be_node *resp, peer_addr *peers, int max_peers) {
    be_node *plist = be_dict_get(resp, "peers");
    if (!plist) return 0;
    int count = 0;

    if (plist->type == BE_STR) {
        // Compact format: 6 bytes per peer (4 IP + 2 port, network order)
        size_t n = plist->str.len / 6;
        for (size_t i = 0; i < n && count < max_peers; i++) {
            const uint8_t *p = (const uint8_t *)plist->str.ptr + i * 6;
            memcpy(&peers[count].ip, p, 4);
            peers[count].port = (uint16_t)((p[4] << 8) | p[5]);
            if (peers[count].port) count++;
        }
    } else if (plist->type == BE_LIST) {
        // Dict format: list of {"ip": str, "port": int}
        for (size_t i = 0; i < plist->list.count && count < max_peers; i++) {
            be_node *ip = be_dict_get(plist->list.items[i], "ip");
            be_node *port = be_dict_get(plist->list.items[i], "port");
            if (!ip || ip->type != BE_STR || !port || port->type != BE_INT) continue;
            char ipstr[64];
            size_t n = ip->str.len < sizeof(ipstr) - 1 ? ip->str.len : sizeof(ipstr) - 1;
            memcpy(ipstr, ip->str.ptr, n);
            ipstr[n] = '\0';
            unsigned a, b, c, d;
            if (sscanf(ipstr, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
            uint8_t raw[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
            memcpy(&peers[count].ip, raw, 4);
            peers[count].port = (uint16_t)port->i;
            if (peers[count].port) count++;
        }
    }
    return count;
}

// Merges peers from `src` (count `sn`) into `dst` (current size *dn, capacity
// max), skipping duplicates by ip+port. Returns how many were newly added.
static int merge_peers(peer_addr *dst, int *dn, int max,
                       const peer_addr *src, int sn) {
    int added = 0;
    for (int i = 0; i < sn && *dn < max; i++) {
        bool dup = false;
        for (int j = 0; j < *dn; j++)
            if (dst[j].ip == src[i].ip && dst[j].port == src[i].port) {
                dup = true;
                break;
            }
        if (dup) continue;
        dst[(*dn)++] = src[i];
        added++;
    }
    return added;
}

// Announces to a single HTTP(S) tracker, filling `out` (up to max). Returns the
// peer count, or -1 on failure (message in err).
// curl calls this periodically during a transfer; returning non-zero aborts it.
// We use it purely to bail out promptly when teardown flips the cancel flag,
// so a slow tracker can't hold the announce join for the full CURLOPT_TIMEOUT.
static int curl_cancel_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    const volatile bool *cancel = clientp;
    return (cancel && *cancel) ? 1 : 0;
}

static bool is_local_curl_err(CURLcode rc, long os_err, int saved_errno) {
    if (rc == CURLE_OUT_OF_MEMORY || rc == CURLE_ABORTED_BY_CALLBACK) return true;
    int e = os_err ? (int)os_err : saved_errno;
    return e == ENOBUFS || e == ENOMEM || e == EAGAIN || e == EWOULDBLOCK
#ifdef __SWITCH__
        || e == 105
#endif
    ;
}

static int announce_http(const char *tracker, const char *hash_enc,
                         const char *peer_id_enc, int64_t left,
                         bool first, int port,
                         peer_addr *out, int max,
                         const volatile bool *cancel,
                         bool *transport_ok, bool *local_err,
                         char *err, size_t errlen) {
    *transport_ok = false;
    *local_err = false;
    char url[1024];
    snprintf(url, sizeof(url),
             "%s%cinfo_hash=%s&peer_id=%s&port=%d&uploaded=0&downloaded=0"
             "&left=%lld&compact=1&event=%s&numwant=%d&key=%08X",
             tracker, strchr(tracker, '?') ? '&' : '?',
             hash_enc, peer_id_enc, port, (long long)left,
             first ? "started" : "", max, (unsigned)torrent_announce_key());

    membuf resp = {0};
    CURL *curl = curl_easy_init();
    if (!curl) { *local_err = true; return -1; }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);  // bound announce/close time
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_cancel_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchTorrent/0.1");
    CURLcode rc = curl_easy_perform(curl);
    long os_err = 0;
    long http_code = 0;
    int saved_errno = errno;
    if (rc == CURLE_COULDNT_CONNECT && (!cancel || !*cancel)) {
#ifdef __SWITCH__
        svcSleepThread(50000000ULL);
#else
        usleep(50000);
#endif
        free(resp.data);
        resp.data = NULL;
        resp.len = 0;
        rc = curl_easy_perform(curl);
        saved_errno = errno;
    }
    curl_easy_getinfo(curl, CURLINFO_OS_ERRNO, &os_err);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        *local_err = is_local_curl_err(rc, os_err, saved_errno);
        snprintf(err, errlen, "http: %s", curl_easy_strerror(rc));
        free(resp.data);
        return -1;
    }
    if (http_code >= 400) {
        snprintf(err, errlen, "http status %ld", http_code);
        free(resp.data);
        return -1;
    }

    be_node *root = be_parse(resp.data, resp.len);
    if (!root) {
        set_err(err, errlen, "invalid tracker response");
        free(resp.data);
        return -1;
    }
    /* Tracker responded with valid bencoded reply: transport is healthy! */
    *transport_ok = true;

    be_node *failure = be_dict_get(root, "failure reason");
    if (failure && failure->type == BE_STR) {
        snprintf(err, errlen, "tracker: %.*s",
                 (int)(failure->str.len < 128 ? failure->str.len : 128),
                 failure->str.ptr);
        be_free(root);
        free(resp.data);
        return -1;
    }

    int count = parse_peers(root, out, max);
    be_free(root);
    free(resp.data);
    return count;
}

// One tracker announce, run on its own thread. Each job holds its own result
// buffer so no locking is needed; results are merged after all threads join.
// Each announce thread sits in a BLOCKING socket call (curl or SO_RCVTIMEO
// recvfrom) for its whole lifetime, and the Switch OS allows only 16 such
// concurrent BSD sessions per process. The metadata fetch and torrentfs's
// discovery thread can both announce at the same moment; the process-wide
// ANNOUNCE_GLOBAL_MAX (below) bounds how many sockets that takes in total.
#define AJOB_PEERS 512

/* Tracker announces: every one in flight sits in a blocking socket call.
 *
 * This used to be a global mutex around the whole announce round plus 2
 * threads per round, the remaining trackers announced one after another on
 * the caller's thread. A round over ~10 trackers (3-5 s timeouts each) held
 * the mutex for tens of seconds, and the metadata fetch of a newly added
 * torrent queued behind the discovery thread of the active download.
 *
 * Now: no round waits for another. Each round drains its tracker list with a
 * small worker pool (the caller's own thread always works, so a round always
 * progresses), and ANNOUNCE_GLOBAL_MAX caps the EXTRA worker threads across
 * all rounds in the process -- the BSD-session footprint stays bounded. */
#ifdef __SWITCH__
#define ANNOUNCE_GLOBAL_MAX   3   /* extra announce threads, whole process */
#define ANNOUNCE_PER_ROUND    3   /* workers per round, the caller included */
#else
#define ANNOUNCE_GLOBAL_MAX   8
#define ANNOUNCE_PER_ROUND    4
#endif

/* Guards s_announce_threads and the tracker cooldown table (s_tc), which the
   announce threads of every round update concurrently. Zero-initialised is a
   valid Mutex in libnx; init_* makes it ready before the first thread runs. */
static Mutex s_announce_mtx;
static int s_announce_threads = 0;   /* extra worker threads running, process-wide */

void torrent_announce_mutex_init(void) {
    mutexInit(&s_announce_mtx);
    mutexInit(&s_peer_cache_mtx);
}

#define TRACKER_COOLDOWN_MAX      64
#define TRACKER_MAX_FAILS         3
#define TRACKER_COOLDOWN_SECS     120
#define TRACKER_COOLDOWN_MAX_SECS 300

typedef struct {
    char url[128];
    int fails;
    u64 cooldown_until;
} tracker_cooldown_entry;

static tracker_cooldown_entry s_tc[TRACKER_COOLDOWN_MAX];
static int s_tc_count = 0;

static int tracker_get_cooldown_slot(const char *url) {
    for (int i = 0; i < s_tc_count; i++) {
        if (strncmp(s_tc[i].url, url, sizeof(s_tc[i].url)) == 0) return i;
    }
    if (s_tc_count < TRACKER_COOLDOWN_MAX) {
        int idx = s_tc_count++;
        snprintf(s_tc[idx].url, sizeof(s_tc[idx].url), "%s", url);
        s_tc[idx].fails = 0;
        s_tc[idx].cooldown_until = 0;
        return idx;
    }
    return -1;
}

static bool tracker_is_on_cooldown_at(const char *url, u64 now, u64 *out_until) {
    bool on = false;
    u64 until = 0;
    mutexLock(&s_announce_mtx);
    for (int i = 0; i < s_tc_count; i++) {
        if (strncmp(s_tc[i].url, url, sizeof(s_tc[i].url)) == 0) {
            on = s_tc[i].fails >= TRACKER_MAX_FAILS && now < s_tc[i].cooldown_until;
            until = s_tc[i].cooldown_until;
            break;
        }
    }
    mutexUnlock(&s_announce_mtx);
    if (out_until) *out_until = until;
    return on;
}

static void tracker_record_result(const char *url, bool transport_ok,
                                  bool local_err, u64 freq) {
    if (local_err) return;
    mutexLock(&s_announce_mtx);
    int idx = tracker_get_cooldown_slot(url);
    if (idx < 0) { mutexUnlock(&s_announce_mtx); return; }
    if (transport_ok) {
        s_tc[idx].fails = 0;
        s_tc[idx].cooldown_until = 0;
    } else {
        s_tc[idx].fails++;
        if (s_tc[idx].fails >= TRACKER_MAX_FAILS) {
            u64 secs = TRACKER_COOLDOWN_SECS;
            int extra = s_tc[idx].fails - TRACKER_MAX_FAILS;
            if (extra > 0) secs += (u64)extra * 60;
            if (secs > TRACKER_COOLDOWN_MAX_SECS) secs = TRACKER_COOLDOWN_MAX_SECS;
            s_tc[idx].cooldown_until = armGetSystemTick() + secs * freq;
        }
    }
    mutexUnlock(&s_announce_mtx);
}

void torrent_tracker_reset_cooldowns(void) {
    mutexLock(&s_announce_mtx);
    memset(s_tc, 0, sizeof(s_tc));
    s_tc_count = 0;
    mutexUnlock(&s_announce_mtx);
}

bool torrent_tracker_is_cooled_down(const char *url) {
    if (!url) return false;
    return tracker_is_on_cooldown_at(url, armGetSystemTick(), NULL);
}

void torrent_tracker_record_status(const char *url, bool transport_ok, bool local_err) {
    if (!url) return;
    tracker_record_result(url, transport_ok, local_err, armGetSystemTickFreq());
}

typedef struct {
    const char *tracker;
    const uint8_t *info_hash;
    uint8_t peer_id[20];
    char hash_enc[61];
    char peer_id_enc[61];
    int64_t left;
    bool first;          // first announce round of this torrent: event=started
    int port;            // listen/UPnP port to advertise

    peer_addr peers[AJOB_PEERS];
    int count;
    bool transport_ok;
    bool local_err;
    u64 cooldown_until;
    double secs;
    char terr[128];   // failure reason when count < 0

    torrent_peer_cb cb;   // delivered from this thread as soon as peers arrive
    void *cb_ctx;
    const volatile bool *cancel;  // teardown flag; aborts a slow announce
    bool skip;                    // on cooldown: not announced this round
} ajob;

// One announce round's work queue, drained by up to ANNOUNCE_PER_ROUND workers.
typedef struct {
    ajob *jobs;
    int n;
    Mutex lock;
    int next;
} apool;

static void announce_thread(void *arg) {
    ajob *j = arg;
    u64 freq = armGetSystemTickFreq();
    u64 t0 = armGetSystemTick();
    bool tok = false, lerr = false;

    if (strncmp(j->tracker, "udp://", 6) == 0) {
        int rc = udp_announce(j->tracker, j->info_hash, j->peer_id, j->left,
                              j->first, j->port,
                              j->peers, AJOB_PEERS, j->cancel, j->terr,
                              sizeof(j->terr));
        j->count = rc;
        tok = (rc >= 0);
        lerr = (rc == -2);
    } else {
        j->count = announce_http(j->tracker, j->hash_enc, j->peer_id_enc,
                                 j->left, j->first, j->port,
                                 j->peers, AJOB_PEERS, j->cancel,
                                 &tok, &lerr, j->terr, sizeof(j->terr));
    }
    j->transport_ok = tok;
    j->local_err = lerr;
    j->secs = (double)(armGetSystemTick() - t0) / freq;

    if (!(j->cancel && *j->cancel)) {
        tracker_record_result(j->tracker, tok, lerr, freq);
    }

    // Hand the peers over immediately so callers don't wait for slow trackers.
    if (j->count > 0 && j->cb) j->cb(j->cb_ctx, j->peers, j->count);
}

static void apool_worker(void *arg) {
    apool *p = arg;
    for (;;) {
        mutexLock(&p->lock);
        int i = p->next < p->n ? p->next++ : -1;
        mutexUnlock(&p->lock);
        if (i < 0) return;
        ajob *j = &p->jobs[i];
        if (j->skip) continue;
        if (j->cancel && *j->cancel) {
            j->count = -1;
            snprintf(j->terr, sizeof(j->terr), "cancelled");
            continue;
        }
        announce_thread(j);
    }
}

int torrent_announce_cb(const torrent_meta *t, torrent_peer_cb cb, void *ctx,
                        const volatile bool *cancel, char *err, size_t errlen) {
    char hash_enc[61], peer_id_enc[61];
    uint8_t peer_id[20];

    torrent_peer_id(peer_id);

    urlencode_bytes(t->info_hash, 20, hash_enc);
    urlencode_bytes(peer_id, 20, peer_id_enc);

    // Only the very first announce round of a torrent carries event=started;
    // later rounds omit it. Trackers (t-ru.org observed) treat a stream of
    // started-events as client abuse and cut the peer list to a single
    // address; a plain re-announce stays within the protocol.
    bool first = __atomic_fetch_add(&((torrent_meta *)t)->announce_seq, 1,
                                    __ATOMIC_RELAXED) == 0;

    ajob *jobs = calloc(t->tracker_count > 0 ? t->tracker_count : 1, sizeof(*jobs));
    if (!jobs) {
        set_err(err, errlen, "out of memory (announce)");
        return -1;
    }

    u64 now = armGetSystemTick();
    int runnable = 0;
    for (int i = 0; i < t->tracker_count; i++) {
        ajob *j = &jobs[i];
        j->tracker = t->trackers[i];
        j->info_hash = t->info_hash;
        memcpy(j->peer_id, peer_id, 20);
        memcpy(j->hash_enc, hash_enc, sizeof(hash_enc));
        memcpy(j->peer_id_enc, peer_id_enc, sizeof(peer_id_enc));
        j->left = t->total_len;
        j->first = first;
        j->port = s_announce_port;
        j->cb = cb;
        j->cb_ctx = ctx;
        j->cancel = cancel;
        if (tracker_is_on_cooldown_at(j->tracker, now, &j->cooldown_until)) {
            j->skip = true;
            j->count = -1;
            snprintf(j->terr, sizeof(j->terr), "cooldown");
        } else {
            runnable++;
        }
    }

    // Total-blackout guard: if every tracker is cooled down (e.g. after a
    // brief offline blip), allow the earliest-expiring ones to try anyway.
    if (t->tracker_count > 0 && runnable == 0) {
        int want_unskip = t->tracker_count < ANNOUNCE_PER_ROUND
                              ? t->tracker_count : ANNOUNCE_PER_ROUND;
        for (int k = 0; k < want_unskip; k++) {
            int best = -1;
            for (int i = 0; i < t->tracker_count; i++) {
                if (!jobs[i].skip) continue;
                if (best < 0 || jobs[i].cooldown_until < jobs[best].cooldown_until) {
                    best = i;
                }
            }
            if (best < 0) break;
            jobs[best].skip = false;
            jobs[best].count = 0;
            jobs[best].terr[0] = '\0';
            runnable++;
        }
    }

    // Peers reach the callback the moment each tracker answers, so the
    // fastest tracker unblocks downloading; the round returns once all have.
    apool pool = { jobs, t->tracker_count };
    mutexInit(&pool.lock);
    int want = runnable < ANNOUNCE_PER_ROUND ? runnable : ANNOUNCE_PER_ROUND;
    int extra = want > 1 ? want - 1 : 0;   // the caller is worker #1
    mutexLock(&s_announce_mtx);
    if (extra > ANNOUNCE_GLOBAL_MAX - s_announce_threads)
        extra = ANNOUNCE_GLOBAL_MAX - s_announce_threads;
    if (extra < 0) extra = 0;
    s_announce_threads += extra;
    mutexUnlock(&s_announce_mtx);

    Thread workers[ANNOUNCE_PER_ROUND];
    int started = 0;
    for (int k = 0; k < extra; k++) {
        if (threadCreate(&workers[started], apool_worker, &pool, NULL, 0x8000,
                         TSNX_THREAD_ARGS(TSNX_CORE_NET, TSNX_PRIO_NET_BG)) != 0)
            break;
        if (threadStart(&workers[started]) != 0) {
            threadClose(&workers[started]);
            break;
        }
        started++;
    }
    if (started < extra) {   // give back the budget we could not use
        mutexLock(&s_announce_mtx);
        s_announce_threads -= extra - started;
        mutexUnlock(&s_announce_mtx);
    }

    apool_worker(&pool);   // the caller drains the queue too
    for (int k = 0; k < started; k++) {
        threadWaitForExit(&workers[k]);
        threadClose(&workers[k]);
    }
    if (started > 0) {
        mutexLock(&s_announce_mtx);
        s_announce_threads -= started;
        mutexUnlock(&s_announce_mtx);
    }

    int answered = 0;
    for (int i = 0; i < t->tracker_count; i++) {
        ajob *j = &jobs[i];
        if (j->count > 0) {
            answered++;
            tlog("tracker %.1fs (%d) %.60s", j->secs, j->count, j->tracker);
        } else if (j->count == 0 && !j->skip) {
            answered++;
            tlog("tracker %.1fs (0 peers) %.60s", j->secs, j->tracker);
        } else if (!j->skip) {
            tlog("tracker %.1fs (failed: %s) %.60s", j->secs,
                 j->terr[0] ? j->terr : "timeout", j->tracker);
        }
    }

    free(jobs);
    return answered;
}

// Collector so the array-based torrent_announce reuses the callback machinery.
typedef struct {
    peer_addr *peers;
    int total;
    int max;
    Mutex lock;
} collector;

static void collect_cb(void *ctx, const peer_addr *peers, int n) {
    collector *c = ctx;
    mutexLock(&c->lock);
    merge_peers(c->peers, &c->total, c->max, peers, n);
    mutexUnlock(&c->lock);
}

int torrent_announce(const torrent_meta *t, peer_addr *peers, int max_peers,
                     const volatile bool *cancel, char *err, size_t errlen) {
    collector c = { peers, 0, max_peers };
    mutexInit(&c.lock);
    torrent_announce_cb(t, collect_cb, &c, cancel, err, errlen);
    if (c.total == 0) { set_err(err, errlen, "no peer from the trackers"); return -1; }
    return c.total;
}
