// torrentfs3.c вЂ” v3 of the streaming torrent engine (same public API as v1/v2).
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
//   - per-session adaptive request pipeline (~2 s of the peer's rate in flight)
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
#ifdef __SWITCH__
#include <switch/crypto/sha1.h>
#else
#include <mbedtls/sha1.h>
#endif

#include "torrent_meta.h"
#include "bt_peer.h"      // MSG_* ids, BLOCK_LEN, bf_has_piece, peer_nb
#include "dhtclient.h"
#include "utp_nb.h"
#include "listen.h"
#include "engine_log.h"

//-----------------------------------------------------------------------------
// Tuning
//-----------------------------------------------------------------------------

#ifdef __SWITCH__
/* Live sessions. TCP sockets are non-blocking and don't touch the 16-session
   blocking-BSD budget; memory is ~40 KB per TCP session plus ~1.25 MB per
   uTP session, ~25 MB worst case -- fine in title mode. On a 100 Mbit wifi
   link a few dozen 500-800 KB/s peers are what saturates it, so the cap sits
   above the typical 20-25 reachable peers with headroom for churn. */
#define MAX_SESS         48
#define MAX_CONNECTING   24     // slots allowed to sit in a pending connect
#define DIAL_STOP_LIVE   40     // enough live sessions: stop dialing new peers
#else
// PC can hold more concurrent BSD sockets; use them to stress-test the engine.
#define MAX_SESS         64
#define MAX_CONNECTING   32
#define DIAL_STOP_LIVE   48
#endif
#define CONNECT_SECS     2      // outbound SYN patience
// When the swarm is starved the old code dropped the connect budget to 1 s to
// churn through dead DHT addresses faster. That is self-defeating: slow-but-
// alive seeders (home NAT, long SYN queues, high RTT) cannot complete TCP in
// 1 s, so the starved state sustained itself. The floor is now 3 s; a peer
// whose previous failure was a connect timeout gets 5 s on retry, which gives
// exactly those slow seeds a real chance. The dial-rate cap below keeps the
// churn (and the home router's NAT table) bounded instead.
#define CONNECT_STARVED_SECS 3
#define CONNECT_RETRY_SECS  5
// New outbound dials per 250 ms upkeep tick (~8-12/s): a SYN storm of failed
// connects accumulates half-open entries in the home router's NAT table and
// starts dropping EVERYTHING, including the healthy sessions.
#define DIAL_BUDGET_PER_TICK 3
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

#define REQ_RING         256    // per-session outstanding-request capacity
#define REQ_EXPIRE_SECS  30     // re-request a block after this long undelivered
#define REQ_FIRST_EXPIRE_SECS 4 // zero-delivery peers: free their blocks fast
// Duplicate (hedge) requests a piece may issue over its whole lifetime: each
// hedge is a deliberate duplicate whose delivery is guaranteed waste unless
// the original requester died first. 32 blocks = 512 KiB of hedge traffic
// per piece.
#define HEDGE_BUDGET     32

// RAM for in-flight piece buffers; bounds how many pieces are open at once. The
// budget is in BYTES: the floor is only there to keep a tiny bit of pipelining,
// NOT to force a piece count -- a 64 MB-piece torrent clamped to 4 buffers held
// 256 MB of them (and 8 look-ahead pieces = 512 MB, more than the RAM window,
// which then evicted what it had just fetched ahead and re-downloaded it).
// 80 MB puts ~9 of the common 8 MB pieces in flight: with ~20 live sessions the
// old 5-buffer pool left most of the swarm idle while one slow piece blocked
// the reader, and the idle sessions capped the aggregate download rate.
#define RAM_BUDGET       (80LL << 20)
#define AQ_MAX           16
#define AQ_MIN           2

// Streaming window: wide enough to feed the sessions, narrow enough that they
// don't advance 50 pieces in parallel while the player starves on one. Byte-
// budgeted, with a small piece floor and (in RAM mode) a ceiling of half the
// RAM window so the look-ahead can never outrun what the window can hold.
// 80 MB (~9 of the common 8 MB pieces) matches the in-flight buffer pool so
// every live session can hold a claim; the installer's 128 MB ring buffer plus
// this window is what absorbs a slow piece without the read blocking on it.
#define STREAM_WINDOW    (80LL * 1024 * 1024)
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

// Peer provenance, kept per pool slot. Tracker/PEX/manual addresses are live
// seeders reported moments ago; DHT addresses are cached (often stale) entries
// from other clients' routing tables. The dialer prefers the former and, when
// the pool is full, DHT entries are the first to be evicted for fresh peers.
enum { PEER_SRC_TRACKER = 0, PEER_SRC_PEX = 1, PEER_SRC_DHT = 2 };
// Failure kinds for peer_fail_kind: 0 = none, 1 = connect timeout (the peer
// might just be slow), 2 = refusal/other (the port really is closed),
// 3 = dropped after the choked-rotation patience ran out (a normal choke cycle
// from a seed, not a dead address: it gets the minimal backoff so we can catch
// the next optimistic-unchoke window).
enum { PEER_FAIL_NONE = 0, PEER_FAIL_TIMEOUT = 1, PEER_FAIL_OTHER = 2,
       PEER_FAIL_CHOKE_ROTATE = 3 };

// Discovery re-runs every 15 min, or 90 s after the last round when starved.
// The 90 s floor matters: trackers (t-ru.org observed) throttle announces to
// a single peer address when a client re-announces every few dozen seconds,
// so starving must not turn into an announce spam that gets us cut off.
#define DISC_INTERVAL_SECS (15 * 60)
#define DISC_STARVED_SECS  90
#define STARVED_LIVE       6

// PEX (BEP 11): exchange peer addresses with live sessions. Peers discovered
// this way are by definition reachable (they come from someone who is talking
// to us right now), which is what makes PEX the antidote to the dead-address
// flood from trackers/DHT. Send every minute, at most 50 addresses per
// message; accept at most one message per session per 5 s (abuse guard).
#define PEX_INTERVAL_SECS        60
#define PEX_MAX_PEERS            50
#define PEX_RX_MIN_INTERVAL_SECS 5

// Upload serving: we answer peers' block requests from the RAM window (v1:
// RAM mode only; an SD read on the netloop would stall it). The per-session
// tx queue is bounded anyway (512 KiB cap), but a burst of requests from a
// fast peer would otherwise queue well past the cap and waste CPU on refused
// appends; refuse new requests once this much is still unsent.
#define UP_TX_WATERMARK  (384 * 1024)

// A handshaked peer that keeps us choked for this long while never having
// delivered a block is dead weight: drop it and dial a fresh candidate (the
// pool has alternatives most of the time; 60 s > the ~30 s optimistic-unchoke
// rotation of a seed that might still come around).
#define CHOKED_ROTATE_SECS 60
// During bootstrap (fewer than this many pieces ever completed) we have nothing
// to reciprocate with yet, so seeds are precious: they optimistic-unchoke us
// every ~30 s, and killing them after 60 s meant we missed their slot and then
// had to wait out the backoff to try again. Give full seeds three times the
// patience so the first optimistic window is always caught.
#define BOOTSTRAP_THRESH              5
#define CHOKED_ROTATE_BOOTSTRAP_SECS 180

// Reciprocation-based choking (BEP-3 tit-for-tat, adapted: ranking is by the
// peer's download rate TO US, not their upload rate from us -- a streaming
// client mostly downloads from seeds that want nothing back, so the peers
// worth rewarding are the ones that serve us). Round every 10 s: unchoke the
// top MAX_UNCHOKE_SLOTS interested peers by download rate, choke the rest, and
// rotate one optimistic slot every OPTIMISTIC_SECS so a new peer can prove
// itself and the swarm sees us as a willing uploader.
#define CHOKE_INTERVAL_SECS 10
#define MAX_UNCHOKE_SLOTS    4
#define OPTIMISTIC_SECS     30

// One poll() call covers at most this many sockets. On cygwin/msys2 poll()
// is built on WaitForMultipleObjects, which waits on at most 64 handles: the
// PC build's MAX_SESS(64) + uTP socket = 65 pollfds could exceed it and the
// call misbehaved (rare full-process hangs on the PC test build). The netloop
// rotates through the session set, so a socket that misses one poll waits at
// most one extra 100 ms loop. Switch builds (48 + 1) never hit the cap.
#define POLL_CHUNK_MAX   56

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
    uint8_t *req;       // one byte per block: outstanding-request COUNT
    int nblocks;
    int have_cnt;
    int next_req;       // scan cursor for the next block to request
    int hedge_budget;   // duplicate requests this piece may still ISSUE
} aq_entry;

typedef struct {
    bool active;
    bool connecting;
    bool utp_retry;     // this dial is a one-shot uTP retry after a TCP fail
    peer_nb nb;         // sock/framing/handshake/bitfield (peer.c)
    peer_addr addr;     // remote endpoint
    int pidx;           // peer pool index (for backoff bookkeeping)
    int64_t claim;      // piece being fetched, -1 = none
    // Outstanding block requests, as a ring of (piece, block, time). A PIECE
    // is only accepted if it exactly matches an entry here -- piece AND block,
    // because a late delivery for a previous claim shares block numbers with
    // the new one and would otherwise eat the new claim's entries (that was
    // silently dropping every fresh block of the new piece, freezing the
    // stream). Expired entries free their per-piece request counts so other
    // peers can re-request the block (the pipensx model: request counts +
    // exact matching, no duplicate storms).
    int req_n;
    int req_piece[REQ_RING];
    int req_block[REQ_RING];
    u64 req_time[REQ_RING];
    u64 started, last_rx, last_block, last_ka;
    u64 conn_budget;     // dial patience for THIS attempt (ticks, 0 = default)
    int fail_kind;       // why this session failed, for pool bookkeeping
    u64 last_pex;      // last ut_pex we SENT (0 = never)
    u64 last_pex_rx;   // last ut_pex we ACCEPTED (rate-limit guard)
    // Per-session receive-rate meter, for the relative slow-peer cull.
    int64_t rx_total;   // cumulative bytes received (never reset)
    int64_t rate_prev;  // rx_total at the last rate sample
    u64 rate_tick;      // last rate sample (0 = meter not started yet)
    u64 rate_start;     // first sample: start of the measure grace
    double rate;        // smoothed receive rate, bytes/s
    // Per-session upload meter, for the choking round's reciprocity ranking.
    int64_t tx_total;   // cumulative bytes we sent to this peer
    int64_t up_rate_prev;   // tx_total at the last up-rate sample
    u64 up_rate_tick;       // last up-rate sample (0 = meter not started yet)
    double  up_rate;        // smoothed upload rate to this peer, bytes/s
    bool peer_interested;  // peer wants our pieces (we may unchoke)
} sess;

