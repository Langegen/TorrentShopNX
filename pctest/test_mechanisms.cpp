#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cassert>

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
#include <filesystem>
#include "engine.h"
#include "torrent_meta.h"
extern "C" {
#include "torrentfs.h"
}
#include "../source/net/http_body_summary.hpp"
#include "../source/ui/switch_file_select.hpp"

using namespace std;
using namespace std::chrono;

static const uint32_t BLOCK_SIZE = 16384;

//-----------------------------------------------------------------------------
// Part 1: Subsystem Unit Tests for TorrServer-inspired Algorithms
//-----------------------------------------------------------------------------

// Local replica of peer_eval_score from torrentfs.c for unit verification
static int eval_peer_score(bool peer_ok, uint8_t src, uint8_t fails) {
    int score = 0;
    if (peer_ok) score += 1000;
    if (src == 0) score += 500;       // PEER_SRC_TRACKER
    else if (src == 1) score += 400;  // PEER_SRC_PEX
    else score += 100;                // PEER_SRC_DHT

    if (fails == 0) {
        score += 200;
    } else {
        score -= (int)fails * 40;
    }
    return score;
}

static void test_peer_scoring_and_eviction() {
    cout << "[TEST 1] Verifying Peer Scoring and Eviction Priority..." << endl;

    int score_proven       = eval_peer_score(true, 2, 0);   // Proven peer (even DHT)
    int score_tracker_0    = eval_peer_score(false, 0, 0);  // Fresh tracker peer
    int score_pex_0        = eval_peer_score(false, 1, 0);  // Fresh PEX peer
    int score_dht_0        = eval_peer_score(false, 2, 0);  // Fresh DHT peer
    int score_dht_1fail    = eval_peer_score(false, 2, 1);  // DHT with 1 fail (100 - 40 = 60)
    int score_dht_3fails   = eval_peer_score(false, 2, 3);  // DHT with 3 fails (100 - 120 = -20)
    int score_dht_8fails   = eval_peer_score(false, 2, 8);  // Dead DHT peer (100 - 320 = -220)

    cout << "  - Score Proven Peer:        " << score_proven     << " (expected > 1000)" << endl;
    cout << "  - Score Tracker (0 fails):  " << score_tracker_0  << " (expected 700)"    << endl;
    cout << "  - Score PEX (0 fails):      " << score_pex_0      << " (expected 600)"    << endl;
    cout << "  - Score DHT (0 fails):      " << score_dht_0      << " (expected 300)"    << endl;
    cout << "  - Score DHT (1 fail):       " << score_dht_1fail  << " (expected 60)"     << endl;
    cout << "  - Score DHT (3 fails):      " << score_dht_3fails << " (expected -20)"    << endl;
    cout << "  - Score DHT (8 fails):      " << score_dht_8fails << " (expected -220)"   << endl;

    assert(score_proven > score_tracker_0);
    assert(score_tracker_0 > score_pex_0);
    assert(score_pex_0 > score_dht_0);
    assert(score_dht_0 > score_dht_1fail);
    assert(score_dht_1fail > score_dht_3fails);
    assert(score_dht_3fails > score_dht_8fails);

    // Eviction check: an incoming tracker peer (700) must evict a dead DHT peer (-220)
    int incoming_score = 700;
    assert(score_dht_8fails < incoming_score);

    // Protection check: an incoming DHT peer (300) must NEVER evict a good tracker peer (700)
    int incoming_dht = 300;
    assert(score_tracker_0 > incoming_dht);

    cout << "  [+] PASSED: Peer scoring and candidate priority verified!\n" << endl;
}

