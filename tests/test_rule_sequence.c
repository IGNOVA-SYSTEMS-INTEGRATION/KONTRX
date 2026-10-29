/**
 * @file  test_rule_sequence.c
 * @brief Host test: timed action sequence state machine (APP/rule_sequence.c).
 *
 * Build (from tests/):
 *   gcc -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined -Ihost_stubs -I../HAL -I../APP \
 *       test_rule_sequence.c ../APP/rule_sequence.c -o test_rule_sequence && ./test_rule_sequence
 */
#include <stdio.h>
#include <string.h>
#include "rule_sequence.h"

#define TICK_HZ 1000U

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); failures++; } } while (0)

/* Relay 5 s ON / 7 s OFF, configurable mode and on_false. */
static ActionSequence_t make_seq(uint8_t mode, uint8_t on_false, uint8_t repeat) {
    ActionSequence_t s;
    memset(&s, 0, sizeof(s));
    s.enabled = 1;
    s.mode = mode;
    s.on_false = on_false;
    s.repeat_count = repeat;
    s.step_count = 2;
    strcpy(s.steps[0].action, "ON");
    s.steps[0].hold_ms = 5000;
    strcpy(s.steps[1].action, "OFF");
    s.steps[1].hold_ms = 7000;
    return s;
}

/* Runs the state machine every 25 ms (control engine period) from t0 to t1.
 * Returns the step at t1, or -1 if idle. */
static int run(const ActionSequence_t *seq, RuleSeqState_t *st, uint8_t cond, uint32_t *t, uint32_t until) {
    int step = -1;
    for (; *t <= until; *t += 25) {
        uint8_t idx = 0;
        step = RuleSeq_Update(seq, st, cond, *t, TICK_HZ, &idx) ? idx : -1;
    }
    return step;
}

static void test_loop_cycles(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_LOOP, SEQ_ON_FALSE_ABORT_SAFE, 0);
    RuleSeqState_t st = {0};
    uint32_t t = 0;
    CHECK(run(&seq, &st, 1, &t, 4900) == 0, "loop: ON during first 5 s");
    CHECK(run(&seq, &st, 1, &t, 11900) == 1, "loop: OFF from 5 s to 12 s");
    CHECK(run(&seq, &st, 1, &t, 12100) == 0, "loop: back to ON after 12 s");
}

static void test_abort_safe_stops_at_once(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_LOOP, SEQ_ON_FALSE_ABORT_SAFE, 0);
    RuleSeqState_t st = {0};
    uint32_t t = 0;
    run(&seq, &st, 1, &t, 3000);
    CHECK(run(&seq, &st, 0, &t, 3025) == -1, "abort_safe: idle as soon as condition is false");
    CHECK(run(&seq, &st, 1, &t, 3100) == 0, "abort_safe: restarts at step 0");
}

static void test_hold_pauses_and_resumes(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_LOOP, SEQ_ON_FALSE_HOLD, 0);
    RuleSeqState_t st = {0};
    uint32_t t = 0;
    run(&seq, &st, 1, &t, 3000);                 /* 3 s into the 5 s ON step */
    /* 5 s false: a running timer would be in the OFF step by now */
    CHECK(run(&seq, &st, 0, &t, 8000) == 0, "hold: stays on ON while condition false");
    CHECK(run(&seq, &st, 1, &t, 9900) == 0, "hold: ~2 s of ON left after resume");
    CHECK(run(&seq, &st, 1, &t, 10200) == 1, "hold: OFF after the remaining 2 s");
}

static void test_finish_cycle_loop_stops(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_LOOP, SEQ_ON_FALSE_FINISH_CYCLE, 0);
    RuleSeqState_t st = {0};
    uint32_t t = 0;
    run(&seq, &st, 1, &t, 3000);
    CHECK(run(&seq, &st, 0, &t, 11900) == 1, "finish_cycle: runs to the end of the cycle");
    CHECK(run(&seq, &st, 0, &t, 12100) == -1, "finish_cycle: stops at cycle end (was endless)");
    CHECK(run(&seq, &st, 0, &t, 40000) == -1, "finish_cycle: stays stopped");
}

static void test_finish_cycle_resumes_if_true_again(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_LOOP, SEQ_ON_FALSE_FINISH_CYCLE, 0);
    RuleSeqState_t st = {0};
    uint32_t t = 0;
    run(&seq, &st, 1, &t, 3000);
    run(&seq, &st, 0, &t, 6000);
    CHECK(run(&seq, &st, 1, &t, 12100) == 0, "finish_cycle: keeps looping if condition is back");
}

static void test_once_is_edge_triggered(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_ONCE, SEQ_ON_FALSE_ABORT_SAFE, 0);
    RuleSeqState_t st = {0};
    uint32_t t = 0;
    CHECK(run(&seq, &st, 1, &t, 11900) == 1, "once: runs ON then OFF");
    CHECK(run(&seq, &st, 1, &t, 30000) == -1, "once: no restart while condition stays true");
    run(&seq, &st, 0, &t, 30100);
    CHECK(run(&seq, &st, 1, &t, 30200) == 0, "once: runs again on the next rising edge");
}

static void test_count_runs_n_cycles(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_COUNT, SEQ_ON_FALSE_ABORT_SAFE, 2);
    RuleSeqState_t st = {0};
    uint32_t t = 0;
    CHECK(run(&seq, &st, 1, &t, 12100) == 0, "count: second cycle starts");
    CHECK(run(&seq, &st, 1, &t, 23900) == 1, "count: second cycle OFF step");
    CHECK(run(&seq, &st, 1, &t, 24100) == -1, "count: idle after 2 cycles");
    CHECK(run(&seq, &st, 1, &t, 60000) == -1, "count: no restart while condition stays true");
}

static void test_tick_wraparound(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_LOOP, SEQ_ON_FALSE_ABORT_SAFE, 0);
    RuleSeqState_t st = {0};
    uint32_t t = 0xFFFFF000U; /* ~4 s before uint32 wrap */
    uint8_t idx = 0;
    RuleSeq_Update(&seq, &st, 1, t, TICK_HZ, &idx);
    t += 4900;
    RuleSeq_Update(&seq, &st, 1, t, TICK_HZ, &idx);
    CHECK(idx == 0, "wrap: still ON before 5 s");
    t += 200;
    RuleSeq_Update(&seq, &st, 1, t, TICK_HZ, &idx);
    CHECK(idx == 1, "wrap: OFF after 5 s across the tick wrap");
}

static void test_empty_sequence(void) {
    ActionSequence_t seq = make_seq(SEQ_MODE_LOOP, SEQ_ON_FALSE_ABORT_SAFE, 0);
    seq.step_count = 0;
    RuleSeqState_t st = {0};
    uint8_t idx = 0;
    CHECK(RuleSeq_Update(&seq, &st, 1, 0, TICK_HZ, &idx) == 0, "empty: never active");
}

int main(void) {
    test_loop_cycles();
    test_abort_safe_stops_at_once();
    test_hold_pauses_and_resumes();
    test_finish_cycle_loop_stops();
    test_finish_cycle_resumes_if_true_again();
    test_once_is_edge_triggered();
    test_count_runs_n_cycles();
    test_tick_wraparound();
    test_empty_sequence();
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("rule_sequence: all checks passed\n");
    return 0;
}
