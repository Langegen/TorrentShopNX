// End-to-end test of incoming uTP and ut_holepunch (BEP 55) on 127.0.0.1,
// three processes:
//
//   engine (this process)  downloads a 64 MB torrent, strict SHA-1 reads.
//   relay  (--relay)       dials IN to the engine over uTP (incoming uTP path),
//                          speaks ut_holepunch + ut_pex, claims to be a seed
//                          but never unchokes, and PEXes the seed's address
//                          flagged "uTP + holepunch, NOT reachable". When the
//                          engine asks it for a RENDEZVOUS with the seed, it
//                          opens the seed's "NAT" and answers CONNECT.
//   seed   (--seed)        serves the data over uTP only, and refuses every
//                          connection until the relay opened its "NAT" (a
//                          go-file) -- so a plain TCP / uTP dial cannot reach
//                          it, only the hole-punch path can.
//
// PASSED = every byte read and verified, the seed served it, and the engine
// counted >= 1 incoming uTP session, >= 1 rendezvous and >= 1 hole punch.
//
//   pctest\test_holepunch.exe [--size MB]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#include <mbedtls/sha1.h>
#include "engine.h"
#include "utp_nb.h"

using namespace std;
using namespace std::chrono;

static const uint32_t BLOCK = 16384;
static const char *GO_FILE   = "pctest/hp_test.go";
static const char *STOP_FILE = "pctest/hp_test.stop";
static const char *PORT_FILE = "pctest/hp_test.port";
static const char *TORRENT   = "pctest/hp_test.torrent";

struct Payload {
    vector<uint8_t> data;
    uint32_t piece = 1u << 20, pieces = 0;
    uint8_t info_hash[20] = {0};
};

static void make_payload(Payload &pl, uint32_t size_mb) {
    pl.data.resize((size_t)size_mb << 20);
    pl.pieces = (uint32_t)(pl.data.size() / pl.piece);
    uint32_t x = 0xC0FFEE11u;
    for (size_t i = 0; i < pl.data.size(); i++) { x = x * 1103515245u + 12345u; pl.data[i] = (uint8_t)(x >> 24); }
    vector<uint8_t> hashes(pl.pieces * 20);
    for (uint32_t i = 0; i < pl.pieces; i++)
        mbedtls_sha1(&pl.data[(size_t)i * pl.piece], pl.piece, &hashes[i * 20]);
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
        "d6:lengthi%llde4:name11:hp_test.bin12:piece lengthi%ue6:pieces%u:",
        (long long)pl.data.size(), pl.piece, pl.pieces * 20);
    vector<uint8_t> info(hdr, hdr + hl);
    info.insert(info.end(), hashes.begin(), hashes.end());
    info.push_back('e');
    mbedtls_sha1(info.data(), info.size(), pl.info_hash);
    FILE *f = fopen(TORRENT, "wb");
    if (!f) return;
    const char *root = "d8:announce30:http://127.0.0.1:9/announce/xx4:info";
    fwrite(root, 1, strlen(root), f);
    fwrite(info.data(), 1, info.size(), f);
    fputc('e', f);
    fclose(f);
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void msleep(int ms) { this_thread::sleep_for(milliseconds(ms)); }

// --- a framed BitTorrent stream over one utp_nb session ----------------------

struct Conn {
    utp_nb_sess *u = nullptr;
    vector<uint8_t> rx, tx;
    bool hs_done = false;
    bool dead = false;

    void queue(const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        tx.insert(tx.end(), b, b + n);
    }
    void msg(uint8_t id, const void *pl, uint32_t n) {
        uint32_t l = htonl(1 + n);
        queue(&l, 4);
        queue(&id, 1);
        if (n) queue(pl, n);
    }
    void ext(uint8_t ext_id, const string &body) {
        vector<uint8_t> p(1 + body.size());
        p[0] = ext_id;
        memcpy(p.data() + 1, body.data(), body.size());
        msg(20, p.data(), (uint32_t)p.size());
    }
    void pump() {
        if (!u || dead) return;
        if (utp_nb_state(u) < 0) { dead = true; return; }
        uint8_t buf[65536];
        for (;;) {
            int n = utp_nb_read(u, buf, sizeof(buf));
            if (n < 0) { dead = true; return; }
            if (n == 0) break;
            rx.insert(rx.end(), buf, buf + n);
        }
        while (!tx.empty()) {
            int n = utp_nb_write(u, tx.data(), (int)tx.size());
            if (n < 0) { dead = true; return; }
            if (n == 0) break;
            tx.erase(tx.begin(), tx.begin() + n);
        }
    }
    // Next whole message: true with id/payload; keep-alives skipped.
    bool next(uint8_t &id, vector<uint8_t> &pl) {
        for (;;) {
            if (rx.size() < 4) return false;
            uint32_t l;
            memcpy(&l, rx.data(), 4);
            l = ntohl(l);
            if (l == 0) { rx.erase(rx.begin(), rx.begin() + 4); continue; }
            if (rx.size() < 4 + (size_t)l) return false;
            id = rx[4];
            pl.assign(rx.begin() + 5, rx.begin() + 4 + l);
            rx.erase(rx.begin(), rx.begin() + 4 + l);
            return true;
        }
    }
};