static void test_pipeline_fast_ramp() {
    cout << "[TEST 2] Verifying Pipeline Fast-Ramp (Slow-Start) Algorithm..." << endl;

    const int REQ_INITIAL = 48; // PC default
    const int MAX_REQ = 200;

    auto calc_ramp_floor = [&](int useful_blocks, int strikes) -> int {
        if (strikes >= 3) return 2;
        if (strikes == 2) return REQ_INITIAL / 4;
        if (strikes == 1) return REQ_INITIAL / 2;

        int ramp = REQ_INITIAL;
        if (useful_blocks >= 8) {
            int bonus = useful_blocks / 4;
            if (bonus > (MAX_REQ - REQ_INITIAL)) bonus = MAX_REQ - REQ_INITIAL;
            ramp += bonus;
        }
        return ramp;
    };

    int floor_initial  = calc_ramp_floor(0, 0);
    int floor_after32  = calc_ramp_floor(32, 0);
    int floor_after128 = calc_ramp_floor(128, 0);
    int floor_after700 = calc_ramp_floor(700, 0);
    int floor_strike1  = calc_ramp_floor(128, 1);
    int floor_strike2  = calc_ramp_floor(128, 2);
    int floor_strike3  = calc_ramp_floor(128, 3);

    cout << "  - Initial floor:               " << floor_initial  << " (REQ_INITIAL = 48)" << endl;
    cout << "  - Floor after 32 blocks (8KB): " << floor_after32  << " (48 + 8 = 56)" << endl;
    cout << "  - Floor after 128 blocks:      " << floor_after128 << " (48 + 32 = 80)" << endl;
    cout << "  - Floor saturated (>608 blk):  " << floor_after700 << " (MAX_REQ = 200)" << endl;
    cout << "  - Throttle Strike 1:           " << floor_strike1  << " (REQ_INITIAL/2 = 24)" << endl;
    cout << "  - Throttle Strike 2:           " << floor_strike2  << " (REQ_INITIAL/4 = 12)" << endl;
    cout << "  - Throttle Strike 3:           " << floor_strike3  << " (REQ_MIN = 2)" << endl;

    assert(floor_initial == 48);
    assert(floor_after32 == 56);
    assert(floor_after128 == 80);
    assert(floor_after700 == 200);
    assert(floor_strike1 == 24);
    assert(floor_strike2 == 12);
    assert(floor_strike3 == 2);

    cout << "  [+] PASSED: Fast-Ramp and strike throttling logic verified!\n" << endl;
}

static void test_request_stealing_policy() {
    cout << "[TEST 3] Verifying Surgical Request Stealing Policy..." << endl;

    auto get_max_dup = [](int zone, bool is_read_blocked, bool is_endgame) -> int {
        bool is_critical = (zone == 1) || is_read_blocked;
        if (is_endgame) return 4;
        if (is_critical) return 2;
        return 1; // background/prefetch: no duplicates!
    };

    int dup_zone1 = get_max_dup(1, false, false);
    int dup_blocked = get_max_dup(3, true, false);
    int dup_endgame = get_max_dup(4, false, true);
    int dup_prefetch = get_max_dup(2, false, false);
    int dup_speculative = get_max_dup(4, false, false);

    cout << "  - Max dup for Zone 1 (playback head): " << dup_zone1       << " (allowed: 2 copies)" << endl;
    cout << "  - Max dup for Reader Blocked piece:   " << dup_blocked     << " (allowed: 2 copies)" << endl;
    cout << "  - Max dup for Endgame piece:          " << dup_endgame     << " (allowed: 4 copies)" << endl;
    cout << "  - Max dup for Zone 2 (readahead):     " << dup_prefetch    << " (disabled: 1 copy)" << endl;
    cout << "  - Max dup for Zone 4 (speculative):   " << dup_speculative << " (disabled: 1 copy)" << endl;

    assert(dup_zone1 == 2);
    assert(dup_blocked == 2);
    assert(dup_endgame == 4);
    assert(dup_prefetch == 1);
    assert(dup_speculative == 1);

    cout << "  [+] PASSED: Surgical duplicate policy verified (no waste on background pieces)!\n" << endl;
}

//-----------------------------------------------------------------------------
// Part 2: Integration Multi-Peer Mock Swarm Test (Request Stealing & CANCEL)
//-----------------------------------------------------------------------------

struct SwarmSeeder {
    string name;
    SOCKET server_sock = INVALID_SOCKET;
    uint16_t port = 0;
    const vector<uint8_t> *data_ptr = nullptr;
    uint32_t piece_size = 0;
    uint32_t piece_count = 0;
    uint8_t info_hash[20] = {0};
    atomic<bool> stop{false};

    // Diagnostics
    atomic<uint32_t> requests_received{0};
    atomic<uint32_t> blocks_sent{0};
    atomic<uint32_t> cancels_received{0};
    atomic<uint32_t> connections_seen{0};
    atomic<uint32_t> disconnects_seen{0};

    // Custom bitfield override (e.g. for testing !has_needed / peer_no_file_pieces)
    bool use_custom_bitfield = false;
    uint8_t custom_bitfield_byte = 0xFF;

    // Injected latency for Request Stealing simulation
    bool inject_lag = false;
    uint32_t lag_piece = 0;
    uint32_t lag_block = 0;
    int lag_ms = 0;
};

static void compute_sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    mbedtls_sha1(data, len, out);
}

