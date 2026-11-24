#ifndef META_FETCH_H
#define META_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "torrent_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t       *info_hash;     /* 20 bytes (required) */
    const uint8_t       *peer_id;       /* 20 bytes (required) */
    torrent_meta        *announce_stub; /* optional: parallel tracker announces */
    bool                 use_dht;       /* subscribe to background DHT (hungry=1) */
    const peer_addr     *seed_peers;    /* optional initial peer list */
    int                  seed_count;
    int                  budget_ms;     /* <= 0 -> default 50000 ms */
    int                  max_sess;      /* <= 0 -> default 16 (8 if active downloads) */
    const volatile bool *cancel;        /* polled every <= 50 ms */
} meta_fetch_opts;

/* Fetch BEP 9 metadata using a single non-blocking poll() loop supporting TCP,
 * MSE/PE encryption, uTP fallback, incoming TCP/uTP connections, ut_pex
 * discovery, parallel tracker+DHT streaming, and multi-peer piece assembly.
 * Returns 0 on success (*out_meta malloc'd, caller frees), or -1 on failure. */
int meta_fetch_run(const meta_fetch_opts *opts,
                   uint8_t **out_meta, size_t *out_len,
                   peer_addr *out_peers, int max_peers, int *out_peer_n,
                   char *err, size_t errlen);

/* Single-peer BEP 9 fetch (cancellable, MSE/uTP/large-BITFIELD capable). */
int meta_fetch_single_peer(peer_addr addr, const uint8_t info_hash[20],
                           const uint8_t peer_id[20],
                           const volatile bool *cancel,
                           uint8_t **out, size_t *out_len,
                           char *err, size_t errlen);

/* Incoming TCP/uTP connection routing hooks (called from engine.c). */
bool meta_fetch_route_incoming(const uint8_t info_hash[20], int fd,
                               const uint8_t hs68[68]);
bool meta_fetch_route_incoming_utp(const uint8_t info_hash[20], void *utp,
                                   const uint8_t hs68[68],
                                   uint32_t ip_net, uint16_t port_host);

/* Router for incoming uTP connections accepted during meta_fetch that belong
 * to other open torrents (installed by engine.c -> engine_route_incoming_utp). */
typedef bool (*meta_utp_route_fn)(const uint8_t info_hash[20], void *utp,
                                  const uint8_t hs68[68],
                                  uint32_t ip_net, uint16_t port_host);
void meta_fetch_set_utp_router(meta_utp_route_fn fn);

/* Initialize registry mutex early at engine startup. */
void meta_fetch_init_early(void);

/* Cap concurrent metadata sockets to 8 when another download is already active. */
void meta_fetch_set_low_concurrency(bool low);

#ifdef __cplusplus
}
#endif

#endif
