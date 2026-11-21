#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <mbedtls/sha1.h>

#ifdef __MINGW32__
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#include "torrent_meta.h"

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

int main(void) {
    printf("Running torrent_meta unit tests:\n");
    RUN(load_torrent_file);
    RUN(piece_len_last);
    RUN(meta_cache_hit);
    RUN(tracker_mirrors_only_from_announce_url);
    RUN(tracker_mirrors_added_for_t_ru_announce);
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