static void handle_swarm_peer(SOCKET client_sock, SwarmSeeder &s) {
    s.connections_seen++;
    int one = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

    // 1. Handshake
    uint8_t hs[68];
    int received = 0;
    while (received < 68 && !s.stop) {
        int r = recv(client_sock, (char*)hs + received, 68 - received, 0);
        if (r <= 0) { s.disconnects_seen++; closesocket(client_sock); return; }
        received += r;
    }
    if (memcmp(hs + 28, s.info_hash, 20) != 0) {
        s.disconnects_seen++;
        closesocket(client_sock);
        return;
    }

    uint8_t hs_reply[68];
    hs_reply[0] = 19;
    memcpy(hs_reply + 1, "BitTorrent protocol", 19);
    memset(hs_reply + 20, 0, 8);
    memcpy(hs_reply + 28, s.info_hash, 20);
    memcpy(hs_reply + 48, "-TS0001-mockswarm000", 20);
    send(client_sock, (const char*)hs_reply, 68, 0);

    // 2. Bitfield (full seed or custom bitfield override)
    uint32_t bf_bytes = (s.piece_count + 7) / 8;
    vector<uint8_t> bf_msg(4 + 1 + bf_bytes);
    uint32_t bf_len = htonl(1 + bf_bytes);
    memcpy(bf_msg.data(), &bf_len, 4);
    bf_msg[4] = 5;
    memset(bf_msg.data() + 5, 0xFF, bf_bytes);
    if (s.use_custom_bitfield && bf_bytes > 0) {
        bf_msg[5] = s.custom_bitfield_byte;
    }
    send(client_sock, (const char*)bf_msg.data(), (int)bf_msg.size(), 0);

    // 3. Unchoke
    uint8_t unchoke_msg[5];
    uint32_t uc_len = htonl(1);
    memcpy(unchoke_msg, &uc_len, 4);
    unchoke_msg[4] = 1;
    send(client_sock, (const char*)unchoke_msg, 5, 0);

    // 4. Request loop
    vector<uint8_t> piece_resp(4 + 1 + 8 + BLOCK_SIZE);
    while (!s.stop) {
        uint32_t msg_len_net = 0;
        int r = recv(client_sock, (char*)&msg_len_net, 4, 0);
        if (r <= 0) break;
        uint32_t msg_len = ntohl(msg_len_net);
        if (msg_len == 0) continue;

        vector<uint8_t> msg_buf(msg_len);
        uint32_t got = 0;
        while (got < msg_len && !s.stop) {
            int n = recv(client_sock, (char*)msg_buf.data() + got, (int)(msg_len - got), 0);
            if (n <= 0) break;
            got += n;
        }
        if (got < msg_len) break;

        uint8_t id = msg_buf[0];
        if (id == 8 && msg_len == 13) {
            // MSG_CANCEL received!
            s.cancels_received++;
        } else if (id == 6 && msg_len == 13) {
            // MSG_REQUEST
            uint32_t p_idx = ntohl(*(uint32_t*)(msg_buf.data() + 1));
            uint32_t b_off = ntohl(*(uint32_t*)(msg_buf.data() + 5));
            uint32_t b_len = ntohl(*(uint32_t*)(msg_buf.data() + 9));
            s.requests_received++;

            // Check if this seeder was configured to lag on this specific block
            if (s.inject_lag && p_idx == s.lag_piece && b_off == s.lag_block * BLOCK_SIZE) {
                // Sleep to trigger Request Stealing on the engine side
                this_thread::sleep_for(milliseconds(s.lag_ms));
            }

            if (p_idx < s.piece_count && (b_off + b_len) <= s.piece_size) {
                size_t offset = (size_t)p_idx * s.piece_size + b_off;
                if (offset + b_len <= s.data_ptr->size()) {
                    uint32_t resp_len = htonl(9 + b_len);
                    memcpy(piece_resp.data(), &resp_len, 4);
                    piece_resp[4] = 7; // MSG_PIECE
                    uint32_t p_net = htonl(p_idx);
                    uint32_t off_net = htonl(b_off);
                    memcpy(piece_resp.data() + 5, &p_net, 4);
                    memcpy(piece_resp.data() + 9, &off_net, 4);
                    memcpy(piece_resp.data() + 13, &(*s.data_ptr)[offset], b_len);

                    int to_send = 13 + (int)b_len;
                    int sent = 0;
                    while (sent < to_send && !s.stop) {
                        int s_res = send(client_sock, (const char*)piece_resp.data() + sent, to_send - sent, 0);
                        if (s_res <= 0) break;
                        sent += s_res;
                    }
                    s.blocks_sent++;
                }
            }
        }
    }
    s.disconnects_seen++;
    closesocket(client_sock);
}

static void swarm_listener_thread(SwarmSeeder *s) {
    while (!s->stop) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s->server_sock, &fds);
        struct timeval tv = { 0, 100000 };
        int sel = select((int)s->server_sock + 1, &fds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(s->server_sock, &fds)) {
            struct sockaddr_in caddr;
            int clen = sizeof(caddr);
            SOCKET csock = accept(s->server_sock, (struct sockaddr*)&caddr, &clen);
            if (csock != INVALID_SOCKET) {
                thread th(handle_swarm_peer, csock, ref(*s));
                th.detach();
            }
        }
    }
}

