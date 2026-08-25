/*
 * DHT smoke test for the PC harness.
 *
 *   dhttest find <hex> <budget_ms>   - one-shot lookup (the metadata path)
 *   dhttest bg   <hex> <budget_ms>   - persistent background DHT
 *
 * Logs the routing-table growth and the peers found, so DHT health can be
 * checked against a known-popular infohash without the Switch.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "dhtclient.h"
#include "dht.h"
#include "engine_log.h"

static volatile bool g_stop = false;

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static int decode_hex(const char *hex, uint8_t out[20]) {
    if (!hex || strlen(hex) < 40) return -1;
    for (int i = 0; i < 20; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

static void print_peer(const peer_addr *p) {
    const uint8_t *ip = (const uint8_t *)&p->ip;
    printf("      peer %d.%d.%d.%d:%d\n", ip[0], ip[1], ip[2], ip[3], p->port);
}

static void on_peers(void *ctx, const peer_addr *peers, int n) {
    int *total = (int *)ctx;
    for (int i = 0; i < n; i++) print_peer(&peers[i]);
    *total += n;
}

static void dlog_cb(const char *msg) {
    printf("  [dht] %s\n", msg);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "find";
    const char *hex = argc > 2 ? argv[2] : "EDC9BE013621189954A1E7D4EA514A804739EBB1";
    int budget = argc > 3 ? atoi(argv[3]) : 90000;

    engine_log_init(NULL);
    dht_set_log(dlog_cb);
    dht_set_cache_path("./dht_cache_test.bin");

    uint8_t ih[20];
    if (decode_hex(hex, ih) != 0) {
        printf("bad hex infohash\n");
        return 2;
    }

    if (strcmp(mode, "find") == 0) {
        printf("one-shot dht_find_peers for %s, budget=%d ms\n", hex, budget);
        int peers_found = 0;
        char err[256] = {0};
        uint64_t t0 = now_ms();
        int n = dht_find_peers(ih, 80, budget, on_peers, &peers_found,
                               NULL, err, sizeof(err));
        printf("done in %llu ms: %d peers delivered (cb saw %d) err=%s\n",
               (unsigned long long)(now_ms() - t0), n, peers_found,
               err[0] ? err : "none");
        int good = 0, dubious = 0;
        dhtclient_get_nodes(&good, &dubious);
        printf("final nodes: good=%d dubious=%d\n", good, dubious);
    } else if (strcmp(mode, "bg") == 0) {
        printf("background DHT for %s, budget=%d ms\n", hex, budget);
        int peers_found = 0;
        dht_background_add(ih, on_peers, &peers_found);

        uint64_t t0 = now_ms();
        int last_good = -1, last_dubious = -1;
        while (now_ms() - t0 < (uint64_t)budget) {
            sleep_ms(2000);
            int good = 0, dubious = 0;
            dhtclient_get_nodes(&good, &dubious);
            if (good != last_good || dubious != last_dubious) {
                printf("  t=%llus nodes good=%d dubious=%d peers=%d\n",
                       (unsigned long long)(now_ms() - t0) / 1000,
                       good, dubious, peers_found);
                last_good = good;
                last_dubious = dubious;
            }
        }
        dht_background_remove(ih);
        printf("--- routing table dump (first 40 buckets) ---\n");
        fflush(stdout);
        dht_dump_tables(stdout);
        dht_stop();
        int good = 0, dubious = 0;
        dhtclient_get_nodes(&good, &dubious);
        printf("done: nodes good=%d dubious=%d peers=%d\n",
               good, dubious, peers_found);
        printf("cache file written: %s\n",
               fopen("./dht_cache_test.bin", "rb") ? "yes" : "no");
    } else {
        printf("usage: dhttest find|bg <hex> <budget_ms>\n");
        return 2;
    }
    return 0;
}