static void handshake(Conn &c, const uint8_t ih[20], const char *id20, bool ext) {
    uint8_t hs[68];
    hs[0] = 19;
    memcpy(hs + 1, "BitTorrent protocol", 19);
    memset(hs + 20, 0, 8);
    if (ext) hs[25] = 0x10;
    memcpy(hs + 28, ih, 20);
    memcpy(hs + 48, id20, 20);
    c.queue(hs, 68);
}

static void full_bitfield(Conn &c, uint32_t pieces) {
    vector<uint8_t> bf((pieces + 7) / 8, 0xFF);
    if (pieces % 8) bf.back() = (uint8_t)(0xFF << (8 - pieces % 8));
    c.msg(5, bf.data(), (uint32_t)bf.size());
}

// --- seed ---------------------------------------------------------------------

static int seed_main(uint32_t size_mb) {
    Payload pl;
    make_payload(pl, size_mb);
    if (utp_nb_init(0) != 0) { fprintf(stderr, "seed: utp init failed\n"); return 2; }
    FILE *f = fopen(PORT_FILE, "w");
    if (!f) return 2;
    fprintf(f, "%d\n", utp_nb_port());
    fclose(f);
    utp_nb_set_accept(0);   // "behind NAT" until the relay opens it

    vector<Conn *> conns;
    uint64_t served = 0;
    int refused_phase_end = 0;
    auto t0 = steady_clock::now();
    auto last_check = t0;
    bool go = false;
    while (duration<double>(steady_clock::now() - t0).count() < 180) {
        if (steady_clock::now() - last_check > milliseconds(20)) {
            last_check = steady_clock::now();
            if (file_exists(STOP_FILE)) break;
            if (!go && file_exists(GO_FILE)) {
                go = true;
                utp_nb_set_accept(1);
                refused_phase_end = (int)duration_cast<milliseconds>(steady_clock::now() - t0).count();
            }
        }
        utp_nb_service();
        uint8_t hs[68];
        uint32_t ip; uint16_t port;
        while (utp_nb_sess *u = utp_nb_accept(hs, &ip, &port)) {
            if (memcmp(hs + 28, pl.info_hash, 20) != 0) { utp_nb_close(u); continue; }
            Conn *c = new Conn;
            c->u = u;
            handshake(*c, pl.info_hash, "-TS0001-hpseed000000", false);
            full_bitfield(*c, pl.pieces);
            c->msg(1, NULL, 0);   // unchoke
            conns.push_back(c);
        }
        for (size_t i = 0; i < conns.size(); ) {
            Conn *c = conns[i];
            c->pump();
            uint8_t id;
            vector<uint8_t> m;
            while (!c->dead && c->tx.size() < (1u << 20) && c->next(id, m)) {
                if (id != 6 || m.size() != 12) continue;   // REQUEST only
                uint32_t idx, off, len;
                memcpy(&idx, &m[0], 4); memcpy(&off, &m[4], 4); memcpy(&len, &m[8], 4);
                idx = ntohl(idx); off = ntohl(off); len = ntohl(len);
                size_t at = (size_t)idx * pl.piece + off;
                if (idx >= pl.pieces || len > BLOCK || at + len > pl.data.size()) continue;
                vector<uint8_t> r(8 + len);
                uint32_t ni = htonl(idx), no = htonl(off);
                memcpy(&r[0], &ni, 4); memcpy(&r[4], &no, 4);
                memcpy(&r[8], &pl.data[at], len);
                c->msg(7, r.data(), (uint32_t)r.size());
                served++;
            }
            c->pump();
            if (c->dead) { utp_nb_close(c->u); delete c; conns.erase(conns.begin() + i); }
            else i++;
        }
        msleep(1);
    }
    printf("seed: served %llu blocks, refused connections for the first %d ms\n",
           (unsigned long long)served, refused_phase_end);
    for (Conn *c : conns) { utp_nb_close(c->u); delete c; }
    utp_nb_exit();
    return 0;
}

