/**
 * @file  test_rules_parser.c
 * @brief Host test: the heap-free rules parser (APP/rules_parser.c) must give
 *        exactly the same result and errors as the former cJSON parser
 *        (tests/legacy_rules_parse.c), on fixed cases and on random
 *        mutations of a real rule set taken from the board.
 *
 * Build (from tests/):
 *   gcc -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined -Ihost_stubs -I../HAL -I../APP \
 *       test_rules_parser.c legacy_rules_parse.c ../APP/rules_parser.c ../APP/jspan.c ../APP/cJSON.c \
 *       -o test_rules_parser -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rules_parser.h"

const char *Legacy_RulesParse(char *body, RuleConfig_t *out, uint32_t auto_tick);

static RuleConfig_t a_out, b_out;
static int failures;

static int same_err(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

/* Runs both parsers on text and reports any difference. */
static int compare(const char *name, const char *text, size_t len, int verbose) {
    char *copy = malloc(len + 1);
    memcpy(copy, text, len);
    copy[len] = '\0';
    size_t clen = strlen(copy);   /* the firmware hands both a NUL-terminated body */

    memset(&a_out, 0, sizeof(a_out));
    memset(&b_out, 0, sizeof(b_out));
    const char *ea = Legacy_RulesParse(copy, &a_out, 1234);
    const char *eb = RulesParser_Parse(copy, clen, &b_out, 1234);
    free(copy);

    int ok = same_err(ea, eb) && (ea != NULL || memcmp(&a_out, &b_out, sizeof(a_out)) == 0);
    if (!ok) {
        failures++;
        if (verbose) {
            printf("  [FAIL] %s: legacy=%s new=%s\n", name, ea ? ea : "OK", eb ? eb : "OK");
            if (!ea && !eb) {
                const unsigned char *x = (const unsigned char *)&a_out, *y = (const unsigned char *)&b_out;
                for (size_t i = 0; i < sizeof(a_out); i++) {
                    if (x[i] != y[i]) { printf("    first diff at byte %zu: %02X vs %02X\n", i, x[i], y[i]); break; }
                }
            }
        }
    } else if (verbose) {
        printf("  [PASS] %s (%s)\n", name, ea ? ea : "parsed");
    }
    return ok;
}

#define CASE(name, json) compare(name, json, sizeof(json) - 1U, 1)

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    *len = fread(b, 1, (size_t)n, f);
    b[*len] = '\0';
    fclose(f);
    return b;
}

