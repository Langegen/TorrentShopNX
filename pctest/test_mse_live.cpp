// Live MSE check against a real client: libtorrent (the library inside
// qBittorrent) seeds a file with encryption FORCED -- plaintext connections
// are refused -- and the engine has to download it through MSE/RC4.
//
//   test_mse_live.exe --prepare <dir> [--size MB]   writes <dir>/mse_test.bin + .torrent
//   test_mse_live.exe --run <dir> <port> [--size MB] downloads from 127.0.0.1:<port>
//
// pctest/mse_seed.py runs the seeding side (python-libtorrent).
// PASSED = every byte read and verified from that one peer.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <mbedtls/sha1.h>
#include "engine.h"

using namespace std;
using namespace std::chrono;

static void make(vector<uint8_t> &data, uint32_t size_mb, uint32_t piece, string *torrent) {
    data.resize((size_t)size_mb << 20);
    uint32_t x = 0x5EED1234u;
    for (size_t i = 0; i < data.size(); i++) { x = x * 1103515245u + 12345u; data[i] = (uint8_t)(x >> 24); }
    if (!torrent) return;
    const uint32_t pieces = (uint32_t)(data.size() / piece);
    vector<uint8_t> hashes(pieces * 20);
    for (uint32_t i = 0; i < pieces; i++) mbedtls_sha1(&data[(size_t)i * piece], piece, &hashes[i * 20]);
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr), "d6:lengthi%llde4:name12:mse_test.bin12:piece lengthi%ue6:pieces%u:",
                      (long long)data.size(), piece, pieces * 20);
    string info(hdr, hl);
    info.append((const char *)hashes.data(), hashes.size());
    info += "e";
    *torrent = "d4:info" + info + "e";
}

int main(int argc, char **argv) {
    uint32_t size_mb = 32;
    const uint32_t piece = 1u << 20;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--size") && i + 1 < argc) size_mb = (uint32_t)atoi(argv[++i]);
    if (argc < 3) { fprintf(stderr, "usage: --prepare <dir> | --run <dir> <port>\n"); return 2; }
    const string dir = argv[2];
    const string tpath = dir + "/mse_test.torrent";

    if (!strcmp(argv[1], "--prepare")) {
        vector<uint8_t> data;
        string tor;
        make(data, size_mb, piece, &tor);
        FILE *f = fopen((dir + "/mse_test.bin").c_str(), "wb");
        if (!f) return 2;
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
        f = fopen(tpath.c_str(), "wb");
        if (!f) return 2;
        fwrite(tor.data(), 1, tor.size(), f);
        fclose(f);
        printf("prepared %u MB\n", size_mb);
        return 0;
    }
    if (strcmp(argv[1], "--run") || argc < 4) return 2;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const int port = atoi(argv[3]);
    vector<uint8_t> data;
    make(data, size_mb, piece, nullptr);

    tsnx_engine *eng = tsnx_engine_start(0);
    char hash[41] = {0};
    if (!eng || !tsnx_engine_add_torrent_file(eng, tpath.c_str(), hash, sizeof(hash)) ||
        !tsnx_engine_prepare_stream(eng, hash, 0)) {
        fprintf(stderr, "engine setup failed\n");
        return 1;
    }
    tsnx_engine_set_strict_verify(eng, hash, 1);
    uint32_t ip = htonl(0x7F000001);
    uint16_t p16 = (uint16_t)port;
    tsnx_engine_add_peers(eng, hash, &ip, &p16, 1);

    atomic<bool> done{false};
    auto t0 = steady_clock::now();
    thread wd([&]() {
        while (!done && duration<double>(steady_clock::now() - t0).count() < 90)
            this_thread::sleep_for(milliseconds(100));
        if (!done) tsnx_engine_cancel_read(eng, hash);
    });
    vector<uint8_t> buf(4 << 20);
    int64_t off = 0, total = (int64_t)data.size();
    uint64_t mism = 0;
    while (off < total) {
        int64_t want = min<int64_t>((int64_t)buf.size(), total - off);
        int64_t n = tsnx_engine_read(eng, hash, off, buf.data(), want);
        if (n <= 0) break;
        if (memcmp(buf.data(), &data[(size_t)off], (size_t)n) != 0) mism++;
        off += n;
        tsnx_engine_set_min_keep_offset(eng, hash, off);
    }
    done = true;
    wd.join();
    double secs = duration<double>(steady_clock::now() - t0).count();
    tsnx_engine_diag d;
    bool hd = tsnx_engine_get_diag(eng, hash, &d);
    tsnx_engine_stop(eng);
    printf("mse live test: %.1f MB in %.2f s, %llu mismatches, hs_fail=%d\n",
           off / 1048576.0, secs, (unsigned long long)mism, hd ? d.hs_fail : -1);
    bool ok = off == total && mism == 0;
    printf("%s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : 1;
}
