// torrentfs3.c — v3 of the streaming torrent engine (same public API as v1/v2).
//
// A deliberate simplification of v2. What v2 bought with its complexity
// (global block scheduler, adaptive pipelines, upload serving, incoming
// acceptors, stalled-piece racing) was throughput headroom the player rarely
// needs: the stream plays at a few MB/s and the platform costs (bsd IPC per
// packet, SD latency, three CPU cores shared with mpv) dominate long before
// the scheduler does. v3 keeps the parts that were hard-won on this platform
// and deletes the rest:
//
//   KEPT (load-bearing, see v1/v2 history):
//   - one poll() netloop (libnx caps concurrent *blocking* BSD calls at 16)
//   - RAM piece assembly, SHA-1 from RAM, ONE sequential SD write per piece
//   - the FAT32-chunked, append-only cache (sparse writes freeze the console)
//   - raw fds, sliced writes under cache_lock (SD GC stalls must not block
//     the player's reads)
//   - lock-free playhead/have_piece on the mpv demuxer thread (an mpv thread
//     holding an engine lock while starved froze the netloop with it)
//   - bounded streaming window + startup head/tail criticals (moov atom)
//   - per-peer reconnect backoff, 130 s idle patience (> the 120 s keep-alive
//     interval: reaping sooner re-dials choking peers forever)
//   - backlog-driven calm mode + fill-rate governor (wifi bursts on the OS
//     core are what freeze the console)
//
//   DROPPED:
//   - upload serving, incoming acceptors, the listen socket (leech-only;
//     removes 2 threads, the INQ, tit-for-tat state)
//   - the global block scheduler (a session claims ONE piece at a time and
//     pipelines blocks within it; a stalled claim is parked -- buffer and
//     progress kept -- and adopted by the next session that has the piece)
//   - adaptive pipeline depth (fixed 48 blocks ~= 768 KB in flight per peer)
//   - separate announce and DHT threads (one discovery thread runs both)
//
// Session framing/handshake lives in peer.c's peer_nb layer instead of being
// duplicated here.

#include "torrentfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <switch.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <mbedtls/sha1.h>

#include "torrent_meta.h"
#include "bt_peer.h"      // MSG_* ids, BLOCK_LEN, bf_has_piece, peer_nb
#include "dhtclient.h"
#include "utp_nb.h"
#include "engine_log.h"

//-----------------------------------------------------------------------------
// Tuning
//-----------------------------------------------------------------------------

#ifdef __SWITCH__
/* Live sessions. TCP sockets are non-blocking and don't touch the 16-session
   blocking-BSD budget; memory is ~40 KB per TCP session plus ~1.25 MB per
   uTP session (about every 4th dial), ~12 MB worst case -- fine in title
   mode. 40 sessions with an 8-slot churn reserve. */
#define MAX_SESS         40
#define MAX_CONNECTING   16     // slots allowed to sit in a pending connect
#define DIAL_STOP_LIVE   32     // enough live sessions: stop dialing new peers
#else
// PC can hold more concurrent BSD sockets; use them to stress-test the engine.
#define MAX_SESS         64
#define MAX_CONNECTING   32
#define DIAL_STOP_LIVE   48
#endif
#define CONNECT_SECS     2      // outbound SYN patience
#define PREHS_SECS       10     // connected but no handshake yet
#define IDLE_SECS        130    // handshaked, nothing received (see header)
#define KEEPALIVE_SECS   60
#define STALL_SECS       6      // requested blocks but nothing came: drop peer
// A peer can dodge STALL_SECS forever by dribbling one block every few seconds:
// last_block keeps resetting while the piece it holds never lands. Rather than
// ban it, we meter every session's receive rate (per session, so it is piece-
// size independent) and steer the fastest peer onto the most urgent piece --
// demoting a slow owner to a less urgent one instead of killing it. The takeover
// only fires on a clear rate win, so a uniformly slow swarm is never thrashed.
#define SLOW_MEASURE_SECS 5          // download time before a session's rate counts
#define SLOW_FACTOR_CRIT  4          // take over only if this much faster than the owner

#define DEPTH            48     // default request pipeline, in blocks (~768 KB)

// RAM for in-flight piece buffers; bounds how many pieces are open at once. The
// budget is in BYTES: the floor is only there to keep a tiny bit of pipelining,
// NOT to force a piece count -- a 64 MB-piece torrent clamped to 4 buffers held
// 256 MB of them (and 8 look-ahead pieces = 512 MB, more than the RAM window,
// which then evicted what it had just fetched ahead and re-downloaded it).
#define RAM_BUDGET       (48LL << 20)
#define AQ_MAX           16
#define AQ_MIN           2

// Streaming window: wide enough to feed the sessions, narrow enough that they
// don't advance 50 pieces in parallel while the player starves on one. Byte-
// budgeted, with a small piece floor and (in RAM mode) a ceiling of half the
// RAM window so the look-ahead can never outrun what the window can hold.
#define STREAM_WINDOW    (32LL * 1024 * 1024)
#define STREAM_MIN_PIECES 2

// Startup criticals: mpv probes the container head and tail (moov atom) first,
// so we speculatively pre-fetch both ends in parallel rather than let mpv's
// blocking reads discover them one seek at a time. Budgeted in BYTES, not
// pieces: it is a latency hedge over the region mpv is about to read, so with a
// 64 MB piece one piece already covers it -- pre-fetching a fixed 3 tail pieces
// there was 192 MB of speculation. The piece count is ceil(budget / piece_len),
// clamped so tiny pieces do not explode and every torrent still probes >=1 each.
#define CRIT_HEAD_BYTES  (1LL << 20)   // ~1 MB at the head (ftyp / start)
#define CRIT_TAIL_BYTES  (4LL << 20)   // ~4 MB at the tail (where moov usually is)
#define CRIT_HEAD_MAX    2
#define CRIT_TAIL_MAX    3

// FAT32 caps a file at 4 GB: the cache is chunked.
#define CACHE_CHUNK      (1LL << 30)
#define CACHE_MAX_CHUNKS 64
// Piece writes release cache_lock between slices of this size, so a slow SD
// write never makes the player's reads wait out the whole piece.
#define WR_SLICE         (256 * 1024)

// RAM streaming mode: verified pieces are kept in a bounded RAM window instead
// of being written to the SD card. Completing a piece then never bursts the
// FAT filesystem service on the OS core -- the SD write is what stutters
// playback at each piece boundary, the more so the bigger the piece. Once the
// resident bytes pass RAM_STREAM_BUDGET the pieces furthest behind the playhead
// are dropped; a read that lands on a dropped piece re-downloads it (the read
// moves the playhead there, so the streaming window covers it). Sized to leave
// the forward window plenty of seek-back slack; needs a full-RAM launch.
#define RAM_STREAM_BUDGET (256LL << 20)

#define TFS_MAX_PEERS    512
#define BACKOFF_CONN_SECS 5
#define BACKOFF_MAX_SECS  120
#define BACKOFF_DROP_SECS 5

// Discovery re-runs every 15 min, or 60 s after the last round when starved.
#define DISC_INTERVAL_SECS (15 * 60)
#define DISC_STARVED_SECS  15
#define STARVED_LIVE       3

enum { PIECE_NEEDED = 0, PIECE_ACTIVE = 1, PIECE_DONE = 2, PIECE_WRITING = 3 };

static volatile int g_governor = 1;
void torrentfs_set_governor(int on) { g_governor = on ? 1 : 0; }
int  torrentfs_governor(void)       { return g_governor; }

// Read once per torrent at open time (like the peer id); a live torrent keeps
// whatever mode it opened with. Set it before torrentfs_open.
static volatile int g_ram_stream = 0;
void torrentfs_set_ram_stream(int on) { g_ram_stream = on ? 1 : 0; }
int  torrentfs_ram_stream(void)       { return g_ram_stream; }

//-----------------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------------

// One piece being assembled in RAM. idx -1 = slot free. owner is the session
// currently requesting its blocks (-1 = parked: progress kept, waiting to be
// adopted). Only the netloop touches entries, except the writer returning a
// buffer under t->lock.
typedef struct {
    int64_t idx;
    int owner;          // session that allocated the slot, or -1 (parked)
    int workers;        // how many sessions currently hold this piece claim
    uint8_t *buf;       // piece_len bytes, lazily allocated, reused
    uint8_t *have;      // one byte per block: data present in buf
    uint8_t *req;       // one byte per block: requested by someone
    int nblocks;
    int have_cnt;
    int next_req;       // scan cursor for the next block to request
} aq_entry;

typedef struct {
    bool active;
    bool connecting;
    peer_nb nb;         // sock/framing/handshake/bitfield (peer.c)
    peer_addr addr;     // remote endpoint
    int pidx;           // peer pool index (for backoff bookkeeping)
    int64_t claim;      // piece being fetched, -1 = none
    int out_n;          // block requests outstanding
    u64 started, last_rx, last_block, last_ka;
    // Per-session receive-rate meter, for the relative slow-peer cull.
    int64_t rx_total;   // cumulative bytes received (never reset)
    int64_t rate_prev;  // rx_total at the last rate sample
    u64 rate_tick;      // last rate sample (0 = meter not started yet)
    u64 rate_start;     // first sample: start of the measure grace
    double rate;        // smoothed receive rate, bytes/s
} sess;

static void release_claim(torrentfs *t, sess *s);

typedef struct {
    int64_t idx;
    int64_t plen;
    uint8_t *buf;
} wjob;

struct torrentfs {
    torrent_meta meta;

    int64_t stream_offset, stream_size;
    int64_t file_first_piece, file_last_piece;
    int blocks_per_piece;      // of a full piece
    int crit_head, crit_tail;  // startup pieces to pre-fetch at each end

    // FAT32-chunked cache (see v2 for the append-only rationale).
    int chunks[CACHE_MAX_CHUNKS];
    char cache_base[192];
    Mutex cache_lock;
    int32_t *piece_slot;       // piece index -> cache slot, -1 = not stored
    int64_t next_slot;
    int64_t slots_per_chunk;

    // RAM streaming window (ram_mode). ram_piece[idx] is the resident buffer for
    // a verified piece, or NULL. Both the writer (store/evict) and the reader
    // touch it under cache_lock. ram_lo is a scan cursor for the eviction sweep.
    bool ram_mode;
    uint8_t **ram_piece;
    int64_t ram_budget;
    int64_t ram_resident;
    int64_t ram_peak;
    int64_t ram_lo;

    // t->lock guards: status[] transitions shared with the writer, the writer
// queue, buffer returns, the peer pool (shared with discovery), counters
// the UI reads. Session/entry state is netloop-private and needs no lock.
    Mutex lock;
    uint8_t *status;
    uint8_t *piece_zone;       // scheduler-assigned zone per piece (0 = none)
    int64_t pieces_done;       // currently resident (RAM window) / on disk (SD)
    int64_t pieces_ever;       // distinct pieces ever completed: monotonic, for UI
    uint8_t *ever;             // one bit per piece, already counted in pieces_ever
    int64_t playhead_piece;    // lock-free (aligned 64-bit store, see below)
    volatile bool stop;

    aq_entry aq[AQ_MAX];
    int n_aq;

    wjob wq[AQ_MAX + 2];
    int wq_head, wq_n;

    sess S[MAX_SESS];

    // Peer pool + backoff.
    peer_addr peers[TFS_MAX_PEERS];
    uint8_t peer_fails[TFS_MAX_PEERS];
    uint8_t peer_busy[TFS_MAX_PEERS];
    u64 peer_next_try[TFS_MAX_PEERS];
    int peer_count;
    int next_peer;

