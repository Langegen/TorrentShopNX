#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "bencode.h"

static int tests_run = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN(name) do { tests_run++; printf("  " #name " ... "); fflush(stdout); test_##name(); printf("OK\n"); } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; return; } while (0)
#define ASSERT(cond) do { if (!(cond)) FAIL(#cond); } while (0)

TEST(parse_int) {
    be_node *n = be_parse("i42e", 4);
    if (!n) FAIL("parse returned NULL");
    ASSERT(n->type == BE_INT);
    ASSERT(n->i == 42);
    be_free(n);
}

TEST(parse_negative_int) {
    be_node *n = be_parse("i-7e", 4);
    if (!n) FAIL("parse returned NULL");
    ASSERT(n->type == BE_INT);
    ASSERT(n->i == -7);
    be_free(n);
}

TEST(parse_string) {
    be_node *n = be_parse("5:hello", 7);
    if (!n) FAIL("parse returned NULL");
    ASSERT(n->type == BE_STR);
    ASSERT(n->str.len == 5);
    ASSERT(memcmp(n->str.ptr, "hello", 5) == 0);
    be_free(n);
}

TEST(parse_empty_string) {
    be_node *n = be_parse("0:", 2);
    if (!n) FAIL("parse returned NULL");
    ASSERT(n->type == BE_STR);
    ASSERT(n->str.len == 0);
    be_free(n);
}

TEST(parse_list) {
    be_node *n = be_parse("li1ei2ei3ee", 11);
    if (!n) FAIL("parse returned NULL");
    ASSERT(n->type == BE_LIST);
    ASSERT(n->list.count == 3);
    ASSERT(n->list.items[0]->type == BE_INT && n->list.items[0]->i == 1);
    ASSERT(n->list.items[1]->type == BE_INT && n->list.items[1]->i == 2);
    ASSERT(n->list.items[2]->type == BE_INT && n->list.items[2]->i == 3);
    be_free(n);
}

TEST(parse_dict) {
    be_node *n = be_parse("d3:fooi42e3:bar5:helloe", 23);
    if (!n) FAIL("parse returned NULL");
    ASSERT(n->type == BE_DICT);
    be_node *foo = be_dict_get(n, "foo");
    be_node *bar = be_dict_get(n, "bar");
    if (!foo || foo->type != BE_INT || foo->i != 42) FAIL("foo wrong");
    if (!bar || bar->type != BE_STR || bar->str.len != 5 ||
        memcmp(bar->str.ptr, "hello", 5) != 0) FAIL("bar wrong");
    be_free(n);
}

TEST(parse_invalid) {
    be_node *n = be_parse("xyz", 3);
    if (n) { be_free(n); FAIL("invalid input accepted"); }
}

TEST(nested) {
    be_node *n = be_parse("d4:dictd3:keyi10ee4:listli1ei2eee", 33);
    if (!n) FAIL("parse returned NULL");
    ASSERT(n->type == BE_DICT);
    be_node *dict = be_dict_get(n, "dict");
    be_node *list = be_dict_get(n, "list");
    if (!dict || dict->type != BE_DICT) FAIL("dict wrong");
    if (!list || list->type != BE_LIST || list->list.count != 2) FAIL("list wrong");
    be_free(n);
}

// A peer can send "llll..." in an extension message: deep nesting must be
// rejected instead of recursing until the stack overflows.
TEST(deep_nesting_rejected) {
    enum { DEPTH = 20000 };
    char *buf = malloc(DEPTH * 2);
    if (!buf) FAIL("malloc");
    memset(buf, 'l', DEPTH);
    memset(buf + DEPTH, 'e', DEPTH);
    be_node *n = be_parse(buf, DEPTH * 2);
    free(buf);
    if (n) { be_free(n); FAIL("deep nesting accepted"); }
}

TEST(moderate_nesting_ok) {
    const char *s = "lllllllllleeeeeeeeee";  /* 10 levels */
    be_node *n = be_parse(s, strlen(s));
    if (!n) FAIL("10 levels rejected");
    be_free(n);
}

TEST(int_overflow_rejected) {
    const char *s = "i99999999999999999999999e";
    be_node *n = be_parse(s, strlen(s));
    if (n) { be_free(n); FAIL("overflowing int accepted"); }
}

TEST(big_list) {
    enum { N = 5000 };
    char *buf = malloc(N * 3 + 2);
    if (!buf) FAIL("malloc");
    size_t pos = 0;
    buf[pos++] = 'l';
    for (int i = 0; i < N; i++) { buf[pos++] = 'i'; buf[pos++] = '7'; buf[pos++] = 'e'; }
    buf[pos++] = 'e';
    be_node *n = be_parse(buf, pos);
    free(buf);
    if (!n) FAIL("big list rejected");
    ASSERT(n->type == BE_LIST && n->list.count == N && n->list.items[N - 1]->i == 7);
    be_free(n);
}

int main(void) {
    printf("Running bencode unit tests:\n");
    RUN(parse_int);
    RUN(parse_negative_int);
    RUN(parse_string);
    RUN(parse_empty_string);
    RUN(parse_list);
    RUN(parse_dict);
    RUN(parse_invalid);
    RUN(nested);
    RUN(deep_nesting_rejected);
    RUN(moderate_nesting_ok);
    RUN(int_overflow_rejected);
    RUN(big_list);
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