// --- relay --------------------------------------------------------------------

static int relay_main(int engine_port, int seed_port, uint32_t size_mb) {
    Payload pl;
    make_payload(pl, size_mb);
    if (utp_nb_init(0) != 0) return 2;
    const uint32_t lo = htonl(0x7F000001);
    Conn c;
    c.u = utp_nb_connect(lo, (uint16_t)engine_port);
    if (!c.u) { fprintf(stderr, "relay: connect failed\n"); return 2; }
    handshake(c, pl.info_hash, "-TS0001-hprelay00000", true);

    const uint8_t MY_HP = 7, MY_PEX = 9;          // ids we advertise
    const uint8_t THEIR_HP = 2, THEIR_PEX = 1;    // the engine's (EXT_ID_*)
    bool greeted = false, pexed = false, connect_sent = false;
    int rendezvous = 0;
    auto t0 = steady_clock::now();
    auto pex_at = t0;
    while (duration<double>(steady_clock::now() - t0).count() < 180) {
        if (file_exists(STOP_FILE)) break;
        utp_nb_service();
        c.pump();
        if (c.dead) { fprintf(stderr, "relay: session died\n"); break; }
        if (!c.hs_done && c.rx.size() >= 68) {
            c.rx.erase(c.rx.begin(), c.rx.begin() + 68);
            c.hs_done = true;
        }
        if (c.hs_done && !greeted) {
            greeted = true;
            full_bitfield(c, pl.pieces);    // a seed that never unchokes
            char e[96];
            snprintf(e, sizeof(e), "d1:md12:ut_holepunchi%de6:ut_pexi%dee1:pi%dee",
                     MY_HP, MY_PEX, utp_nb_port());
            c.ext(0, e);
            pex_at = steady_clock::now() + milliseconds(300);
        }
        if (greeted && !pexed && steady_clock::now() > pex_at) {
            pexed = true;
            // added: 127.0.0.1:<seed>, flags uTP|holepunch (0x04|0x08), not reachable.
            string body = "d5:added6:";
            uint8_t a[6] = {127, 0, 0, 1, (uint8_t)(seed_port >> 8), (uint8_t)seed_port};
            body.append((const char *)a, 6);
            body += "7:added.f1:";
            body.push_back((char)0x0C);
            body += "e";
            c.ext(THEIR_PEX, body);
        }
        uint8_t id;
        vector<uint8_t> m;
        while (c.hs_done && c.next(id, m)) {
            if (id != 20 || m.size() < 9 || m[0] != MY_HP) continue;
            // RENDEZVOUS for the seed?
            if (m[1] != 0 || m[2] != 0) continue;
            uint16_t port = (uint16_t)((m[7] << 8) | m[8]);
            if (port != seed_port) continue;
            rendezvous++;
            if (connect_sent) continue;
            connect_sent = true;
            FILE *g = fopen(GO_FILE, "w");          // the seed's "NAT" opens
            if (g) fclose(g);
            msleep(200);
            string cm;
            cm.push_back((char)1);                  // CONNECT
            cm.push_back((char)0);                  // IPv4
            cm.append((const char *)&m[3], 6);      // addr + port, as requested
            c.ext(THEIR_HP, cm);
        }
        msleep(2);
    }
    printf("relay: rendezvous received %d, connect sent %d\n", rendezvous, connect_sent ? 1 : 0);
    utp_nb_close(c.u);
    for (int i = 0; i < 20; i++) { utp_nb_service(); msleep(5); }
    utp_nb_exit();
    return 0;
}

// --- engine (main) ------------------------------------------------------------