    uint8_t peer_id[20];

    Thread netloop, writer, discovery;
    bool netloop_started, writer_started, discovery_started;

    u64 freq;

    // Calm / governor.
    int backlog_ms;            // written by the app thread, read racily
    int calm_now;
    volatile bool announce_now;
    volatile int paused;       // pause: stop dialing + claiming (sessions stay)

    // Transport alternation: 0 = try TCP next, 1 = try µTP next.
    int dial_toggle;
    double rate_bps;           // netloop-only EWMA
    int64_t rate_last_bytes;
    u64 rate_last_tick;
    u64 last_progress_tick;    // last time a block arrived or a piece finished

    // Debug surface (racy reads by the UI are fine; single-writer counters).
    int st_conn_ok, st_conn_fail, st_sock_fail, st_conn_timeout;
    int st_unchoke_ok, st_choked;
    int st_piece_ok, st_fetch_fail, st_sha_fail;
    int st_interested_recv, st_request_recv;
    int st_bf_empty, st_bf_ok, st_bf_bad;
    int st_live, st_peak_live, st_connecting;
    int st_claim_ok, st_claim_fail;
    int st_cache_wr_fail, st_cache_rd_short;
    int64_t st_cache_written;
    int64_t st_bytes_recv;
    int64_t st_blocks_have;    // blocks resident in RAM partials
    int64_t st_win_ph, st_win_lo, st_win_hi;
    char st_last_err[128];

    u64 hb_tick[4];
    u8  hb_core[4];
    uint32_t lat_n[5];
    uint64_t lat_max[5];
};

enum { LAT_POLL = 0, LAT_RECV, LAT_SEND, LAT_WR, LAT_RD };
enum { HB_NET = 0, HB_WRITER = 1, HB_READER = 2, HB_UI = 3 };

static void hb_beat(torrentfs *t, int k) {
    t->hb_tick[k] = armGetSystemTick();
    t->hb_core[k] = (u8)svcGetCurrentProcessorNumber();
}

static void lat_add(torrentfs *t, int c, u64 t0) {
    u64 d = armGetSystemTick() - t0;
    t->lat_n[c]++;
    if (d > t->lat_max[c]) t->lat_max[c] = d;
}

static uint32_t block_len_of(int64_t plen, int b) {
    int64_t rem = plen - (int64_t)b * BLOCK_LEN;
    return rem > BLOCK_LEN ? BLOCK_LEN : (uint32_t)rem;
}

static void set_err(torrentfs *t, const char *fmt, long long a) {
    snprintf(t->st_last_err, sizeof(t->st_last_err), fmt, a);
}

//-----------------------------------------------------------------------------
// Chunked cache I/O (ported from v2 -- append-only on purpose, FAT has no
// sparse files: a write past EOF journals every cluster in between on the OS
// core while we hold cache_lock. Raw fds, not stdio: newlib's FILE* buffer
// turns one piece into thousands of fs IPCs.)
//-----------------------------------------------------------------------------

static int cache_chunk(torrentfs *t, int ci) {
    if (ci < 0 || ci >= CACHE_MAX_CHUNKS) return -1;
    if (t->chunks[ci] >= 0) return t->chunks[ci];
    char path[256];
    snprintf(path, sizeof(path), "%s.%03d", t->cache_base, ci);
    int f = open(path, O_RDWR | O_CREAT, 0666);
    t->chunks[ci] = f;
    return f;
}

static bool fd_xfer(int f, int64_t off, uint8_t *p, size_t len, bool wr) {
    if (lseek(f, (off_t)off, SEEK_SET) < 0) return false;
    while (len > 0) {
        ssize_t d = wr ? write(f, p, len) : read(f, p, len);
        if (d <= 0) return false;
        p += d;
        len -= (size_t)d;
    }
    return true;
}

static bool piece_loc(torrentfs *t, int64_t idx, bool alloc, int *ci,
                      int64_t *co) {
    int32_t slot = t->piece_slot[idx];
    if (slot < 0) {
        if (!alloc) return false;
        if (t->next_slot >= (int64_t)CACHE_MAX_CHUNKS * t->slots_per_chunk)
            return false;
        slot = (int32_t)t->next_slot++;
        t->piece_slot[idx] = slot;
    }
    *ci = (int)(slot / t->slots_per_chunk);
    *co = (int64_t)(slot % t->slots_per_chunk) * t->meta.piece_len;
    return true;
}

static bool cache_piece_read(torrentfs *t, int64_t idx, int64_t within,
                             void *buf, size_t len) {
    if (t->ram_mode) {
        // Caller (cache_read_upto) holds cache_lock, so the buffer cannot be
        // freed by an eviction mid-copy. A dropped piece reads short, which the
        // player path tolerates (and the read moves the playhead here, pulling
        // the piece back into the window).
        uint8_t *src = t->ram_piece[idx];
        if (!src) return false;
        memcpy(buf, src + within, len);
        return true;
    }
    int ci;
    int64_t co;
    if (!piece_loc(t, idx, false, &ci, &co)) return false;
    int f = cache_chunk(t, ci);
    if (f < 0) return false;
    return fd_xfer(f, co + within, buf, len, false);
}

static void cache_delete_all(torrentfs *t) {
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s.%03d", t->cache_base, i);
        remove(path);
    }
}

// Read as much as is there; the player path must never hard-fail (mpv treats
// an error as a dead stream and stops reading for good).
static size_t cache_read_upto(torrentfs *t, int64_t off, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t total = 0;
    int64_t plen = t->meta.piece_len;
    while (len > 0) {
        int64_t idx    = off / plen;
        int64_t within = off % plen;
        size_t n       = len;
        if (within + (int64_t)n > plen) n = (size_t)(plen - within);
        if (!cache_piece_read(t, idx, within, p, n)) break;
        total += n;
        p += n; off += (int64_t)n; len -= n;
    }
    return total;
}

//-----------------------------------------------------------------------------
// RAM streaming window (ram_mode). Verified pieces stay in RAM rather than
// going to the SD card, so a piece completing never bursts the FAT filesystem
// service on the OS core. Bounded: over budget, the pieces furthest behind the
// playhead are dropped (status DONE -> NEEDED, so a later read that lands on one
// re-downloads it -- the read moves the playhead there, and the streaming
// window only fetches forward, never backfilling what was intentionally
// dropped). All state is touched under cache_lock.
//-----------------------------------------------------------------------------

// Lock order note: this takes t->lock while holding cache_lock. No path takes
// them the other way round (the writer's SD path releases cache_lock before it
// touches t->lock), so the nesting cannot deadlock.
static void ram_evict(torrentfs *t) {   // caller holds t->cache_lock
    int64_t ph = t->playhead_piece;
    while (t->ram_resident > t->ram_budget) {
        while (t->ram_lo < ph && !t->ram_piece[t->ram_lo]) t->ram_lo++;
        if (t->ram_lo >= ph) break;     // nothing strictly behind left to drop
        int64_t v = t->ram_lo;
        free(t->ram_piece[v]);
        t->ram_piece[v] = NULL;
        t->ram_resident -= t->meta.piece_len;
        mutexLock(&t->lock);
        if (t->status[v] == PIECE_DONE) { t->status[v] = PIECE_NEEDED; t->pieces_done--; }
        mutexUnlock(&t->lock);
    }
}

// Move a verified piece buffer into the window, taking ownership of buf.
static void ram_store(torrentfs *t, int64_t idx, uint8_t *buf) {
    mutexLock(&t->cache_lock);
    if (t->ram_piece[idx]) {            // a re-download of a still-resident piece
        free(t->ram_piece[idx]);
        t->ram_resident -= t->meta.piece_len;
    }
    t->ram_piece[idx] = buf;
    t->ram_resident += t->meta.piece_len;
    if (idx < t->ram_lo) t->ram_lo = idx;   // seek-back reload below the cursor
    if (t->ram_resident > t->ram_peak) t->ram_peak = t->ram_resident;
    ram_evict(t);
    mutexUnlock(&t->cache_lock);
}

//-----------------------------------------------------------------------------
// Assembly entries
//-----------------------------------------------------------------------------

static aq_entry *aq_find(torrentfs *t, int64_t idx) {
    for (int i = 0; i < t->n_aq; i++)
        if (t->aq[i].idx == idx) return &t->aq[i];
    return NULL;
}

// A free slot WITH a buffer (buffers move to the writer and come back; a slot
// whose buffer is out cannot start a new piece). The scan-and-take runs under
// t->lock: the writer picks a return slot by the same idx/buf fields, and a
// half-taken slot seen from the other thread would double-assign a buffer.
static aq_entry *aq_alloc(torrentfs *t, int64_t idx) {
    aq_entry *a = NULL;
    mutexLock(&t->lock);
    for (int i = 0; i < t->n_aq; i++) {
        aq_entry *c = &t->aq[i];
        if (c->idx >= 0) continue;
        if (!c->buf) c->buf = malloc((size_t)t->meta.piece_len);
        if (!c->buf) continue;
        c->idx = idx;
        a = c;
        break;
    }
    mutexUnlock(&t->lock);
    if (!a) return NULL;
    a->owner    = -1;
    a->workers  = 0;
    a->have_cnt = 0;
    a->next_req = 0;
    a->nblocks  =
        (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
    memset(a->have, 0, (size_t)t->blocks_per_piece);
    memset(a->req, 0, (size_t)t->blocks_per_piece);
    return a;
}

// Park: keep buffer and progress, forget who was fetching it. Blocks the old
// owner still had on order will either arrive (stored anyway -- lookups are by
// piece, not by owner) or never come; the adopter re-requests what's missing.
static void aq_park(torrentfs *t, aq_entry *a) {
    (void)t;
    a->owner    = -1;
    a->workers  = 0;
    a->next_req = 0;
    memcpy(a->req, a->have, (size_t)a->nblocks);
}

//-----------------------------------------------------------------------------
// Peer pool (shared with the discovery thread -> under t->lock)
//-----------------------------------------------------------------------------

static void add_peers_cb(void *ctx, const peer_addr *peers, int n) {
    torrentfs *t = ctx;
    int added = 0;
    mutexLock(&t->lock);
    for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < t->peer_count; j++)
            if (t->peers[j].ip == peers[i].ip &&
                t->peers[j].port == peers[i].port) { dup = true; break; }
        if (dup) continue;
        int slot = -1;
        if (t->peer_count < TFS_MAX_PEERS) {
            slot = t->peer_count++;
        } else {
            for (int j = 0; j < TFS_MAX_PEERS; j++) {
                if (t->peer_busy[j] || t->peer_fails[j] == 0) continue;
                if (slot < 0 || t->peer_fails[j] > t->peer_fails[slot]) slot = j;
            }
        }
        if (slot < 0) continue;
        t->peers[slot]         = peers[i];
        t->peer_fails[slot]    = 0;
        t->peer_next_try[slot] = 0;
        added++;
    }
    if (added > 0)
        engine_log(ENGINE_LOG_INFO, "[torrentfs] added %d peers (total %d)", added, t->peer_count);
    mutexUnlock(&t->lock);
}

static int take_peer(torrentfs *t, peer_addr *out) {
    u64 now = armGetSystemTick();
    mutexLock(&t->lock);
    int idx = -1;
    if (!t->stop && t->peer_count > 0) {
        for (int tries = 0; tries < t->peer_count; tries++) {
            int i = t->next_peer++ % t->peer_count;
            if (t->peer_busy[i]) continue;
            if (t->peer_next_try[i] > now) continue;
            idx = i;
            *out = t->peers[i];
            t->peer_busy[i] = 1;
            break;
        }
    }
    mutexUnlock(&t->lock);
    return idx;
}

