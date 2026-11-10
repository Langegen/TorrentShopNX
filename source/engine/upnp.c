#include "upnp.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <switch.h>
#include <curl/curl.h>

#include "torrent_meta.h"   // torrent_set_announce_port
#include "engine_log.h"
#include "thread_cfg.h"
#include "natpmp.h"

// NAT-PMP mapping lifetime we ask for; renewed at half of what was granted.
#define NATPMP_LIFETIME_SECS 7200

enum { MAP_PROBING = 0, MAP_NONE, MAP_UPNP, MAP_NATPMP };
static volatile int s_map_kind = MAP_NONE;

const char *upnp_mapping_kind(void) {
    switch (s_map_kind) {
        case MAP_PROBING: return "probing";
        case MAP_UPNP:    return "upnp";
        case MAP_NATPMP:  return "natpmp";
        default:          return "none";
    }
}

#define SSDP_ADDR      "239.255.255.250"
#define SSDP_PORT      1900
#define SSDP_BIND_PORT 1900     // preferred local port; ephemeral as fallback
#define SSDP_MSEND     3        // M-SEARCH transmissions
#define SSDP_WAIT_MS   500      // per transmission, recv timeout
#define HTTP_TIMEOUT_SECS 4

static Thread        s_thread;
static bool          s_started = false;
static volatile bool s_stop = false;
static int           s_internal_port = 0;

// --- tiny HTTP/XML helpers --------------------------------------------------

typedef struct {
    char *data;
    size_t len;
} membuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    membuf *m = ud;
    size_t n = size * nmemb;
    // Descriptions are a few KB at most. The cap covers the whole reply: any
    // device on the LAN can answer SSDP, and a per-chunk cap let it make us
    // grow this buffer without limit.
    if (n > 512 * 1024 || m->len + n > 512 * 1024) return 0;
    char *p = realloc(m->data, m->len + n + 1);
    if (!p) return 0;
    m->data = p;
    memcpy(m->data + m->len, ptr, n);
    m->len += n;
    m->data[m->len] = '\0';
    return n;
}

static char *str_find_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return (char *)p;
    }
    return NULL;
}

// Extracts the first HTTP header value (up to CR/LF). Returns malloc'd string.
static char *header_value(const char *resp, const char *name) {
    char *h = str_find_ci(resp, name);
    if (!h) return NULL;
    h += strlen(name);
    while (*h == ' ' || *h == ':') h++;
    const char *end = h;
    while (*end && *end != '\r' && *end != '\n') end++;
    size_t n = (size_t)(end - h);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, h, n);
    out[n] = '\0';
    return out;
}

// http:// or https:// prefix; returns 1 if the string starts with either.
static int has_scheme(const char *url) {
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0;
}

// --- LAN addresses -------------------------------------------------------------

// Our LAN address and the default gateway, network byte order. The gateway
// comes from the system where it can tell us; otherwise it is guessed as host
// .1 of our /24, which is what nearly every home router uses.
static bool lan_addrs(uint32_t *local, uint32_t *gw) {
    *local = 0;
    *gw = 0;
    // UDP "connect" to a public address sends nothing; the route lookup
    // gives the interface address.
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(53);
        sa.sin_addr.s_addr = inet_addr("8.8.8.8");
        struct sockaddr_in me;
        socklen_t mel = sizeof(me);
        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
            getsockname(sock, (struct sockaddr *)&me, &mel) == 0)
            *local = me.sin_addr.s_addr;
        close(sock);
    }
#ifdef __SWITCH__
    // nifm is only opened in title mode (main.cpp userAppInit): never call
    // into it when it is not, an IPC on a dead session is not an error result.
    if (serviceIsActive(nifmGetServiceSession_GeneralService())) {
        u32 ip = 0, mask = 0, g = 0, d1 = 0, d2 = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&ip, &mask, &g, &d1, &d2)) && g != 0) {
            *gw = g;
            if (ip != 0) *local = ip;
        }
    }
#endif
    if (*local == 0) return false;
    if (*gw == 0) *gw = (*local & htonl(0xFFFFFF00u)) | htonl(1);
    return true;
}

// --- SSDP --------------------------------------------------------------------

