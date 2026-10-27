#ifndef TSNX_LISTEN_H
#define TSNX_LISTEN_H

#include <stdbool.h>
#include <stdint.h>

#include <switch.h>   // Mutex (pctest/compat on PC)

typedef struct torrentfs torrentfs;

// Engine-wide incoming-connection listener. One listen socket serves every open
// torrent: the acceptor reads the peer's 68-byte handshake (to learn the
// info_hash) and routes the socket to the matching torrentfs via `find`.
//
// Incoming connections are what lets firewalled/NAT'd seeders reach us: they
// cannot be dialed outbound, but they dial every client whose (UPnP-forwarded)
// port the tracker/DHT advertised. Without them those seeders are invisible,
// which on swarms full of home connections leaves only the few port-forwarded
// seeds reachable.
//
// v1 limitation: plaintext handshakes only. A peer that insists on MSE
// (encryption-required incoming) starts with a 96-byte pubkey, not the
// "BitTorrent protocol" preamble, and is dropped. Most clients accept
// plaintext incoming by default; the MSE responder side can be added later.

// Hands an accepted connection (its 68-byte handshake already read) to the
// torrent with this info_hash. The engine looks the torrent up and queues the
// socket in one step under its registry lock, so the torrent cannot be closed
// in between. Returns false when no such torrent is open (the caller closes fd).
typedef bool (*listen_route_fn)(const uint8_t info_hash[20], int fd,
                                const uint8_t hs68[68]);

// Returns 0 on success, -1 if the socket could not be created/bound.
int  torrent_listener_start(int port, listen_route_fn route);
void torrent_listener_stop(void);

// The port actually bound (0 if the listener is not running). Announce this
// to trackers/DHT unless UPnP mapped a different external port.
int  torrent_listener_port(void);

// torrentfs side: the listener hands an accepted socket (plus the 68 bytes of
// the peer's handshake it already consumed) to the matching torrentfs. The
// netloop attaches it to a session on its next upkeep tick; if no free
// session slot exists by then the socket is dropped.
void torrentfs_incoming_push(torrentfs *t, int fd, const uint8_t hs68[68]);

// The per-torrentfs pending-incoming queue (defined here so torrentfs.c can
// embed it by value; the listener only ever touches it through the push API).
#define TSNX_INCOMING_QUEUE_MAX 8

typedef struct {
    int  fd;
    uint8_t hs[68];
} tsnx_inc_conn;

struct torrentfs_incoming_q {
    Mutex        lock;
    tsnx_inc_conn q[TSNX_INCOMING_QUEUE_MAX];
    int          n;
};

struct torrentfs_incoming_q *torrentfs_incoming_queue(torrentfs *t);

#endif