static void release_peer(torrentfs *t, int pidx, bool failed, bool had_conn) {
    if (pidx < 0) return;
    mutexLock(&t->lock);
    t->peer_busy[pidx] = 0;
    if (failed) {
        int f = ++t->peer_fails[pidx];
        int secs = had_conn ? BACKOFF_DROP_SECS : BACKOFF_CONN_SECS * (1 << (f > 5 ? 5 : f - 1));
        if (secs > BACKOFF_MAX_SECS) secs = BACKOFF_MAX_SECS;
        t->peer_next_try[pidx] = armGetSystemTick() + (u64)secs * t->freq;
    } else {
        t->peer_fails[pidx]    = 0;
        t->peer_next_try[pidx] = armGetSystemTick() + (u64)BACKOFF_DROP_SECS * t->freq;
    }
    mutexUnlock(&t->lock);
}

//-----------------------------------------------------------------------------
// Discovery (one thread: trackers, then DHT, then wait)
//-----------------------------------------------------------------------------

static bool file_done(torrentfs *t) {
    mutexLock(&t->lock);
    bool d = t->pieces_done >= t->file_last_piece - t->file_first_piece + 1;
    mutexUnlock(&t->lock);
    return d;
}

static bool discovery_wait(torrentfs *t) {
    u64 start = armGetSystemTick();
    while (!t->stop && !file_done(t)) {
        svcSleepThread(1000000000ULL);  // 1 s
        if (t->announce_now) {
            t->announce_now = false;
            return true;
        }
        u64 secs = (armGetSystemTick() - start) / t->freq;
        if (secs >= DISC_INTERVAL_SECS) return true;
        if (secs >= DISC_STARVED_SECS && t->st_live < STARVED_LIVE) return true;
    }
    return false;
}

static void discovery_main(void *arg) {
    torrentfs *t = arg;
    char e[128];
    do {
        engine_log(ENGINE_LOG_INFO, "[discovery] tracker announce start");
        torrent_announce_cb(&t->meta, add_peers_cb, t, &t->stop, e, sizeof(e));
        if (t->stop) break;
    } while (discovery_wait(t));
    engine_log(ENGINE_LOG_INFO, "[discovery] exiting");
}

//-----------------------------------------------------------------------------
// Writer: verify from RAM, one sequential (sliced) SD write, mark DONE.
//-----------------------------------------------------------------------------

// Count a piece the first time it ever completes, never again. pieces_done
// tracks what is resident *now* (it falls when the RAM window evicts), so it is
// the wrong number for a progress bar; this one only ever climbs. Caller holds
// t->lock.
static void mark_ever_done(torrentfs *t, int64_t idx) {
    uint8_t bit = 1u << (idx & 7);
    if (!(t->ever[idx >> 3] & bit)) {
        t->ever[idx >> 3] |= bit;
        t->pieces_ever++;
    }
}

static void writer_main(void *arg) {
    torrentfs *t = arg;
    for (;;) {
        hb_beat(t, HB_WRITER);
        wjob j;
        bool has = false;
        mutexLock(&t->lock);
        if (t->wq_n > 0) {
            j = t->wq[t->wq_head];
            t->wq_head = (t->wq_head + 1) % (AQ_MAX + 2);
            t->wq_n--;
            has = true;
        }
        bool stopping = t->stop;
        mutexUnlock(&t->lock);

        if (!has) {
            if (stopping) break;   // drained: nothing more can be queued
            svcSleepThread(2000000ULL);  // 2 ms
            continue;
        }

        int nb = (int)((j.plen + BLOCK_LEN - 1) / BLOCK_LEN);
        uint8_t hash[20];
        mbedtls_sha1(j.buf, (size_t)j.plen, hash);
        bool ok = memcmp(hash, t->meta.piece_hashes + j.idx * 20, 20) == 0;

        if (ok && t->ram_mode) {
            // No SD write at all: hand the buffer to the RAM window and mark the
            // piece done. This is the whole point of the mode -- the per-piece
            // FAT write that stutters playback simply never happens.
            ram_store(t, j.idx, j.buf);
            j.buf = NULL;                          // ownership moved to the window
            mutexLock(&t->lock);
            if (t->status[j.idx] != PIECE_DONE) {
                t->status[j.idx] = PIECE_DONE;
                t->pieces_done++;
            }
            mark_ever_done(t, j.idx);
            t->st_piece_ok++;
            t->st_cache_written += j.plen;
            engine_log(ENGINE_LOG_INFO, "[writer] piece %lld verified (RAM)", (long long)j.idx);
        } else if (ok) {
            mutexLock(&t->cache_lock);
            int wci = -1;
            int64_t wco = 0;
            ok = piece_loc(t, j.idx, true, &wci, &wco);
            mutexUnlock(&t->cache_lock);
            u64 lt0 = armGetSystemTick();
            size_t woff = 0;
            while (ok && woff < (size_t)j.plen) {
                size_t n = (size_t)j.plen - woff;
                if (n > WR_SLICE) n = WR_SLICE;
                mutexLock(&t->cache_lock);
                int f = cache_chunk(t, wci);
                ok = f >= 0 && fd_xfer(f, wco + (int64_t)woff, j.buf + woff, n, true);
                mutexUnlock(&t->cache_lock);
                woff += n;
            }
            lat_add(t, LAT_WR, lt0);

            mutexLock(&t->lock);
            if (ok) {
                if (t->status[j.idx] != PIECE_DONE) {
                    t->status[j.idx] = PIECE_DONE;
                    t->pieces_done++;
                }
                mark_ever_done(t, j.idx);
                t->st_piece_ok++;
                t->st_cache_written += j.plen;
                engine_log(ENGINE_LOG_INFO, "[writer] piece %lld verified (SD)", (long long)j.idx);
            } else {
                t->status[j.idx] = PIECE_NEEDED;   // lost: re-download
                t->st_cache_wr_fail++;
                set_err(t, "cache write failed, piece %lld", (long long)j.idx);
                engine_log(ENGINE_LOG_WARN, "[writer] cache write fail piece %lld", (long long)j.idx);
            }
        } else {
            mutexLock(&t->lock);
            t->status[j.idx] = PIECE_NEEDED;       // corrupt: re-download
            t->st_sha_fail++;
            set_err(t, "sha fail piece %lld", (long long)j.idx);
            engine_log(ENGINE_LOG_WARN, "[writer] sha fail piece %lld", (long long)j.idx);
        }
        t->st_blocks_have -= nb;
        // Return the buffer to a bufferless slot for reuse.
        for (int i = 0; i < t->n_aq; i++)
            if (t->aq[i].idx < 0 && !t->aq[i].buf) { t->aq[i].buf = j.buf; j.buf = NULL; break; }
        mutexUnlock(&t->lock);
        free(j.buf);   // no slot took it (shouldn't happen); don't leak
    }
}

//-----------------------------------------------------------------------------
// Sessions
//-----------------------------------------------------------------------------

static void sess_close(torrentfs *t, sess *s, bool failed) {
    if (!s->active) return;
    if (s->claim >= 0) release_claim(t, s);
    bool had_conn = s->nb.handshaked;
    if (s->connecting) {
        // still a raw socket; peer_nb was never attached
        if (s->nb.sock >= 0) close(s->nb.sock);
        s->nb.sock = -1;
        t->st_connecting--;
    } else {
        if (s->nb.handshaked) t->st_live--;
        peer_nb_free(&s->nb);
    }
    release_peer(t, s->pidx, failed, had_conn);
    s->active = false;
    s->connecting = false;
    s->out_n = 0;
}

// Start a non-blocking dial. Returns false when no session/peer is available.
static bool sess_dial(torrentfs *t, u64 now) {
    int sid = -1;
    for (int i = 0; i < MAX_SESS; i++)
        if (!t->S[i].active) { sid = i; break; }
    if (sid < 0) return false;

    peer_addr pa;
    int pidx = take_peer(t, &pa);
    if (pidx < 0) return false;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        t->st_sock_fail++;
        t->st_conn_fail++;
        set_err(t, "socket(): errno %lld", (long long)errno);
        release_peer(t, pidx, true, false);
        return false;
    }
    int fl = fcntl(sock, F_GETFL, 0);
    if (fl >= 0) fcntl(sock, F_SETFL, fl | O_NONBLOCK);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = pa.ip;
    sa.sin_port = htons(pa.port);
    engine_log(ENGINE_LOG_DEBUG, "[sess %d] dial tcp %u.%u.%u.%u:%d",
               sid, pa.ip & 0xff, (pa.ip >> 8) & 0xff,
               (pa.ip >> 16) & 0xff, (pa.ip >> 24) & 0xff, pa.port);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0 &&
        errno != EINPROGRESS) {
        t->st_sock_fail++;
        t->st_conn_fail++;
        engine_log(ENGINE_LOG_DEBUG, "[sess %d] tcp connect failed errno=%d", sid, errno);
        close(sock);
        release_peer(t, pidx, true, false);
        return true;   // slot still free; try another peer next tick
    }

    sess *s = &t->S[sid];
    memset(s, 0, sizeof(*s));
    s->active     = true;
    s->connecting = true;
    s->nb.sock    = sock;   // raw until the connect completes
    s->addr       = pa;
    s->pidx       = pidx;
    s->claim      = -1;
    s->started    = now;
    t->st_connecting++;
    return true;
}

// Start a non-blocking µTP dial.
static bool sess_dial_utp(torrentfs *t, u64 now) {
    int sid = -1;
    for (int i = 0; i < MAX_SESS; i++)
        if (!t->S[i].active) { sid = i; break; }
    if (sid < 0) return false;

    peer_addr pa;
    int pidx = take_peer(t, &pa);
    if (pidx < 0) return false;

    engine_log(ENGINE_LOG_DEBUG, "[sess %d] dial utp %u.%u.%u.%u:%d",
               sid, pa.ip & 0xff, (pa.ip >> 8) & 0xff,
               (pa.ip >> 16) & 0xff, (pa.ip >> 24) & 0xff, pa.port);
    utp_nb_sess *us = utp_nb_connect(pa.ip, pa.port);
    if (!us) {
        engine_log(ENGINE_LOG_DEBUG, "[sess %d] utp connect failed", sid);
        release_peer(t, pidx, true, false);
        return true;
    }

    sess *s = &t->S[sid];
    memset(s, 0, sizeof(*s));
    s->active     = true;
    s->connecting = true;
    s->nb.sock    = -1;
    s->nb.utp     = us;
    s->addr       = pa;
    s->pidx       = pidx;
    s->claim      = -1;
    s->started    = now;
    s->last_rx    = now;
    s->last_ka    = now;

    if (peer_nb_init_utp(&s->nb, us, t->meta.piece_count) != 0) {
        utp_nb_close(us);
        release_peer(t, pidx, true, false);
        return true;
    }
    peer_nb_send_handshake(&s->nb, t->meta.info_hash, t->peer_id);
    peer_nb_flush(&s->nb);
    t->st_connecting++;
    return true;
}