static void release_claim(torrentfs *t, sess *s);
static aq_entry *aq_find(torrentfs *t, int64_t idx);

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
    int64_t read_blocked_piece;// piece the reader thread is waiting on, -1 = none
    volatile bool stop;

    aq_entry aq[AQ_MAX];
    int n_aq;

    wjob wq[AQ_MAX + 2];
    int wq_head, wq_n;

    sess S[MAX_SESS];

    // Incoming connections accepted by the shared engine listener and routed
    // here by info_hash. The netloop drains this queue every upkeep tick.
    struct torrentfs_incoming_q inc_q;

    // Peer pool + backoff.
    peer_addr peers[TFS_MAX_PEERS];
    uint8_t peer_fails[TFS_MAX_PEERS];
    uint8_t peer_busy[TFS_MAX_PEERS];
    uint8_t peer_ok[TFS_MAX_PEERS];      // 1 if peer ever delivered a MSG_PIECE block
    uint8_t peer_utp_retry[TFS_MAX_PEERS];  // TCP failed: try uTP once
    uint8_t peer_mse_off[TFS_MAX_PEERS];    // peer proved not MSE-capable: plaintext only
    uint8_t peer_src[TFS_MAX_PEERS];     // where the address came from (PEER_SRC_*)
    uint8_t peer_fail_kind[TFS_MAX_PEERS];  // last failure: 1 = connect timeout, 2 = other
    u64 peer_next_try[TFS_MAX_PEERS];
    int peer_count;
    int next_peer;

    // Starvation recovery state (escalating rounds).
    u64 last_starve;
    int starve_round;
    int st_starve_rounds;
    int st_peer_evicted;

    uint8_t peer_id[20];

    Thread netloop, writer, discovery;
    bool netloop_started, writer_started, discovery_started;

    u64 freq;

    // Calm / governor.
    int backlog_ms;            // written by the app thread, read racily
    int calm_now;
    volatile bool announce_now;
    volatile int paused;       // pause: stop dialing + claiming (sessions stay)

    // Transport alternation: 0 = try TCP next, 1 = try ВµTP next.
    int dial_toggle;
    int poll_cursor;      // next session index for the bounded poll() rotation
    double rate_bps;      // netloop-only EWMA
    int64_t rate_last_bytes;
    u64 rate_last_tick;
    u64 last_progress_tick;    // last time a block arrived or a piece finished

    // Reciprocation choking state (netloop-private).
    u64  last_choke;        // last choking_round tick
    u64  last_optimistic;   // last optimistic-slot rotation tick
    sess *optimistic_peer;  // current optimistic unchoke target

    // Debug surface (racy reads by the UI are fine; single-writer counters).
    int st_conn_ok, st_conn_fail, st_sock_fail, st_conn_timeout;
    int st_unchoke_ok, st_choked;
    int st_piece_ok, st_fetch_fail, st_sha_fail;
    int st_hs_fail;   // TCP connected but the peer rejected/wrong handshake
    int st_interested_recv, st_request_recv;
    int st_up_unchoke;   // times we unchoked an interested peer
    int64_t st_up_blocks;   // blocks served
    int64_t st_up_bytes;    // bytes served
    int st_bf_empty, st_bf_ok, st_bf_bad;
    int st_live, st_peak_live, st_connecting;
    int st_claim_ok, st_claim_fail;
    int st_cache_wr_fail, st_cache_rd_short;
    int64_t st_cache_written;
    int64_t st_bytes_recv;
    int64_t st_dup_bytes;      // MSG_PIECE payloads for blocks we already had
    int64_t st_dup_noring;     //   of which: no matching outstanding request
    int64_t st_dup_have;       //   of which: ring matched but block already held
    int64_t st_dup_gone;       //   of which: assembly slot gone (piece finished)
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

// Copy up to len contiguous, already-arrived bytes of an ACTIVE assembly piece,
// starting at `within` (block-aligned). The caller holds cache_lock; takes
// t->lock so the slot cannot be recycled (or its buffer handed to the writer)
// under the copy. Returns bytes copied (may be 0); stops at the first missing
// Copy up to len contiguous, already-arrived bytes of an ACTIVE assembly piece,
// starting at `within` (supports arbitrary byte offset into the first block).
// The caller holds cache_lock; takes t->lock so the slot cannot be recycled
// under the copy. Returns bytes copied (may be 0); stops at the first missing
// block or the piece end. Only used in RAM mode.
static size_t aq_piece_read(torrentfs *t, int64_t idx, int64_t within,
                            uint8_t *dst, size_t len) {
    size_t total = 0;
    mutexLock(&t->lock);
    aq_entry *a = aq_find(t, idx);
    if (!a || a->idx != idx) {
        mutexUnlock(&t->lock);
        return 0;
    }
    int64_t plen = torrent_piece_len(&t->meta, idx);
    int b = (int)(within / BLOCK_LEN);
    int64_t b_offset = within % BLOCK_LEN;
    while (b < a->nblocks && total < len && within + (int64_t)total < plen) {
        if (!__atomic_load_n(&a->have[b], __ATOMIC_ACQUIRE)) break;
        uint32_t bl = block_len_of(plen, b);
        if (b_offset >= bl) break;
        size_t avail_in_block = bl - (size_t)b_offset;
        size_t take = avail_in_block;
        if (take > len - total) take = len - total;
        memcpy(dst + total, a->buf + (int64_t)b * BLOCK_LEN + b_offset, take);
        total += take;
        b_offset = 0;
        b++;
    }
    mutexUnlock(&t->lock);
    return total;
}

// Read as much as is contiguously there; the installer path must never
// hard-fail. DONE pieces come from the RAM window (or the SD cache in cache
// mode); in RAM mode an ACTIVE piece can additionally stream its already-
// arrived blocks, so the consumer no longer waits for a whole 8 MB piece to
// verify before draining it -- data flows at block granularity as the network
// delivers it, which is what turns the old piece-sized bursts into a smooth
// stream.
static size_t cache_read_upto(torrentfs *t, int64_t off, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t total = 0;
    int64_t plen = t->meta.piece_len;
    while (len > 0) {
        int64_t idx    = off / plen;
        int64_t within = off % plen;
        size_t n       = len;
        if (within + (int64_t)n > plen) n = (size_t)(plen - within);
        if (cache_piece_read(t, idx, within, p, n)) {
            total += n;
            p += n; off += (int64_t)n; len -= n;
            continue;
        }
        if (!t->ram_mode) break;
        size_t m = aq_piece_read(t, idx, within, p, n);
        if (m == 0) break;
        total += m;
        if (within + (int64_t)m < plen) break;   // gap inside the piece: stop
        // The piece's tail is complete: the next piece may continue the run.
        p += m; off += (int64_t)m; len -= m;
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
// The bitmap reset ALSO runs under the lock: the streaming reader (RAM mode)
// reads have[] of an active piece, and a reset visible mid-assignment would
// hand it stale blocks.
static aq_entry *aq_alloc(torrentfs *t, int64_t idx) {
    aq_entry *a = NULL;
    mutexLock(&t->lock);
    for (int i = 0; i < t->n_aq; i++) {
        aq_entry *c = &t->aq[i];
        if (c->idx >= 0) continue;
        if (!c->buf) c->buf = malloc((size_t)t->meta.piece_len);
        if (!c->buf) continue;
        c->idx = idx;
        c->owner    = -1;
        c->workers  = 0;
        c->have_cnt = 0;
        c->nblocks  =
            (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
        int hb = c->nblocks / 16;
        if (hb < HEDGE_BUDGET) hb = HEDGE_BUDGET;
        if (hb > 64) hb = 64;
        c->hedge_budget = hb;
        memset(c->have, 0, (size_t)t->blocks_per_piece);
        memset(c->req, 0, (size_t)t->blocks_per_piece);
        a = c;
        break;
    }
    mutexUnlock(&t->lock);
    return a;
}

// Park: keep buffer and progress, forget who was fetching it. The session
// leaving already freed its per-piece request counts (release_claim), so
// in-flight blocks of its own are re-requestable by the adopter; blocks still
// on order from other sessions keep their counts until they arrive or expire.
static void aq_park(torrentfs *t, aq_entry *a) {
    (void)t;
    a->owner    = -1;
    a->workers  = 0;
}

//-----------------------------------------------------------------------------
// Peer pool (shared with the discovery thread -> under t->lock)
//-----------------------------------------------------------------------------

static void add_peers_src(torrentfs *t, const peer_addr *peers, int n, int src) {
    int added = 0;
    mutexLock(&t->lock);
    for (int i = 0; i < n; i++) {
        // Port 0/1 are never real listen ports: some DHT nodes store (and
        // happily serve) announce_peer entries with port 1 because jech/dht
        // only rejects port 0. Dialing them just burns the dial budget.
        if (peers[i].port <= 1) continue;
        bool dup = false;
        for (int j = 0; j < t->peer_count; j++)
            if (t->peers[j].ip == peers[i].ip &&
                t->peers[j].port == peers[i].port) {
                // Upgrade provenance: a DHT-cached address re-reported by a
                // live tracker/PEX peer is far more likely to be alive. Lower
                // enum values are better sources, so PEX never demotes a
                // tracker-reported entry.
                if ((uint8_t)src < t->peer_src[j]) t->peer_src[j] = (uint8_t)src;
                dup = true;
                break;
            }
        if (dup) continue;
        int slot = -1;
        if (t->peer_count < TFS_MAX_PEERS) {
            slot = t->peer_count++;
        } else {
            // Pool full: evict a failed DHT-sourced entry first (it is the
            // most stale class of address); only fall back to any failed entry
            // when the pool holds no DHT junk. Never evict healthy/untried
            // entries -- they cost nothing to keep.
            int worst = -1;
            for (int pass = 0; pass < 2 && worst < 0; pass++) {
                for (int j = 0; j < TFS_MAX_PEERS; j++) {
                    if (t->peer_busy[j] || t->peer_fails[j] == 0) continue;
                    if (pass == 0 && t->peer_src[j] != PEER_SRC_DHT) continue;
                    if (worst < 0 || t->peer_fails[j] > t->peer_fails[worst])
                        worst = j;
                }
            }
            slot = worst;
        }
        if (slot < 0) continue;
        t->peers[slot]         = peers[i];
        t->peer_fails[slot]    = 0;
        t->peer_fail_kind[slot]= PEER_FAIL_NONE;
        t->peer_next_try[slot] = 0;
        t->peer_ok[slot]       = 0;
        t->peer_src[slot]      = (uint8_t)src;
        added++;
    }
    mutexUnlock(&t->lock);
    // Log outside the lock: engine_log can block (stderr pipe on PC builds)
    // and a lock held across a blocking write freezes every thread that
    // needs it.
    if (added > 0)
        engine_log(ENGINE_LOG_INFO, "[torrentfs] added %d peers (total %d)", added, t->peer_count);
}

static void add_peers_tracker_cb(void *ctx, const peer_addr *peers, int n) {
    add_peers_src(ctx, peers, n, PEER_SRC_TRACKER);
}

static void add_peers_dht_cb(void *ctx, const peer_addr *peers, int n) {
    add_peers_src(ctx, peers, n, PEER_SRC_DHT);
}

// Take a pool peer (round-robin from the ready set). Tracker/PEX/manual
// addresses get the first pass: they were reported live moments ago, while a
// DHT address is somebody's 30-minute-old cache entry. DHT peers are only
// dialed when nothing better is ready, so the DHT flood can no longer starve
// the few dozen real seeds of dialing slots.
static int take_peer(torrentfs *t, peer_addr *out) {
    u64 now = armGetSystemTick();
    mutexLock(&t->lock);
    int idx = -1;
    if (!t->stop && t->peer_count > 0) {
        for (int pass = 0; pass < 2 && idx < 0; pass++) {
            for (int tries = 0; tries < t->peer_count; tries++) {
                int i = t->next_peer++ % t->peer_count;
                if (t->peer_busy[i]) continue;
                if (t->peer_next_try[i] > now) continue;
                if (pass == 0 && t->peer_src[i] == PEER_SRC_DHT) continue;
                idx = i;
                *out = t->peers[i];
                break;
            }
        }
    }
    if (idx >= 0) t->peer_busy[idx] = 1;
    mutexUnlock(&t->lock);
    return idx;
}

static void release_peer(torrentfs *t, int pidx, bool failed, bool had_conn,
                         int fail_kind) {
    if (pidx < 0) return;
    mutexLock(&t->lock);
    t->peer_busy[pidx] = 0;
    if (failed || !t->peer_ok[pidx]) {
        if (t->peer_fails[pidx] < 0xFE) t->peer_fails[pidx]++;
        t->peer_fail_kind[pidx] = (uint8_t)fail_kind;
        int f = t->peer_fails[pidx];
        int secs;
        if (fail_kind == PEER_FAIL_CHOKE_ROTATE) {
            // A seed that rotated us out is not a dead address: the minimal
            // backoff gets us back into its next optimistic-unchoke window.
            secs = BACKOFF_DROP_SECS;
        } else if (t->pieces_ever < BOOTSTRAP_THRESH &&
                   t->peer_src[pidx] == PEER_SRC_TRACKER) {
            // During bootstrap, tracker-reported addresses are live seeders:
            // keep the reconnect cadence flat so a missed optimistic slot is
            // retried before the next one passes.
            secs = BACKOFF_DROP_SECS;
        } else {
            secs = had_conn ? BACKOFF_DROP_SECS
                            : BACKOFF_CONN_SECS * (1 << (f > 5 ? 5 : f - 1));
        }
        if (secs > BACKOFF_MAX_SECS) secs = BACKOFF_MAX_SECS;
        t->peer_next_try[pidx] = armGetSystemTick() + (u64)secs * t->freq;
    } else {
        t->peer_fails[pidx]     = 0;
        t->peer_fail_kind[pidx] = PEER_FAIL_NONE;
        t->peer_next_try[pidx] = armGetSystemTick() + (u64)BACKOFF_DROP_SECS * t->freq;
    }
    mutexUnlock(&t->lock);
}

// Is this peer a seed (its bitfield covers every piece of the streamed file)?
// Used to grant full seeds extra patience during bootstrap, when we have
// nothing to reciprocate with and must wait out their optimistic-unchoke cycle.
static bool peer_is_seed(torrentfs *t, sess *s) {
    if (!s->nb.bitfield) return false;
    for (int64_t p = t->file_first_piece; p <= t->file_last_piece; p++) {
        if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, p))
            return false;
    }
    return true;
}

// Take a peer flagged for the one-shot uTP hole-punch retry (TCP dial failed).
static int take_utp_retry_peer(torrentfs *t, peer_addr *out) {
    u64 now = armGetSystemTick();
    int idx = -1;
    mutexLock(&t->lock);
    if (!t->stop) {
        for (int tries = 0; tries < t->peer_count; tries++) {
            int i = t->next_peer++ % t->peer_count;
            if (!t->peer_utp_retry[i]) continue;
            if (t->peer_busy[i]) continue;
            if (t->peer_next_try[i] > now) continue;
            idx = i;
            *out = t->peers[i];
            t->peer_busy[i] = 1;
            t->peer_utp_retry[i] = 0;   // one attempt, then normal backoff
            break;
        }
    }
    mutexUnlock(&t->lock);
    return idx;
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
        tsnx_engine_wd_tick(1);
        engine_log(ENGINE_LOG_INFO, "[discovery] tracker announce start");
        torrent_announce_cb(&t->meta, add_peers_tracker_cb, t, &t->stop, e, sizeof(e));
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
        if (t->pieces_ever == BOOTSTRAP_THRESH) {
            // Bootstrap over: we finally have pieces to reciprocate with, so
            // the aggressive bootstrap behavior winds down from here.
            engine_log(ENGINE_LOG_INFO,
                       "[torrentfs] bootstrap complete: %lld pieces verified, can reciprocate",
                       (long long)t->pieces_ever);
        }
    }
}

