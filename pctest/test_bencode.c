#include <stdio.h>
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
    printf("\n%d tests run, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