// Pending connect finished (POLLOUT): attach the peer_nb layer and handshake.
static void sess_connected(torrentfs *t, sess *s, u64 now) {
    int soerr = 0;
    socklen_t sl = sizeof(soerr);
    getsockopt(s->nb.sock, SOL_SOCKET, SO_ERROR, &soerr, &sl);
    if (soerr != 0) {
        t->st_conn_fail++;
        t->st_conn_timeout++;   // most refusals here are dead/NAT'd peers
        engine_log(ENGINE_LOG_DEBUG, "[sess] connect soerr=%d", soerr);
        sess_close(t, s, true);
        return;
    }
    int sock = s->nb.sock;
    if (peer_nb_init(&s->nb, sock, t->meta.piece_count) != 0) {
        close(sock);
        s->nb.sock = -1;
        t->st_sock_fail++;
        sess_close(t, s, true);
        return;
    }
    s->connecting = false;
    t->st_connecting--;
    t->st_conn_ok++;
    s->last_rx = s->last_ka = now;
    peer_nb_send_handshake(&s->nb, t->meta.info_hash, t->peer_id);
    peer_nb_flush(&s->nb);
}

//-----------------------------------------------------------------------------
// Claiming and the request pipeline
//-----------------------------------------------------------------------------

// Streaming window; also published for the debug panel.
static void calc_window(torrentfs *t, int64_t *ph, int64_t *lo, int64_t *hi) {
    int64_t p = t->playhead_piece;   // lock-free read (netloop only writes stats)
    int64_t flo = t->file_first_piece, fhi = t->file_last_piece;
    if (p < flo) p = flo;
    if (p > fhi) p = fhi;
    int64_t win = STREAM_WINDOW / t->meta.piece_len;
    if (win < STREAM_MIN_PIECES) win = STREAM_MIN_PIECES;
    // Never look further ahead than the RAM window can hold, or the download
    // races ahead of the playhead, fills the budget, and evicts the very pieces
    // it just fetched -- only to re-download them. Cap at half the window so the
    // other half stays available for seek-back behind the playhead.
    if (t->ram_mode) {
        int64_t cap = (t->ram_budget / 2) / t->meta.piece_len;
        if (cap < 1) cap = 1;
        if (win > cap) win = cap;
    }
    // With very few live peers a wide window just spreads them thin. Narrow it
    // so the scarce peers focus on the immediate next pieces.
    if (t->st_live < 4) {
        int64_t live_cap = t->st_live > 0 ? t->st_live + 1 : STREAM_MIN_PIECES;
        if (live_cap < STREAM_MIN_PIECES) live_cap = STREAM_MIN_PIECES;
        if (win > live_cap) win = live_cap;
    }
    int64_t h = p + win;
    if (h > fhi + 1) h = fhi + 1;
    *ph = p; *lo = flo; *hi = h;
    t->st_win_ph = p; t->st_win_lo = flo; t->st_win_hi = h;
}

// The header/moov probe pieces (the ends of the file). These are fetched
// speculatively at startup and must not be preempted when the playhead moves --
// mpv still needs them to demux even after it seeks to a resume position.
static bool is_critical(torrentfs *t, int64_t idx) {
    return idx < t->file_first_piece + t->crit_head ||
           idx > t->file_last_piece - t->crit_tail;
}

static int unreq_count(const aq_entry *a) {
    if (!a) return 0;
    int n = 0;
    for (int i = 0; i < a->nblocks; i++)
        if (!a->req[i] && !a->have[i]) n++;
    return n;
}

static int missing_count(const aq_entry *a) {
    if (!a) return 0;
    return a->nblocks - a->have_cnt;
}

// Release one session's claim on a piece. If no workers remain, park the
// partial progress so other peers can adopt it.
static void release_claim(torrentfs *t, sess *s) {
    if (s->claim < 0) return;
    aq_entry *a = aq_find(t, s->claim);
    if (a) {
        if (a->owner == s - t->S) a->owner = -1;
        if (a->workers > 0) a->workers--;
        if (a->workers == 0) aq_park(t, a);
    }
    s->claim = -1;
    s->out_n = 0;
}

// Can this session take piece idx? NEEDED needs a free buffer; an ACTIVE entry
// can be joined by multiple sessions as long as there are blocks left to order.
static bool try_claim(torrentfs *t, sess *s, int sid, int64_t idx) {
    if (idx < t->file_first_piece || idx > t->file_last_piece) return false;
    uint8_t st = t->status[idx];
    if (st == PIECE_DONE || st == PIECE_WRITING) return false;
    if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, idx)) return false;

    aq_entry *a;
    if (st == PIECE_ACTIVE) {
        a = aq_find(t, idx);
        if (!a) return false;
        // Join if there are still blocks nobody has requested. Once only missing
        // blocks remain we stay cooperative: duplicates waste bandwidth, so a
        // second peer joins only when endgame is close (<= 4 missing blocks).
        if (unreq_count(a) == 0 && missing_count(a) > 4) return false;
    } else {
        a = aq_alloc(t, idx);
        if (!a) return false;                    // no buffer free right now
        mutexLock(&t->lock);
        t->status[idx] = PIECE_ACTIVE;
        mutexUnlock(&t->lock);
    }
    if (a->owner < 0) a->owner = sid;
    a->workers++;
    s->claim = idx;
    s->out_n = 0;
    s->last_block = armGetSystemTick();
    return true;
}

// Claim a piece without knowing that this peer has it.  Used as a last resort
// for the playhead piece: if no currently-handshaked peer reports having it,
// we still want a session to ask the next peer it connects to.  If that peer
// also does not have it, fill_pipeline leaves out_n==0 and the session drops
// the claim on the next service tick.
static bool try_claim_blind(torrentfs *t, sess *s, int sid, int64_t idx) {
    if (idx < t->file_first_piece || idx > t->file_last_piece) return false;
    uint8_t st = t->status[idx];
    if (st == PIECE_DONE || st == PIECE_WRITING) return false;

    aq_entry *a;
    if (st == PIECE_ACTIVE) {
        a = aq_find(t, idx);
        if (!a) return false;
        if (unreq_count(a) == 0 && missing_count(a) > 4) return false;
    } else {
        a = aq_alloc(t, idx);
        if (!a) return false;
        mutexLock(&t->lock);
        t->status[idx] = PIECE_ACTIVE;
        mutexUnlock(&t->lock);
    }
    if (a->owner < 0) a->owner = sid;
    a->workers++;
    s->claim = idx;
    s->out_n = 0;
    s->last_block = armGetSystemTick();
    return true;
}

// Pick a piece for an idle unchoked session.  If the external scheduler has
// assigned zones, honour them (Critical -> Urgent -> Prefetch -> Speculative
// -> Tail).  Within a zone prefer pieces at or ahead of the playhead.  When no
// scheduler zones are set, fall back to the internal window/critical logic.
static void claim_piece(torrentfs *t, sess *s, int sid) {
    int64_t ph, lo, hi;
    calc_window(t, &ph, &lo, &hi);
    int64_t fhi = t->file_last_piece;

    // External 5-zone scheduler.
    for (int zone = 1; zone <= 5; zone++) {
        int64_t chosen = -1;
        // Closest piece at or ahead of the playhead.
        for (int64_t i = ph; i <= fhi; i++) {
            if (t->piece_zone[i] == zone && try_claim(t, s, sid, i)) {
                chosen = i;
                break;
            }
        }
        if (chosen < 0) {
            // Then anything behind the playhead (keeps tail/cached data valid).
            for (int64_t i = ph - 1; i >= lo; i--) {
                if (t->piece_zone[i] == zone && try_claim(t, s, sid, i)) {
                    chosen = i;
                    break;
                }
            }
        }
        if (chosen >= 0) { t->st_claim_ok++; return; }
    }

    // Internal fallback.
    if (ph <= lo + t->crit_head) {   // startup: tail (moov) then head
        for (int64_t i = fhi; i > fhi - t->crit_tail && i >= lo; i--)
            if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }
        for (int64_t i = lo; i < lo + t->crit_head && i <= fhi; i++)
            if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }
    }
    for (int64_t i = ph; i < hi; i++)
        if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }
    if (!t->ram_mode)
        for (int64_t i = lo; i < ph; i++)
            if (try_claim(t, s, sid, i)) { t->st_claim_ok++; return; }

    // Last resort: the playhead piece itself.  If no currently known peer has
    // it we still assign someone to ask the next peers that handshake.
    if (t->status[ph] == PIECE_NEEDED && try_claim_blind(t, s, sid, ph)) {
        engine_log(ENGINE_LOG_DEBUG, "[claim] blind playhead piece=%lld", (long long)ph);
        t->st_claim_ok++;
        return;
    }

    t->st_claim_fail++;
}

// Find the next block this session can order. Uses the shared cursor first,
// then scans for holes, and finally allows a duplicate during endgame.
static int next_block_to_request(aq_entry *a) {
    while (a->next_req < a->nblocks) {
        int b = a->next_req;
        a->next_req++;
        if (!a->req[b] && !a->have[b]) return b;
    }
    for (int b = 0; b < a->nblocks; b++)
        if (!a->req[b] && !a->have[b]) return b;
    // Endgame: duplicate one missing block so a slow peer does not stall us.
    if (missing_count(a) > 0 && missing_count(a) <= 4) {
        for (int b = 0; b < a->nblocks; b++)
            if (!a->have[b]) return b;
    }
    return -1;
}

// Dynamic pipeline: with few live peers each peer must carry more in-flight
// blocks to keep the line saturated; with many peers we keep the default so
// we do not overwhelm any single peer.
static int pipeline_depth(torrentfs *t) {
    if (t->st_live <= 3) return 128;
    if (t->st_live <= 8) return 80;
    return DEPTH;
}

// Keep the pipeline full for the session's claimed piece.
static void fill_pipeline(torrentfs *t, sess *s) {
    if (s->claim < 0 || s->nb.choked) return;
    aq_entry *a = aq_find(t, s->claim);
    if (!a) { s->claim = -1; return; }
    int64_t plen = torrent_piece_len(&t->meta, s->claim);
    int depth = pipeline_depth(t);
    while (s->out_n < depth) {
        int b = next_block_to_request(a);
        if (b < 0) break;
        uint8_t pl[12];
        uint32_t v;
        v = htonl((uint32_t)s->claim);        memcpy(pl, &v, 4);
        v = htonl((uint32_t)b * BLOCK_LEN);   memcpy(pl + 4, &v, 4);
        v = htonl(block_len_of(plen, b));     memcpy(pl + 8, &v, 4);
        if (peer_nb_queue(&s->nb, MSG_REQUEST, pl, 12) != 0) break;
        a->req[b] = 1;
        s->out_n++;
    }
}

// All blocks landed: hand the buffer to the writer (which verifies + writes).
static void piece_full(torrentfs *t, aq_entry *a) {
    int64_t idx = a->idx;
    t->last_progress_tick = armGetSystemTick();
    mutexLock(&t->lock);
    t->status[idx] = PIECE_WRITING;
    int tail = (t->wq_head + t->wq_n) % (AQ_MAX + 2);
    t->wq[tail] = (wjob){ idx, torrent_piece_len(&t->meta, idx), a->buf };
    t->wq_n++;                 // can't overflow: each job holds one aq buffer
    a->idx = -1;               // slot reset inside the lock: the writer picks
    a->buf = NULL;             // its return slot by these very fields
    a->owner = -1;
    a->workers = 0;
    mutexUnlock(&t->lock);
    for (int i = 0; i < MAX_SESS; i++) {
        if (t->S[i].claim == idx) {
            t->S[i].claim = -1;
            t->S[i].out_n = 0;
        }
    }
}

//-----------------------------------------------------------------------------
// Message handling
//-----------------------------------------------------------------------------

