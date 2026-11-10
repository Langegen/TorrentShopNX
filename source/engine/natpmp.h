#ifndef TSNX_NATPMP_H
#define TSNX_NATPMP_H

#include <stdbool.h>
#include <stdint.h>

// NAT-PMP (RFC 6886): the port-mapping protocol of routers that do not speak
// UPnP IGD (Apple routers, many OpenWrt/miniupnpd setups with UPnP switched
// off). One UDP request/reply per mapping, sent to the gateway on port 5351.
//
// All calls block for at most ~2 s (three tries: 250, 500, 1000 ms) and poll
// `stop` (may be NULL) between tries.

#define NATPMP_PORT 5351

enum {
    NATPMP_OK = 0,
    NATPMP_ERR_SOCKET = -1,     // local socket error
    NATPMP_ERR_NO_REPLY = -2,   // the gateway does not answer on 5351
    NATPMP_ERR_REFUSED = -3,    // answered with a result code != 0 (see *result)
    NATPMP_ERR_BAD_REPLY = -4,  // answered with something that is not NAT-PMP
};

// Ask the gateway for its external IPv4 address (network byte order).
int natpmp_external_ip(uint32_t gw_ip_net, int gw_port, uint32_t *ext_ip_net,
                       const volatile bool *stop, int *result);

// Map external `want_ext_port` -> our `internal_port` for `lifetime_secs`
// (0 deletes the mapping). udp: true = UDP, false = TCP. On NATPMP_OK
// *ext_port is the port the gateway actually assigned and *granted_secs the
// lifetime it granted -- renew at half of it.
int natpmp_map(uint32_t gw_ip_net, int gw_port, bool udp, int internal_port,
               int want_ext_port, uint32_t lifetime_secs,
               int *ext_port, uint32_t *granted_secs,
               const volatile bool *stop, int *result);

#endif