static void writer_main(void *arg) {
    torrentfs *t = arg;
    for (;;) {
        tsnx_engine_wd_tick(5);
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
#ifdef __SWITCH__
        // ARMv8 CE hardware SHA-1 (libnx crypto): ~10x faster than the
        // software mbedTLS path for the same bytes, at zero engine CPU.
        sha1CalculateHash(hash, j.buf, (size_t)j.plen);
#else
        mbedtls_sha1(j.buf, (size_t)j.plen, hash);
#endif
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
    if (s == t->optimistic_peer) t->optimistic_peer = NULL;
    bool had_conn = s->nb.handshaked;
    // A TCP dial that never connected may have died to a strict NAT: before
    // the backoff retires the address, flag it for one uTP hole-punch
    // attempt (uTP can traverse NAT where TCP SYN is dropped). uTP sessions
    // (nb.utp != NULL) never re-flag, so a peer cannot loop.
    bool was_tcp = (s->nb.utp == NULL);
    if (s->connecting) {
        // still a raw socket; peer_nb was never attached
        if (s->nb.sock >= 0) close(s->nb.sock);
        s->nb.sock = -1;
        // a uTP dial in flight also occupies a utp_nb session slot; without
        // this the 128-slot table leaks and all further uTP dials fail
        if (s->nb.utp) {
            utp_nb_close((utp_nb_sess *)s->nb.utp);
            s->nb.utp = NULL;
        }
        t->st_connecting--;
    } else {
        if (s->nb.handshaked) t->st_live--;
        peer_nb_free(&s->nb);
    }
    int kind = s->fail_kind ? s->fail_kind
                            : (failed ? PEER_FAIL_OTHER : PEER_FAIL_NONE);
    release_peer(t, s->pidx, failed, had_conn, kind);
    // A plaintext handshake that actually connected is proof the peer does
    // not need MSE; remember it so later dials skip the (wasted) attempt.
    if (had_conn && !s->nb.mse_want && s->pidx >= 0) {
        mutexLock(&t->lock);
        t->peer_mse_off[s->pidx] = 1;
        mutexUnlock(&t->lock);
    }
    // MSE refused (or answered plaintext): the peer does not speak
    // encryption. Flag the address so the next dial goes plaintext, and
    // retry immediately -- this is a protocol mismatch, not a network
    // failure, so it must not count into the backoff. A peer that closes
    // the stream before the MSE handshake finished counts as refused too.
    if (failed && !had_conn && s->nb.mse_want &&
        (s->nb.mse_failed || !s->nb.mse_active)) {
        mutexLock(&t->lock);
        t->peer_mse_off[s->pidx] = 1;
        t->peer_fails[s->pidx]   = 0;
        t->peer_next_try[s->pidx] = armGetSystemTick();
        mutexUnlock(&t->lock);
        engine_log(ENGINE_LOG_DEBUG,
                   "[sess %d] mse refused by %u.%u.%u.%u -> retry plaintext",
                   (int)(s - t->S), s->addr.ip & 0xff, (s->addr.ip >> 8) & 0xff,
                   (s->addr.ip >> 16) & 0xff, (s->addr.ip >> 24) & 0xff);
    }
    if (failed && !had_conn && was_tcp && s->pidx >= 0 &&
        utp_nb_fd() >= 0) {
        mutexLock(&t->lock);
        t->peer_utp_retry[s->pidx] = 1;
        t->peer_next_try[s->pidx] = armGetSystemTick();  // dial on next wave
        mutexUnlock(&t->lock);
    }
    s->active = false;
    s->connecting = false;
    s->req_n = 0;
}

// Start a non-blocking TCP dial. Returns false when no session/peer slot.
// `conn_secs` is the base patience for this attempt; a peer whose last
// failure was a connect timeout gets CONNECT_RETRY_SECS instead (slow seeders
// behind NAT need more than the starved floor to complete the SYN handshake).
static bool sess_dial(torrentfs *t, u64 now, int conn_secs) {
    int sid = -1;
    for (int i = 0; i < MAX_SESS; i++)
        if (!t->S[i].active) { sid = i; break; }
    if (sid < 0) return false;

    peer_addr pa;
    int pidx = take_peer(t, &pa);
    if (pidx < 0) return false;

    int secs = conn_secs;
    mutexLock(&t->lock);
    if (t->peer_fail_kind[pidx] == PEER_FAIL_TIMEOUT &&
        t->peer_fails[pidx] <= 3)
        secs = CONNECT_RETRY_SECS;
    mutexUnlock(&t->lock);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        t->st_sock_fail++;
        t->st_conn_fail++;
        set_err(t, "socket(): errno %lld", (long long)errno);
        release_peer(t, pidx, true, false, PEER_FAIL_OTHER);
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
        release_peer(t, pidx, true, false, PEER_FAIL_OTHER);
        return true;   // slot still free; try another peer next tick
    }

    sess *s = &t->S[sid];
    memset(s, 0, sizeof(*s));
    s->active      = true;
    s->connecting  = true;
    s->nb.sock     = sock;   // raw until the connect completes
    s->addr        = pa;
    s->pidx        = pidx;
    s->claim       = -1;
    s->started     = now;
    s->conn_budget = now + (u64)secs * t->freq;
    t->st_connecting++;
    return true;
}

// Start a non-blocking µTP dial of a specific pool peer (already taken).
static bool sess_dial_utp_at(torrentfs *t, u64 now, int pidx, peer_addr pa) {
    int sid = -1;
    for (int i = 0; i < MAX_SESS; i++)
        if (!t->S[i].active) { sid = i; break; }
    if (sid < 0) {
        release_peer(t, pidx, false, false, PEER_FAIL_NONE);
        return false;
    }

    engine_log(ENGINE_LOG_DEBUG, "[sess %d] dial utp %u.%u.%u.%u:%d",
               sid, pa.ip & 0xff, (pa.ip >> 8) & 0xff,
               (pa.ip >> 16) & 0xff, (pa.ip >> 24) & 0xff, pa.port);
    utp_nb_sess *us = utp_nb_connect(pa.ip, pa.port);
    if (!us) {
        engine_log(ENGINE_LOG_DEBUG, "[sess %d] utp connect failed", sid);
        release_peer(t, pidx, true, false, PEER_FAIL_OTHER);
        return true;
    }

    sess *s = &t->S[sid];
    memset(s, 0, sizeof(*s));
    s->active      = true;
    s->connecting  = true;
    s->nb.sock     = -1;
    s->nb.utp      = us;
    s->addr        = pa;
    s->pidx        = pidx;
    s->claim       = -1;
    s->started     = now;
    s->last_rx     = now;
    s->last_ka     = now;
    s->conn_budget = now + (u64)10 * t->freq;  // uTP SYN exchange patience

    if (peer_nb_init_utp(&s->nb, us, t->meta.piece_count) != 0) {
        utp_nb_close(us);
        release_peer(t, pidx, true, false, PEER_FAIL_OTHER);
        return true;
    }
    mutexLock(&t->lock);
    s->nb.mse_want = 0;   // MSE disabled: see sess_connected
    mutexUnlock(&t->lock);
    peer_nb_send_handshake(&s->nb, t->meta.info_hash, t->peer_id);
    peer_nb_flush(&s->nb);
    t->st_connecting++;
    return true;
}