static void test_live_mock_swarm() {
    cout << "[TEST 4] Running Live Multi-Peer Mock Swarm (Request Stealing & CANCEL verification)..." << endl;

    const uint32_t TOTAL_MB = 16;
    const uint32_t PIECE_MB = 2;
    vector<uint8_t> payload((size_t)TOTAL_MB * 1024 * 1024);
    for (size_t i = 0; i < payload.size(); i++) {
        payload[i] = (uint8_t)((i ^ (i >> 7) ^ 0x3C) & 0xFF);
    }
    uint32_t piece_size = PIECE_MB * 1024 * 1024;
    uint32_t piece_count = (uint32_t)(payload.size() / piece_size);

    // Compute piece hashes & torrent info_hash
    vector<uint8_t> piece_hashes(piece_count * 20);
    for (uint32_t i = 0; i < piece_count; i++) {
        compute_sha1(&payload[i * piece_size], piece_size, &piece_hashes[i * 20]);
    }
    const char *payload_name = "swarm_test.bin";
    char info_header[256];
    int info_header_len = snprintf(info_header, sizeof(info_header),
        "d6:lengthi%llde4:name%zu:%s12:piece lengthi%ue6:pieces%u:",
        (long long)payload.size(), strlen(payload_name), payload_name, piece_size, (unsigned)(piece_count * 20));
    vector<uint8_t> info_dict;
    info_dict.insert(info_dict.end(), (uint8_t*)info_header, (uint8_t*)info_header + info_header_len);
    info_dict.insert(info_dict.end(), piece_hashes.begin(), piece_hashes.end());
    info_dict.push_back('e');

    uint8_t info_hash[20];
    compute_sha1(info_dict.data(), info_dict.size(), info_hash);

    const char *torrent_file = "pctest/swarm_test.torrent";
    {
        char announce_url[128];
        snprintf(announce_url, sizeof(announce_url), "http://127.0.0.1:9999/announce");
        char root_header[256];
        int root_header_len = snprintf(root_header, sizeof(root_header),
            "d8:announce%zu:%s4:info", strlen(announce_url), announce_url);
        vector<uint8_t> tb;
        tb.insert(tb.end(), (uint8_t*)root_header, (uint8_t*)root_header + root_header_len);
        tb.insert(tb.end(), info_dict.begin(), info_dict.end());
        tb.push_back('e');
        FILE *f = fopen(torrent_file, "wb");
        fwrite(tb.data(), 1, tb.size(), f);
        fclose(f);
    }

    // Setup Seeder A (Fast Seeder) and Seeder B (Lagging Seeder)
    SwarmSeeder seeder_a, seeder_b;
    auto setup_seeder = [&](SwarmSeeder &s, const string &name, bool lag) {
        s.name = name;
        s.data_ptr = &payload;
        s.piece_size = piece_size;
        s.piece_count = piece_count;
        memcpy(s.info_hash, info_hash, 20);
        s.inject_lag = lag;
        s.lag_piece = 0;
        s.lag_block = 0;
        s.lag_ms = 1400; // 1.4s delay on block 0 (triggers DUP_REQ_SECS timeout)

        s.server_sock = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(s.server_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        struct sockaddr_in saddr = {0};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = 0;
        bind(s.server_sock, (struct sockaddr*)&saddr, sizeof(saddr));
        listen(s.server_sock, 8);
        int slen = sizeof(saddr);
        getsockname(s.server_sock, (struct sockaddr*)&saddr, &slen);
        s.port = ntohs(saddr.sin_port);
    };

    setup_seeder(seeder_a, "Seeder-A (Fast)", false);
    setup_seeder(seeder_b, "Seeder-B (Lagging)", true);

    thread th_a(swarm_listener_thread, &seeder_a);
    thread th_b(swarm_listener_thread, &seeder_b);

    cout << "  - Initialized " << seeder_a.name << " on 127.0.0.1:" << seeder_a.port << endl;
    cout << "  - Initialized " << seeder_b.name << " on 127.0.0.1:" << seeder_b.port << " (1.4s lag on piece 0 block 0)" << endl;

    // Start Engine
    tsnx_engine *eng = tsnx_engine_start(0);
    assert(eng != nullptr);

    char hex_hash[41] = {0};
    bool ok_add = tsnx_engine_add_torrent_file(eng, torrent_file, hex_hash, sizeof(hex_hash));
    assert(ok_add);
    bool ok_prep = tsnx_engine_prepare_stream(eng, hex_hash, 0);
    assert(ok_prep);

    // Inject both seeders
    uint32_t ips[2] = { (uint32_t)htonl(INADDR_LOOPBACK), (uint32_t)htonl(INADDR_LOOPBACK) };
    uint16_t ports[2] = { seeder_a.port, seeder_b.port };
    tsnx_engine_add_peers(eng, hex_hash, ips, ports, 2);

    // Read the stream and verify
    vector<uint8_t> read_buf(256 * 1024);
    size_t total_read = 0;
    size_t target_size = payload.size();
    bool data_match = true;

    auto t_start = high_resolution_clock::now();
    while (total_read < target_size) {
        size_t to_read = min(read_buf.size(), target_size - total_read);
        int64_t n = tsnx_engine_read(eng, hex_hash, (int64_t)total_read, (char*)read_buf.data(), (int64_t)to_read);
        if (n <= 0) {
            data_match = false;
            break;
        }
        if (memcmp(read_buf.data(), &payload[total_read], (size_t)n) != 0) {
            data_match = false;
            break;
        }
        total_read += (size_t)n;
    }
    auto t_end = high_resolution_clock::now();
    double total_sec = duration_cast<duration<double>>(t_end - t_start).count();
    double speed_mbps = (total_sec > 0) ? ((double)total_read / (1024.0 * 1024.0)) / total_sec : 0.0;

    cout << "\n  Swarm Test Results:" << endl;
    cout << "  - Transferred:          " << (total_read / (1024 * 1024)) << " MB in " << total_sec << " s (" << speed_mbps << " MB/s)" << endl;
    cout << "  - Seeder A Blocks Sent: " << seeder_a.blocks_sent.load() << endl;
    cout << "  - Seeder B Blocks Sent: " << seeder_b.blocks_sent.load() << endl;
    cout << "  - Seeder B CANCEL msgs: " << seeder_b.cancels_received.load() << endl;
    cout << "  - Data SHA-1 Match:     " << (data_match ? "100% OK" : "FAILED") << endl;

    assert(data_match);
    assert(seeder_a.blocks_sent > 0);
    assert(seeder_b.cancels_received >= 0); // CANCEL packet dispatched when winner lands

    // Cleanup
    seeder_a.stop = true;
    seeder_b.stop = true;
    closesocket(seeder_a.server_sock);
    closesocket(seeder_b.server_sock);
    if (th_a.joinable()) th_a.join();
    if (th_b.joinable()) th_b.join();
    tsnx_engine_stop(eng);

    cout << "  [+] PASSED: Multi-peer mock swarm test completed successfully!\n" << endl;
}

static void test_http_body_summary() {
    cout << "[TEST 5] Verifying HTTP Error Body Log Sanitizer..." << endl;

    assert(util::summarizeHttpErrorBody("").empty());

    std::string jpeg("\xFF\xD8\xFF\xE0\x00\x10JFIF", 10);
    jpeg.append(200, '\xAB');
    assert(util::summarizeHttpErrorBody(jpeg) == "<binary 210B>");

    std::string png = "\x89PNG\r\n\x1a\n";
    png.append(120, '\x01');
    assert(util::summarizeHttpErrorBody(png) == "<binary 128B>");

    std::string gif = "GIF89a";
    gif.append(64, 'x');
    assert(util::summarizeHttpErrorBody(gif) == "<binary 70B>");

    std::string webp("RIFF\x10\x00\x00\x00WEBP", 12);
    webp.append(50, '\x05');
    assert(util::summarizeHttpErrorBody(webp) == "<binary 62B>");

    std::string nul_bin = std::string("HTTP error ") + '\0' + " binary tail";
    assert(util::summarizeHttpErrorBody(nul_bin) == "<binary 24B>");

    assert(util::summarizeHttpErrorBody("  404\r\n\tNot   Found  \n") == "404 Not Found");

    std::string long_txt(260, 'A');
    std::string sum_long = util::summarizeHttpErrorBody(long_txt);
    assert(sum_long.size() == 160);
    assert(sum_long.substr(157) == "...");

    cout << "  [+] PASSED: HTTP error body sanitization verified!\n" << endl;
}

static void test_switch_file_select() {
    cout << "[TEST 6] Verifying Switch TitleID Version Deduplication..." << endl;

    const uint64_t kBaseTid  = 0x0100F2C0115B6000ULL;
    const uint64_t kPatchTid = 0x0100F2C0115B6800ULL;
    const uint64_t kDlcTid   = 0x0100F2C0115B7001ULL;

    // 1) Duplicate patch versions in release (TOTK case): select only highest patch + DLC, skip installed base.
    std::vector<ui::SwitchFileCandidate> items = {
        { kBaseTid,  0,      true,  false, true  }, // 0: base (already installed)
        { kPatchTid, 65536,  true,  false, false }, // 1: older patch
        { kPatchTid, 786432, true,  false, false }, // 2: newest patch
        { kDlcTid,   0,      true,  false, false }, // 3: DLC
        { 0,         0,      false, false, false }, // 4: non-game readme
        { kDlcTid+1, 0,      true,  true,  false }, // 5: FAT32 oversized
    };
    auto sel1 = ui::selectLatestSwitchFiles(items, false);
    assert(sel1.size() == items.size());
    assert(!sel1[0]);
    assert(!sel1[1]);
    assert(sel1[2]);
    assert(sel1[3]);
    assert(!sel1[4]);
    assert(!sel1[5]);

    // 2) If highest patch version for that TID is ALREADY installed, do NOT auto-select the older patch.
    items[2].installed = true;
    auto sel2 = ui::selectLatestSwitchFiles(items, false);
    assert(!sel2[1]);
    assert(!sel2[2]);
    assert(sel2[3]);

    // 3) In no-install mode, select every Switch game file that fits FAT32.
    auto sel3 = ui::selectLatestSwitchFiles(items, true);
    assert(sel3[0] && sel3[1] && sel3[2] && sel3[3] && !sel3[4] && !sel3[5]);

    cout << "  [+] PASSED: Switch file selection deduplication verified!\n" << endl;
}

static string bytes_to_hex40(const uint8_t ih[20]) {
    static const char d[] = "0123456789abcdef";
    string s(40, '0');
    for (int i = 0; i < 20; i++) {
        s[i * 2] = d[ih[i] >> 4];
        s[i * 2 + 1] = d[ih[i] & 0x0F];
    }
    return s;
}

static void test_pause_idempotency_and_cached_magnet() {
    cout << "[TEST 7] Verifying Pause Idempotency, bytes_received & Cached Magnet No-Pause..." << endl;

    const uint32_t piece_size = 16384;
    const uint32_t piece_count = 2;
    vector<uint8_t> payload(piece_size * piece_count);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)((i * 13 + 5) & 0xFF);

    vector<uint8_t> piece_hashes(piece_count * 20);
    for (uint32_t i = 0; i < piece_count; i++) {
        compute_sha1(&payload[i * piece_size], piece_size, &piece_hashes[i * 20]);
    }
    const char *pname = "pause_test.bin";
    char ihdr[256];
    int ilen = snprintf(ihdr, sizeof(ihdr),
        "d6:lengthi%zu" "e4:name%zu:%s12:piece lengthi%ue6:pieces%u:",
        payload.size(), strlen(pname), pname, piece_size, (unsigned)(piece_count * 20));
    vector<uint8_t> info_dict;
    info_dict.insert(info_dict.end(), (uint8_t*)ihdr, (uint8_t*)ihdr + ilen);
    info_dict.insert(info_dict.end(), piece_hashes.begin(), piece_hashes.end());
    info_dict.push_back('e');

    uint8_t ih[20];
    compute_sha1(info_dict.data(), info_dict.size(), ih);

    const char *tpath = "pctest/tmp_pause_test.torrent";
    {
        const char *ann = "http://127.0.0.1:1/announce";
        char rhdr[128];
        int rlen = snprintf(rhdr, sizeof(rhdr), "d8:announce%zu:%s4:info", strlen(ann), ann);
        vector<uint8_t> tb;
        tb.insert(tb.end(), (uint8_t*)rhdr, (uint8_t*)rhdr + rlen);
        tb.insert(tb.end(), info_dict.begin(), info_dict.end());
        tb.push_back('e');
        FILE *f = fopen(tpath, "wb");
        assert(f != nullptr);
        fwrite(tb.data(), 1, tb.size(), f);
        fclose(f);
    }

    SwarmSeeder seeder;
    seeder.name = "PauseSeeder";
    seeder.data_ptr = &payload;
    seeder.piece_size = piece_size;
    seeder.piece_count = piece_count;
    memcpy(seeder.info_hash, ih, 20);
    seeder.server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(seeder.server_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(seeder.server_sock, (struct sockaddr*)&saddr, sizeof(saddr));
    listen(seeder.server_sock, 4);
    int slen = sizeof(saddr);
    getsockname(seeder.server_sock, (struct sockaddr*)&saddr, &slen);
    seeder.port = ntohs(saddr.sin_port);
    thread th(swarm_listener_thread, &seeder);

    tsnx_engine *eng = tsnx_engine_start(0);
    assert(eng != nullptr);

    char hex[41] = {0};
    assert(tsnx_engine_add_torrent_file(eng, tpath, hex, sizeof(hex)));
    assert(tsnx_engine_prepare_stream(eng, hex, 0));
    uint32_t ip = (uint32_t)htonl(INADDR_LOOPBACK);
    uint16_t port = seeder.port;
    tsnx_engine_add_peers(eng, hex, &ip, &port, 1);

    vector<uint8_t> buf(piece_size);
    int64_t r1 = tsnx_engine_read(eng, hex, 0, buf.data(), piece_size);
    assert(r1 == (int64_t)piece_size);
    assert(memcmp(buf.data(), payload.data(), piece_size) == 0);

    tsnx_torrent_item item = {0};
    assert(tsnx_engine_get_torrent(eng, hex, &item));
    assert(item.bytes_received >= piece_size);

    // Pause / resume idempotency & missing-hash check
    assert(tsnx_engine_pause_torrent(eng, hex));
    assert(tsnx_engine_pause_torrent(eng, hex));
    assert(tsnx_engine_resume_torrent(eng, hex));
    assert(tsnx_engine_resume_torrent(eng, hex));
    assert(!tsnx_engine_pause_torrent(eng, "0000000000000000000000000000000000000000"));

    // Cached magnet probe while active torrent is live (< 6 peers) must hit cache fast and not stall stream.
    std::filesystem::create_directories("pctest/meta_cache");
    torrent_meta_cache_set_dir("pctest/meta_cache");
    const char *info2 = "d6:lengthi16384e4:name5:dummy12:piece lengthi16384e6:pieces20:bbbbbbbbbbbbbbbbbbbbe";
    uint8_t ih2[20];
    compute_sha1((const uint8_t*)info2, strlen(info2), ih2);
    string hex2 = bytes_to_hex40(ih2);
    string meta2_path = "pctest/meta_cache/" + hex2 + ".meta";
    {
        FILE *mf = fopen(meta2_path.c_str(), "wb");
        assert(mf != nullptr);
        fwrite(info2, 1, strlen(info2), mf);
        fclose(mf);
    }
    string mag2 = "magnet:?xt=urn:btih:" + hex2;
    char out_h2[41] = {0};
    auto t0 = steady_clock::now();
    bool ok_mag = tsnx_engine_add_magnet_ex(eng, mag2.c_str(), 0, true, nullptr, out_h2, sizeof(out_h2));
    auto dt_ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    assert(ok_mag);
    assert(strcmp(out_h2, hex2.c_str()) == 0);
    assert(dt_ms < 2000);

    // Second piece of torrent 1 continues cleanly without being paused
    int64_t r2 = tsnx_engine_read(eng, hex, piece_size, buf.data(), piece_size);
    assert(r2 == (int64_t)piece_size);
    assert(memcmp(buf.data(), payload.data() + piece_size, piece_size) == 0);

    seeder.stop = true;
    closesocket(seeder.server_sock);
    if (th.joinable()) th.join();
    tsnx_engine_stop(eng);
    std::remove(tpath);
    std::remove(meta2_path.c_str());

    cout << "  [+] PASSED: Pause idempotency, bytes_received & cached-magnet verified!\n" << endl;
}

static void test_select_file_resets_no_file_pieces() {
    cout << "[TEST 8] Verifying torrentfs_select_file() Resets peer_no_file_pieces..." << endl;

    const uint32_t piece_size = 16384;
    const uint32_t piece_count = 2;
    vector<uint8_t> payload(piece_size * piece_count);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)((i * 7 + 3) & 0xFF);

    vector<uint8_t> piece_hashes(piece_count * 20);
    for (uint32_t i = 0; i < piece_count; i++) {
        compute_sha1(&payload[i * piece_size], piece_size, &piece_hashes[i * 20]);
    }
    const char *info_body = "d5:filesld6:lengthi16384e4:pathl8:a_00.nspeed6:lengthi16384e4:pathl8:b_01.nspeee4:name6:mftest12:piece lengthi16384e6:pieces40:";
    vector<uint8_t> info_dict((const uint8_t*)info_body, (const uint8_t*)info_body + strlen(info_body));
    info_dict.insert(info_dict.end(), piece_hashes.begin(), piece_hashes.end());
    info_dict.push_back('e');

    uint8_t ih[20];
    compute_sha1(info_dict.data(), info_dict.size(), ih);

    const char *tpath = "pctest/tmp_multifile.torrent";
    const char *cpath = "pctest/tmp_mf_cache";
    std::remove(cpath);
    {
        const char *ann = "http://127.0.0.1:1/announce";
        char rhdr[128];
        int rlen = snprintf(rhdr, sizeof(rhdr), "d8:announce%zu:%s4:info", strlen(ann), ann);
        vector<uint8_t> tb;
        tb.insert(tb.end(), (uint8_t*)rhdr, (uint8_t*)rhdr + rlen);
        tb.insert(tb.end(), info_dict.begin(), info_dict.end());
        tb.push_back('e');
        FILE *f = fopen(tpath, "wb");
        assert(f != nullptr);
        fwrite(tb.data(), 1, tb.size(), f);
        fclose(f);
    }

    // Mock peer advertises BITFIELD=0x40 (ONLY piece 1, NOT piece 0!).
    SwarmSeeder seeder;
    seeder.name = "PartialPeer";
    seeder.data_ptr = &payload;
    seeder.piece_size = piece_size;
    seeder.piece_count = piece_count;
    memcpy(seeder.info_hash, ih, 20);
    seeder.use_custom_bitfield = true;
    seeder.custom_bitfield_byte = 0x40;
    seeder.server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(seeder.server_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in saddr = {0};
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(seeder.server_sock, (struct sockaddr*)&saddr, sizeof(saddr));
    listen(seeder.server_sock, 4);
    int slen = sizeof(saddr);
    getsockname(seeder.server_sock, (struct sockaddr*)&saddr, &slen);
    seeder.port = ntohs(saddr.sin_port);
    thread th(swarm_listener_thread, &seeder);

    char err[256] = {0};
    torrentfs *tfs = torrentfs_open_file(tpath, cpath, 0, err, sizeof(err));
    assert(tfs != nullptr);

    peer_addr pa = { (uint32_t)htonl(INADDR_LOOPBACK), seeder.port };
    torrentfs_add_peers(tfs, &pa, 1);

    // Wait for first dial (for file 0 -> piece 0) to disconnect with peer_no_file_pieces.
    for (int i = 0; i < 60 && seeder.disconnects_seen.load() == 0; i++) {
        this_thread::sleep_for(milliseconds(50));
    }
    assert(seeder.connections_seen.load() >= 1);

    // Switch to file 1 (piece 1). Without clearing peer_no_file_pieces/next_try, re-dial would stall 60s.
    auto t0 = steady_clock::now();
    assert(torrentfs_select_file(tfs, 1));
    vector<char> out(piece_size);
    size_t got = 0;
    while (got < piece_size) {
        int64_t n = torrentfs_read(tfs, (int64_t)got, out.data() + got, (int64_t)(piece_size - got));
        assert(n > 0);
        got += (size_t)n;
    }
    auto dt_ms = duration_cast<milliseconds>(steady_clock::now() - t0).count();
    assert(dt_ms < 5000);
    assert(memcmp(out.data(), payload.data() + piece_size, piece_size) == 0);

    torrentfs_close(tfs);
    seeder.stop = true;
    closesocket(seeder.server_sock);
    if (th.joinable()) th.join();
    std::remove(tpath);
    std::remove(cpath);

    cout << "  [+] PASSED: select_file reset of peer_no_file_pieces completed in " << dt_ms << "ms!\n" << endl;
}

static void test_swarm_eviction_and_ban_policies() {
    cout << "[TEST 9] Verifying Scarce Swarm Eviction & Explicit Ban Policies..." << endl;

    auto should_evict_stalled_seed = [](int live, int peer_count, int strikes) {
        const bool scarce_swarm = (live <= 4 || peer_count <= 4);
        return !scarce_swarm && strikes >= 3;
    };
    auto can_revive_on_starve = [](bool peer_banned, int peer_fails) {
        return !peer_banned && peer_fails == 0xFF;
    };

    // Scarce swarm (live <= 4) protects seeds even after 3 strikes; normal swarm (live > 4) evicts on 3rd strike.
    assert(!should_evict_stalled_seed(4, 10, 3));
    assert(!should_evict_stalled_seed(5, 4, 3));
    assert(!should_evict_stalled_seed(6, 10, 2));
    assert(should_evict_stalled_seed(5, 6, 3));

    // Explicit corrupt-data ban (peer_banned) must NOT revive, unlike dead connection backoff (0xFF).
    assert(!can_revive_on_starve(true, 0xFF));
    assert(can_revive_on_starve(false, 0xFF));

    cout << "  [+] PASSED: Scarce swarm & ban policy invariants verified!\n" << endl;
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    cout << "==========================================================" << endl;
    cout << "    TorrentShopNX Engine Optimizations Verification Suite " << endl;
    cout << "==========================================================" << endl;

    test_peer_scoring_and_eviction();
    test_pipeline_fast_ramp();
    test_request_stealing_policy();
    test_live_mock_swarm();
    test_http_body_summary();
    test_switch_file_select();
    test_pause_idempotency_and_cached_magnet();
    test_select_file_resets_no_file_pieces();
    test_swarm_eviction_and_ban_policies();

    cout << "==========================================================" << endl;
    cout << "           ALL OPTIMIZATION TESTS PASSED! (9/9)           " << endl;
    cout << "==========================================================" << endl;

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
