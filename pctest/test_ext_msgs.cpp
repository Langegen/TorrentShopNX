// Unit tests for the peer-reachability wire formats:
//   - our extended handshake (BEP 10) and the parser for the peer's one,
//   - ut_holepunch messages (BEP 55),
//   - NAT-PMP (RFC 6886) against a mock gateway on 127.0.0.1,
// plus a fuzz pass over both parsers with random and truncated input.
//
//   pctest\test_ext_msgs.exe        exit code = number of failed checks

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>
#include <vector>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif

#include <mbedtls/bignum.h>
#include <mbedtls/sha1.h>

extern "C" {
#include "bt_peer.h"
#include "natpmp.h"
#include "mse_peer.h"
}

static int g_fail = 0, g_run = 0;
#define CHECK(cond) do { g_run++; if (!(cond)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static uint32_t ip4(int a, int b, int c, int d) {
    uint8_t x[4] = {(uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d};
    uint32_t v;
    memcpy(&v, x, 4);
    return v;   // network order
}

static void test_ext_handshake() {
    char buf[256];
    const uint32_t peer = ip4(203, 0, 113, 7);
    size_t n = peer_build_ext_handshake(buf, sizeof(buf), 6882, peer, true);
    CHECK(n > 0);
    std::string s(buf, n);
    // Keys in sorted order, both extensions advertised under our fixed ids.
    CHECK(s.rfind("d1:md12:ut_holepunchi2e6:ut_pexi1ee1:pi6882e4:reqqi", 0) == 0);
    CHECK(s.find("1:v13:TorrentShopNX6:yourip4:") != std::string::npos);
    CHECK(s.back() == 'e');

    // Our own handshake parses back.
    peer_ext_info x;
    CHECK(peer_parse_ext_handshake((const uint8_t *)buf, (uint32_t)n, &x));
    CHECK(x.pex_id == 1 && x.hp_id == 2 && x.port == 6882 && x.reqq == 250);
    CHECK(x.yourip == peer);

    // Without holepunch / port / ip.
    n = peer_build_ext_handshake(buf, sizeof(buf), 0, 0, false);
    CHECK(n > 0);
    s.assign(buf, n);
    CHECK(s.find("ut_holepunch") == std::string::npos);
    CHECK(s.find("1:pi") == std::string::npos);
    CHECK(s.find("yourip") == std::string::npos);
    CHECK(peer_parse_ext_handshake((const uint8_t *)buf, (uint32_t)n, &x) && x.hp_id == 0 && x.port == 0);

    // Too small a buffer: refused, not truncated.
    CHECK(peer_build_ext_handshake(buf, 20, 6882, peer, true) == 0);

    // A libtorrent-style handshake.
    const char lt[] = "d1:ei0e1:md11:ut_metadatai2e6:ut_pexi1e12:ut_holepunchi4ee"
                      "13:metadata_sizei1234e1:pi51413e4:reqqi500e11:upload_onlyi1e"
                      "1:v17:qBittorrent/5.0.06:yourip4:\x0a\x00\x00\x02" "e";
    CHECK(peer_parse_ext_handshake((const uint8_t *)lt, (uint32_t)(sizeof(lt) - 1), &x));
    CHECK(x.pex_id == 1 && x.hp_id == 4 && x.port == 51413 && x.reqq == 500 && x.upload_only);
    CHECK(x.yourip == ip4(10, 0, 0, 2));

    // Out-of-range values are ignored, not truncated into something valid.
    const char bad[] = "d1:md6:ut_pexi300e12:ut_holepunchi0ee1:pi70000e4:reqqi-5e6:yourip3:abce";
    CHECK(peer_parse_ext_handshake((const uint8_t *)bad, (uint32_t)(sizeof(bad) - 1), &x));
    CHECK(x.pex_id == 0 && x.hp_id == 0 && x.port == 0 && x.reqq == 0 && x.yourip == 0);

    // Not a dictionary.
    CHECK(!peer_parse_ext_handshake((const uint8_t *)"li1ee", 5, &x));
    CHECK(!peer_parse_ext_handshake((const uint8_t *)"", 0, &x));
}