// `gw` (network order, 0 = unknown) also gets every M-SEARCH as a unicast
// datagram: routers that filter multicast between Wi-Fi and LAN, or drop IGMP
// from a console, still answer a search addressed to them directly.
static int ssdp_discover(char *location, size_t loclen, uint32_t gw) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_in bindaddr = {0};
    bindaddr.sin_family = AF_INET;
    bindaddr.sin_addr.s_addr = INADDR_ANY;
    bindaddr.sin_port = htons(SSDP_BIND_PORT);
    if (bind(sock, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) != 0) {
        bindaddr.sin_port = 0;
        if (bind(sock, (struct sockaddr *)&bindaddr, sizeof(bindaddr)) != 0) {
            close(sock);
            return -1;
        }
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = SSDP_WAIT_MS * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in maddr = {0};
    maddr.sin_family = AF_INET;
    maddr.sin_port = htons(SSDP_PORT);
    maddr.sin_addr.s_addr = inet_addr(SSDP_ADDR);

    static const char *STS[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
    };

    char req[512];
    for (int t = 0; t < SSDP_MSEND && !s_stop; t++) {
        for (size_t s = 0; s < sizeof(STS) / sizeof(*STS); s++) {
            snprintf(req, sizeof(req),
                     "M-SEARCH * HTTP/1.1\r\n"
                     "HOST: %s:%d\r\n"
                     "MAN: \"ssdp:discover\"\r\n"
                     "MX: 2\r\n"
                     "ST: %s\r\n"
                     "\r\n",
                     SSDP_ADDR, SSDP_PORT, STS[s]);
            sendto(sock, req, strlen(req), 0,
                   (struct sockaddr *)&maddr, sizeof(maddr));
            if (gw != 0) {
                struct sockaddr_in uaddr = {0};
                uaddr.sin_family = AF_INET;
                uaddr.sin_port = htons(SSDP_PORT);
                uaddr.sin_addr.s_addr = gw;
                snprintf(req, sizeof(req),
                         "M-SEARCH * HTTP/1.1\r\n"
                         "HOST: %u.%u.%u.%u:%d\r\n"
                         "MAN: \"ssdp:discover\"\r\n"
                         "ST: %s\r\n"
                         "\r\n",
                         (unsigned)(gw & 0xff), (unsigned)((gw >> 8) & 0xff),
                         (unsigned)((gw >> 16) & 0xff), (unsigned)((gw >> 24) & 0xff),
                         SSDP_PORT, STS[s]);
                sendto(sock, req, strlen(req), 0,
                       (struct sockaddr *)&uaddr, sizeof(uaddr));
            }
        }
        // Collect replies for a beat; the first LOCATION wins.
        for (;;) {
            char resp[2048];
            ssize_t n = recv(sock, resp, sizeof(resp) - 1, 0);
            if (n <= 0) break;
            resp[n] = '\0';
            char *loc = header_value(resp, "location");
            if (loc) {
                snprintf(location, loclen, "%s", loc);
                free(loc);
                close(sock);
                return 0;
            }
        }
    }
    close(sock);
    return -1;
}

// --- device description ------------------------------------------------------

// Finds "<controlURL>value</controlURL>" after the service block for `stype`.
static char *find_control_url(const char *xml, const char *stype) {
    const char *svc = strstr(xml, stype);
    if (!svc) return NULL;
    const char *ctl = strstr(svc, "<controlURL>");
    if (!ctl) return NULL;
    ctl += strlen("<controlURL>");
    const char *end = strstr(ctl, "</controlURL>");
    if (!end) return NULL;
    size_t n = (size_t)(end - ctl);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, ctl, n);
    out[n] = '\0';
    return out;
}

static char *base_url(const char *xml) {
    const char *b = strstr(xml, "<URLBase>");
    if (!b) return NULL;
    b += strlen("<URLBase>");
    const char *end = strstr(b, "</URLBase>");
    if (!end) return NULL;
    size_t n = (size_t)(end - b);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, b, n);
    out[n] = '\0';
    return out;
}

// Aborts curl transfers immediately when upnp_stop() sets s_stop.
static int upnp_curl_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                                 curl_off_t ultotal, curl_off_t ulnow) {
    (void)clientp; (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return s_stop ? 1 : 0;
}

// GETs `url`, returns malloc'd body ("" on failure).
static char *http_get(const char *url) {
    membuf m = {0};
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)HTTP_TIMEOUT_SECS);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, upnp_curl_progress_cb);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || !m.data) {
        free(m.data);
        return NULL;
    }
    return m.data;
}