// Start a non-blocking µTP dial.
static bool sess_dial_utp(torrentfs *t, u64 now) {
    peer_addr pa;
    int pidx = take_peer(t, &pa);
    if (pidx < 0) return false;
    return sess_dial_utp_at(t, now, pidx, pa);
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
    mutexLock(&t->lock);
    // MSE/PE disabled (measured verdict): our implementation was verified
    // against libtorrent's pe_crypto (keys, DH prime, pe1-pe4 framing all
    // match) and two real bugs were fixed (padB-aware VC resync in
    // mse_peer.c, duplicate BT-handshake in bt_peer.c), but in practice MSE
    // is a net loss on this swarm: nearly every MSE attempt is answered by a
    // plaintext-only peer that closes on our pubA, the failed session burns
    // the dial slot, and the plaintext retry then works anyway. A/B on the
    // same torrent: MSE off = 66 KB/s, MSE on = 39 KB/s. Peers that require
    // encryption decline us; that is their call. Re-enable by setting
    // mse_want = !peer_mse_off[...] here and in sess_dial_utp_at.
    s->nb.mse_want = 0;
    mutexUnlock(&t->lock);
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
        if (cap < STREAM_MIN_PIECES) cap = STREAM_MIN_PIECES;
        if (win > cap) win = cap;
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

// Cancel a session's outstanding block requests on the wire. The stragglers
// of a released/detached claim are pure waste: every block they deliver has
// already been delivered by someone else and lands as duplicate traffic
// (measured: the single biggest dup source, ~1/3 of all received bytes).
// MSG_CANCEL stops the peer from sending them in the first place.
static void cancel_outstanding(torrentfs *t, sess *s) {
    if (s->req_n <= 0 || !s->nb.handshaked || s->connecting) return;
    for (int i = 0; i < s->req_n; i++) {
        int ep = s->req_piece[i];
        int eb = s->req_block[i];
        if (ep < 0 || ep >= t->meta.piece_count) continue;
        uint8_t pl[12];
        uint32_t v;
        v = htonl((uint32_t)ep);        memcpy(pl, &v, 4);
        v = htonl((uint32_t)eb * BLOCK_LEN); memcpy(pl + 4, &v, 4);
        v = htonl(block_len_of(torrent_piece_len(&t->meta, ep), eb));
        memcpy(pl + 8, &v, 4);
        peer_nb_queue(&s->nb, MSG_CANCEL, pl, 12);
    }
    peer_nb_flush(&s->nb);
}

// Release one session's claim on a piece. Its outstanding request counts are
// freed so other peers can re-request those blocks (only genuinely abandoned
// requests are re-requested -- the per-session ring + counts are what stops
// the duplicate-request storms). If no workers remain, park the partial
// progress so other peers can adopt it.
static void release_claim(torrentfs *t, sess *s) {
    if (s->claim < 0) return;
    aq_entry *a = aq_find(t, s->claim);
            cancel_outstanding(t, s);
    // Free every outstanding request's per-piece count (looked up by the
    // piece each entry belongs to), then detach.
    for (int i = 0; i < s->req_n; i++) {
        int ep = s->req_piece[i];
        int eb = s->req_block[i];
        aq_entry *ea = (ep == s->claim) ? a : aq_find(t, ep);
        if (ea && eb >= 0 && eb < ea->nblocks && ea->req[eb] > 0)
            ea->req[eb]--;
    }
    if (a) {
        if (a->owner == s - t->S) a->owner = -1;
        if (a->workers > 0) a->workers--;
        if (a->workers == 0) {
            aq_park(t, a);
        } else if (a->owner < 0) {
            // Re-home the piece on a remaining worker so its pipeline stays
            // the deep owner one instead of everyone degrading to helper caps.
            for (int i = 0; i < MAX_SESS; i++)
                if (t->S[i].claim == a->idx) { a->owner = i; break; }
        }
    }
    s->req_n = 0;
    s->claim = -1;
}

// Can this session take piece idx? NEEDED needs a free buffer; an ACTIVE entry
// can be joined by multiple sessions as long as there are blocks left to order.
// endgame=true skips the "no duplicate requests" gate: the caller knows the
// piece gates the reader, so extra workers (even duplicating requests) are
// exactly what unblocks it.
static bool try_claim(torrentfs *t, sess *s, int sid, int64_t idx, bool endgame) {
    if (idx < t->file_first_piece || idx > t->file_last_piece) return false;
    uint8_t st = t->status[idx];
    if (st == PIECE_DONE || st == PIECE_WRITING) return false;
    if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, idx)) {
        if (idx == t->file_first_piece)
            engine_log(ENGINE_LOG_DEBUG, "[claim] sess %d lacks piece %lld",
                       sid, (long long)idx);
        return false;
    }

    aq_entry *a;
    if (st == PIECE_ACTIVE) {
        a = aq_find(t, idx);
        if (!a) return false;
        // Join if there are still blocks nobody has requested. Once only missing
        // blocks remain we stay cooperative: duplicates waste bandwidth, so a
        // second peer joins only when endgame is close (missing blocks <= threshold)
        // or when the reader is blocked on this very piece.
        int eg_thresh = a->nblocks / 16;
        if (eg_thresh < 6) eg_thresh = 6;
        if (eg_thresh > 32) eg_thresh = 32;
        if (!endgame && unreq_count(a) == 0 && missing_count(a) > eg_thresh) return false;
    } else {
        a = aq_alloc(t, idx);
        if (!a) {
            engine_log(ENGINE_LOG_DEBUG, "[claim] sess %d no aq buffer for %lld",
                       sid, (long long)idx);
            return false;
        }
        mutexLock(&t->lock);
        t->status[idx] = PIECE_ACTIVE;
        mutexUnlock(&t->lock);
    }
    if (a->owner < 0) a->owner = sid;
    a->workers++;
    s->claim = idx;
    s->req_n = 0;
    s->last_block = armGetSystemTick();
    return true;
}

// Claim a piece without knowing that this peer has it.  Used as a last resort
// for the playhead piece: if no currently-handshaked peer reports having it,
// we still want a session to ask the next peer it connects to.  If that peer
// also does not have it, fill_pipeline leaves req_n==0 and the session drops
// the claim on the next service tick.
static bool try_claim_blind(torrentfs *t, sess *s, int sid, int64_t idx) {
    if (idx < t->file_first_piece || idx > t->file_last_piece) return false;
    uint8_t st = t->status[idx];
    if (st == PIECE_DONE || st == PIECE_WRITING) return false;

    aq_entry *a;
    if (st == PIECE_ACTIVE) {
        a = aq_find(t, idx);
        if (!a) return false;
        int eg_thresh = a->nblocks / 16;
        if (eg_thresh < 6) eg_thresh = 6;
        if (eg_thresh > 32) eg_thresh = 32;
        if (unreq_count(a) == 0 && missing_count(a) > eg_thresh) return false;
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
    s->req_n = 0;
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

    // External 5-zone scheduler, clamped to the streaming window: the zones
    // describe priority WITHIN the look-ahead, they must not drag sessions
    // past it. Scanning the whole file here let Prefetch/Speculative claims
    // live outside the window, where the seek-preemption pass then killed
    // and re-made them every upkeep -- a churn that re-downloaded partials,
    // inflated the byte counters, and starved the piece the reader gated on.
    // The behind-playhead (Tail) scan is skipped for the same reason: a
    // sequential installer never reads back, and the preemption pass drops
    // those claims one upkeep later anyway, so each Tail claim was
    // request-discard-request churn (measured: ~87% of received blocks were
    // re-deliveries of that kind).
    for (int zone = 1; zone <= 5; zone++) {
        for (int64_t i = ph; i < hi; i++) {
            if (t->piece_zone[i] == zone && try_claim(t, s, sid, i, false)) {
                t->st_claim_ok++;
                return;
            }
        }
    }

    // Internal fallback.
    if (ph <= lo + t->crit_head) {   // startup: tail (moov) then head
        for (int64_t i = fhi; i > fhi - t->crit_tail && i >= lo; i--)
            if (try_claim(t, s, sid, i, false)) { t->st_claim_ok++; return; }
        for (int64_t i = lo; i < lo + t->crit_head && i <= fhi; i++)
            if (try_claim(t, s, sid, i, false)) { t->st_claim_ok++; return; }
    }
    for (int64_t i = ph; i < hi; i++)
        if (try_claim(t, s, sid, i, false)) { t->st_claim_ok++; return; }
    if (!t->ram_mode)
        for (int64_t i = lo; i < ph; i++)
            if (try_claim(t, s, sid, i, false)) { t->st_claim_ok++; return; }

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
// then scans for holes, and finally allows a duplicate during true endgame
// (<= 4 missing blocks, or the piece the reader is blocked on), BUDGETED:
// the <=4-missing endgame used to let every worker re-request the same last
// blocks without limit -- a fast worker's whole pipeline churned those four
// blocks until the piece closed, and every delivery past the first was a
// duplicate (measured: over half of all dup traffic was this). Two bounds
// now: at most 2 outstanding requests per block (the original plus one
// hedge), and at most HEDGE_BUDGET hedges ISSUED per piece.
static int next_block_to_request(torrentfs *t, aq_entry *a) {
    while (a->next_req < a->nblocks) {
        int b = a->next_req;
        a->next_req++;
        if (!a->req[b] && !a->have[b]) return b;
    }
    for (int b = 0; b < a->nblocks; b++)
        if (!a->req[b] && !a->have[b]) return b;
    // Endgame: duplicate one missing block so a slow peer does not stall us.
    // Budgeted on ISSUED hedges, not delivered ones: gating on delivered dups
    // let every worker hedge a whole pipeline before the counter caught up
    // (up to 96 duplicate blocks in flight per read-blocked piece -- the
    // dominant dup source). Each issued hedge costs one from the budget.
    int eg_thresh = a->nblocks / 16;
    if (eg_thresh < 6) eg_thresh = 6;
    if (eg_thresh > 32) eg_thresh = 32;
    if (missing_count(a) > 0 && a->hedge_budget > 0 &&
        (missing_count(a) <= eg_thresh || a->idx == t->read_blocked_piece)) {
        for (int b = 0; b < a->nblocks; b++)
            if (!a->have[b] && a->req[b] < 2) {
                a->hedge_budget--;
                return b;
            }
    }
    return -1;
}

// Dynamic, per-session pipeline (the pipensx model): keep roughly two seconds
// of this peer's measured delivery rate in flight, so a fast peer on a
// high-latency wifi link carries hundreds of blocks while a trickle peer
// carries few.
#define PROBE_DEPTH_FEW_PEERS 64
static int pipeline_depth(torrentfs *t, sess *s) {
    if (t->st_live <= 3 && s->rate <= 0) return PROBE_DEPTH_FEW_PEERS;
    double r = s->rate > 0 ? s->rate : (t->st_live <= 4 ? 512.0 * 1024.0 : 384.0 * 1024.0);
    // During bootstrap, an unchoked peer has just let us in through its
    // optimistic slot; with no rate history yet, assume it is fast so we fill
    // the whole unchoke window with requests instead of trickling 32 blocks.
    if (t->pieces_ever < BOOTSTRAP_THRESH && s->rate == 0)
        r = 1.0 * 1024.0 * 1024.0;
    int d = (int)(r * 2.0 / BLOCK_LEN);
    if (d < 16) d = 16;
    if (d > REQ_RING) d = REQ_RING;
    return d;
}

// Cap for sessions that join an already-owned piece.
#define HELPER_PIPELINE_CAP 48

// Keep the pipeline full for the session's claimed piece. Every request is
// recorded in the session's ring and bumps the piece's per-block count, so
// in-flight blocks are never blindly re-requested.
static void fill_pipeline(torrentfs *t, sess *s, u64 now) {
    if (s->claim < 0 || s->nb.choked) return;
    aq_entry *a = aq_find(t, s->claim);
    if (!a) { s->claim = -1; return; }
    int64_t plen = torrent_piece_len(&t->meta, s->claim);
    int depth = pipeline_depth(t, s);
    if (s->req_n == 0)
        engine_log(ENGINE_LOG_DEBUG, "[sess %d] fill claim=%lld depth=%d",
                   (int)(s - t->S), (long long)s->claim, depth);
    if (a->workers > 1 && a->owner != s - t->S) {
        int helper_cap = (t->st_live <= 2) ? 64 : (t->st_live <= 4) ? 48 : HELPER_PIPELINE_CAP;
        if (depth > helper_cap) depth = helper_cap;
    }
    while (s->req_n < depth && s->req_n < REQ_RING) {
        int b = next_block_to_request(t, a);
        if (b < 0) break;
        uint8_t pl[12];
        uint32_t v;
        v = htonl((uint32_t)s->claim);        memcpy(pl, &v, 4);
        v = htonl((uint32_t)b * BLOCK_LEN);   memcpy(pl + 4, &v, 4);
        v = htonl(block_len_of(plen, b));     memcpy(pl + 8, &v, 4);
        if (peer_nb_queue(&s->nb, MSG_REQUEST, pl, 12) != 0) break;
        if (a->req[b] < 255) a->req[b]++;
        s->req_piece[s->req_n] = (int)s->claim;
        s->req_block[s->req_n] = b;
        s->req_time[s->req_n]  = now;
        s->req_n++;
    }
}

// All blocks landed: hand the buffer to the writer (which verifies + writes).
static void piece_full(torrentfs *t, aq_entry *a) {
    int64_t idx = a->idx;
    t->last_progress_tick = armGetSystemTick();
    // Announce the piece to every live peer so we look like a well-behaved
    // client (and seeders learn what we hold for later piece exchange).
    uint32_t hv = htonl((uint32_t)idx);
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        peer_nb_queue(&s->nb, MSG_HAVE, &hv, 4);
    }
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
    // Every block landed, so every outstanding request was matched and its
    // per-piece count already decremented; just detach the sessions. Their
    // in-flight requests are now guaranteed duplicates: cancel them on the
    // wire so the peers stop wasting bandwidth on deliveries we no longer
    // need.
    for (int i = 0; i < MAX_SESS; i++) {
        if (t->S[i].claim == idx) {
            cancel_outstanding(t, &t->S[i]);
            t->S[i].claim = -1;
            t->S[i].req_n = 0;
        }
    }
}

//-----------------------------------------------------------------------------
// Message handling
//-----------------------------------------------------------------------------

// Peer exchange (BEP 11): the addresses we share are provably live -- live
// sessions first, then pool entries that never failed -- and the addresses we
// receive come from a live session, which is what makes PEX the antidote to
// the dead-address flood that trackers/DHT hand out. IPv4 only (the engine is).

// Their extended handshake (ext id 0): learn the peer's ut_pex message id.
static void handle_ext_handshake(torrentfs *t, sess *s, uint8_t *pl,
                                 uint32_t plen) {
    (void)t;
    be_node *d = be_parse((const char *)pl, plen);
    if (!d) return;
    be_node *m = be_dict_get(d, "m");
    be_node *u = m ? be_dict_get(m, "ut_pex") : NULL;
    if (u && u->type == BE_INT && u->i > 0 && u->i < 256)
        s->nb.pex_id = (uint8_t)u->i;
    be_free(d);
}

// Their ut_pex message: pull the added addresses into the shared pool.
// Rate-limited per session: a hostile peer could otherwise flood the pool
// with garbage faster than the dial loop can burn through it. The limit
// relaxes while starved (2 s): fresh PEX addresses are the best antidote to
// a dead pool, since they come from a peer that is talking to us right now.
static void handle_pex(torrentfs *t, sess *s, uint8_t *pl, uint32_t plen,
                       u64 now) {
    u64 rx_min = (t->st_live < STARVED_LIVE)
                     ? (u64)2 * t->freq
                     : (u64)PEX_RX_MIN_INTERVAL_SECS * t->freq;
    if (now - s->last_pex_rx < rx_min) return;
    be_node *d = be_parse((const char *)pl, plen);
    if (!d) return;
    be_node *added = be_dict_get(d, "added");
    if (added && added->type == BE_STR) {
        peer_addr buf[PEX_MAX_PEERS];
        int nb = 0;
        for (size_t i = 0; i + 6 <= added->str.len && nb < PEX_MAX_PEERS;
             i += 6) {
            const uint8_t *e = (const uint8_t *)added->str.ptr + i;
            uint32_t ip;
            memcpy(&ip, e, 4);                       // network byte order
            uint16_t port = (uint16_t)((e[4] << 8) | e[5]);
            if (!port) continue;
            buf[nb].ip   = ip;
            buf[nb].port = port;
            nb++;
        }
        if (nb > 0) {
            add_peers_src(t, buf, nb, PEER_SRC_PEX);
            s->last_pex_rx = now;
        }
    }
    be_free(d);
}

static int put_dec(char *buf, int v) {
    if (v <= 0) { buf[0] = '0'; return 1; }
    char tmp[8];
    int t = 0, n = 0;
    while (v > 0) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t > 0) buf[n++] = tmp[--t];
    return n;
}