static void fixed_cases(void) {
    printf("fixed cases\n");
    CASE("empty object", "{}");
    CASE("empty rules", "{\"rules\":[]}");
    CASE("bare array", "[{\"rule_id\":\"r1\",\"input_id\":\"ph\",\"operator\":\">\",\"threshold\":7.5,\"output_id\":2,\"action\":\"ON\"}]");
    CASE("invalid json", "{\"rules\":[}");
    CASE("not json", "hello");
    CASE("trailing text", "{\"rules\":[]} trailing");
    CASE("bom", "\xEF\xBB\xBF{\"rules\":[]}");
    CASE("rule not object", "{\"rules\":[1]}");
    CASE("missing action", "{\"rules\":[{\"rule_id\":\"r\",\"input_id\":1,\"operator\":\"<\",\"output_id\":0}]}");
    CASE("numeric ids", "{\"version_id\":\"v9\",\"rules\":[{\"rule_id\":\"r\",\"input_id\":12,\"operator\":\"<=\",\"output_id\":3,\"action\":\"OFF\",\"priority\":14,\"auto_reverse\":true}]}");
    CASE("escapes", "{\"version_id\":\"a\\\"b\\\\c\\u00e9\\ud83d\\ude00\",\"rules\":[{\"rule_id\":\"x\\ny\",\"input_id\":\"t\\u0041\",\"operator\":\">\",\"output_id\":\"Pump\",\"action\":\"ON\"}]}");
    CASE("big numbers", "{\"rules\":[{\"rule_id\":\"r\",\"input_id\":1e12,\"operator\":\">\",\"threshold\":-1e40,\"output_id\":-5e9,\"action\":\"ON\",\"steps\":3.9,\"frequency\":-2}]}");
    CASE("17 rules", "{\"rules\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]}");
    CASE("compound tree",
         "{\"rules\":[{\"rule_id\":\"c\",\"output_id\":1,\"action\":\"ON\",\"condition\":{\"type\":\"and\",\"children\":["
         "{\"type\":\"compare\",\"input_id\":1,\"operator\":\">\",\"threshold\":2},"
         "{\"type\":\"or\",\"children\":[{\"type\":\"hysteresis\",\"input_id\":\"do\",\"high_threshold\":8,\"low_threshold\":6},"
         "{\"type\":\"not\",\"child\":{\"type\":\"sensor_state\",\"input_id\":4}}]},"
         "{\"type\":\"timer_on\",\"delay_sec\":2.5,\"child\":{\"type\":\"range\",\"input_id\":3,\"min\":1,\"max\":9,\"mode\":\"outside\"}},"
         "{\"type\":\"pulse_timer\",\"pulse_ms\":700,\"child\":{\"type\":\"compare\",\"compare_mode\":\"input\",\"input_id\":1,\"input_b_id\":2,\"operator\":\"!=\"}}]}}]}");
    CASE("sr latch",
         "{\"rules\":[{\"rule_id\":\"s\",\"output_id\":2,\"action\":\"ON\",\"condition\":{\"type\":\"sr_latch\",\"priority\":\"set\","
         "\"set_condition\":{\"type\":\"compare\",\"input_id\":5,\"operator\":\">=\",\"threshold\":3},"
         "\"reset_condition\":{\"type\":\"compare\",\"input_id\":6,\"operator\":\"<\",\"threshold\":1}}}]}");
    CASE("sequence",
         "{\"rules\":[{\"rule_id\":\"q\",\"input_id\":1,\"operator\":\">\",\"output_id\":0,\"action\":\"ON\","
         "\"actuator_type\":3,\"params\":{\"duty\":40.5,\"frequency\":2000},"
         "\"sequence\":{\"mode\":\"count\",\"repeat_count\":3,\"on_false\":\"hold\",\"steps\":["
         "{\"action\":\"SET_DUTY\",\"value\":10,\"hold_ms\":500},{\"action\":\"OFF\",\"hold_sec\":1.5},{\"action\":\"ON\"},"
         "{},{},{},{\"action\":\"extra\"}]}}]}");
    CASE("operator default", "{\"rules\":[{\"rule_id\":\"d\",\"output_id\":0,\"action\":\"ON\",\"condition\":{\"type\":\"sensor_state\",\"input_id\":3}}]}");
    CASE("duplicate keys", "{\"rules\":[{\"rule_id\":\"first\",\"rule_id\":\"second\",\"input_id\":1,\"operator\":\">\",\"output_id\":0,\"action\":\"ON\"}]}");
    CASE("long strings", "{\"version_id\":\"0123456789012345678901234567890123456789012345678901234567890123456789\",\"rules\":[]}");
    char deep[200];
    size_t n = 0;
    for (int i = 0; i < 40; i++) deep[n++] = '[';
    for (int i = 0; i < 40; i++) deep[n++] = ']';
    deep[n] = '\0';
    /* Intended difference: nesting deeper than 32 is rejected (cJSON allowed
     * 1000 levels of recursion, tens of KB of stack on a 12 KB body). */
    memset(&b_out, 0, sizeof(b_out));
    const char *e = RulesParser_Parse(deep, n, &b_out, 1);
    if (e && strcmp(e, "Invalid JSON syntax") == 0) printf("  [PASS] deep nesting (40) rejected\n");
    else { printf("  [FAIL] deep nesting (40) accepted\n"); failures++; }
}

