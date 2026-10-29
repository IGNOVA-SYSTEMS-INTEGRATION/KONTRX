/**
 * @file    rule_sequence.c
 * @brief   Timed action sequence state machine (see rule_sequence.h).
 */
#include "rule_sequence.h"
#include <string.h>

#define SEQ_DEFAULT_HOLD_MS 1000U

static uint32_t Step_Hold_Ticks(const SeqStep_t *step, uint32_t tick_rate_hz) {
    uint32_t ms = step->hold_ms > 0 ? step->hold_ms : SEQ_DEFAULT_HOLD_MS;
    return (uint32_t)(((uint64_t)ms * tick_rate_hz) / 1000U);
}

static void Seq_Stop(RuleSeqState_t *st, uint8_t finished_while_true) {
    memset(st, 0, sizeof(*st));
    st->done = finished_while_true;
}

uint8_t RuleSeq_Update(const ActionSequence_t *seq, RuleSeqState_t *st,
                       uint8_t cond_met, uint32_t now_ticks,
                       uint32_t tick_rate_hz, uint8_t *out_step) {
    if (!seq || !st || seq->step_count == 0) return 0;
    uint8_t step_count = seq->step_count > MAX_SEQ_STEPS ? MAX_SEQ_STEPS : seq->step_count;

    if (cond_met) {
        st->stopping = 0;
        if (st->held) {
            /* Resume the paused step where it stopped. */
            st->held = 0;
            st->step_start_ticks = now_ticks - st->held_elapsed;
            st->held_elapsed = 0;
        }
        if (!st->active && !st->done) {
            st->active = 1;
            st->current_step = 0;
            st->current_cycle = 0;
            st->step_start_ticks = now_ticks;
        }
    } else {
        st->done = 0; /* re-arm ONCE / COUNT for the next rising edge */
        if (st->active) {
            if (seq->on_false == SEQ_ON_FALSE_HOLD) {
                if (!st->held) {
                    st->held = 1;
                    st->held_elapsed = now_ticks - st->step_start_ticks;
                }
                if (out_step) *out_step = st->current_step;
                return 1; /* keep driving the current step, timer paused */
            } else if (seq->on_false == SEQ_ON_FALSE_FINISH_CYCLE) {
                st->stopping = 1;
            } else {
                Seq_Stop(st, 0); /* SEQ_ON_FALSE_ABORT_SAFE */
                return 0;
            }
        }
    }

    if (!st->active) return 0;

    if (st->current_step >= step_count) st->current_step = 0;
    const SeqStep_t *cur = &seq->steps[st->current_step];
    if ((uint32_t)(now_ticks - st->step_start_ticks) >= Step_Hold_Ticks(cur, tick_rate_hz)) {
        st->current_step++;
        st->step_start_ticks = now_ticks;
        if (st->current_step >= step_count) {
            st->current_cycle++;
            uint8_t more = 0;
            if (!st->stopping) {
                if (seq->mode == SEQ_MODE_LOOP) more = 1;
                else if (seq->mode == SEQ_MODE_COUNT && st->current_cycle < seq->repeat_count) more = 1;
            }
            if (!more) {
                Seq_Stop(st, cond_met);
                return 0;
            }
            st->current_step = 0;
        }
    }

    if (out_step) *out_step = st->current_step;
    return 1;
}