static void sess_msg(torrentfs *t, sess *s, int sid, uint8_t id, uint8_t *pl,
                     uint32_t plen, u64 now) {
    (void)sid;
    switch (id) {
        case MSG_CHOKE:
            s->nb.choked = true;
            t->st_choked++;
            engine_log(ENGINE_LOG_DEBUG, "[sess] choke");
            if (s->claim >= 0) {   // outstanding requests are void now
                release_claim(t, s);
            }
            break;
        case MSG_UNCHOKE:
            s->nb.choked = false;
            t->st_unchoke_ok++;
            engine_log(ENGINE_LOG_DEBUG, "[sess] unchoke");
            break;
        case MSG_INTERESTED:
            t->st_interested_recv++;
            break;
        case MSG_REQUEST:
            t->st_request_recv++;   // leech-only: we never unchoke, ignore
            break;
        case MSG_HAVE:
            if (plen == 4) {
                uint32_t v;
                memcpy(&v, pl, 4);
                int64_t idx = (int64_t)ntohl(v);
                if (idx >= 0 && idx < t->meta.piece_count)
                    s->nb.bitfield[idx / 8] |= (uint8_t)(0x80 >> (idx % 8));
            }
            break;
        case MSG_BITFIELD:
            if (plen == s->nb.bitfield_len) {
                memcpy(s->nb.bitfield, pl, plen);
                t->st_bf_ok++;
                bool empty = true;
                for (size_t b = 0; b < plen && empty; b++)
                    if (pl[b]) empty = false;
                engine_log(ENGINE_LOG_DEBUG, "[sess] bitfield ok empty=%d", empty ? 1 : 0);
            } else {
                t->st_bf_bad++;
                engine_log(ENGINE_LOG_DEBUG, "[sess] bitfield bad len=%u expected=%zu", plen, s->nb.bitfield_len);
            }
            break;
        case MSG_PIECE: {
            if (plen < 8) break;
            uint32_t iv, bv;
            memcpy(&iv, pl, 4);
            memcpy(&bv, pl + 4, 4);
            int64_t idx     = (int64_t)ntohl(iv);
            uint32_t begin  = ntohl(bv);
            uint32_t dlen   = plen - 8;
            if (idx < 0 || idx >= t->meta.piece_count) break;
            if (begin % BLOCK_LEN != 0) break;
            int b = (int)(begin / BLOCK_LEN);
            int64_t p_len = torrent_piece_len(&t->meta, idx);
            if (dlen != block_len_of(p_len, b)) break;

            t->st_bytes_recv += dlen;
            s->last_block = now;
            s->rx_total += dlen;
            if (s->out_n > 0) s->out_n--;

            // Stored by piece, not by owner: a parked piece's stragglers (or
            // an adopted piece's duplicates) still count.
            aq_entry *a = aq_find(t, idx);
            if (!a || b >= a->nblocks || a->have[b]) break;
            memcpy(a->buf + begin, pl + 8, dlen);
            a->have[b] = 1;
            a->req[b]  = 1;
            a->have_cnt++;
            mutexLock(&t->lock);
            t->st_blocks_have++;
            mutexUnlock(&t->lock);
            t->last_progress_tick = now;
            if (a->have_cnt == a->nblocks) piece_full(t, a);
            break;
        }
        default:
            break;   // MSG_CANCEL / MSG_NOT_INTERESTED: nothing useful to do
    }
}

static void sess_service(torrentfs *t, sess *s, int sid, u64 now) {
    ssize_t got = peer_nb_pump_rx(&s->nb);
    if (got < 0) { t->st_fetch_fail++; engine_log(ENGINE_LOG_DEBUG, "[sess] rx error"); sess_close(t, s, true); return; }
    if (got > 0) s->last_rx = now;

    if (!s->nb.handshaked) {
        int hs = peer_nb_recv_handshake(&s->nb, t->meta.info_hash);
        if (hs < 0) { sess_close(t, s, true); return; }
        if (hs == 0) return;
        t->st_live++;
        if (t->st_live > t->st_peak_live) t->st_peak_live = t->st_live;
        engine_log(ENGINE_LOG_INFO, "[sess] handshake ok live=%d/%d",
                   t->st_live, MAX_SESS);
        peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
    }

    for (;;) {
        uint8_t id;
        uint8_t *pl;
        uint32_t plen;
        int r = peer_nb_next(&s->nb, &id, &pl, &plen);
        if (r < 0) { engine_log(ENGINE_LOG_DEBUG, "[sess] message decode error"); sess_close(t, s, true); return; }
        if (r == 0) break;
        sess_msg(t, s, sid, id, pl, plen, now);
        if (!s->active) return;
    }

    fill_pipeline(t, s);

    // If we hold a piece but could not request any blocks (peer is unchoked and
    // the piece is still incomplete), the peer does not have it.  Drop the claim
    // so the session can try another peer / piece.  This is especially important
    // for blind playhead claims that turn out to be on peers without the block.
    if (s->claim >= 0 && !s->nb.choked && s->out_n == 0) {
        aq_entry *a = aq_find(t, s->claim);
        if (a && a->have_cnt < a->nblocks) {
            engine_log(ENGINE_LOG_DEBUG,
                       "[sess] peer lacks piece %lld, releasing claim",
                       (long long)s->claim);
            release_claim(t, s);
        }
    }

    if (peer_nb_flush(&s->nb) != 0) { sess_close(t, s, true); return; }
}

//-----------------------------------------------------------------------------
// Netloop
//-----------------------------------------------------------------------------

// Calm budget: how many sessions may hold a claim, from the player's backlog.
// The whole swarm bursting at wifi line rate on every window slide is what
// freezes the console (bsd/wlan pay per packet on the OS core).
static int calm_budget(torrentfs *t) {
    int ms = t->backlog_ms;   // racy read, written by the app thread
    int budget = ms >= 30000 ? 1 : ms >= 20000 ? 3 : ms >= 10000 ? 8 : MAX_SESS;

    if (g_governor && ms >= 10000) {
        // Rate above the backlog-tied target: pause claiming entirely.
        double target = ms >= 25000
                            ? 1.5e6
                            : 6.0e6 - (ms - 10000) * (4.5e6 / 15000.0);
        if (t->rate_bps > target) budget = 0;
    }
    return budget;
}

// Refresh every session's smoothed receive rate. Metered per session (so it is
// piece-size independent) and only WHILE actively downloading: between claims or
// while choked the rate is frozen, not decayed -- a fast peer that just finished
// a piece must keep its measured rate, or the steerer sees it as slow the moment
// it goes idle and never picks it to take over. rate_start (the grace anchor)
// persists across those pauses; a fresh session is zeroed by sess_dial's memset.
// Run once per upkeep tick.
static void update_rates(torrentfs *t, u64 now) {
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || !s->nb.handshaked) { s->rate_tick = 0; continue; }
        // Not downloading: pause the meter, keep rate + rate_start.
        if (s->claim < 0 || s->nb.choked || s->out_n == 0) {
            s->rate_tick = 0;
            continue;
        }
        if (s->rate_start == 0) s->rate_start = now;   // first download: start grace
        if (s->rate_tick == 0) {                        // (re)open the sample window
            s->rate_prev = s->rx_total;
            s->rate_tick = now;
            continue;
        }
        double dt = (double)(now - s->rate_tick) / (double)t->freq;
        if (dt < 0.4) continue;    // at most one sample per upkeep
        double inst = (double)(s->rx_total - s->rate_prev) / dt;
        if (inst < 0) inst = 0;
        s->rate      = s->rate * 0.6 + inst * 0.4;
        s->rate_prev = s->rx_total;
        s->rate_tick = now;
    }
}

// The single piece that most needs the fastest peer right now: at startup the
// container's head/tail (the moov probe that gates the first frame), otherwise
// the nearest not-yet-downloaded piece ahead of the playhead (what the player
// will block on soonest). -1 if there is nothing to steer.
static int64_t hot_piece(torrentfs *t) {
    int64_t lo = t->file_first_piece, fhi = t->file_last_piece;
    int64_t ph = t->playhead_piece;
    if (ph < lo) ph = lo;
    if (ph > fhi) ph = fhi;
    if (ph <= lo + t->crit_head) {           // startup: head then tail
        for (int64_t i = lo; i < lo + t->crit_head && i <= fhi; i++)
            if (t->status[i] != PIECE_DONE && t->status[i] != PIECE_WRITING) return i;
        for (int64_t i = fhi; i > fhi - t->crit_tail && i >= lo; i--)
            if (t->status[i] != PIECE_DONE && t->status[i] != PIECE_WRITING) return i;
    }
    int64_t win = STREAM_WINDOW / t->meta.piece_len;
    if (win < STREAM_MIN_PIECES) win = STREAM_MIN_PIECES;
    for (int64_t i = ph; i <= ph + win && i <= fhi; i++)
        if (t->status[i] != PIECE_DONE && t->status[i] != PIECE_WRITING) return i;
    return -1;
}

// How urgent a piece is (lower = more urgent). At startup the head/tail moov
// probe outranks everything (head before tail); otherwise it is the distance
// ahead of the playhead. Index order is NOT urgency order -- the tail critical
// has a high index but is needed before piece #1 -- so steering must compare by
// this, not by raw index, to know which peer is on the more important piece.
static int64_t piece_urgency(torrentfs *t, int64_t p) {
    int64_t lo = t->file_first_piece, fhi = t->file_last_piece;
    int64_t ph = t->playhead_piece;
    if (ph < lo) ph = lo;
    if (ph > fhi) ph = fhi;
    if (ph <= lo + t->crit_head && is_critical(t, p)) {
        if (p < lo + t->crit_head) return p - lo;   // head: 0, 1, ...
        return t->crit_head + (fhi - p);            // tail: just after the head
    }
    if (p >= ph) return 1000 + (p - ph);            // ahead of the playhead
    return 2000000 + (ph - p);                      // behind it: least urgent
}

// Put the fastest available peer on the most urgent piece, instead of banning
// slow ones. A slow peer that grabbed the head/tail (or the piece the player is
// about to need) first would otherwise hold up the start / the buffer while
// faster peers work pieces further ahead. Demote-and-swap, never kill: the slow
// peer keeps downloading, just on a less urgent piece it naturally re-claims
// (the front is now taken by the fast peer). One swap per tick keeps it stable.
static void steer_peers(torrentfs *t, u64 now) {
    if (t->st_live <= 1) return;
    int64_t hot = hot_piece(t);
    if (hot < 0) return;

    int64_t hot_u = piece_urgency(t, hot);
    u64 grace = (u64)SLOW_MEASURE_SECS * t->freq;
    sess *owner = NULL, *best = NULL;
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || !s->nb.handshaked || s->nb.choked) continue;
        if (s->claim == hot) { owner = s; continue; }
        // Skip only a peer already on a MORE urgent piece -- by urgency, not by
        // index: a peer on #1 must still be a candidate to take the tail moov.
        if (s->claim >= 0 && piece_urgency(t, s->claim) < hot_u) continue;
        if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, hot)) continue;
        if (best == NULL || s->rate > best->rate) best = s;
    }
    if (!best) return;

    if (owner) {
        // Only take over on a clear win, and only once both are metered enough
        // to trust the comparison -- otherwise leave the current owner alone.
        // rate_start (not rate_tick) so an idle-but-fast challenger still counts:
        // its meter is paused, but its measured rate and grace anchor persist.
        if (best->rate_start == 0 || now - best->rate_start < grace) return;
        if (owner->rate_start == 0 || now - owner->rate_start < grace) return;
        if (best->rate <= owner->rate * SLOW_FACTOR_CRIT) return;
        // Fast peer joins the hot piece; the slower owner keeps helping instead
        // of being parked, so we do not lose in-flight blocks.
    }
    // best drops its colder piece, then takes hot (or joins it).
    if (best->claim >= 0 && best->claim != hot) {
        release_claim(t, best);
    }
    int bi = (int)(best - t->S);
    if (try_claim(t, best, bi, hot)) {
        fill_pipeline(t, best);
        peer_nb_flush(&best->nb);
    }
}

