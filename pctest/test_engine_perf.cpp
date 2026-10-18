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
#endif

#include <mbedtls/sha1.h>
#include "engine.h"
#include "torrent_meta.h"

using namespace std;
using namespace std::chrono;

static const uint32_t DEFAULT_TOTAL_MB = 32;
static const uint32_t DEFAULT_PIECE_MB = 2;
static const uint32_t BLOCK_SIZE = 16384;

// Mock seeder state
struct MockSeeder {
    SOCKET server_sock = INVALID_SOCKET;
    uint16_t port = 0;
    vector<uint8_t> data;
    uint32_t piece_size = 0;
    uint32_t piece_count = 0;
    uint8_t info_hash[20] = {0};
    atomic<bool> stop{false};
    atomic<uint64_t> total_bytes_sent{0};
    atomic<uint32_t> blocks_served{0};
    atomic<int> max_concurrent_requests{0};
    atomic<int> current_concurrent_requests{0};
};

static void compute_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    mbedtls_sha1(data, len, out);
}

// Generate deterministic synthetic payload and compute piece hashes
static void init_seeder_data(MockSeeder &s, uint32_t total_mb, uint32_t piece_mb) {
    s.piece_size = piece_mb * 1024 * 1024;
    s.data.resize((size_t)total_mb * 1024 * 1024);
    s.piece_count = (uint32_t)(s.data.size() / s.piece_size);

    for (size_t i = 0; i < s.data.size(); i++) {
        s.data[i] = (uint8_t)((i ^ (i >> 8) ^ 0x5A) & 0xFF);
    }
}

// Create a valid .torrent file
static bool create_test_torrent_file(const char *filepath, MockSeeder &s) {
    vector<uint8_t> piece_hashes;
    piece_hashes.resize(s.piece_count * 20);
    for (uint32_t i = 0; i < s.piece_count; i++) {
        compute_sha1(&s.data[i * s.piece_size], s.piece_size, &piece_hashes[i * 20]);
    }

    // Build the bencoded "info" dictionary
    // keys: length, name, piece length, pieces
    char info_header[256];
    int info_header_len = snprintf(info_header, sizeof(info_header),
        "d6:lengthi%llde4:name16:perf_payload.bin12:piece lengthi%ue6:pieces%u:",
        (long long)s.data.size(), s.piece_size, (unsigned)(s.piece_count * 20));

    vector<uint8_t> info_dict;
    info_dict.insert(info_dict.end(), (uint8_t*)info_header, (uint8_t*)info_header + info_header_len);
    info_dict.insert(info_dict.end(), piece_hashes.begin(), piece_hashes.end());
    info_dict.push_back('e');

    // Calculate info_hash
    compute_sha1(info_dict.data(), info_dict.size(), s.info_hash);

    char announce_url[128];
    snprintf(announce_url, sizeof(announce_url), "http://127.0.0.1:%u/announce", s.port ? s.port : 9999);
    char root_header[256];
    int root_header_len = snprintf(root_header, sizeof(root_header),
        "d8:announce%zu:%s4:info", strlen(announce_url), announce_url);

    vector<uint8_t> torrent_bytes;
    torrent_bytes.insert(torrent_bytes.end(), (uint8_t*)root_header, (uint8_t*)root_header + root_header_len);
    torrent_bytes.insert(torrent_bytes.end(), info_dict.begin(), info_dict.end());
    torrent_bytes.push_back('e');

    FILE *f = fopen(filepath, "wb");
    if (!f) return false;
    fwrite(torrent_bytes.data(), 1, torrent_bytes.size(), f);
    fclose(f);
    return true;
}