static void test_holepunch_msgs() {
    uint8_t buf[16];
    peer_hp_msg m = {HP_CONNECT, ip4(198, 51, 100, 9), 6881, 0}, r;
    size_t n = peer_build_holepunch(buf, &m);
    CHECK(n == 8);
    CHECK(buf[0] == 1 && buf[1] == 0 && buf[6] == 0x1A && buf[7] == 0xE1);
    CHECK(peer_parse_holepunch(buf, (uint32_t)n, &r));
    CHECK(r.type == HP_CONNECT && r.ip == m.ip && r.port == 6881);

    m.type = HP_ERROR;
    m.err = HP_ERR_NOT_CONNECTED;
    n = peer_build_holepunch(buf, &m);
    CHECK(n == 12);
    CHECK(peer_parse_holepunch(buf, (uint32_t)n, &r) && r.type == HP_ERROR && r.err == HP_ERR_NOT_CONNECTED);
    CHECK(!peer_parse_holepunch(buf, 8, &r));          // ERROR without its code

    m.type = HP_RENDEZVOUS;
    n = peer_build_holepunch(buf, &m);
    buf[1] = 1;                                         // IPv6
    CHECK(!peer_parse_holepunch(buf, 20, &r));
    buf[1] = 0;
    buf[0] = 3;                                         // unknown type
    CHECK(!peer_parse_holepunch(buf, (uint32_t)n, &r));
    buf[0] = HP_RENDEZVOUS;
    buf[6] = buf[7] = 0;                                // port 0
    CHECK(!peer_parse_holepunch(buf, (uint32_t)n, &r));
    CHECK(!peer_parse_holepunch(buf, 7, &r));           // truncated
    CHECK(!peer_parse_holepunch(NULL, 0, &r));
}

static void test_fuzz() {
    uint32_t x = 12345;
    std::vector<uint8_t> b(512);
    const char *seed = "d1:md12:ut_holepunchi2e6:ut_pexi1ee1:pi6882e4:reqqi250e6:yourip4:abcde";
    const size_t sl = strlen(seed);
    int parsed = 0;
    for (int it = 0; it < 200000; it++) {
        size_t len;
        if (it & 1) {
            // Mutated valid handshake (bit flips, truncation).
            len = (x % (sl + 1));
            memcpy(b.data(), seed, sl);
            for (int k = 0; k < 3; k++) { x = x * 1103515245u + 12345u; b[x % sl] ^= (uint8_t)(x >> 16); }
        } else {
            len = x % b.size();
            for (size_t i = 0; i < len; i++) { x = x * 1103515245u + 12345u; b[i] = (uint8_t)(x >> 24); }
        }
        x = x * 1103515245u + 12345u;
        peer_ext_info e;
        if (peer_parse_ext_handshake(b.data(), (uint32_t)len, &e)) parsed++;
        peer_hp_msg h;
        (void)peer_parse_holepunch(b.data(), (uint32_t)len, &h);
    }
    CHECK(parsed > 0);   // and above all: no crash (run under the debugger / ASan)
}

// --- NAT-PMP against a mock gateway -----------------------------------------

struct MockGw {
    int sock = -1;
    uint16_t port = 0;
    std::atomic<bool> stop{false};
    std::atomic<int> requests{0};
    int mode = 0;            // 0 = normal, 1 = refuse maps (result 2), 2 = silent, 3 = wrong opcode
    int map_tcp_ext = 40000; // external port handed out for TCP
};

static void gw_main(MockGw *g) {
    uint8_t q[64], r[16];
    while (!g->stop) {
        sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(g->sock, (char *)q, sizeof(q), 0, (sockaddr *)&from, &fl);
        if (n <= 0) continue;
        g->requests++;
        if (g->mode == 2) continue;
        memset(r, 0, sizeof(r));
        r[1] = (uint8_t)(q[1] + 128);
        if (g->mode == 3) r[1] = 99;
        // seconds since epoch
        r[4] = 0; r[5] = 0; r[6] = 0x10; r[7] = 0;
        int rl;
        if (q[1] == 0) {
            r[8] = 203; r[9] = 0; r[10] = 113; r[11] = 50;
            rl = 12;
        } else {
            if (g->mode == 1) { r[3] = 2; rl = 16; }
            else {
                memcpy(r + 8, q + 4, 2);                 // internal port
                int ext = q[1] == 2 ? g->map_tcp_ext : ((q[6] << 8) | q[7]);
                uint32_t life = ((uint32_t)q[8] << 24) | (q[9] << 16) | (q[10] << 8) | q[11];
                if (life == 0) ext = 0;
                r[10] = (uint8_t)(ext >> 8); r[11] = (uint8_t)ext;
                uint32_t granted = life ? 3600 : 0;
                r[12] = (uint8_t)(granted >> 24); r[13] = (uint8_t)(granted >> 16);
                r[14] = (uint8_t)(granted >> 8); r[15] = (uint8_t)granted;
                rl = 16;
            }
        }
        sendto(g->sock, (const char *)r, rl, 0, (sockaddr *)&from, fl);
    }
}

