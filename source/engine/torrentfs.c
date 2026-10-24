// torrentfs3.c — v3 of the streaming torrent engine (same public API as v1/v2).
//
// The v3 scheduler was rebuilt to mirror torrserver's engine (the anacrolix/
// torrent client, requestStrategy 3), because that client streams on this very
// swarm at line rate while the old per-piece-owner design stalled:
//
//   - NO piece ownership. Every unchoked session, on every refill, scans ALL
//     pieces in priority order (Now > Next > Readahead > High > Normal) and
//     requests the first blocks that are not held by someone else yet. The
//     reader's piece therefore gets the whole swarm's in-flight capacity
//     before any other piece does (anacrolix: PiecePriorityNow).
//   - Duplicate requests: a block requested but not delivered for 1 s may be
//     re-requested by another session (up to MAX_DUP copies). This is
//     anacrolix's duplicateRequestTimeout -- it is what unblocks a piece held
//     by a slow/stalled peer without any owner-takeover machinery.
//   - Per-session pipeline depth = ~0.5 s of that session's measured useful
//     delivery, clamped 2..250 (anacrolix nominalMaxRequests, strategy 3).
//     Requests are never expired and never cancelled on release: stragglers
//     land, get deduplicated, and count toward the piece. On arrival of a
//     block, duplicate copies in flight on other sessions are CANCELed
//     (anacrolix postCancel) so the swarm stops wasting bandwidth on it.
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
//   DROPPED (v3 ownership model):
//   - upload serving, incoming acceptors, the listen socket (leech-only;
//     removes 2 threads, the INQ, tit-for-tat state)
//   - the per-session piece claim + owner/helper worker caps, parked blocks,
//     stall-driven hedging, hot-piece steering: all replaced by the global
//     priority scan + 1 s duplicate timer above
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
#include <sys/stat.h>
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
#include "thread_cfg.h"
#include "perf_hist.h"
#include "../utils/app_paths.h"

//-----------------------------------------------------------------------------
// Tuning
//-----------------------------------------------------------------------------

#ifdef __SWITCH__
/* All sockets of the process share one BSD buffer pool (~13 MB, configured in
   main.cpp userAppInit). Peers get a 128 KB receive window, proven-fast ones
   512 KB (classify_sessions). Targeting 48-50 live sessions with active
   unchoke upkeep ensures 25-35 active uploaders. */
#define MAX_SESS             64
#define MAX_CONNECTING       32     // slots allowed to sit in a pending connect (24 was always full on the console: 81 % of dials time out)
#define DIAL_STOP_LIVE       50     // baseline live sessions
#define MIN_ACTIVE_UNCHOKED  24     // keep dialing up to MAX_SESS if unchoked peers are below this
#define REQ_INITIAL          32     // Initial probe window on Switch: 32 blocks (512 KB)
#define MAX_REQ_PER_CONN     256    // Up to 4.0 MB in flight per fast session on Switch (matching pipensx MAX_PIPELINE)
#else
#define MAX_SESS             80
#define MAX_CONNECTING       32
#define DIAL_STOP_LIVE       64
#define MIN_ACTIVE_UNCHOKED  32
#define REQ_INITIAL          32     // Initial probe window on PC: 32 blocks (512 KB)
#define MAX_REQ_PER_CONN     256    // Up to 4.0 MB in flight per fast session on PC
#endif
#define CONNECT_SECS         2      // 2.0s fast-fail for dead DHT peers (pipensx uses 1.5s; 8-10s starved connection pool)
#define CONNECT_STARVED_SECS 3      // starved swarm floor
#define CONNECT_RETRY_SECS   5      // patience for proven peers (peer_ok)
#define UTP_MAX_CONNECTING   4      // uTP dials in flight (each may hold a slot 10 s)
// Dial budgets: bootstrap burst connects rapidly, upkeep maintains
#define DIAL_BUDGET_PER_TICK 6
#define DIAL_BUDGET_BOOTSTRAP 12
#define PREHS_SECS       10     // connected but no handshake yet
#define IDLE_SECS        130    // handshaked, nothing received (see header)
#define KEEPALIVE_SECS   60
#define STALL_SECS       25     // requested blocks but nothing came: drop peer (with strike tolerance)
#define DUP_REQ_SECS     1      // anacrolix duplicateRequestTimeout: re-request an undelivered block after this long
#define MAX_DUP          4      // outstanding copies of one block

#define REQ_RING         512    // per-session outstanding-request capacity
#define REQ_MIN          2
#define LOOKAHEAD_PIECES 16     // Multi-file lookahead: allow prefetching pieces into upcoming files

// Speed classes. A session delivering less than SLOW_FRACTION of the fastest
// session's rate (once it has delivered SLOW_MIN_BYTES), or one with stall
// strikes, is "slow" while at least one fast session exists. Slow sessions no
// longer take blocks of the pieces the reader needs next: a block on order at
// a slow peer held its whole piece back until the 1 s hedge (2.5 s for
// background pieces), and a mixed swarm downloaded several times SLOWER than
// its fastest peer alone (measured: 1 fast + 2 slow seeds = 62 MB/s, the fast
// seed alone = 384 MB/s). Instead each slow session works on a piece of its
// own at the far end of the window (libtorrent's speed-class picking).
#define SLOW_FRACTION    0.25
#define SLOW_MIN_BYTES   (512 * 1024)

// Peer turnover ("tournament"): every TURNOVER_SECS, if the session table is
// (nearly) at its target and the pool has candidates ready, up to
// TURNOVER_HEADROOM extra sessions may be dialed. Every session gets a
// TRIAL_SECS grace period; when the window closes the table is trimmed back
// to its target, one session per upkeep tick, always dropping the worst
// established session by capability -- an old one or a newcomer alike. What
// stays is the best of old + new, so the swarm only gets faster. A peer
// rotated out this way waits SLOW_DROP_SECS before it is dialed again, which
// leaves the slots to untried candidates. Without this the engine kept
// whichever peers connected first: dialing stopped at the target and a
// 20 KB/s peer held its slot forever.
#define TURNOVER_SECS     30
#define TRIAL_SECS        60
#define TURNOVER_HEADROOM 4
#define SLOW_DROP_SECS    600
// Unchoked, requests on order, nothing delivered for this long: snubbed. Such
// a session counts as slow at once (the stall gate needs 25 s and strikes).
#define SNUB_SECS         15

// RAM for in-flight piece buffers; bounds how many pieces are open at once.
#define RAM_BUDGET       (256LL << 20)
#define AQ_MAX           48
#define AQ_MIN           4
// Writer queue capacity. Every queued job owns a whole piece buffer, and an
// assembly slot gets a FRESH buffer as soon as its last one is queued -- so
// the queue is not bounded by the slot count. A new piece may only be opened
// while (queued jobs + active pieces) < WQ_CAP (see wq_has_room): each active
// piece adds at most one job, so the ring can never overflow, and the RAM held
// by verify-pending pieces is bounded. Before this, a writer slower than the
// network (software SHA-1 at loopback speed) grew the queue to 150+ jobs:
// the ring wrapped onto itself, jobs ran twice and the heap was corrupted.
#define WQ_CAP           (AQ_MAX + 2)

// Streaming window
#define STREAM_WINDOW    (384LL * 1024 * 1024)
#define STREAM_MIN_PIECES 2

// Package installation: Focus 100% on container header (PFS0/HFS0).
// Tail speculation is completely disabled (crit_tail = 0): games have no moov tail.
#define CRIT_HEAD_BYTES  (4LL << 20)   // ~4 MB at the head (PFS0/HFS0 header & NCA tables)
#define CRIT_TAIL_BYTES  0             // 0 for package streaming
#define CRIT_HEAD_MAX    4
#define CRIT_TAIL_MAX    0
#define PIN_HEAD_BYTES   (8LL << 20)   // Pin first 8 MB in RAM so package headers never evict

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

#define TFS_MAX_PEERS    2048
// ip:port -> pool slot index (open addressing, linear probing). Finding an
// address used to be a scan of all 2048 entries for every address a tracker /
// DHT / PEX reply delivered. Keyed by ip:port (it used to be the IP alone):
// several clients behind one NAT / CGNAT address are distinct peers.
#define POOL_HASH_SIZE   4096   // power of two, >= 2 * TFS_MAX_PEERS
// When the pool is full, the eviction victim is the worst of this many
// entries from a rotating cursor (was: a scan of the whole pool per address).
#define POOL_EVICT_SAMPLE 64
// Outbound sessions per IP (different ports = different clients behind one
// address). An IP that already has an INCOMING session is not dialed at all:
// that would just open a second session to the same peer.
#define MAX_SESS_PER_IP   2
#define BACKOFF_CONN_SECS 5
#define BACKOFF_MAX_SECS  120
#define BACKOFF_DEAD_SECS 300   // connect timed out 3+ times: unreachable address
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

// Discovery re-runs every 15 min, or 60 s after the last round when starved.
// Trackers throttle announces to a single peer address when a client
// re-announces too often, so starving must not turn into an announce spam.
#define DISC_INTERVAL_SECS (15 * 60)
#define DISC_STARVED_SECS  60
// Below this many live sessions the engine runs starvation mode.
// 8 good peers is enough to saturate a 100 Mbit link.
#define STARVED_LIVE       8

// PEX (BEP 11): exchange peer addresses with live sessions. Peers discovered
// this way are by definition reachable (they come from someone who is talking
// to us right now), which is what makes PEX the antidote to the dead-address
// flood from trackers/DHT. Send every minute, at most 50 addresses per
// message; accept at most one message per session per 5 s (abuse guard).
#define PEX_INTERVAL_SECS        30
#define PEX_MAX_PEERS            50
#define PEX_RX_MIN_INTERVAL_SECS 5

// Upload serving: we answer peers' block requests from the RAM window (v1:
// RAM mode only; an SD read on the netloop would stall it). The per-session
// tx queue is bounded anyway (512 KiB cap), but a burst of requests from a
// fast peer would otherwise queue well past the cap and waste CPU on refused
// appends; refuse new requests once this much is still unsent.
#define UP_TX_WATERMARK  (384 * 1024)

// A handshaked peer that keeps us choked for this long while never having
// delivered a block is dead weight: drop it and dial a fresh candidate.
// 45 s: gives BitTorrent peers (whose optimistic unchoke cycle is 30 s)
// time to unchoke before we prematurely disconnect them.
#define CHOKED_ROTATE_SECS 45
// Bootstrap: fewer than this many pieces ever completed -- we have nothing to
// reciprocate with yet, so full seeds are precious and never rotated out.
#define BOOTSTRAP_THRESH 5

// Reciprocation-based choking (BEP-3 tit-for-tat, adapted: ranking is by the
// peer's download rate TO US, not their upload rate from us -- a streaming
// client mostly downloads from seeds that want nothing back, so the peers
// worth rewarding are the ones that serve us). Round every 10 s: unchoke the
// top MAX_UNCHOKE_SLOTS interested peers by download rate, choke the rest, and
// rotate one optimistic slot every OPTIMISTIC_SECS so a new peer can prove
// itself and the swarm sees us as a willing uploader.
#define CHOKE_INTERVAL_SECS 10
#define MAX_UNCHOKE_SLOTS    8
#define OPTIMISTIC_SECS     30

#ifdef __SWITCH__
#define POLL_CHUNK_MAX   100
#else
#define POLL_CHUNK_MAX   56
#endif

enum { PIECE_NEEDED = 0, PIECE_ACTIVE = 1, PIECE_DONE = 2, PIECE_WRITING = 3 };

static volatile int g_turnover_secs = TURNOVER_SECS;   // tunable (tests)
static volatile int g_trial_secs = TRIAL_SECS;
void torrentfs_set_rotation_timing(int turnover_secs, int trial_secs) {
    if (turnover_secs > 0) g_turnover_secs = turnover_secs;
    if (trial_secs > 0) g_trial_secs = trial_secs;
}

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

// One piece being assembled in RAM. idx -1 = slot free. There is no owner:
// any session may request any of its blocks at any time (anacrolix model).
// Only the netloop touches entries, except the writer returning a buffer
// under t->lock.
typedef struct {
    int64_t idx;
    uint8_t *buf;       // piece_len bytes, lazily allocated, reused
    uint8_t *have;      // one byte per block: data present in buf
    uint8_t *req;       // one byte per block: outstanding-request COUNT (capped at 255)
    uint64_t *req_ts;   // when the block was last requested (dup gate: DUP_REQ_SECS)
    int16_t *peer_from; // pidx of peer that delivered block b (-1 if none)
    // Which sessions hold an outstanding request for each block: row b is
    // SESS_MASK_BYTES bytes, bit = session index. Kept in step with every
    // session's request ring (rb_set on request, rb_clr on every removal), so
    // "does this session already have (piece, block) on order" and "who else
    // must get a CANCEL" are O(1) / O(holders) instead of ring scans.
    uint8_t *req_by;
    int nblocks;
    int have_cnt;
    int next_req;       // scan cursor for the next block to request
    u64 last_deliver;   // last time a block of this piece arrived (diagnostics)
} aq_entry;

typedef struct {
    bool active;
    bool connecting;
    bool utp_retry;     // this dial is a one-shot uTP retry after a TCP fail
    peer_nb nb;         // sock/framing/handshake/bitfield (peer.c)
    peer_addr addr;     // remote endpoint
    int pidx;           // peer pool index (for backoff bookkeeping)
    // Outstanding block requests, as a ring of (piece, block, time). A PIECE
    // is only accepted if it exactly matches an entry here -- piece AND block,
    // because a late delivery for a previous claim shares block numbers with
    // the new one and would otherwise eat the new claim's entries. Requests
    // are never expired: a block that never arrives is re-requested by other
    // sessions after DUP_REQ_SECS (duplicate), and deliveries simply pop the
    // matching entry.
    int req_n;
    int req_piece[REQ_RING];
    int req_block[REQ_RING];
    u64 req_time[REQ_RING];
    u64 started, last_rx, last_block, last_ka;
    u64 conn_budget;     // dial patience for THIS attempt (ticks, 0 = default)
    int fail_kind;       // why this session failed, for pool bookkeeping
    u64 last_pex;      // last ut_pex we SENT (0 = never)
    u64 last_pex_rx;   // last ut_pex we ACCEPTED (rate-limit guard)
    // Per-session receive meter, for the pipeline depth (anacrolix
    // nominalMaxRequests: delivered_rate * DUP_REQ_SECS / 2, clamped 2..250).
    int64_t rx_total;   // cumulative bytes received (never reset)
    int64_t useful;     // matched (expected) blocks received
    u64 expect_ticks;   // time spent with outstanding requests (rate anchor)
    int64_t rate_prev;  // rx_total at the last rate sample
    u64 rate_tick;      // last rate sample (0 = meter not started yet)
    double rate;        // smoothed receive rate, bytes/s
    // Per-session upload meter, for the choking round's reciprocity ranking.
    int64_t tx_total;   // cumulative bytes we sent to this peer
    int64_t up_rate_prev;   // tx_total at the last up-rate sample
    u64 up_rate_tick;       // last up-rate sample (0 = meter not started yet)
    double  up_rate;        // smoothed upload rate to this peer, bytes/s
    int have_in_file;      // pieces of the streamed file the peer has (BITFIELD + HAVE)
    bool slow;             // speed class (see SLOW_FRACTION), refreshed every upkeep
    bool snubbed;          // unchoked with requests on order, nothing for SNUB_SECS
    // Service-time meter for the speed class: per matched block, the time from
    // when the peer could start on it (the later of its request and the
    // previous delivery) to its arrival. Idle time never counts, so a bursty
    // fast peer reads fast (the upkeep-sampled rate saw it idle and read 0).
    u64 svc_ticks;
    int64_t svc_bytes;
    u64 last_rx_block;
    int64_t own_piece1;    // slow sessions: piece they work on alone, + 1 (0 = none)
    bool peer_interested;  // peer wants our pieces (we may unchoke)
    bool rcvbuf_big;       // SO_RCVBUF already raised for a proven-fast peer
    int64_t scan_useful;   // `useful` at the last receive-path refill
    uint8_t timeout_strikes; // strikes before closing on stall
    u64 choked_at;         // tick when peer last choked us (0 = unchoked)
} sess;

static void sess_drop_requests(torrentfs *t, sess *s);
static aq_entry *aq_find(torrentfs *t, int64_t idx);
static double sess_capability(const torrentfs *t, const sess *s);

#define SESS_MASK_BYTES ((MAX_SESS + 7) / 8)

static inline uint8_t *rb_row(const aq_entry *a, int b) {
    return a->req_by + (size_t)b * SESS_MASK_BYTES;
}
static inline bool rb_has(const aq_entry *a, int b, int sid) {
    return (rb_row(a, b)[sid >> 3] >> (sid & 7)) & 1;
}
static inline void rb_set(aq_entry *a, int b, int sid) {
    rb_row(a, b)[sid >> 3] |= (uint8_t)(1u << (sid & 7));
}
static inline void rb_clr(aq_entry *a, int b, int sid) {
    rb_row(a, b)[sid >> 3] &= (uint8_t)~(1u << (sid & 7));
}
static void torrentfs_apply_select_file(torrentfs *tfs, int file_index);

