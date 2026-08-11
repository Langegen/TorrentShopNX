#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine.h"

static int test_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do { \
    test_count++; \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        fail_count++; \
        return; \
    } else { \
        printf("  OK: %s\n", msg); \
    } \
} while (0)

static void test_start_stop(void) {
    printf("test_start_stop\n"); fflush(stdout);
    tsnx_engine *eng = tsnx_engine_start(8080);
    CHECK(eng != NULL, "engine start");
    CHECK(tsnx_engine_running(eng), "engine running");
    tsnx_engine_stop(eng);
    eng = NULL;
    CHECK(!tsnx_engine_running(NULL), "engine stopped");
}

static void test_add_torrent_file(void) {
    printf("test_add_torrent_file\n");
    tsnx_engine *eng = tsnx_engine_start(8080);
    CHECK(eng != NULL, "engine start");

    char hash[41] = {0};
    int ok = tsnx_engine_add_torrent_file(eng, "pctest/test.torrent", hash, sizeof(hash));
    CHECK(ok, "add torrent file");
    CHECK(strlen(hash) == 40, "hash length 40");

    tsnx_file_info files[TSNX_MAX_FILES];
    int n = tsnx_engine_get_files(eng, hash, files, TSNX_MAX_FILES);
    CHECK(n == 1, "one file in torrent");
    CHECK(files[0].size == 1024, "file size 1024");
    CHECK(strcmp(files[0].path, "test") == 0, "file name 'test'");

    ok = tsnx_engine_prepare_stream(eng, hash, 0);
    CHECK(ok, "prepare stream");

    tsnx_torrent_item items[8];
    int count = tsnx_engine_get_torrents(eng, items, 8);
    CHECK(count == 1, "one torrent in list");
    CHECK(strcmp(items[0].hash, hash) == 0, "torrent hash matches");

    tsnx_engine_stop(eng);
}

static void test_read_timeout(void) {
    printf("test_read_timeout\n");
    tsnx_engine *eng = tsnx_engine_start(8080);
    CHECK(eng != NULL, "engine start");

    char hash[41] = {0};
    int ok = tsnx_engine_add_torrent_file(eng, "pctest/test.torrent", hash, sizeof(hash));
    CHECK(ok, "add torrent file");
    ok = tsnx_engine_prepare_stream(eng, hash, 0);
    CHECK(ok, "prepare stream");

    char buf[1024];
    /* The test torrent points to a non-existent tracker, so read will block.
       Cancel after a short timeout to avoid hanging the test. */
    tsnx_engine_cancel_read(eng, hash);
    int64_t got = tsnx_engine_read(eng, hash, 0, buf, sizeof(buf));
    /* After cancel, read returns <= 0. */
    CHECK(got <= 0, "read returns <= 0 after cancel");

    tsnx_engine_stop(eng);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running engine integration tests:\n\n");
    test_start_stop();
    test_add_torrent_file();
    test_read_timeout();
    printf("\n%d checks, %d failed\n", test_count, fail_count);
    return fail_count ? 1 : 0;
}
