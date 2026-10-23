// Multi-seeder scheduler test: three local seeders of different quality serve
// one torrent while the reader streams it with strict SHA-1 verification and a
// scheduler thread keeps moving the piece zones with the read position, the
// way CustomEngineScheduler does in the app. Exercises the request scheduler
// paths a single fast seeder never reaches: duplicate (hedged) requests, CANCEL
// of copies in flight, stall handling and zone-driven candidate rebuilds.
//
// Seeders listen on 127.0.0.1 / .2 / .3 (the peer pool dedups by IP):
//   fast   - no delay
//   medium - ~1 ms per block
//   flaky  - every 40th block is held back for 3 s
//
//   pctest\test_swarm_sched.exe [--size MB] [--piece MB] [--nozones | --appsched]
//
//   default     a fixed 24-piece zone window re-applied as the reader moves
//   --nozones   no zones: the engine's own picker
//   --appsched  the app's CustomEngineScheduler (installer profile), driven
//               like CustomEngineBackend: on_read_request per read, on_tick 1/s
//
// Passes when every byte matches and the whole file is read.

#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#define SOCKET int
#define INVALID_SOCKET -1
#endif

#include <mbedtls/sha1.h>
#include "engine.h"
#include "../source/datasource/custom_engine_scheduler.h"

std::atomic<bool> g_appExiting{false};   // extern in utils/log.h

using namespace std;
using namespace std::chrono;

static const uint32_t BLOCK = 16384;

struct Payload {
    vector<uint8_t> data;
    uint32_t piece = 0, pieces = 0;
    uint8_t info_hash[20] = {0};
};

struct Seeder {
    const char *name;
    uint32_t ip_host;      // host order, 127.0.0.x
    int delay_us;          // per block
    int stall_every;       // 0 = never; else hold every Nth block
    int stall_ms;
    SOCKET srv = INVALID_SOCKET;
    uint16_t port = 0;
    atomic<bool> stop{false};
    atomic<uint64_t> blocks{0}, cancels{0}, requests{0};
};