typedef struct {
    int64_t idx;
    int64_t plen;
    uint8_t *buf;
    int16_t *peer_from;
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
    bool strict_verify;
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
    bool has_custom_zones;     // true when an external scheduler has configured piece zones
    int64_t pieces_done;       // currently resident (RAM window) / on disk (SD)
    int64_t pieces_ever;       // distinct pieces ever completed: monotonic, for UI
    uint8_t *ever;             // one bit per piece, already counted in pieces_ever
    int64_t playhead_piece;    // lock-free (aligned 64-bit store, see below)
    int64_t read_blocked_piece;// piece the reader thread is waiting on, -1 = none
    volatile bool stop;
    volatile bool cancel_reader;
    volatile int pending_select_file; // -1 = none, >= 0 = file index to switch to on netloop thread

    aq_entry aq[AQ_MAX];
    int16_t *aq_of_piece;      // piece -> aq slot hint (validated by aq[slot].idx)

    // Request candidates (netloop-private): the pieces with a non-zero
    // priority, grouped by zone 1..5 and ascending within a zone. The request
    // scheduler used to recompute the priority of EVERY piece from the
    // playhead to the end of the file, 5 zones x 2 passes, for every single
    // block it requested -- O(pieces) per 16 KB, the cost that grew with file
    // size. The list is rebuilt only when an input of piece_priority changes
    // (playhead, zones, file); the read-blocked piece, which the reader sets
    // and clears on every read, is handled separately (always first).
    int32_t *cand;
    uint8_t *cand_tmp;         // rebuild scratch: per-piece zone
    int cand_end[6];           // zone z occupies cand[cand_end[z-1] .. cand_end[z])
    bool cand_valid;
    int64_t cand_ph, cand_first, cand_last;
    uint32_t cand_gen;
    uint32_t zone_gen;         // bumped (release) whenever piece zones change
    uint32_t pf_rebuilds;      // [perf] candidate rebuilds
    int n_fast;                // unchoked sessions not classified slow (netloop)

    // Turnover (netloop): see TURNOVER_SECS.
    int live_target;           // sessions to keep (default DIAL_STOP_LIVE)
    int live_max;              // hard cap (default MAX_SESS)
    u64 last_turnover;
    u64 turnover_until;        // headroom window open until (0 = closed)
    u64 turnover_start;        // when the current window opened
    int trim_left;             // the window closed: sessions still to trim (<= headroom)
    uint32_t dial_seq;         // every 4th dial explores an untried address
    int st_rotated, pf_rotated;    // sessions rotated out by turnover / cap
    int st_dials, pf_dials;
    // Pool exhaustion (netloop): below the session target with nothing left
    // to dial -> ask trackers and the DHT for fresh addresses.
    u64 last_exhaust_check;
    u64 last_exhaust_announce;
    bool pool_exhausted;
    int st_exhaust_announces;
    u64 last_dup_dbg;          // [dupdbg] cadence (was a static shared by all torrents)
    // Files the app did not select (1 = unwanted). Only the multi-file
    // lookahead consults it: near the end of the streamed file the engine
    // pre-fetches up to 64 MB of the NEXT file -- wasted when that file was
    // not selected.
    uint8_t *file_unwanted;
    int n_aq;
    int n_active_aq;
    uint8_t *aq_buf_pool[AQ_MAX];
    int aq_buf_pool_cnt;

    wjob wq[WQ_CAP];
    // Event wake-ups instead of polling (the writer used to poll its queue
    // every 2 ms, the reader its piece every 20 ms -- in strict mode ~10 ms of
    // dead time per piece). Both are waited on with t->lock held.
    CondVar wq_cv;      // writer: a job was queued
    CondVar piece_cv;   // reader: a piece became readable
    int wq_head, wq_n;
    int64_t writing_idx;
    uint8_t *writing_buf;

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
    uint8_t peer_sha_fails[TFS_MAX_PEERS];  // accumulated sha1 check failures from this peer
    // Speed history: the best capability (KB/s) any session to this address
    // reached, and how often it was rotated out for being slow. Dial order and
    // pool eviction prefer addresses that were fast before.
    float peer_best_kbps[TFS_MAX_PEERS];
    uint8_t peer_rot[TFS_MAX_PEERS];
    u64 peer_next_try[TFS_MAX_PEERS];
    u64 pause_dials_until;               // temporarily suspend new TCP dials (e.g. metadata probe)
    int peer_count;
    int next_peer;
    int peer_evict_rr;
    int16_t pool_hash[POOL_HASH_SIZE];   // slot + 1, 0 = empty (under t->lock)

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
    u64 last_starve_announce;
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
    char st_last_err[128];

    u64 hb_tick[4];
    u8  hb_core[4];
    uint32_t lat_n[5];
    uint64_t lat_max[5];

    // [perf] window, netloop-private (see perf_emit): per-iteration work time
    // with the poll() wait excluded, and how often the request scheduler ran.
    perf_hist pf_loop;
    uint32_t pf_scans;         // request_scan() calls
    uint32_t pf_picks;         // scan_next_block() calls (one per requested block)
    // Time inside peer socket I/O (recv/send of sessions; on the Switch every
    // call is an IPC to the bsd sysmodule on core 3). Tells "slow loop" apart
    // from "slow socket service" in the [perf] line.
    u64 pf_sock_ticks;
    uint32_t pf_sock_calls;
    u64 last_poll_ret;         // netloop: when the last poll() returned (rate cap)
    int pf_utp_connecting;     // uTP dials in flight (last upkeep)
    int st_utp_dials, pf_utp_dials;
    u64 pf_tick;
    int64_t pf_bytes, pf_dup;
    int pf_sock_fail, pf_timeouts, pf_enobufs;
    int st_enobufs;            // socket()/connect() failed with ENOBUFS (BSD pool exhausted)
    int st_wq_full;            // new pieces refused because the writer queue was full

    // Status snapshot for other threads (UI, scheduler, status polls). The
    // session table is netloop-private -- reading it from another thread
    // raced with sess_close() freeing a session's bitfield -- so the netloop
    // publishes what those callers need every upkeep tick, under snap_lock.
    Mutex snap_lock;
    torrentfs_peer_info snap_peers[MAX_SESS];
    int snap_n_peers;
    int snap_seeds;
    int snap_claiming, snap_idle;
    int pf_wq_full;
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
        if (!src) {
            if (!t->strict_verify) {
                mutexLock(&t->lock);
                if (t->writing_idx == idx && t->writing_buf) {
                    src = t->writing_buf;
                } else {
                    for (int i = 0; i < t->wq_n; i++) {
                        int slot = (t->wq_head + i) % WQ_CAP;
                        if (t->wq[slot].idx == idx && t->wq[slot].buf) {
                            src = t->wq[slot].buf;
                            break;
                        }
                    }
                }
                if (src) {
                    memcpy(buf, src + within, len);
                    mutexUnlock(&t->lock);
                    return true;
                }
                mutexUnlock(&t->lock);
            }
            return false;
        }
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
    int64_t plen = torrent_piece_len(&t->meta, idx);
    // The piece may have completed since the caller looked in the writer
    // queue: piece_full() moves it from its slot to the queue under this very
    // lock. Check both here, in ONE critical section, or a read landing in
    // that gap finds the piece nowhere and comes back empty (the caller then
    // returned 0 = "no data" mid-file). The writer cannot move it on into the
    // RAM window meanwhile: that needs cache_lock, which the caller holds.
    uint8_t *full = NULL;
    if (t->writing_idx == idx && t->writing_buf) {
        full = t->writing_buf;
    } else {
        for (int i = 0; i < t->wq_n; i++) {
            int slot = (t->wq_head + i) % WQ_CAP;
            if (t->wq[slot].idx == idx && t->wq[slot].buf) { full = t->wq[slot].buf; break; }
        }
    }
    if (full) {
        size_t n = len;
        if (within + (int64_t)n > plen) n = (size_t)(plen - within);
        memcpy(dst, full + within, n);
        mutexUnlock(&t->lock);
        return n;
    }
    aq_entry *a = aq_find(t, idx);
    if (!a || a->idx != idx) {
        mutexUnlock(&t->lock);
        return 0;
    }
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
        if (!t->ram_mode || t->strict_verify) break;
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
    int64_t pin_limit = t->file_first_piece +
        ((PIN_HEAD_BYTES + t->meta.piece_len - 1) / t->meta.piece_len);
    if (t->ram_lo < pin_limit) t->ram_lo = pin_limit;

    // Keep at most ~32 MB (or at least 4 pieces) behind playhead for slight seek resilience.
    int64_t keep_behind = (32LL * 1024 * 1024) / t->meta.piece_len;
    if (keep_behind < 4) keep_behind = 4;
    int64_t safe_ph = (ph > keep_behind) ? ph - keep_behind : 0;
    if (safe_ph < pin_limit) safe_ph = pin_limit;

    while (t->ram_resident > t->ram_budget || (t->ram_lo < safe_ph && t->ram_resident > (64LL << 20))) {
        int64_t target_ph = (t->ram_resident > t->ram_budget) ? ph : safe_ph;
        while (t->ram_lo < target_ph && !t->ram_piece[t->ram_lo]) t->ram_lo++;
        if (t->ram_lo >= target_ph) break;     // nothing strictly behind left to drop
        int64_t v = t->ram_lo;
        uint8_t *freed_buf = t->ram_piece[v];
        t->ram_piece[v] = NULL;
        t->ram_resident -= t->meta.piece_len;
        mutexLock(&t->lock);
        if (t->status[v] == PIECE_DONE) { t->status[v] = PIECE_NEEDED; t->pieces_done--; }
        // Recycle buffer into an empty aq slot or into a small bounded free pool
        bool recycled = false;
        for (int i = 0; i < t->n_aq; i++) {
            if (t->aq[i].idx < 0 && !t->aq[i].buf) {
                t->aq[i].buf = freed_buf;
                recycled = true;
                break;
            }
        }
        if (!recycled && t->aq_buf_pool_cnt < 4 &&
            (int64_t)t->aq_buf_pool_cnt * t->meta.piece_len < (64LL << 20)) {
            t->aq_buf_pool[t->aq_buf_pool_cnt++] = freed_buf;
            recycled = true;
        }
        mutexUnlock(&t->lock);
        if (!recycled) free(freed_buf);
    }
}

// Move a verified piece buffer into the window, taking ownership of buf.
static void ram_store(torrentfs *t, int64_t idx, uint8_t *buf) {
    mutexLock(&t->cache_lock);
    if (t->ram_piece[idx]) {            // a re-download of a still-resident piece
        uint8_t *old_buf = t->ram_piece[idx];
        t->ram_piece[idx] = NULL;
        t->ram_resident -= t->meta.piece_len;
        mutexLock(&t->lock);
        bool recycled = false;
        for (int i = 0; i < t->n_aq; i++) {
            if (t->aq[i].idx < 0 && !t->aq[i].buf) {
                t->aq[i].buf = old_buf;
                recycled = true;
                break;
            }
        }
        if (!recycled && t->aq_buf_pool_cnt < 4 &&
            (int64_t)t->aq_buf_pool_cnt * t->meta.piece_len < (64LL << 20)) {
            t->aq_buf_pool[t->aq_buf_pool_cnt++] = old_buf;
            recycled = true;
        }
        mutexUnlock(&t->lock);
        if (!recycled) free(old_buf);
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
    // O(1): aq_of_piece is set when a slot takes a piece and never cleared --
    // a stale hint simply fails the idx check below (the slot moved on).
    if (idx < 0 || idx >= t->meta.piece_count || !t->aq_of_piece) return NULL;
    int i = t->aq_of_piece[idx];
    if (i >= 0 && i < t->n_aq && t->aq[i].idx == idx) return &t->aq[i];
    return NULL;
}

// A free slot WITH a buffer (buffers move to the writer and come back; a slot
// whose buffer is out cannot start a new piece). The scan-and-take runs under
// t->lock: the writer picks a return slot by the same idx/buf fields, and a
// half-taken slot seen from the other thread would double-assign a buffer.
// The bitmap reset ALSO runs under the lock: the streaming reader (RAM mode)
// reads have[] of an active piece, and a reset visible mid-assignment would
// hand it stale blocks.
// Room to open one more piece without the writer queue ever overflowing:
// every active piece can add one job. Caller holds t->lock (or reads racily
// as a pre-check: the writer only ever shrinks wq_n).
static bool wq_has_room(const torrentfs *t) {
    return t->wq_n + t->n_active_aq < WQ_CAP;
}

static aq_entry *aq_alloc(torrentfs *t, int64_t idx) {
    aq_entry *a = NULL;
    mutexLock(&t->lock);
    if (!wq_has_room(t)) {           // backpressure: the writer is behind
        t->st_wq_full++;
        mutexUnlock(&t->lock);
        return NULL;
    }
    for (int i = 0; i < t->n_aq; i++) {
        aq_entry *c = &t->aq[i];
        if (c->idx >= 0) continue;
        if (!c->buf) {
            if (t->aq_buf_pool_cnt > 0) {
                c->buf = t->aq_buf_pool[--t->aq_buf_pool_cnt];
            } else {
                c->buf = malloc((size_t)t->meta.piece_len);
            }
        }
        if (!c->buf) continue;
        c->idx = idx;
        t->n_active_aq++;
        c->have_cnt = 0;
        c->next_req = 0;
        c->last_deliver = armGetSystemTick();
        c->nblocks  =
            (int)((torrent_piece_len(&t->meta, idx) + BLOCK_LEN - 1) / BLOCK_LEN);
        memset(c->have, 0, (size_t)t->blocks_per_piece);
        memset(c->req, 0, (size_t)t->blocks_per_piece);
        memset(c->req_ts, 0, (size_t)t->blocks_per_piece * sizeof(uint64_t));
        memset(c->req_by, 0, (size_t)t->blocks_per_piece * SESS_MASK_BYTES);
        if (t->aq_of_piece) t->aq_of_piece[idx] = (int16_t)i;
        if (!c->peer_from) {
            c->peer_from = malloc((size_t)t->blocks_per_piece * sizeof(int16_t));
        }
        if (c->peer_from) {
            memset(c->peer_from, 0xFF, (size_t)t->blocks_per_piece * sizeof(int16_t));
        }
        a = c;
        break;
    }
    mutexUnlock(&t->lock);
    return a;
}

//-----------------------------------------------------------------------------
// Peer pool (shared with the discovery thread -> under t->lock)
//-----------------------------------------------------------------------------

static inline int peer_eval_score(const torrentfs *t, int j) {
    int score = 0;
    if (t->peer_ok[j]) score += 1000;
    if (t->peer_src[j] == PEER_SRC_TRACKER) score += 500;
    else if (t->peer_src[j] == PEER_SRC_PEX) score += 400;
    else score += 100; // DHT

    if (t->peer_fails[j] == 0) {
        score += 200;
    } else {
        score -= (int)t->peer_fails[j] * 40;
    }
    // Speed history: an address that was fast before is dialed first.
    float kb = t->peer_best_kbps[j];
    if (kb >= 4096.0f) score += 1500;
    else if (kb >= 1024.0f) score += 1000;
    else if (kb >= 256.0f) score += 400;
    else if (kb >= 64.0f) score += 100;
    score -= (int)t->peer_rot[j] * 300;
    return score;
}

static inline uint32_t pool_hash_of(uint32_t ip, uint16_t port) {
    return ((ip ^ ((uint32_t)port * 0x9E3779B1u)) * 2654435761u) >> (32 - 12);   // 12 bits
}

// Caller holds t->lock. Pool slot of ip:port, or -1.
static int pool_find(const torrentfs *t, uint32_t ip, uint16_t port) {
    for (uint32_t h = pool_hash_of(ip, port), k = 0; k < POOL_HASH_SIZE; k++, h = (h + 1) & (POOL_HASH_SIZE - 1)) {
        int v = t->pool_hash[h];
        if (v == 0) return -1;
        if (t->peers[v - 1].ip == ip && t->peers[v - 1].port == port) return v - 1;
    }
    return -1;
}

static void pool_hash_insert(torrentfs *t, uint32_t ip, uint16_t port, int slot) {
    uint32_t h = pool_hash_of(ip, port);
    while (t->pool_hash[h] != 0) h = (h + 1) & (POOL_HASH_SIZE - 1);
    t->pool_hash[h] = (int16_t)(slot + 1);
}

// Remove ip:port (backward-shift deletion keeps every probe chain unbroken).
// Must run BEFORE peers[slot] is overwritten: probing compares stored keys.
static void pool_hash_remove(torrentfs *t, uint32_t ip, uint16_t port) {
    uint32_t h = pool_hash_of(ip, port);
    for (;;) {
        int v = t->pool_hash[h];
        if (v == 0) return;
        if (t->peers[v - 1].ip == ip && t->peers[v - 1].port == port) break;
        h = (h + 1) & (POOL_HASH_SIZE - 1);
    }
    uint32_t hole = h;
    for (uint32_t j = (hole + 1) & (POOL_HASH_SIZE - 1);; j = (j + 1) & (POOL_HASH_SIZE - 1)) {
        int v = t->pool_hash[j];
        if (v == 0) break;
        uint32_t home = pool_hash_of(t->peers[v - 1].ip, t->peers[v - 1].port);
        // Move j into the hole unless its home lies cyclically in (hole, j].
        bool in_range = (hole <= j) ? (home > hole && home <= j)
                                    : (home > hole || home <= j);
        if (!in_range) {
            t->pool_hash[hole] = (int16_t)v;
            hole = j;
        }
    }
    t->pool_hash[hole] = 0;
}

static void add_peers_src(torrentfs *t, const peer_addr *peers, int n, int src) {
    int added = 0;
    mutexLock(&t->lock);
    for (int i = 0; i < n; i++) {
        // Port 0/1 are never real listen ports: some DHT nodes store (and
        // happily serve) announce_peer entries with port 1 because jech/dht
        // only rejects port 0. Dialing them just burns the dial budget.
        if (peers[i].port <= 1) continue;
        bool dup = false;
        int j = pool_find(t, peers[i].ip, peers[i].port);
        if (j >= 0) {
                // Known address: upgrade provenance if the source is better
                if ((uint8_t)src < t->peer_src[j]) t->peer_src[j] = (uint8_t)src;
                // If we are starved and tracker/PEX reports this peer again, give it a fresh chance
                if (t->st_live < STARVED_LIVE && t->peer_fails[j] > 0 && !t->peer_busy[j]) {
                    t->peer_fails[j] = 0;
                    t->peer_next_try[j] = 0;
                }
                dup = true;
        }
        if (dup) continue;
        int slot = -1;
        if (t->peer_count < TFS_MAX_PEERS) {
            slot = t->peer_count++;
        } else {
            // Pool full: find the lowest scoring entry to evict (anacrolix dropCandidate model).
            // A dead DHT entry that failed repeatedly has the lowest score and will be evicted first.
            int incoming_score = (src == PEER_SRC_TRACKER) ? 700 : (src == PEER_SRC_PEX ? 600 : 300);
            int worst = -1;
            int worst_score = 999999;
            for (int k = 0; k < POOL_EVICT_SAMPLE; k++) {
                int e = t->peer_evict_rr;
                t->peer_evict_rr = (t->peer_evict_rr + 1) % TFS_MAX_PEERS;
                if (t->peer_busy[e]) continue;
                int score = peer_eval_score(t, e);
                if (score < worst_score) {
                    worst_score = score;
                    worst = e;
                    if (score <= -20) break; // Dead entry, evict immediately
                }
            }
            if (worst >= 0 && (worst_score < incoming_score || worst_score <= 300)) {
                slot = worst;
                pool_hash_remove(t, t->peers[slot].ip, t->peers[slot].port);   // before overwriting
            }
        }
        if (slot < 0) continue;
        t->peers[slot]         = peers[i];
        pool_hash_insert(t, peers[i].ip, peers[i].port, slot);
        t->peer_fails[slot]    = 0;
        t->peer_fail_kind[slot]= PEER_FAIL_NONE;
        t->peer_sha_fails[slot]= 0;
        t->peer_next_try[slot] = 0;
        t->peer_ok[slot]       = 0;
        t->peer_src[slot]      = (uint8_t)src;
        t->peer_best_kbps[slot] = 0.0f;
        t->peer_rot[slot]      = 0;
        added++;
    }
    mutexUnlock(&t->lock);
    // Log outside the lock: engine_log can block (stderr pipe on PC builds)
    // and a lock held across a blocking write freezes every thread that
    // needs it.
    if (added > 0)
        engine_log(ENGINE_LOG_DEBUG, "[torrentfs] added %d peers (total %d)", added, t->peer_count);
}

static void add_peers_tracker_cb(void *ctx, const peer_addr *peers, int n) {
    add_peers_src(ctx, peers, n, PEER_SRC_TRACKER);
}

static void add_peers_dht_cb(void *ctx, const peer_addr *peers, int n) {
    add_peers_src(ctx, peers, n, PEER_SRC_DHT);
}

// Take a pool peer with candidate scoring (anacrolix priority model).
// Proven peers (peer_ok) and live tracker/PEX peers get first priority,
// while stale or failing DHT addresses are only dialed when nothing better is ready.
static int take_peer(torrentfs *t, peer_addr *out) {
    u64 now = armGetSystemTick();
    mutexLock(&t->lock);
    int best_idx = -1;
    int best_score = -999999;
    // Sessions per IP: at most MAX_SESS_PER_IP outbound (distinct ports), and
    // none at all to an IP that already dialed us. Built once per call into a
    // small hash table (the per-entry scan of every session was O(pool x sessions)).
    enum { IPTAB = 256 };   // power of two, > 2 * MAX_SESS
    uint32_t tab_ip[IPTAB];
    uint8_t tab_n[IPTAB];   // outbound sessions; 0xFF = has an incoming one
    memset(tab_n, 0, sizeof(tab_n));
    for (int k = 0; k < MAX_SESS; k++) {
        const sess *ss = &t->S[k];
        if (!ss->active) continue;
        uint32_t h = (ss->addr.ip * 2654435761u) >> 24;
        while (tab_n[h] && tab_ip[h] != ss->addr.ip) h = (h + 1) & (IPTAB - 1);
        tab_ip[h] = ss->addr.ip;
        if (ss->pidx < 0) tab_n[h] = 0xFF;
        else if (tab_n[h] != 0xFF) tab_n[h]++;
    }
    // Every 4th dial goes to an address never tried before, so the pool keeps
    // being explored even when known-good addresses are ready.
    const bool explore = (t->dial_seq++ % 4) == 3;
    if (!t->stop && t->peer_count > 0) {
        for (int tries = 0; tries < t->peer_count; tries++) {
            int i = (t->next_peer + tries) % t->peer_count;
            if (t->peer_busy[i]) continue;
            if (t->peer_next_try[i] > now) continue;

            {
                uint32_t ip = t->peers[i].ip;
                uint32_t h = (ip * 2654435761u) >> 24;
                while (tab_n[h] && tab_ip[h] != ip) h = (h + 1) & (IPTAB - 1);
                if (tab_n[h] && tab_n[h] >= MAX_SESS_PER_IP) continue;   // 0xFF counts too
            }

            int score = peer_eval_score(t, i);
            if (explore && t->peer_fails[i] == 0 && !t->peer_ok[i] &&
                t->peer_best_kbps[i] == 0.0f && t->peer_rot[i] == 0)
                score += 3000;
            if (score > best_score) {
                best_score = score;
                best_idx = i;
                // Known-fast (or, exploring, untried) address: no need to look on.
                if (score >= 3000) break;
            }
        }
        if (best_idx >= 0) {
            t->next_peer = (best_idx + 1) % t->peer_count;
            *out = t->peers[best_idx];
            t->peer_busy[best_idx] = 1;
        }
    }
    mutexUnlock(&t->lock);
    return best_idx;
}

static void release_peer(torrentfs *t, int pidx, bool failed, bool had_conn,
                         int fail_kind) {
    if (pidx < 0) return;
    mutexLock(&t->lock);
    t->peer_busy[pidx] = 0;
    if (failed || !t->peer_ok[pidx]) {
        if (fail_kind != PEER_FAIL_CHOKE_ROTATE && t->peer_fails[pidx] < 0xFE)
            t->peer_fails[pidx]++;
        t->peer_fail_kind[pidx] = (uint8_t)fail_kind;
        int f = t->peer_fails[pidx];
        int secs;
        if (fail_kind == PEER_FAIL_CHOKE_ROTATE) {
            // A seed that rotated us out is not a dead address: the minimal
            // backoff gets us back into its next optimistic-unchoke window.
            secs = BACKOFF_DROP_SECS;
        } else if (fail_kind == PEER_FAIL_TIMEOUT && !had_conn) {
            // Dead/unreachable address (TCP connect never answered). Park it
            // for a long time on the FIRST miss: re-dialing dead DHT entries
            // every few seconds burns the dial budget that fresh peers need
            // (measured: ~half of all dial slots went to re-dialing dead
            // addresses).
            // Tracker / PEX addresses were reported live moments ago: their
            // one retry (with CONNECT_RETRY_SECS patience, see sess_dial)
            // comes after 30 s; cached DHT entries keep the 60 s park.
            // Third miss and later: 5 minutes (a pool of ~580 mostly dead
            // DHT addresses otherwise costs ~5 dials/s at the 2-minute park).
            secs = f > 2 ? BACKOFF_DEAD_SECS
                 : f > 1 ? BACKOFF_MAX_SECS
                         : (t->peer_src[pidx] == PEER_SRC_DHT ? 60 : 30);
        } else if (!had_conn &&
                   t->st_piece_ok < BOOTSTRAP_THRESH &&
                   t->peer_src[pidx] == PEER_SRC_TRACKER) {
            // During bootstrap, tracker-reported addresses are live seeders:
            // keep the DIAL cadence flat so a missed optimistic slot is
            // retried before the next one passes. A peer that DID complete
            // a connection falls through to the exponential ladder below --
            // flat 5 s redials of a seed that connects but sends nothing is
            // a reconnect loop, not recovery (measured in the field: the
            // same silent seed re-dialed every ~30 s for 10+ minutes).
            secs = BACKOFF_DROP_SECS;
        } else {
            // Exponential backoff for repeat failures to prevent 11s reconnect storms
            int shift = (f > 1) ? (f - 1) : 0;
            if (shift > 5) shift = 5;
            secs = BACKOFF_CONN_SECS * (1 << shift);
            if (had_conn && f == 1) secs = BACKOFF_DROP_SECS;
            if (t->peer_ok[pidx] && (t->peer_count <= 4 || t->st_live <= 2)) secs = BACKOFF_DROP_SECS;
        }
        // A peer that was fast before and dropped on a network error (not a
        // rotation) is redialed quickly, its first few times.
        if (had_conn && fail_kind != PEER_FAIL_CHOKE_ROTATE &&
            t->peer_best_kbps[pidx] >= 1024.0f && t->peer_fails[pidx] <= 3)
            secs = BACKOFF_DROP_SECS;
        const int cap = (fail_kind == PEER_FAIL_TIMEOUT && !had_conn) ? BACKOFF_DEAD_SECS
                                                                       : BACKOFF_MAX_SECS;
        if (secs > cap) secs = cap;
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
    // O(1): have_in_file is kept current on BITFIELD / HAVE / file switch
    // (it used to scan every piece of the file, per session, per upkeep tick).
    return s->nb.bitfield &&
           s->have_in_file == (int)(t->file_last_piece - t->file_first_piece + 1);
}

// Recount a session's pieces inside the streamed file (BITFIELD, file switch).
static int count_have_in_file(const torrentfs *t, const sess *s) {
    if (!s->nb.bitfield) return 0;
    int n = 0;
    for (int64_t p = t->file_first_piece; p <= t->file_last_piece; p++)
        if (bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, p)) n++;
    return n;
}

// Does this peer have pieces we currently need (the read-blocked piece or
// pieces in the active streaming window)? Such peers must not be prematurely
// dropped on choke-rotation when the swarm is scarce.
static bool peer_has_needed(torrentfs *t, sess *s) {
    if (!s->nb.bitfield) return false;
    if (t->read_blocked_piece >= 0 &&
        bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, t->read_blocked_piece))
        return true;
    int64_t ph = t->playhead_piece;
    if (ph < t->file_first_piece) ph = t->file_first_piece;
    int64_t window_end = ph + 32;
    if (window_end > t->file_last_piece || t->file_last_piece - ph <= 64)
        window_end = t->file_last_piece;
    for (int64_t p = ph; p <= window_end; p++) {
        if (t->status[p] != PIECE_DONE &&
            bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, p))
            return true;
    }
    return false;
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
            // The flag is the one-shot permission: the TCP backoff does not
            // apply, a ban (next_try far in the future) does.
            if (t->peer_next_try[i] > now + (u64)BACKOFF_MAX_SECS * t->freq) continue;
            idx = i;
            *out = t->peers[i];
            t->peer_busy[i] = 1;
            t->peer_utp_retry[i] = 0;   // one attempt, then normal backoff
            // Dialing now: clear the TCP backoff so the upkeep's "banned"
            // check (next_try > now + 60 s) does not kill this session.
            t->peer_next_try[i] = now;
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
    bool d = true;
    for (int64_t p = t->file_first_piece; p <= t->file_last_piece; p++) {
        if (!(t->ever[p >> 3] & (1u << (p & 7)))) {
            d = false;
            break;
        }
    }
    mutexUnlock(&t->lock);
    return d;
}

