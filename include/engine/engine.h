#ifndef TSNX_ENGINE_H
#define TSNX_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Lightweight BitTorrent engine for TorrentShopNX.
 *
 * Replaces libtorrent/boost with a small streaming-first implementation
 * inspired by NX-torrent-player. The engine is written in C and exposes a
 * plain C API so the C++ backend can link against it without pulling in
 * libtorrent headers.
 */

#define TSNX_MAX_HASH_LEN   40
#define TSNX_MAX_NAME_LEN   256
#define TSNX_MAX_PATH_LEN   512
#define TSNX_MAX_FILES      256

typedef struct tsnx_engine tsnx_engine;
typedef struct tsnx_torrent tsnx_torrent;

/* Information about one file inside a torrent. */
typedef struct {
    int         index;
    char        path[TSNX_MAX_PATH_LEN];
    int64_t     size;
    int64_t     offset;     /* byte offset within the concatenated torrent */
    bool        wanted;
} tsnx_file_info;

/* Summary of one torrent, suitable for the download list UI. */
typedef struct {
    char        hash[TSNX_MAX_HASH_LEN + 1];
    char        name[TSNX_MAX_NAME_LEN];
    float       progress;          /* 0.0 .. 1.0 */
    float       download_kbps;
    int64_t     loaded_size;
    int64_t     total_size;
    int         seeds;
    int         peers;
    int         known_peers;
    int         dht_nodes;
} tsnx_torrent_item;

/* Global engine state. Created by tsnx_engine_start(), destroyed by stop(). */
tsnx_engine *tsnx_engine_start(int listen_port);
void         tsnx_engine_stop(tsnx_engine *eng);
bool         tsnx_engine_running(const tsnx_engine *eng);

/* Add a torrent from a magnet URI or a local .torrent file. */
bool tsnx_engine_add_magnet(tsnx_engine *eng, const char *magnet_uri,
                            char *out_hash, size_t out_hash_len);

/* Full-featured magnet add:
 *   file_index  - file streamed by the initial torrentfs open (-1 = largest)
 *   meta_only   - true: fetch metadata into the slot but do not start the
 *                 download threads (used by the file-list probe). The slot
 *                 stays registered; prepare_stream() opens the stream later.
 *   cancel      - polled while metadata is fetched (may be NULL). */
bool tsnx_engine_add_magnet_ex(tsnx_engine *eng, const char *magnet_uri,
                               int file_index, bool meta_only,
                               const volatile bool *cancel,
                               char *out_hash, size_t out_hash_len);

/* True if a torrent with this info-hash is registered in the engine. */
bool tsnx_engine_has_torrent(tsnx_engine *eng, const char *hash);

/* Update the cancel flag this torrent's (re)opens poll (may be NULL). */
void tsnx_engine_set_cancel(tsnx_engine *eng, const char *hash,
                            const volatile bool *flag);

bool tsnx_engine_add_torrent_file(tsnx_engine *eng, const char *path,
                                  char *out_hash, size_t out_hash_len);

/* Remove a torrent from the engine. */
bool tsnx_engine_remove_torrent(tsnx_engine *eng, const char *hash);

/* Pause / resume (best-effort; leech-only engine may treat these as no-ops). */
bool tsnx_engine_pause_torrent(tsnx_engine *eng, const char *hash);
bool tsnx_engine_resume_torrent(tsnx_engine *eng, const char *hash);

/* List all torrents. Returns number of items written (<= max_items). */
int  tsnx_engine_get_torrents(tsnx_engine *eng, tsnx_torrent_item *out,
                              int max_items);

/* List files inside a torrent. Returns number of files written (<= max_files). */
int  tsnx_engine_get_files(tsnx_engine *eng, const char *hash,
                           tsnx_file_info *out, int max_files);

/* Mark a file as wanted or not. Unwanted files are not downloaded. */
bool tsnx_engine_set_file_wanted(tsnx_engine *eng, const char *hash,
                                 int file_index, bool wanted);

/*
 * Streaming read API.
 *
 * prepare_stream() starts background download for a given file.
 * read() blocks until at least some of the requested data is available and
 * returns the number of contiguous bytes copied (short reads are normal).
 */
bool     tsnx_engine_prepare_stream(tsnx_engine *eng, const char *hash,
                                    int file_index);
int64_t  tsnx_engine_read(tsnx_engine *eng, const char *hash,
                          int64_t offset, void *buf, int64_t size);
