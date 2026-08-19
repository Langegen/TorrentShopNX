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
    if (n > 512 * 1024) return 0;   // descriptions are a few KB at most
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

// --- SSDP --------------------------------------------------------------------

static int ssdp_discover(char *location, size_t loclen) {
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

// GETs `url`, returns malloc'd body ("" on failure).
static char *http_get(const char *url) {
    membuf m = {0};
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)HTTP_TIMEOUT_SECS);
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
                            const char *local_ip, int internal_port) {
    char body[1024];
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\"?>"
             "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
             "<s:Body><u:AddPortMapping xmlns:u=\"%s\">"
             "<NewRemoteHost></NewRemoteHost>"
             "<NewExternalPort>%d</NewExternalPort>"
             "<NewProtocol>TCP</NewProtocol>"
             "<NewInternalPort>%d</NewInternalPort>"
             "<NewInternalClient>%s</NewInternalClient>"
             "<NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>TorrentShopNX</NewPortMappingDescription>"
             "<NewLeaseDuration>0</NewLeaseDuration>"
             "</u:AddPortMapping></s:Body></s:Envelope>",
             svc_type, internal_port, internal_port, local_ip);

    membuf m = {0};
    CURL *c = curl_easy_init();
    if (!c) return 0;

    char soapaction[192];
    snprintf(soapaction, sizeof(soapaction), "\"%s#AddPortMapping\"", svc_type);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: text/xml; charset=\"utf-8\"");
    hdrs = curl_slist_append(hdrs, soapaction);
    hdrs = curl_slist_append(hdrs, "User-Agent: TorrentShopNX/2.2 UPnP/1.1");

    curl_easy_setopt(c, CURLOPT_URL, control_url);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)HTTP_TIMEOUT_SECS);
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
             (me.sin_addr.s_addr >> 0) & 0xff, (me.sin_addr.s_addr >> 8) & 0xff,
             (me.sin_addr.s_addr >> 16) & 0xff, (me.sin_addr.s_addr >> 24) & 0xff);
    return 0;
}

// --- probe thread -------------------------------------------------------------

static void upnp_main(void *arg) {
    (void)arg;
    tsnx_engine_wd_tick(3);
    int port = s_internal_port;
    engine_log(ENGINE_LOG_INFO, "[upnp] probing for IGD (port %d)...", port);

    char location[512] = {0};
    // Some routers answer M-SEARCH slowly or only after the network stack
    // settles (console just woke up / Wi-Fi re-associated). Retry the
    // discovery a few times with a 10 s backoff; the whole probe still
    // terminates silently and non-fatally when there is no IGD at all.
    int found = 0;
    for (int attempt = 0; attempt < 3 && !s_stop; attempt++) {
        if (ssdp_discover(location, sizeof(location)) == 0) { found = 1; break; }
        if (attempt < 2) svcSleepThread(10000000000ULL);
    }
    if (!found) {
        engine_log(ENGINE_LOG_INFO, "[upnp] no IGD found via SSDP");
        return;
    }
    engine_log(ENGINE_LOG_INFO, "[upnp] IGD location: %s", location);
    if (s_stop) return;

    char *desc = http_get(location);
    if (!desc) {
        engine_log(ENGINE_LOG_WARN, "[upnp] failed to fetch the device description");
        return;
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
        return;
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
        if (!host) { free(desc); free(ctl); free(base); return; }
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
        return;
    }
    if (s_stop) return;

    engine_log(ENGINE_LOG_INFO, "[upnp] AddPortMapping %s:%d TCP -> %s:%d",
               local_ip, port, local_ip, port);
    if (soap_add_mapping(curl_full, svc, local_ip, port)) {
        torrent_set_announce_port(port);
        engine_log(ENGINE_LOG_INFO,
                   "[upnp] port %d mapped: peers can dial in now", port);
    } else {
        engine_log(ENGINE_LOG_WARN, "[upnp] AddPortMapping failed");
    }
}

int upnp_start(int internal_port) {
    if (s_started || internal_port <= 0) return 0;
    s_internal_port = internal_port;
    s_stop = false;
    if (threadCreate(&s_thread, upnp_main, NULL, NULL, 0x20000, 0x2C, -2) != 0) {
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