static bool discovery_wait(torrentfs *t) {
    u64 start = armGetSystemTick();
    while (!t->stop) {
        // 1 s between checks, in 50 ms steps: torrentfs_close() joins this
        // thread, and a plain 1 s sleep made every remove/cancel wait for it.
        for (int i = 0; i < 20 && !t->stop && !t->announce_now; i++)
            svcSleepThread(50000000ULL);
        if (t->stop) break;
        tsnx_engine_wd_tick(1);
        if (t->announce_now) {
            t->announce_now = false;
            return true;
        }
        if (file_done(t)) {
            start = armGetSystemTick();
            continue;
        }
        u64 secs = (armGetSystemTick() - start) / t->freq;
        if (secs >= DISC_INTERVAL_SECS) return true;
        if (secs >= 30 && t->st_live == 0 && t->peer_count < 32) return true;
        if (secs >= DISC_STARVED_SECS && t->st_live < STARVED_LIVE && t->peer_count < 32) return true;
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

// Count a piece of the active file the first time it completes. pieces_done
// tracks what is resident *now* (it falls when the RAM window evicts), so it is
// the wrong number for a progress bar; this one only climbs within the active file.
// Caller holds t->lock.
static void mark_ever_done(torrentfs *t, int64_t idx) {
    if (idx < t->file_first_piece || idx > t->file_last_piece) return;
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
        if (t->wq_n == 0 && !t->stop)   // 50 ms cap keeps the heartbeat alive
            condvarWaitTimeout(&t->wq_cv, &t->lock, 50000000ULL);
        if (t->wq_n > 0) {
            j = t->wq[t->wq_head];
            t->wq_head = (t->wq_head + 1) % WQ_CAP;
            t->wq_n--;
            t->writing_idx = j.idx;
            t->writing_buf = j.buf;
            has = true;
        }
        bool stopping = t->stop;
        mutexUnlock(&t->lock);

        if (!has) {
            if (stopping) break;   // drained: nothing more can be queued
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
            condvarWakeAll(&t->piece_cv);
            mark_ever_done(t, j.idx);
            t->st_piece_ok++;
            t->st_cache_written += j.plen;
            engine_log(ENGINE_LOG_DEBUG, "[writer] piece %lld verified (RAM)", (long long)j.idx);
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
                condvarWakeAll(&t->piece_cv);
                mark_ever_done(t, j.idx);
                t->st_piece_ok++;
                t->st_cache_written += j.plen;
                engine_log(ENGINE_LOG_DEBUG, "[writer] piece %lld verified (SD)", (long long)j.idx);
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

            // Peer attribution and corrupt peer banning
            if (j.peer_from && nb > 0) {
                u64 now_t = armGetSystemTick();
                for (int b = 0; b < nb; b++) {
                    int16_t p = j.peer_from[b];
                    if (p >= 0 && p < TFS_MAX_PEERS) {
                        int cnt = 0;
                        for (int k = 0; k < nb; k++) {
                            if (j.peer_from[k] == p) cnt++;
                        }
                        if (cnt * 4 >= nb || (nb <= 4 && cnt >= 1)) {
                            t->peer_sha_fails[p]++;
                            if (t->peer_sha_fails[p] >= 2 || cnt * 2 >= nb) {
                                t->peer_next_try[p] = now_t + (u64)1800 * t->freq;
                                t->peer_ok[p] = 0;
                                engine_log(ENGINE_LOG_WARN,
                                           "[writer] banning corrupt peer %d for 30m (sha fails: %d, blocks: %d/%d)",
                                           p, t->peer_sha_fails[p], cnt, nb);
                            }
                            for (int k = b + 1; k < nb; k++) {
                                if (j.peer_from[k] == p) j.peer_from[k] = -1;
                            }
                        }
                    }
                }
            }
        }
        t->st_blocks_have -= nb;
        t->writing_idx = -1;
        t->writing_buf = NULL;
        // Return the buffer to a bufferless slot for reuse, or pool it (bounded).
        for (int i = 0; i < t->n_aq; i++) {
            if (t->aq[i].idx < 0) {
                if (!t->aq[i].buf && j.buf) { t->aq[i].buf = j.buf; j.buf = NULL; }
                if (!t->aq[i].peer_from && j.peer_from) { t->aq[i].peer_from = j.peer_from; j.peer_from = NULL; }
            }
        }
        if (j.buf && t->aq_buf_pool_cnt < 4 &&
            (int64_t)t->aq_buf_pool_cnt * t->meta.piece_len < (64LL << 20)) {
            t->aq_buf_pool[t->aq_buf_pool_cnt++] = j.buf;
            j.buf = NULL;
        }
        mutexUnlock(&t->lock);
        if (j.buf) free(j.buf);   // pool full: don't leak
        if (j.peer_from) free(j.peer_from);
    }
}

//-----------------------------------------------------------------------------
// Sessions
//-----------------------------------------------------------------------------

static void sess_close(torrentfs *t, sess *s, bool failed) {
    if (!s->active) return;
    sess_drop_requests(t, s);   // free per-piece request counts, no cancels
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
        if (t->st_connecting > 0) t->st_connecting--;
    } else {
        if (s->nb.handshaked && t->st_live > 0) t->st_live--;
        peer_nb_free(&s->nb);
    }
    int kind = s->fail_kind ? s->fail_kind
                            : (failed ? PEER_FAIL_OTHER : PEER_FAIL_NONE);
    if (s->pidx >= 0 && s->svc_ticks > 0) {
        float kb = (float)(sess_capability(t, s) / 1024.0);
        mutexLock(&t->lock);
        if (kb > t->peer_best_kbps[s->pidx]) t->peer_best_kbps[s->pidx] = kb;
        mutexUnlock(&t->lock);
    }
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
        // Flag only: the TCP backoff that release_peer just set stays in
        // force. take_utp_retry_peer() ignores it for the one uTP attempt.
        // Resetting next_try to "now" here (as before) also made the address
        // due for plain TCP again at once: dead addresses were re-dialed ~35
        // times each (20k dials, 95 % timeouts, in one 24-minute download).
        mutexLock(&t->lock);
        t->peer_utp_retry[s->pidx] = 1;
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
    if ((t->peer_fail_kind[pidx] == PEER_FAIL_TIMEOUT && t->peer_fails[pidx] <= 3) ||
        t->peer_ok[pidx] || t->peer_count <= 4)
        secs = CONNECT_RETRY_SECS;
    mutexUnlock(&t->lock);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        t->st_sock_fail++;
        t->st_conn_fail++;
        if (errno == ENOBUFS) t->st_enobufs++;
        set_err(t, "socket(): errno %lld", (long long)errno);
        // Local descriptor/buffer exhaustion; do not penalize the remote peer.
        release_peer(t, pidx, false, false, PEER_FAIL_NONE);
        return false;
    }
#ifdef __SWITCH__
    // Lean buffers while dialing: most dials fail, and every socket draws on
    // the one process-wide BSD pool (main.cpp). Raised after the handshake.
    int rcvbuf = 32 * 1024;
    int sndbuf = 16 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#endif
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
        int cerr = errno;   // before anything that may clobber it
        t->st_sock_fail++;
        t->st_conn_fail++;
        engine_log(ENGINE_LOG_DEBUG, "[sess %d] tcp connect failed errno=%d", sid, cerr);
        close(sock);
        if (cerr == ENOBUFS) t->st_enobufs++;
        bool is_local = (cerr == ENOBUFS || cerr == ENOMEM || cerr == EMFILE || cerr == ENFILE);
        release_peer(t, pidx, !is_local, false, is_local ? PEER_FAIL_NONE : PEER_FAIL_OTHER);
        return true;   // slot still free; try another peer next tick
    }

    sess *s = &t->S[sid];
    memset(s, 0, sizeof(*s));
    s->active      = true;
    s->connecting  = true;
    s->nb.sock     = sock;   // raw until the connect completes
    s->addr        = pa;
    s->pidx        = pidx;
    s->started     = now;
    s->choked_at   = now;
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
    s->started     = now;
    s->choked_at   = now;
    s->last_rx     = now;
    s->last_ka     = now;
    s->conn_budget = now + (u64)10 * t->freq;  // uTP SYN exchange patience

    if (peer_nb_init_utp(&s->nb, us, t->meta.piece_count) != 0) {
        utp_nb_close(us);
        s->active = false;
        s->connecting = false;
        release_peer(t, pidx, false, false, PEER_FAIL_NONE);
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
    if (t->st_connecting > 0) t->st_connecting--;
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

// Streaming window, for the debug panel. The request scheduler itself works
// off the piece-priority gradient (piece_priority), not this window.
static void calc_window(torrentfs *t, int64_t *ph, int64_t *lo, int64_t *hi) {
    int64_t p = t->playhead_piece;   // lock-free read
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
        int64_t cap = (t->ram_budget * 3 / 4) / t->meta.piece_len;
        if (cap < STREAM_MIN_PIECES) cap = STREAM_MIN_PIECES;
        if (win > cap) win = cap;
    }
    int64_t h = p + win;
    if (h > fhi + 1) h = fhi + 1;
    *ph = p; *lo = flo; *hi = h;
}

// The header/moov probe pieces (the ends of the file). These are fetched
// speculatively at startup and must not be preempted when the playhead moves --
// mpv still needs them to demux even after it seeks to a resume position.
static bool is_critical(torrentfs *t, int64_t idx) {
    return idx < t->file_first_piece + t->crit_head ||
           idx > t->file_last_piece - t->crit_tail;
}

// Send one MSG_CANCEL for a single (piece, block) on a session's wire
// (anacrolix postCancel: when a block arrives, its duplicate copies in
// flight on other sessions are cancelled so they stop wasting bandwidth).
static void send_cancel_one(torrentfs *t, sess *s, int ep, int eb) {
    if (!s->nb.handshaked || s->connecting) return;
    if (ep < 0 || ep >= t->meta.piece_count) return;
    uint8_t pl[12];
    uint32_t v;
    v = htonl((uint32_t)ep);        memcpy(pl, &v, 4);
    v = htonl((uint32_t)eb * BLOCK_LEN); memcpy(pl + 4, &v, 4);
    v = htonl(block_len_of(torrent_piece_len(&t->meta, ep), eb));
    memcpy(pl + 8, &v, 4);
    peer_nb_queue(&s->nb, MSG_CANCEL, pl, 12);
    // Flush immediately to drop duplicate transfers on the wire right away
    if (peer_nb_tx_pending(&s->nb)) {
        if (peer_nb_flush(&s->nb) != 0) {
            sess_close(t, s, true);
        }
    }
}

