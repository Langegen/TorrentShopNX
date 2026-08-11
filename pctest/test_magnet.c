#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "magnet.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { tests_run++; printf("  " #name " ... "); fflush(stdout); test_##name(); printf("OK\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; return; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

TEST(parse_btih_hex) {
    const char *uri = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678&dn=Test";
    magnet_info m = {0};
    char err[256] = {0};
    int rc = magnet_parse(uri, &m, err, sizeof(err));
    ASSERT(rc == 0);
    ASSERT(strcmp(m.name, "Test") == 0);
    ASSERT(m.tracker_count == 0);
    static const uint8_t expected[20] = {
        0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd, 0xef,
        0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd, 0xef,
        0x12, 0x34, 0x56, 0x78
    };
    ASSERT(memcmp(m.info_hash, expected, 20) == 0);
    magnet_free(&m);
}

TEST(parse_with_trackers) {
    const char *uri = "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678&tr=udp://tracker.example.com:1337";
    magnet_info m = {0};
    char err[256] = {0};
    int rc = magnet_parse(uri, &m, err, sizeof(err));
    ASSERT(rc == 0);
    ASSERT(m.tracker_count == 1);
    ASSERT(strstr(m.trackers[0], "tracker.example.com") != NULL);
    magnet_free(&m);
}

TEST(parse_invalid) {
    const char *uri = "magnet:?xt=urn:btih:zzzz";
    magnet_info m = {0};
    char err[256] = {0};
    int rc = magnet_parse(uri, &m, err, sizeof(err));
    ASSERT(rc != 0);
}

int main(void) {
    printf("Running magnet unit tests:\n");
    RUN(parse_btih_hex);
    RUN(parse_with_trackers);
    RUN(parse_invalid);
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