static bool gw_start(MockGw &g) {
    g.sock = (int)socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (bind(g.sock, (sockaddr *)&a, sizeof(a)) != 0) return false;
    socklen_t al = sizeof(a);
    getsockname(g.sock, (sockaddr *)&a, &al);
    g.port = ntohs(a.sin_port);
#ifdef _WIN32
    DWORD tv = 100;
    setsockopt(g.sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    timeval tv = {0, 100000};
    setsockopt(g.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return true;
}

static void test_natpmp() {
    const uint32_t gw = htonl(0x7F000001);
    for (int mode = 0; mode < 4; mode++) {
        MockGw g;
        g.mode = mode;
        if (!gw_start(g)) { CHECK(!"mock gateway bind"); return; }
        std::thread th(gw_main, &g);
        int res = -1;
        uint32_t ext_ip = 0;
        int rc = natpmp_external_ip(gw, g.port, &ext_ip, NULL, &res);
        int ext = 0;
        uint32_t life = 0;
        int rc2 = natpmp_map(gw, g.port, false, 6882, 6882, 7200, &ext, &life, NULL, &res);
        if (mode == 0) {
            CHECK(rc == NATPMP_OK && ext_ip == ip4(203, 0, 113, 50));
            CHECK(rc2 == NATPMP_OK && ext == 40000 && life == 3600);
            int eu = 0;
            CHECK(natpmp_map(gw, g.port, true, 6882, 40000, 7200, &eu, &life, NULL, &res) == NATPMP_OK && eu == 40000);
            CHECK(natpmp_map(gw, g.port, false, 6882, 0, 0, NULL, NULL, NULL, NULL) == NATPMP_OK);   // delete
        } else if (mode == 1) {
            CHECK(rc == NATPMP_OK);
            CHECK(rc2 == NATPMP_ERR_REFUSED && res == 2);
        } else if (mode == 2) {
            CHECK(rc == NATPMP_ERR_NO_REPLY);
            CHECK(rc2 == NATPMP_ERR_NO_REPLY);
            CHECK(g.requests.load() == 6);   // three tries each
        } else {
            CHECK(rc == NATPMP_ERR_BAD_REPLY && rc2 == NATPMP_ERR_BAD_REPLY);
        }
        g.stop = true;
        th.join();
        closesocket(g.sock);
    }
    // stop flag honoured before the first try
    volatile bool stop = true;
    CHECK(natpmp_external_ip(htonl(0x7F000001), 1, NULL, &stop, NULL) == NATPMP_ERR_NO_REPLY);
}

// --- MSE: our initiator against an independent responder ----------------------
//
// The responder below follows the MSE/PE description directly (as libtorrent
// implements it) and shares no code with mse_peer.c except mbedtls:
//   B <- Ya | PadA
//   B -> Yb | PadB                           (PadB: 0..512 bytes, PLAIN)
//   B <- HASH(req1,S) | HASH(req2,SKEY)^HASH(req3,S) | ENC_A(VC|provide|lenC|PadC|lenIA) | ENC_A(IA)
//   B -> ENC_B(VC | select | lenD | PadD) | ENC_B(stream)

namespace ref {
struct Rc4 {
    uint8_t s[256]; uint8_t i = 0, j = 0;
    void init(const uint8_t *k, size_t n) {
        for (int x = 0; x < 256; x++) s[x] = (uint8_t)x;
        uint8_t jj = 0;
        for (int x = 0; x < 256; x++) { jj = (uint8_t)(jj + s[x] + k[x % n]); std::swap(s[x], s[jj]); }
        i = j = 0;
        uint8_t d[1024];
        memset(d, 0, sizeof(d));
        crypt(d, sizeof(d));                       // discard 1024
    }
    void crypt(uint8_t *p, size_t n) {
        for (size_t x = 0; x < n; x++) {
            i++; j = (uint8_t)(j + s[i]); std::swap(s[i], s[j]);
            p[x] ^= s[(uint8_t)(s[i] + s[j])];
        }
    }
};
static const char *P_HEX =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A63A36210000000000090563";
static void sha1_parts(std::initializer_list<std::pair<const uint8_t *, size_t>> parts, uint8_t out[20]) {
    mbedtls_sha1_context c;
    mbedtls_sha1_init(&c);
    mbedtls_sha1_starts(&c);
    for (auto &p : parts) mbedtls_sha1_update(&c, p.first, p.second);
    mbedtls_sha1_finish(&c, out);
    mbedtls_sha1_free(&c);
}
}

// One handshake: padB of `padb` bytes, our side fed `chunk` bytes at a time.
// Returns true when both sides agree and the post-handshake stream decrypts.
static int g_mse_stage = 0;
static bool mse_fail(int st) { g_mse_stage = st; return false; }

static bool mse_round(size_t padb, size_t chunk, uint32_t select, size_t padd, uint32_t seed) {
    uint8_t ih[20];
    for (int i = 0; i < 20; i++) ih[i] = (uint8_t)(i * 7 + 1);
    uint8_t ia[68];
    ia[0] = 19; memcpy(ia + 1, "BitTorrent protocol", 19); memset(ia + 20, 0, 8);
    memcpy(ia + 28, ih, 20); memset(ia + 48, 'A', 20);

    mse_peer a;
    uint8_t ya[MSE_DH_LEN];
    size_t ya_len = 0;
    mse_peer_start(&a, ih, ia, 68, ya, &ya_len);
    if (ya_len != MSE_DH_LEN) return mse_fail(1);

    // Responder: Yb, S.
    mbedtls_mpi P, G, xb, Yb, Ya, S;
    mbedtls_mpi_init(&P); mbedtls_mpi_init(&G); mbedtls_mpi_init(&xb);
    mbedtls_mpi_init(&Yb); mbedtls_mpi_init(&Ya); mbedtls_mpi_init(&S);
    mbedtls_mpi_read_string(&P, 16, ref::P_HEX);
    mbedtls_mpi_lset(&G, 2);
    uint8_t xbb[20];
    uint32_t x = seed;
    for (auto &b : xbb) { x = x * 1103515245u + 12345u; b = (uint8_t)(x >> 24); }
    mbedtls_mpi_read_binary(&xb, xbb, 20);
    mbedtls_mpi_exp_mod(&Yb, &G, &xb, &P, NULL);
    mbedtls_mpi_read_binary(&Ya, ya, MSE_DH_LEN);
    mbedtls_mpi_exp_mod(&S, &Ya, &xb, &P, NULL);
    uint8_t yb[MSE_DH_LEN], s[MSE_DH_LEN];
    mbedtls_mpi_write_binary(&Yb, yb, MSE_DH_LEN);
    mbedtls_mpi_write_binary(&S, s, MSE_DH_LEN);
    mbedtls_mpi_free(&P); mbedtls_mpi_free(&G); mbedtls_mpi_free(&xb);
    mbedtls_mpi_free(&Yb); mbedtls_mpi_free(&Ya); mbedtls_mpi_free(&S);

    uint8_t ka[20], kb[20];
    ref::sha1_parts({{(const uint8_t *)"keyA", 4}, {s, MSE_DH_LEN}, {ih, 20}}, ka);
    ref::sha1_parts({{(const uint8_t *)"keyB", 4}, {s, MSE_DH_LEN}, {ih, 20}}, kb);
    ref::Rc4 enc_b, dec_a;
    enc_b.init(kb, 20);
    dec_a.init(ka, 20);

    // B -> A: Yb | PadB, then (once A's request is checked) ENC_B(VC|select|lenD|PadD) | stream.
    std::vector<uint8_t> to_a(yb, yb + MSE_DH_LEN);
    for (size_t i = 0; i < padb; i++) { x = x * 1103515245u + 12345u; to_a.push_back((uint8_t)(x >> 24)); }

    // Drive A over what B sent so far, collecting A's output.
    std::vector<uint8_t> to_b, rx;
    size_t fed = 0;
    int r = MSE_CONT;
    auto drive = [&](void) {
        while (fed < to_a.size() && r == MSE_CONT) {
            size_t n = std::min(chunk, to_a.size() - fed);
            rx.insert(rx.end(), to_a.begin() + fed, to_a.begin() + fed + n);
            fed += n;
            for (;;) {
                if (rx.empty()) break;
                uint8_t out[256];
                size_t used = 0, prod = 0;
                r = mse_peer_feed(&a, rx.data(), rx.size(), &used, out, sizeof(out), &prod);
                to_b.insert(to_b.end(), out, out + prod);
                rx.erase(rx.begin(), rx.begin() + used);
                if (r != MSE_CONT || used == 0) break;
            }
        }
    };
    drive();
    if (r != MSE_CONT || to_b.size() != 40 + 8 + 4 + 2 + 2 + 68) return mse_fail(2);

    // B checks A's request.
    uint8_t req1[20], req2[20], req3[20];
    ref::sha1_parts({{(const uint8_t *)"req1", 4}, {s, MSE_DH_LEN}}, req1);
    ref::sha1_parts({{(const uint8_t *)"req2", 4}, {ih, 20}}, req2);
    ref::sha1_parts({{(const uint8_t *)"req3", 4}, {s, MSE_DH_LEN}}, req3);
    if (memcmp(to_b.data(), req1, 20) != 0) return mse_fail(3);
    for (int i = 0; i < 20; i++) if ((uint8_t)(to_b[20 + i] ^ req3[i]) != req2[i]) return mse_fail(4);
    std::vector<uint8_t> body(to_b.begin() + 40, to_b.end());
    dec_a.crypt(body.data(), body.size());
    static const uint8_t zero8[8] = {0};
    if (memcmp(body.data(), zero8, 8) != 0) return mse_fail(5);                 // VC
    if (body[11] != 0x03 || body[8] || body[9] || body[10]) return mse_fail(6); // provide RC4|plain
    if (body[12] != 0 || body[13] != 0) return mse_fail(7);                     // lenC = 0
    if (body[14] != 0 || body[15] != 68) return mse_fail(8);                    // lenIA
    if (memcmp(body.data() + 16, ia, 68) != 0) return mse_fail(9);              // IA

    // B -> A: ENC_B(VC | select | lenD | PadD) | stream.
    std::vector<uint8_t> hs(8 + 4 + 2 + padd, 0);
    hs[8] = (uint8_t)(select >> 24); hs[9] = (uint8_t)(select >> 16);
    hs[10] = (uint8_t)(select >> 8); hs[11] = (uint8_t)select;
    hs[12] = (uint8_t)(padd >> 8); hs[13] = (uint8_t)padd;
    enc_b.crypt(hs.data(), hs.size());
    const char *stream_plain = "\x13" "BitTorrent protocol after MSE";
    std::vector<uint8_t> st(stream_plain, stream_plain + strlen(stream_plain));
    if (select == 2) enc_b.crypt(st.data(), st.size());
    to_a.insert(to_a.end(), hs.begin(), hs.end());
    to_a.insert(to_a.end(), st.begin(), st.end());
    drive();
    if (r != MSE_DONE) return mse_fail(10);
    if (a.rc4_selected != (select == 2)) return mse_fail(11);
    // What is left over (plus anything not yet fed) is the stream.
    rx.insert(rx.end(), to_a.begin() + fed, to_a.end());
    if (rx.size() != st.size()) return mse_fail(12);
    if (a.rc4_selected) mse_peer_decrypt(&a, rx.data(), rx.size());
    return memcmp(rx.data(), stream_plain, rx.size()) == 0;
}

static void test_mse() {
    const size_t pads[] = {0, 1, 7, 8, 9, 100, 333, 511, 512};
    const size_t chunks[] = {1, 13, 96, 4096};
    int ok = 0, n = 0;
    uint32_t seed = 77;
    for (size_t p : pads)
        for (size_t c : chunks) {
            n++;
            bool good = mse_round(p, c, 2, (p * 3) % 17, seed += 101);
            if (good) ok++;
            else printf("  mse: padB=%zu chunk=%zu FAILED at step %d\n", p, c, g_mse_stage);
        }
    CHECK(ok == n);
    CHECK(mse_round(200, 50, 1, 5, 9001));   // responder selects plaintext
    printf("  mse: %d/%d handshakes ok\n", ok, n);
}

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    test_ext_handshake();
    test_holepunch_msgs();
    test_fuzz();
    test_natpmp();
    test_mse();
    printf("%d tests run, %d failed\n", g_run, g_fail);
    return g_fail;
}