static unsigned long rng = 12345;
static unsigned long rnd(void) { rng = rng * 1103515245UL + 12345UL; return (rng >> 16) & 0x7FFF; }

/* Mismatches are only allowed when the new parser rejects text as invalid
 * JSON that cJSON tolerated; those inputs go to strict_check.txt so a strict
 * validator (tests/check_invalid_json.py) can confirm each one is invalid.
 * Any other difference is a failure. */
static void fuzz(const char *seed, size_t seed_len, int iterations, const char *reject_log) {
    static const char tokens[][8] = { "{", "}", "[", "]", ",", ":", "\"", "\\", "1", "-", "e", ".", "null", "true", " ", "\\u00" };
    char *buf = malloc(seed_len + 256);
    FILE *log = fopen(reject_log, "w");
    int stricter = 0, bad = 0;
    for (int it = 0; it < iterations; it++) {
        memcpy(buf, seed, seed_len);
        size_t len = seed_len;
        int muts = 1 + (int)(rnd() % 4);
        for (int m = 0; m < muts && len > 2; m++) {
            size_t pos = rnd() % len;
            switch (rnd() % 3) {
                case 0: memmove(buf + pos, buf + pos + 1, len - pos - 1); len--; break;
                case 1: buf[pos] = (char)(32 + rnd() % 95); break;
                default: {
                    const char *t = tokens[rnd() % (sizeof(tokens) / sizeof(tokens[0]))];
                    size_t tl = strlen(t);
                    memmove(buf + pos + tl, buf + pos, len - pos);
                    memcpy(buf + pos, t, tl);
                    len += tl;
                }
            }
        }
        buf[len] = '\0';
        len = strlen(buf);
        char *c1 = malloc(len + 1), *c2 = malloc(len + 1);
        memcpy(c1, buf, len + 1); memcpy(c2, buf, len + 1);
        memset(&a_out, 0, sizeof(a_out)); memset(&b_out, 0, sizeof(b_out));
        const char *ea = Legacy_RulesParse(c1, &a_out, 1234);
        const char *eb = RulesParser_Parse(c2, len, &b_out, 1234);
        free(c1); free(c2);
        int same = same_err(ea, eb) && (ea != NULL || memcmp(&a_out, &b_out, sizeof(a_out)) == 0);
        if (same) continue;
        if (eb && strcmp(eb, "Invalid JSON syntax") == 0 && (!ea || strcmp(ea, "Invalid JSON syntax") != 0)) {
            stricter++;
            if (log) { fwrite(buf, 1, len, log); fputc('\n', log); }
            continue;
        }
        bad++;
        if (bad <= 5) {
            size_t d = 0;
            while (d < len && d < seed_len && buf[d] == seed[d]) d++;
            size_t from = d > 40 ? d - 40 : 0;
            printf("  [FAIL] fuzz: legacy=%s new=%s near: %.*s\n", ea ? ea : "OK", eb ? eb : "OK",
                   (int)((len - from) > 100 ? 100 : (len - from)), buf + from);
        }
    }
    if (log) fclose(log);
    free(buf);
    failures += bad;
    printf("fuzz: %d iterations, %d stricter rejections (logged for check), %d real mismatches\n",
           iterations, stricter, bad);
}

int main(int argc, char **argv) {
    fixed_cases();
    size_t len = 0;
    char *board = read_file(argc > 1 ? argv[1] : "data/rules_board_v1.0.3.json", &len);
    if (!board) { printf("cannot read board rules\n"); return 2; }
    printf("board rule set (%zu bytes)\n", len);
    compare("board rules v1.0.3", board, len, 1);
    fuzz(board, len, argc > 2 ? atoi(argv[2]) : 20000, "strict_check.txt");
    free(board);
    printf(failures ? "FAILURES: %d\n" : "ALL PASSED\n", failures);
    return failures ? 1 : 0;
}