static void netloop_main(void *arg) {
    torrentfs *t = arg;
    u64 last_upkeep = 0;
    engine_log(ENGINE_LOG_INFO, "[netloop] start");

    while (!t->stop) {
        hb_beat(t, HB_NET);
        u64 now = armGetSystemTick();

        // Upkeep every ~500 ms: dial, reap, keep-alives, rate EWMA, calm.
        if (now - last_upkeep > t->freq / 2) {
            last_upkeep = now;

            // Download-rate EWMA (governor input).
            if (t->rate_last_tick) {
                double dt = (double)(now - t->rate_last_tick) / t->freq;
                if (dt > 0.05) {
                    double inst = (double)(t->st_bytes_recv - t->rate_last_bytes) / dt;
                    t->rate_bps = t->rate_bps * 0.7 + inst * 0.3;
                }
            }
            t->rate_last_bytes = t->st_bytes_recv;
            t->rate_last_tick  = now;
            t->calm_now        = calm_budget(t);

            // When peers are scarce, fail outbound connects faster so dead
            // addresses do not monopolise the connecting budget.
            bool few_peers = t->st_live < STARVED_LIVE ||
                             (now - t->last_progress_tick > (u64)10 * t->freq);
            int conn_secs = few_peers ? 1 : CONNECT_SECS;

            for (int i = 0; i < MAX_SESS; i++) {
                sess *s = &t->S[i];
                if (!s->active) continue;
                u64 age = now - (s->connecting ? s->started : s->last_rx);
                if (s->connecting && age > (u64)conn_secs * t->freq) {
                    t->st_conn_fail++;
                    t->st_conn_timeout++;
                    engine_log(ENGINE_LOG_DEBUG, "[sess] close connect timeout");
                    sess_close(t, s, true);
                    continue;
                }
                if (!s->connecting && !s->nb.handshaked &&
                    now - s->started > (u64)PREHS_SECS * t->freq) {
                    engine_log(ENGINE_LOG_DEBUG, "[sess] close handshake timeout");
                    sess_close(t, s, true);
                    continue;
                }
                if (!s->connecting && age > (u64)IDLE_SECS * t->freq) {
                    engine_log(ENGINE_LOG_DEBUG, "[sess] close idle timeout");
                    sess_close(t, s, true);
                    continue;
                }
                // Requested blocks and nothing came: the peer accepted work it
                // will not deliver -- drop it, its claim gets adopted.
                if (s->out_n > 0 &&
                    now - s->last_block > (u64)STALL_SECS * t->freq) {
                    t->st_fetch_fail++;
                    set_err(t, "stall, piece %lld", (long long)s->claim);
                    engine_log(ENGINE_LOG_DEBUG, "[sess] close stall piece=%lld", (long long)s->claim);
                    sess_close(t, s, true);
                    continue;
                }
                // Rate metering + strength steering run as their own passes
                // (update_rates / steer_peers) after this loop.

                // Keep-alive: raw 4 zero bytes, only when the tx queue is
                // empty (an atomic small send cannot interleave mid-message).
                if (!s->connecting && s->nb.handshaked &&
                    now - s->last_ka > (u64)KEEPALIVE_SECS * t->freq &&
                    !peer_nb_tx_pending(&s->nb)) {
                    uint8_t ka[4] = {0};
                    send(s->nb.sock, ka, 4, 0);
                    s->last_ka = now;
                }
            }

            update_rates(t, now);   // per-session rate meter
            steer_peers(t, now);    // fastest peer -> most urgent piece

            int connecting = 0, bf_empty = 0;
            for (int i = 0; i < MAX_SESS; i++) {
                sess *s = &t->S[i];
                if (!s->active) continue;
                if (s->connecting) { connecting++; continue; }
                if (s->nb.handshaked && s->nb.bitfield) {
                    bool any = false;
                    for (size_t b = 0; b < s->nb.bitfield_len && !any; b++)
                        if (s->nb.bitfield[b]) any = true;
                    if (!any) bf_empty++;
                }
            }
            t->st_bf_empty = bf_empty;
            // Starvation recovery: if we have almost no live peers, stop wasting
            // connection slots on µTP and retry peers that failed earlier. DHT
            // swarms are often full of dead/NAT'd addresses; without this the pool
            // can exhaust its backoff budget and stall the stream.
            if (few_peers) {
                static u64 last_starve;
                if (now - last_starve > (u64)5 * t->freq) {
                    last_starve = now;
                    int reset = 0;
                    mutexLock(&t->lock);
                    for (int i = 0; i < t->peer_count && reset < 64; i++) {
                        if (t->peer_busy[i]) continue;
                        if (t->peer_fails[i] > 0) {
                            t->peer_fails[i] = 0;
                            t->peer_next_try[i] = 0;
                            reset++;
                        }
                    }
                    mutexUnlock(&t->lock);
                    engine_log(ENGINE_LOG_WARN,
                               "[torrentfs] starvation recovery: reset %d peers, live=%d",
                               reset, t->st_live);
                    t->announce_now = true;
                }
            }

            while (!t->paused &&
                   t->st_live < DIAL_STOP_LIVE && connecting < MAX_CONNECTING) {
                bool ok;
                bool try_utp = utp_nb_fd() >= 0 && !few_peers &&
                               (t->dial_toggle % 4) == 0;
                if (try_utp) {
                    ok = sess_dial_utp(t, now);
                } else {
                    ok = sess_dial(t, now);
                }
                t->dial_toggle = (t->dial_toggle + 1) & 0x7f;
                if (!ok) break;
                connecting++;
            }

            // Seek preemption: if the playhead has moved so a session's piece is
            // no longer in the streaming window, drop that claim so the session
            // re-targets the new window instead of finishing a piece the player
            // has moved away from. With multiple workers on one piece, the slot
            // is only freed once the last worker leaves.
            {
                int64_t pph, plo, phi;
                calc_window(t, &pph, &plo, &phi);
                (void)plo;
                for (int i = 0; i < MAX_SESS; i++) {
                    sess *s = &t->S[i];
                    if (!s->active || s->claim < 0) continue;
                    int64_t cl = s->claim;
                    if (cl >= pph && cl < phi) continue;   // still in the window
                    if (is_critical(t, cl)) continue;      // moov probe: keep it
                    aq_entry *a = aq_find(t, cl);
                    if (a && a->workers <= 1) {
                        mutexLock(&t->lock);
                        if (t->status[cl] == PIECE_ACTIVE) {
                            t->status[cl] = PIECE_NEEDED;  // discard partial, re-DL
                            a->idx = -1;                   // release slot + buffer
                        }
                        mutexUnlock(&t->lock);
                    }
                    release_claim(t, s);
                }
            }

            // Claims for idle unchoked sessions, within the calm budget.
            // Nothing new while paused: existing pipelines are left to drain
            // (their pieces may as well land) but no fresh work is started.
            int claiming = 0;
            for (int i = 0; i < MAX_SESS; i++)
                if (t->S[i].active && t->S[i].claim >= 0) claiming++;
            for (int i = 0; i < MAX_SESS && claiming < t->calm_now; i++) {
                sess *s = &t->S[i];
                if (!s->active || s->connecting || !s->nb.handshaked) continue;
                if (s->nb.choked || s->claim >= 0) continue;
                if (t->paused) break;
                claim_piece(t, s, i);
                if (s->claim >= 0) {
                    claiming++;
                    fill_pipeline(t, s);
                    peer_nb_flush(&s->nb);
                }
            }
        }

        // One poll over every session socket plus the shared µTP UDP socket.
        struct pollfd pfd[MAX_SESS + 1];
        int map[MAX_SESS + 1];
        int n = 0;
        for (int i = 0; i < MAX_SESS; i++) {
            sess *s = &t->S[i];
            if (!s->active || s->nb.sock < 0) continue;
            pfd[n].fd = s->nb.sock;
            pfd[n].events = s->connecting
                                ? POLLOUT
                                : (short)(POLLIN | (peer_nb_tx_pending(&s->nb)
                                                        ? POLLOUT : 0));
            pfd[n].revents = 0;
            map[n] = i;
            n++;
        }
        int utp_fd = utp_nb_fd();
        if (utp_fd >= 0) {
            pfd[n].fd = utp_fd;
            pfd[n].events = POLLIN;
            pfd[n].revents = 0;
            map[n] = -1;
            n++;
        }

        u64 lt0 = armGetSystemTick();
        int pr = poll(pfd, (nfds_t)n, 200);
        lat_add(t, LAT_POLL, lt0);

        now = armGetSystemTick();
        bool utp_serviced = false;
        if (pr > 0) {
            for (int k = 0; k < n; k++) {
                if (!pfd[k].revents) continue;
                if (map[k] == -1) {
                    // Shared µTP socket: process packets and timeouts.
                    utp_nb_service();
                    utp_serviced = true;
                    continue;
                }
                sess *s = &t->S[map[k]];
                if (!s->active) continue;
                if (s->connecting) {
                    if (pfd[k].revents & (POLLOUT | POLLERR | POLLHUP))
                        sess_connected(t, s, now);
                    continue;
                }
                if (pfd[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    t->st_fetch_fail++;
                    engine_log(ENGINE_LOG_DEBUG, "[sess] poll error revents=0x%x", pfd[k].revents);
                    sess_close(t, s, true);
                    continue;
                }
                if (pfd[k].revents & POLLIN)
                    sess_service(t, s, map[k], now);
                if (s->active && (pfd[k].revents & POLLOUT))
                    if (peer_nb_flush(&s->nb) != 0) sess_close(t, s, true);
            }
        }
        // Service µTP sessions even if the UDP fd did not wake us, so libutp
        // timeouts and buffered data are handled on every loop iteration.
        if (!utp_serviced && utp_fd >= 0) utp_nb_service();
        for (int i = 0; i < MAX_SESS; i++) {
            sess *s = &t->S[i];
            if (!s->active || !s->nb.utp) continue;
            if (s->connecting) {
                int st = utp_nb_state((utp_nb_sess *)s->nb.utp);
                if (st == 1) {
                    s->connecting = false;
                    t->st_connecting--;
                    t->st_conn_ok++;
                    s->last_rx = s->last_ka = now;
                } else if (st < 0) {
                    sess_close(t, s, true);
                    continue;
                }
            }
            sess_service(t, s, i, now);
        }
    }

    for (int i = 0; i < MAX_SESS; i++) sess_close(t, &t->S[i], false);
    engine_log(ENGINE_LOG_INFO, "[netloop] stop");
}

//-----------------------------------------------------------------------------
// Open / close
//-----------------------------------------------------------------------------

torrentfs *torrentfs_open(const char *source, const char *cache_path,
                          char *err, size_t errlen) {
    return torrentfs_open_file(source, cache_path, -1, err, errlen);
}

torrentfs *torrentfs_open_file(const char *source, const char *cache_path,
                               int file_index, char *err, size_t errlen) {
    return torrentfs_open_file_cancel(source, cache_path, file_index, NULL,
                                      err, errlen);
}

torrentfs *torrentfs_open_file_cancel(const char *source, const char *cache_path,
                                      int file_index,
                                      const volatile bool *cancel,
                                      char *err, size_t errlen) {
    torrentfs *t = calloc(1, sizeof(*t));
    if (!t) { snprintf(err, errlen, "out of memory"); return NULL; }

    for (int i = 0; i < AQ_MAX; i++) t->aq[i].idx = -1;
    for (int i = 0; i < MAX_SESS; i++) t->S[i].claim = -1;
    t->freq     = armGetSystemTickFreq();
    t->ram_mode = g_ram_stream != 0;   // latched for this torrent's lifetime
    t->last_progress_tick = armGetSystemTick();

    peer_addr seed_peers[80];
    int seed_count = 0;

    int rc = strncmp(source, "magnet:", 7) == 0
                 ? torrent_load_magnet_peers_cancel(&t->meta, source, seed_peers,
                                                    (int)(sizeof(seed_peers) /
                                                          sizeof(seed_peers[0])),
                                                    &seed_count, cancel,
                                                    err, errlen)
                 : torrent_load(&t->meta, source, err, errlen);
    if (rc != 0) {
        free(t);
        return NULL;
    }

    int fi = file_index;
    if (fi < 0 || fi >= t->meta.file_count) fi = torrent_largest_file(&t->meta);
    if (fi < 0) {
        snprintf(err, errlen, "torrent has no files");
        torrent_unload(&t->meta);
        free(t);
        return NULL;
    }
    t->stream_offset    = t->meta.files[fi].offset;
    t->stream_size      = t->meta.files[fi].length;
    t->file_first_piece = t->stream_offset / t->meta.piece_len;
    t->file_last_piece  =
        (t->stream_offset + t->stream_size - 1) / t->meta.piece_len;
    t->playhead_piece   = t->file_first_piece;

    t->blocks_per_piece =
        (int)((t->meta.piece_len + BLOCK_LEN - 1) / BLOCK_LEN);

    // Critical pieces = ceil(budget / piece_len), clamped: one big piece already
    // covers the probed region, so we do not pre-fetch three of them.
    int64_t plen = t->meta.piece_len;
    t->crit_head = (int)((CRIT_HEAD_BYTES + plen - 1) / plen);
    t->crit_tail = (int)((CRIT_TAIL_BYTES + plen - 1) / plen);
    if (t->crit_head < 1) t->crit_head = 1;
    if (t->crit_head > CRIT_HEAD_MAX) t->crit_head = CRIT_HEAD_MAX;
    if (t->crit_tail < 1) t->crit_tail = 1;
    if (t->crit_tail > CRIT_TAIL_MAX) t->crit_tail = CRIT_TAIL_MAX;

    engine_log(ENGINE_LOG_INFO,
               "[torrentfs] open name=%s file=%d size=%lld pieces=%lld ram=%d",
               t->meta.name[0] ? t->meta.name : "torrent",
               fi, (long long)t->stream_size,
               (long long)(t->file_last_piece - t->file_first_piece + 1),
               t->ram_mode ? 1 : 0);

    t->status     = calloc(1, (size_t)t->meta.piece_count);
    t->piece_zone = calloc(1, (size_t)t->meta.piece_count);
    t->ever       = calloc(1, (size_t)(t->meta.piece_count + 7) / 8);

    int64_t n_aq = RAM_BUDGET / t->meta.piece_len;
    if (n_aq < AQ_MIN) n_aq = AQ_MIN;
    if (n_aq > AQ_MAX) n_aq = AQ_MAX;
    t->n_aq = (int)n_aq;
    bool aq_ok = true;
    for (int i = 0; i < t->n_aq; i++) {
        t->aq[i].have = calloc(1, (size_t)t->blocks_per_piece);
        t->aq[i].req  = calloc(1, (size_t)t->blocks_per_piece);
        if (!t->aq[i].have || !t->aq[i].req) aq_ok = false;
    }

    snprintf(t->cache_base, sizeof(t->cache_base), "%s", cache_path);
    mutexInit(&t->cache_lock);
    mutexInit(&t->lock);
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++) t->chunks[i] = -1;
    t->calm_now = MAX_SESS;
    t->slots_per_chunk = CACHE_CHUNK / t->meta.piece_len;
    t->piece_slot = malloc((size_t)t->meta.piece_count * sizeof(int32_t));
    if (t->piece_slot)
        for (int64_t i = 0; i < t->meta.piece_count; i++) t->piece_slot[i] = -1;
    cache_delete_all(t);  // a previous run's leftovers (crash) are dead weight

    if (t->ram_mode) {
        t->ram_budget = RAM_STREAM_BUDGET;
        t->ram_lo     = t->file_first_piece;
        t->ram_piece  = calloc((size_t)t->meta.piece_count, sizeof(uint8_t *));
    }
    // The SD cache is only needed when not streaming into RAM: skip its file so
    // RAM mode touches the card zero times.
    bool cache_ok = t->ram_mode
                        ? (t->ram_piece != NULL)
                        : (t->piece_slot && t->slots_per_chunk >= 1 &&
                           cache_chunk(t, 0) >= 0);
    if (!t->status || !t->ever || !aq_ok || !cache_ok) {
        snprintf(err, errlen, "cache/status alloc failed");
        torrentfs_close(t);
        return NULL;
    }

    memcpy(t->peer_id, "-SW0003-", 8);
    srand((unsigned)time(NULL));
    for (int i = 8; i < 20; i++) t->peer_id[i] = (uint8_t)(rand() % 256);

    if (seed_count > 0) add_peers_cb(t, seed_peers, seed_count);

    // Persistent background DHT: keeps the routing table warm and re-issues
    // searches every 30 s, instead of one-shot lookups that re-bootstrap each time.
    dht_background_add(t->meta.info_hash, add_peers_cb, t);

    if (threadCreate(&t->discovery, discovery_main, t, NULL, 0x20000, 0x2C,
                     -2) == 0) {
        t->discovery_started = true;
        threadStart(&t->discovery);
    }
    // Priority 0x2D, one notch BELOW the app default (0x2C): the writer runs
    // on the render thread's default core, and Horizon neither timeslices nor
    // preempts between equal priorities -- at 0x2C, a render thread waking
    // from vsync waited out whatever slice of a 4 MB SD write (or the SHA-1)
    // the writer had started, and the playback draw loop has zero frame
    // budget to spare. One notch lower, the renderer preempts it instead.
    // The writer needs ~1 write per streamed piece; it starves only if every
    // core is 100% busy, which hardware-decoded playback never sustains.
    if (threadCreate(&t->writer, writer_main, t, NULL, 0x20000, 0x2D, -2) == 0) {
        t->writer_started = true;
        threadStart(&t->writer);
    } else {
        snprintf(err, errlen, "could not start the writer thread");
        torrentfs_close(t);
        return NULL;
    }
    // Priority 0x2B, one notch above the app default: Horizon never preempts
    // between equal priorities, and a CPU-bound mpv thread otherwise starves
    // this loop's ~100 IPC yield points for seconds at a time (v2's finding).
    if (threadCreate(&t->netloop, netloop_main, t, NULL, 0x40000, 0x2B, -2) == 0) {
        t->netloop_started = true;
        threadStart(&t->netloop);
    } else {
        snprintf(err, errlen, "could not start the network thread");
        torrentfs_close(t);
        return NULL;
    }

    return t;
}

