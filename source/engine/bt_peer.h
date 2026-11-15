#ifndef BT_PEER_H
#define BT_PEER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>  // ssize_t

#include "torrent_meta.h"
#include "mse_peer.h"

// BitTorrent peer wire protocol message IDs
enum {
    MSG_CHOKE = 0,
    MSG_UNCHOKE = 1,
    MSG_INTERESTED = 2,
    MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4,
    MSG_BITFIELD = 5,
    MSG_REQUEST = 6,
    MSG_PIECE = 7,
    MSG_CANCEL = 8,
    MSG_PORT = 9,        // BEP 5: payload = <u16 DHT UDP port>
    MSG_EXTENDED = 20,   // BEP 10: payload = <ext_id><bencoded dict>
};

// Extended message ids WE assign (advertised in our extended handshake's "m").
enum {
    EXT_ID_PEX = 1,        // ut_pex (BEP 11)
    EXT_ID_HOLEPUNCH = 2,  // ut_holepunch (BEP 55)
};

// Blocks are the unit of transfer within a piece (16 KiB is the de-facto standard).
#define BLOCK_LEN (16 * 1024)

// Largest peer-wire message payload we accept/emit (one block plus headroom).
#define MAX_MSG_LEN (BLOCK_LEN + 512)

// TCP connect timeout. Each worker is blocked for this long on every peer that
// never answers, and on a big swarm most peers are NAT'd or dead -- so this
// directly bounds how fast the (few) reachable peers get found. A live peer's
// SYN-ACK lands in tens of milliseconds; 3 s tolerates a slow/loaded WiFi link
// while still moving through dead peers quickly.
#define PEER_CONNECT_SECS 3

typedef struct {
    int sock;           // TCP socket, or -1 when using µTP
    void *utp;          // µTP connection (utp_conn*), or NULL when using TCP
    bool choked;        // peer is choking us -> we cannot request
    uint8_t *bitfield;  // which pieces the peer has; NULL if not received yet
    size_t bitfield_len;
} peer_conn;

// ---------------------------------------------------------------------------
// Non-blocking peer transport (TCP only), for the poll()-driven session loop.
//
// The blocking peer_conn path above dedicates a thread (and, while parked in
// recv(), one of libnx's 16 BSD sessions) to a single peer, which caps us at
// ~11 concurrent peers. These primitives never block, so one thread can drive
// a hundred peers through a single poll() -- one BSD session for the whole set.
// ---------------------------------------------------------------------------

// Framed reads need to survive short reads, so bytes accumulate here until a
// whole message is present. Sized well past the largest message (4-byte length
// prefix + one 16 KiB block): every recv() on Switch is a bsd IPC, so a small
// buffer turns a fast peer's socket drain into a storm of small IPCs. 64 KiB
// cuts the per-session recv IPC count ~4x versus one-message buffering; at 48
// sessions that is 6 MB total.
#define PEER_RX_CAP (512 * 1024)

typedef struct {
    int sock;
    void *utp;          // non-blocking µTP session, or NULL for TCP

    uint8_t *rx;        // accumulated inbound bytes (partial messages included)
    size_t rx_off;      // bytes at the front already handed out to the caller
    size_t rx_len;      // total bytes held, rx_off included
    bool rx_more;       // last pump stopped on a full buffer: more may be waiting

    uint8_t *tx;        // queued outbound bytes, drained as the socket allows
    size_t tx_head;     // bytes already sent from the front of tx
    size_t tx_len;
    size_t tx_cap;

    bool handshaked;    // peer's 68-byte handshake received and validated
    bool choked;        // peer is choking us -> we cannot request
    bool we_unchoked;   // we have unchoked the peer
    bool ext_ok;        // peer advertised BEP 10 extensions in its handshake
    bool dht_ok;        // peer set the DHT bit (BEP 5): it accepts a PORT message
    uint8_t pex_id;     // peer's ut_pex extended message id (0 = not offered)
    uint8_t hp_id;      // peer's ut_holepunch extended message id (0 = not offered)

    uint8_t *bitfield;  // which pieces the peer has
    size_t bitfield_len;

    // MSE/PE (Message Stream Encryption). mse_want: dial with the encrypted
    // handshake first; mse_active: the stream is crypto-live (all framed
    // traffic is RC4 when rc4_selected); mse_failed: the handshake concluded
    // the peer does not speak MSE, so the caller should retry plaintext.
    bool mse_want;
    bool mse_active;
    bool mse_failed;
    bool mse_ia_sent;   // our 68-byte handshake queued on the stream after MSE
    mse_peer mse;
} peer_nb;