// Our ut_pex: live-session addresses first (provably alive), then healthy
// pool entries, at most PEX_MAX_PEERS, bencoded as
// d5:added<...>7:added.f<...>7:dropped0:e
//
// Only peers that have PROVEN themselves are advertised: a leecher that
// announces untested DHT addresses within the first second of a handshake
// looks like a PEX spammer, and seeds close the connection over it (measured
// repeatedly against live libtorrent seeds: any ut_pex sent at connect time,
// even empty, got us dropped within ~0.2 s, while the same peer stayed alive
// and uploaded without one).
static void send_pex(torrentfs *t, sess *s, u64 now) {
    uint8_t added[PEX_MAX_PEERS * 6];
    int na = 0;

    // Live sessions first, only ones that actually delivered blocks to us
    // (they are reachable by definition). NEVER list the receiver itself: a
    // peer that gets its own address back in PEX treats it as a protocol
    // violation and closes us.
    for (int i = 0; i < MAX_SESS && na < PEX_MAX_PEERS; i++) {
        sess *o = &t->S[i];
        if (o == s || !o->active || o->connecting || !o->nb.handshaked)
            continue;
        if (o->rx_total <= 0) continue;                // not proven yet
        if (o->addr.port <= 1) continue;               // junk ports
        if ((o->addr.ip & 0xFF) == 127 || (o->addr.ip & 0xFF) == 0)
            continue;                                  // loopback/martian
        memcpy(added + na * 6, &o->addr.ip, 4);    // network byte order
        uint16_t p = htons(o->addr.port);
        memcpy(added + na * 6 + 4, &p, 2);
        na++;
    }
    if (na < PEX_MAX_PEERS) {
        mutexLock(&t->lock);
        for (int i = 0; i < t->peer_count && na < PEX_MAX_PEERS; i++) {
            if (t->peer_busy[i] || t->peer_fails[i] > 0) continue;
            if (!t->peer_ok[i]) continue;              // never delivered: junk
            if (t->peers[i].port <= 1) continue;       // junk ports
            if ((t->peers[i].ip & 0xFF) == 127 || (t->peers[i].ip & 0xFF) == 0)
                continue;                              // loopback/martian
            // Never re-advertise the receiver's own address to it.
            if (t->peers[i].ip == s->addr.ip &&
                t->peers[i].port == s->addr.port)
                continue;
            memcpy(added + na * 6, &t->peers[i].ip, 4);
            uint16_t p = htons(t->peers[i].port);
            memcpy(added + na * 6 + 4, &p, 2);
            na++;
        }
        mutexUnlock(&t->lock);
    }
    if (na <= 0 || na > PEX_MAX_PEERS) return;

    uint8_t msg[1024];
    int off = 0;
    memcpy(msg + off, "d5:added", 8); off += 8;
    off += put_dec((char *)msg + off, na * 6);
    msg[off++] = ':';
    memcpy(msg + off, added, (size_t)na * 6); off += na * 6;
    memcpy(msg + off, "7:added.f", 9); off += 9;
    off += put_dec((char *)msg + off, na);
    msg[off++] = ':';
    memset(msg + off, 0, (size_t)na); off += na;
    memcpy(msg + off, "7:dropped0:e", 12); off += 12;

    if (peer_nb_queue_ext(&s->nb, s->nb.pex_id, msg, (uint32_t)off) == 0) {
        s->last_pex = now;
        peer_nb_flush(&s->nb);
    }
}

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
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] unchoke from %u.%u.%u.%u:%d",
                       (int)(s - t->S), s->addr.ip & 0xff, (s->addr.ip >> 8) & 0xff,
                       (s->addr.ip >> 16) & 0xff, (s->addr.ip >> 24) & 0xff, s->addr.port);
            break;
        case MSG_INTERESTED:
            t->st_interested_recv++;
            s->peer_interested = true;
            // Unchoke decisions are made by choking_round() every 10 s
            // (reciprocation-based: top download-rate peers get the upload
            // slots, everyone else stays choked). Unchoking every interested
            // peer spread our upload so thin that seeds saw no reciprocation
            // and treated us as dead weight. During bootstrap (pieces_done ==
            // 0) there is nothing to serve anyway.
            break;
        case MSG_REQUEST: {
            t->st_request_recv++;
            // Serve blocks we have verified, from the RAM window. v1 scope:
            // RAM mode only (an SD read on the netloop thread would stall it).
            if (!t->ram_mode || plen != 12) break;
            uint32_t r_idx, r_begin, r_len;
            memcpy(&r_idx, pl, 4);     r_idx   = ntohl(r_idx);
            memcpy(&r_begin, pl + 4, 4); r_begin = ntohl(r_begin);
            memcpy(&r_len, pl + 8, 4);  r_len   = ntohl(r_len);
            if (r_idx >= t->meta.piece_count) break;
            if (r_begin % BLOCK_LEN != 0 || r_len == 0 || r_len > BLOCK_LEN) break;
            int64_t p_len = torrent_piece_len(&t->meta, r_idx);
            if ((int64_t)r_begin + r_len > p_len) break;
            if (s->nb.tx_len - s->nb.tx_head > UP_TX_WATERMARK) break;
            // We only serve pieces that are verified AND resident in the RAM
            // window (status under t->lock, buffer under cache_lock: nesting
            // t->lock -> cache_lock is safe, no path takes them the other way).
            uint8_t blk[BLOCK_LEN];
            mutexLock(&t->lock);
            bool ok = t->status[r_idx] == PIECE_DONE;
            mutexUnlock(&t->lock);
            uint8_t *src = NULL;
            if (ok) {
                mutexLock(&t->cache_lock);
                if (t->ram_piece) src = t->ram_piece[r_idx];
                if (src) memcpy(blk, src + r_begin, r_len);
                mutexUnlock(&t->cache_lock);
            }
            if (!src) break;
            {
                uint8_t msg[8 + BLOCK_LEN];
                uint32_t v;
                v = htonl(r_idx);   memcpy(msg, &v, 4);
                v = htonl(r_begin); memcpy(msg + 4, &v, 4);
                memcpy(msg + 8, blk, r_len);
                if (peer_nb_queue(&s->nb, MSG_PIECE, msg, 8 + r_len) == 0) {
                    t->st_up_blocks++;
                    t->st_up_bytes += r_len;
                    s->tx_total += r_len;
                }
            }
            break;
        }
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
                bool full = true;
                for (size_t b = 0; b < plen; b++)
                    if (pl[b]) empty = false;
                for (int64_t p = 0; p < t->meta.piece_count && full; p++)
                    if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, p))
                        full = false;
                engine_log(ENGINE_LOG_INFO,
                           "[sess %d] bitfield ok empty=%d full=%d ip=%u.%u.%u.%u",
                           (int)(s - t->S), empty ? 1 : 0, full ? 1 : 0,
                           s->addr.ip & 0xff, (s->addr.ip >> 8) & 0xff,
                           (s->addr.ip >> 16) & 0xff, (s->addr.ip >> 24) & 0xff);

                bool has_needed = false;
                if (!empty) {
                    for (int64_t p = t->file_first_piece; p <= t->file_last_piece; p++) {
                        if (bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, p)) {
                            has_needed = true;
                            break;
                        }
                    }
                }
                if (!has_needed) {
                    engine_log(ENGINE_LOG_INFO,
                               "[sess %d] close: peer has 0 needed pieces (empty=%d)",
                               (int)(s - t->S), empty ? 1 : 0);
                    if (s->pidx >= 0 && s->pidx < TFS_MAX_PEERS) {
                        t->peer_next_try[s->pidx] = now + (u64)600 * t->freq;
                    }
                    s->fail_kind = PEER_FAIL_CHOKE_ROTATE;
                    sess_close(t, s, true);
                    break;
                }
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
            if (s->pidx >= 0 && s->pidx < TFS_MAX_PEERS)
                t->peer_ok[s->pidx] = 1;
            if (t->starve_round > 0) {
                engine_log(ENGINE_LOG_INFO,
                           "[torrentfs] starvation recovered (delivered block after %d rounds)",
                           t->starve_round);
                t->starve_round = 0;
            }

            // Exact-match against this session's outstanding requests: a
            // delivery for an expired/abandoned request (or a duplicate from
            // another session) never touches storage, so re-request churn
            // cannot flood a piece with re-deliveries.
            int r = -1;
            for (int i = 0; i < s->req_n; i++) {
                if (s->req_piece[i] == (int)idx && s->req_block[i] == b) {
                    r = i;
                    break;
                }
            }
            if (r < 0) { t->st_dup_bytes += dlen; t->st_dup_noring += dlen; break; }
            s->req_piece[r] = s->req_piece[s->req_n - 1];
            s->req_block[r] = s->req_block[s->req_n - 1];
            s->req_time[r]  = s->req_time[s->req_n - 1];
            s->req_n--;

            // Stored by piece, not by owner: a parked piece's stragglers (or
            // an adopted piece's duplicates) still count.
            aq_entry *a = aq_find(t, idx);
            if (!a || b >= a->nblocks || a->have[b]) {
                if (a && b < a->nblocks) {
                    if (a->req[b] > 0) a->req[b]--;
                }
                if (!a) t->st_dup_gone += dlen; else t->st_dup_have += dlen;
                t->st_dup_bytes += dlen;   // delivered twice: wasted traffic
                break;
            }
            memcpy(a->buf + begin, pl + 8, dlen);
            // Release so the streaming reader's acquire load of have[] (RAM
            // mode partial reads) can never observe the flag before the data.
            __atomic_store_n(&a->have[b], 1, __ATOMIC_RELEASE);
            if (a->req[b] > 0) a->req[b]--;
            a->have_cnt++;
            mutexLock(&t->lock);
            t->st_blocks_have++;
            mutexUnlock(&t->lock);
            t->last_progress_tick = now;
            if (a->have_cnt == a->nblocks) piece_full(t, a);
            break;
        }
        case MSG_EXTENDED:
            if (plen < 1 || !s->nb.ext_ok) break;
            if (pl[0] == 0) {
                handle_ext_handshake(t, s, pl + 1, plen - 1);
            } else if (pl[0] == s->nb.pex_id) {
                handle_pex(t, s, pl + 1, plen - 1, now);
            }
            break;
        default:
            break;   // MSG_CANCEL / MSG_NOT_INTERESTED: nothing useful to do
    }
}

