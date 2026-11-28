#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <switch.h>
#include <mbedtls/sha1.h>

#ifdef __MINGW32__
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#include "torrent_meta.h"
#include "magnet.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { tests_run++; printf("  " #name " ... "); fflush(stdout); test_##name(); printf("OK\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; return; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

TEST(load_torrent_file) {
    torrent_meta t;
    char err[256] = {0};
    int rc = torrent_load(&t, "pctest/test.torrent", err, sizeof(err));
    ASSERT(rc == 0);
    ASSERT(t.total_len == 1024);
    ASSERT(t.piece_len == 262144);
    ASSERT(t.piece_count == 1);
    ASSERT(t.file_count == 1);
    ASSERT(strcmp(t.name, "test") == 0);
    ASSERT(t.file_count == 1);
    ASSERT(t.files[0].length == 1024);
    ASSERT(strcmp(t.files[0].path, "test") == 0);

    static const uint8_t expected[20] = {
        0x00, 0xe2, 0xd5, 0xf6, 0x86, 0xf7, 0x78, 0x4d,
        0xb5, 0xf7, 0x9a, 0x99, 0x17, 0xcb, 0x8e, 0x6a,
        0xc0, 0x52, 0x74, 0x7f
    };
    ASSERT(memcmp(t.info_hash, expected, 20) == 0);
    torrent_unload(&t);
}

TEST(piece_len_last) {
    torrent_meta t;
    char err[256] = {0};
    int rc = torrent_load(&t, "pctest/test.torrent", err, sizeof(err));
    ASSERT(rc == 0);
    ASSERT(torrent_piece_len(&t, 0) == 1024);
    torrent_unload(&t);
}

TEST(meta_cache_hit) {
    // A magnet whose metadata was fetched earlier must load from the disk
    // cache without touching the network (no trackers in the magnet).
    torrent_meta_cache_set_dir("pctest/meta_cache");

    const char *info =
        "d4:name4:test12:piece lengthi1024e6:pieces20:"
        "aaaaaaaaaaaaaaaaaaaa6:lengthi1024ee";
    size_t info_len = strlen(info);

    uint8_t ih[20];
    mbedtls_sha1((const unsigned char *)info, info_len, ih);

    char hex[41];
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        hex[i * 2]     = digits[ih[i] >> 4];
        hex[i * 2 + 1] = digits[ih[i] & 0x0f];
    }
    hex[40] = '\0';

    mkdir("pctest/meta_cache", 0755);
    char path[320];
    snprintf(path, sizeof(path), "pctest/meta_cache/%s.meta", hex);
    FILE *f = fopen(path, "wb");
    ASSERT(f != NULL);
    ASSERT(fwrite(info, 1, info_len, f) == info_len);
    fclose(f);

    char magnet[512];
    snprintf(magnet, sizeof(magnet), "magnet:?xt=urn:btih:%s", hex);

    torrent_meta t;
    char err[256] = {0};
    int rc = torrent_load_magnet_peers(&t, magnet, NULL, 0, NULL,
                                       err, sizeof(err));
    ASSERT(rc == 0);
    ASSERT(t.file_count == 1);
    ASSERT(strcmp(t.name, "test") == 0);
    ASSERT(t.piece_len == 1024);
    ASSERT(t.piece_count == 1);
    ASSERT(t.files[0].length == 1024);
    torrent_unload(&t);
    remove(path);
}

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = strlen(data);
    int ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok ? 0 : -1;
}

static int has_tracker(const torrent_meta *t, const char *url) {
    for (int i = 0; i < t->tracker_count; i++)
        if (strcmp(t->trackers[i], url) == 0) return 1;
    return 0;
}

// The t-ru mirror check must look only at the announce URL, not at whatever
// follows it in the file (here: a comment mentioning the tracker).
TEST(tracker_mirrors_only_from_announce_url) {
    const char *path = "pctest/tmp_comment.torrent";
    ASSERT(write_file(path,
        "d8:announce22:http://a.example/annou"
        "7:comment25:mirror at bt.t-ru.org/ann"
        "4:infod6:lengthi1024e4:name4:test12:piece lengthi1024e"
        "6:pieces20:aaaaaaaaaaaaaaaaaaaaee") == 0);
    torrent_meta t;
    char err[256] = {0};
    int rc = torrent_load(&t, path, err, sizeof(err));
    remove(path);
    ASSERT(rc == 0);
    ASSERT(has_tracker(&t, "http://a.example/annou"));
    ASSERT(!has_tracker(&t, "http://bt2.t-ru.org/ann?magnet"));
    torrent_unload(&t);
}

