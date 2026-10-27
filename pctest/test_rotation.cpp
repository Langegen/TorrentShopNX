// Peer rotation test: 24 local seeders (4 fast, 20 slow) and a 6-session cap.
// Dialing in pool order picks mostly slow seeders first; turnover has to find
// the fast ones and keep them. Prints the rate per 2 s window and the share of
// blocks the fast seeders delivered.
//
//   pctest\test_rotation.exe [--norotate] [--size MB] [--cap N] [--turnover S] [--trial S]
//
// Seeders: 127.0.0.10 .. 127.0.0.33; fast ones are #5, #11, #17, #23
// (~16 MB/s each on Windows timers), the rest ~1 MB/s.

#define main swarm_test_main
#include "test_swarm_sched.cpp"
#undef main

#include <memory>

int main(int argc, char **argv) {
    uint32_t size_mb = 768;
    int cap = 6;
    bool rotate = true;
    int turnover_s = 3, trial_s = 4;
    bool fast_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--norotate")) rotate = false;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) size_mb = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cap") && i + 1 < argc) cap = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--turnover") && i + 1 < argc) turnover_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trial") && i + 1 < argc) trial_s = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fastonly")) fast_only = true;   // ceiling: only the fast seeders
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    Payload pl;
    pl.piece = 2u << 20;
    pl.data.resize((size_t)size_mb << 20);
    pl.pieces = (uint32_t)(pl.data.size() / pl.piece);
    uint32_t x = 0x9E3779B9u;
    for (size_t i = 0; i < pl.data.size(); i++) { x = x * 1103515245u + 12345u; pl.data[i] = (uint8_t)(x >> 24); }
    const char *tpath = "pctest/rotation_test.torrent";
    if (!write_torrent(tpath, pl)) { cerr << "cannot write torrent" << endl; return 1; }

    const int N = 24;
    unique_ptr<Seeder[]> sd(new Seeder[N]);
    vector<thread> acceptors;
    for (int i = 0; i < N; i++) {
        bool fast = (i % 6) == 5;
        sd[i].name = fast ? "fast" : "slow";
        sd[i].ip_host = 0x7F00000Au + (uint32_t)i;
        sd[i].delay_us = fast ? 0 : 1000;       // ~1 block per Windows timer tick
        sd[i].stall_every = fast ? 16 : 0;      // fast: 16 blocks per tick
        sd[i].stall_ms = fast ? 15 : 0;
        if (!start_seeder(sd[i])) { cerr << "bind failed for seeder " << i << endl; return 1; }
        acceptors.emplace_back(accept_loop, &sd[i], &pl);
    }

    if (rotate) tsnx_engine_set_rotation_timing(turnover_s, trial_s);
    else tsnx_engine_set_rotation_timing(1000000, 4);
    tsnx_engine *eng = tsnx_engine_start(0);
    char hash[41] = {0};
    if (!eng || !tsnx_engine_add_torrent_file(eng, tpath, hash, sizeof(hash)) ||
        !tsnx_engine_prepare_stream(eng, hash, 0)) {
        cerr << "engine setup failed" << endl;
        return 1;
    }
    tsnx_engine_set_strict_verify(eng, hash, 1);
    tsnx_engine_set_session_limit(eng, hash, cap);
    for (int i = 0; i < N; i++) {
        if (fast_only && (i % 6) != 5) continue;
        uint32_t ip = htonl(sd[i].ip_host);
        uint16_t port = sd[i].port;
        tsnx_engine_add_peers(eng, hash, &ip, &port, 1);
    }

    vector<uint8_t> buf(4 << 20);
    int64_t off = 0, total = (int64_t)pl.data.size();
    uint64_t mismatches = 0;
    int empty = 0;
    auto t0 = steady_clock::now();
    auto win_start = t0;
    int64_t win_bytes = 0;
    vector<double> windows;
    while (off < total) {
        int64_t want = min<int64_t>((int64_t)buf.size(), total - off);
        int64_t n = tsnx_engine_read(eng, hash, off, buf.data(), want);
        if (n <= 0) {
            if (++empty > 3) { cerr << "read failed at " << off << endl; break; }
            continue;
        }
        if (memcmp(buf.data(), &pl.data[(size_t)off], (size_t)n) != 0) mismatches++;
        off += n;
        win_bytes += n;
        tsnx_engine_set_min_keep_offset(eng, hash, off);
        auto now = steady_clock::now();
        double ws = duration<double>(now - win_start).count();
        if (ws >= 2.0) {
            windows.push_back(win_bytes / 1048576.0 / ws);
            win_start = now;
            win_bytes = 0;
        }
        if (duration<double>(now - t0).count() > 240) { cerr << "timeout" << endl; break; }
    }
    double secs = duration<double>(steady_clock::now() - t0).count();
    tsnx_engine_stop(eng);
    for (int i = 0; i < N; i++) { sd[i].stop = true; closesocket(sd[i].srv); }
    for (auto &t : acceptors) t.join();
    remove(tpath);

    uint64_t fast_blocks = 0, all_blocks = 0;
    int slow_used = 0;
    for (int i = 0; i < N; i++) {
        all_blocks += sd[i].blocks;
        if ((i % 6) == 5) fast_blocks += sd[i].blocks;
        else if (sd[i].blocks > 0) slow_used++;
    }
    printf("\nrotation test (%s, cap %d): %.1f MB in %.1f s (%.1f MB/s), %llu mismatches\n",
           rotate ? "turnover on" : "turnover off", cap, off / 1048576.0, secs,
           off / 1048576.0 / secs, (unsigned long long)mismatches);
    printf("  rate per 2 s:");
    for (double w : windows) printf(" %.0f", w);
    printf("\n  fast seeders delivered %.1f%% of the blocks; %d slow seeders were used\n",
           all_blocks ? 100.0 * (double)fast_blocks / (double)all_blocks : 0.0, slow_used);
    bool ok = mismatches == 0 && off == total;
    printf("%s\n", ok ? "PASSED" : "FAILED");
#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