// Client handler running in mock seeder thread
static void handle_client(SOCKET client_sock, MockSeeder &s) {
    int one = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    int sbuf = 512 * 1024;
    setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, (const char *)&sbuf, sizeof(sbuf));

    // 1. Read handshake (68 bytes)
    uint8_t hs[68];
    int received = 0;
    while (received < 68 && !s.stop) {
        int r = recv(client_sock, (char*)hs + received, 68 - received, 0);
        if (r <= 0) { closesocket(client_sock); return; }
        received += r;
    }

    if (memcmp(hs + 28, s.info_hash, 20) != 0) {
        cerr << "[mock_seeder] handshake info_hash mismatch!" << endl;
        closesocket(client_sock);
        return;
    }

    // 2. Send handshake response
    uint8_t hs_reply[68];
    hs_reply[0] = 19;
    memcpy(hs_reply + 1, "BitTorrent protocol", 19);
    memset(hs_reply + 20, 0, 8);
    memcpy(hs_reply + 28, s.info_hash, 20);
    memcpy(hs_reply + 48, "-TS0001-mockseeder00", 20);
    send(client_sock, (const char*)hs_reply, 68, 0);

    // 3. Send BITFIELD (all 1s)
    uint32_t bf_bytes = (s.piece_count + 7) / 8;
    vector<uint8_t> bf_msg(4 + 1 + bf_bytes);
    uint32_t bf_len = htonl(1 + bf_bytes);
    memcpy(bf_msg.data(), &bf_len, 4);
    bf_msg[4] = 5; // MSG_BITFIELD
    memset(bf_msg.data() + 5, 0xFF, bf_bytes);
    // Mask off spare bits in last byte
    if (s.piece_count % 8 != 0) {
        uint8_t mask = (uint8_t)(0xFF << (8 - (s.piece_count % 8)));
        bf_msg[5 + bf_bytes - 1] &= mask;
    }
    send(client_sock, (const char*)bf_msg.data(), (int)bf_msg.size(), 0);

    // 4. Send UNCHOKE
    uint8_t unchoke_msg[5];
    uint32_t uc_len = htonl(1);
    memcpy(unchoke_msg, &uc_len, 4);
    unchoke_msg[4] = 1; // MSG_UNCHOKE
    send(client_sock, (const char*)unchoke_msg, 5, 0);

    // 5. Serve block requests loop
    vector<uint8_t> piece_resp(4 + 1 + 8 + BLOCK_SIZE);
    while (!s.stop) {
        uint32_t msg_len_net = 0;
        int r = recv(client_sock, (char*)&msg_len_net, 4, 0);
        if (r <= 0) break;
        uint32_t msg_len = ntohl(msg_len_net);
        if (msg_len == 0) continue; // keepalive

        vector<uint8_t> msg_buf(msg_len);
        uint32_t got = 0;
        while (got < msg_len && !s.stop) {
            int n = recv(client_sock, (char*)msg_buf.data() + got, (int)(msg_len - got), 0);
            if (n <= 0) break;
            got += n;
        }
        if (got < msg_len) break;

        uint8_t id = msg_buf[0];
        if (id == 6 && msg_len == 13) { // MSG_REQUEST
            uint32_t p_idx = ntohl(*(uint32_t*)(msg_buf.data() + 1));
            uint32_t b_off = ntohl(*(uint32_t*)(msg_buf.data() + 5));
            uint32_t b_len = ntohl(*(uint32_t*)(msg_buf.data() + 9));

            s.current_concurrent_requests++;
            int cur = s.current_concurrent_requests.load();
            int max_val = s.max_concurrent_requests.load();
            while (cur > max_val && !s.max_concurrent_requests.compare_exchange_weak(max_val, cur)) {}

            if (p_idx < s.piece_count && (b_off + b_len) <= s.piece_size) {
                size_t offset_in_data = (size_t)p_idx * s.piece_size + b_off;
                if (offset_in_data + b_len <= s.data.size()) {
                    uint32_t resp_len = htonl(9 + b_len);
                    memcpy(piece_resp.data(), &resp_len, 4);
                    piece_resp[4] = 7; // MSG_PIECE
                    uint32_t p_idx_net = htonl(p_idx);
                    uint32_t b_off_net = htonl(b_off);
                    memcpy(piece_resp.data() + 5, &p_idx_net, 4);
                    memcpy(piece_resp.data() + 9, &b_off_net, 4);
                    memcpy(piece_resp.data() + 13, &s.data[offset_in_data], b_len);

                    int to_send = 13 + (int)b_len;
                    int sent_total = 0;
                    while (sent_total < to_send && !s.stop) {
                        int s_res = send(client_sock, (const char*)piece_resp.data() + sent_total, to_send - sent_total, 0);
                        if (s_res <= 0) break;
                        sent_total += s_res;
                    }
                    s.total_bytes_sent += b_len;
                    s.blocks_served++;
                }
            }
            s.current_concurrent_requests--;
        }
    }
    closesocket(client_sock);
}