// SOAP AddPortMapping POST; returns 1 on success.
static int soap_add_mapping(const char *control_url, const char *svc_type,
                            const char *local_ip, int internal_port,
                            const char *proto) {
    char body[1024];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\"?>"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             "<s:Body><u:AddPortMapping xmlns:u=\"%s\">"
             "<NewRemoteHost></NewRemoteHost>"
             "<NewExternalPort>%d</NewExternalPort>"
             "<NewProtocol>%s</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort>"
             "<NewInternalClient>%s</NewInternalClient>"
             "<NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>TorrentShopNX</NewPortMappingDescription>"
             "<NewLeaseDuration>0</NewLeaseDuration>"
             "</u:AddPortMapping></s:Body></s:Envelope>",
             svc_type, internal_port, proto, internal_port, local_ip);

    membuf m = {0};
    CURL *c = curl_easy_init();
    if (!c) return 0;

    char soapaction[192];
    snprintf(soapaction, sizeof(soapaction), "\"%s#AddPortMapping\"", svc_type);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: text/xml; charset=\"utf-8\"");
    hdrs = curl_slist_append(hdrs, soapaction);
    hdrs = curl_slist_append(hdrs, "User-Agent: TorrentShopNX/2.3 UPnP/1.1");

    curl_easy_setopt(c, CURLOPT_URL, control_url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)HTTP_TIMEOUT_SECS);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, upnp_curl_progress_cb);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    int ok = (rc == CURLE_OK && code == 200);
    free(m.data);
    return ok;
}

// Our LAN IP, as seen by the IGD: connect a UDP socket towards the IGD host
// (nothing is sent; the route lookup gives us the interface address).
static int local_ip_for(const char *location, char *out, size_t outlen) {
    // location = "http://host:port/..."  -> host
    const char *h = strstr(location, "://");
    if (!h) return -1;
    h += 3;
    const char *end = h;
    while (*end && *end != ':' && *end != '/') end++;
    if (end == h) return -1;
    char host[128];
    size_t n = (size_t)(end - h);
    if (n >= sizeof(host)) return -1;
    memcpy(host, h, n);
    host[n] = '\0';

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(SSDP_PORT);
    sa.sin_addr.s_addr = inet_addr(host);
    if (sa.sin_addr.s_addr == INADDR_NONE || sa.sin_addr.s_addr == 0) {
        close(sock);
        return -1;
    }
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(sock);
        return -1;
    }
    struct sockaddr_in me;
    socklen_t mel = sizeof(me);
    if (getsockname(sock, (struct sockaddr *)&me, &mel) != 0) {
        close(sock);
        return -1;
    }
    close(sock);
    snprintf(out, outlen, "%u.%u.%u.%u",
             (unsigned)((me.sin_addr.s_addr >> 0) & 0xff),
             (unsigned)((me.sin_addr.s_addr >> 8) & 0xff),
             (unsigned)((me.sin_addr.s_addr >> 16) & 0xff),
             (unsigned)((me.sin_addr.s_addr >> 24) & 0xff));
    return 0;
}

// --- probe thread -------------------------------------------------------------

