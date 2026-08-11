#ifndef UDP_TRACKER_H
#define UDP_TRACKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "torrent_meta.h"

// Announce to a udp:// tracker (BEP 15). Fills peers (up to max_peers) and
// returns the count, or -1 on failure. peer_id must be 20 bytes.
// `cancel` (may be NULL) is polled between retransmits so a teardown aborts the
// announce instead of waiting out the recv timeouts.
int udp_announce(const char *url, const uint8_t info_hash[20],
                 const uint8_t peer_id[20], int64_t left,
                 peer_addr *peers, int max_peers,
                 const volatile bool *cancel, char *err, size_t errlen);

#endif
