// Engine registry stress test: one torrent is removed and re-added over and
// over while a reader streams from it and two pollers hammer the status /
// zone / peer API -- the access pattern of the UI, the download progress
// thread and the installer's collector running at the same time. Before the
// registry lock this was a use-after-free (remove freed the torrentfs under
// the pollers). Passes when every byte read matches the seeder's data and the
// process exits cleanly.
//
//   pctest\test_registry.exe [--secs N]

// Reuse the mock seeder and .torrent generator from the perf test.
#define main perf_test_main
#include "test_engine_perf.cpp"
#undef main

#include <random>

int main(int argc, char **argv) {
    int secs = 12;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--secs") == 0 && i + 1 < argc) secs = atoi(argv[++i]);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    MockSeeder seeder;
    init_seeder_data(seeder, 384, 1);   // 384 MB in 1 MB pieces: RAM-window eviction runs too
    seeder.server_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    saddr.sin_port = 0;
    bind(seeder.server_sock, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(seeder.server_sock, 16);
    int slen = sizeof(saddr);
    getsockname(seeder.server_sock, (struct sockaddr *)&saddr, &slen);
    seeder.port = ntohs(saddr.sin_port);

    const char *torrent_path = "pctest/registry_test.torrent";
    if (!create_test_torrent_file(torrent_path, seeder)) {
        cerr << "[-] cannot write " << torrent_path << endl;
        return 1;
    }
    thread seeder_th(seeder_server_thread, &seeder);

    tsnx_engine *eng = tsnx_engine_start(0);
    if (!eng) { cerr << "[-] engine start failed" << endl; return 1; }

    char hash[41] = {0};
    {
        // The hash is fixed by the .torrent: learn it once.
        if (!tsnx_engine_add_torrent_file(eng, torrent_path, hash, sizeof(hash))) {
            cerr << "[-] add_torrent_file failed" << endl;
            return 1;
        }
        tsnx_engine_remove_torrent(eng, hash);
    }

    atomic<bool> stop{false};
    atomic<uint64_t> cycles{0}, verified{0}, mismatches{0}, read_fail{0}, polls{0};
    const uint32_t seed_ip = htonl(INADDR_LOOPBACK);
    const uint16_t seed_port = seeder.port;

    // Remove / re-add / reopen, at random intervals.
    thread churn([&]() {
        mt19937 rng(1234);
        uniform_int_distribution<int> pause_ms(50, 400);
        while (!stop) {
            char h[41];
            if (tsnx_engine_add_torrent_file(eng, torrent_path, h, sizeof(h))) {
                tsnx_engine_prepare_stream(eng, h, 0);
                tsnx_engine_add_peers(eng, h, &seed_ip, &seed_port, 1);
            }
            this_thread::sleep_for(milliseconds(pause_ms(rng)));
            tsnx_engine_remove_torrent(eng, h);
            cycles++;
        }
    });

    // Sequential reader that verifies every byte it gets.
    thread reader([&]() {
        vector<uint8_t> buf(256 * 1024);
        int64_t off = 0;
        const int64_t size = (int64_t)seeder.data.size();
        while (!stop) {
            int64_t want = min<int64_t>((int64_t)buf.size(), size - off);
            int64_t n = tsnx_engine_read(eng, hash, off, buf.data(), want);
            if (n <= 0) {   // removed / cancelled / not open: normal under churn
                read_fail++;
                this_thread::sleep_for(milliseconds(5));
                continue;
            }
            if (memcmp(buf.data(), &seeder.data[(size_t)off], (size_t)n) != 0) mismatches++;
            verified += (uint64_t)n;
            off += n;
            if (off >= size) off = 0;
        }
    });

    // Pollers: the status/zone/peer calls the UI and scheduler make.
    auto poller = [&]() {
        vector<tsnx_torrent_item> items(32);
        vector<tsnx_peer_info> peers(64);
        vector<tsnx_file_info> files(4);
        tsnx_engine_diag diag;
        while (!stop) {
            tsnx_engine_get_torrents(eng, items.data(), (int)items.size());
            tsnx_engine_get_peers(eng, hash, peers.data(), (int)peers.size());
            tsnx_engine_get_diag(eng, hash, &diag);
            tsnx_engine_set_piece_zone(eng, hash, 0, 4, TSNX_ZONE_CRITICAL);
            tsnx_engine_clear_piece_zones(eng, hash);
            tsnx_engine_has_torrent(eng, hash);
            tsnx_engine_get_files(eng, hash, files.data(), (int)files.size());
            tsnx_engine_set_min_keep_offset(eng, hash, 0);
            tsnx_engine_piece_size(eng, hash);
            tsnx_engine_file_offset(eng, hash);
            tsnx_engine_set_strict_verify(eng, hash, 0);
            tsnx_engine_announce_now(eng, hash);
            polls++;
        }
    };
    thread p1(poller), p2(poller);

    auto t0 = steady_clock::now();
    while (duration_cast<seconds>(steady_clock::now() - t0).count() < secs)
        this_thread::sleep_for(milliseconds(100));
    stop = true;
    churn.join();
    reader.join();
    p1.join();
    p2.join();

    tsnx_engine_stop(eng);
    seeder.stop = true;
    closesocket(seeder.server_sock);
    if (seeder_th.joinable()) seeder_th.join();
    remove(torrent_path);

    printf("\nregistry stress: %llu remove/re-add cycles, %.1f MB read and verified, "
           "%llu mismatches, %llu empty reads, %llu poll rounds\n",
           (unsigned long long)cycles.load(), verified.load() / 1048576.0,
           (unsigned long long)mismatches.load(), (unsigned long long)read_fail.load(),
           (unsigned long long)polls.load());
    bool ok = mismatches == 0 && cycles > 5 && verified > 0;
    printf("%s\n", ok ? "PASSED" : "FAILED");
#ifdef _WIN32
    WSACleanup();
#endif
    return ok ? 0 : 1;
}
