#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "torrentfs.h"
#include "torrent_meta.h"
#include "dhtclient.h"
#include "utp_nb.h"
#include "engine_log.h"

#define MAX_TORRENTS 8

struct tsnx_torrent {
    char            hash[TSNX_MAX_HASH_LEN + 1];
    char           *source;     /* magnet URI or .torrent path (owned) */
    torrentfs      *fs;
    torrent_meta    meta;
    int             file_index;
    bool            used;
    bool            paused;
    bool            wanted_files[TSNX_MAX_FILES];
    const volatile bool *cancel; /* polled by (re)opens of this torrent */
    uint64_t        bytes_recv_at_start;
    uint64_t        last_bytes_recv;
    uint64_t        last_speed_time_ms;
    float           download_kbps;
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

tsnx_engine *tsnx_engine_start(int listen_port) {
    tsnx_engine *eng = calloc(1, sizeof(tsnx_engine));
    if (!eng) return NULL;
    eng->port = listen_port;
    eng->running = true;
    engine_log_init("sdmc:/switch/TorrentShopNX/engine.log");
    engine_log(ENGINE_LOG_INFO, "engine start port=%d", listen_port);
    dht_set_log(dht_log_cb);
    torrent_set_log(torrent_log_cb);
    torrentfs_set_ram_stream(1);
    utp_nb_init();
    g_engine = eng;
    return eng;
}

void tsnx_engine_stop(tsnx_engine *eng) {
    int i;
    eng = active_engine(eng);
    if (!eng) return;
    for (i = 0; i < MAX_TORRENTS; i++) {
        if (eng->torrents[i].used) {
            torrentfs_cancel(eng->torrents[i].fs);
            torrentfs_close(eng->torrents[i].fs);
            torrent_unload(&eng->torrents[i].meta);
            free(eng->torrents[i].source);
        }
    }
    eng->running = false;
    if (g_engine == eng) g_engine = NULL;
    engine_log(ENGINE_LOG_INFO, "engine stop");
    dht_stop();  // persistent DHT: save the routing table for the next start
    utp_nb_exit();
    engine_log_close();
    free(eng);
}

bool tsnx_engine_running(const tsnx_engine *eng) {
    eng = active_engine((tsnx_engine *)eng);
    return eng && eng->running;
}

static tsnx_torrent *find_slot(tsnx_engine *eng) {
    int i;
    eng = active_engine(eng);
    if (!eng) return NULL;
    for (i = 0; i < MAX_TORRENTS; i++)
        if (!eng->torrents[i].used) return &eng->torrents[i];
    return NULL;
}

static tsnx_torrent *find_by_hash(tsnx_engine *eng, const char *hash) {
    int i;
    eng = active_engine(eng);
    if (!eng || !hash) return NULL;
    for (i = 0; i < MAX_TORRENTS; i++)
        if (eng->torrents[i].used && strcmp(eng->torrents[i].hash, hash) == 0)
            return &eng->torrents[i];
    return NULL;
}

bool tsnx_engine_add_torrent_file(tsnx_engine *eng, const char *path,
                                  char *out_hash, size_t out_hash_len) {
    tsnx_torrent *slot;
    char err[256] = {0};
    eng = active_engine(eng);
    if (!eng || !path || !out_hash || out_hash_len < TSNX_MAX_HASH_LEN + 1)
        return false;
    slot = find_slot(eng);
    if (!slot) return false;
    if (torrent_load(&slot->meta, path, err, sizeof(err)) != 0)
        return false;
    bin_to_hex(slot->meta.info_hash, slot->hash, 40);
    free(slot->source);
    slot->source = strdup(path);
    slot->fs = torrentfs_open_file(path, "sdmc:/switch/TorrentShopNX/cache.bin",
                                   -1, err, sizeof(err));
    if (!slot->fs) {
        torrent_unload(&slot->meta);
        return false;
    }
    slot->used = true;
    slot->file_index = -1;
    slot->cancel = NULL;
    for (int i = 0; i < TSNX_MAX_FILES; i++) slot->wanted_files[i] = true;
    slot->bytes_recv_at_start = 0;
    slot->last_bytes_recv = 0;
    slot->last_speed_time_ms = 0;
    slot->download_kbps = 0.0f;
    snprintf(out_hash, out_hash_len, "%s", slot->hash);
    fprintf(stderr, "[engine] add_torrent_file ok hash=%s\n", slot->hash);
    return true;
}

bool tsnx_engine_add_magnet(tsnx_engine *eng, const char *magnet_uri,
                            char *out_hash, size_t out_hash_len) {
    return tsnx_engine_add_magnet_ex(eng, magnet_uri, -1, false, NULL,
                                     out_hash, out_hash_len);
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
    tsnx_torrent *slot;
    char err[256] = {0};
    eng = active_engine(eng);
    if (!eng || !magnet_uri || !out_hash || out_hash_len < TSNX_MAX_HASH_LEN + 1)
        return false;
    slot = find_slot(eng);
    if (!slot) return false;
    if (torrent_load_magnet_cancel(&slot->meta, magnet_uri, cancel,
                                   err, sizeof(err)) != 0)
        return false;
    bin_to_hex(slot->meta.info_hash, slot->hash, 40);
    free(slot->source);
    slot->source = strdup(magnet_uri);
    slot->cancel = cancel;
    slot->file_index = file_index;
    if (!meta_only) {
        slot->fs = torrentfs_open_file_cancel(magnet_uri,
                                              "sdmc:/switch/TorrentShopNX/cache.bin",
                                              file_index, cancel, err, sizeof(err));
        if (!slot->fs) {
            torrent_unload(&slot->meta);
            free(slot->source);
            slot->source = NULL;
            slot->cancel = NULL;
            return false;
        }
    }
    slot->used = true;
    for (int i = 0; i < TSNX_MAX_FILES; i++) slot->wanted_files[i] = true;
    slot->bytes_recv_at_start = 0;
    slot->last_bytes_recv = 0;
    slot->last_speed_time_ms = 0;
    slot->download_kbps = 0.0f;
    snprintf(out_hash, out_hash_len, "%s", slot->hash);
    return true;
}

bool tsnx_engine_has_torrent(tsnx_engine *eng, const char *hash) {
    return find_by_hash(eng, hash) != NULL;
}

/* Update the cancel flag a torrent's (re)opens poll. The download backend
 * takes ownership of the slot the probe kept, so the slot must stop polling
 * the probe's flag and follow the download's instead. */
void tsnx_engine_set_cancel(tsnx_engine *eng, const char *hash,
                            const volatile bool *flag) {
    tsnx_torrent *t = find_by_hash(eng, hash);
    if (t) t->cancel = flag;
}

bool tsnx_engine_remove_torrent(tsnx_engine *eng, const char *hash) {
    tsnx_torrent *t = find_by_hash(eng, hash);
    if (!t) return false;
    eng = active_engine(eng);
    (void)eng;
    torrentfs_cancel(t->fs);
    torrentfs_close(t->fs);
    torrent_unload(&t->meta);
    free(t->source);
    memset(t, 0, sizeof(*t));
    return true;
}

bool tsnx_engine_pause_torrent(tsnx_engine *eng, const char *hash) {
    tsnx_torrent *t = find_by_hash(eng, hash);
    if (!t) return false;
    t->paused = true;
    if (t->fs) torrentfs_pause(t->fs, 1);
    engine_log(ENGINE_LOG_INFO, "[engine] pause torrent %s", hash);
    return true;
}

bool tsnx_engine_resume_torrent(tsnx_engine *eng, const char *hash) {
    tsnx_torrent *t = find_by_hash(eng, hash);
    if (!t) return false;
    t->paused = false;
    if (t->fs) torrentfs_pause(t->fs, 0);
    engine_log(ENGINE_LOG_INFO, "[engine] resume torrent %s", hash);
    return true;
}

int tsnx_engine_get_torrents(tsnx_engine *eng, tsnx_torrent_item *out,
                             int max_items) {
    int count = 0;
    int i;
    eng = active_engine(eng);
    if (!eng || !out || max_items <= 0) return 0;
    for (i = 0; i < MAX_TORRENTS && count < max_items; i++) {
        tsnx_torrent *t;
        tsnx_torrent_item *it;
        int64_t done, total, ph;
        uint64_t now;
        uint64_t bytes_recv;
        if (!eng->torrents[i].used) continue;
        t = &eng->torrents[i];
        it = &out[count++];
        snprintf(it->hash, sizeof(it->hash), "%s", t->hash);
        // Metadata-only slots (probe result) have no torrentfs yet.
        if (t->fs) {
            snprintf(it->name, sizeof(it->name), "%s", torrentfs_name(t->fs));
            torrentfs_stats(t->fs, &done, &total, &ph);
            it->progress = total > 0 ? (float)done / (float)total : 0.0f;
            it->loaded_size = done;
            it->total_size = total;

            int live = 0, peak = 0, connecting = 0;
            torrentfs_live_peers(t->fs, &live, &peak, &connecting);
            it->seeds       = torrentfs_seed_count(t->fs); // peers holding the whole file
            it->peers       = live;                        // currently connected sessions
            it->known_peers = torrentfs_peer_count(t->fs); // discovered pool

            bytes_recv = (uint64_t)torrentfs_bytes_recv(t->fs);
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
        now = now_ms();
        if (t->last_speed_time_ms == 0) {
            t->last_speed_time_ms = now;
            t->last_bytes_recv = bytes_recv;
            t->download_kbps = 0.0f;
        } else {
            uint64_t dt = now - t->last_speed_time_ms;
            if (dt >= 500) {
                uint64_t db = bytes_recv - t->last_bytes_recv;
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
                t->last_bytes_recv = bytes_recv;
            }
        }
        it->download_kbps = t->download_kbps;
    }
    return count;
}

int tsnx_engine_get_files(tsnx_engine *eng, const char *hash,
                          tsnx_file_info *out, int max_files) {
    tsnx_torrent *t;
    int count = 0;
    int i;
    eng = active_engine(eng);
    if (!eng || !out || max_files <= 0) return 0;
    t = find_by_hash(eng, hash);
    if (!t) return 0;
    for (i = 0; i < t->meta.file_count && count < max_files; i++) {
        tsnx_file_info *f = &out[count++];
        f->index = i;
        snprintf(f->path, sizeof(f->path), "%s", t->meta.files[i].path);
        f->size = t->meta.files[i].length;
        f->offset = t->meta.files[i].offset;
        f->wanted = true;
    }
    return count;
}

bool tsnx_engine_set_file_wanted(tsnx_engine *eng, const char *hash,
                                 int file_index, bool wanted) {
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    t = find_by_hash(eng, hash);
    if (!t) return false;
    if (file_index < 0 || file_index >= TSNX_MAX_FILES) return false;
    t->wanted_files[file_index] = wanted;
    return true;
}

bool tsnx_engine_prepare_stream(tsnx_engine *eng, const char *hash,
                                int file_index) {
    tsnx_torrent *t;
    char err[256] = {0};
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    t = find_by_hash(eng, hash);
    if (!t) return false;
    if (file_index < 0 || file_index >= t->meta.file_count) return false;
    if (t->file_index == file_index && t->fs) return true;
    /* Close previous stream and reopen the requested file. */
    if (t->fs) torrentfs_close(t->fs);
    t->fs = torrentfs_open_file_cancel(t->source ? t->source : t->hash,
                                       "sdmc:/switch/TorrentShopNX/cache.bin",
                                       file_index, t->cancel, err, sizeof(err));
    if (!t->fs) return false;
    if (t->paused) torrentfs_pause(t->fs, 1);
    t->file_index = file_index;
    return true;
}

int64_t tsnx_engine_read(tsnx_engine *eng, const char *hash,
                         int64_t offset, void *buf, int64_t size) {
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash || !buf || size <= 0) return -1;
    t = find_by_hash(eng, hash);
    if (!t || !t->fs) return -1;
    return torrentfs_read(t->fs, offset, (char *)buf, size);
}

void tsnx_engine_cancel_read(tsnx_engine *eng, const char *hash) {
    eng = active_engine(eng);
    (void)eng;
    tsnx_torrent *t = find_by_hash(eng, hash);
    if (t && t->fs) torrentfs_cancel(t->fs);
}

void tsnx_engine_set_min_keep_offset(tsnx_engine *eng, const char *hash,
                                     int64_t offset) {
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash) return;
    t = find_by_hash(eng, hash);
    if (t && t->fs) {
        /* Advance the playhead so the RAM window follows the installer. */
        torrentfs_set_playhead(t->fs, offset);
    }
}

