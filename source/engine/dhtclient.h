#ifndef DHTCLIENT_H
#define DHTCLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "torrent_meta.h"  // peer_addr

// Peer discovery over the mainline DHT (BEP 5), implemented on top of the
// battle-tested jech/dht library (source/dht.c). Peers are delivered
// incrementally through `cb` as the lookup finds them.
//
// Runs for at most `budget_ms`, or stops early once about `target_peers` peers
// have been delivered. `cancel` (may be NULL) is polled to stop early. Returns
// the number of peers delivered, or -1 on failure (message in err).
typedef void (*dht_peer_cb)(void *ctx, const peer_addr *peers, int n);

int dht_find_peers(const uint8_t info_hash[20], int target_peers, int budget_ms,
                   dht_peer_cb cb, void *ctx, const volatile bool *cancel,
                   char *err, size_t errlen);

// Optional debug logger; if set, the lookup reports progress through it.
void dht_set_log(void (*fn)(const char *msg));

// Last seen DHT node counts from the most recent lookup (racy but diagnostic).
void dhtclient_get_nodes(int *good, int *dubious);

// Peers found by the last completed lookup plus the last node counts.
void dhtclient_get_last_lookup(int *peers_found, int *good_nodes, int *dubious_nodes);

// Start a persistent background DHT process for one info-hash.  The routing
// table is kept alive and refreshed, and peers are delivered through cb as
// they are found.  Only one info-hash is tracked at a time; adding a new one
// replaces the previous.  The background process stops when remove is called.
void dht_background_add(const uint8_t info_hash[20], dht_peer_cb cb, void *ctx);
void dht_background_remove(const uint8_t info_hash[20]);

#endif