// Advertise the pieces we hold (verified, resident) so peers know what they
// can request from us. Always sent, even when empty: BEP-3 lets a client with
// zero pieces omit the bitfield, but in practice full seeders (libtorrent,
// qBittorrent) expect a bitfield -- or at least a HAVE-less handshake -- and
// some of them close a peer that never states its pieces (measured: seeds
// dropped our connection within seconds of the handshake until the empty
// bitfield was sent). v1 does not broadcast HAVE updates afterwards; every new
// connection gets the full picture, which is enough for the reciprocation the
// swarm's choking policy rewards.
static void send_our_bitfield(torrentfs *t, sess *s) {
    size_t n = s->nb.bitfield_len;
    uint8_t *bf = malloc(n);
    if (!bf) return;
    memset(bf, 0, n);
    mutexLock(&t->lock);
    for (int64_t p = 0; p < t->meta.piece_count; p++) {
        if (t->status[p] == PIECE_DONE) {
            bf[p / 8] |= (uint8_t)(0x80 >> (p % 8));
        }
    }
    mutexUnlock(&t->lock);
    if (peer_nb_queue(&s->nb, MSG_BITFIELD, bf, (uint32_t)n) == 0) {
        engine_log(ENGINE_LOG_INFO,
                   "[sess %d] sent our bitfield (%zu bytes)", (int)(s - t->S), n);
    }
    free(bf);
}

static void sess_service(torrentfs *t, sess *s, int sid, u64 now) {
    for (int pump = 0; pump < 4; pump++) {
        ssize_t got = peer_nb_pump_rx(&s->nb);
        if (got < 0) {
            t->st_fetch_fail++;
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] rx error ip=%u.%u.%u.%u errno=%d rxlen=%zu",
                       sid, s->addr.ip & 0xff, (s->addr.ip >> 8) & 0xff,
                       (s->addr.ip >> 16) & 0xff, (s->addr.ip >> 24) & 0xff, errno, s->nb.rx_len);
            sess_close(t, s, true);
            return;
        }
        if (got > 0) s->last_rx = now;

        if (!s->nb.handshaked) {
            int hs = peer_nb_recv_handshake(&s->nb, t->meta.info_hash);
            if (hs < 0) {
                // TCP connected, we sent a plaintext handshake, the peer either
                // closed on us (encryption-required clients reject plaintext) or
                // spoke a different protocol/info_hash.
                t->st_hs_fail++;
                sess_close(t, s, true);
                return;
            }
            if (hs == 0) return;
            t->st_live++;
            if (t->st_live > t->st_peak_live) t->st_peak_live = t->st_live;
            engine_log(ENGINE_LOG_INFO, "[sess %d] handshake ok live=%d/%d",
                       sid, t->st_live, MAX_SESS);
            peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] queued interested", sid);
            // Tell the peer which pieces they may request from us (tit-for-tat).
            send_our_bitfield(t, s);
            // PEX: advertise ut_pex to extension-speaking peers so they start
            // feeding us live addresses.
            if (s->nb.ext_ok) peer_nb_queue_ext_handshake(&s->nb);
        }

        int msgs = 0;
        for (;;) {
            uint8_t id;
            uint8_t *pl;
            uint32_t plen;
            int r = peer_nb_next(&s->nb, &id, &pl, &plen);
            if (r < 0) { engine_log(ENGINE_LOG_DEBUG, "[sess] message decode error"); sess_close(t, s, true); return; }
            if (r == 0) break;
            sess_msg(t, s, sid, id, pl, plen, now);
            msgs++;
            if (!s->active) return;
        }

        if (got == 0 || msgs == 0) break;
    }

    fill_pipeline(t, s, now);

    // If we hold a piece but could not request any blocks (peer is unchoked and
    // the piece is still incomplete), the peer does not have it.  Drop the claim
    // so the session can try another peer / piece.  This is especially important
    // for blind playhead claims that turn out to be on peers without the block.
    if (s->claim >= 0 && !s->nb.choked && s->req_n == 0) {
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
        // Upload meter first: it must run regardless of download state -- a
        // peer can be downloading from us while it chokes us, and the choking
        // round ranks by this rate.
        if (s->tx_total == s->up_rate_prev) {
            s->up_rate_tick = 0;                 // idle: keep the smoothed rate
        } else if (s->up_rate_tick == 0) {       // (re)open the sample window
            s->up_rate_prev = s->tx_total;
            s->up_rate_tick = now;
        } else {
            double udt = (double)(now - s->up_rate_tick) / (double)t->freq;
            if (udt >= 0.4) {                    // at most one sample per upkeep
                double uinst = (double)(s->tx_total - s->up_rate_prev) / udt;
                if (uinst < 0) uinst = 0;
                s->up_rate      = s->up_rate * 0.6 + uinst * 0.4;
                s->up_rate_prev = s->tx_total;
                s->up_rate_tick = now;
            }
        }
        // Not downloading: pause the meter, keep rate + rate_start.
        if (s->claim < 0 || s->nb.choked || s->req_n == 0) {
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
    if (try_claim(t, best, bi, hot, hot == t->read_blocked_piece)) {
        fill_pipeline(t, best, now);
        peer_nb_flush(&best->nb);
    }
}

//-----------------------------------------------------------------------------
// Reciprocation-based choking (BEP-3 tit-for-tat, streaming-adapted)
//
// Every 10 s, the interested peers are ranked by their download rate TO us and
// the top MAX_UNCHOKE_SLOTS are unchoked; everyone else is choked. The peers
// that serve us fastest see us reciprocate, which is what their choking policy
// rewards -- the unchoke-all behavior before this let seeds write us off as a
// leecher that gives nothing back. One optimistic slot rotates every 30 s so a
// stranger can always prove itself (BEP-3's optimistic unchoke).
//-----------------------------------------------------------------------------

// Rank peers by their download rate to us, descending. Tie-break on lifetime
// bytes so a peer with no rate yet but a proven record outranks a fresh peer.
static int cmp_dl_rate(const void *a, const void *b) {
    const sess *sa = *(const sess *const *)a;
    const sess *sb = *(const sess *const *)b;
    if (sa->rate > sb->rate) return -1;
    if (sa->rate < sb->rate) return 1;
    if (sa->rx_total > sb->rx_total) return -1;
    if (sa->rx_total < sb->rx_total) return 1;
    return 0;
}

// Random choked interested peer for the optimistic slot (0 = none eligible).
static sess *pick_optimistic(torrentfs *t) {
    sess *pool[MAX_SESS];
    int n = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || !s->nb.handshaked || s->connecting) continue;
        if (!s->peer_interested || s->nb.we_unchoked) continue;
        pool[n++] = s;
        if (n == MAX_SESS) break;
    }
    if (n == 0) return NULL;
    return pool[rand() % n];
}

static void choking_round(torrentfs *t, u64 now) {
    // Nothing verified yet: nothing to serve, so no unchokes (and no reason to
    // choke peers that are merely interested -- they may serve us).
    mutexLock(&t->lock);
    bool have_offer = t->pieces_done > 0;
    mutexUnlock(&t->lock);
    if (!have_offer) return;

    sess *candidates[MAX_SESS];
    int n = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || !s->nb.handshaked || s->connecting) continue;
        if (!s->peer_interested) continue;
        candidates[n++] = s;
        if (n == MAX_SESS) break;
    }
    qsort(candidates, (size_t)n, sizeof(*candidates), cmp_dl_rate);

    int unchoked = 0;
    for (int i = 0; i < n; i++) {
        sess *s = candidates[i];
        bool should = (unchoked < MAX_UNCHOKE_SLOTS) ||
                      (s == t->optimistic_peer);
        if (should) {
            if (!s->nb.we_unchoked) {
                peer_nb_queue(&s->nb, MSG_UNCHOKE, NULL, 0);
                s->nb.we_unchoked = true;
                t->st_up_unchoke++;
            }
            unchoked++;
        } else if (s->nb.we_unchoked) {
            peer_nb_queue(&s->nb, MSG_CHOKE, NULL, 0);
            s->nb.we_unchoked = false;
        }
    }

    // Optimistic unchoke rotation: pick a new stranger every 30 s (it may be
    // one we just choked; the top-N loop above ran before this, so the new
    // slot always adds one peer).
    if (now - t->last_optimistic > (u64)OPTIMISTIC_SECS * t->freq) {
        t->last_optimistic = now;
        t->optimistic_peer = pick_optimistic(t);
        if (t->optimistic_peer && !t->optimistic_peer->nb.we_unchoked) {
            peer_nb_queue(&t->optimistic_peer->nb, MSG_UNCHOKE, NULL, 0);
            t->optimistic_peer->nb.we_unchoked = true;
            t->st_up_unchoke++;
        }
    }
}

