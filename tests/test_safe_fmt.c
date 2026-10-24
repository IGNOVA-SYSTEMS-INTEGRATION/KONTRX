/**
 * @file  test_safe_fmt.c
 * @brief Host-side unit tests for the bounded string builder (HAL/safe_fmt.c).
 *        Compile: gcc -Wall -Wextra -I../HAL test_safe_fmt.c ../HAL/safe_fmt.c -o test_safe_fmt
 *
 * A canary region after the buffer proves no call ever writes past `cap`.
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "safe_fmt.h"

#define CAP     16U
#define CANARY  0xA5

static char mem[CAP + 32];

static void reset(void) {
    memset(mem, CANARY, sizeof(mem));
}

static void assert_canary(void) {
    for (size_t i = CAP; i < sizeof(mem); i++) {
        assert((unsigned char)mem[i] == CANARY);
    }
}

static void test_basic_append(void) {
    reset();
    size_t pos = 0;
    pos = sfmt_append(mem, CAP, pos, "%s", "abc");
    pos = sfmt_append(mem, CAP, pos, "-%d", 42);
    assert(pos == 6);
    assert(strcmp(mem, "abc-42") == 0);
    assert(!sfmt_full(CAP, pos));
    assert_canary();
    printf("  [PASS] basic append\n");
}

static void test_truncation_clamps(void) {
    reset();
    size_t pos = 0;
    pos = sfmt_append(mem, CAP, pos, "%s", "0123456789");
    pos = sfmt_append(mem, CAP, pos, "%s", "ABCDEFGHIJ");   /* overflows */
    assert(pos == CAP - 1U);
    assert(mem[CAP - 1U] == '\0');
    assert(strlen(mem) == CAP - 1U);
    assert(sfmt_full(CAP, pos));
    assert_canary();
    printf("  [PASS] truncation clamps at cap-1\n");
}

static void test_appends_after_full_are_noops(void) {
    reset();
    size_t pos = sfmt_append(mem, CAP, 0, "%s", "0123456789ABCDEFGHIJ");
    char snapshot[CAP];
    memcpy(snapshot, mem, CAP);
    for (int i = 0; i < 100; i++) {
        pos = sfmt_append(mem, CAP, pos, "%s", "more text that would overflow");
    }
    assert(pos == CAP - 1U);
    assert(memcmp(snapshot, mem, CAP) == 0);
    assert_canary();
    printf("  [PASS] appends after full are no-ops\n");
}

static void test_exact_fit(void) {
    reset();
    /* 15 chars + NUL fits CAP = 16 exactly; that reports full (cannot tell
     * an exact fit from a truncation, so callers treat it as full). */
    size_t pos = sfmt_append(mem, CAP, 0, "%s", "123456789012345");
    assert(pos == CAP - 1U);
    assert(strcmp(mem, "123456789012345") == 0);
    assert(sfmt_full(CAP, pos));
    assert_canary();
    printf("  [PASS] exact fit\n");
}

static void test_out_of_range_pos(void) {
    reset();
    size_t pos = sfmt_append(mem, CAP, 1000U, "%s", "x");
    assert(pos == CAP - 1U);
    assert(mem[CAP - 1U] == '\0');
    assert_canary();
    printf("  [PASS] pos beyond cap is clamped\n");
}

static void test_zero_cap_and_null(void) {
    reset();
    assert(sfmt_append(mem, 0U, 0U, "%s", "x") == 0U);
    assert((unsigned char)mem[0] == CANARY);
    assert(sfmt_append(NULL, CAP, 0U, "%s", "x") == 0U);
    assert(sfmt_full(0U, 0U));
    printf("  [PASS] zero cap / NULL buffer\n");
}

static void test_advance(void) {
    assert(sfmt_advance(CAP, 0U, 5) == 5U);
    assert(sfmt_advance(CAP, 5U, -1) == 5U);       /* error from sub-serializer */
    assert(sfmt_advance(CAP, 5U, 0) == 5U);
    assert(sfmt_advance(CAP, 5U, 100) == CAP - 1U); /* sub-serializer overflow */
    assert(sfmt_advance(CAP, CAP - 1U, 1) == CAP - 1U);
    assert(sfmt_advance(CAP, 999U, 1) == CAP - 1U);
    assert(sfmt_advance(0U, 0U, 1) == 0U);
    printf("  [PASS] advance clamps\n");
}

static void test_sub_serializer_pattern(void) {
    /* Mirrors `pos = sfmt_advance(cap, pos, F(buf + pos, cap - pos, ...))` */
    reset();
    size_t pos = sfmt_append(mem, CAP, 0, "%s", "0123456789");
    int n = snprintf(mem + pos, CAP - pos, "%s", "ABCDEFGHIJ");
    pos = sfmt_advance(CAP, pos, n);
    assert(pos == CAP - 1U);
    pos = sfmt_append(mem, CAP, pos, "%s", "Z");
    assert(pos == CAP - 1U);
    assert_canary();
    printf("  [PASS] sub-serializer pattern\n");
}

int main(void) {
    printf("safe_fmt tests\n");
    test_basic_append();
    test_truncation_clamps();
    test_appends_after_full_are_noops();
    test_exact_fit();
    test_out_of_range_pos();
    test_zero_cap_and_null();
    test_advance();
    test_sub_serializer_pattern();
    printf("ALL PASSED\n");
    return 0;
}