// Drop a session's outstanding requests without cancelling them on the wire:
// anacrolix never cancels on release -- the peer's copies, if any, land and
// get deduplicated. The per-piece request counts are freed so other sessions
// may re-request the blocks after the duplicate timer expires. Used when the
// peer chokes us or when the session closes.
static void sess_drop_requests(torrentfs *t, sess *s) {
    int sid = (int)(s - t->S);
    for (int i = 0; i < s->req_n; i++) {
        int ep = s->req_piece[i];
        int eb = s->req_block[i];
        aq_entry *ea = aq_find(t, ep);
        if (ea && eb >= 0 && eb < ea->nblocks) {
            if (ea->req[eb] > 0 && ea->req[eb] < 255) ea->req[eb]--;
            rb_clr(ea, eb, sid);
        }
    }
    s->req_n = 0;
}

// The effective request priority of a piece:
// Zone 1: Read-blocked / current playhead piece
// Zone 2: Next piece
// Zone 3: Readahead (up to 5 pieces)
// Zone 4: Speculative (up to 15 pieces)
// Zone 5: Normal pieces of current file, and lookahead pieces into next files.
// True if the file holding piece idx's first byte was deselected.
static bool piece_file_unwanted(const torrentfs *t, int64_t idx) {
    if (!t->file_unwanted || t->meta.file_count <= 0) return false;
    int64_t at = idx * t->meta.piece_len;
    int lo = 0, hi = t->meta.file_count - 1;
    while (lo < hi) {   // last file with offset <= at
        int mid = (lo + hi + 1) / 2;
        if (t->meta.files[mid].offset <= at) lo = mid; else hi = mid - 1;
    }
    return t->file_unwanted[lo] != 0;
}

static int piece_priority_rb(torrentfs *t, int64_t idx, int64_t rb) {
    if (idx < t->file_first_piece) return 0;
    if (idx > t->file_last_piece + LOOKAHEAD_PIECES || idx >= t->meta.piece_count) return 0;
    if (idx == rb) return 1;
    int64_t ph = t->playhead_piece;
    if (ph < t->file_first_piece) ph = t->file_first_piece;
    if (ph > t->file_last_piece) ph = t->file_last_piece;
    if (idx > t->file_last_piece) {
        int64_t plen = t->meta.piece_len > 0 ? t->meta.piece_len : (1LL << 20);
        int64_t lookahead = (64LL << 20) / plen;
        if (lookahead < 1) lookahead = 1;
        if (lookahead > LOOKAHEAD_PIECES) lookahead = LOOKAHEAD_PIECES;
        if (idx <= t->file_last_piece + lookahead && (t->file_last_piece - ph) <= 4 &&
            !piece_file_unwanted(t, idx)) {
            return 5;
        }
        return 0;
    }
    int z = t->piece_zone[idx];
    if (z != 0) return z;
    if (t->has_custom_zones) {
        // When an external scheduler has defined the active window, any unzoned
        // piece outside that window is NOT wanted right now (prevents out-of-order downloads).
        return 0;
    }
    if (is_critical(t, idx))
        return idx < t->file_first_piece + t->crit_head ? 1 : 2;
    int64_t d = idx - ph;
    int max_d = t->n_aq > 2 ? t->n_aq - 2 : 24;
    if (d < 0 || d > max_d) {
        return 0;
    }
    if (d == 0) return 1;
    if (d == 1) return 2;
    if (d <= 5) return 3;
    if (d <= 15) return 4;
    return 5;
}

static int piece_priority(torrentfs *t, int64_t idx) {
    return piece_priority_rb(t, idx, t->read_blocked_piece);
}

// Rebuild the candidate list if any input of piece_priority (other than the
// read-blocked piece) changed since the last build. Netloop only.
static void cand_refresh(torrentfs *t) {
    uint32_t gen = __atomic_load_n(&t->zone_gen, __ATOMIC_ACQUIRE);
    int64_t ph = t->playhead_piece;
    if (t->cand_valid && ph == t->cand_ph && gen == t->cand_gen &&
        t->file_first_piece == t->cand_first && t->file_last_piece == t->cand_last)
        return;
    t->cand_valid = false;
    if (!t->cand || !t->cand_tmp) return;
    t->cand_ph = ph;
    t->cand_gen = gen;
    t->cand_first = t->file_first_piece;
    t->cand_last = t->file_last_piece;
    t->pf_rebuilds++;

    int64_t fhi = t->file_last_piece + LOOKAHEAD_PIECES;
    if (fhi >= t->meta.piece_count) fhi = t->meta.piece_count - 1;
    int64_t flo = (ph > t->file_first_piece) ? ph : t->file_first_piece;
    int count[6] = {0};
    for (int64_t i = flo; i <= fhi; i++) {
        int z = piece_priority_rb(t, i, -1);
        if (z < 1 || z > 5) z = 0;
        t->cand_tmp[i - flo] = (uint8_t)z;
        count[z]++;
    }
    t->cand_end[0] = 0;
    for (int z = 1; z <= 5; z++) t->cand_end[z] = t->cand_end[z - 1] + count[z];
    int pos[6];
    for (int z = 1; z <= 5; z++) pos[z] = t->cand_end[z - 1];
    for (int64_t i = flo; i <= fhi; i++) {
        int z = t->cand_tmp[i - flo];
        if (z) t->cand[pos[z]++] = (int32_t)i;
    }
    t->cand_valid = true;
}
// Per-session in-flight request budget: dynamic window scaled by rate EMA and
// connection RTT (~2.0s of useful throughput) with TCP slow-start fast-ramp.
// Active unchoked peers ramp up their pipeline as blocks land without strikes,
// so fast peers are never bottlenecked by static initial floors.
static int nominal_depth(torrentfs *t, sess *s) {
    if (s->nb.choked || s->snubbed) return REQ_MIN;
    bool is_seed = peer_is_seed(t, s);
    if (!is_seed && s->timeout_strikes >= 3) return REQ_MIN;
    if (s->expect_ticks <= t->freq) return (t->st_live <= 2) ? 48 : REQ_INITIAL;

    double rate_bps = (s->rate > 0) ? (s->rate / (double)BLOCK_LEN) : 0.0;
    double life_bps = (s->expect_ticks > 0)
        ? ((double)s->useful * (double)t->freq / (double)s->expect_ticks)
        : 0.0;
    u64 now = armGetSystemTick();
    bool stalling = (s->req_n > 0 && s->last_block > 0 &&
                     now - s->last_block > (u64)5 * t->freq);
    double blocks_per_sec = (!stalling && life_bps > rate_bps) ? life_bps : rate_bps;

    // Bandwidth-Delay Product: target ~2.0s of throughput buffer (up to 256 blocks = 4 MB)
    int d = (int)(blocks_per_sec * 2.0);
    int floor_d;
    if (is_seed) {
        // Full seeds have 100% data: never choke floor below REQ_INITIAL
        floor_d = REQ_INITIAL;
    } else if (s->timeout_strikes == 2) {
        floor_d = REQ_MIN * 2;
    } else if (s->timeout_strikes == 1 || stalling) {
        floor_d = REQ_INITIAL / 2;
    } else {
        // Fast-Ramp / Slow-Start:
        // As a peer successfully delivers blocks with 0 strikes, expand the floor
        // rapidly so fast peers are not throttled by RTT.
        int ramp = REQ_INITIAL;
        if (s->useful >= 4) {
            int bonus = (int)(s->useful);
            if (bonus > (MAX_REQ_PER_CONN - REQ_INITIAL))
                bonus = MAX_REQ_PER_CONN - REQ_INITIAL;
            ramp += bonus;
        }
        floor_d = (t->st_live <= 2 && ramp < 48) ? 48 : ramp;
    }
    if (floor_d < REQ_MIN) floor_d = REQ_MIN;
    if (d < floor_d) d = floor_d;
    if (d > MAX_REQ_PER_CONN) d = MAX_REQ_PER_CONN;
    return d;
}

// Reconcile a->req[b] against actual active sessions and count how many
// non-stalled sessions currently hold a request for (piece_idx, b). Only the
// sessions in the block's holder bitmap can hold one.
static int active_healthy_reqs(torrentfs *t, aq_entry *a, int b, u64 now,
                               uint8_t *out_total) {
    int64_t piece_idx = a->idx;
    int total = 0;
    int healthy = 0;
    const uint8_t *row = rb_row(a, b);
    for (int i = 0; i < MAX_SESS; i++) {
        if (!(row[i >> 3] & (1u << (i & 7)))) continue;
        const sess *os = &t->S[i];
        if (!os->active || os->connecting || !os->nb.handshaked) continue;
        for (int k = 0; k < os->req_n; k++) {
            if (os->req_piece[k] == (int)piece_idx && os->req_block[k] == b) {
                total++;
                if (os->timeout_strikes == 0 &&
                    (os->req_time[k] == 0 || now - os->req_time[k] <= (u64)2500 * (t->freq / 1000))) {
                    healthy++;
                }
                break;
            }
        }
    }
    if (out_total) *out_total = (uint8_t)(total > 255 ? 255 : total);
    return healthy;
}

// Find the next block of piece idx this session may request.
// Critical stream-head pieces (current playback head and next piece, or read_blocked)
// and endgame pieces use aggressive Adaptive Hedging (up to 4 duplicate requests)
// with short hedge timeouts so slow peers never stall the installer.
// True if every session holding (a, b) on order is slow (or gone).
static bool holders_all_slow(const torrentfs *t, const aq_entry *a, int b) {
    const uint8_t *row = rb_row(a, b);
    for (int i = 0; i < MAX_SESS; i++) {
        if (!(row[i >> 3] & (1u << (i & 7)))) continue;
        const sess *o = &t->S[i];
        if (o->active && !o->connecting && !o->slow) return false;
    }
    return true;
}

// True if some session holding (a, b) on order is delivering right now and, at
// its measured speed, will still reach this block: peers serve requests in
// order, so with a deep pipeline (up to MAX_REQ_PER_CONN blocks = 4 MB) a block
// can wait several seconds behind the others without anything being wrong.
// Re-requesting it then only downloads it twice. Measured on the console before
// this check: 10-20 % of all traffic was duplicates, with a fixed 0.5-2.5 s
// "late" timer.
static bool holder_on_schedule(const torrentfs *t, const aq_entry *a, int b, u64 now) {
    const uint8_t *row = rb_row(a, b);
    u64 age = now - a->req_ts[b];
    for (int i = 0; i < MAX_SESS; i++) {
        if (!(row[i >> 3] & (1u << (i & 7)))) continue;
        const sess *o = &t->S[i];
        if (!o->active || o->connecting || !o->nb.handshaked) continue;
        if (o->nb.choked || o->timeout_strikes > 0 || o->snubbed) continue;
        if (now - o->last_block > t->freq / 2) continue;   // not delivering right now
        double cap = sess_capability(t, o);
        if (cap < 16.0 * 1024.0) continue;
        double drain_s = (double)o->req_n * (double)BLOCK_LEN / cap;
        u64 budget = (u64)((drain_s * 1.5 + 0.5) * (double)t->freq);
        if (age <= budget) return true;
    }
    return false;
}

static int next_block_in(torrentfs *t, const sess *s, aq_entry *a, u64 now, int zone) {
    bool is_read_blocked = (t->read_blocked_piece == a->idx);
    // Head pieces (Critical + Urgent zones): a fast session may take over a
    // block only slow sessions hold.
    bool is_head = (zone <= 2) || is_read_blocked ||
                   (a->idx >= t->playhead_piece && a->idx <= t->playhead_piece + 1);
    // Hedged (duplicated on a timer) only: the Critical zone, the piece at the
    // read head, the piece the reader is blocked on.
    bool is_critical = (zone <= 1) || is_read_blocked || (a->idx == t->playhead_piece);
    int remaining_blocks = a->nblocks - a->have_cnt;
    // Tail of a critical piece: at most 64 blocks (1 MB). It used to be a
    // quarter of the piece, i.e. 4 MB of every 16 MB piece hedged 4 times.
    int tail_threshold = a->nblocks / 4;
    if (tail_threshold > 64) tail_threshold = 64;
    if (tail_threshold < 1) tail_threshold = 1;
    bool file_endgame = (t->file_last_piece - t->playhead_piece <= 4) ||
                        (t->meta.piece_count > 0 && (t->file_last_piece - t->file_first_piece + 1) - t->pieces_ever <= 4);
    bool critical_tail = is_critical && remaining_blocks > 0 && remaining_blocks <= tail_threshold;
    bool is_endgame  = file_endgame || critical_tail;
    // Where waiting costs the most, hedge without asking whether the holder is
    // on schedule: the last blocks of the piece the reader is blocked on, or
    // the download is about to end. (With 16 MB pieces the reader is blocked
    // almost all the time; hedging that whole piece kept dup at ~8 %.)
    bool must_hedge = (is_read_blocked && critical_tail) || file_endgame;

    int max_dup = 1; // Default for background readahead pieces: 1 request
    if (must_hedge) {
        max_dup = 3;
    } else if (is_critical || critical_tail) {
        max_dup = 2;
    }

    // Adaptive hedge timeout: 500ms in endgame/tail of head piece, 1000ms for critical stream head
    u64 hedge_timeout_ticks = (is_endgame || (is_critical && remaining_blocks <= tail_threshold))
                              ? (t->freq / 2) : ((u64)DUP_REQ_SECS * t->freq);

    for (int k = 0; k < a->nblocks; k++) {
        int b = a->next_req;
        a->next_req = (b + 1) % a->nblocks;
        if (a->have[b]) continue;
        if (rb_has(a, b, (int)(s - t->S))) continue;   // already on order here
        uint8_t r = a->req[b];
        if (r == 0) return b; // Fresh unrequested block: always grab

        // If duplicate requests are disabled for this piece tier:
        if (max_dup <= 1) {
            // Head pieces outside the Critical zone: take over blocks only
            // slow sessions hold (no timer, no extra copies beyond 2).
            if (is_head && !s->slow && r < 2 && holders_all_slow(t, a, b))
                return b;
            // For background pieces, steal/hedge if the existing request has
            // stalled > 2.5s and its holder is not simply working through
            // its queue.
            if (now - a->req_ts[b] > (u64)2500 * (t->freq / 1000) &&
                !holder_on_schedule(t, a, b, now)) {
                uint8_t actual_r = 0;
                int healthy = active_healthy_reqs(t, a, b, now, &actual_r);
                a->req[b] = actual_r;
                if (actual_r < 2 || healthy == 0)
                    return b;
            }
            continue;
        }

        // Critical piece or Endgame: Fast Request Stealing / Adaptive Hedging.
        // A fast session does not wait out the hedge timer for a block only
        // slow sessions hold: that wait is what stalls the reader.
        if (is_critical && !s->slow && r < max_dup && holders_all_slow(t, a, b))
            return b;
        if (now - a->req_ts[b] <= hedge_timeout_ticks) continue;
        if (!must_hedge && holder_on_schedule(t, a, b, now)) continue;
        if (r < max_dup) return b;

        // If r reached max_dup, check if existing holders are actually stalled (> 2.0s)
        if (now - a->req_ts[b] > (u64)2000 * (t->freq / 1000)) {
            uint8_t actual_r = 0;
            int healthy = active_healthy_reqs(t, a, b, now, &actual_r);
            a->req[b] = actual_r;
            if (actual_r < max_dup || healthy < 2)
                return b;
        }
    }
    return -1;
}

// Pick the next (piece, block) this session should request:
// Uses strict_fill_pending_first: existing open/assembling pieces are filled
// to completion BEFORE opening new pieces far ahead, preventing pieces from
// finishing out of order and starving the installer.
// One candidate of scan_next_block's pass 1: finish an assembling piece.
// 1 = found a block, 0 = nothing here.
static int try_active(torrentfs *t, sess *s, u64 now, int64_t i, int zone,
                      int64_t *out_piece, int *out_block) {
    if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, i)) return 0;
    if (t->status[i] != PIECE_ACTIVE) return 0;
    aq_entry *a = aq_find(t, i);
    if (!a) {
        engine_log(ENGINE_LOG_WARN,
                   "[claim] healing orphaned PIECE_ACTIVE for piece %lld",
                   (long long)i);
        a = aq_alloc(t, i);
        if (!a) {
            mutexLock(&t->lock);
            if (t->status[i] == PIECE_ACTIVE) t->status[i] = PIECE_NEEDED;
            mutexUnlock(&t->lock);
            return 0;
        }
    }
    int b = next_block_in(t, s, a, now, zone);
    if (b < 0) return 0;
    *out_piece = i;
    *out_block = b;
    return 1;
}

// One candidate of pass 2: open a new piece. 1 = found, 0 = next, -1 = stop
// the pass (assembly budget or writer queue full).
static int try_open(torrentfs *t, sess *s, u64 now, int64_t i, int zone,
                    int64_t *out_piece, int *out_block) {
    if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, i)) return 0;
    if (t->status[i] != PIECE_NEEDED) return 0;
    if (t->n_active_aq >= t->n_aq) return -1; // Assembly budget capped
    if (!wq_has_room(t)) return -1;           // writer behind: no new pieces
    aq_entry *a = aq_alloc(t, i);
    if (!a) return 0;
    mutexLock(&t->lock);
    t->status[i] = PIECE_ACTIVE;
    mutexUnlock(&t->lock);
    int b = next_block_in(t, s, a, now, zone);
    if (b < 0) return 0;
    *out_piece = i;
    *out_block = b;
    return 1;
}

// Pick the next (piece, block) this session should request:
// Uses strict_fill_pending_first: existing open/assembling pieces are filled
// to completion BEFORE opening new pieces far ahead, preventing pieces from
// finishing out of order and starving the installer. Walks the candidate list
// (cand_refresh) zone by zone; the read-blocked piece leads zone 1.
// scan_next_block for a slow session (while fast sessions exist): stay off
// the head zones; keep working on its own piece, else open the farthest
// needed piece, else help on the farthest active pieces.
static bool scan_next_block_slow(torrentfs *t, sess *s, u64 now,
                                 int64_t *out_piece, int *out_block) {
    int64_t own = s->own_piece1 - 1;
    if (own >= 0 && own < t->meta.piece_count && t->status[own] == PIECE_ACTIVE &&
        bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, own)) {
        aq_entry *a = aq_find(t, own);
        if (a) {
            int b = next_block_in(t, s, a, now, 5);
            if (b >= 0) { *out_piece = own; *out_block = b; return true; }
        }
    }
    s->own_piece1 = 0;
    for (int zone = 5; zone >= 3; zone--) {
        const int z0 = t->cand_end[zone - 1], z1 = t->cand_end[zone];
        for (int c = z1 - 1; c >= z0; c--) {
            int r = try_open(t, s, now, t->cand[c], zone, out_piece, out_block);
            if (r > 0) { s->own_piece1 = *out_piece + 1; return true; }
            if (r < 0) { zone = 0; break; }   // no room to open: go help instead
        }
    }
    for (int zone = 5; zone >= 3; zone--) {
        const int z0 = t->cand_end[zone - 1], z1 = t->cand_end[zone];
        for (int c = z1 - 1; c >= z0; c--)
            if (try_active(t, s, now, t->cand[c], zone, out_piece, out_block)) return true;
    }
    return false;
}