struct torrentfs_incoming_q *torrentfs_incoming_queue(torrentfs *t) {
    return &t->inc_q;
}

// Attach sockets queued by the shared listener. The listener already consumed
// the peer's 68-byte handshake (that is how it routed the connection here), so
// we preload it into the peer_nb rx buffer and re-validate it against our own
// info_hash. These peers dialed US: they are the firewalled/NAT'd seeders that
// no outbound dial can ever reach.
static void drain_incoming(torrentfs *t, u64 now) {
    for (;;) {
        int fd = -1;
        uint8_t hs[68];
        bool have = false;
        mutexLock(&t->inc_q.lock);
        if (t->inc_q.n > 0) {
            t->inc_q.n--;
            fd = t->inc_q.q[t->inc_q.n].fd;
            memcpy(hs, t->inc_q.q[t->inc_q.n].hs, 68);
            have = true;
        }
        mutexUnlock(&t->inc_q.lock);
        if (!have) break;

        int sid = -1;
        for (int i = 0; i < MAX_SESS; i++)
            if (!t->S[i].active) { sid = i; break; }
        if (sid < 0) {
            engine_log(ENGINE_LOG_DEBUG, "[sess] no slot for incoming conn");
            close(fd);
            continue;
        }

        sess *s = &t->S[sid];
        memset(s, 0, sizeof(*s));
        if (peer_nb_init(&s->nb, fd, t->meta.piece_count) != 0) {
            close(fd);
            continue;
        }
        s->active     = true;
        s->pidx       = -1;   // not a pool peer: no backoff bookkeeping
        s->claim      = -1;
        s->started    = now;
        s->last_rx    = now;
        s->last_block = now;
        s->last_ka    = now;
        struct sockaddr_in psa;
        socklen_t psl = sizeof(psa);
        if (getpeername(fd, (struct sockaddr *)&psa, &psl) == 0)
            s->addr.ip = psa.sin_addr.s_addr;

        memcpy(s->nb.rx, hs, 68);
        s->nb.rx_len = 68;
        s->nb.rx_off = 0;
        if (peer_nb_recv_handshake(&s->nb, t->meta.info_hash) < 0) {
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] incoming handshake mismatch", sid);
            peer_nb_free(&s->nb);
            s->active = false;
            continue;
        }

        t->st_live++;
        if (t->st_live > t->st_peak_live) t->st_peak_live = t->st_live;
        engine_log(ENGINE_LOG_INFO,
                   "[sess %d] incoming handshake ok from %u.%u.%u.%u live=%d/%d",
                   sid, s->addr.ip & 0xff, (s->addr.ip >> 8) & 0xff,
                   (s->addr.ip >> 16) & 0xff, (s->addr.ip >> 24) & 0xff,
                   t->st_live, MAX_SESS);

        // Answer with our handshake and start the normal exchange: the peer
        // sends its bitfield as its first message, exactly like outbound.
        peer_nb_send_handshake(&s->nb, t->meta.info_hash, t->peer_id);
        peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
        send_our_bitfield(t, s);
        if (s->nb.ext_ok) peer_nb_queue_ext_handshake(&s->nb);
        peer_nb_flush(&s->nb);
    }
}