#ifdef _WIN32
static intptr_t spawn(const char *self, const vector<string> &args) {
    vector<const char *> av;
    av.push_back(self);
    for (auto &a : args) av.push_back(a.c_str());
    av.push_back(nullptr);
    return _spawnv(_P_NOWAIT, self, av.data());
}
static void reap(intptr_t h) {
    if (h <= 0) return;
    if (WaitForSingleObject((HANDLE)h, 15000) != WAIT_OBJECT_0) TerminateProcess((HANDLE)h, 9);
    CloseHandle((HANDLE)h);
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    uint32_t size_mb = 64;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--size") && i + 1 < argc) size_mb = (uint32_t)atoi(argv[++i]);
    if (argc > 1 && !strcmp(argv[1], "--seed")) return seed_main((uint32_t)atoi(argv[2]));
    if (argc > 1 && !strcmp(argv[1], "--relay"))
        return relay_main(atoi(argv[2]), atoi(argv[3]), (uint32_t)atoi(argv[4]));

#ifndef _WIN32
    fprintf(stderr, "multi-process test: Windows only for now\n");
    return 0;
#else
    remove(GO_FILE); remove(STOP_FILE); remove(PORT_FILE);
    Payload pl;
    make_payload(pl, size_mb);

    tsnx_engine *eng = tsnx_engine_start(0);
    char hash[41] = {0};
    if (!eng || !tsnx_engine_add_torrent_file(eng, TORRENT, hash, sizeof(hash)) ||
        !tsnx_engine_prepare_stream(eng, hash, 0)) {
        fprintf(stderr, "engine setup failed\n");
        return 1;
    }
    tsnx_engine_set_strict_verify(eng, hash, 1);
    tsnx_nat_stats ns;
    tsnx_engine_get_nat_stats(eng, hash, &ns);
    printf("engine: listen tcp=%d utp=%d\n", ns.listen_port, ns.utp_port);
    if (ns.utp_port <= 0) { fprintf(stderr, "no uTP port\n"); return 1; }

    intptr_t seed = spawn(argv[0], {"--seed", to_string(size_mb)});
    int seed_port = 0;
    for (int i = 0; i < 300 && seed_port == 0; i++) {
        msleep(50);
        FILE *f = fopen(PORT_FILE, "r");
        if (f) { if (fscanf(f, "%d", &seed_port) != 1) seed_port = 0; fclose(f); }
    }
    if (seed_port <= 0) { fprintf(stderr, "seed did not start\n"); return 1; }
    intptr_t relay = spawn(argv[0], {"--relay", to_string(ns.utp_port), to_string(seed_port),
                                     to_string(size_mb)});

    vector<uint8_t> buf(4 << 20);
    int64_t off = 0, total = (int64_t)pl.data.size();
    uint64_t mismatches = 0;
    auto t0 = steady_clock::now();
    std::atomic<bool> done{false};
    // The read blocks until data arrives: a watchdog cancels it after 120 s.
    std::thread wd([&]() {
        while (!done && duration<double>(steady_clock::now() - t0).count() < 120) msleep(100);
        if (!done) tsnx_engine_cancel_read(eng, hash);
    });
    while (off < total) {
        int64_t want = min<int64_t>((int64_t)buf.size(), total - off);
        int64_t n = tsnx_engine_read(eng, hash, off, buf.data(), want);
        if (n <= 0) break;
        if (memcmp(buf.data(), &pl.data[(size_t)off], (size_t)n) != 0) mismatches++;
        off += n;
        tsnx_engine_set_min_keep_offset(eng, hash, off);
    }
    done = true;
    wd.join();
    double secs = duration<double>(steady_clock::now() - t0).count();
    tsnx_engine_get_nat_stats(eng, hash, &ns);
    tsnx_engine_stop(eng);
    FILE *s = fopen(STOP_FILE, "w");
    if (s) fclose(s);
    reap(relay);
    reap(seed);
    remove(GO_FILE); remove(STOP_FILE); remove(PORT_FILE); remove(TORRENT);

    printf("\nhole-punch test: %.1f MB in %.2f s, %llu mismatches\n",
           off / 1048576.0, secs, (unsigned long long)mismatches);
    printf("  incoming utp=%d tcp=%d  hp tries=%d ok=%d inbound=%d relayed=%d  live_utp=%d\n",
           ns.incoming_utp, ns.incoming_tcp, ns.hp_tries, ns.hp_ok, ns.hp_inbound,
           ns.hp_relayed, ns.live_utp);
    bool ok = off == total && mismatches == 0 && ns.incoming_utp >= 1 &&
              ns.hp_tries >= 1 && ns.hp_ok >= 1;
    printf("%s\n", ok ? "PASSED" : "FAILED");
    WSACleanup();
    return ok ? 0 : 1;
#endif
}