static bool recv_all(SOCKET s, uint8_t *p, size_t n, atomic<bool> &stop) {
    size_t got = 0;
    while (got < n && !stop) {
        int r = recv(s, (char *)p + got, (int)(n - got), 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return got == n;
}

static bool send_all(SOCKET s, const uint8_t *p, size_t n, atomic<bool> &stop) {
    size_t sent = 0;
    while (sent < n && !stop) {
        int r = send(s, (const char *)p + sent, (int)(n - sent), 0);
        if (r <= 0) return false;
        sent += (size_t)r;
    }
    return sent == n;
}

static void serve(SOCKET c, Seeder *sd, const Payload *pl) {
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    uint8_t hs[68];
    if (!recv_all(c, hs, 68, sd->stop) || memcmp(hs + 28, pl->info_hash, 20) != 0) {
        closesocket(c);
        return;
    }
    uint8_t rep[68];
    rep[0] = 19;
    memcpy(rep + 1, "BitTorrent protocol", 19);
    memset(rep + 20, 0, 8);
    memcpy(rep + 28, pl->info_hash, 20);
    memcpy(rep + 48, "-TS0001-swarmtest000", 20);
    send_all(c, rep, 68, sd->stop);

    uint32_t bfb = (pl->pieces + 7) / 8;
    vector<uint8_t> bf(5 + bfb, 0xFF);
    uint32_t l = htonl(1 + bfb);
    memcpy(bf.data(), &l, 4);
    bf[4] = 5;
    if (pl->pieces % 8) bf[5 + bfb - 1] = (uint8_t)(0xFF << (8 - pl->pieces % 8));
    send_all(c, bf.data(), bf.size(), sd->stop);
    uint8_t unchoke[5] = {0, 0, 0, 1, 1};
    send_all(c, unchoke, 5, sd->stop);

    vector<uint8_t> msg, resp(13 + BLOCK);
    uint64_t served = 0;
    while (!sd->stop) {
        uint32_t ln;
        if (!recv_all(c, (uint8_t *)&ln, 4, sd->stop)) break;
        ln = ntohl(ln);
        if (ln == 0) continue;
        if (ln > 1 << 20) break;
        msg.resize(ln);
        if (!recv_all(c, msg.data(), ln, sd->stop)) break;
        if (msg[0] == 8) { sd->cancels++; continue; }      // CANCEL
        if (msg[0] != 6 || ln != 13) continue;              // REQUEST only
        sd->requests++;
        uint32_t idx, off, len;
        memcpy(&idx, &msg[1], 4); memcpy(&off, &msg[5], 4); memcpy(&len, &msg[9], 4);
        idx = ntohl(idx); off = ntohl(off); len = ntohl(len);
        size_t at = (size_t)idx * pl->piece + off;
        if (idx >= pl->pieces || len > BLOCK || at + len > pl->data.size()) continue;
        served++;
        if (sd->stall_every && served % sd->stall_every == 0)
            this_thread::sleep_for(milliseconds(sd->stall_ms));
        else if (sd->delay_us)
            this_thread::sleep_for(microseconds(sd->delay_us));
        uint32_t rl = htonl(9 + len), ni = htonl(idx), no = htonl(off);
        memcpy(&resp[0], &rl, 4);
        resp[4] = 7;
        memcpy(&resp[5], &ni, 4);
        memcpy(&resp[9], &no, 4);
        memcpy(&resp[13], &pl->data[at], len);
        if (!send_all(c, resp.data(), 13 + len, sd->stop)) break;
        sd->blocks++;
    }
    closesocket(c);
}

static void accept_loop(Seeder *sd, const Payload *pl) {
    while (!sd->stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sd->srv, &fds);
        struct timeval tv = {0, 100000};
        if (select((int)sd->srv + 1, &fds, NULL, NULL, &tv) > 0) {
            SOCKET c = accept(sd->srv, NULL, NULL);
            if (c != INVALID_SOCKET) thread(serve, c, sd, pl).detach();
        }
    }
}

static bool start_seeder(Seeder &sd) {
    sd.srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(sd.ip_host);
    a.sin_port = 0;
    if (bind(sd.srv, (struct sockaddr *)&a, sizeof(a)) != 0) return false;
    listen(sd.srv, 16);
    socklen_t al = sizeof(a);
    getsockname(sd.srv, (struct sockaddr *)&a, &al);
    sd.port = ntohs(a.sin_port);
    return true;
}

static bool write_torrent(const char *path, Payload &pl) {
    vector<uint8_t> hashes(pl.pieces * 20);
    for (uint32_t i = 0; i < pl.pieces; i++)
        mbedtls_sha1(&pl.data[(size_t)i * pl.piece], pl.piece, &hashes[i * 20]);
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
        "d6:lengthi%llde4:name14:swarm_test.bin12:piece lengthi%ue6:pieces%u:",
        (long long)pl.data.size(), pl.piece, pl.pieces * 20);
    vector<uint8_t> info(hdr, hdr + hl);
    info.insert(info.end(), hashes.begin(), hashes.end());
    info.push_back('e');
    mbedtls_sha1(info.data(), info.size(), pl.info_hash);
    const char *root = "d8:announce30:http://127.0.0.1:9/announce/xx4:info";
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(root, 1, strlen(root), f);
    fwrite(info.data(), 1, info.size(), f);
    fputc('e', f);
    fclose(f);
    return true;
}