static void netloop_main(void *arg) {
    torrentfs *t = arg;
    u64 last_upkeep = 0;
    engine_log(ENGINE_LOG_INFO, "[netloop] start");

    while (!t->stop) {
        tsnx_engine_wd_tick(4);
        hb_beat(t, HB_NET);
        u64 now = armGetSystemTick();

        // Upkeep every ~250 ms: dial, reap, keep-alives, rate EWMA, calm.
        // 500 ms turned the 24-slot dial wave into 2/s, dragging out the
        // swarm ramp (a fresh magnet needs a few dozen dials); 250 ms doubles
        // the dial rate and halves stall/expiry reaction without any
        // meaningful per-tick cost (the scans are all O(MAX_SESS)).
        if (now - last_upkeep > t->freq / 4) {
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

            // Duplicate-traffic provenance, every ~10 s (PC diagnostics).
            {
                static u64 last_dup_dbg;
                if (now - last_dup_dbg > (u64)10 * t->freq) {
                    last_dup_dbg = now;
                    engine_log(ENGINE_LOG_DEBUG,
                               "[dupdbg] total=%lld noring=%lld have=%lld gone=%lld",
                               (long long)t->st_dup_bytes,
                               (long long)t->st_dup_noring,
                               (long long)t->st_dup_have,
                               (long long)t->st_dup_gone);
                }
            }

            // When peers are scarce the old code cut the connect budget to 1 s
            // to churn through dead DHT addresses faster. That starved itself:
            // slow-but-alive seeders could not complete TCP in 1 s, so the
            // swarm never grew. The floor is CONNECT_STARVED_SECS now, and each
            // session carries its own conn_budget (uTP keeps its 10 s).
            bool few_peers = t->st_live < STARVED_LIVE ||
                             (now - t->last_progress_tick > (u64)10 * t->freq);
            int conn_secs = few_peers ? CONNECT_STARVED_SECS : CONNECT_SECS;

            for (int i = 0; i < MAX_SESS; i++) {
                sess *s = &t->S[i];
                if (!s->active) continue;
                u64 age = now - (s->connecting ? s->started : s->last_rx);
                if (s->connecting && s->conn_budget && now > s->conn_budget) {
                    t->st_conn_fail++;
                    t->st_conn_timeout++;
                    s->fail_kind = PEER_FAIL_TIMEOUT;
                    engine_log(ENGINE_LOG_DEBUG, "[sess %d] close connect timeout", i);
                    sess_close(t, s, true);
                    continue;
                }
                if (!s->connecting && !s->nb.handshaked &&
                    now - s->started > (u64)PREHS_SECS * t->freq) {
                    engine_log(ENGINE_LOG_DEBUG, "[sess %d] close handshake timeout", i);
                    sess_close(t, s, true);
                    continue;
                }
                // A peer that keeps us choked forever without ever delivering
                // is dead weight: with candidates waiting in the pool, drop it
                // and dial a fresh one. 60 s is past the ~30 s optimistic
                // unchoke rotation, so a seed that merely rotates slots still
                // gets its chance; a peer that already proved useful
                // (rx_total > 0) is never rotated out on this basis. During
                // bootstrap a FULL seed gets triple the patience: it will
                // optimistic-unchoke us eventually, and killing it early just
                // throws away the slot we were waiting for.
                if (!s->connecting && s->nb.handshaked && s->nb.choked &&
                    s->rx_total == 0 &&
                    t->peer_count > t->st_live + t->st_connecting) {
                    int rotate_secs = CHOKED_ROTATE_SECS;
                    if (t->pieces_ever < BOOTSTRAP_THRESH &&
                        peer_is_seed(t, s))
                        rotate_secs = CHOKED_ROTATE_BOOTSTRAP_SECS;
                    if (now - s->started > (u64)rotate_secs * t->freq) {
                        engine_log(ENGINE_LOG_DEBUG,
                                   "[sess %d] rotate: peer choked us %ds without delivering",
                                   i, rotate_secs);
                        // A rotate-out is a normal choke cycle, not a dead
                        // address: minimal backoff, so we catch the next
                        // optimistic-unchoke window.
                        s->fail_kind = PEER_FAIL_CHOKE_ROTATE;
                        sess_close(t, s, true);
                        continue;
                    }
                }
                if (!s->connecting && age > (u64)IDLE_SECS * t->freq) {
                    engine_log(ENGINE_LOG_DEBUG, "[sess %d] close idle timeout", i);
                    sess_close(t, s, true);
                    continue;
                }
                // Requested blocks and nothing came: the peer accepted work it
                // will not deliver -- drop it, its claim gets adopted.
                if (s->req_n > 0 &&
                    now - s->last_block > (u64)STALL_SECS * t->freq) {
                    t->st_fetch_fail++;
                    set_err(t, "stall, piece %lld", (long long)s->claim);
                    engine_log(ENGINE_LOG_DEBUG, "[sess] close stall piece=%lld", (long long)s->claim);
                    sess_close(t, s, true);
                    continue;
                }
                // Expire individual requests that were never answered: their
                // per-piece counts are freed so another session (or this one)
                // can re-request just those blocks -- instead of the whole
                // piece being re-downloaded, or blindly duplicated. A session
                // that has never delivered a block gets the short fuse (its
                // deep probe should not hold hostage blocks for the full 15 s).
                if (!s->connecting && s->req_n > 0) {
                    u64 expire_after =
                        (s->rx_total == 0 ? (u64)REQ_FIRST_EXPIRE_SECS
                                          : (u64)REQ_EXPIRE_SECS) *
                        t->freq;
                    int kept = 0;
                    for (int k = 0; k < s->req_n; k++) {
                        if (now - s->req_time[k] > expire_after) {
                            int ep = s->req_piece[k];
                            int eb = s->req_block[k];
                            aq_entry *ea = aq_find(t, ep);
                            if (ea && eb >= 0 && eb < ea->nblocks &&
                                ea->req[eb] > 0)
                                ea->req[eb]--;
                        } else {
                            if (kept != k) {
                                s->req_piece[kept] = s->req_piece[k];
                                s->req_block[kept] = s->req_block[k];
                                s->req_time[kept]  = s->req_time[k];
                            }
                            kept++;
                        }
                    }
                    s->req_n = kept;
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
                // PEX: hand live addresses to extension-speaking peers; they
                // answer in kind, and their peers are alive by construction.
                // Normally once a minute; while starved, every 15 s so the few
                // live sessions snowball fresh addresses into the pool faster.
                // PEX out: never during bootstrap (a leecher that announces
                // peer lists within the first seconds of a handshake looks
                // like a spammer and gets dropped by seeds -- measured), never
                // sooner than a minute into the connection even after, and
                // send_pex only fills the list with proven addresses.
                if (!s->connecting && s->nb.handshaked && s->nb.pex_id &&
                    t->pieces_ever > 0 &&
                    now - s->started > (u64)PEX_INTERVAL_SECS * t->freq &&
                    now - s->last_pex > (u64)PEX_INTERVAL_SECS * t->freq) {
                    send_pex(t, s, now);
                }
            }

            update_rates(t, now);   // per-session rate meter
            steer_peers(t, now);    // fastest peer -> most urgent piece
            // Reciprocation choking: 10 s round, 30 s optimistic rotation.
            if (now - t->last_choke > (u64)CHOKE_INTERVAL_SECS * t->freq) {
                t->last_choke = now;
                choking_round(t, now);
            }
            // Starvation signal for the background DHT: search more often so
            // the peer pool refills instead of waiting out the 15 s timer.
            dht_bg_set_hungry(t->st_live < STARVED_LIVE ? 1 : 0);
            drain_incoming(t, now); // sockets the shared listener accepted

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
            // connection slots on ВµTP and retry peers that failed earlier. DHT
            // swarms are often full of dead/NAT'd addresses; without this the pool
            // can exhaust its backoff budget and stall the stream.
            if (few_peers) {
                u64 pause_ticks = (t->starve_round <= 1) ? (u64)5 * t->freq :
                                  (t->starve_round == 2)  ? (u64)15 * t->freq :
                                  (t->starve_round == 3)  ? (u64)30 * t->freq :
                                                            (u64)60 * t->freq;
                if (now - t->last_starve > pause_ticks) {
                    t->last_starve = now;
                    t->starve_round++;
                    t->st_starve_rounds++;
                    int reset = 0;
                    int condemned = 0;
                    mutexLock(&t->lock);
                    if (t->starve_round == 1) {
                        // Round 1 (soft): reset backoff only for proven peers
                        // (peer_ok) with few failures.
                        for (int i = 0; i < t->peer_count && reset < 64; i++) {
                            if (t->peer_busy[i]) continue;
                            if (t->peer_ok[i] && t->peer_fails[i] > 0 &&
                                t->peer_fails[i] <= 3) {
                                t->peer_fails[i] = 0;
                                t->peer_next_try[i] = 0;
                                reset++;
                            }
                        }
                    } else {
                        // Round 2+ (hard): condemn unproven peers that failed
                        // repeatedly. 0xFF makes add_peers_src replace them with
                        // fresh DHT/tracker peers; long next_try stops redials.
                        // Tracker-reported peers are never condemned: they are
                        // the swarm's registered seeds and get replaced by junk
                        // if we drop them. Connect-timeout failures get a
                        // higher bar than refusals (a slow seeder is not a
                        // dead one).
                        for (int i = 0; i < t->peer_count && condemned < 64; i++) {
                            if (t->peer_busy[i]) continue;
                            if (t->peer_src[i] == PEER_SRC_TRACKER) continue;
                            int threshold = (t->peer_fail_kind[i] == PEER_FAIL_TIMEOUT)
                                                ? 6 : 4;
                            if (!t->peer_ok[i] && t->peer_fails[i] >= threshold &&
                                t->peer_fails[i] < 0xFF) {
                                t->peer_fails[i] = 0xFF;
                                t->peer_next_try[i] = now + (u64)600 * t->freq;
                                condemned++;
                                t->st_peer_evicted++;
                            }
                        }
                    }
                    mutexUnlock(&t->lock);
                    engine_log(ENGINE_LOG_WARN,
                               "[torrentfs] starvation recovery (round %d): reset=%d, condemned=%d, evicted_total=%d, live=%d",
                               t->starve_round, reset, condemned,
                               t->st_peer_evicted, t->st_live);

                    // Re-announce to trackers only after actual peer evacuation
                    // and at most once per 60s (no announce storm).
                    static u64 last_starve_announce;
                    if ((condemned > 0 || t->starve_round >= 2) &&
                        now - last_starve_announce > (u64)60 * t->freq) {
                        last_starve_announce = now;
                        t->announce_now = true;
                    }
                }
            }

            // One uTP hole-punch retry per wave, ahead of the regular dials.
            int dial_budget = DIAL_BUDGET_PER_TICK;
            {
                peer_addr rpa;
                int rp = take_utp_retry_peer(t, &rpa);
                if (rp >= 0) {
                    if (sess_dial_utp_at(t, now, rp, rpa)) {
                        connecting++;
                        dial_budget--;
                    }
                }
            }

            // DIAL_BUDGET_PER_TICK new dials per 250 ms upkeep (~8-12/s max):
            // a SYN storm of failed connects fills the home router's NAT table
            // with half-open entries, which then drops healthy traffic too.
            while (!t->paused && dial_budget > 0 &&
                   t->st_live < DIAL_STOP_LIVE && connecting < MAX_CONNECTING) {
                bool ok;
                // µTP runs through a userspace stack and tops out well below
                // plain TCP on the Switch; keep it as a rare NAT fallback
                // (1 dial in 8) rather than an every-4th-dial transport.
                bool try_utp = utp_nb_fd() >= 0 && !few_peers &&
                               (t->dial_toggle % 8) == 0;
                if (try_utp) {
                    ok = sess_dial_utp(t, now);
                } else {
                    ok = sess_dial(t, now, conn_secs);
                }
                t->dial_toggle = (t->dial_toggle + 1) & 0x7f;
                if (!ok) break;
                connecting++;
                dial_budget--;
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
                // The reader is blocked waiting on a piece: a free session
                // that has it joins the existing claim before taking new work,
                // so one slow peer cannot gate the whole sequential stream.
                // Workers on it are capped at ~live/3 (clamped 2..6): in a
                // small swarm the rest must keep filling the look-ahead window,
                // or everyone just duplicates each other's requests on one
                // piece and the stream starves on the next one.
                int64_t rb = t->read_blocked_piece;   // racy read: benign
                if (rb >= t->file_first_piece && rb <= t->file_last_piece) {
                    aq_entry *ba = aq_find(t, rb);
                    int wcap = t->st_live / 3;
                    if (wcap < 2) wcap = 2;
                    if (wcap > 6) wcap = 6;
                    if (!ba || ba->workers < wcap) {
                        if (try_claim(t, s, i, rb, true)) {
                            claiming++;
                            fill_pipeline(t, s, now);
                            peer_nb_flush(&s->nb);
                            continue;
                        }
                    }
                }
                claim_piece(t, s, i);
                if (s->claim >= 0) {
                    claiming++;
                    fill_pipeline(t, s, now);
                    peer_nb_flush(&s->nb);
                }
            }
        }

        // One poll over the session sockets plus the shared ВµTP UDP socket,
        // capped at POLL_CHUNK_MAX and rotated so a busy PC build stays under
        // the 64-handle WaitForMultipleObjects limit (see the define above).
        struct pollfd pfd[POLL_CHUNK_MAX];
        int map[POLL_CHUNK_MAX];
        int n = 0;
        int start = t->poll_cursor;
        for (int w = 0; w < MAX_SESS && n < POLL_CHUNK_MAX - 1; w++) {
            int i = (start + w) % MAX_SESS;
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
            t->poll_cursor = (i + 1) % MAX_SESS;   // next poll starts after it
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
        int pr = poll(pfd, (nfds_t)n, 100);
        lat_add(t, LAT_POLL, lt0);

        now = armGetSystemTick();
        bool utp_serviced = false;
        if (pr > 0) {
            for (int k = 0; k < n; k++) {
                if (!pfd[k].revents) continue;
                if (map[k] == -1) {
                    // Shared ВµTP socket: process packets and timeouts.
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
        // Service ВµTP sessions even if the UDP fd did not wake us, so libutp
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
    t->read_blocked_piece = -1;

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
    mutexInit(&t->inc_q.lock);
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

    torrent_peer_id(t->peer_id);  // stable engine-wide id (announce/DHT/sessions)

    if (seed_count > 0) add_peers_src(t, seed_peers, seed_count, PEER_SRC_TRACKER);

    // Persistent background DHT: keeps the routing table warm and re-issues
    // searches every 30 s, instead of one-shot lookups that re-bootstrap each time.
    dht_background_add(t->meta.info_hash, add_peers_dht_cb, t);

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

// True once a read starting at piece idx, block b0 can return data: the piece
// is fully verified, or (RAM mode) its first needed block has arrived and can
// be streamed before verification like any streaming client does.
static bool piece_ready(torrentfs *t, int64_t idx, int b0) {
    if (have_piece(t, idx)) return true;
    if (!t->ram_mode) return false;
    aq_entry *a = aq_find(t, idx);
    if (!a || b0 < 0 || b0 >= a->nblocks) return false;
    return __atomic_load_n(&a->have[b0], __ATOMIC_ACQUIRE) != 0;
}

int64_t torrentfs_read(torrentfs *tfs, int64_t offset, char *buf, int64_t nbytes) {
    tsnx_engine_wd_tick(6);
    hb_beat(tfs, HB_READER);
    if (offset >= tfs->stream_size) return 0;
    if (offset + nbytes > tfs->stream_size) nbytes = tfs->stream_size - offset;
    if (nbytes <= 0) return 0;

    torrentfs_set_playhead(tfs, offset);

    int64_t abs   = tfs->stream_offset + offset;
    int64_t plen  = tfs->meta.piece_len;
    int64_t first = abs / plen;
    int     b0    = (int)((abs % plen) / BLOCK_LEN);

    // Wait for the piece to verify, or (RAM mode) for its first needed block
    // to land -- the installer then drains the piece as it arrives instead of
    // stalling at every 8 MB piece boundary. While we wait, tell the netloop
    // which piece gates us so free sessions that have it can join the claim.
    tfs->read_blocked_piece = first;
    u64 wait_start = armGetSystemTick();
    while (!tfs->stop && !piece_ready(tfs, first, b0))
        svcSleepThread(20000000ULL);  // 20 ms
    tfs->read_blocked_piece = -1;
    if (tfs->stop) return -1;
    double waited = (double)(armGetSystemTick() - wait_start) / tfs->freq;
    if (waited > 5.0) {
        engine_log(ENGINE_LOG_WARN,
                   "[read] waited %.1fs for piece %lld", waited, (long long)first);
    }

    mutexLock(&tfs->cache_lock);
    u64 lt0 = armGetSystemTick();
    size_t got = cache_read_upto(tfs, abs, buf, (size_t)nbytes);
    // In RAM mode cache_read_upto is a plain memcpy from the window or an
    // assembly buffer, not an SD syscall: recording it as an "sd rd" probe
    // made the ZR panel's read counter climb on every mpv read and look like
    // disk activity. Only the real, fd-backed read path is a genuine SD access.
    if (!tfs->ram_mode) lat_add(tfs, LAT_RD, lt0);
    mutexUnlock(&tfs->cache_lock);

    // Racy on purpose: diagnostic counter on the reader thread. With partial
    // streaming a short read is the norm (it stops at the next missing block),
    // so this counts only reads that came back empty.
    if (got == 0) ((torrentfs *)tfs)->st_cache_rd_short++;
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

void torrentfs_add_peers(torrentfs *tfs, const peer_addr *peers, int n) {
    if (!tfs || !peers || n <= 0) return;
    add_peers_src(tfs, peers, n, PEER_SRC_TRACKER);
}

// Live handshaked sessions whose bitfield covers every piece of the streamed
// file, i.e. the peers that can actually serve the whole thing (what a UI
// means by "seeds"). Racy by design: bitfields are netloop-owned, so a torn
// read can only misclassify one peer for one snapshot.
int torrentfs_seed_count(const torrentfs *tfs) {
    torrentfs *t = (torrentfs *)tfs;
    int seeds = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &t->S[i];
        if (!s->active || !s->nb.handshaked || !s->nb.bitfield) continue;
        bool full = true;
        for (int64_t p = t->file_first_piece; p <= t->file_last_piece; p++) {
            if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, p)) {
                full = false;
                break;
            }
        }
        if (full) seeds++;
    }
    return seeds;
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

int torrentfs_hs_fail(const torrentfs *tfs) {
    return tfs->st_hs_fail;
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
    // Accepted-and-handshaked incoming sessions (their pidx is -1).
    int n = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &tfs->S[i];
        if (s->active && s->nb.handshaked && s->pidx < 0) n++;
    }
    return n;
}

int64_t torrentfs_bytes_recv(const torrentfs *tfs) {
    return tfs->st_bytes_recv;
}

// Block payloads received for blocks already held (wasted duplicate traffic).
int64_t torrentfs_dup_bytes(const torrentfs *tfs) {
    return tfs->st_dup_bytes;
}

void torrentfs_last_err(const torrentfs *tfs, char *buf, size_t len) {
    snprintf(buf, len, "%s", tfs->st_last_err);
}