static bool scan_next_block(torrentfs *t, sess *s, u64 now,
                            int64_t *out_piece, int *out_block) {
    t->pf_picks++;
    if (!t->cand_valid) return false;
    if (s->slow && t->n_fast > 0) return scan_next_block_slow(t, s, now, out_piece, out_block);
    int64_t fhi = t->file_last_piece + LOOKAHEAD_PIECES;
    if (fhi >= t->meta.piece_count) fhi = t->meta.piece_count - 1;
    int64_t flo = (t->playhead_piece > t->file_first_piece) ? t->playhead_piece : t->file_first_piece;
    int64_t rb = t->read_blocked_piece;   // racy snapshot (reader thread)
    if (rb < flo || rb > fhi) rb = -1;

    for (int zone = 1; zone <= 5; zone++) {
        const int z0 = t->cand_end[zone - 1], z1 = t->cand_end[zone];
        // Pass 1: Complete already active assembly slots first
        if (zone == 1 && rb >= 0 &&
            try_active(t, s, now, rb, 1, out_piece, out_block)) return true;
        for (int c = z0; c < z1; c++) {
            int64_t i = t->cand[c];
            if (i == rb) continue;   // zone 1 already tried it
            if (try_active(t, s, now, i, zone, out_piece, out_block)) return true;
        }
        // Pass 2: Open a new piece if no active piece has available blocks in this zone
        if (zone == 1 && rb >= 0) {
            int r = try_open(t, s, now, rb, 1, out_piece, out_block);
            if (r > 0) return true;
            if (r < 0) continue;
        }
        for (int c = z0; c < z1; c++) {
            int64_t i = t->cand[c];
            if (i == rb) continue;
            int r = try_open(t, s, now, i, zone, out_piece, out_block);
            if (r > 0) return true;
            if (r < 0) break;
        }
    }
    return false;
}

// Keep the session's outstanding-request ring full up to its nominal depth.
// Called on every delivery burst and every upkeep tick for every unchoked
// session, so the swarm's requests always reflect the current priority
// gradient without any per-piece ownership state.
// `prune`: first drop stale ring entries (finished pieces, held blocks, >4 s
// timeouts). That pass walks the whole ring (up to 256 entries) and is only
// needed on the upkeep tick and on unchoke; the receive path skips it.
static void request_scan_ex(torrentfs *t, sess *s, u64 now, bool prune) {
    if (!s->nb.handshaked || s->connecting || s->nb.choked) return;
    if (t->paused) return;
    t->pf_scans++;
    const int sid = (int)(s - t->S);
    if (prune) {
    // Prune any stale requests for pieces that already finished or blocks already held,
    // or requests that timed out (> 4.0s)
    int kept = 0;
    u64 req_timeout_ticks = (u64)4000 * (t->freq / 1000);
    for (int i = 0; i < s->req_n; i++) {
        int ep = s->req_piece[i];
        int eb = s->req_block[i];
        bool stale = (ep < 0 || ep >= t->meta.piece_count ||
                      t->status[ep] == PIECE_DONE || t->status[ep] == PIECE_WRITING);
        if (!stale) {
            aq_entry *ea = aq_find(t, ep);
            if (!ea || eb < 0 || eb >= ea->nblocks) {
                stale = true;
            } else if (ea->have[eb]) {
                rb_clr(ea, eb, sid);
                stale = true;
            } else if (s->req_time[i] > 0 && (now - s->req_time[i] > req_timeout_ticks)) {
                // Individual block request timed out (> 4.0s): release lock on block
                if (ea->req[eb] > 0 && ea->req[eb] < 255)
                    ea->req[eb]--;
                rb_clr(ea, eb, sid);
                send_cancel_one(t, s, ep, eb);
                stale = true;
            }
        }
        if (!stale) {
            if (kept != i) {
                s->req_piece[kept] = ep;
                s->req_block[kept] = eb;
                s->req_time[kept]  = s->req_time[i];
            }
            kept++;
        }
    }
    s->req_n = kept;
    }

    int depth = nominal_depth(t, s);
    cand_refresh(t);
    int was_empty = (s->req_n == 0);
    if (was_empty)
        engine_log(ENGINE_LOG_DEBUG, "[sess %d] scan depth=%d",
                    (int)(s - t->S), depth);
    while (s->req_n < depth && s->req_n < REQ_RING) {
        int64_t pi;
        int b;
        if (!scan_next_block(t, s, now, &pi, &b)) break;
        uint8_t pl[12];
        uint32_t v;
        v = htonl((uint32_t)pi);        memcpy(pl, &v, 4);
        v = htonl((uint32_t)b * BLOCK_LEN);   memcpy(pl + 4, &v, 4);
        v = htonl(block_len_of(torrent_piece_len(&t->meta, pi), b));
        memcpy(pl + 8, &v, 4);
        if (peer_nb_queue(&s->nb, MSG_REQUEST, pl, 12) != 0) break;
        aq_entry *a = aq_find(t, pi);
        if (a) {
            if (a->req[b] < 255) a->req[b]++;
            a->req_ts[b] = now;
            rb_set(a, b, sid);
        }
        s->req_piece[s->req_n] = (int)pi;
        s->req_block[s->req_n] = b;
        s->req_time[s->req_n]  = now;
        s->req_n++;
        // Anchor the stall clock at the first request of a burst: the stall
        // gate (STALL_SECS without a delivered block) must not fire on a
        // freshly connected session whose first block is still on the wire
        // (last_block was 0 for TCP dials and the check read a garbage age).
        if (s->req_n == 1) s->last_block = now;
        t->st_claim_ok++;
    }
    if (s->req_n == 0 && depth > 0) t->st_claim_fail++;
    if (was_empty) {
        if (s->req_n > 0)
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] first req piece=%lld block=%d",
                       (int)(s - t->S),
                       (long long)s->req_piece[0], s->req_block[0]);
        else
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] scan found nothing", (int)(s - t->S));
    }
}

static void request_scan(torrentfs *t, sess *s, u64 now) {
    request_scan_ex(t, s, now, true);
}

// request_scan() after a receive burst. Refilling the ring on every delivery
// re-ran the whole prune + pick pass for one or two new requests (13k passes/s
// on loopback); refill once a batch's worth of the pipeline has drained
// instead. The upkeep tick still runs the full scan every 125 ms (timeouts),
// and a blocked reader keeps the per-delivery refill (its piece is urgent).
//
// Called once per readable event, and a 16 KB block usually arrives over
// several events: only refill when a requested block actually landed since
// the last refill. On the console the reader is blocked most of the time (16 MB
// pieces), and the old "blocked reader -> scan on every event" path ran ~4000
// full scans/s for ~500 requests/s.
static void request_scan_rx(torrentfs *t, sess *s, u64 now) {
    if (!s->nb.handshaked || s->connecting || s->nb.choked || t->paused) return;
    // Nothing delivered since the last refill: the upkeep tick (125 ms),
    // UNCHOKE and the next delivery cover everything else.
    if (s->useful == s->scan_useful) return;
    if (s->req_n == 0) {
        s->scan_useful = s->useful;
        request_scan_ex(t, s, now, true);
        return;
    }
    int depth = nominal_depth(t, s);
    int gap = depth / 8;
    if (gap < 4) gap = 4;
    if (t->read_blocked_piece >= 0) gap = 2;   // reader waiting: refill sooner
    int room = depth - s->req_n;
    if (room >= gap || (depth <= gap && room > 0)) {
        s->scan_useful = s->useful;
        request_scan_ex(t, s, now, false);
    }
}

// All blocks landed: hand the buffer to the writer (which verifies + writes).
// Duplicate copies of this piece's blocks may still be in flight on other
// sessions; they land later, fail the have[] test, and count as duplicate
// traffic (anacrolix behaves the same: stragglers are deduplicated, not
// cancelled -- postCancel only fires when a block arrives while other
// sessions still hold a pending copy of that same block).
static void piece_full(torrentfs *t, aq_entry *a) {
    int64_t idx = a->idx;
    t->last_progress_tick = armGetSystemTick();
    // Announce the piece to every live peer so we look like a well-behaved
    // client (and seeders learn what we hold for later piece exchange), and
    // purge any lingering request entries for this completed piece.
    uint32_t hv = htonl((uint32_t)idx);
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        peer_nb_queue(&s->nb, MSG_HAVE, &hv, 4);
        int kept = 0;
        for (int k = 0; k < s->req_n; k++) {
            if (s->req_piece[k] != (int)idx) {
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
    mutexLock(&t->lock);
    if (t->wq_n >= WQ_CAP) {
        // Must not happen (wq_has_room gates every new piece). Never let the
        // ring overwrite itself: drop the piece, keep the buffer in its slot.
        t->status[idx] = PIECE_NEEDED;
        a->idx = -1;
        if (t->n_active_aq > 0) t->n_active_aq--;
        t->st_wq_full++;
        mutexUnlock(&t->lock);
        engine_log(ENGINE_LOG_ERROR, "[writer] queue full (%d), piece %lld dropped",
                   WQ_CAP, (long long)idx);
        return;
    }
    t->status[idx] = PIECE_WRITING;
    int tail = (t->wq_head + t->wq_n) % WQ_CAP;
    t->wq[tail] = (wjob){ idx, torrent_piece_len(&t->meta, idx), a->buf, a->peer_from };
    t->wq_n++;                 // bounded by WQ_CAP via wq_has_room()
    condvarWakeAll(&t->wq_cv);
    condvarWakeAll(&t->piece_cv);
    a->idx = -1;               // slot reset inside the lock: the writer picks
    a->buf = NULL;             // its return slot by these very fields
    a->peer_from = NULL;
    if (t->n_active_aq > 0) t->n_active_aq--;
    mutexUnlock(&t->lock);
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
            s->choked_at = now;
            t->st_choked++;
            engine_log(ENGINE_LOG_DEBUG, "[sess] choke");
            // Outstanding requests are void now. Drop them WITHOUT cancels
            // (anacrolix does not cancel on choke either): their per-piece
            // counts are freed so the swarm re-requests the blocks after the
            // duplicate timer.
            sess_drop_requests(t, s);
            break;
        case MSG_UNCHOKE:
            s->nb.choked = false;
            s->choked_at = 0;
            t->st_unchoke_ok++;
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] unchoke from %u.%u.%u.%u:%d",
                       (int)(s - t->S), s->addr.ip & 0xff, (s->addr.ip >> 8) & 0xff,
                       (s->addr.ip >> 16) & 0xff, (s->addr.ip >> 24) & 0xff, s->addr.port);
            if (!t->paused) {
                request_scan(t, s, now);
                if (peer_nb_tx_pending(&s->nb)) peer_nb_flush(&s->nb);
            }
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
                if (idx >= 0 && idx < t->meta.piece_count) {
                    uint8_t bit = (uint8_t)(0x80 >> (idx % 8));
                    if (!(s->nb.bitfield[idx / 8] & bit) &&
                        idx >= t->file_first_piece && idx <= t->file_last_piece)
                        s->have_in_file++;
                    s->nb.bitfield[idx / 8] |= bit;
                }
            }
            break;
        case MSG_BITFIELD:
            if (plen == s->nb.bitfield_len) {
                memcpy(s->nb.bitfield, pl, plen);
                s->have_in_file = count_have_in_file(t, s);
                t->st_bf_ok++;
                bool empty = true;
                bool full = true;
                for (size_t b = 0; b < plen; b++)
                    if (pl[b]) empty = false;
                for (int64_t p = 0; p < t->meta.piece_count && full; p++)
                    if (!bf_has_piece(s->nb.bitfield, s->nb.bitfield_len, p))
                        full = false;
                engine_log(ENGINE_LOG_DEBUG,
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

            {
                aq_entry *da = aq_find(t, idx);
                if (da) da->last_deliver = now;
            }
            if (t->starve_round > 0) {
                engine_log(ENGINE_LOG_INFO,
                           "[torrentfs] starvation recovered (delivered block after %d rounds)",
                           t->starve_round);
                t->starve_round = 0;
            }

            // Match against this session's outstanding requests. Even if the
            // request was duplicated by another peer and this copy arrives
            // second, we NEVER discard genuine, unreceived payload data.
            int matched = 0;
            {
                aq_entry *ma = aq_find(t, idx);
                if (ma && b < ma->nblocks) rb_clr(ma, b, (int)(s - t->S));
            }
            u64 req_t = 0;
            for (int i = 0; i < s->req_n; ) {
                if (s->req_piece[i] == (int)idx && s->req_block[i] == b) {
                    if (!matched) req_t = s->req_time[i];
                    s->req_piece[i] = s->req_piece[s->req_n - 1];
                    s->req_block[i] = s->req_block[s->req_n - 1];
                    s->req_time[i]  = s->req_time[s->req_n - 1];
                    s->req_n--;
                    matched++;
                } else {
                    i++;
                }
            }
            if (matched > 0) {
                s->useful++;          // expected delivery: feeds nominal_depth
                u64 start = s->last_rx_block > req_t ? s->last_rx_block : req_t;
                if (start && now > start) s->svc_ticks += now - start;
                s->svc_bytes += dlen;
                s->last_rx_block = now;
                s->snubbed = false;
            } else {
                t->st_dup_noring += dlen;
            }

            aq_entry *a = aq_find(t, idx);
            if (!a || b >= a->nblocks || a->have[b]) {
                if (a && b < a->nblocks) {
                    if (a->req[b] > 0 && a->req[b] < 255) a->req[b]--;
                    t->st_dup_have += dlen;
                } else {
                    t->st_dup_gone += dlen;
                }
                t->st_dup_bytes += dlen;   // delivered twice: wasted traffic
                break;
            }
            if (matched == 0 && a->req[b] == 0) {
                // Late arrival of an expired request: accept it since piece is actively assembling and block is needed
                t->st_dup_noring += dlen;
            }
            memcpy(a->buf + begin, pl + 8, dlen);
            if (a->peer_from && b < a->nblocks) {
                a->peer_from[b] = (int16_t)s->pidx;
            }
            // Release so the streaming reader's acquire load of have[] (RAM
            // mode partial reads) can never observe the flag before the data.
            __atomic_store_n(&a->have[b], 1, __ATOMIC_RELEASE);
            if (a->req[b] > 0 && a->req[b] < 255) a->req[b]--;
            a->have_cnt++;
            // A non-strict reader streams partial pieces block by block.
            if ((int64_t)idx == t->read_blocked_piece && !t->strict_verify)
                condvarWakeAll(&t->piece_cv);

            t->st_blocks_have++;
            t->last_progress_tick = now;
            s->timeout_strikes = 0;

            // Cancel duplicate copies of this block still in flight on other
            // sessions (anacrolix postCancel): the block is here, the copies
            // would only waste the swarm's bandwidth.
            // Only the sessions in the block's holder bitmap can have a copy
            // on order (this used to walk every ring of every session, per
            // delivered 16 KB block).
            uint8_t *holders = rb_row(a, b);
            for (int j = 0; j < MAX_SESS; j++) {
                if (!(holders[j >> 3] & (1u << (j & 7)))) continue;
                rb_clr(a, b, j);
                sess *other = &t->S[j];
                if (other == s || !other->active || other->connecting) continue;
                bool sent_cancel = false;
                for (int k = 0; k < other->req_n; ) {
                    if (other->req_piece[k] == (int)idx && other->req_block[k] == b) {
                        if (!sent_cancel) {
                            send_cancel_one(t, other, (int)idx, b);
                            sent_cancel = true;
                        }
                        if (a->req[b] > 0 && a->req[b] < 255) a->req[b]--;
                        other->req_piece[k] = other->req_piece[other->req_n - 1];
                        other->req_block[k] = other->req_block[other->req_n - 1];
                        other->req_time[k]  = other->req_time[other->req_n - 1];
                        other->req_n--;
                        if (!other->active) break;
                    } else {
                        k++;
                    }
                }
            }

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
        engine_log(ENGINE_LOG_DEBUG,
                   "[sess %d] sent our bitfield (%zu bytes)", (int)(s - t->S), n);
    }
    free(bf);
}

static void sess_service(torrentfs *t, sess *s, int sid, u64 now) {
    for (int pump = 0; pump < 8; pump++) {
        u64 io0 = armGetSystemTick();
        ssize_t got = peer_nb_pump_rx(&s->nb);
        t->pf_sock_ticks += armGetSystemTick() - io0;
        t->pf_sock_calls++;
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
            peer_nb_apply_speed_opts(&s->nb);
            engine_log(ENGINE_LOG_DEBUG, "[sess %d] handshake ok live=%d/%d",
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

        // Pump again only if the last pump stopped on a full rx buffer; a
        // drained socket would just cost another recv IPC for EAGAIN.
        if (got == 0 || msgs == 0 || !s->nb.rx_more) break;
    }

    request_scan_rx(t, s, now);

    u64 io0 = armGetSystemTick();
    int fr = peer_nb_flush(&s->nb);
    t->pf_sock_ticks += armGetSystemTick() - io0;
    t->pf_sock_calls++;
    if (fr != 0) { sess_close(t, s, true); return; }
}

//-----------------------------------------------------------------------------
// Netloop
//-----------------------------------------------------------------------------

// Calm budget: for game downloads/installations, allocate all MAX_SESS slots
// to saturate available network bandwidth without artificial mpv video player throttling.
static int calm_budget(torrentfs *t) {
    (void)t;
    return MAX_SESS;
}

// Refresh every session's smoothed receive rate and the expect-time anchor
// that nominal_depth() uses (anacrolix totalExpectingTime: the time the
// session has spent with outstanding requests). Run once per upkeep tick.
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
            if (udt >= 0.2) {                    // at most one sample per upkeep
                double uinst = (double)(s->tx_total - s->up_rate_prev) / udt;
                if (uinst < 0) uinst = 0;
                s->up_rate      = s->up_rate * 0.6 + uinst * 0.4;
                s->up_rate_prev = s->tx_total;
                s->up_rate_tick = now;
            }
        }
        // Expect-time accumulation: count the ~125 ms upkeep interval while
        // this session has requests outstanding and is not choked.
        if (s->req_n > 0 && !s->nb.choked) s->expect_ticks += t->freq / 8;
        // Download meter: sample only while actively receiving. Between
        // bursts the rate is frozen, not decayed (choking round ranks by it).
        if (s->nb.choked || s->req_n == 0) {
            s->rate_tick = 0;
            continue;
        }
        if (s->rate_tick == 0) {                        // (re)open the sample window
            s->rate_prev = s->rx_total;
            s->rate_tick = now;
            continue;
        }
        double dt = (double)(now - s->rate_tick) / (double)t->freq;
        if (dt < 0.2) continue;    // at most one sample per upkeep
        double inst = (double)(s->rx_total - s->rate_prev) / dt;
        if (inst < 0) inst = 0;
        s->rate      = s->rate * 0.6 + inst * 0.4;
        s->rate_prev = s->rx_total;
        s->rate_tick = now;
    }
}