void torrentfs_close(torrentfs *tfs) {
    if (!tfs) return;
    tfs->stop = true;

    // Stop the background DHT before freeing the torrent context it delivers to.
    dht_background_remove(tfs->meta.info_hash);

    if (tfs->discovery_started) {
        threadWaitForExit(&tfs->discovery);
        threadClose(&tfs->discovery);
        tfs->discovery_started = false;
    }
    if (tfs->netloop_started) {
        threadWaitForExit(&tfs->netloop);
        threadClose(&tfs->netloop);
        tfs->netloop_started = false;
    }
    // The writer drains its queue after stop; join it after the netloop
    // (which can still push jobs).
    if (tfs->writer_started) {
        threadWaitForExit(&tfs->writer);
        threadClose(&tfs->writer);
        tfs->writer_started = false;
    }
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++)
        if (tfs->chunks[i] >= 0) { close(tfs->chunks[i]); tfs->chunks[i] = -1; }
    cache_delete_all(tfs);  // per-playback scratch, not a library
    for (int i = 0; i < AQ_MAX; i++) {
        free(tfs->aq[i].buf);
        free(tfs->aq[i].have);
        free(tfs->aq[i].req);
    }
    if (tfs->ram_piece) {   // free before torrent_unload: needs meta.piece_count
        for (int64_t i = 0; i < tfs->meta.piece_count; i++) free(tfs->ram_piece[i]);
        free(tfs->ram_piece);
    }
    free(tfs->status);
    free(tfs->piece_zone);
    free(tfs->ever);
    free(tfs->piece_slot);
    torrent_unload(&tfs->meta);
    free(tfs);
}

//-----------------------------------------------------------------------------
// The player-facing read path (mpv demuxer thread). LOCK-FREE on purpose:
// Horizon does not timeslice equal-priority threads, so a CPU-bound mpv
// thread can starve this one for whole seconds -- if it held an engine lock
// at that moment, the netloop would freeze with it. An aligned 64-bit store
// is atomic on AArch64.
//-----------------------------------------------------------------------------

int64_t torrentfs_size(const torrentfs *tfs) {
    return tfs->stream_size;
}

void torrentfs_set_playhead(torrentfs *tfs, int64_t offset) {
    int64_t abs = tfs->stream_offset + offset;
    ((torrentfs *)tfs)->playhead_piece = abs / tfs->meta.piece_len;
}

// Pause/resume downloading. Paused: no new dials, no new piece claims;
// existing sessions stay connected (keep-alives) and in-flight pipelines are
// left to drain. A blocked read keeps waiting for its piece, so an installer
// mid-read simply stalls until resume -- the intended backpressure.
void torrentfs_pause(torrentfs *tfs, int on) {
    if (!tfs) return;
    int v = on ? 1 : 0;
    if (tfs->paused == v) return;  // idempotent: polled from the UI
    tfs->paused = v;
    engine_log(ENGINE_LOG_INFO, "[torrentfs] %s", on ? "paused" : "resumed");
}

static bool have_piece(torrentfs *t, int64_t idx) {
    // DONE is terminal and only set once the piece is fully on disk, so the
    // worst a stale read costs is one extra 20 ms wait.
    return t->status[idx] == PIECE_DONE;
}

