#include "natpmp.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <switch.h>

// One request, up to three tries. Returns the reply length (> 0), or a
// NATPMP_ERR_* code. Only a datagram from the gateway's address and with the
// expected opcode (request opcode + 128) is accepted: anything else on the
// LAN could answer on this socket.
static int exchange(uint32_t gw_ip_net, int gw_port, const uint8_t *req, size_t req_len,
                    uint8_t *resp, size_t resp_cap, const volatile bool *stop) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return NATPMP_ERR_SOCKET;

    struct sockaddr_in gw;
    memset(&gw, 0, sizeof(gw));
    gw.sin_family = AF_INET;
    gw.sin_addr.s_addr = gw_ip_net;
    gw.sin_port = htons((uint16_t)gw_port);

    int rc = NATPMP_ERR_NO_REPLY;
    int wait_ms = 250;
    for (int attempt = 0; attempt < 3 && !(stop && *stop); attempt++, wait_ms *= 2) {
        // Blocking recvfrom with a receive timeout: select() does not report
        // UDP readability on the console.
        struct timeval tv = { wait_ms / 1000, (wait_ms % 1000) * 1000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        if (sendto(sock, (const char *)req, req_len, 0, (struct sockaddr *)&gw, sizeof(gw)) < 0) {
            rc = NATPMP_ERR_SOCKET;
            continue;
        }
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(sock, (char *)resp, resp_cap, 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) { rc = NATPMP_ERR_NO_REPLY; continue; }
        if (from.sin_addr.s_addr != gw_ip_net) { rc = NATPMP_ERR_BAD_REPLY; continue; }
        if (n < 4 || resp[0] != 0 || resp[1] != (uint8_t)(req[1] + 128)) {
            rc = NATPMP_ERR_BAD_REPLY;
            continue;
        }
        rc = (int)n;
        break;
    }
    close(sock);
    return rc;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int natpmp_external_ip(uint32_t gw_ip_net, int gw_port, uint32_t *ext_ip_net,
                       const volatile bool *stop, int *result) {
    const uint8_t req[2] = { 0, 0 };   // version 0, opcode 0
    uint8_t resp[32];
    if (result) *result = 0;
    int n = exchange(gw_ip_net, gw_port, req, sizeof(req), resp, sizeof(resp), stop);
    if (n < 0) return n;
    int res = rd16(resp + 2);
    if (result) *result = res;
    if (res != 0) return NATPMP_ERR_REFUSED;
    if (n < 12) return NATPMP_ERR_BAD_REPLY;
    if (ext_ip_net) memcpy(ext_ip_net, resp + 8, 4);   // already network order
    return NATPMP_OK;
}

int natpmp_map(uint32_t gw_ip_net, int gw_port, bool udp, int internal_port,
               int want_ext_port, uint32_t lifetime_secs,
               int *ext_port, uint32_t *granted_secs,
               const volatile bool *stop, int *result) {
    if (result) *result = 0;
    if (internal_port <= 0 || internal_port > 65535 || want_ext_port < 0 || want_ext_port > 65535)
        return NATPMP_ERR_SOCKET;
    uint8_t req[12];
    req[0] = 0;                     // version
    req[1] = udp ? 1 : 2;           // opcode: map UDP / map TCP
    req[2] = req[3] = 0;            // reserved
    req[4] = (uint8_t)(internal_port >> 8);
    req[5] = (uint8_t)internal_port;
    req[6] = (uint8_t)(want_ext_port >> 8);
    req[7] = (uint8_t)want_ext_port;
    req[8]  = (uint8_t)(lifetime_secs >> 24);
    req[9]  = (uint8_t)(lifetime_secs >> 16);
    req[10] = (uint8_t)(lifetime_secs >> 8);
    req[11] = (uint8_t)lifetime_secs;

    uint8_t resp[32];
    int n = exchange(gw_ip_net, gw_port, req, sizeof(req), resp, sizeof(resp), stop);
    if (n < 0) return n;
    int res = rd16(resp + 2);
    if (result) *result = res;
    if (res != 0) return NATPMP_ERR_REFUSED;
    if (n < 16) return NATPMP_ERR_BAD_REPLY;
    // The reply must be about OUR internal port.
    if (rd16(resp + 8) != (uint16_t)internal_port) return NATPMP_ERR_BAD_REPLY;
    int mapped = rd16(resp + 10);
    if (lifetime_secs != 0 && mapped == 0) return NATPMP_ERR_BAD_REPLY;
    if (ext_port) *ext_port = mapped;
    if (granted_secs) *granted_secs = rd32(resp + 12);
    return NATPMP_OK;
}