// Speed classes (see SLOW_FRACTION), once per upkeep tick after the rates.
// What a session can deliver, bytes/s: the better of its smoothed rate and
// its average while it had requests outstanding. The EWMA alone is useless
// here: it freezes and decays while a session has nothing on order, and a
// fast peer left idle for a moment read as slow -- it was then kept off the
// head pieces, stayed idle, and never recovered (measured: a 400 MB/s seed
// classified slow behind a 1 MB/s one).
static double sess_capability(const torrentfs *t, const sess *s) {
    if (s->svc_ticks > t->freq / 20)   // >= 50 ms of measured service
        return (double)s->svc_bytes * (double)t->freq / (double)s->svc_ticks;
    return s->rate;
}

static void classify_sessions(torrentfs *t) {
    double best = 0.0;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &t->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked || s->nb.choked) continue;
        if (s->rx_total < SLOW_MIN_BYTES) continue;
        double cap = sess_capability(t, s);
        if (cap > best) best = cap;
    }
    int n_fast = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        bool slow = false;
        if (s->svc_ticks > (u64)4 * t->freq) {   // ~4 s memory: follow speed changes
            s->svc_ticks /= 2;
            s->svc_bytes /= 2;
        }
        if (s->active && !s->connecting && s->nb.handshaked && !s->nb.choked) {
            if (s->timeout_strikes > 0 || s->snubbed) {
                slow = true;
            } else if (best > 0.0 && s->rx_total >= SLOW_MIN_BYTES) {
                // Hysteresis: become slow under SLOW_FRACTION of the best,
                // become fast again only above 1.6x that.
                double cap = sess_capability(t, s);
                double limit = best * SLOW_FRACTION * (s->slow ? 1.6 : 1.0);
                slow = cap < limit;
            }
            if (!slow) n_fast++;
            // Adaptive receive window (plan S5): everyone starts at 128 KB;
            // a TCP peer that sustains >= 1 MB/s gets 512 KB once, so the
            // socket pool goes to the peers that can fill it.
            if (!slow && !s->rcvbuf_big && s->nb.utp == NULL &&
                s->rx_total >= SLOW_MIN_BYTES &&
                sess_capability(t, s) >= 1024.0 * 1024.0) {
                peer_nb_set_rcvbuf(&s->nb, 512 * 1024);
                s->rcvbuf_big = true;
            }
        }
        s->slow = slow;
    }
    t->n_fast = n_fast;
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
    bool have_offer = t->pieces_done > 0 || t->st_piece_ok > 0;
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

// A session to make room for an incoming peer when every slot is taken.
// Incoming peers dialed US, so they are reachable by construction -- often the
// NAT'd seeders no outbound dial can reach -- and used to be dropped whenever
// the table was full. Victims, cheapest first: a pending outbound dial; a
// peer that has kept us choked for 30 s without delivering anything; the
// slowest established peer older than 60 s. Never a seed of a scarce swarm.
// Returns a session index or -1 (then the incoming socket is dropped).
static int pick_incoming_victim(torrentfs *t, u64 now) {
    for (int i = 0; i < MAX_SESS; i++)
        if (t->S[i].active && t->S[i].connecting) return i;
    const bool scarce = t->st_live <= 4;
    int choked_idle = -1;
    int slowest = -1;
    double slowest_cap = 0.0;
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        if (scarce && peer_is_seed(t, s)) continue;
        u64 age = now - s->started;
        if (s->nb.choked && s->rx_total == 0 && age > (u64)30 * t->freq) {
            if (choked_idle < 0) choked_idle = i;
            continue;
        }
        if (age > (u64)60 * t->freq) {
            double cap = sess_capability(t, s);
            if (slowest < 0 || cap < slowest_cap) { slowest = i; slowest_cap = cap; }
        }
    }
    if (choked_idle >= 0) return choked_idle;
    // Only displace a slow peer: a full table of fast ones keeps its slots.
    if (slowest >= 0 && t->S[slowest].slow) return slowest;
    return -1;
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
            int v = pick_incoming_victim(t, now);
            if (v < 0) {
                engine_log(ENGINE_LOG_DEBUG, "[sess] no slot for incoming conn");
                close(fd);
                continue;
            }
            engine_log(ENGINE_LOG_INFO,
                       "[sess %d] closed to make room for an incoming peer (connecting=%d slow=%d)",
                       v, t->S[v].connecting ? 1 : 0, t->S[v].slow ? 1 : 0);
            // A normal rotation, not a dead address: minimal backoff.
            t->S[v].fail_kind = PEER_FAIL_CHOKE_ROTATE;
            sess_close(t, &t->S[v], true);
            sid = v;
        }

        sess *s = &t->S[sid];
        memset(s, 0, sizeof(*s));
        if (peer_nb_init(&s->nb, fd, t->meta.piece_count) != 0) {
            close(fd);
            continue;
        }
        s->active     = true;
        s->pidx       = -1;   // not a pool peer: no backoff bookkeeping
        s->started    = now;
        s->choked_at  = now;
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
        peer_nb_apply_speed_opts(&s->nb);
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

// One [perf] line every PERF_WINDOW_SECS (netloop thread): the baseline the
// optimisation work is measured against. loop_us is the CPU time of one netloop
// iteration with the poll() wait excluded; scans/picks are request_scan() and
// per-block scheduler calls per second; dup is the share of received payload
// that duplicated blocks we already had.
#define PERF_WINDOW_SECS 5

static void perf_emit(torrentfs *t, u64 now) {
    double secs = (double)(now - t->pf_tick) / (double)t->freq;
    if (secs <= 0.0) secs = 1.0;
    int unchoked = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &t->S[i];
        if (s->active && !s->connecting && s->nb.handshaked && !s->nb.choked)
            unchoked++;
    }
    int64_t bytes = t->st_bytes_recv - t->pf_bytes;
    int64_t dup = t->st_dup_bytes - t->pf_dup;
    double dup_pct = bytes > 0 ? 100.0 * (double)dup / (double)bytes : 0.0;
    double kbps = (double)(bytes - dup) / 1024.0 / secs;
    engine_log(ENGINE_LOG_INFO,
               "[perf] net loop_us p50=%llu p95=%llu max=%llu iters=%u "
               "scans/s=%.0f picks/s=%.0f rebuilds/s=%.1f rate=%.0fKB/s dup=%.1f%% "
               "live=%d unchoked=%d connecting=%d pool=%d "
               "sock_fail+%d enobufs+%d timeouts+%d wq=%d wq_full+%d fast=%d slow=%d "
               "aq=%d/%d rb=%lld rb_st=%d rotated+%d dials+%d "
               "sock_ms/s=%.0f sock_calls/s=%.0f sock_us/call=%.0f utp_conn=%d utp_dials+%d",
               (unsigned long long)perf_hist_pct(&t->pf_loop, 0.50),
               (unsigned long long)perf_hist_pct(&t->pf_loop, 0.95),
               (unsigned long long)t->pf_loop.max, t->pf_loop.n,
               t->pf_scans / secs, t->pf_picks / secs, t->pf_rebuilds / secs, kbps, dup_pct,
               t->st_live, unchoked, t->st_connecting, t->peer_count,
               t->st_sock_fail - t->pf_sock_fail, t->st_enobufs - t->pf_enobufs,
               t->st_conn_timeout - t->pf_timeouts,
               t->wq_n, t->st_wq_full - t->pf_wq_full, t->n_fast, unchoked - t->n_fast,
               t->n_active_aq, t->n_aq, (long long)t->read_blocked_piece,
               t->read_blocked_piece >= 0 ? (int)t->status[t->read_blocked_piece] : -1,
               t->st_rotated - t->pf_rotated, t->st_dials - t->pf_dials,
               (double)t->pf_sock_ticks * 1000.0 / (double)t->freq / secs,
               t->pf_sock_calls / secs,
               t->pf_sock_calls ? (double)t->pf_sock_ticks * 1e6 / (double)t->freq / t->pf_sock_calls : 0.0,
               t->pf_utp_connecting, t->st_utp_dials - t->pf_utp_dials);
    t->pf_utp_dials = t->st_utp_dials;
    perf_hist_reset(&t->pf_loop);
    t->pf_sock_ticks = 0;
    t->pf_sock_calls = 0;
    t->pf_scans = t->pf_picks = 0;
    t->pf_rebuilds = 0;
    t->pf_tick = now;
    t->pf_bytes = t->st_bytes_recv;
    t->pf_dup = t->st_dup_bytes;
    t->pf_sock_fail = t->st_sock_fail;
    t->pf_enobufs = t->st_enobufs;
    t->pf_timeouts = t->st_conn_timeout;
    t->pf_wq_full = t->st_wq_full;
    t->pf_rotated = t->st_rotated;
    t->pf_dials = t->st_dials;
}

// Pool addresses that could be dialed right now (turnover only opens a
// window when there is someone new to try). Takes t->lock.
static bool pool_has_ready(torrentfs *t, u64 now) {
    bool ready = false;
    mutexLock(&t->lock);
    for (int i = 0; i < t->peer_count && !ready; i++)
        if (!t->peer_busy[i] && t->peer_next_try[i] <= now && t->peer_fails[i] < 0xFF)
            ready = true;
    mutexUnlock(&t->lock);
    return ready;
}

// The worst established session by capability, or -1. Established: older
// than min_age, or measured (SLOW_MIN_BYTES delivered) and at least 3 s old --
// a newcomer that is plainly slow does not get to sit out its whole trial.
// A seed of a scarce swarm is never picked.
static int worst_session(torrentfs *t, u64 now, u64 min_age) {
    const bool scarce = t->st_live <= 4;
    int worst = -1;
    double worst_cap = 0.0;
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        u64 age = now - s->started;
        bool measured = s->svc_bytes >= SLOW_MIN_BYTES && age >= (u64)3 * t->freq;
        if (age < min_age && !measured) continue;
        if (scarce && peer_is_seed(t, s)) continue;
        double cap = sess_capability(t, s);
        if (worst < 0 || cap < worst_cap) { worst = i; worst_cap = cap; }
    }
    return worst;
}

// Close a session for turnover: a normal rotation, but the address waits
// SLOW_DROP_SECS so untried candidates get the slot.
// Best capability among the established sessions (for rotate_out).
static double best_capability(const torrentfs *t) {
    double best = 0.0;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &t->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        double cap = sess_capability(t, s);
        if (cap > best) best = cap;
    }
    return best;
}

static void rotate_out(torrentfs *t, sess *s, u64 now, const char *why) {
    int pidx = s->pidx;
    double cap = sess_capability(t, s);
    // Only a genuinely slow peer is parked and marked: a decent one that just
    // lost the tournament may come back with the next window.
    bool slow = s->slow || cap < 0.5 * best_capability(t);
    engine_log(ENGINE_LOG_INFO, "[rotate] sess %d out (%s): cap=%.0fKB/s slow=%d live=%d",
               (int)(s - t->S), why, cap / 1024.0, slow ? 1 : 0, t->st_live);
    s->fail_kind = PEER_FAIL_CHOKE_ROTATE;
    sess_close(t, s, true);
    if (pidx >= 0 && slow) {
        mutexLock(&t->lock);
        t->peer_next_try[pidx] = now + (u64)SLOW_DROP_SECS * t->freq;
        if (t->peer_rot[pidx] < 255) t->peer_rot[pidx]++;
        mutexUnlock(&t->lock);
    }
    t->st_rotated++;
}

// Turnover, once per upkeep tick (netloop).
// True once every session that joined in the current window has been
// measured (or has used up its trial) and no dial is pending.
static bool window_trials_done(torrentfs *t, u64 now) {
    if (now - t->turnover_start < (u64)3 * t->freq) return false;
    if (t->st_connecting > 0 && now - t->turnover_start < (u64)g_trial_secs * t->freq)
        return false;
    for (int i = 0; i < MAX_SESS; i++) {
        const sess *s = &t->S[i];
        if (!s->active || s->connecting || !s->nb.handshaked) continue;
        if (s->started < t->turnover_start) continue;   // not a newcomer
        if (s->svc_bytes < SLOW_MIN_BYTES &&
            now - s->started < (u64)g_trial_secs * t->freq) return false;
    }
    return true;
}

static void turnover_tick(torrentfs *t, u64 now) {
    const u64 freq = t->freq;
    if (t->turnover_until && (now >= t->turnover_until || window_trials_done(t, now))) {
        t->turnover_until = 0;
        t->trim_left = TURNOVER_HEADROOM;   // trials over: keep the best live_target
    }
    // While slow sessions hold slots, run windows three times as often.
    u64 interval = (u64)g_turnover_secs * freq;
    if (t->n_fast < t->st_live) interval /= 3;
    if (!t->turnover_until && t->trim_left == 0 && now - t->last_turnover > interval) {
        t->last_turnover = now;
        if (t->st_live >= t->live_target - 1 && pool_has_ready(t, now)) {
            t->turnover_start = now;
            t->turnover_until = now + (u64)(g_trial_secs + 2) * freq;
        }
    }
    // Hard cap: never more than live_max sessions (oldest-measured first).
    if (t->st_live > t->live_max) {
        int v = worst_session(t, now, (u64)5 * freq);
        if (v >= 0) { rotate_out(t, &t->S[v], now, "over cap"); return; }
    }
    // End of a window: trim back to the target, one per tick, at most the
    // headroom's worth (sessions above the target for other reasons -- the
    // scarce-uploader dial rule -- are not this window's to cut).
    if (t->trim_left > 0) {
        if (t->st_live <= t->live_target) { t->trim_left = 0; return; }
        int v = worst_session(t, now, (u64)g_trial_secs * freq);
        if (v >= 0) { rotate_out(t, &t->S[v], now, "turnover"); t->trim_left--; }
        else t->trim_left = 0;
    }
}

// Publish the status snapshot (netloop, every upkeep tick).
static void publish_snapshot(torrentfs *t) {
    torrentfs_peer_info peers[MAX_SESS];
    int n = 0, seeds = 0, claiming = 0, idle = 0;
    for (int i = 0; i < MAX_SESS; i++) {
        sess *s = &t->S[i];
        if (!s->active) continue;
        torrentfs_peer_info *o = &peers[n++];
        o->ip         = s->addr.ip;
        o->port       = s->addr.port;
        o->bytes_recv = s->rx_total;
        o->rate_bps   = s->rate;
        o->rtt_ms     = -1;
        o->connecting = s->connecting;
        o->handshaked = s->nb.handshaked;
        o->choked     = s->nb.choked;
        // The app's slow-peer isolation reads this: the highest-priority
        // piece this session is currently requesting (first ring entry).
        o->claim_piece = s->req_n > 0 ? s->req_piece[0] : -1;
        if (s->connecting || !s->nb.handshaked) continue;
        if (peer_is_seed(t, s)) seeds++;
        if (s->req_n > 0) claiming++;          // actively downloading
        else if (!s->nb.choked) idle++;        // unchoked but nothing outstanding
    }
    mutexLock(&t->snap_lock);
    memcpy(t->snap_peers, peers, (size_t)n * sizeof(peers[0]));
    t->snap_n_peers = n;
    t->snap_seeds = seeds;
    t->snap_claiming = claiming;
    t->snap_idle = idle;
    mutexUnlock(&t->snap_lock);
}