// UPnP IGD: discover, fetch the description, map TCP and UDP. Returns 1 when
// the TCP mapping is in place.
static int igd_map(int port, uint32_t gw) {
    engine_log(ENGINE_LOG_INFO, "[upnp] probing for IGD (port %d)...", port);

    char location[512] = {0};
    // Some routers answer M-SEARCH slowly or only after the network stack
    // settles (console just woke up / Wi-Fi re-associated). Retry the
    // discovery a few times with a 10 s backoff; the whole probe still
    // terminates silently and non-fatally when there is no IGD at all.
    int found = 0;
    for (int attempt = 0; attempt < 3 && !s_stop; attempt++) {
        if (ssdp_discover(location, sizeof(location), gw) == 0) { found = 1; break; }
        if (attempt < 2) {
            for (int w = 0; w < 100 && !s_stop; w++) {
                svcSleepThread(100000000ULL); // 100 ms slices
            }
        }
    }
    if (!found) {
        engine_log(ENGINE_LOG_INFO, "[upnp] no IGD found via SSDP (multicast + gateway unicast)");
        return 0;
    }
    engine_log(ENGINE_LOG_INFO, "[upnp] IGD location: %s", location);
    if (s_stop) return 0;

    char *desc = http_get(location);
    if (!desc) {
        engine_log(ENGINE_LOG_WARN, "[upnp] failed to fetch the device description");
        return 0;
    }

    static const char *WANIP   = "urn:schemas-upnp-org:service:WANIPConnection:1";
    static const char *WANPPP  = "urn:schemas-upnp-org:service:WANPPPConnection:1";
    char *ctl = find_control_url(desc, WANIP);
    const char *svc = WANIP;
    if (!ctl) {
        ctl = find_control_url(desc, WANPPP);
        svc = WANPPP;
    }
    if (!ctl) {
        free(desc);
        engine_log(ENGINE_LOG_WARN, "[upnp] no WANIP/WANPPP service in the description");
        return 0;
    }

    // controlURL may be relative: resolve against URLBase (or the location).
    char curl_full[768] = {0};
    if (has_scheme(ctl)) {
        snprintf(curl_full, sizeof(curl_full), "%s", ctl);
    } else {
        char *base = base_url(desc);
        const char *root = base ? base : location;
        // root = http://host[:port][/path]; keep scheme+host, drop the path.
        const char *scheme = root;
        const char *host = strstr(scheme, "://");
        if (!host) { free(desc); free(ctl); free(base); return 0; }
        host += 3;
        const char *slash = strchr(host, '/');
        size_t n = slash ? (size_t)(slash - scheme) : strlen(scheme);
        if (n >= sizeof(curl_full)) n = sizeof(curl_full) - 1;
        memcpy(curl_full, scheme, n);
        curl_full[n] = '\0';
        // control URL starts with '/' after the host part
        snprintf(curl_full + n, sizeof(curl_full) - n, "%s%s",
                 ctl[0] == '/' ? "" : "/", ctl);
        free(base);
    }
    free(desc);
    free(ctl);

    char local_ip[64];
    if (local_ip_for(location, local_ip, sizeof(local_ip)) != 0) {
        engine_log(ENGINE_LOG_WARN, "[upnp] could not determine the local IP");
        return 0;
    }
    if (s_stop) return 0;

    engine_log(ENGINE_LOG_INFO, "[upnp] AddPortMapping %s:%d TCP+UDP -> %s:%d",
               local_ip, port, local_ip, port);
    if (!soap_add_mapping(curl_full, svc, local_ip, port, "TCP")) {
        engine_log(ENGINE_LOG_WARN, "[upnp] AddPortMapping failed");
        return 0;
    }
    // uTP runs on the same port number over UDP. A router that refuses the
    // second mapping still leaves TCP working.
    int udp_ok = s_stop ? 0 : soap_add_mapping(curl_full, svc, local_ip, port, "UDP");
    torrent_set_announce_port(port);
    engine_log(ENGINE_LOG_INFO,
               "[upnp] port %d mapped (tcp=1 udp=%d): peers can dial in now", port, udp_ok);
    return 1;
}

// Sleep `secs` in 200 ms slices; false when the thread was told to stop.
static bool sleep_unless_stopped(uint32_t secs) {
    for (uint64_t i = 0; i < (uint64_t)secs * 5 && !s_stop; i++) {
        svcSleepThread(200000000ULL);
        if ((i % 25) == 0) tsnx_engine_wd_tick(3);
    }
    return !s_stop;
}