int tsnx_engine_piece_size(tsnx_engine *eng, const char *hash) {
    tsnx_torrent *t = find_by_hash(eng, hash);
    if (!t || !t->fs) return 0;
    return (int)torrentfs_piece_len(t->fs);
}

int64_t tsnx_engine_file_offset(tsnx_engine *eng, const char *hash) {
    tsnx_torrent *t = find_by_hash(eng, hash);
    eng = active_engine(eng);
    (void)eng;
    if (!t || t->file_index < 0) return 0;
    return t->meta.files[t->file_index].offset;
}

int tsnx_engine_get_peers(tsnx_engine *eng, const char *hash,
                          tsnx_peer_info *out, int max_peers) {
    tsnx_torrent *t;
    if (!out || max_peers <= 0) return 0;
    eng = active_engine(eng);
    if (!eng || !hash) return 0;
    t = find_by_hash(eng, hash);
    if (!t || !t->fs) return 0;
    torrentfs_peer_info pinfos[64];
    int n = torrentfs_get_peers(t->fs, pinfos,
                                max_peers < 64 ? max_peers : 64);
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
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    t = find_by_hash(eng, hash);
    if (!t || !t->fs) return false;
    return torrentfs_set_piece_zone(t->fs, first_piece, piece_count, (int)zone);
}

