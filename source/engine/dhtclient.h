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

// Override the node-cache file location (defaults to the SD path on Switch).
// Mostly useful for PC tests of the warm start.
void dht_set_cache_path(const char *path);

// Last seen DHT node counts from the most recent lookup (racy but diagnostic).
void dhtclient_get_nodes(int *good, int *dubious);

// Peers found by the last completed lookup plus the last node counts.
void dhtclient_get_last_lookup(int *peers_found, int *good_nodes, int *dubious_nodes);

// Register a persistent target on the shared background DHT: it keeps
// searching the info-hash and delivers peers through cb until removed. The
// background process itself keeps running across add/remove (one warm
// routing table for the whole engine session) and stops with dht_stop().
void dht_background_add(const uint8_t info_hash[20], dht_peer_cb cb, void *ctx);
void dht_background_remove(const uint8_t info_hash[20]);

// Stop the persistent background DHT (engine shutdown). Saves the node cache
// for the next session's warm start.
void dht_stop(void);

#endif