// Set `sock` non-blocking and attach buffers. Takes ownership of sock.
// Returns 0, or -1 on allocation failure (sock untouched).
int peer_nb_init(peer_nb *p, int sock, int64_t piece_count);
// Queue a 4-byte keep-alive through the tx queue (so uTP sessions get it
// too, and it is never interleaved with a partly sent message).
int peer_nb_queue_keepalive(peer_nb *p);
// Same, but uses a µTP session instead of a TCP socket.
int peer_nb_init_utp(peer_nb *p, void *utp, int64_t piece_count);
// Post-handshake socket buffers. Returns the receive buffer that was set, in
// KB (0 for uTP sessions / no socket).
int peer_nb_apply_speed_opts(peer_nb *p);
// Set SO_RCVBUF to `bytes`, halving down to 64 KB if the stack refuses.
// Returns the size that stuck, or 0 (TCP only; no-op without a socket).
int peer_nb_set_rcvbuf(peer_nb *p, int bytes);
void peer_nb_free(peer_nb *p);

// Drain the socket into the rx buffer. Returns the number of bytes read (0 if
// the socket had nothing), or -1 if the peer closed or errored. The count is
// what tells a genuinely silent peer from a spurious poll wakeup.
ssize_t peer_nb_pump_rx(peer_nb *p);

// Pop one complete message out of the rx buffer, skipping keep-alives.
// Returns 1 and sets *id/*payload/*plen, 0 if no whole message is buffered yet,
// -1 if the peer framed something illegal. *payload points into p->rx and stays
// valid until the next peer_nb_next/peer_nb_pump_rx call.
int peer_nb_next(peer_nb *p, uint8_t *id, uint8_t **payload, uint32_t *plen);

// Queue a framed message. Never blocks; grows the tx buffer as needed.
int peer_nb_queue(peer_nb *p, uint8_t id, const void *payload, uint32_t plen);

// Push as much of the tx queue as the socket accepts. 0 = ok, -1 = dead.
int peer_nb_flush(peer_nb *p);

// True while there are bytes still waiting to go out (poll for POLLOUT).
bool peer_nb_tx_pending(const peer_nb *p);

// Queue our 68-byte handshake. Call once, right after connect() completes.
int peer_nb_send_handshake(peer_nb *p, const uint8_t info_hash[20],
                           const uint8_t peer_id[20]);

// Consume the peer's handshake from rx once it has fully arrived.
// 1 = handshaked ok, 0 = still waiting, -1 = wrong protocol/info_hash.
// On success p->ext_ok records whether the peer advertises BEP 10 extensions.
int peer_nb_recv_handshake(peer_nb *p, const uint8_t info_hash[20]);

// Queue an extended-protocol message (BEP 10): <len><20><ext_id><payload>.
int peer_nb_queue_ext(peer_nb *p, uint8_t ext_id, const void *payload,
                      uint32_t plen);

// Build OUR extended handshake dictionary (the payload after ext_id 0) into
// `out`; returns its length, or 0 if it does not fit. It carries:
//   m       ut_pex (EXT_ID_PEX) and, with `holepunch`, ut_holepunch
//   p       our listen port (omitted when listen_port <= 0)
//   reqq    how many requests we queue per peer
//   v       client name
//   yourip  the peer's address as we see it (omitted when peer_ip_net == 0)
size_t peer_build_ext_handshake(char *out, size_t cap, int listen_port,
                                uint32_t peer_ip_net, bool holepunch);