bool tsnx_engine_clear_piece_zones(tsnx_engine *eng, const char *hash) {
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    t = find_by_hash(eng, hash);
    if (!t || !t->fs) return false;
    return torrentfs_clear_piece_zones(t->fs);
}

void tsnx_engine_set_backlog_ms(tsnx_engine *eng, const char *hash, int ms) {
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash) return;
    t = find_by_hash(eng, hash);
    if (t && t->fs) torrentfs_set_backlog(t->fs, ms);
}

void tsnx_engine_set_governor(tsnx_engine *eng, int on) {
    (void)eng;
    torrentfs_set_governor(on);
}

void tsnx_engine_set_ram_stream(tsnx_engine *eng, int on) {
    (void)eng;
    torrentfs_set_ram_stream(on);
}

bool tsnx_engine_announce_now(tsnx_engine *eng, const char *hash) {
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash) return false;
    t = find_by_hash(eng, hash);
    if (!t || !t->fs) return false;
    torrentfs_announce_now(t->fs);
    return true;
}

bool tsnx_engine_get_diag(tsnx_engine *eng, const char *hash, tsnx_engine_diag *out) {
    tsnx_torrent *t;
    eng = active_engine(eng);
    if (!eng || !hash || !out) return false;
    t = find_by_hash(eng, hash);
    if (!t || !t->fs) return false;

    memset(out, 0, sizeof(*out));

    tsnx_torrent_item items[8];
    int n = tsnx_engine_get_torrents(eng, items, 8);
    for (int i = 0; i < n; i++) {
        if (strcmp(items[i].hash, hash) == 0) {
            out->peers = items[i].known_peers;
            out->download_kbps = items[i].download_kbps;
            break;
        }
    }

    int live = 0, peak = 0, connecting = 0;
    torrentfs_live_peers(t->fs, &live, &peak, &connecting);
    out->live = live;
    out->peak = peak;
    out->connecting = connecting;

    int claiming = 0, idle = 0;
    torrentfs_claim_stats(t->fs, &claiming, &idle);
    out->claiming = claiming;
    out->idle = idle;

    int empty = 0, ok = 0, bad = 0;
    torrentfs_bitfield_stats(t->fs, &empty, &ok, &bad);
    out->empty_bitfield = empty;
    out->good_bitfield = ok;
    out->bad_bitfield = bad;

    torrentfs_fail_kinds(t->fs, &out->sock_fail, &out->timeouts);
    out->calm = torrentfs_calm(t->fs);
    out->bytes_recv = torrentfs_bytes_recv(t->fs);
    out->dup_bytes  = torrentfs_dup_bytes(t->fs);

    int64_t done = 0, total = 0, ph = 0;
    torrentfs_stats(t->fs, &done, &total, &ph);
    out->pieces_done = done;
    out->pieces_total = total;
    out->playhead_piece = ph;

    int status = 0, have = 0, req = 0, tot = 0;
    torrentfs_piece_debug(t->fs, ph, &status, &have, &req, &tot);
    out->piece_status = status;
    out->piece_have = have;
    out->piece_req = req;
    out->piece_total = tot;

    dhtclient_get_last_lookup(&out->dht_peers, &out->dht_good, &out->dht_dubious);
    return true;
}