int main(int argc, char **argv) {
    uint32_t size_mb = 512, piece_mb = 2;
    bool zones_on = true;
    bool app_sched = false;
    bool fast_only = false;
    bool loose = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) size_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--piece") && i + 1 < argc) piece_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nozones")) zones_on = false;   // engine's own picker
        else if (!strcmp(argv[i], "--appsched")) { app_sched = true; zones_on = false; }
        else if (!strcmp(argv[i], "--fastonly")) fast_only = true;   // just the fast seeder
        else if (!strcmp(argv[i], "--loose")) loose = true;          // no strict SHA-1 reads
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Payload pl;
    pl.piece = piece_mb << 20;
    pl.data.resize((size_t)size_mb << 20);
    pl.pieces = (uint32_t)(pl.data.size() / pl.piece);
    uint32_t x = 0x12345678;
    for (size_t i = 0; i < pl.data.size(); i++) { x = x * 1103515245u + 12345u; pl.data[i] = (uint8_t)(x >> 24); }
    const char *tpath = "pctest/swarm_sched_test.torrent";
    if (!write_torrent(tpath, pl)) { cerr << "cannot write torrent" << endl; return 1; }

    Seeder seeders[3] = {
        {"fast",   0x7F000001, 0,    0,  0},
        {"medium", 0x7F000002, 1000, 0,  0},
        {"flaky",  0x7F000003, 0,    40, 3000},
    };
    vector<thread> acceptors;
    for (auto &sd : seeders) {
        if (!start_seeder(sd)) { cerr << "bind failed for " << sd.name << endl; return 1; }
        acceptors.emplace_back(accept_loop, &sd, &pl);
    }

    tsnx_engine *eng = tsnx_engine_start(0);
    char hash[41] = {0};
    if (!eng || !tsnx_engine_add_torrent_file(eng, tpath, hash, sizeof(hash)) ||
        !tsnx_engine_prepare_stream(eng, hash, 0)) {
        cerr << "engine setup failed" << endl;
        return 1;
    }
    tsnx_engine_set_strict_verify(eng, hash, loose ? 0 : 1);   // strict: as the app does
    for (auto &sd : seeders) {
        if (fast_only && &sd != &seeders[0]) continue;
        uint32_t ip = htonl(sd.ip_host);
        uint16_t port = sd.port;
        tsnx_engine_add_peers(eng, hash, &ip, &port, 1);
    }

    // Scheduler: re-zone around the read position, like CustomEngineScheduler
    // (clear, then Critical / Urgent / Prefetch / Speculative / Normal ranges).
    atomic<int64_t> read_off{0};
    atomic<bool> stop{false};
    const int pieces = (int)pl.pieces;
    thread sched([&]() {
        int last = -1;
        while (!stop && zones_on) {
            int cur = (int)(read_off.load() / pl.piece);
            if (cur != last) {
                last = cur;
                tsnx_engine_clear_piece_zones(eng, hash);
                struct { int n; tsnx_piece_zone z; } zones[] = {
                    {1, TSNX_ZONE_CRITICAL}, {2, TSNX_ZONE_URGENT}, {8, TSNX_ZONE_PREFETCH},
                    {6, TSNX_ZONE_SPECULATIVE}, {7, TSNX_ZONE_TAIL}};
                int at = cur;
                for (auto &zz : zones) {
                    int n = zz.n;
                    if (at >= pieces) break;
                    if (at + n > pieces) n = pieces - at;
                    tsnx_engine_set_piece_zone(eng, hash, at, n, zz.z);
                    at += n;
                }
            }
            this_thread::sleep_for(milliseconds(20));
        }
    });

    datasource::CustomEngineScheduler app;
    thread app_tick;
    if (app_sched) {
        app.setConfig(datasource::CustomEngineScheduler::defaultInstallerConfig((int)pl.piece));
        app.init(eng, hash, (int)pl.piece, 0, 0, pieces - 1);
        app_tick = thread([&]() {
            while (!stop) {
                this_thread::sleep_for(seconds(1));
                app.on_tick();
            }
        });
    }

    vector<uint8_t> buf(4 << 20);
    int64_t off = 0, total = (int64_t)pl.data.size();
    uint64_t mismatches = 0;
    int empty = 0;
    auto t0 = steady_clock::now();
    while (off < total) {
        int64_t want = min<int64_t>((int64_t)buf.size(), total - off);
        if (app_sched) app.on_read_request(off, want);
        int64_t n = tsnx_engine_read(eng, hash, off, buf.data(), want);
        if (n <= 0) {
            if (++empty > 3) { cerr << "read failed at " << off << " (" << n << ")" << endl; break; }
            continue;
        }
        if (memcmp(buf.data(), &pl.data[(size_t)off], (size_t)n) != 0) mismatches++;
        off += n;
        read_off = off;
        tsnx_engine_set_min_keep_offset(eng, hash, off);
    }
    double secs = duration<double>(steady_clock::now() - t0).count();
    tsnx_engine_diag diag;
    bool have_diag = tsnx_engine_get_diag(eng, hash, &diag);
    stop = true;
    sched.join();
    if (app_tick.joinable()) app_tick.join();
    tsnx_engine_stop(eng);
    for (auto &sd : seeders) { sd.stop = true; closesocket(sd.srv); }
    for (auto &t : acceptors) t.join();
    remove(tpath);

    printf("\nswarm/scheduler test: %.1f MB in %.2f s (%.1f MB/s), %llu mismatches\n",
           off / 1048576.0, secs, off / 1048576.0 / secs, (unsigned long long)mismatches);
    for (auto &sd : seeders)
        printf("  %-6s requests=%llu blocks=%llu cancels=%llu\n", sd.name,
               (unsigned long long)sd.requests.load(), (unsigned long long)sd.blocks.load(),
               (unsigned long long)sd.cancels.load());
    if (have_diag && diag.bytes_recv > 0)
        printf("  duplicate payload: %.2f%%\n", 100.0 * (double)diag.dup_bytes / (double)diag.bytes_recv);
    bool ok = mismatches == 0 && off == total;
    printf("%s\n", ok ? "PASSED" : "FAILED");
#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
