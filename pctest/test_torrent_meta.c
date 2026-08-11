#include <stdio.h>
#include <string.h>
#include <assert.h>

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

int main(void) {
    printf("Running torrent_meta unit tests:\n");
    RUN(load_torrent_file);
    RUN(piece_len_last);
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