int64_t torrentfs_read(torrentfs *tfs, int64_t offset, char *buf, int64_t nbytes) {
    hb_beat(tfs, HB_READER);
    if (offset >= tfs->stream_size) return 0;
    if (offset + nbytes > tfs->stream_size) nbytes = tfs->stream_size - offset;
    if (nbytes <= 0) return 0;

    torrentfs_set_playhead(tfs, offset);

    int64_t abs       = tfs->stream_offset + offset;
    int64_t abs_total = tfs->stream_offset + tfs->stream_size;
    int64_t plen      = tfs->meta.piece_len;
    int64_t first     = abs / plen;

    u64 wait_start = armGetSystemTick();
    while (!tfs->stop && !have_piece(tfs, first))
        svcSleepThread(20000000ULL);  // 20 ms
    if (tfs->stop) return -1;
    double waited = (double)(armGetSystemTick() - wait_start) / tfs->freq;
    if (waited > 5.0) {
        engine_log(ENGINE_LOG_WARN,
                   "[read] waited %.1fs for piece %lld", waited, (long long)first);
    }

    int64_t last_byte  = abs + nbytes - 1;
    int64_t last_piece = last_byte / plen;
    int64_t avail_end  = (first + 1) * plen;
    for (int64_t pc = first + 1; pc <= last_piece; pc++) {
        if (!have_piece(tfs, pc)) break;
        avail_end = (pc + 1) * plen;
    }
    if (avail_end > abs_total) avail_end = abs_total;

    int64_t can_read = avail_end - abs;
    if (can_read > nbytes) can_read = nbytes;

    mutexLock(&tfs->cache_lock);
    u64 lt0 = armGetSystemTick();
    size_t got = cache_read_upto(tfs, abs, buf, (size_t)can_read);
    // In RAM mode cache_read_upto is a plain memcpy from the window, not an SD
    // syscall: recording it as an "sd rd" probe made the ZR panel's read counter
    // climb on every mpv read and look like disk activity. Only the real,
    // fd-backed read path is a genuine SD access.
    if (!tfs->ram_mode) lat_add(tfs, LAT_RD, lt0);
    mutexUnlock(&tfs->cache_lock);

    // Racy on purpose: diagnostic counter on the mpv thread.
    if ((int64_t)got < can_read) ((torrentfs *)tfs)->st_cache_rd_short++;
    // Short reads are legal for mpv; an error would kill the stream for good.
    return (int64_t)got;
}

void torrentfs_cancel(torrentfs *tfs) {
    if (!tfs) return;  // metadata-only slots have no torrentfs
    tfs->stop = true;
}

//-----------------------------------------------------------------------------
// Debug surface
//-----------------------------------------------------------------------------

void torrentfs_stats(const torrentfs *tfs, int64_t *pieces_done,
                     int64_t *pieces_total, int64_t *playhead_piece) {
    mutexLock((Mutex *)&tfs->lock);
    // pieces_ever, not pieces_done: the latter is the RAM window's current
    // occupancy and falls back as pieces evict, so it made the progress count
    // jump backwards. This one only climbs.
    if (pieces_done) *pieces_done = tfs->pieces_ever;
    if (pieces_total)
        *pieces_total = tfs->file_last_piece - tfs->file_first_piece + 1;
    if (playhead_piece) *playhead_piece = tfs->playhead_piece;
    mutexUnlock((Mutex *)&tfs->lock);
}

int torrentfs_peer_count(const torrentfs *tfs) {
    return tfs->peer_count;
}

int torrentfs_get_peers(const torrentfs *tfs, torrentfs_peer_info *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_SESS && n < max; i++) {
        const sess *s = &tfs->S[i];
        if (!s->active) continue;
        out[n].ip           = s->addr.ip;
        out[n].port         = s->addr.port;
        out[n].bytes_recv   = s->rx_total;
        out[n].rate_bps     = s->rate;
        out[n].rtt_ms       = -1;
        out[n].connecting   = s->connecting;
        out[n].handshaked   = s->nb.handshaked;
        out[n].choked       = s->nb.choked;
        out[n].claim_piece  = s->claim;
        n++;
    }
    return n;
}

void torrentfs_live_peers(const torrentfs *tfs, int *live, int *peak,
                          int *connecting) {
    if (live) *live = tfs->st_live;
    if (peak) *peak = tfs->st_peak_live;
    if (connecting) *connecting = tfs->st_connecting;
}

void torrentfs_claim_stats(const torrentfs *tfs, int *claiming, int *idle) {
    int c = 0, id = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &tfs->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        if (s->claim >= 0) c++;
        else if (!s->nb.choked) id++;
    }
    if (claiming) *claiming = c;
    if (idle) *idle = id;
}

void torrentfs_piece_debug(const torrentfs *tfs, int64_t idx, int *status,
                           int *have, int *req, int *total) {
    torrentfs *t = (torrentfs *)tfs;
    int h = 0, r = 0, nb = 0, st = -1;
    if (idx >= 0 && idx < t->meta.piece_count) {
        st = t->status[idx];
        nb = (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
        aq_entry *a = aq_find(t, idx);   // racy vs the netloop: diagnostics
        if (a) {
            h = a->have_cnt;
            for (int b = 0; b < a->nblocks; b++)
                if (a->req[b] && !a->have[b]) r++;
        }
    }
    if (status) *status = st;
    if (have) *have = h;
    if (req) *req = r;
    if (total) *total = nb;
}

int64_t torrentfs_stored_bytes(const torrentfs *tfs) {
    mutexLock((Mutex *)&tfs->lock);
    int64_t v = tfs->pieces_done * tfs->meta.piece_len +
                tfs->st_blocks_have * BLOCK_LEN;
    mutexUnlock((Mutex *)&tfs->lock);
    return v;
}

void torrentfs_cache_stats(const torrentfs *tfs, int *wr_fail, int *rd_short,
                           int64_t *total_bytes) {
    if (wr_fail) *wr_fail = tfs->st_cache_wr_fail;
    if (rd_short) *rd_short = tfs->st_cache_rd_short;
    if (total_bytes)
        *total_bytes = (tfs->file_last_piece - tfs->file_first_piece + 1) *
                       tfs->meta.piece_len;
}

int64_t torrentfs_cache_written(const torrentfs *tfs) {
    return tfs->st_cache_written;
}

// The mode this torrent actually opened with (latched, so it can differ from
// the global torrentfs_ram_stream() toggle if that was flipped mid-playback).
// In RAM mode torrentfs_cache_written is a RAM-store byte count, not SD writes.
int torrentfs_ram_active(const torrentfs *tfs) {
    return tfs->ram_mode;
}

void torrentfs_set_backlog(torrentfs *tfs, int ms) {
    tfs->backlog_ms = ms;
}

void torrentfs_announce_now(torrentfs *tfs) {
    if (tfs) tfs->announce_now = true;
}

bool torrentfs_set_piece_zone(torrentfs *tfs, int64_t first_piece,
                              int64_t piece_count, int zone) {
    if (first_piece < 0 || piece_count <= 0 ||
        first_piece + piece_count > tfs->meta.piece_count)
        return false;
    // Clamp zone to fit in uint8_t.
    if (zone < 0) zone = 0;
    if (zone > 255) zone = 255;
    mutexLock(&tfs->lock);
    for (int64_t i = 0; i < piece_count; i++)
        tfs->piece_zone[first_piece + i] = (uint8_t)zone;
    mutexUnlock(&tfs->lock);
    return true;
}

bool torrentfs_clear_piece_zones(torrentfs *tfs) {
    if (!tfs->piece_zone) return false;
    mutexLock(&tfs->lock);
    memset(tfs->piece_zone, 0, (size_t)tfs->meta.piece_count);
    mutexUnlock(&tfs->lock);
    return true;
}

int torrentfs_calm(const torrentfs *tfs) {
    return tfs->calm_now;
}

void torrentfs_hb_ui(torrentfs *tfs) {
    hb_beat(tfs, HB_UI);
}

void torrentfs_heartbeats(const torrentfs *tfs, uint32_t age_ms[4],
                          int core[4]) {
    u64 now = armGetSystemTick();
    for (int i = 0; i < 4; i++) {
        u64 tk    = tfs->hb_tick[i];
        age_ms[i] = tk ? (uint32_t)((now - tk) * 1000 / tfs->freq) : 0;
        core[i]   = tfs->hb_core[i];
    }
}

void torrentfs_lat_stats(const torrentfs *tfs, uint32_t count[5],
                         uint64_t max_us[5]) {
    torrentfs *t = (torrentfs *)tfs;
    for (int i = 0; i < 5; i++) {
        count[i]  = t->lat_n[i];
        max_us[i] = t->lat_max[i] * 1000000 / t->freq;
        t->lat_max[i] = 0;   // reading clears the peaks
    }
}

void torrentfs_claim_debug(const torrentfs *tfs, int64_t *ph, int64_t *lo,
                           int64_t *hi, int *fail, int *ok, int *inflight) {
    if (ph) *ph = tfs->st_win_ph;
    if (lo) *lo = tfs->st_win_lo;
    if (hi) *hi = tfs->st_win_hi;
    if (fail) *fail = tfs->st_claim_fail;
    if (ok) *ok = tfs->st_claim_ok;
    if (inflight) {
        int n = 0;
        for (int i = 0; i < tfs->n_aq; i++)
            if (tfs->aq[i].idx >= 0) n++;
        *inflight = n;
    }
}

void torrentfs_bitfield_stats(const torrentfs *tfs, int *empty, int *ok, int *bad) {
    // st_bf_empty is computed by the netloop's upkeep tick: walking the live
    // bitfields here (UI thread) would race peer_nb_free.
    if (empty) *empty = tfs->st_bf_empty;
    if (ok) *ok = tfs->st_bf_ok;
    if (bad) *bad = tfs->st_bf_bad;
}

void torrentfs_fail_kinds(const torrentfs *tfs, int *sock_fail, int *timeouts) {
    if (sock_fail) *sock_fail = tfs->st_sock_fail;
    if (timeouts) *timeouts = tfs->st_conn_timeout;
}

void torrentfs_debug_counts(const torrentfs *tfs, int out[10]) {
    out[0] = tfs->st_conn_ok;
    out[1] = tfs->st_conn_fail;
    out[2] = tfs->st_unchoke_ok;
    out[3] = tfs->st_choked;
    out[4] = tfs->st_piece_ok;
    out[5] = tfs->st_fetch_fail;
    out[6] = tfs->st_sha_fail;
    out[7] = 0;   // blocks_served: v3 is leech-only
    out[8] = tfs->st_interested_recv;
    out[9] = tfs->st_request_recv;
}

int64_t torrentfs_piece_len(const torrentfs *tfs) {
    return tfs->meta.piece_len;
}

void torrentfs_crit(const torrentfs *tfs, int *head, int *tail) {
    if (head) *head = tfs->crit_head;
    if (tail) *tail = tfs->crit_tail;
}

int torrentfs_active_pieces(const torrentfs *tfs, int64_t *idx, int *have,
                            int *total, int max) {
    torrentfs *t = (torrentfs *)tfs;
    int n = 0;
    mutexLock(&t->lock);
    for (int i = 0; i < t->n_aq && n < max; i++) {
        aq_entry *a = &t->aq[i];
        if (a->idx < 0) continue;           // free slot
        if (idx)   idx[n]   = a->idx;
        if (have)  have[n]  = a->have_cnt;
        if (total) total[n] = a->nblocks;
        n++;
    }
    mutexUnlock(&t->lock);
    return n;
}

const char *torrentfs_name(const torrentfs *tfs) {
    return tfs->meta.name[0] ? tfs->meta.name : "torrent";
}

int torrentfs_piece_done(const torrentfs *tfs, int64_t idx) {
    if (idx < 0 || idx >= tfs->meta.piece_count) return 0;
    return tfs->status[idx] == PIECE_DONE;
}

int torrentfs_incoming_count(const torrentfs *tfs) {
    (void)tfs;
    return 0;   // leech-only: no listen socket
}

int64_t torrentfs_bytes_recv(const torrentfs *tfs) {
    return tfs->st_bytes_recv;
}

void torrentfs_last_err(const torrentfs *tfs, char *buf, size_t len) {
    snprintf(buf, len, "%s", tfs->st_last_err);
}