TEST(tracker_mirrors_added_for_t_ru_announce) {
    const char *path = "pctest/tmp_tru.torrent";
    ASSERT(write_file(path,
        "d8:announce26:http://bt.t-ru.org/ann?x=1"
        "4:infod6:lengthi1024e4:name4:test12:piece lengthi1024e"
        "6:pieces20:aaaaaaaaaaaaaaaaaaaaee") == 0);
    torrent_meta t;
    char err[256] = {0};
    int rc = torrent_load(&t, path, err, sizeof(err));
    remove(path);
    ASSERT(rc == 0);
    ASSERT(has_tracker(&t, "http://bt2.t-ru.org/ann?magnet"));
    torrent_unload(&t);
}

TEST(meta_cache_has_check) {
    torrent_meta_cache_set_dir("pctest/meta_cache");
    mkdir("pctest/meta_cache", 0755);

    uint8_t ih[20];
    for (int i = 0; i < 20; i++) ih[i] = (uint8_t)(0xA0 + i);
    char hex[41];
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        hex[i * 2]     = digits[ih[i] >> 4];
        hex[i * 2 + 1] = digits[ih[i] & 0x0f];
    }
    hex[40] = '\0';

    char path[320];
    snprintf(path, sizeof(path), "pctest/meta_cache/%s.meta", hex);
    remove(path);

    ASSERT(!torrent_meta_cache_has(ih));
    ASSERT(write_file(path, "d4:name1:ae") == 0);
    ASSERT(torrent_meta_cache_has(ih));
    remove(path);
    ASSERT(!torrent_meta_cache_has(ih));
}

typedef struct {
    int listener;
    volatile int hits;
    volatile bool stop;
} loopback_http_ctx;

static void loopback_http_tracker_fn(void *arg) {
    loopback_http_ctx *ctx = (loopback_http_ctx *)arg;
    const char *body = "d8:intervali60e5:peers0:e";
    char resp[256];
    int rlen = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                        strlen(body), body);
    while (!ctx->stop) {
        struct pollfd pfd = { (SOCKET)ctx->listener, POLLIN, 0 };
        int rc = poll(&pfd, 1, 30);
        if (rc <= 0 || !(pfd.revents & POLLIN)) continue;
        struct sockaddr_in sa;
        socklen_t sl = sizeof(sa);
        int cli = accept(ctx->listener, (struct sockaddr *)&sa, &sl);
        if (cli < 0) continue;
        char req[1024];
        recv(cli, req, sizeof(req), 0);
        ctx->hits++;
        send(cli, resp, rlen, 0);
        close(cli);
    }
}