// Queue OUR extended handshake (ext_id 0), see peer_build_ext_handshake.
// Only meaningful if the peer's handshake showed the extension bit.
int peer_nb_queue_ext_handshake(peer_nb *p, int listen_port,
                                uint32_t peer_ip_net, bool holepunch);

// What the peer told us in ITS extended handshake. Fields it did not send
// stay 0. Never trusts lengths: every value is range-checked.
typedef struct {
    uint8_t  pex_id;       // m.ut_pex
    uint8_t  hp_id;        // m.ut_holepunch
    int      reqq;         // requests the peer queues for us (0 = not stated)
    int      port;         // p: the peer's listen port (0 = not stated)
    bool     upload_only;
    uint32_t yourip;       // our address as the peer sees it, network order (0 = none)
} peer_ext_info;

// Parse a peer's extended handshake payload. Returns false if it is not a
// bencoded dictionary.
bool peer_parse_ext_handshake(const uint8_t *pl, uint32_t plen, peer_ext_info *out);

// ut_holepunch (BEP 55). A peer behind NAT cannot be dialed, but a third peer
// connected to both of us (the relay) can tell each side to open a uTP
// connection to the other at the same moment, which gets through most NATs.
//   RENDEZVOUS  initiator -> relay : "introduce me to <addr>"
//   CONNECT     relay -> both      : "start a uTP connection to <addr> now"
//   ERROR       relay -> initiator : "<addr>: <code>"
enum { HP_RENDEZVOUS = 0, HP_CONNECT = 1, HP_ERROR = 2 };
enum { HP_ERR_NO_SUCH_PEER = 1, HP_ERR_NOT_CONNECTED = 2, HP_ERR_NO_SUPPORT = 3,
       HP_ERR_NO_SELF = 4 };

typedef struct {
    uint8_t  type;     // HP_*
    uint32_t ip;       // network byte order (IPv4 only)
    uint16_t port;     // host byte order
    uint32_t err;      // HP_ERR_*, ERROR messages only
} peer_hp_msg;

// Parse a ut_holepunch payload (the bytes after the extended message id).
// False for anything malformed, an unknown type or an IPv6 address.
bool peer_parse_holepunch(const uint8_t *pl, uint32_t plen, peer_hp_msg *out);
// Build one into `out` (at least 12 bytes); returns its length.
size_t peer_build_holepunch(uint8_t *out, const peer_hp_msg *m);

void peer_disconnect(peer_conn *p);

bool peer_has_piece(const peer_conn *p, int64_t index);

// Same test against a raw bitfield, so the blocking (peer_conn) and
// non-blocking (peer_nb) paths can share the piece-selection code.
bool bf_has_piece(const uint8_t *bf, size_t bf_len, int64_t index);

// Low-level message primitives, for callers that run their own session loop
// (e.g. a bidirectional download+upload loop). peer_send writes one message;
// peer_recv reads one (skipping keep-alives), returning the message id and
// setting *plen, or -1 on error/timeout. `payload`/`bufcap` size the buffer.
int peer_send(peer_conn *p, uint8_t id, const void *payload, uint32_t plen);
int peer_recv(peer_conn *p, uint8_t *payload, uint32_t bufcap, uint32_t *plen);

// Connect to a peer and download the torrent metadata (the info dict) via the
// extension protocol (BEP 10) + ut_metadata (BEP 9), given only the info hash.
// On success returns 0 and sets *out to a malloc'd buffer of *out_len bytes
// whose SHA-1 equals info_hash (caller frees). This is how magnet links obtain
// what a .torrent file would have contained.
int peer_fetch_metadata(peer_addr addr, const uint8_t info_hash[20],
                        const uint8_t peer_id[20], uint8_t **out, size_t *out_len,
                        char *err, size_t errlen);

#endif