void     tsnx_engine_cancel_read(tsnx_engine *eng, const char *hash);

/* Hints the engine how far the installer has already consumed. */
void tsnx_engine_set_min_keep_offset(tsnx_engine *eng, const char *hash,
                                     int64_t offset);

/* Current piece size and file offset within the torrent (for the installer). */
int      tsnx_engine_piece_size(tsnx_engine *eng, const char *hash);
int64_t  tsnx_engine_file_offset(tsnx_engine *eng, const char *hash);

/* --------------------------------------------------------------------------
 * Peer and scheduler API
 * -------------------------------------------------------------------------- */

typedef struct {
    uint32_t ip;          // network byte order
    uint16_t port;        // host byte order
    int64_t  bytes_recv;  // cumulative bytes received from this peer
    double   rate_bps;    // smoothed receive rate
    int      rtt_ms;      // -1 if unknown
    bool     connecting;
    bool     handshaked;
    bool     choked;
    int64_t  claim_piece; // -1 if none
} tsnx_peer_info;

// Fill up to `max_peers` live session entries; returns count written.
int tsnx_engine_get_peers(tsnx_engine *eng, const char *hash,
                          tsnx_peer_info *out, int max_peers);

// 5-zone piece priority. Zones are applied to absolute piece indices
// (relative to the whole torrent, not the streamed file).
typedef enum {
    TSNX_ZONE_NONE = 0,
    TSNX_ZONE_CRITICAL = 1,
    TSNX_ZONE_URGENT,
    TSNX_ZONE_PREFETCH,
    TSNX_ZONE_SPECULATIVE,
    TSNX_ZONE_TAIL
} tsnx_piece_zone;

// Assign a zone to a consecutive range of pieces. Replaces any previous
// external zone for those pieces. Returns false if the range is invalid.
bool tsnx_engine_set_piece_zone(tsnx_engine *eng, const char *hash,
                                int first_piece, int piece_count,
                                tsnx_piece_zone zone);

// Remove all externally-set zones and let the internal picker decide.
bool tsnx_engine_clear_piece_zones(tsnx_engine *eng, const char *hash);

// Calm mode: how many milliseconds of buffer are ahead of the consumer.
// Deeper backlog narrows the number of peers allowed to claim new work.
void tsnx_engine_set_backlog_ms(tsnx_engine *eng, const char *hash, int ms);

// Global governor (1 = on, 0 = off). When on and backlog is deep, claiming
// pauses while the measured rate exceeds a backlog-tied target.
void tsnx_engine_set_governor(tsnx_engine *eng, int on);

// Global RAM streaming mode. Affects only torrents opened after the call.
void tsnx_engine_set_ram_stream(tsnx_engine *eng, int on);

// Force an immediate tracker re-announce for this torrent.
bool tsnx_engine_announce_now(tsnx_engine *eng, const char *hash);

// Detailed runtime diagnostics for the streaming engine.
typedef struct {
    int peers;              // peers discovered (tracker + DHT)
    int live;               // currently handshaked sessions
    int peak;               // high-water live sessions
    int connecting;         // outbound TCP connects in flight
    int claiming;           // sessions with an active piece claim
    int idle;               // unchoked sessions with nothing to claim
    int empty_bitfield;     // peers that advertised no pieces
    int good_bitfield;      // bitfield messages accepted
    int bad_bitfield;       // bitfield messages with wrong length
    int sock_fail;          // socket()/connect() refused immediately
    int timeouts;           // SYN sent but no answer
    int calm;               // current calm-mode session budget
    int dht_peers;          // peers found by the last completed DHT lookup
    int dht_good;           // DHT good nodes seen during last lookup
    int dht_dubious;        // DHT dubious nodes seen during last lookup
    int64_t bytes_recv;     // cumulative bytes received
    int64_t dup_bytes;      // received payloads for blocks already held
    float download_kbps;    // smoothed download speed
    int64_t playhead_piece; // current playhead piece index
    int64_t pieces_done;    // pieces currently resident
    int64_t pieces_total;   // total pieces in the streamed file
    int piece_status;       // playhead piece state
    int piece_have;         // blocks we have for playhead piece
    int piece_req;          // blocks requested for playhead piece
    int piece_total;        // total blocks in playhead piece
} tsnx_engine_diag;

bool tsnx_engine_get_diag(tsnx_engine *eng, const char *hash, tsnx_engine_diag *out);

#ifdef __cplusplus
}
#endif

#endif /* TSNX_ENGINE_H */