TEST(tracker_cooldown_and_blackout) {
    torrent_tracker_reset_cooldowns();

    // Local errors must never cool down a tracker.
    const char *local_url = "http://local-err.example/ann";
    for (int i = 0; i < 5; i++) torrent_tracker_record_status(local_url, false, true);
    ASSERT(!torrent_tracker_is_cooled_down(local_url));

    // Remote failures cool down after 3 hits; valid 0-peer transport_ok resets it.
    const char *flaky = "http://flaky.example/ann";
    torrent_tracker_record_status(flaky, false, false);
    torrent_tracker_record_status(flaky, false, false);
    ASSERT(!torrent_tracker_is_cooled_down(flaky));
    torrent_tracker_record_status(flaky, false, false);
    ASSERT(torrent_tracker_is_cooled_down(flaky));
    torrent_tracker_record_status(flaky, true, false);
    ASSERT(!torrent_tracker_is_cooled_down(flaky));

    // Blackout guard + live HTTP 0-peer valid reply.
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT(lsock >= 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    ASSERT(bind(lsock, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    socklen_t sl = sizeof(sa);
    ASSERT(getsockname(lsock, (struct sockaddr *)&sa, &sl) == 0);
    ASSERT(listen(lsock, 4) == 0);
    uint16_t port = ntohs(sa.sin_port);

    loopback_http_ctx hctx = { lsock, 0, false };
    Thread th;
    ASSERT(threadCreate(&th, loopback_http_tracker_fn, &hctx, NULL, 0x4000, 0x2C, -2) == 0);
    threadStart(&th);

    char url_ok[128];
    snprintf(url_ok, sizeof(url_ok), "http://127.0.0.1:%u/ann", (unsigned)port);
    const char *url_dead = "http://127.0.0.1:1/ann";

    // Pre-cool EVERY tracker so blackout guard must override the cooldown.
    for (int i = 0; i < 3; i++) {
        torrent_tracker_record_status(url_ok, false, false);
        torrent_tracker_record_status(url_dead, false, false);
    }
    ASSERT(torrent_tracker_is_cooled_down(url_ok));
    ASSERT(torrent_tracker_is_cooled_down(url_dead));

    torrent_meta stub;
    memset(&stub, 0, sizeof(stub));
    for (int i = 0; i < 20; i++) stub.info_hash[i] = (uint8_t)(i + 1);
    stub.trackers[0] = (char *)url_ok;
    stub.trackers[1] = (char *)url_dead;
    stub.tracker_count = 2;

    peer_addr peers[16];
    char err[128] = {0};
    torrent_announce(&stub, peers, 16, NULL, err, sizeof(err));

    hctx.stop = true;
    close(lsock);
    threadWaitForExit(&th);
    threadClose(&th);

    ASSERT(hctx.hits >= 1);
    ASSERT(!torrent_tracker_is_cooled_down(url_ok));
    torrent_tracker_reset_cooldowns();
}

TEST(magnet_t_ru_mirrors) {
    magnet_info m;
    char err[128] = {0};
    int rc = magnet_parse(
        "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
        "&tr=http%3A%2F%2Fbt.t-ru.org%2Fann%3Fmagnet",
        &m, err, sizeof(err));
    ASSERT(rc == 0);

    const char *expected[4] = {
        "http://bt.t-ru.org/ann?magnet",
        "http://bt2.t-ru.org/ann?magnet",
        "http://bt3.t-ru.org/ann?magnet",
        "http://bt4.t-ru.org/ann?magnet",
    };
    for (int k = 0; k < 4; k++) {
        int found = 0;
        for (int i = 0; i < m.tracker_count; i++) {
            if (strcmp(m.trackers[i], expected[k]) == 0) { found = 1; break; }
        }
        ASSERT(found);
    }
    magnet_free(&m);
}

TEST(default_trackers_and_self_peer_filter) {
    torrent_meta t;
    char err[256] = {0};
    ASSERT(torrent_load(&t, "pctest/test.torrent", err, sizeof(err)) == 0);
    ASSERT(has_tracker(&t, "udp://tracker.opentrackr.org:1337/announce"));
    torrent_unload(&t);

    int saved_port = torrent_announce_port();
    torrent_clear_external_ip();

    const char *ignored[] = {
        "0.0.0.0", "10.0.0.1", "127.0.0.1", "169.254.1.1",
        "172.16.0.1", "172.31.255.254", "192.168.1.104", "224.0.0.1"
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        torrent_set_external_ip(inet_addr(ignored[i]));
        ASSERT(torrent_external_ip() == 0);
    }

    uint32_t pub = inet_addr("95.27.161.169");
    uint32_t other = inet_addr("95.27.161.170");
    torrent_set_announce_port(6882);
    torrent_set_external_ip(pub);
    ASSERT(torrent_external_ip() == pub);

    ASSERT(torrent_is_self_peer(pub, 6882));
    ASSERT(!torrent_is_self_peer(pub, 6881));
    ASSERT(!torrent_is_self_peer(other, 6882));
    ASSERT(!torrent_is_self_peer(htonl(INADDR_LOOPBACK), 6882));

    torrent_clear_external_ip();
    torrent_set_announce_port(saved_port);
}

int main(void) {
    torrent_announce_mutex_init();
    printf("Running torrent_meta unit tests:\n");
    RUN(load_torrent_file);
    RUN(piece_len_last);
    RUN(meta_cache_hit);
    RUN(tracker_mirrors_only_from_announce_url);
    RUN(tracker_mirrors_added_for_t_ru_announce);
    RUN(meta_cache_has_check);
    RUN(tracker_cooldown_and_blackout);
    RUN(magnet_t_ru_mirrors);
    RUN(default_trackers_and_self_peer_filter);
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