// Seeder background thread
static void seeder_server_thread(MockSeeder *s) {
    while (!s->stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s->server_sock, &fds);
        struct timeval tv = { 0, 100000 }; // 100ms
        int sel = select((int)s->server_sock + 1, &fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(s->server_sock, &fds)) {
            struct sockaddr_in caddr;
            int clen = sizeof(caddr);
            SOCKET csock = accept(s->server_sock, (struct sockaddr*)&caddr, &clen);
            if (csock != INVALID_SOCKET) {
                thread th(handle_client, csock, ref(*s));
                th.detach();
            }
        }
    }
}

int main(int argc, char **argv) {
    uint32_t total_mb = DEFAULT_TOTAL_MB;
    uint32_t piece_mb = DEFAULT_PIECE_MB;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            total_mb = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--piece-size") == 0 && i + 1 < argc) {
            piece_mb = (uint32_t)atoi(argv[++i]);
        }
    }

    cout << "==========================================================" << endl;
    cout << "  TorrentShopNX Engine Performance & Verification Suite   " << endl;
    cout << "==========================================================" << endl;
    cout << "Test Parameters:" << endl;
    cout << "  - Dataset Size: " << total_mb << " MB (" << (total_mb * 1024 * 1024) << " bytes)" << endl;
    cout << "  - Piece Size:   " << piece_mb << " MB (" << (piece_mb * 1024 * 1024) << " bytes)" << endl;
    cout << "  - Mode:         High-Speed Local Loopback Seeder (127.0.0.1)" << endl;
    cout << "==========================================================" << endl;

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    MockSeeder seeder;
    init_seeder_data(seeder, total_mb, piece_mb);

    // Setup local listener
    seeder.server_sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    saddr.sin_port = 0; // OS assigned port
    bind(seeder.server_sock, (struct sockaddr*)&saddr, sizeof(saddr));
    listen(seeder.server_sock, 16);

    int slen = sizeof(saddr);
    getsockname(seeder.server_sock, (struct sockaddr*)&saddr, &slen);
    seeder.port = ntohs(saddr.sin_port);
    cout << "[+] Mock seeder listening on 127.0.0.1:" << seeder.port << endl;

    // Create .torrent file
    const char *torrent_path = "pctest/perf_test.torrent";
    if (!create_test_torrent_file(torrent_path, seeder)) {
        cerr << "[-] Failed to generate test torrent file!" << endl;
        return 1;
    }
    cout << "[+] Generated test torrent metadata (info_hash ready)" << endl;

    // Start seeder listener thread
    thread seeder_th(seeder_server_thread, &seeder);

    // Start Engine
    cout << "[+] Initializing TorrentShopNX Engine..." << endl;
    tsnx_engine *eng = tsnx_engine_start(0);
    if (!eng) {
        cerr << "[-] Failed to start engine!" << endl;
        seeder.stop = true;
        seeder_th.join();
        return 1;
    }

    char hex_hash[41] = {0};
    torrent_meta tm;
    char err_buf[256] = {0};
    if (torrent_load(&tm, torrent_path, err_buf, sizeof(err_buf)) != 0) {
        cerr << "[-] torrent_load error: " << err_buf << endl;
    } else {
        cout << "[+] torrent_load parsed OK: " << tm.name << ", piece_len=" << tm.piece_len << ", pieces=" << tm.piece_count << endl;
        torrent_unload(&tm);
    }

    if (!tsnx_engine_add_torrent_file(eng, torrent_path, hex_hash, sizeof(hex_hash))) {
        cerr << "[-] Failed to add torrent file to engine!" << endl;
        tsnx_engine_stop(eng);
        seeder.stop = true;
        seeder_th.join();
        return 1;
    }
    cout << "[+] Torrent loaded with hash: " << hex_hash << endl;

    // Prepare stream for file 0
    if (!tsnx_engine_prepare_stream(eng, hex_hash, 0)) {
        cerr << "[-] Failed to prepare stream!" << endl;
        tsnx_engine_stop(eng);
        seeder.stop = true;
        seeder_th.join();
        return 1;
    }

    // Inject the local mock seeder peer into the engine pool
    uint32_t ip = htonl(INADDR_LOOPBACK);
    uint16_t port = seeder.port;
    cout << "[+] Injecting mock seeder peer into engine pool..." << endl;
    tsnx_engine_add_peers(eng, hex_hash, &ip, &port, 1);

    cout << "\n[*] Benchmarking throughput and pipeline depth..." << endl;
    auto t_start = high_resolution_clock::now();

    // Read the entire file sequentially using tsnx_engine_read
    vector<uint8_t> read_buffer(256 * 1024); // 256 KB chunk reads
    size_t total_read = 0;
    size_t target_size = seeder.data.size();
    bool verification_ok = true;

    while (total_read < target_size) {
        size_t to_read = min((size_t)read_buffer.size(), target_size - total_read);
        int64_t n = tsnx_engine_read(eng, hex_hash, (int64_t)total_read, (char*)read_buffer.data(), (int64_t)to_read);
        if (n <= 0) {
            cerr << "[-] Engine read error at offset " << total_read << " (returned " << n << ")" << endl;
            verification_ok = false;
            break;
        }

        // Verify data against ground truth
        if (memcmp(read_buffer.data(), &seeder.data[total_read], (size_t)n) != 0) {
            cerr << "[-] Data corruption mismatch at offset " << total_read << "!" << endl;
            verification_ok = false;
            break;
        }

        total_read += (size_t)n;
        double progress = (double)total_read * 100.0 / (double)target_size;
        auto cur_time = high_resolution_clock::now();
        double elapsed_sec = duration_cast<duration<double>>(cur_time - t_start).count();
        double cur_mbps = (elapsed_sec > 0.0) ? ((double)total_read / (1024.0 * 1024.0)) / elapsed_sec : 0.0;

        printf("\rProgress: [%5.1f%%]  Read: %5.1f/%5.1f MB  Speed: %6.1f MB/s  Pipeline: %d reqs   ",
               progress, (double)total_read / (1024.0 * 1024.0), (double)target_size / (1024.0 * 1024.0),
               cur_mbps, seeder.max_concurrent_requests.load());
        fflush(stdout);
    }
    printf("\n");

    auto t_end = high_resolution_clock::now();
    double total_sec = duration_cast<duration<double>>(t_end - t_start).count();
    double avg_mbps = (total_sec > 0.0) ? ((double)total_read / (1024.0 * 1024.0)) / total_sec : 0.0;

    cout << "\n==========================================================" << endl;
    cout << "                    Benchmark Results                     " << endl;
    cout << "==========================================================" << endl;
    cout << "  - Total Transferred:    " << (total_read / (1024 * 1024)) << " MB (" << total_read << " bytes)" << endl;
    cout << "  - Total Blocks Served:  " << seeder.blocks_served.load() << endl;
    cout << "  - Peak Pipeline Depth:  " << seeder.max_concurrent_requests.load() << " in-flight requests" << endl;
    cout << "  - Elapsed Time:         " << total_sec << " seconds" << endl;
    cout << "  - Effective Throughput: " << avg_mbps << " MB/s (" << (avg_mbps * 8.0) << " Mbps)" << endl;
    cout << "  - Data Verification:    " << (verification_ok ? "PASSED (100% SHA-1 & Byte Match)" : "FAILED") << endl;
    cout << "==========================================================" << endl;

    // Teardown
    seeder.stop = true;
    closesocket(seeder.server_sock);
    if (seeder_th.joinable()) seeder_th.join();
    tsnx_engine_stop(eng);

#ifdef _WIN32
    WSACleanup();
#endif

    return verification_ok ? 0 : 1;
}