static void netloop_main(void *arg) {
    torrentfs *t = arg;
    u64 last_upkeep = 0;
    engine_log(ENGINE_LOG_INFO, "[netloop] start");
    t->pf_tick = armGetSystemTick();
    t->pf_bytes = t->st_bytes_recv;
    t->pf_dup = t->st_dup_bytes;

    while (!t->stop) {
        u64 it0 = armGetSystemTick();   // [perf] iteration start
        tsnx_engine_wd_tick(4);
        hb_beat(t, HB_NET);
        int sel = __atomic_load_n(&t->pending_select_file, __ATOMIC_ACQUIRE);
        if (sel >= 0) {
            int claimed = __atomic_exchange_n(&t->pending_select_file, -1, __ATOMIC_ACQ_REL);
            if (claimed >= 0) {
                torrentfs_apply_select_file(t, claimed);
                last_upkeep = 0; // force immediate request_scan for the newly selected file
            }
        }
        u64 now = armGetSystemTick();

        // Upkeep every ~125 ms: dial, reap, keep-alives, rate EWMA, calm.
        if (now - last_upkeep > t->freq / 8) {
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
                if (now - t->last_dup_dbg > (u64)10 * t->freq) {
                    t->last_dup_dbg = now;
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
                if (s->pidx >= 0 && t->peer_next_try[s->pidx] > now + (u64)60 * t->freq) {
                    engine_log(ENGINE_LOG_INFO, "[sess %d] close: peer %d is banned", i, s->pidx);
                    s->fail_kind = PEER_FAIL_OTHER;
                    sess_close(t, s, true);
                    continue;
                }
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
                // and dial a fresh one. A peer that already proved useful
                // (rx_total > 0) is never rotated out on this basis. FULL
                // seeds during bootstrap are exempt entirely (anacrolix/
                // torrserver model): they optimistic-unchoke on a ~30 s cycle
                // and are the only thing that can start the download -- killing
                // the connection just re-dials the same seed over and over,
                // which is exactly the reconnect loop we observed in the
                // field (one live seed, handshake every ~30 s, zero blocks).
                bool is_seed = peer_is_seed(t, s);
                bool has_needed = peer_has_needed(t, s);
                int eff_connecting = t->st_connecting > 0 ? t->st_connecting : 0;
                // Only rotate out choked peers when swarm is established and pool has plenty of candidates.
                bool pool_rich = (t->st_live >= 8 && t->peer_count > t->st_live + eff_connecting + 4);
                bool can_rotate = false;
                if (pool_rich && !s->connecting && s->nb.handshaked && s->nb.choked) {
                    if (s->rx_total == 0) {
                        // Kept us choked without delivering any data for CHOKED_ROTATE_SECS
                        int wait_secs = has_needed ? CHOKED_ROTATE_SECS : (CHOKED_ROTATE_SECS / 2);
                        can_rotate = (now - s->started > (u64)wait_secs * t->freq);
                    } else if (s->choked_at != 0) {
                        // Choked us again after delivering and kept us choked for >60s
                        can_rotate = (now - s->choked_at > (u64)60 * t->freq);
                    } else if (s->timeout_strikes >= 3) {
                        can_rotate = true;
                    }
                }
                if (can_rotate) {
                    engine_log(ENGINE_LOG_DEBUG,
                               "[sess %d] rotate: peer choked us (rx_total=%lld, seed=%d)",
                               i, (long long)s->rx_total, is_seed);
                    // A rotate-out is a normal choke cycle, not a dead
                    // address: minimal backoff, so we catch the next
                    // optimistic-unchoke window.
                    s->fail_kind = PEER_FAIL_CHOKE_ROTATE;
                    sess_close(t, s, true);
                    continue;
                }
                bool scarce_swarm = (t->st_live <= 2 || t->peer_count <= 2);
                if (!s->connecting && age > (u64)IDLE_SECS * t->freq) {
                    if (scarce_swarm || is_seed) {
                        // Do not close unique seeds or peers on starved swarms on idle
                        s->last_rx = now;
                        if (s->timeout_strikes < 2) s->timeout_strikes++;
                        else s->timeout_strikes = 1;
                        sess_drop_requests(t, s);
                        peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
                        engine_log(ENGINE_LOG_DEBUG, "[sess %d] idle on scarce swarm / seed: renew interested", i);
                    } else {
                        engine_log(ENGINE_LOG_DEBUG, "[sess %d] close idle timeout", i);
                        sess_close(t, s, true);
                        continue;
                    }
                }
                // Requested blocks and nothing came. Proven peers get
                // strikes before disconnecting. A peer that NEVER delivered
                // a single byte keeps its connection up to 3 strikes (anacrolix model:
                // the duplicate timer in next_block_in() redistributes its
                // blocks to other sessions); its outstanding requests are
                // dropped so the per-block duplicate caps cannot saturate,
                // and the next scan re-requests them fresh.
                if (s->req_n > 0 &&
                    now - s->last_block > (u64)STALL_SECS * t->freq) {
                    bool urgent_or_endgame = (t->read_blocked_piece >= 0) ||
                                            (t->file_last_piece - t->playhead_piece <= 4) ||
                                            (t->meta.piece_count > 0 && (t->file_last_piece - t->file_first_piece + 1) - t->pieces_ever <= 4);
                    if (scarce_swarm || is_seed) {
                        // On a scarce swarm or with a full seed, NEVER disconnect on strikes!
                        // A seed holds 100% of the torrent data; dropping the TCP connection only forces
                        // expensive re-connects. Drop pending requests so blocks can be fetched elsewhere,
                        // clamp strikes so nominal_depth stays responsive, and refresh INTERESTED.
                        if (s->timeout_strikes < 2) {
                            s->timeout_strikes++;
                        } else {
                            s->timeout_strikes = 1;
                        }
                        s->last_block = now;
                        sess_drop_requests(t, s);
                        peer_nb_queue(&s->nb, MSG_INTERESTED, NULL, 0);
                        engine_log(ENGINE_LOG_INFO,
                                   "[sess %d] scarce swarm / seed stall (strike %d): keep connection alive, refresh requests",
                                   i, s->timeout_strikes);
                    } else if (s->rx_total > 0 && s->timeout_strikes < 3) {
                        s->timeout_strikes++;
                        s->last_block = now;
                        engine_log(ENGINE_LOG_DEBUG, "[sess %d] stall strike %d/3",
                                   i, s->timeout_strikes);
                        // In endgame or when reader is blocked, or on 2nd strike:
                        // drop outstanding requests so other peers can fetch missing blocks immediately.
                        if (urgent_or_endgame || s->timeout_strikes >= 2) {
                            sess_drop_requests(t, s);
                        }
                    } else if (s->rx_total > 0 || s->timeout_strikes >= 3) {
                        t->st_fetch_fail++;
                        set_err(t, "stall", 0);
                        engine_log(ENGINE_LOG_DEBUG, "[sess %d] close stall strikes=%d",
                                   i, s->timeout_strikes);
                        s->fail_kind = PEER_FAIL_TIMEOUT;
                        sess_close(t, s, true);
                        continue;
                    } else {
                        s->timeout_strikes++;
                        s->last_block = now;
                        engine_log(ENGINE_LOG_DEBUG,
                                   "[sess %d] stall (never delivered, strike %d): drop requests, keep conn",
                                   i, s->timeout_strikes);
                        sess_drop_requests(t, s);
                    }
                }

                // Snub: unchoked, requests on order, nothing for SNUB_SECS.
                if (!s->connecting && s->nb.handshaked && !s->nb.choked && s->req_n > 0 &&
                    !s->snubbed && s->last_block &&
                    now - s->last_block > (u64)SNUB_SECS * t->freq &&
                    (!s->last_rx_block || now - s->last_rx_block > (u64)SNUB_SECS * t->freq)) {
                    s->snubbed = true;
                    engine_log(ENGINE_LOG_DEBUG, "[sess %d] snubbed", i);
                }
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
                    (t->pieces_ever > 0 || t->st_piece_ok > 0) &&
                    now - s->started > (u64)PEX_INTERVAL_SECS * t->freq &&
                    now - s->last_pex > (u64)PEX_INTERVAL_SECS * t->freq) {
                    send_pex(t, s, now);
                }
            }

            update_rates(t, now);   // per-session rate meter + expect-time anchor
            classify_sessions(t);   // speed classes for the request scheduler
            turnover_tick(t, now);  // replace the slowest sessions over time
            // Reciprocation choking: 10 s round, 30 s optimistic rotation.
            if (now - t->last_choke > (u64)CHOKE_INTERVAL_SECS * t->freq) {
                t->last_choke = now;
                choking_round(t, now);
            }
            // Pool exhaustion: under the session target and no address left to
            // dial. Checked every 5 s; triggers an extra tracker announce at
            // most every 2 min (trackers cut the peer list of clients that
            // announce too often) and keeps the DHT searching hard.
            if (now - t->last_exhaust_check > (u64)5 * t->freq) {
                t->last_exhaust_check = now;
                t->pool_exhausted = t->st_live < t->live_target && !pool_has_ready(t, now);
                if (t->pool_exhausted &&
                    (t->last_exhaust_announce == 0 ||
                     now - t->last_exhaust_announce > (u64)120 * t->freq)) {
                    t->last_exhaust_announce = now;
                    t->announce_now = true;
                    t->st_exhaust_announces++;
                    engine_log(ENGINE_LOG_INFO,
                               "[torrentfs] peer pool exhausted (live=%d pool=%d): re-announce",
                               t->st_live, t->peer_count);
                }
            }
            // Starvation signal for the background DHT: search more often so
            // the peer pool refills instead of waiting out the 15 s timer.
            dht_bg_set_hungry((t->st_live < STARVED_LIVE || t->pool_exhausted) ? 1 : 0);
            drain_incoming(t, now); // sockets the shared listener accepted

            int live = 0, connecting = 0, bf_empty = 0;
            for (int i = 0; i < MAX_SESS; i++) {
                sess *s = &t->S[i];
                if (!s->active) continue;
                if (s->connecting) { connecting++; continue; }
                if (s->nb.handshaked) live++;
                if (s->nb.handshaked && s->nb.bitfield) {
                    bool any = false;
                    for (size_t b = 0; b < s->nb.bitfield_len && !any; b++)
                        if (s->nb.bitfield[b]) any = true;
                    if (!any) bf_empty++;
                }
            }
            t->st_bf_empty = bf_empty;
            t->st_connecting = connecting;
            t->st_live = live;
            if (t->st_live > t->st_peak_live) t->st_peak_live = t->st_live;
            // Starvation recovery: if we have almost no live peers and progress is stalled,
            // stop wasting connection slots on uTP and retry peers that failed earlier. DHT
            // swarms are often full of dead/NAT'd addresses; without this the pool
            // can exhaust its backoff budget and stall the stream.
            bool progress_stalled = (now - t->last_progress_tick > (u64)5 * t->freq);
            bool starving = (t->st_live == 0) || (few_peers && progress_stalled);
            if (starving) {
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
                    } else if (t->st_live < 4) {
                        // Round 2+ (hard): condemn unproven peers that failed
                        // repeatedly ONLY if live connections dropped below 4.
                        // 0xFF makes add_peers_src replace them with
                        // fresh DHT/tracker peers; long next_try stops redials.
                        for (int i = 0; i < t->peer_count && condemned < 32; i++) {
                            if (t->peer_busy[i]) continue;
                            if (t->peer_src[i] == PEER_SRC_TRACKER) continue;
                            int threshold = (t->peer_fail_kind[i] == PEER_FAIL_TIMEOUT)
                                                ? 8 : 6;
                            if (!t->peer_ok[i] && t->peer_fails[i] >= threshold &&
                                t->peer_fails[i] < 0xFF) {
                                t->peer_fails[i] = 0xFF;
                                t->peer_next_try[i] = now + (u64)300 * t->freq;
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
                    // and at most once per 30s when live == 0 or starving.
                    if ((condemned > 0 || t->starve_round >= 2 || t->st_live == 0) &&
                        t->peer_count < 64 &&
                        now - t->last_starve_announce > (u64)30 * t->freq) {
                        t->last_starve_announce = now;
                        t->announce_now = true;
                    }
                }
            }

            // One uTP hole-punch retry per wave, ahead of the regular dials.
            int dial_budget = (t->st_live < 12) ? DIAL_BUDGET_BOOTSTRAP : DIAL_BUDGET_PER_TICK;
            if (t->pause_dials_until && now < t->pause_dials_until) {
                dial_budget = 0;
            }
            // uTP dials hold a connecting slot for up to 10 s. Every failed
            // TCP dial (81 % of them on the console, mostly dead DHT
            // addresses) queued a uTP retry ahead of new dials, so the 24
            // slots sat on retries to dead peers while live=19 never grew.
            // Cap the uTP dials in flight.
            int utp_connecting = 0;
            for (int s = 0; s < MAX_SESS; s++)
                if (t->S[s].active && t->S[s].connecting && t->S[s].nb.utp) utp_connecting++;
            t->pf_utp_connecting = utp_connecting;
            if (dial_budget > 0 && utp_connecting < UTP_MAX_CONNECTING) {
                peer_addr rpa;
                int rp = take_utp_retry_peer(t, &rpa);
                if (rp >= 0) {
                    if (sess_dial_utp_at(t, now, rp, rpa)) {
                        connecting++;
                        dial_budget--;
                        utp_connecting++;
                        t->st_utp_dials++;
                    }
                }
            }

            // DIAL_BUDGET_PER_TICK new dials per 250 ms upkeep (~8-12/s max):
            // Keep dialing up to MAX_SESS if unchoked uploaders are scarce
            int active_unchoked = 0;
            for (int s = 0; s < MAX_SESS; s++) {
                if (t->S[s].active && !t->S[s].connecting && t->S[s].nb.handshaked && !t->S[s].nb.choked) {
                    active_unchoked++;
                }
            }
            const bool window = t->turnover_until != 0;
            const int live_cap = t->live_target + (window ? TURNOVER_HEADROOM : 0);
            // Sessions + dials in flight never pass the hard cap (+ headroom in
            // a turnover window). At the default cap this is just the slot
            // table; a lower cap would otherwise be overrun by one dial burst.
            const int hard_cap = t->live_max + (window ? TURNOVER_HEADROOM : 0);
            bool need_dial = ((t->st_live < live_cap) ||
                              (active_unchoked < MIN_ACTIVE_UNCHOKED && t->st_live < t->live_max)) &&
                             t->st_live + connecting < hard_cap;
            if (t->pause_dials_until && now < t->pause_dials_until) need_dial = false;
            while (!t->paused && dial_budget > 0 &&
                   need_dial && connecting < MAX_CONNECTING) {
                bool ok;
                // µTP runs through a userspace stack and tops out well below
                // plain TCP on the Switch; keep it as a rare NAT fallback
                // (1 dial in 8) rather than an every-4th-dial transport.
                bool try_utp = utp_nb_fd() >= 0 && !few_peers &&
                               (t->dial_toggle % 8) == 0 &&
                               utp_connecting < UTP_MAX_CONNECTING;
                if (try_utp) {
                    ok = sess_dial_utp(t, now);
                    if (ok) { utp_connecting++; t->st_utp_dials++; }
                } else {
                    ok = sess_dial(t, now, conn_secs);
                }
                t->dial_toggle = (t->dial_toggle + 1) & 0x7f;
                if (!ok) break;
                connecting++;
                dial_budget--;
                t->st_dials++;
                need_dial = ((t->st_live < live_cap) ||
                             (active_unchoked < MIN_ACTIVE_UNCHOKED && t->st_live < t->live_max)) &&
                            t->st_live + connecting < hard_cap;
            }

            // Seek preemption: if the playhead has moved, a partially
            // assembled piece that is no longer in the priority gradient AND
            // has no outstanding requests will never be filled -- discard it
            // so its assembly slot can serve the new position. Pieces with
            // blocks still in flight are left alone: the deliveries land and
            // the piece completes (anacrolix also lets stragglers finish).
            for (int i = 0; i < t->n_aq; i++) {
                aq_entry *a = &t->aq[i];
                if (a->idx < 0) continue;
                if (piece_priority(t, a->idx) > 0) continue;
                bool inflight = false;
                for (int b = 0; b < a->nblocks; b++)
                    if (a->req[b] > 0) { inflight = true; break; }
                if (inflight) continue;
                mutexLock(&t->lock);
                if (t->status[a->idx] == PIECE_ACTIVE) {
                    t->status[a->idx] = PIECE_NEEDED;  // discard partial, re-DL
                    a->idx = -1;                       // release slot + buffer
                    if (t->n_active_aq > 0) t->n_active_aq--;
                }
                mutexUnlock(&t->lock);
            }

            // Fill every unchoked session's request ring up to its nominal
            // depth. This is the whole scheduler: each session scans the
            // priority gradient and takes the next available block, so the
            // reader's piece always gets the swarm's in-flight capacity
            // first. Nothing new while paused.
            for (int i = 0; i < MAX_SESS && !t->paused; i++) {
                sess *s = &t->S[i];
                if (!s->active || s->connecting || !s->nb.handshaked) continue;
                if (s->nb.choked) continue;
                request_scan(t, s, now);
                if (peer_nb_tx_pending(&s->nb)) peer_nb_flush(&s->nb);
            }

            publish_snapshot(t);
            if (now - t->pf_tick >= (u64)PERF_WINDOW_SECS * t->freq)
                perf_emit(t, now);
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

        u64 slept = 0;
#ifdef __SWITCH__
        // At most ~250 polls/s. With data trickling in on 20 sockets poll()
        // returned immediately every time (~630 iterations/s measured), and
        // each pass cost a poll IPC plus recv IPCs for a few KB. 4 ms is
        // nothing next to RTT and 16 MB pieces, and the 128-512 KB receive
        // windows absorb it.
        {
            u64 min_gap = t->freq / 250;
            u64 since = armGetSystemTick() - t->last_poll_ret;
            if (t->last_poll_ret && since < min_gap) {
                u64 s0 = armGetSystemTick();
                svcSleepThread((s64)((min_gap - since) * 1000000000ULL / t->freq));
                slept = armGetSystemTick() - s0;
            }
        }
#endif
        u64 lt0 = armGetSystemTick();
        int pr = poll(pfd, (nfds_t)n, 20);
        lat_add(t, LAT_POLL, lt0);

        now = armGetSystemTick();
        t->last_poll_ret = now;
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
                    if (t->st_connecting > 0) t->st_connecting--;
                    t->st_conn_ok++;
                    s->last_rx = s->last_ka = now;
                } else if (st < 0) {
                    sess_close(t, s, true);
                    continue;
                }
            }
            sess_service(t, s, i, now);
        }

        // [perf] CPU time of this iteration, the poll() wait left out.
        u64 work = (lt0 - it0 - slept) + (armGetSystemTick() - now);
        perf_hist_add(&t->pf_loop, work * 1000000ULL / t->freq);
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
    t->pending_select_file = -1;
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
    t->writing_idx = -1;
    t->writing_buf = NULL;

    t->blocks_per_piece =
        (int)((t->meta.piece_len + BLOCK_LEN - 1) / BLOCK_LEN);

    // Critical pieces = ceil(budget / piece_len), clamped: one big piece already
    // covers the probed region, so we do not pre-fetch three of them.
    int64_t plen = t->meta.piece_len;
    t->crit_head = (int)((CRIT_HEAD_BYTES + plen - 1) / plen);
    t->crit_tail = 0; // Sequential stream: do not waste bandwidth downloading tail pieces
    if (t->crit_head < 1) t->crit_head = 1;
    if (t->crit_head > CRIT_HEAD_MAX) t->crit_head = CRIT_HEAD_MAX;

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
        t->aq[i].req_ts = calloc(1, (size_t)t->blocks_per_piece * sizeof(uint64_t));
        t->aq[i].req_by = calloc(1, (size_t)t->blocks_per_piece * SESS_MASK_BYTES);
        if (!t->aq[i].have || !t->aq[i].req || !t->aq[i].req_ts || !t->aq[i].req_by)
            aq_ok = false;
    }
    t->file_unwanted = calloc((size_t)(t->meta.file_count > 0 ? t->meta.file_count : 1), 1);
    t->cand     = malloc((size_t)(t->meta.piece_count + 1) * sizeof(int32_t));
    t->cand_tmp = malloc((size_t)t->meta.piece_count + 1);
    if (!t->cand || !t->cand_tmp) aq_ok = false;
    t->aq_of_piece = malloc((size_t)t->meta.piece_count * sizeof(int16_t));
    if (t->aq_of_piece)
        for (int64_t i = 0; i < t->meta.piece_count; i++) t->aq_of_piece[i] = -1;
    else
        aq_ok = false;

    snprintf(t->cache_base, sizeof(t->cache_base), "%s", cache_path);
    tsnx_ensure_parent_dirs(t->cache_base);
    mutexInit(&t->cache_lock);
    mutexInit(&t->lock);
    mutexInit(&t->inc_q.lock);
    mutexInit(&t->snap_lock);
    condvarInit(&t->wq_cv);
    condvarInit(&t->piece_cv);
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++) t->chunks[i] = -1;
    t->calm_now = MAX_SESS;
    t->live_target = DIAL_STOP_LIVE;
    t->live_max = MAX_SESS;
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

    if (threadCreate(&t->discovery, discovery_main, t, NULL, 0x20000,
                     TSNX_THREAD_ARGS(TSNX_CORE_NET, TSNX_PRIO_NET_BG)) == 0) {
        t->discovery_started = true;
        threadStart(&t->discovery);
    }
    // Writer (SHA-1 + cache write) on the work core, off the UI core. Horizon
    // neither timeslices nor preempts between equal priorities, so if it ever
    // has to fall back to the UI core it runs one notch BELOW the UI (0x2D):
    // at 0x2C a frame waking from vsync waited out a whole SHA-1/SD write.
    if (threadCreate(&t->writer, writer_main, t, NULL, 0x20000,
                     TSNX_THREAD_ARGS(TSNX_CORE_WORK, TSNX_PRIO_WRITER)) == 0) {
        t->writer_started = true;
        threadStart(&t->writer);
    } else {
        snprintf(err, errlen, "could not start the writer thread");
        torrentfs_close(t);
        return NULL;
    }
    // Netloop on the network core at 0x2B, above the background net threads
    // that share it: Horizon never preempts between equal priorities, and
    // CPU-bound threads otherwise starve this loop's ~100 IPC yield points for
    // seconds at a time (v2's finding). It used to run on the UI core at 0x2B
    // -- ABOVE the UI -- which is what stuttered the interface during
    // downloads; on a fallback to the UI core it now drops below the UI.
    if (threadCreate(&t->netloop, netloop_main, t, NULL, 0x40000,
                     TSNX_THREAD_ARGS(TSNX_CORE_NET, TSNX_PRIO_NETLOOP)) == 0) {
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
    // Incoming sockets the listener queued after the netloop's last drain
    // would otherwise leak (the engine routes nothing here once we are
    // unregistered, so the queue cannot refill).
    mutexLock(&tfs->inc_q.lock);
    for (int i = 0; i < tfs->inc_q.n; i++) close(tfs->inc_q.q[i].fd);
    tfs->inc_q.n = 0;
    mutexUnlock(&tfs->inc_q.lock);
    for (int i = 0; i < CACHE_MAX_CHUNKS; i++)
        if (tfs->chunks[i] >= 0) { close(tfs->chunks[i]); tfs->chunks[i] = -1; }
    cache_delete_all(tfs);  // per-playback scratch, not a library
    for (int i = 0; i < AQ_MAX; i++) {
        free(tfs->aq[i].buf);
        free(tfs->aq[i].have);
        free(tfs->aq[i].req);
        free(tfs->aq[i].req_ts);
        free(tfs->aq[i].req_by);
        free(tfs->aq[i].peer_from);
    }
    free(tfs->aq_of_piece);
    free(tfs->cand);
    free(tfs->cand_tmp);
    free(tfs->file_unwanted);
    for (int i = 0; i < tfs->aq_buf_pool_cnt; i++) {
        free(tfs->aq_buf_pool[i]);
        tfs->aq_buf_pool[i] = NULL;
    }
    tfs->aq_buf_pool_cnt = 0;
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

static void torrentfs_apply_select_file(torrentfs *tfs, int file_index) {
    int64_t new_offset = tfs->meta.files[file_index].offset;
    int64_t new_size   = tfs->meta.files[file_index].length;

    // Lock order: cache_lock -> lock (to match ram_evict convention)
    if (tfs->ram_mode) {
        mutexLock(&tfs->cache_lock);
    }
    mutexLock(&tfs->lock);

    tfs->stream_offset    = new_offset;
    tfs->stream_size      = new_size;
    tfs->file_first_piece = new_offset / tfs->meta.piece_len;
    tfs->file_last_piece  = (new_offset + new_size - 1) / tfs->meta.piece_len;
    tfs->playhead_piece   = tfs->file_first_piece;

    // Free any resident pieces that belong to the previous file or are outside the new file
    int64_t plen = tfs->meta.piece_len > 0 ? tfs->meta.piece_len : (1LL << 20);
    int64_t max_keep_hi = tfs->file_last_piece;
    if ((tfs->file_last_piece - tfs->file_first_piece) <= 4) {
        int64_t lookahead = (64LL << 20) / plen;
        if (lookahead < 1) lookahead = 1;
        if (lookahead > LOOKAHEAD_PIECES) lookahead = LOOKAHEAD_PIECES;
        max_keep_hi = tfs->file_last_piece + lookahead;
    }
    if (tfs->ram_mode && tfs->ram_piece) {
        for (int64_t p = 0; p < tfs->meta.piece_count; p++) {
            if (p < tfs->file_first_piece || p > max_keep_hi) {
                if (tfs->ram_piece[p]) {
                    uint8_t *freed_buf = tfs->ram_piece[p];
                    tfs->ram_piece[p] = NULL;
                    if (tfs->aq_buf_pool_cnt < 4 &&
                        (int64_t)tfs->aq_buf_pool_cnt * plen < (64LL << 20)) {
                        tfs->aq_buf_pool[tfs->aq_buf_pool_cnt++] = freed_buf;
                    } else {
                        free(freed_buf);
                    }
                }
                if (tfs->status && tfs->status[p] == PIECE_DONE) {
                    tfs->status[p] = PIECE_NEEDED;
                }
            }
        }
        int64_t res = 0;
        for (int64_t p = 0; p < tfs->meta.piece_count; p++) {
            if (tfs->ram_piece[p]) res += tfs->meta.piece_len;
        }
        tfs->ram_resident = res;
        tfs->ram_lo       = tfs->file_first_piece;
    }

    tfs->crit_head = (int)((CRIT_HEAD_BYTES + plen - 1) / plen);
    tfs->crit_tail = 0;
    if (tfs->crit_head < 1) tfs->crit_head = 1;
    if (tfs->crit_head > CRIT_HEAD_MAX) tfs->crit_head = CRIT_HEAD_MAX;

    // Reset scheduler zones
    if (tfs->piece_zone) {
        memset(tfs->piece_zone, 0, (size_t)tfs->meta.piece_count);
    }
    tfs->has_custom_zones = false;
    __atomic_add_fetch(&tfs->zone_gen, 1, __ATOMIC_RELEASE);
    tfs->cand_valid = false;

    for (int i = 0; i < MAX_SESS; i++)
        if (tfs->S[i].active) tfs->S[i].have_in_file = count_have_in_file(tfs, &tfs->S[i]);

    // Drop only in-flight requests that are outside the new file range and allowed lookahead window.
    // Retaining valid in-flight requests prevents peer stall during multi-file transitions.
    u64 now_sel = armGetSystemTick();
    for (int i = 0; i < MAX_SESS; i++) {
        if (tfs->S[i].active && tfs->S[i].nb.handshaked) {
            sess *s = &tfs->S[i];
            int kept = 0;
            for (int k = 0; k < s->req_n; k++) {
                int ep = s->req_piece[k];
                int eb = s->req_block[k];
                if (ep >= tfs->file_first_piece && ep <= max_keep_hi) {
                    if (kept != k) {
                        s->req_piece[kept] = s->req_piece[k];
                        s->req_block[kept] = s->req_block[k];
                        s->req_time[kept]  = s->req_time[k];
                    }
                    kept++;
                } else {
                    aq_entry *ea = aq_find(tfs, ep);
                    if (ea && eb >= 0 && eb < ea->nblocks) {
                        if (ea->req[eb] > 0 && ea->req[eb] < 255) ea->req[eb]--;
                        rb_clr(ea, eb, i);
                    }
                }
            }
            s->req_n = kept;
            s->last_block = now_sel;
            s->timeout_strikes = 0;
        }
    }

    // Reset active assembly queue (aq) only for pieces outside the new file range and allowed lookahead,
    // and restore their status from PIECE_ACTIVE back to PIECE_NEEDED so they are never orphaned.
    for (int i = 0; i < tfs->n_aq; i++) {
        if (tfs->aq[i].idx >= 0 &&
            (tfs->aq[i].idx < tfs->file_first_piece || tfs->aq[i].idx > max_keep_hi)) {
            int64_t old_idx = tfs->aq[i].idx;
            if (old_idx >= 0 && old_idx < tfs->meta.piece_count &&
                tfs->status && tfs->status[old_idx] == PIECE_ACTIVE) {
                tfs->status[old_idx] = PIECE_NEEDED;
            }
            tfs->aq[i].idx = -1;
            if (tfs->n_active_aq > 0) tfs->n_active_aq--;
            tfs->aq[i].have_cnt = 0;
            tfs->aq[i].next_req = 0;
            if (tfs->aq[i].have) memset(tfs->aq[i].have, 0, (size_t)tfs->blocks_per_piece);
            if (tfs->aq[i].req) memset(tfs->aq[i].req, 0, (size_t)tfs->blocks_per_piece);
            if (tfs->aq[i].req_ts) memset(tfs->aq[i].req_ts, 0, (size_t)tfs->blocks_per_piece * sizeof(uint64_t));
        }
        // Release idle aq slot buffers back to the OS heap during file transition
        if (tfs->aq[i].idx < 0 && tfs->aq[i].buf) {
            free(tfs->aq[i].buf);
            tfs->aq[i].buf = NULL;
        }
    }
    while (tfs->aq_buf_pool_cnt > 2) {
        free(tfs->aq_buf_pool[--tfs->aq_buf_pool_cnt]);
    }

    // Heal any orphaned PIECE_ACTIVE status and recompute pieces_done / pieces_ever for the new file
    int64_t done = 0;
    if (tfs->ever) {
        memset(tfs->ever, 0, (size_t)((tfs->meta.piece_count + 7) / 8));
    }
    if (tfs->status) {
        for (int64_t p = 0; p < tfs->meta.piece_count; p++) {
            if (tfs->status[p] == PIECE_ACTIVE && !aq_find(tfs, p)) {
                tfs->status[p] = PIECE_NEEDED;
            }
            if (p >= tfs->file_first_piece && p <= tfs->file_last_piece &&
                tfs->status[p] == PIECE_DONE) {
                done++;
                if (tfs->ever) {
                    tfs->ever[p >> 3] |= (uint8_t)(1u << (p & 7));
                }
            }
        }
    }
    tfs->pieces_done = done;
    tfs->pieces_ever = done;

    mutexUnlock(&tfs->lock);
    if (tfs->ram_mode) {
        mutexUnlock(&tfs->cache_lock);
    }
}

bool torrentfs_select_file(torrentfs *tfs, int file_index) {
    if (!tfs) return false;
    if (file_index < 0 || file_index >= tfs->meta.file_count) return false;

    int64_t new_offset = tfs->meta.files[file_index].offset;
    int64_t new_size = tfs->meta.files[file_index].length;
    if (new_size <= 0) return false;

    // If already on this file, nothing to change
    if (tfs->stream_offset == new_offset && tfs->stream_size == new_size) {
        tfs->cancel_reader = false;
        return true;
    }

    engine_log(ENGINE_LOG_INFO,
               "[torrentfs] select_file: switching to file %d (%lld bytes)",
               file_index, (long long)new_size);

    // 1. Unblock any current reader
    tfs->cancel_reader = true;
    tfs->read_blocked_piece = -1;

    // 2. Delegate aq[] / S[] reconfiguration to the netloop thread to avoid data races
    if (tfs->netloop_started && !tfs->stop) {
        __atomic_store_n(&tfs->pending_select_file, file_index, __ATOMIC_RELEASE);
        for (int wait_i = 0; wait_i < 500 && !tfs->stop; wait_i++) {
            if (__atomic_load_n(&tfs->pending_select_file, __ATOMIC_ACQUIRE) < 0) {
                break;
            }
            svcSleepThread(2000000ULL); // 2 ms
        }
        int leftover = __atomic_exchange_n(&tfs->pending_select_file, -1, __ATOMIC_ACQ_REL);
        if (leftover >= 0) {
            torrentfs_apply_select_file(tfs, leftover);
        }
    } else {
        torrentfs_apply_select_file(tfs, file_index);
    }

    tfs->cancel_reader = false;

    engine_log(ENGINE_LOG_INFO,
               "[torrentfs] select_file: now streaming pieces %lld..%lld (peers: %d)",
               (long long)tfs->file_first_piece, (long long)tfs->file_last_piece,
               tfs->peer_count);
    return true;
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
    if (!tfs) return;
    int64_t abs = tfs->stream_offset + offset;
    ((torrentfs *)tfs)->playhead_piece = abs / tfs->meta.piece_len;
    if (tfs->ram_mode) {
        mutexLock(&((torrentfs *)tfs)->cache_lock);
        ram_evict((torrentfs *)tfs);
        mutexUnlock(&((torrentfs *)tfs)->cache_lock);
    }
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
    if (!on) {
        tfs->announce_now = true;
    }
    engine_log(ENGINE_LOG_INFO, "[torrentfs] %s", on ? "paused" : "resumed");
}

void torrentfs_pause_dials(torrentfs *tfs, int seconds) {
    if (!tfs || seconds <= 0) return;
    u64 now = armGetSystemTick();
    tfs->pause_dials_until = now + (u64)seconds * tfs->freq;
    engine_log(ENGINE_LOG_INFO, "[torrentfs] paused new dials for %d seconds", seconds);
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
    if (!t->ram_mode || t->strict_verify) return false;
    if (t->status[idx] == PIECE_WRITING) return true;
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
    u64 last_wd = wait_start;
    int64_t last_recv = tfs->st_bytes_recv;
    u64 last_recv_tick = wait_start;
    while (!tfs->stop && !tfs->cancel_reader && !piece_ready(tfs, first, b0)) {
        // Woken when the piece verifies (or, non-strict, a block lands); the
        // 20 ms cap keeps the stall/heartbeat bookkeeping below running.
        mutexLock(&tfs->lock);
        if (!tfs->stop && !tfs->cancel_reader && !piece_ready(tfs, first, b0))
            condvarWaitTimeout(&tfs->piece_cv, &tfs->lock, 20000000ULL);
        mutexUnlock(&tfs->lock);
        u64 now = armGetSystemTick();
        if (tfs->paused) {
            wait_start = now;
            last_recv_tick = now;
            last_wd = now;
            continue;
        }
        if (tfs->st_bytes_recv != last_recv) {
            last_recv = tfs->st_bytes_recv;
            last_recv_tick = now;
        }
        if (now - last_wd > tfs->freq) {
            last_wd = now;
            hb_beat(tfs, HB_READER);
            tsnx_engine_wd_tick(6);
            u64 stall_secs = (now - last_recv_tick) / tfs->freq;
            if (stall_secs >= 20 && tfs->st_live == 0 && tfs->peer_count < 32) {
                tfs->announce_now = true;
            }
            // Self-heal if the needed piece is stuck in PIECE_ACTIVE without an aq slot
            mutexLock(&tfs->lock);
            if (first >= 0 && first < tfs->meta.piece_count &&
                tfs->status && tfs->status[first] == PIECE_ACTIVE && !aq_find(tfs, first)) {
                engine_log(ENGINE_LOG_WARN,
                           "[read] piece %lld orphaned in PIECE_ACTIVE, resetting to PIECE_NEEDED",
                           (long long)first);
                tfs->status[first] = PIECE_NEEDED;
            }
            mutexUnlock(&tfs->lock);
            // Return 0 after 45s of complete stall (when not paused) so the caller's retry/timeout logic can run
            if (!tfs->paused && stall_secs >= 45 && (now - wait_start) / tfs->freq >= 45) {
                engine_log(ENGINE_LOG_WARN,
                           "[read] stall timeout waiting for piece %lld block %d (stall=%llus, live=%d)",
                           (long long)first, b0, (unsigned long long)stall_secs, tfs->st_live);
                tfs->read_blocked_piece = -1;
                return 0;
            }
        }
    }
    tfs->read_blocked_piece = -1;
    if (tfs->stop || tfs->cancel_reader) return -1;
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
    mutexLock(&tfs->lock);
    condvarWakeAll(&tfs->piece_cv);
    condvarWakeAll(&tfs->wq_cv);
    mutexUnlock(&tfs->lock);
}

void torrentfs_cancel_reader(torrentfs *tfs) {
    if (!tfs) return;
    tfs->cancel_reader = true;
    mutexLock(&tfs->lock);
    condvarWakeAll(&tfs->piece_cv);
    mutexUnlock(&tfs->lock);
}

void torrentfs_resume_reader(torrentfs *tfs) {
    if (!tfs) return;
    tfs->cancel_reader = false;
}

void torrentfs_set_strict_verify(torrentfs *tfs, int on) {
    if (!tfs) return;
    tfs->strict_verify = on ? true : false;
    engine_log(ENGINE_LOG_INFO, "[torrentfs] strict_verify=%d", on ? 1 : 0);
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
    mutexLock(&t->snap_lock);
    int seeds = t->snap_seeds;
    mutexUnlock(&t->snap_lock);
    return seeds;
}

int torrentfs_get_peers(const torrentfs *tfs, torrentfs_peer_info *out, int max) {
    torrentfs *t = (torrentfs *)tfs;
    mutexLock(&t->snap_lock);
    int n = t->snap_n_peers < max ? t->snap_n_peers : max;
    if (n > 0) memcpy(out, t->snap_peers, (size_t)n * sizeof(out[0]));
    mutexUnlock(&t->snap_lock);
    return n < 0 ? 0 : n;
}

void torrentfs_live_peers(const torrentfs *tfs, int *live, int *peak,
                          int *connecting) {
    if (live) *live = tfs->st_live >= 0 ? tfs->st_live : 0;
    if (peak) *peak = tfs->st_peak_live;
    if (connecting) *connecting = tfs->st_connecting >= 0 ? tfs->st_connecting : 0;
}

void torrentfs_claim_stats(const torrentfs *tfs, int *claiming, int *idle) {
    torrentfs *t = (torrentfs *)tfs;
    mutexLock(&t->snap_lock);
    int c = t->snap_claiming, id = t->snap_idle;
    mutexUnlock(&t->snap_lock);
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
    if (zone > 0) tfs->has_custom_zones = true;
    __atomic_add_fetch(&tfs->zone_gen, 1, __ATOMIC_RELEASE);
    mutexUnlock(&tfs->lock);
    engine_log(ENGINE_LOG_DEBUG, "[zone] set first=%lld count=%lld zone=%d",
               (long long)first_piece, (long long)piece_count, zone);
    return true;
}

bool torrentfs_set_zones(torrentfs *tfs, const int64_t *first, const int64_t *count,
                         const int *zone, int n) {
    if (!tfs || !tfs->piece_zone || n < 0) return false;
    bool any = false;
    mutexLock(&tfs->lock);
    memset(tfs->piece_zone, 0, (size_t)tfs->meta.piece_count);
    for (int r = 0; r < n; r++) {
        int64_t f = first[r], c = count[r];
        int z = zone[r];
        if (f < 0 || c <= 0 || z <= 0) continue;
        if (f >= tfs->meta.piece_count) continue;
        if (f + c > tfs->meta.piece_count) c = tfs->meta.piece_count - f;
        if (z > 255) z = 255;
        memset(tfs->piece_zone + f, z, (size_t)c);
        any = true;
    }
    tfs->has_custom_zones = any;
    __atomic_add_fetch(&tfs->zone_gen, 1, __ATOMIC_RELEASE);   // one rebuild
    mutexUnlock(&tfs->lock);
    return true;
}

bool torrentfs_clear_piece_zones(torrentfs *tfs) {
    if (!tfs->piece_zone) return false;
    mutexLock(&tfs->lock);
    memset(tfs->piece_zone, 0, (size_t)tfs->meta.piece_count);
    tfs->has_custom_zones = false;
    __atomic_add_fetch(&tfs->zone_gen, 1, __ATOMIC_RELEASE);
    mutexUnlock(&tfs->lock);
    return true;
}

void torrentfs_set_file_wanted(torrentfs *tfs, int file_index, bool wanted) {
    if (!tfs || !tfs->file_unwanted || file_index < 0 || file_index >= tfs->meta.file_count)
        return;
    mutexLock(&tfs->lock);
    tfs->file_unwanted[file_index] = wanted ? 0 : 1;
    __atomic_add_fetch(&tfs->zone_gen, 1, __ATOMIC_RELEASE);   // rebuild candidates
    mutexUnlock(&tfs->lock);
}

void torrentfs_set_session_limit(torrentfs *tfs, int n) {
    if (!tfs) return;
    if (n <= 0 || n > MAX_SESS) n = MAX_SESS;
    tfs->live_max = n;
    tfs->live_target = n < DIAL_STOP_LIVE ? n : DIAL_STOP_LIVE;
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
    int64_t p, l, h;
    calc_window((torrentfs *)tfs, &p, &l, &h);
    if (ph) *ph = p;
    if (lo) *lo = l;
    if (hi) *hi = h;
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