// NAT-PMP: map TCP and UDP, then stay around to renew the lease. Returns 0 if
// the gateway does not speak it (or refused); otherwise returns only when the
// engine stops, after deleting the mappings.
static int natpmp_run(int port, uint32_t gw) {
    int res = 0;
    uint32_t ext_ip = 0;
    int rc = natpmp_external_ip(gw, NATPMP_PORT, &ext_ip, &s_stop, &res);
    if (rc == NATPMP_ERR_NO_REPLY || rc == NATPMP_ERR_SOCKET || rc == NATPMP_ERR_BAD_REPLY) {
        engine_log(ENGINE_LOG_INFO, "[natpmp] gateway %u.%u.%u.%u does not answer on %d",
                   (unsigned)(gw & 0xff), (unsigned)((gw >> 8) & 0xff),
                   (unsigned)((gw >> 16) & 0xff), (unsigned)((gw >> 24) & 0xff), NATPMP_PORT);
        return 0;
    }
    // A refused address request (the WAN link is down, say) does not rule out
    // a mapping. Result 1 is a PCP-only gateway, which we do not speak.
    if (rc == NATPMP_ERR_REFUSED && res == 1) {
        engine_log(ENGINE_LOG_INFO, "[natpmp] gateway speaks PCP only (unsupported version)");
        return 0;
    }
    int ext_tcp = 0, ext_udp = 0;
    uint32_t life = 0;
    rc = natpmp_map(gw, NATPMP_PORT, false, port, port, NATPMP_LIFETIME_SECS,
                    &ext_tcp, &life, &s_stop, &res);
    if (rc != NATPMP_OK) {
        engine_log(ENGINE_LOG_WARN, "[natpmp] TCP mapping failed rc=%d result=%d", rc, res);
        return 0;
    }
    uint32_t life_udp = 0;
    int udp_ok = natpmp_map(gw, NATPMP_PORT, true, port, ext_tcp, NATPMP_LIFETIME_SECS,
                            &ext_udp, &life_udp, &s_stop, &res) == NATPMP_OK;
    torrent_set_announce_port(ext_tcp);
    s_map_kind = MAP_NATPMP;
    engine_log(ENGINE_LOG_INFO,
               "[natpmp] port %d mapped: external tcp=%d udp=%d ext_ip=%u.%u.%u.%u lease=%us",
               port, ext_tcp, udp_ok ? ext_udp : 0,
               (unsigned)(ext_ip & 0xff), (unsigned)((ext_ip >> 8) & 0xff),
               (unsigned)((ext_ip >> 16) & 0xff), (unsigned)((ext_ip >> 24) & 0xff),
               (unsigned)life);
    engine_log(ENGINE_LOG_INFO, "[net] port=%d mapped=natpmp", port);

    // Renew at half the granted lease (never more often than once a minute).
    while (!s_stop) {
        uint32_t wait = life / 2;
        if (wait < 60) wait = 60;
        if (!sleep_unless_stopped(wait)) break;
        uint32_t l2 = 0;
        int e2 = 0;
        if (natpmp_map(gw, NATPMP_PORT, false, port, ext_tcp, NATPMP_LIFETIME_SECS,
                       &e2, &l2, &s_stop, &res) == NATPMP_OK) {
            if (l2 > 0) life = l2;
            if (e2 != ext_tcp) {
                ext_tcp = e2;
                torrent_set_announce_port(ext_tcp);
                engine_log(ENGINE_LOG_INFO, "[natpmp] external port changed to %d", ext_tcp);
            }
        } else if (!s_stop) {
            engine_log(ENGINE_LOG_WARN, "[natpmp] renewal failed result=%d", res);
        }
        if (udp_ok && !s_stop)
            natpmp_map(gw, NATPMP_PORT, true, port, ext_udp, NATPMP_LIFETIME_SECS,
                       &ext_udp, &life_udp, &s_stop, &res);
    }
    // Engine stop: give the mappings back, best effort. The stop flag is not
    // passed on (it is set already and would skip the request).
    natpmp_map(gw, NATPMP_PORT, false, port, 0, 0, NULL, NULL, NULL, NULL);
    if (udp_ok) natpmp_map(gw, NATPMP_PORT, true, port, 0, 0, NULL, NULL, NULL, NULL);
    return 1;
}

static void upnp_main(void *arg) {
    (void)arg;
    tsnx_engine_wd_tick(3);
    int port = s_internal_port;
    uint32_t local = 0, gw = 0;
    lan_addrs(&local, &gw);

    if (igd_map(port, gw)) {
        s_map_kind = MAP_UPNP;
        engine_log(ENGINE_LOG_INFO, "[net] port=%d mapped=upnp", port);
        return;
    }
    if (!s_stop && gw != 0 && natpmp_run(port, gw)) return;
    if (s_stop) return;
    s_map_kind = MAP_NONE;
    engine_log(ENGINE_LOG_INFO,
               "[net] port=%d mapped=none: no UPnP IGD and no NAT-PMP on the router. "
               "Peers behind NAT cannot dial in; forward TCP+UDP %d to this device "
               "on the router for more peers.", port, port);
}

int upnp_start(int internal_port) {
    if (s_started || internal_port <= 0) return 0;
    s_internal_port = internal_port;
    s_stop = false;
    s_map_kind = MAP_PROBING;
    if (threadCreate(&s_thread, upnp_main, NULL, NULL, 0x20000,
                     TSNX_THREAD_ARGS(TSNX_CORE_NET, TSNX_PRIO_NET_BG)) != 0) {
        s_stop = true;
        return -1;
    }
    threadStart(&s_thread);
    s_started = true;
    return 0;
}

void upnp_stop(void) {
    if (!s_started) return;
    s_stop = true;
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
    s_started = false;
}
